<p align="center">
  <img src="OpenFS/docs/assets/openfs-logo.png" alt="OpenFS logo" width="600">
</p>


<p align="center">
  <strong>A filesystem built in Rust, from its on-disk format to persistent file operations.</strong>
  <br>
  A long-term project aiming for a reliable, secure and performant filesystem for Windows, Linux and, eventually, ArchiaOS.
</p>

<p align="center">
  <a href="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml"><img src="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml/badge.svg?branch=OpenFS" alt="OpenFS CI"></a>
  <img src="https://img.shields.io/badge/language-Rust-orange.svg" alt="Rust">
  <img src="https://img.shields.io/badge/status-experimental-yellow.svg" alt="Experimental">
</p>

> **Status: experimental — not production-ready.** OpenFS has a persistent on-disk format, journal/recovery code, namespace and file I/O, plus a Windows + Dokany adapter for image files and an initial raw-USB-disk path. The USB path requires the disk to be explicitly offline and checks its exact Windows UniqueId, USB transport, geometry, and boot/system status before opening it. Formatting erases the entire selected USB disk. This is an early integration and must only be tested with disposable hardware and data; it is not a replacement for NTFS.

## Project goals

OpenFS is intended to become an independent filesystem rather than a wrapper around NTFS or another existing format. The long-term goals are to:

- define and maintain a documented, versioned on-disk format;
- store directories, metadata, extents and file contents persistently;
- protect filesystem integrity through strict validation, journaling and crash recovery;
- provide safe, predictable file operations and useful integrity-checking tools;
- build a safe path from image-based development to carefully validated device support;
- integrate with Windows and Linux, and eventually ArchiaOS, through dedicated OS adapters.

These are **goals, not claims that every capability is implemented today**. See the [roadmap](OpenFS/docs/ROADMAP.md) for phase estimates and acceptance gaps.

## What works today

The workspace already contains:

- **On-disk format primitives:** checked layout geometry, superblock validation, checksummed records, metadata and directory-entry codecs.
- **Block I/O:** bounded reads and writes, explicit flushes, image-file handling and deterministic I/O failure injection.
- **Allocation and verification:** bitmap allocation, extent maps, persistent inventory reconstruction and cross-layer ownership checks.
- **Journal components:** journal/checkpoint encoding and recovery paths for supported operations.
- **Persistent namespace and file APIs:** directory and file operations, streaming file import, range I/O, truncation, rename and removal through the development tool.
- **Image CLI and initial Windows mounts:** create mount-ready images, inspect/check/audit/recover them, and mount images or explicitly confirmed offline USB disks as Windows drives through Dokany. Raw physical-disk formatting is destructive and gated by exact identity and USB/offline checks.

The `truncate` command shrinks files or grows them sparsely (newly exposed bytes read as zeroes). Windows Dokany replacement of an existing regular file now stages the namespace swap as one copy-on-write publication and reclaims the retired destination extents afterward; replacing a destination with open handles is refused. This does **not** mean all multi-step operations are atomic under every crash scenario. Per-handle Windows read/write/delete sharing checks and operation-specific access checks are now enforced by the Dokany adapter; special-rights and ACL auditing, broader transaction coverage, recovery guarantees, device safety and OS mounting remain active development areas.

## Repository layout

```text
.
├── README.md
├── .github/workflows/ci.yml
└── OpenFS/
    ├── Cargo.toml                 # Rust workspace
    ├── crates/
    │   ├── openfs-core/           # Shared errors and checked ranges
    │   ├── openfs-block/          # Bounded block I/O and image backend
    │   ├── openfs-format/         # Format, allocator, journal, namespace and file APIs
    │   └── openfs-tool/           # Image/raw-USB CLI + Windows/Dokany mount
    └── docs/
        ├── assets/openfs-logo.svg
        ├── ARCHITECTURE.md
        ├── FORMAT_SPEC.md
        ├── SECURITY_MODEL.md
        ├── COMPATIBILITY.md
        ├── DEVELOPMENT.md
        └── ROADMAP.md
```

## Quick start

### Requirements

- A stable Rust toolchain (the workspace declares Rust 1.75 as its minimum version).
- Git.

### Build and validate

Run these commands from the Rust workspace directory:

```sh
cd OpenFS
cargo build --workspace
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

GitHub Actions is configured to run formatting, workspace tests and Clippy on Linux and Windows. Check the [CI runs for the `OpenFS` branch](https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml?query=branch%3AOpenFS) before treating a change as validated.

### Create a development image

From `OpenFS/`:

```sh
cargo run -p openfs-tool -- create-image ./openfs.img 1024
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096
```

The example creates a 1 GiB image. OpenFS supports block sizes of 4096, 8192, 16384, 32768 and 65536 bytes where accepted by the relevant command.

### Format and mount a real USB disk (destructive, experimental)

This path writes directly to the **entire physical USB disk**, not to a partition or image file. It destroys its partition table and every existing partition. Use a spare USB stick with nothing important on it. Run PowerShell as Administrator.

1. Build the CLI and identify the USB disk:

```powershell
cd .\OpenFS
cargo build --release -p openfs-tool
.\target\release\openfs.exe list-devices
Get-Disk | Format-Table Number, FriendlyName, SerialNumber, UniqueId, BusType, Size, IsOffline, IsBoot, IsSystem
```

2. Copy the exact `UniqueId` and disk number. Never rely on the disk number alone. The helper refuses non-USB, read-only, boot and system disks.

3. **Destructive format:** from the repository root, run the following with the real values shown on your PC. It erases the whole selected disk:

```powershell
.\OpenFS\scripts\mount-openfs-usb.ps1 -Action Format -OpenFsExe .\OpenFS\target\release\openfs.exe -DiskNumber 2 -UniqueId "EXACT-UNIQUE-ID-FROM-WINDOWS" -MountPoint O: -ConfirmEraseAllData ERASE-ALL-DATA
```

The helper verifies the exact identity and takes the disk offline before calling `format-usb`. The literal `ERASE-ALL-DATA` confirmation is mandatory. Formatting initializes OpenFS's persistent root directory.

4. **Mount:** in the first elevated PowerShell window, run:

```powershell
.\OpenFS\scripts\mount-openfs-usb.ps1 -Action Mount -OpenFsExe .\OpenFS\target\release\openfs.exe -DiskNumber 2 -UniqueId "EXACT-UNIQUE-ID-FROM-WINDOWS" -MountPoint O:
```

Keep this window open. The drive `O:\\` should appear in Explorer; use only disposable test files.

5. **Unmount:** in a second elevated PowerShell window, run:

```powershell
.\OpenFS\scripts\mount-openfs-usb.ps1 -Action Unmount -OpenFsExe .\OpenFS\target\release\openfs.exe -MountPoint O:
```

Wait for the mount window to finish its final flush and exit before disconnecting the stick. The mount helper restores the disk's previous online/offline state when the mount process ends.

**Important limitations:** this raw USB route is implemented but has not yet been validated against real USB controller/driver combinations or power-loss scenarios. Do not use it for important data. If the disk index or UniqueId changes, stop and rerun the inventory; do not guess. See [USB testing and operating guide](OpenFS/docs/USB_TESTING.md).

### Mount the image as a Windows drive (experimental)

This currently requires Windows and an installed, working [Dokany](https://dokany.com/) driver/runtime. Build the workspace on Windows, then run the following from the repository root in PowerShell:

```powershell
cd .\OpenFS
cargo build --workspace
cargo run -p openfs-tool -- create-image .\openfs.img 1024
cargo run -p openfs-tool -- mount .\openfs.img O: 4096
```

The `create-image` command formats the image **and initializes its persistent root directory**. Do not run `init-namespace` on a newly created image. The mount command stays active while the drive is mounted. Open `O:\` in Explorer and create, copy, edit or delete test files. In a second terminal, unmount it:

```powershell
cargo run -p openfs-tool -- unmount O:
```

After the mount process exits, the image can be mounted again; its files should remain. Keep a backup and use disposable test data only. The adapter also has an initial raw USB-disk path; Linux mounting and Windows share-access conflicts are now checked per handle, but per-operation access enforcement and production validation remain incomplete.
### Create a namespace and work with a file

```sh
cargo run -p openfs-tool -- init-namespace ./openfs.img 4096
cargo run -p openfs-tool -- mkdir ./openfs.img 4096 /docs
cargo run -p openfs-tool -- touch ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- import ./openfs.img 4096 ./README.md /docs/hello.txt
cargo run -p openfs-tool -- cat ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- export ./openfs.img 4096 /docs/hello.txt ./hello-export.txt
cargo run -p openfs-tool -- tree ./openfs.img 4096 /
cargo run -p openfs-tool -- stat ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- truncate ./openfs.img 4096 /docs/hello.txt 16384
```

### Search namespace paths

Use `find` to search a subtree by a case-sensitive substring. The traversal is iterative, so deeply nested directories do not recurse on the native stack:

```sh
cargo run -p openfs-tool -- find ./openfs.img 4096 /docs note
```

Each match reports its type, logical size, and full OpenFS path. The root path and search substring are both required.

### Useful diagnostics

```sh
# Inspect without writing to the image
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096

# Audit persistent namespace and allocation ownership
cargo run -p openfs-tool -- audit-image ./openfs.img 4096

# Open the writable recovery path
cargo run -p openfs-tool -- recover-image ./openfs.img 4096

# Display directory contents and free-space information
cargo run -p openfs-tool -- ls ./openfs.img 4096 /
cargo run -p openfs-tool -- space ./openfs.img 4096
```

**Important:** `check-image` is the non-mutating first check and does not replay pending journal records. `audit-image` opens the writable volume path and may replay committed transactions before checking ownership. `recover-image` deliberately uses the writable recovery path. Back up an image before running commands that may mutate it, especially if its state is uncertain. An audit reports inconsistencies; it is not an automatic repair tool.

For additional examples and exact CLI behavior, see the [development guide](OpenFS/docs/DEVELOPMENT.md).

## Safety and compatibility

- Treat every byte read from an image as untrusted input.
- Do not assume a successful image test proves physical-device or power-loss durability.
- Do not use OpenFS with important data.
- Windows OS integration is experimental: Dokany mounts images or whole offline USB disks. The USB format command erases the selected disk and is not production-safe. Linux OS mounting is not implemented yet.
- Exclusive file locks help coordinate cooperating OpenFS processes, but external programs may ignore advisory locks.
- OpenFS defines its own format. It does not read or write NTFS volumes, and historical OpenFS formats are not implicitly compatible.

See the [security model](OpenFS/docs/SECURITY_MODEL.md), [format specification](OpenFS/docs/FORMAT_SPEC.md), and [compatibility notes](OpenFS/docs/COMPATIBILITY.md) for details.

## Development principles

Contributions and changes should follow the project's engineering rules:

1. Keep one canonical volume implementation and preserve explicit format-version checks.
2. Validate disk data before trusting it; use checked arithmetic and propagate I/O errors.
3. Never free a block until ownership checks prove it is no longer referenced.
4. Specify commit points, recovery behavior and crash cut-points for persistent mutations.
5. Test malformed data, aliasing, allocation failures, I/O failures and reopen/recovery behavior.
6. Keep Linux and Windows CI green; report tests as passed only when they actually pass.
7. Keep raw-device and OS integration work behind explicit safety and recovery gates.

Read the [architecture guide](OpenFS/docs/ARCHITECTURE.md) and [development guide](OpenFS/docs/DEVELOPMENT.md) before making changes.

## Roadmap

Progress is tracked in [`docs/ROADMAP.md`](OpenFS/docs/ROADMAP.md) using the P0–P3 engineering phases. The current documented overall estimate is **73%**, an engineering estimate rather than a measure of production readiness. The largest remaining work is around general transaction/recovery guarantees, Windows mount lifecycle and handle semantics, Linux mounting, complete durable namespace/file semantics, and safe physical-device support. The Windows adapter must be validated on a real Windows + Dokany machine before being treated as usable for anything beyond disposable test data.

## License

The workspace declares **MIT OR Apache-2.0**. See the crate manifests and repository license files for the project's current licensing details.
