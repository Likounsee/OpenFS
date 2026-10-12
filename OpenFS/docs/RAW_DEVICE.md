# Raw block-device backend

**Status: low-level development capability only. Not a safe end-user formatter or mount path.**

`openfs_block::RawDevice` opens an existing path read/write without create or truncate flags. It implements the same bounded block-device interface as the image backend. The caller must provide the logical block size and block count. For regular files, the length must exactly match the declared geometry; for device nodes, geometry is not discovered automatically.

## Safety constraints

- Opening the device does not write to it. Subsequent `write_blocks` calls write directly to the selected path and may overwrite partition tables, filesystem metadata, or user data.
- Do not use this backend on a disk containing data you need. Use a disposable image or dedicated test device.
- The API does not identify the selected physical device, query hardware geometry, acquire an exclusive OS-level lock, coordinate with mounted filesystems, or protect against another process writing concurrently.
- `flush` requests host-side synchronization through `File::sync_all`; it is not a proof that every USB bridge, controller, or device firmware has honored power-loss guarantees.
- No OpenFS formatter CLI, device-selection confirmation, mounted filesystem, or integrated journal recovery is provided by this backend.

## Current validation

Unit tests cover round-trip persistence through reopen, exact regular-file geometry, non-destructive rejection of a mismatched geometry, misaligned buffers, and out-of-range block accesses. CI must pass on Linux and Windows before considering this backend validated for development use.

## Path to real-device testing

1. Finish integrating a versioned journal region into the volume layout and reserve its blocks in the allocation bitmap.
2. Ensure volume format/open paths validate journal geometry and recover before allowing allocation or writes.
3. Add a formatter that requires an explicit target, displays the resolved device identity and capacity, and requires deliberate destructive confirmation.
4. Test first on a disposable image with injected failures and process termination at every write/flush boundary.
5. Test on a dedicated expendable USB device, then disconnect/reconnect and verify data and recovery. Never use the system disk or valuable media.
