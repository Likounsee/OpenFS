# Raw block-device backend

**Status: low-level backend only. This is not yet a complete USB-storage workflow or a mounted filesystem.**

`openfs_block::RawDevice` opens an existing path read/write without create or truncate flags. It implements the same bounded block-device interface as the image backend. The caller must provide the logical block size and block count. For regular files, the length must exactly match the declared geometry; for device nodes, geometry is not discovered automatically. On Unix, only regular files and block devices are accepted: character devices, FIFOs, sockets, and other special files are rejected rather than being treated as disks.

The experimental V2 format now reserves its journal, validates its bitmap, and replays committed journal updates on open. V2 allocation/free and recovery tests exercise a file-backed image. A new test also exercises V2 format, allocation, reopen, free, and block reuse through `RawDevice` using a disposable regular file. This tests the shared backend contract, **not a physical USB device**.

## Safety constraints

- Opening the device does not write to it. Subsequent `write_blocks` calls write directly to the selected path and may overwrite partition tables, filesystem metadata, or user data.
- Do not use this backend on a disk containing data you need. Use a disposable image or dedicated test device.
- The API does not identify whether a path is a USB device, query trusted hardware geometry, or coordinate with mounted filesystems. RawDevice acquires an exclusive OS file lock against cooperating OpenFS handles; external software may ignore advisory locks.
- `flush` requests host-side synchronization through `File::sync_all`; it is not a proof that every USB bridge, controller, or device firmware has honored power-loss guarantees.
- There is no safe end-user raw-device formatter, persistent file/directory namespace, or OS mount adapter yet. The read-only `diagnose-device` CLI command inspects MBR entries and both GPT copies independently, even when the Protective MBR is invalid. It verifies header and partition-array CRCs, GPT header geometry and disk GUID presence, partition-entry GUID uniqueness, usable LBA bounds, and partition overlap. It prints populated partition ranges/names and compares the complete primary/backup partition arrays and key disk metadata. These checks diagnose partition-table structure only; they do not mount, repair, or modify the device. A volume that has been formatted as OpenFS is not automatically browsable as a normal USB drive in Windows or Linux.

## Validation status

CI must pass on Linux and Windows before considering the backend validated for development use. Image-backed tests and the disposable-file `RawDevice` test do not substitute for testing the operating system's raw-device path semantics, USB controllers, disconnects, or power-loss behavior.

## Path to actual USB use

1. Finish crash-cut and torn-write recovery testing, including failures at every journal/checkpoint/home-write/flush boundary.
2. Add OS-specific, read-only device discovery that reports device identity, removable status, capacity, logical sector size, and mounted volumes; reject ambiguous targets.
3. Implement a destructive formatter that uses that verified device metadata, clearly displays the exact target/capacity, requires deliberate confirmation, and refuses mounted/system devices where detectable.
4. Test format/open/recovery on a dedicated expendable USB device only after disposable-image tests pass. Disconnect/reconnect and verify the on-disk state.
5. Implement persistent file/directory operations and an OS integration/mount adapter. Without these, OpenFS cannot yet store and browse ordinary files as a normal USB drive.
6. Never use the system disk or valuable media for early tests.
