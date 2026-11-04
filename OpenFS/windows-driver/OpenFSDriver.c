#include <ntifs.h>
#include "OpenFSSuperblock.h"
#include "OpenFSDeviceIo.h"
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
    NTSTATUS status;

    if (DeviceObject == g_OpenFsFileSystemDevice ||
        DeviceObject == NULL || fileObject == NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);

    vcb = (POPENFS_VCB)DeviceObject->DeviceExtension;
    if (vcb == NULL || vcb->Signature != OPENFS_VCB_SIGNATURE ||
        fileObject->FsContext != NULL)
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE);

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

    if (!ExAcquireRundownProtection(&vcb->RundownProtection))
        return OpenFsCompleteIrp(Irp, STATUS_DELETE_PENDING);
    if (!ExAcquireResourceExclusiveLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }

    if (vcb->Signature != OPENFS_VCB_SIGNATURE ||
        vcb->State != OpenFsVolumeMounted ||
        fcb->OpenHandleCount == MAXULONG) {
        status = STATUS_VOLUME_DISMOUNTED;
    } else {
        ++fcb->OpenHandleCount;
        fileObject->FsContext = fcb;
        status = STATUS_SUCCESS;
    }

    ExReleaseResourceLite(&vcb->Resource);
    ExReleaseRundownProtection(&vcb->RundownProtection);
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
    if (!ExAcquireResourceSharedLite(&vcb->Resource, TRUE)) {
        ExReleaseRundownProtection(&vcb->RundownProtection);
        return OpenFsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);
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
        infoClass != FileDirectoryInformation)
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

    if ((flags & SL_RESTART_SCAN) != 0) context->NextEntryIndex = 0;
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
        ULONG alignedRequired;
        PVOID recordBuffer;

        ++entryIndex;
        context->NextEntryIndex = entryIndex;
        if (entry->ParentIdLow != context->ParentIdLow ||
            entry->ParentIdHigh != context->ParentIdHigh)
            continue;

        status = RtlUTF8ToUnicodeN((PCHAR)unicodeName,
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

        if (infoClass == FileNamesInformation) {
            required = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) +
                unicodeBytes;
        } else {
            required = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) +
                unicodeBytes;
        }
        alignedRequired = (required + 7u) & ~7u;
        if (alignedRequired < required || bytesUsed > outputLength ||
            alignedRequired > outputLength - bytesUsed) {
            if (bytesUsed == 0) status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        recordBuffer = (PUCHAR)output + bytesUsed;
        RtlZeroMemory(recordBuffer, alignedRequired);
        if (infoClass == FileNamesInformation) {
            PFILE_NAMES_INFORMATION info =
                (PFILE_NAMES_INFORMATION)recordBuffer;
            info->FileIndex = entryIndex - 1u;
            info->FileNameLength = unicodeBytes;
            RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
        } else {
            PFILE_DIRECTORY_INFORMATION info =
                (PFILE_DIRECTORY_INFORMATION)recordBuffer;
            info->FileIndex = entryIndex - 1u;
            info->EndOfFile.QuadPart = (LONGLONG)record->Size;
            info->AllocationSize.QuadPart = (LONGLONG)record->Size;
            info->FileAttributes = record->Kind == 2u
                ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            info->FileNameLength = unicodeBytes;
            RtlCopyMemory(info->FileName, unicodeName, unicodeBytes);
        }
        if (previousOffset != MAXULONG) {
            if (infoClass == FileNamesInformation) {
                ((PFILE_NAMES_INFORMATION)((PUCHAR)output +
                    previousOffset))->NextEntryOffset = bytesUsed - previousOffset;
            } else {
                ((PFILE_DIRECTORY_INFORMATION)((PUCHAR)output +
                    previousOffset))->NextEntryOffset = bytesUsed - previousOffset;
            }
        }
        previousOffset = bytesUsed;
        bytesUsed += alignedRequired;
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
    UNREFERENCED_PARAMETER(DeviceObject);
    if (IoGetCurrentIrpStackLocation(Irp)->FileObject != NULL)
        OpenFsFreeDirectoryContext(
            IoGetCurrentIrpStackLocation(Irp)->FileObject);
    return OpenFsCompleteIrp(Irp, STATUS_SUCCESS);
}

static NTSTATUS
OpenFsDispatchFileSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;

    if (DeviceObject != g_OpenFsFileSystemDevice) {
        return OpenFsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }

    stack = IoGetCurrentIrpStackLocation(Irp);

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
