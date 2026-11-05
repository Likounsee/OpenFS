# Writing files from Windows Explorer

## Current Windows write path

Windows Explorer file creation and writes are currently provided by the **Dokany user-mode mount adapter** in `crates/openfs-tool/src/windows_mount.rs`.

The native WDM filesystem driver in `windows-driver/` is still read-only. Do not interpret a successful Dokany write as proof that native WDM `IRP_MJ_WRITE` is implemented.

Dokany forwards Windows operations to the persistent `VolumeSession` API. The adapter supports:

- Creating files and directories.
- Copying file contents into OpenFS.
- Replacing and truncating files.
- Writes at explicit offsets and append-at-EOF.
- Reading files and enumerating directories.
- Renaming/moving files and directories.
- Deleting files and empty directories.
- Flushing on file cleanup and on volume unmount.

The adapter serializes access to its `VolumeSession` so concurrent callbacks cannot interleave path resolution, EOF growth and writes. It also checks Windows desired-access and share-access permissions per open handle.

## Mount an image

Install the Dokany runtime first, then create an OpenFS image and mount it:

```powershell
openfs create-gpt-image .\openfs-test.img 256
openfs mount .\openfs-test.img O:
```

Open `O:\` in File Explorer. Create folders, drag files into it, rename or delete files as needed. When finished, close applications using the volume and run:

```powershell
openfs unmount O:
```

The mount command remains active until the volume is unmounted.

## Mount a formatted physical disk

The disk must already contain a valid OpenFS volume (or one GPT partition with the OpenFS partition type GUID). Use the exact identity reported by `openfs list-devices`:

```powershell
openfs list-devices
openfs mount-disk <disk-index> <exact-UniqueId> <exact-model> <exact-serial> <exact-size-bytes> O:
```

The adapter rechecks the disk identity and geometry before opening the raw device. Do not guess the disk index, UniqueId, model, serial or capacity. The native Windows mount and Dokany mount must not be active on the same volume simultaneously.

## Write-path validation

The Windows CI job runs unit tests for the Dokany adapter. The write regression tests cover:

- A write beyond EOF, including the zero-filled sparse gap.
- Append mode using the current EOF rather than the caller's supplied offset.
- Rejection of negative offsets without changing file contents.
- Flush, close and reopen, followed by verification that written bytes persisted.

These tests do not replace testing with the actual Dokany runtime and a disposable virtual disk. Physical USB formatting remains a separate destructive operation and must only be attempted after the formatter and mount path have passed isolated VM validation.
