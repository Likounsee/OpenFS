# OpenFS 1.0 — native Windows filesystem driver

This directory is the WDK-based kernel-mode filesystem driver track. It is deliberately separate from the Rust workspace: the existing Rust/Dokany adapter remains the development fallback while this driver is brought up.

## Product version

This driver is being developed for the **OpenFS 1.0** release. The `Version` field stored in an on-disk superblock is an internal format-revision number; it is not the OpenFS product/release version. Do not market or label this project as OpenFS 2, 3, or 4 based on those internal values.

## Current status

The first milestone is only a loadable filesystem-driver skeleton:

- registers an OpenFS filesystem device with the Windows I/O manager;
- receives filesystem mount notifications;
- queries the target volume length and performs aligned, read-only reads of the primary and backup superblocks;
- validates magic, internal format revision, CRC32C, geometry, reserved bytes, and the exact volume length;
- reports a valid OpenFS volume as recognized but returns `STATUS_NOT_IMPLEMENTED` until VCB/VPB mounting and teardown are implemented;
- does **not** expose a mounted volume, format media, or read/write files yet.

Do not install this development skeleton on a machine that needs a working OpenFS volume. It is not a replacement for the Dokany mount path.

## Why this is a separate driver

A standalone filesystem needs a filesystem driver (FSD), not a minifilter. Minifilters attach to an existing filesystem stack and are intended to observe or modify I/O; they do not independently provide a new on-disk filesystem. The native driver will eventually own volume objects, file objects, namespace operations, cache/memory-manager integration, and raw block I/O.

The current Rust `VolumeSession<D>` is the format and persistence reference. It currently depends on the Rust host environment and cannot be linked directly into a Windows kernel driver. The kernel implementation therefore needs a carefully reviewed kernel-safe format/I/O layer, with shared on-disk-format definitions and conformance tests to prevent divergence.

## Build

Requirements:

- Visual Studio with the Desktop development with C++ workload;
- Windows Driver Kit (WDK) matching the installed Windows SDK;
- x64 build tools.

From a WDK Developer Command Prompt:

```powershell
msbuild .\OpenFSDriver.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

The output is a development driver only. Production installation requires a properly signed package and driver-signing validation.

## Milestones

1. **Driver loading and registration** — this scaffold.
2. **Volume recognition** — validate OpenFS superblock, version, checksums, geometry, and sector size through the mounted storage device; reject malformed or unsupported media without writes.
3. **Mount lifecycle** — create/tear down VCB and VPB state, verify/dismount, volume information and filesystem control requests.
4. **Read-only filesystem** — open/create semantics for existing objects, directory enumeration, file reads, metadata queries.
5. **Writable filesystem** — create, write, truncate, rename, delete, flush, sharing and locking semantics.
6. **Windows formatting integration** — formatter, volume label/serial, format capabilities, Disk Management/Format UI integration where supported.
7. **Production hardening** — cache manager, memory manager, oplocks, security descriptors, cancellation, concurrency, crash recovery, HLK and signing.

No milestone is considered complete merely because the driver loads. Each requires tests against real Windows I/O paths and image-backed regression tests.


## Superblock recognition implementation

`OpenFSSuperblock.c` implements a read-only kernel-safe decoder for the current Rust format's superblock record:

- magic, supported internal on-disk format revisions, 48-byte header;
- CRC32C (Castagnoli);
- block-size and total-block bounds with overflow checks;
- reserved-byte checks;
- primary/backup geometry, UUID and stable-field consistency;
- exact volume byte-length validation.

The parser is called by the mount dispatch routine through `OpenFSDeviceIo.c`. The probe obtains the target device length with `IOCTL_DISK_GET_LENGTH_INFO`, reads the primary superblock at byte offset zero, derives the backup-superblock offset from the validated candidate block geometry, reads the backup copy, then validates both copies. Read buffers are nonpaged and aligned to the target device's alignment requirement; reads must respect its logical sector size.

A successful probe is only recognition, not a mount. The dispatch returns `STATUS_NOT_IMPLEMENTED` for a valid OpenFS volume until a VCB/VPB lifecycle, root directory, and teardown path are implemented. It must not report a successful mount before those pieces exist.

The host-side parser tests are built with strict C warnings in CI. They exercise supported revisions, CRC32C's standard test vector, differing revision-3/4 inventory hints, invalid magic/version/header, corrupt checksums, mismatched copies, invalid geometry, and device-length mismatch. These tests do not compile the WDK driver itself.
