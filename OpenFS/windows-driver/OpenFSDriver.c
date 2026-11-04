#include <ntifs.h>
#include "OpenFSSuperblock.h"
#include "OpenFSDeviceIo.h"
#include "OpenFSLayout.h"
#include "OpenFSVolume.h"

static PDEVICE_OBJECT g_OpenFsFileSystemDevice = NULL;

static NTSTATUS
OpenFsCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status
    )
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/*
 * Until a mounted volume device object and a file-control path exist, every
 * operation other than the explicitly supported filesystem-control requests
 * must fail deterministically. In particular, never let an unset major
 * function pointer become an accidental dispatch path.
 */
/*
 * OpenFS is currently mounted read-only. A flush therefore has no dirty
 * filesystem cache to write, but it must still validate that the target is a
 * live mounted volume rather than blindly succeeding on the control device.
 */
static NTSTATUS
OpenFsDispatchFlushBuffers(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    OPENFS_VOLUME_STATE state;
    NTSTATUS status;

    if (DeviceObject == NULL ||
        DeviceObject == g_OpenFsFileSystemDevice)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    status = OpenFsQueryVolumeState(DeviceObject, &state);
    if (!NT_SUCCESS(status))
        return OpenFsCompleteIrp(Irp, status);
    if (state != OpenFsVolumeMounted)
        return OpenFsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED);

    return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
OpenFsDispatchUnsupported(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
}

/*
 * The filesystem control device is not a mounted volume. Accepting CREATE
 * here would falsely advertise file access that the driver cannot service.
 */
static NTSTATUS
OpenFsDispatchCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB fcb = NULL;
    PCHAR utf8Path = NULL;
    ULONG utf8Capacity;
    ULONG utf8Length = 0;
    ACCESS_MASK desiredAccess;
    ULONG createDisposition;
    ULONG createOptions;
    NTSTATUS status;

    if (DeviceObject == g_OpenFsFileSystemDevice ||
        DeviceObject == NULL || fileObject == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE ||
        fileObject->FsContext != NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

    desiredAccess = stack->Parameters.Create.SecurityContext != NULL
        ? stack->Parameters.Create.SecurityContext->DesiredAccess
        : MAXIMUM_ALLOWED;
    createDisposition = (stack->Parameters.Create.Options >> 24) & 0xffu;
    createOptions = stack->Parameters.Create.Options & 0x00ffffffu;

    /* The first native driver release is deliberately read-only. */
    if ((desiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA |
            FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE |
            WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL |
            MAXIMUM_ALLOWED)) != 0)
        return OpenFsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED);
    if (createDisposition != FILE_OPEN && createDisposition != FILE_OPEN_IF)
        return OpenFsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED);
    if ((createOptions & (FILE_DELETE_ON_CLOSE | FILE_OPEN_BY_FILE_ID)) != 0)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

    if (fileObject->FileName.Length == 0) {
        /* An empty name denotes the volume root. */
        status = OpenFsLookupPathFcb(DeviceObject, NULL, 0, &fcb);
    } else {
        if (fileObject->FileName.Buffer == NULL ||
            (fileObject->FileName.Length & 1u) != 0 ||
            fileObject->FileName.Length > (MAXULONG - 1u) / 3u)
            return OpenFsCompleteIrp(Irp, STATUS_OBJECT_NAME_INVALID);

        utf8Capacity = (ULONG)fileObject->FileName.Length * 3u + 1u;
        utf8Path = (PCHAR)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, utf8Capacity, 'pFsO');
        if (utf8Path == NULL)
            return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);

        status = RtlUnicodeToUTF8N(utf8Path, utf8Capacity - 1u,
            &utf8Length, fileObject->FileName.Buffer,
            fileObject->FileName.Length);
        if (NT_SUCCESS(status))
            status = OpenFsLookupPathFcb(DeviceObject,
                (const UCHAR*)utf8Path, (USHORT)utf8Length, &fcb);
    }

    if (utf8Path != NULL)
        ExFreePoolWithTag(utf8Path, 'pFsO');
    if (!NT_SUCCESS(status))
        return OpenFsCompleteIrp(Irp, status);

    if ((createOptions & FILE_DIRECTORY_FILE) != 0 && fcb->Kind != 2u)
        return OpenFsCompleteIrp(Irp, STATUS_NOT_A_DIRECTORY);
    if ((createOptions & FILE_NON_DIRECTORY_FILE) != 0 && fcb->Kind == 2u)
        return OpenFsCompleteIrp(Irp, STATUS_FILE_IS_A_DIRECTORY);

    if (!ExAcquireRundownProtection(&vcb->RundownProtection))
        return OpenFsCompleteIrp(Irp, STATUS_DELETE_PENDING);
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeMounted) {
        status = STATUS_VOLUME_DISMOUNTED;
    } else if (vcb->LockFileObject != NULL) {
        status = STATUS_ACCESS_DENIED;
    } else if (fcb->OpenHandleCount == MAXULONG) {
        status = STATUS_TOO_MANY_OPENED_FILES;
    } else {
        /*
         * Enforce the caller's read/write/delete sharing contract before
         * publishing FsContext. The FCB resource serializes this with other
         * creates and cleanup's IoRemoveShareAccess.
         */
        status = IoCheckShareAccess(desiredAccess,
            stack->Parameters.Create.ShareAccess, fileObject,
            &fcb->ShareAccess, TRUE);
        if (NT_SUCCESS(status)) {
            ++fcb->OpenHandleCount;
            fileObject->FsContext = fcb;
        }
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    if (NT_SUCCESS(status)) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = FILE_OPENED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    return OpenFsCompleteIrp(Irp, status);
}

static NTSTATUS
OpenFsDispatchRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB fcb;
    ULONG bytesRead = 0;
    NTSTATUS status;
    LARGE_INTEGER offset;

    if (DeviceObject == g_OpenFsFileSystemDevice ||
        DeviceObject == NULL || fileObject == NULL ||
        fileObject->FsContext == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    fcb = (POPENFS_FCB)fileObject->FsContext;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE ||
        fcb->Signature != OPENFS_FCB_SIGNATURE)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

    offset = stack->Parameters.Read.ByteOffset;
    if (offset.QuadPart == FILE_USE_FILE_POINTER_POSITION) {
        if ((fileObject->Flags & FO_SYNCHRONOUS_IO) == 0)
            return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
        offset = fileObject->CurrentByteOffset;
    } else if (offset.QuadPart < 0) {
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
    }
    if (stack->Parameters.Read.Length != 0 &&
        Irp->AssociatedIrp.SystemBuffer == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_USER_BUFFER);

    status = OpenFsReadFcbData(DeviceObject, fcb,
        (ULONGLONG)offset.QuadPart,
        (UCHAR*)Irp->AssociatedIrp.SystemBuffer,
        stack->Parameters.Read.Length, &bytesRead);
    if (status == STATUS_END_OF_FILE)
        status = STATUS_SUCCESS;

    if (NT_SUCCESS(status) && (fileObject->Flags & FO_SYNCHRONOUS_IO) != 0) {
        if (bytesRead > (ULONGLONG)(MAXLONGLONG - offset.QuadPart)) {
            status = STATUS_INTEGER_OVERFLOW;
            bytesRead = 0;
        } else {
            fileObject->CurrentByteOffset.QuadPart =
                offset.QuadPart + bytesRead;
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = NT_SUCCESS(status) ? bytesRead : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
OpenFsDispatchClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB fcb;

    if (DeviceObject != NULL && DeviceObject != g_OpenFsFileSystemDevice &&
        fileObject != NULL && fileObject->FsContext != NULL) {
        vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
        fcb = (POPENFS_FCB)fileObject->FsContext;
        if (vcb != NULL && vcb->Signature == OPENFS_VCB_SIGNATURE &&
            ExAcquireRundownProtection(&vcb->RundownProtection)) {
            if (ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
                if (vcb->LockFileObject == fileObject && vcb->Vpb != NULL) {
                    KIRQL oldIrql;
                    IoAcquireVpbSpinLock(&oldIrql);
                    if (vcb->Vpb->DeviceObject == DeviceObject)
                        vcb->Vpb->Flags &= (USHORT)~VPB_LOCKED;
                    vcb->LockFileObject = NULL;
                    IoReleaseVpbSpinLock(oldIrql);
                }
                if (fcb->Signature == OPENFS_FCB_SIGNATURE &&
                    fcb->OpenHandleCount != 0)
                    --fcb->OpenHandleCount;
                fileObject->FsContext = NULL;
                ExReleaseResourceLite(&vcb->Resource);
            }
            ExReleaseRundownProtection(&vcb->RundownProtection);
        }
    }
    return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
OpenFsDispatchQueryInformation(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB fcb;
    PVOID output = Irp->AssociatedIrp.SystemBuffer;
    ULONG required = 0;
    NTSTATUS status = STATUS_SUCCESS;
    FILE_INFORMATION_CLASS informationClass =
        stack->Parameters.QueryFile.FileInformationClass;

    if (DeviceObject == NULL || DeviceObject == g_OpenFsFileSystemDevice ||
        fileObject == NULL || fileObject->FsContext == NULL || output == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    fcb = (POPENFS_FCB)fileObject->FsContext;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

    if (!ExAcquireRundownProtection(&vcb->RundownProtection))
        return OpenFsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED);
    if (!ExAcquireResourceSharedLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeMounted ||
        fcb->Signature != OPENFS_FCB_SIGNATURE) {
        status = STATUS_VOLUME_DISMOUNTED;
        goto QueryExit;
    }
    {
        PLIST_ENTRY link;
        BOOLEAN found = FALSE;
        for (link = vcb->FcbList.Flink; link != &vcb->FcbList;
             link = link->Flink) {
            if (CONTAINING_RECORD(link, OPENFS_FCB, CacheLink) == fcb) {
                found = TRUE;
                break;
            }
        }
        if (!found || fcb->FileSize > MAXLONGLONG ||
            vcb->Geometry.BlockSize == 0) {
            status = STATUS_INVALID_DEVICE_STATE;
            goto QueryExit;
        }
    }

    switch (informationClass) {
    case FileBasicInformation:
    {
        PFILE_BASIC_INFORMATION info = (PFILE_BASIC_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        RtlZeroMemory(info, required);
        info->FileAttributes = fcb->Kind == 2u
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        break;
    }
    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION info =
            (PFILE_STANDARD_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        RtlZeroMemory(info, required);
        {
            ULONGLONG blockSize = vcb->Geometry.BlockSize;
            ULONGLONG allocation = fcb->FileSize;
            if (allocation != 0) {
                ULONGLONG remainder = allocation % blockSize;
                if (remainder != 0) {
                    ULONGLONG padding = blockSize - remainder;
                    if (allocation > MAXLONGLONG - padding) {
                        status = STATUS_INTEGER_OVERFLOW;
                        goto QueryExit;
                    }
                    allocation += padding;
                }
            }
            info->AllocationSize.QuadPart = (LONGLONG)allocation;
        }
        info->EndOfFile.QuadPart = (LONGLONG)fcb->FileSize;
        info->NumberOfLinks = 1;
        info->Directory = (fcb->Kind == 2u);
        break;
    }
    case FileInternalInformation:
    {
        PFILE_INTERNAL_INFORMATION info =
            (PFILE_INTERNAL_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        info->IndexNumber.QuadPart = (LONGLONG)fcb->ObjectIdLow;
        break;
    }
    case FilePositionInformation:
    {
        PFILE_POSITION_INFORMATION info =
            (PFILE_POSITION_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        info->CurrentByteOffset = fileObject->CurrentByteOffset;
        break;
    }
    case FileAlignmentInformation:
    {
        PFILE_ALIGNMENT_INFORMATION info =
            (PFILE_ALIGNMENT_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        info->AlignmentRequirement = DeviceObject->AlignmentRequirement;
        break;
    }
    case FileNetworkOpenInformation:
    {
        PFILE_NETWORK_OPEN_INFORMATION info =
            (PFILE_NETWORK_OPEN_INFORMATION)output;
        ULONGLONG allocation = fcb->FileSize;
        ULONGLONG blockSize = vcb->Geometry.BlockSize;

        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        if (blockSize == 0) {
            status = STATUS_INVALID_DEVICE_STATE;
            goto QueryExit;
        }
        if (allocation != 0) {
            ULONGLONG remainder = allocation % blockSize;
            if (remainder != 0) {
                ULONGLONG padding = blockSize - remainder;
                if (allocation > MAXLONGLONG - padding) {
                    status = STATUS_INTEGER_OVERFLOW;
                    goto QueryExit;
                }
                allocation += padding;
            }
        }
        RtlZeroMemory(info, required);
        info->AllocationSize.QuadPart = (LONGLONG)allocation;
        info->EndOfFile.QuadPart = (LONGLONG)fcb->FileSize;
        info->FileAttributes = fcb->Kind == 2u
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        break;
    }
    case FileAttributeTagInformation:
    {
        PFILE_ATTRIBUTE_TAG_INFORMATION info =
            (PFILE_ATTRIBUTE_TAG_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        info->FileAttributes = fcb->Kind == 2u
            ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        info->ReparseTag = 0;
        break;
    }
    case FileEaInformation:
    {
        PFILE_EA_INFORMATION info = (PFILE_EA_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        /* OpenFS currently has no extended-attribute namespace. */
        info->EaSize = 0;
        break;
    }
    case FileIdInformation:
    {
        PFILE_ID_INFORMATION info = (PFILE_ID_INFORMATION)output;
        required = sizeof(*info);
        if (stack->Parameters.QueryFile.Length < required) break;
        RtlZeroMemory(info, required);
        info->VolumeSerialNumber = OpenFsCrc32C(vcb->Geometry.Uuid,
            sizeof(vcb->Geometry.Uuid));
        RtlCopyMemory(info->FileId.Identifier, &fcb->ObjectIdLow,
            sizeof(fcb->ObjectIdLow));
        RtlCopyMemory(info->FileId.Identifier + sizeof(fcb->ObjectIdLow),
            &fcb->ObjectIdHigh, sizeof(fcb->ObjectIdHigh));
        break;
    }
    default:
        status = STATUS_INVALID_INFO_CLASS;
        goto QueryExit;
    }

    if (stack->Parameters.QueryFile.Length < required) {
        status = STATUS_BUFFER_TOO_SMALL;
    } else {
        Irp->IoStatus.Information = required;
    }

QueryExit:
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    Irp->IoStatus.Status = status;
    if (!NT_SUCCESS(status)) Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
OpenFsDispatchQueryVolumeInformation(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    POPENFS_VCB vcb;
    PVOID output = Irp->AssociatedIrp.SystemBuffer;
    ULONG length = stack->Parameters.QueryVolume.Length;
    ULONG required = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (DeviceObject == NULL || DeviceObject == g_OpenFsFileSystemDevice ||
        output == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);
    if (!ExAcquireRundownProtection(&vcb->RundownProtection))
        return OpenFsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED);
    {
        BOOLEAN needsExclusive =
            stack->Parameters.QueryVolume.FsInformationClass == FileFsSizeInformation ||
            stack->Parameters.QueryVolume.FsInformationClass == FileFsFullSizeInformation;
        BOOLEAN acquired = needsExclusive
            ? ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)
            : ExAcquireResourceSharedLite(&vcb->Resource, TRUE);
        if (!acquired) {
            ExReleaseRundownProtection(&vcb->RundownProtection);
            return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
        }
    }
    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeMounted || vcb->Geometry.BlockSize == 0) {
        status = STATUS_VOLUME_DISMOUNTED;
        goto VolumeQueryExit;
    }

    switch (stack->Parameters.QueryVolume.FsInformationClass) {
    case FileFsVolumeInformation:
    {
        PFILE_FS_VOLUME_INFORMATION info =
            (PFILE_FS_VOLUME_INFORMATION)output;
        static const WCHAR label[] = L"OpenFS";
        required = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) +
            sizeof(label) - sizeof(WCHAR);
        if (length < required) break;
        RtlZeroMemory(output, required);
        info->VolumeSerialNumber = OpenFsCrc32C(vcb->Geometry.Uuid,
            sizeof(vcb->Geometry.Uuid));
        info->SupportsObjects = FALSE;
        info->VolumeLabelLength = sizeof(label) - sizeof(WCHAR);
        RtlCopyMemory(info->VolumeLabel, label, info->VolumeLabelLength);
        break;
    }
    case FileFsAttributeInformation:
    {
        PFILE_FS_ATTRIBUTE_INFORMATION info =
            (PFILE_FS_ATTRIBUTE_INFORMATION)output;
        static const WCHAR fsName[] = L"OpenFS";
        required = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) +
            sizeof(fsName) - sizeof(WCHAR);
        if (length < required) break;
        RtlZeroMemory(output, required);
        info->FileSystemAttributes = FILE_CASE_SENSITIVE_SEARCH |
            FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
        info->MaximumComponentNameLength = OPENFS_DIRECTORY_NAME_MAX_BYTES;
        info->FileSystemNameLength = sizeof(fsName) - sizeof(WCHAR);
        RtlCopyMemory(info->FileSystemName, fsName,
            info->FileSystemNameLength);
        break;
    }
    case FileFsSizeInformation:
    {
        PFILE_FS_SIZE_INFORMATION info =
            (PFILE_FS_SIZE_INFORMATION)output;
        OPENFS_VOLUME_LAYOUT layout;
        ULONGLONG freeBlocks = 0;
        ULONGLONG totalBlocks;
        ULONG sectorSize = vcb->RealDeviceObject != NULL
            ? vcb->RealDeviceObject->SectorSize : 0;

        required = sizeof(*info);
        if (length < required) break;
        if (sectorSize == 0 ||
            vcb->Geometry.BlockSize % sectorSize != 0) {
            status = STATUS_INVALID_DEVICE_STATE;
            goto VolumeQueryExit;
        }
        status = OpenFsCalculateVolumeLayout(vcb->Geometry.BlockSize,
            vcb->Geometry.TotalBlocks, vcb->Geometry.Version, &layout);
        if (!NT_SUCCESS(status)) goto VolumeQueryExit;
        if (layout.BackupSuperblockBlock <= layout.DataStartBlock) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto VolumeQueryExit;
        }
        totalBlocks = layout.BackupSuperblockBlock - layout.DataStartBlock;
        if (!vcb->FreeDataBlockCountValid) {
            status = OpenFsCountFreeDataBlocks(vcb->RealDeviceObject,
                &vcb->Geometry, &vcb->FreeDataBlockCount);
            if (!NT_SUCCESS(status)) goto VolumeQueryExit;
            vcb->FreeDataBlockCountValid = TRUE;
        }
        freeBlocks = vcb->FreeDataBlockCount;
        if (totalBlocks > MAXLONGLONG || freeBlocks > totalBlocks) {
            status = STATUS_INTEGER_OVERFLOW;
            goto VolumeQueryExit;
        }
        RtlZeroMemory(info, required);
        info->TotalAllocationUnits.QuadPart = (LONGLONG)totalBlocks;
        info->AvailableAllocationUnits.QuadPart = (LONGLONG)freeBlocks;
        info->SectorsPerAllocationUnit =
            vcb->Geometry.BlockSize / sectorSize;
        info->BytesPerSector = sectorSize;
        break;
    }
    case FileFsFullSizeInformation:
    {
        PFILE_FS_FULL_SIZE_INFORMATION info =
            (PFILE_FS_FULL_SIZE_INFORMATION)output;
        OPENFS_VOLUME_LAYOUT layout;
        ULONGLONG freeBlocks = 0;
        ULONGLONG totalBlocks;
        ULONG sectorSize = vcb->RealDeviceObject != NULL
            ? vcb->RealDeviceObject->SectorSize : 0;

        required = sizeof(*info);
        if (length < required) break;
        if (sectorSize == 0 ||
            vcb->Geometry.BlockSize % sectorSize != 0) {
            status = STATUS_INVALID_DEVICE_STATE;
            goto VolumeQueryExit;
        }
        status = OpenFsCalculateVolumeLayout(vcb->Geometry.BlockSize,
            vcb->Geometry.TotalBlocks, vcb->Geometry.Version, &layout);
        if (!NT_SUCCESS(status)) goto VolumeQueryExit;
        if (layout.BackupSuperblockBlock <= layout.DataStartBlock) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto VolumeQueryExit;
        }
        totalBlocks = layout.BackupSuperblockBlock - layout.DataStartBlock;
        if (!vcb->FreeDataBlockCountValid) {
            status = OpenFsCountFreeDataBlocks(vcb->RealDeviceObject,
                &vcb->Geometry, &vcb->FreeDataBlockCount);
            if (!NT_SUCCESS(status)) goto VolumeQueryExit;
            vcb->FreeDataBlockCountValid = TRUE;
        }
        freeBlocks = vcb->FreeDataBlockCount;
        if (totalBlocks > MAXLONGLONG || freeBlocks > totalBlocks) {
            status = STATUS_INTEGER_OVERFLOW;
            goto VolumeQueryExit;
        }
        RtlZeroMemory(info, required);
        info->TotalAllocationUnits.QuadPart = (LONGLONG)totalBlocks;
        /* OpenFS has no per-user quotas, so caller and actual free space match. */
        info->CallerAvailableAllocationUnits.QuadPart = (LONGLONG)freeBlocks;
        info->ActualAvailableAllocationUnits.QuadPart = (LONGLONG)freeBlocks;
        info->SectorsPerAllocationUnit =
            vcb->Geometry.BlockSize / sectorSize;
        info->BytesPerSector = sectorSize;
        break;
    }
    case FileFsDeviceInformation:
    {
        PFILE_FS_DEVICE_INFORMATION info =
            (PFILE_FS_DEVICE_INFORMATION)output;
        required = sizeof(*info);
        if (length < required) break;
        info->DeviceType = FILE_DEVICE_DISK;
        info->Characteristics = DeviceObject->Characteristics;
        break;
    }
    default:
        status = STATUS_INVALID_INFO_CLASS;
        goto VolumeQueryExit;
    }

    if (length < required) status = STATUS_BUFFER_TOO_SMALL;
    else Irp->IoStatus.Information = required;

VolumeQueryExit:
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    Irp->IoStatus.Status = status;
    if (!NT_SUCCESS(status)) Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

typedef struct _OPENFS_DIRECTORY_ENUM_CONTEXT {
    ULONG Signature;
    ULONG NextEntryIndex;
    ULONGLONG ParentIdLow;
    ULONGLONG ParentIdHigh;
    USHORT PatternLength;
    WCHAR Pattern[1];
} OPENFS_DIRECTORY_ENUM_CONTEXT, *POPENFS_DIRECTORY_ENUM_CONTEXT;

#define OPENFS_DIRECTORY_ENUM_SIGNATURE 0x4F444952UL
#define OPENFS_DIRECTORY_ENUM_POOL_TAG 'eFsO'

static BOOLEAN
OpenFsMatchDirectoryPattern(
    _In_reads_(PatternLength) const WCHAR* Pattern,
    _In_ USHORT PatternLength,
    _In_reads_(NameLength) const WCHAR* Name,
    _In_ USHORT NameLength
    )
{
    USHORT p = 0, n = 0;
    USHORT star = MAXUSHORT, retry = 0;

    while (n < NameLength) {
        if (p < PatternLength &&
            (Pattern[p] == L'?' || Pattern[p] == Name[n])) {
            ++p;
            ++n;
        } else if (p < PatternLength && Pattern[p] == L'*') {
            star = p++;
            retry = n;
        } else if (star != MAXUSHORT) {
            p = (USHORT)(star + 1u);
            n = ++retry;
        } else {
            return FALSE;
        }
    }
    while (p < PatternLength && Pattern[p] == L'*') ++p;
    return p == PatternLength;
}

static VOID
OpenFsFreeDirectoryContext(
    _Inout_ PFILE_OBJECT FileObject
    )
{
    POPENFS_DIRECTORY_ENUM_CONTEXT context =
        (POPENFS_DIRECTORY_ENUM_CONTEXT)FileObject->FsContext2;
    if (context != NULL &&
        context->Signature == OPENFS_DIRECTORY_ENUM_SIGNATURE) {
        context->Signature = 0;
        ExFreePoolWithTag(context, OPENFS_DIRECTORY_ENUM_POOL_TAG);
    }
    FileObject->FsContext2 = NULL;
}

static NTSTATUS
OpenFsGetDirectoryAllocationSize(
    _In_ ULONGLONG FileSize,
    _In_ ULONG BlockSize,
    _Out_ PLONGLONG AllocationSize
    )
{
    ULONGLONG allocation = FileSize;

    if (AllocationSize == NULL || BlockSize == 0 ||
        FileSize > MAXLONGLONG)
        return STATUS_INVALID_PARAMETER;

    if (allocation != 0) {
        ULONGLONG remainder = allocation % BlockSize;
        if (remainder != 0) {
            ULONGLONG padding = BlockSize - remainder;
            if (allocation > (ULONGLONG)MAXLONGLONG - padding)
                return STATUS_INTEGER_OVERFLOW;
            allocation += padding;
        }
    }

    *AllocationSize = (LONGLONG)allocation;
    return STATUS_SUCCESS;
}

static NTSTATUS
OpenFsDispatchDirectoryControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB directoryFcb;
    POPENFS_DIRECTORY_ENUM_CONTEXT context;
    PVOID output = Irp->AssociatedIrp.SystemBuffer;
    ULONG outputLength = stack->Parameters.QueryDirectory.Length;
    ULONG bytesUsed = 0;
    ULONG previousOffset = MAXULONG;
    ULONG entryIndex;
    ULONG flags = stack->Flags;
    FILE_INFORMATION_CLASS infoClass =
        stack->Parameters.QueryDirectory.FileInformationClass;
    NTSTATUS status = STATUS_SUCCESS;

    if (stack->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    if (DeviceObject == NULL || DeviceObject == g_OpenFsFileSystemDevice ||
        fileObject == NULL || fileObject->FsContext == NULL ||
        output == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    if (infoClass != FileNamesInformation &&
        infoClass != FileDirectoryInformation &&
        infoClass != FileFullDirectoryInformation &&
        infoClass != FileBothDirectoryInformation &&
        infoClass != FileIdFullDirectoryInformation &&
        infoClass != FileIdBothDirectoryInformation)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_INFO_CLASS);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    directoryFcb = (POPENFS_FCB)fileObject->FsContext;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE ||
        directoryFcb->Signature != OPENFS_FCB_SIGNATURE)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);
    if (!ExAcquireRundownProtection(&vcb->RundownProtection))
        return OpenFsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED);
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    if (vcb->State != OpenFsVolumeMounted || vcb->Namespace == NULL ||
        directoryFcb->Kind != 2u) {
        status = directoryFcb->Kind == 2u
            ? STATUS_VOLUME_DISMOUNTED : STATUS_NOT_A_DIRECTORY;
        goto DirectoryExit;
    }

    context = (POPENFS_DIRECTORY_ENUM_CONTEXT)fileObject->FsContext2;
    if (context == NULL) {
        PUNICODE_STRING expression =
            stack->Parameters.QueryDirectory.FileName;
        USHORT patternLength = expression != NULL ? expression->Length : 0;
        SIZE_T allocationSize;
        if ((patternLength & 1u) != 0 || patternLength > 1024u ||
            (patternLength != 0 && expression->Buffer == NULL)) {
            status = STATUS_OBJECT_NAME_INVALID;
            goto DirectoryExit;
        }
        allocationSize = FIELD_OFFSET(OPENFS_DIRECTORY_ENUM_CONTEXT, Pattern) +
            (patternLength != 0 ? patternLength : sizeof(WCHAR));
        context = (POPENFS_DIRECTORY_ENUM_CONTEXT)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, allocationSize,
            OPENFS_DIRECTORY_ENUM_POOL_TAG);
        if (context == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto DirectoryExit;
        }
        RtlZeroMemory(context, allocationSize);
        context->Signature = OPENFS_DIRECTORY_ENUM_SIGNATURE;
        context->ParentIdLow = directoryFcb->ObjectIdLow;
        context->ParentIdHigh = directoryFcb->ObjectIdHigh;
        if (patternLength != 0) {
            context->PatternLength = (USHORT)(patternLength / sizeof(WCHAR));
            RtlCopyMemory(context->Pattern, expression->Buffer, patternLength);
        } else {
            context->Pattern[0] = L'*';
            context->PatternLength = 1;
        }
        fileObject->FsContext2 = context;
    } else if (context->Signature != OPENFS_DIRECTORY_ENUM_SIGNATURE ||
        context->ParentIdLow != directoryFcb->ObjectIdLow ||
        context->ParentIdHigh != directoryFcb->ObjectIdHigh) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto DirectoryExit;
    }

    /*
     * A restarted enumeration may carry a new search expression. Replace the
     * per-handle context before resetting the cursor so a failed allocation
     * leaves the previous enumeration state intact.
     */
    if ((flags & SL_RESTART_SCAN) != 0) {
        PUNICODE_STRING expression =
            stack->Parameters.QueryDirectory.FileName;
        if (expression != NULL) {
            USHORT patternLength = expression->Length;
            SIZE_T allocationSize;
            POPENFS_DIRECTORY_ENUM_CONTEXT replacement;

            if ((patternLength & 1u) != 0 || patternLength > 1024u ||
                (patternLength != 0 && expression->Buffer == NULL)) {
                status = STATUS_OBJECT_NAME_INVALID;
                goto DirectoryExit;
            }

            allocationSize = FIELD_OFFSET(OPENFS_DIRECTORY_ENUM_CONTEXT, Pattern) +
                (patternLength != 0 ? patternLength : sizeof(WCHAR));
            replacement = (POPENFS_DIRECTORY_ENUM_CONTEXT)ExAllocatePool2(
                POOL_FLAG_NON_PAGED, allocationSize,
                OPENFS_DIRECTORY_ENUM_POOL_TAG);
            if (replacement == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto DirectoryExit;
            }
            RtlZeroMemory(replacement, allocationSize);
            replacement->Signature = OPENFS_DIRECTORY_ENUM_SIGNATURE;
            replacement->ParentIdLow = directoryFcb->ObjectIdLow;
            replacement->ParentIdHigh = directoryFcb->ObjectIdHigh;
            if (patternLength != 0) {
                replacement->PatternLength =
                    (USHORT)(patternLength / sizeof(WCHAR));
                RtlCopyMemory(replacement->Pattern, expression->Buffer,
                    patternLength);
            } else {
                replacement->Pattern[0] = L'*';
                replacement->PatternLength = 1;
            }

            context->Signature = 0;
            ExFreePoolWithTag(context, OPENFS_DIRECTORY_ENUM_POOL_TAG);
            fileObject->FsContext2 = replacement;
            context = replacement;
        }
        context->NextEntryIndex = 0;
    }
    if ((flags & SL_INDEX_SPECIFIED) != 0)
        context->NextEntryIndex = stack->Parameters.QueryDirectory.FileIndex;

    entryIndex = context->NextEntryIndex;
    while (entryIndex < vcb->Namespace->EntryCount) {
        const OPENFS_DIRECTORY_ENTRY* entry =
            &vcb->Namespace->Entries[entryIndex];
        WCHAR unicodeName[OPENFS_DIRECTORY_NAME_MAX_BYTES];
        ULONG unicodeBytes = 0;
        USHORT unicodeLength;
        ULONG recordIndex;
        const OPENFS_METADATA_RECORD* record;
        ULONG required;
        ULONG recordOffset;
        PVOID recordBuffer;

        ++entryIndex;
        context->NextEntryIndex = entryIndex;
        if (entry->ParentIdLow != context->ParentIdLow ||
            entry->ParentIdHigh != context->ParentIdHigh)
            continue;

        status = RtlUTF8ToUnicodeN((PWSTR)unicodeName,
            sizeof(unicodeName), &unicodeBytes,
            (PCCH)entry->Name, entry->NameLength);
        if (!NT_SUCCESS(status)) goto DirectoryExit;
        if ((unicodeBytes & 1u) != 0 ||
            unicodeBytes > sizeof(unicodeName)) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto DirectoryExit;
        }
        unicodeLength = (USHORT)(unicodeBytes / sizeof(WCHAR));
        if (!OpenFsMatchDirectoryPattern(context->Pattern,
                context->PatternLength, unicodeName, unicodeLength))
            continue;

        for (recordIndex = 0;
             recordIndex < vcb->Namespace->RecordCount; ++recordIndex) {
            const OPENFS_METADATA_RECORD* candidate =
                &vcb->Namespace->Records[recordIndex];
            if (candidate->ObjectIdLow == entry->ObjectIdLow &&
                candidate->ObjectIdHigh == entry->ObjectIdHigh)
                break;
        }
        if (recordIndex == vcb->Namespace->RecordCount) {
            status = STATUS_DISK_CORRUPT_ERROR;
            goto DirectoryExit;
        }
        record = &vcb->Namespace->Records[recordIndex];

        switch (infoClass) {
        case FileNamesInformation:
            required = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) +
                unicodeBytes;
            break;
        case FileDirectoryInformation:
            required = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) +
                unicodeBytes;
            break;
        case FileFullDirectoryInformation:
            required = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) +
                unicodeBytes;
            break;
        case FileBothDirectoryInformation:
            required = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) +
                unicodeBytes;
            break;
        case FileIdFullDirectoryInformation:
            required = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName) +
                unicodeBytes;
            break;
        case FileIdBothDirectoryInformation:
            required = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) +
                unicodeBytes;
            break;
        default:
            status = STATUS_INVALID_INFO_CLASS;
            goto DirectoryExit;
        }
        /*
         * Only the start of each record (and therefore each non-final
         * NextEntryOffset) must be 8-byte aligned. The final record may end
         * exactly at the caller's buffer boundary without trailing padding.
         */
        if (bytesUsed > MAXULONG - 7u) {
            status = STATUS_INTEGER_OVERFLOW;
            goto DirectoryExit;
        }
        recordOffset = (bytesUsed + 7u) & ~7u;
        if (recordOffset < bytesUsed || recordOffset > outputLength ||
            required > outputLength - recordOffset) {
            /*
             * The current matching entry has not been copied. Keep the
             * enumeration cursor on it so the next query retries this entry
             * instead of silently skipping it after a short output buffer.
             */
            context->NextEntryIndex = entryIndex - 1u;
            if (bytesUsed == 0) status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (recordOffset > bytesUsed)
            RtlZeroMemory((PUCHAR)output + bytesUsed,
                recordOffset - bytesUsed);
        recordBuffer = (PUCHAR)output + recordOffset;
        RtlZeroMemory(recordBuffer, required);
        {
            LONGLONG allocationSize;
            status = OpenFsGetDirectoryAllocationSize(record->Size,
                vcb->Geometry.BlockSize, &allocationSize);
            if (!NT_SUCCESS(status))
                goto DirectoryExit;

            switch (infoClass) {
            case FileNamesInformation:
            {
                PFILE_NAMES_INFORMATION info =
                    (PFILE_NAMES_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            case FileDirectoryInformation:
            {
                PFILE_DIRECTORY_INFORMATION info =
                    (PFILE_DIRECTORY_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->EndOfFile.QuadPart = (LONGLONG)record->Size;
                info->AllocationSize.QuadPart = allocationSize;
                info->FileAttributes = record->Kind == 2u
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            case FileFullDirectoryInformation:
            {
                PFILE_FULL_DIR_INFORMATION info =
                    (PFILE_FULL_DIR_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->EndOfFile.QuadPart = (LONGLONG)record->Size;
                info->AllocationSize.QuadPart = allocationSize;
                info->FileAttributes = record->Kind == 2u
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->EaSize = 0;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            case FileBothDirectoryInformation:
            {
                PFILE_BOTH_DIR_INFORMATION info =
                    (PFILE_BOTH_DIR_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->EndOfFile.QuadPart = (LONGLONG)record->Size;
                info->AllocationSize.QuadPart = allocationSize;
                info->FileAttributes = record->Kind == 2u
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->EaSize = 0;
                info->ShortNameLength = 0;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            case FileIdFullDirectoryInformation:
            {
                PFILE_ID_FULL_DIR_INFORMATION info =
                    (PFILE_ID_FULL_DIR_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->EndOfFile.QuadPart = (LONGLONG)record->Size;
                info->AllocationSize.QuadPart = allocationSize;
                info->FileAttributes = record->Kind == 2u
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->EaSize = 0;
                info->FileId.QuadPart = (LONGLONG)record->ObjectIdLow;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            case FileIdBothDirectoryInformation:
            {
                PFILE_ID_BOTH_DIR_INFORMATION info =
                    (PFILE_ID_BOTH_DIR_INFORMATION)recordBuffer;
                info->FileIndex = entryIndex - 1u;
                info->EndOfFile.QuadPart = (LONGLONG)record->Size;
                info->AllocationSize.QuadPart = allocationSize;
                info->FileAttributes = record->Kind == 2u
                    ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                info->EaSize = 0;
                info->ShortNameLength = 0;
                info->FileId.QuadPart = (LONGLONG)record->ObjectIdLow;
                info->FileNameLength = unicodeBytes;
                RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
                break;
            }
            default:
                status = STATUS_INVALID_INFO_CLASS;
                goto DirectoryExit;
            }
        }
        if (previousOffset != MAXULONG) {
            *(PULONG)((PUCHAR)output + previousOffset) =
                recordOffset - previousOffset;
        }
        previousOffset = recordOffset;
        bytesUsed = recordOffset + required;
        if ((flags & SL_RETURN_SINGLE_ENTRY) != 0) break;
    }

    if (bytesUsed == 0 && NT_SUCCESS(status))
        status = STATUS_NO_MORE_FILES;
    else if (bytesUsed != 0)
        status = STATUS_SUCCESS;

DirectoryExit:
    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = NT_SUCCESS(status) ? bytesUsed : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS
OpenFsDispatchCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fileObject = stack->FileObject;
    POPENFS_VCB vcb;
    POPENFS_FCB fcb;

    if (fileObject != NULL)
        OpenFsFreeDirectoryContext(fileObject);

    if (DeviceObject == NULL || fileObject == NULL ||
        fileObject->FsContext == NULL ||
        DeviceObject == g_OpenFsFileSystemDevice)
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    fcb = (POPENFS_FCB)fileObject->FsContext;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE ||
        fcb == NULL || fcb->Signature != OPENFS_FCB_SIGNATURE)
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

    if (ExAcquireRundownProtection(&vcb->RundownProtection)) {
        if (ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
            /*
             * IRP_MJ_CLEANUP is the point at which share reservations are
             * released; the FCB remains referenced until IRP_MJ_CLOSE.
             */
            IoRemoveShareAccess(fileObject, &fcb->ShareAccess);
            ExReleaseResourceLite(&vcb->Resource);
        }
        ExReleaseRundownProtection(&vcb->RundownProtection);
    }

    return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
OpenFsDispatchFileSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    /*
     * Volume FSCTLs arrive on the mounted VDO with USER_FS_REQUEST as the
     * minor function. Keep this path separate from mount-manager requests
     * sent to the filesystem control device.
     */
    if (DeviceObject != g_OpenFsFileSystemDevice) {
        OPENFS_VOLUME_STATE state;
        ULONG controlCode;

        if (DeviceObject == NULL ||
            stack->MinorFunction != IRP_MN_USER_FS_REQUEST)
            return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

        controlCode = stack->Parameters.FileSystemControl.FsControlCode;
        if (controlCode == FSCTL_QUERY_ALLOCATED_RANGES) {
            PFILE_ALLOCATED_RANGE_BUFFER input =
                (PFILE_ALLOCATED_RANGE_BUFFER)
                stack->Parameters.FileSystemControl.Type3InputBuffer;
            PFILE_ALLOCATED_RANGE_BUFFER output =
                (PFILE_ALLOCATED_RANGE_BUFFER)Irp->UserBuffer;
            FILE_ALLOCATED_RANGE_BUFFER inputCopy;
            POPENFS_VCB vcb;
            POPENFS_FCB fcb;
            ULONGLONG requestStart;
            ULONGLONG requestEnd;
            ULONGLONG fileSize;
            ULONG blockSize;
            ULONGLONG currentVcn;
            ULONG outputLength =
                stack->Parameters.FileSystemControl.OutputBufferLength;
            ULONG capacity;
            ULONG returned = 0;
            NTSTATUS status = STATUS_SUCCESS;

            /*
             * QUERY_ALLOCATED_RANGES is METHOD_NEITHER. Capture the range
             * before using it and probe the output before writing records.
             */
            if (input == NULL || output == NULL ||
                stack->Parameters.FileSystemControl.InputBufferLength <
                    sizeof(FILE_ALLOCATED_RANGE_BUFFER) ||
                outputLength < sizeof(FILE_ALLOCATED_RANGE_BUFFER))
                return OpenFsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL);

            __try {
                if (Irp->RequestorMode != KernelMode) {
                    ProbeForRead(input, sizeof(*input),
                        TYPE_ALIGNMENT(FILE_ALLOCATED_RANGE_BUFFER));
                    ProbeForWrite(output, outputLength,
                        TYPE_ALIGNMENT(FILE_ALLOCATED_RANGE_BUFFER));
                }
                inputCopy = *input;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return OpenFsCompleteIrp(Irp, GetExceptionCode());
            }

            if (inputCopy.FileOffset.QuadPart < 0 ||
                inputCopy.Length.QuadPart < 0)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

            requestStart = (ULONGLONG)inputCopy.FileOffset.QuadPart;
            if ((ULONGLONG)inputCopy.Length.QuadPart >
                MAXULONGLONG - requestStart)
                requestEnd = MAXULONGLONG;
            else
                requestEnd = requestStart +
                    (ULONGLONG)inputCopy.Length.QuadPart;

            if (stack->FileObject == NULL ||
                stack->FileObject->FsContext == NULL)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

            vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
            fcb = (POPENFS_FCB)stack->FileObject->FsContext;
            if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

            status = OpenFsQueryFcbMetadata(DeviceObject, fcb,
                &fileSize, &blockSize);
            if (!NT_SUCCESS(status))
                return OpenFsCompleteIrp(Irp, status);
            if (blockSize == 0 || (blockSize & (blockSize - 1u)) != 0)
                return OpenFsCompleteIrp(Irp, STATUS_DISK_CORRUPT_ERROR);
            if (requestStart >= fileSize || requestStart >= requestEnd)
                return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
            if (requestEnd > fileSize)
                requestEnd = fileSize;
            /*
             * FILE_ALLOCATED_RANGE_BUFFER uses signed LARGE_INTEGER values.
             * Never publish an offset or length that cannot be represented.
             */
            if (requestEnd > MAXLONGLONG)
                requestEnd = MAXLONGLONG;

            capacity = outputLength / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            currentVcn = requestStart / blockSize;
            __try {
                RtlZeroMemory(output, outputLength);
                while (currentVcn < (requestEnd / blockSize) +
                        ((requestEnd % blockSize) != 0)) {
                    ULONGLONG nextVcn = 0;
                    ULONGLONG physicalBlock = 0;
                    ULONGLONG runStart;
                    ULONGLONG runEnd;
                    BOOLEAN allocated = FALSE;

                    ULONGLONG fileBlockCount = 0;
                    status = OpenFsGetFcbLogicalRun(DeviceObject, fcb,
                        currentVcn, &fileBlockCount, &nextVcn,
                        &physicalBlock, &allocated);
                    if (!NT_SUCCESS(status))
                        break;
                    if (nextVcn <= currentVcn) {
                        status = STATUS_DISK_CORRUPT_ERROR;
                        break;
                    }

                    if (allocated) {
                        if (currentVcn > MAXULONGLONG / blockSize ||
                            nextVcn > MAXULONGLONG / blockSize) {
                            status = STATUS_INTEGER_OVERFLOW;
                            break;
                        }
                        runStart = currentVcn * blockSize;
                        runEnd = nextVcn * blockSize;
                        if (runStart < requestStart)
                            runStart = requestStart;
                        if (runEnd > requestEnd)
                            runEnd = requestEnd;
                        if (runStart < runEnd) {
                            BOOLEAN merged = FALSE;

                            /*
                             * Adjacent logical allocations form one
                             * allocated byte range even when represented by
                             * separate, physically fragmented extent records.
                             */
                            if (returned != 0) {
                                PFILE_ALLOCATED_RANGE_BUFFER previous =
                                    &output[returned - 1u];
                                ULONGLONG previousStart =
                                    (ULONGLONG)previous->FileOffset.QuadPart;
                                ULONGLONG previousLength =
                                    (ULONGLONG)previous->Length.QuadPart;

                                if (previousLength <=
                                        MAXULONGLONG - previousStart &&
                                    previousStart + previousLength == runStart) {
                                    previous->Length.QuadPart =
                                        (LONGLONG)(runEnd - previousStart);
                                    merged = TRUE;
                                }
                            }

                            if (!merged) {
                                if (returned == capacity) {
                                    status = STATUS_BUFFER_OVERFLOW;
                                    break;
                                }
                                output[returned].FileOffset.QuadPart =
                                    (LONGLONG)runStart;
                                output[returned].Length.QuadPart =
                                    (LONGLONG)(runEnd - runStart);
                                ++returned;
                            }
                        }
                    }
                    currentVcn = nextVcn;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
                returned = 0;
            }

            Irp->IoStatus.Information =
                returned * sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
                Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        if (controlCode == FSCTL_GET_RETRIEVAL_POINTERS) {
            PSTARTING_VCN_INPUT_BUFFER input =
                (PSTARTING_VCN_INPUT_BUFFER)
                stack->Parameters.FileSystemControl.Type3InputBuffer;
            PRETRIEVAL_POINTERS_BUFFER output =
                (PRETRIEVAL_POINTERS_BUFFER)Irp->UserBuffer;
            STARTING_VCN_INPUT_BUFFER inputCopy;
            POPENFS_VCB vcb;
            POPENFS_FCB fcb;
            OPENFS_VOLUME_LAYOUT layout;
            ULONGLONG currentVcn;
            ULONGLONG fileBlockCount = 0;
            ULONG outputLength =
                stack->Parameters.FileSystemControl.OutputBufferLength;
            ULONG outputHeader =
                FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents);
            ULONG extentBytes = sizeof(output->Extents[0]);
            ULONG capacity;
            ULONG returned = 0;
            NTSTATUS status;

            /*
             * FSCTL_GET_RETRIEVAL_POINTERS is METHOD_NEITHER. VCN input is
             * captured before use and the output is written under SEH.
             */
            if (input == NULL || output == NULL ||
                stack->Parameters.FileSystemControl.InputBufferLength <
                    sizeof(STARTING_VCN_INPUT_BUFFER) ||
                outputLength < outputHeader + extentBytes)
                return OpenFsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL);

            __try {
                if (Irp->RequestorMode != KernelMode) {
                    ProbeForRead(input, sizeof(*input),
                        TYPE_ALIGNMENT(STARTING_VCN_INPUT_BUFFER));
                    ProbeForWrite(output, outputLength,
                        TYPE_ALIGNMENT(RETRIEVAL_POINTERS_BUFFER));
                }
                inputCopy = *input;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return OpenFsCompleteIrp(Irp, GetExceptionCode());
            }

            if (inputCopy.StartingVcn.QuadPart < 0)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

            if (stack->FileObject == NULL ||
                stack->FileObject->FsContext == NULL)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

            vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
            fcb = (POPENFS_FCB)stack->FileObject->FsContext;
            if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

            status = OpenFsCalculateVolumeLayout(vcb->Geometry.BlockSize,
                vcb->Geometry.TotalBlocks, vcb->Geometry.Version, &layout);
            if (!NT_SUCCESS(status))
                return OpenFsCompleteIrp(Irp, status);
            if (layout.BackupSuperblockBlock <= layout.DataStartBlock)
                return OpenFsCompleteIrp(Irp, STATUS_DISK_CORRUPT_ERROR);

            currentVcn = (ULONGLONG)inputCopy.StartingVcn.QuadPart;

            capacity = (outputLength - outputHeader) / extentBytes;
            __try {
                RtlZeroMemory(output, outputLength);
                output->StartingVcn = inputCopy.StartingVcn;

                while (returned < capacity) {
                    ULONGLONG nextVcn = 0;
                    ULONGLONG physicalBlock = 0;
                    BOOLEAN allocated = FALSE;

                    status = OpenFsGetFcbLogicalRun(DeviceObject, fcb,
                        currentVcn, &fileBlockCount, &nextVcn,
                        &physicalBlock, &allocated);
                    if (!NT_SUCCESS(status)) {
                        if (status == STATUS_INVALID_PARAMETER &&
                            currentVcn >= fileBlockCount)
                            status = STATUS_END_OF_FILE;
                        break;
                    }
                    if (currentVcn >= fileBlockCount) {
                        status = STATUS_END_OF_FILE;
                        break;
                    }
                    if (nextVcn <= currentVcn || nextVcn > fileBlockCount) {
                        status = STATUS_DISK_CORRUPT_ERROR;
                        break;
                    }

                    output->Extents[returned].NextVcn.QuadPart =
                        (LONGLONG)nextVcn;
                    if (!allocated) {
                        output->Extents[returned].Lcn.QuadPart = -1;
                    } else {
                        if (physicalBlock < layout.DataStartBlock ||
                            physicalBlock >= layout.BackupSuperblockBlock) {
                            status = STATUS_DISK_CORRUPT_ERROR;
                            break;
                        }
                        output->Extents[returned].Lcn.QuadPart =
                            (LONGLONG)(physicalBlock - layout.DataStartBlock);
                    }
                    currentVcn = nextVcn;
                    ++returned;
                    if (currentVcn >= fileBlockCount)
                        break;
                }

                output->ExtentCount = returned;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
                returned = 0;
            }

            if (returned != 0) {
                Irp->IoStatus.Information =
                    outputHeader + returned * extentBytes;
                if (NT_SUCCESS(status) && currentVcn < fileBlockCount)
                    status = STATUS_BUFFER_OVERFLOW;
            } else if (NT_SUCCESS(status)) {
                status = STATUS_BUFFER_TOO_SMALL;
            }

            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
                Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        if (controlCode == FSCTL_GET_VOLUME_BITMAP) {
            PSTARTING_LCN_INPUT_BUFFER input =
                (PSTARTING_LCN_INPUT_BUFFER)
                stack->Parameters.FileSystemControl.Type3InputBuffer;
            PVOLUME_BITMAP_BUFFER output =
                (PVOLUME_BITMAP_BUFFER)Irp->UserBuffer;
            STARTING_LCN_INPUT_BUFFER inputCopy;
            POPENFS_VCB vcb;
            OPENFS_VOLUME_LAYOUT layout;
            ULONGLONG startingLcn;
            ULONGLONG totalDataBlocks;
            ULONGLONG bitsRead = 0;
            ULONG outputLength =
                stack->Parameters.FileSystemControl.OutputBufferLength;
            ULONG outputHeader = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
            SIZE_T bitmapBytes;
            NTSTATUS status;

            /*
             * FSCTL_GET_VOLUME_BITMAP is METHOD_NEITHER: input is supplied
             * through Type3InputBuffer and output through Irp->UserBuffer.
             * Never assume that either pointer is backed by SystemBuffer.
             */
            if (input == NULL || output == NULL ||
                stack->Parameters.FileSystemControl.InputBufferLength <
                    sizeof(STARTING_LCN_INPUT_BUFFER) ||
                outputLength <= outputHeader)
                return OpenFsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL);

            __try {
                if (Irp->RequestorMode != KernelMode) {
                    ProbeForRead(input, sizeof(*input),
                        TYPE_ALIGNMENT(STARTING_LCN_INPUT_BUFFER));
                    ProbeForWrite(output, outputLength,
                        TYPE_ALIGNMENT(ULONG));
                }
                inputCopy = *input;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return OpenFsCompleteIrp(Irp, GetExceptionCode());
            }

            if (inputCopy.StartingLcn.QuadPart < 0)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
            startingLcn = (ULONGLONG)inputCopy.StartingLcn.QuadPart;

            vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
            if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);
            if (!ExAcquireRundownProtection(&vcb->RundownProtection))
                return OpenFsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED);
            if (!ExAcquireResourceSharedLite(&vcb->Resource, TRUE)) {
                ExReleaseRundownProtection(&vcb->RundownProtection);
                return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
            }

            if (vcb->State != OpenFsVolumeMounted ||
                vcb->Geometry.BlockSize == 0) {
                status = STATUS_VOLUME_DISMOUNTED;
                goto BitmapExit;
            }
            status = OpenFsCalculateVolumeLayout(vcb->Geometry.BlockSize,
                vcb->Geometry.TotalBlocks, vcb->Geometry.Version, &layout);
            if (!NT_SUCCESS(status)) goto BitmapExit;
            if (layout.BackupSuperblockBlock <= layout.DataStartBlock) {
                status = STATUS_DISK_CORRUPT_ERROR;
                goto BitmapExit;
            }
            totalDataBlocks =
                layout.BackupSuperblockBlock - layout.DataStartBlock;
            if (totalDataBlocks > MAXLONGLONG) {
                status = STATUS_INTEGER_OVERFLOW;
                goto BitmapExit;
            }
            if (startingLcn >= totalDataBlocks) {
                status = STATUS_INVALID_PARAMETER;
                goto BitmapExit;
            }

            bitmapBytes = outputLength - outputHeader;
            __try {
                RtlZeroMemory(output, outputLength);
                output->StartingLcn.QuadPart = (LONGLONG)startingLcn;
                output->BitmapSize.QuadPart = (LONGLONG)totalDataBlocks;
                status = OpenFsReadDataBitmap(vcb->RealDeviceObject,
                    &vcb->Geometry, layout.DataStartBlock, totalDataBlocks,
                    startingLcn, output->Buffer, bitmapBytes, &bitsRead);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
            }

            if (NT_SUCCESS(status)) {
                /*
                 * Return the bytes that fit and let the caller continue from
                 * the next LCN when the supplied output buffer is too small.
                 */
                Irp->IoStatus.Information = outputHeader +
                    (ULONG)((bitsRead + 7u) / 8u);
                if (bitsRead < totalDataBlocks - startingLcn)
                    status = STATUS_BUFFER_OVERFLOW;
            }

BitmapExit:
            ExReleaseResourceLite(&vcb->Resource);
            ExReleaseRundownProtection(&vcb->RundownProtection);
            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
                Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }
        if (controlCode == FSCTL_IS_VOLUME_MOUNTED) {
            NTSTATUS status = OpenFsQueryVolumeState(DeviceObject, &state);
            if (!NT_SUCCESS(status))
                return OpenFsCompleteIrp(Irp, status);
            return OpenFsCompleteIrp(Irp,
                state == OpenFsVolumeMounted
                    ? STATUS_SUCCESS : STATUS_VOLUME_DISMOUNTED);
        }

        if (controlCode == FSCTL_LOCK_VOLUME) {
            if (stack->FileObject == NULL)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
            return OpenFsCompleteIrp(Irp,
                OpenFsLockVolume(DeviceObject, stack->FileObject));
        }
        if (controlCode == FSCTL_UNLOCK_VOLUME) {
            if (stack->FileObject == NULL)
                return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);
            return OpenFsCompleteIrp(Irp,
                OpenFsUnlockVolume(DeviceObject, stack->FileObject));
        }

        /*
         * Dismount remains disabled until VPB unpublication and deferred VDO
         * teardown can be committed as one lifetime-safe operation.
         */
        if (controlCode == FSCTL_DISMOUNT_VOLUME)
            return OpenFsCompleteIrp(Irp, STATUS_DEVICE_BUSY);

        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }

    switch (stack->MinorFunction) {
    case IRP_MN_LOAD_FILE_SYSTEM:
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

    case IRP_MN_MOUNT_VOLUME:
    {
        OPENFS_VOLUME_GEOMETRY geometry;
        POPENFS_NAMESPACE_SNAPSHOT namespaceSnapshot = NULL;
        PDEVICE_OBJECT privateVolumeDevice = NULL;
        PDEVICE_OBJECT realDeviceObject;
        PVPB vpb = stack->Parameters.MountVolume.Vpb;
        BOOLEAN vpbReserved = FALSE;
        NTSTATUS status;

        realDeviceObject = stack->Parameters.MountVolume.DeviceObject;
        if (realDeviceObject == NULL || vpb == NULL)
            return OpenFsCompleteIrp(Irp, STATUS_INVALID_PARAMETER);

        status = OpenFsProbeVolume(realDeviceObject, &geometry,
            &namespaceSnapshot);
        if (!NT_SUCCESS(status))
            return OpenFsCompleteIrp(Irp, status);

        status = OpenFsCreateVolumeDevice(DeviceObject->DriverObject,
            realDeviceObject, &geometry, &privateVolumeDevice);
        if (!NT_SUCCESS(status))
            goto MountRollback;

        status = OpenFsSetVolumeNamespace(privateVolumeDevice,
            namespaceSnapshot);
        if (!NT_SUCCESS(status))
            goto MountRollback;
        namespaceSnapshot = NULL; /* Ownership transferred to the VCB. */

        status = OpenFsReserveVolumeVpb(privateVolumeDevice, vpb);
        if (!NT_SUCCESS(status))
            goto MountRollback;
        vpbReserved = TRUE;

        status = OpenFsPublishVolumeVpb(privateVolumeDevice, vpb);
        if (!NT_SUCCESS(status))
            goto MountRollback;

        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
            "OpenFS: mounted volume (revision %u, block size %lu, blocks %I64u)\n",
            (ULONG)geometry.Version, geometry.BlockSize, geometry.TotalBlocks);
        return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);

MountRollback:
        if (vpbReserved) {
            NTSTATUS releaseStatus =
                OpenFsReleaseVolumeVpb(privateVolumeDevice, vpb);
            if (NT_SUCCESS(status) && !NT_SUCCESS(releaseStatus))
                status = releaseStatus;
        }
        if (privateVolumeDevice != NULL) {
            NTSTATUS destroyStatus =
                OpenFsDestroyVolumeDevice(privateVolumeDevice);
            if (NT_SUCCESS(status) && !NT_SUCCESS(destroyStatus))
                status = destroyStatus;
        }
        if (namespaceSnapshot != NULL)
            OpenFsFreeNamespaceSnapshot(namespaceSnapshot);
        return OpenFsCompleteIrp(Irp, status);
    }

    case IRP_MN_USER_FS_REQUEST:
    case IRP_MN_VERIFY_VOLUME:
    case IRP_MN_KERNEL_CALL:
    default:
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static VOID
OpenFsUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_OpenFsFileSystemDevice != NULL) {
        IoUnregisterFileSystem(g_OpenFsFileSystemDevice);
        IoDeleteDevice(g_OpenFsFileSystemDevice);
        g_OpenFsFileSystemDevice = NULL;
    }
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    ULONG majorFunction;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName, L"\\FileSystem\\OpenFS");

    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_DISK_FILE_SYSTEM,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_OpenFsFileSystemDevice
        );

    if (!NT_SUCCESS(status)) {
        g_OpenFsFileSystemDevice = NULL;
        return status;
    }

    for (majorFunction = 0;
         majorFunction <= IRP_MJ_MAXIMUM_FUNCTION;
         ++majorFunction) {
        DriverObject->MajorFunction[majorFunction] =
            OpenFsDispatchUnsupported;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = OpenFsDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_READ] = OpenFsDispatchRead;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] =
        OpenFsDispatchFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] =
        OpenFsDispatchQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
        OpenFsDispatchQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] =
        OpenFsDispatchDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = OpenFsDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = OpenFsDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        OpenFsDispatchFileSystemControl;

    DriverObject->DriverUnload = OpenFsUnload;

    IoRegisterFileSystem(g_OpenFsFileSystemDevice);
    g_OpenFsFileSystemDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}
