# OpenFS physical USB test plan (Windows 11, Windows 10 secondary)

## Safety status

This plan is deliberately staged. The current CLI exposes only read-only physical-device commands:
- `probe-device`: reads the first and last logical blocks.
- `inspect-device`: validates the primary and backup GPT structures and partition-entry checksums.

There is currently no supported CLI command to format or write a physical disk. Do not use the library's lower-level writable `RawDevice` directly against a personal disk. Physical write testing remains blocked until the on-disk format, recovery behavior, Windows disk ownership/locking, and an explicit target-confirmation workflow have been validated.

The earlier test machine inventory identified a SanDisk USB disk as disk number 2, 123,009,761,280 bytes, serial `03020915081126092611`. Disk numbers are not stable: **never reuse disk number 2 without re-identifying the device in the current Windows session**.

## 1. Install and verify the toolchain

Install Rust stable with rustup and the Visual Studio 2022 C++ Build Tools (Desktop development with C++). From PowerShell:

```powershell
rustc --version
cargo --version
rustup show
```

Clone the repository and select the active branch:

```powershell
git clone https://github.com/Likounsee/OpenFS.git
Set-Location .\OpenFS\OpenFS
git switch OpenFS
git pull --ff-only
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo fmt --all -- --check
```

Do not proceed to physical-device inspection if the workspace does not compile and pass its local checks.

## 2. Identify the USB device (read-only inventory)

Plug in only the dedicated test USB drive. Run this command and record the model, serial, size, bus type, and disk number:

```powershell
Get-Disk |
  Sort-Object Number |
  Format-Table Number,FriendlyName,SerialNumber,UniqueId,BusType,Size,PartitionStyle,OperationalStatus,IsBoot,IsSystem,IsReadOnly,IsOffline -AutoSize
```

Cross-check the Windows PnP device identity:

```powershell
Get-CimInstance Win32_DiskDrive |
  Select-Object Index,Model,SerialNumber,Size,InterfaceType,PNPDeviceID |
  Format-List
```

Set the candidate number only after physically matching the model, serial, and capacity printed above:

```powershell
$TargetDisk = <REPLACE_WITH_VERIFIED_DISK_NUMBER>
$Disk = Get-Disk -Number $TargetDisk -ErrorAction Stop
$Disk | Format-List Number,FriendlyName,SerialNumber,UniqueId,BusType,Size,PartitionStyle,IsBoot,IsSystem,IsReadOnly,IsOffline
```

Stop immediately unless all of the following are true:
- `BusType` is `USB`.
- Model, serial number, and capacity match the dedicated test stick.
- `IsBoot` and `IsSystem` are both `False`.
- The disk is not an internal disk and contains no data that must be preserved.

Inspect partitions and volumes without changing them:

```powershell
Get-Partition -DiskNumber $TargetDisk -ErrorAction SilentlyContinue |
  Format-Table DiskNumber,PartitionNumber,DriveLetter,Type,Size,IsBoot,IsSystem -AutoSize
Get-Volume | Format-Table DriveLetter,FileSystemLabel,FileSystem,Size,SizeRemaining -AutoSize
```

Do not run `Clear-Disk`, `Initialize-Disk`, `Format-Volume`, `Set-Disk -IsReadOnly $false`, `diskpart clean`, or any write command as part of this read-only phase.

## 3. Derive and cross-check the geometry

Use the logical sector size and size reported by Windows. Do not assume that every USB device uses 512-byte logical sectors.

```powershell
$Disk = Get-Disk -Number $TargetDisk -ErrorAction Stop
$LogicalSectorSize = [uint64]$Disk.LogicalSectorSize
$DiskBytes = [uint64]$Disk.Size
if ($LogicalSectorSize -lt 512 -or ($LogicalSectorSize -band ($LogicalSectorSize - 1)) -ne 0) {
  throw "Unsupported logical sector size: $LogicalSectorSize"
}
if (($DiskBytes % $LogicalSectorSize) -ne 0) {
  throw "Disk size is not a whole number of logical sectors"
}
$BlockCount = [uint64]($DiskBytes / $LogicalSectorSize)
[pscustomobject]@{
  DiskNumber = $TargetDisk
  Model = $Disk.FriendlyName
  Serial = $Disk.SerialNumber
  LogicalSectorSize = $LogicalSectorSize
  BlockCount = $BlockCount
  CapacityBytes = $DiskBytes
}
```

The geometry is caller-supplied to the current raw-device CLI. Re-check it immediately before each invocation.

## 4. Build and run read-only physical probes

From the workspace directory:

```powershell
cargo build --release -p openfs-tool
$OpenFs = Resolve-Path .\target\release\openfs.exe
$DevicePath = "\\.\PhysicalDrive$TargetDisk"
```

Re-identify the target immediately before probing:

```powershell
$Again = Get-Disk -Number $TargetDisk -ErrorAction Stop
if ($Again.BusType -ne 'USB' -or $Again.SerialNumber -ne $Disk.SerialNumber -or $Again.Size -ne $DiskBytes -or $Again.IsBoot -or $Again.IsSystem) {
  throw "USB identity changed; refusing to probe"
}
```

Run the first/last-block probe (read-only):

```powershell
& $OpenFs probe-device $DevicePath $LogicalSectorSize $BlockCount
if ($LASTEXITCODE -ne 0) { throw "OpenFS read-only probe failed: $LASTEXITCODE" }
```

Run GPT validation (also read-only):

```powershell
& $OpenFs inspect-device $DevicePath $LogicalSectorSize $BlockCount
if ($LASTEXITCODE -ne 0) { throw "OpenFS GPT inspection failed: $LASTEXITCODE" }
```

A GPT error is a diagnostic result, not permission to initialize or repair the disk. Preserve the output and stop. A disk with no partition table may legitimately fail GPT inspection; this does not prove that OpenFS can safely format it.

## 5. Current functional boundary

| Capability | Current evidence | Physical USB status |
|---|---|---|
| Windows USB enumeration | Windows `Get-Disk` / PnP inventory | Host OS responsibility; must be confirmed on the test PC |
| Raw block read | `ReadOnlyRawDevice`, first/last block probe | Implemented in code; physical execution still required |
| GPT primary/backup validation | `inspect-device`, CRC and bounds checks | Implemented in code; physical execution still required |
| Writable raw block I/O | `RawDevice` abstraction | Library code exists; physical writes are not authorized or validated |
| OpenFS volume format on raw USB | No physical formatting CLI | Not implemented as a safe end-to-end workflow |
| Journal recovery on raw USB | Recovery exists for block devices in principle, but physical-device crash semantics are unverified | Not validated |
| USB Mass Storage BOT/UASP driver | Not part of this Windows-hosted utility | Provided by Windows USB/storage stack |

## 6. Requirements before any physical write test

A later write-test milestone must not begin until all of these are implemented and reviewed:

1. A physical-device target confirmation bound to the current disk identity (model, serial, capacity, logical sector size, and PnP identity), not merely a reusable disk number.
2. Explicit rejection of boot/system disks and mounted volumes; exclusive ownership and dismount behavior must be defined and tested.
3. A write command that requires an explicit destructive opt-in and a second confirmation token derived from the inspected target identity.
4. A complete backup/restore path for the target's first and last sectors and partition metadata, plus a recovery plan.
5. Verified journal ordering, flush semantics, torn-write recovery, and behavior after unplug/power loss on the actual USB bridge/controller.
6. A test-only filesystem image or reserved test partition whose boundaries are independently verified before any write.
7. Post-write read-back, metadata audit, file lifecycle tests, checksums, and a documented recovery procedure.

Until these gates pass, use regular image files for all OpenFS format, create, read, modify, delete, journal-recovery, and corruption tests. Physical USB tests in this phase are read-only.

## 7. Evidence to capture

For each physical probe, save:
- The complete `Get-Disk` record and `Win32_DiskDrive` PnP identity.
- Logical sector size, capacity, and computed block count.
- Exact OpenFS command line and exit code.
- Complete probe/GPT output.
- OpenFS commit SHA and Windows version/build.

Never report a physical read or write as successful unless the command was run against the currently re-identified USB device and its output was captured.
