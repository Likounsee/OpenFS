//! Read-only physical disk inventory for the Windows host.
//!
//! Windows Storage cmdlets provide the authoritative bus type (including UASP
//! devices presented through a SCSI interface). CIM supplies the matching PnP ID.
//! This module never opens physical-drive handles or issues disk I/O.

use std::error::Error as StdError;

#[cfg(any(windows, test))]
#[derive(Debug, Clone, PartialEq, Eq)]
struct PhysicalDisk {
    index: u32,
    model: String,
    serial_number: String,
    size_bytes: u64,
    bus_type: String,
    logical_sector_size: u32,
    unique_id: String,
    pnp_device_id: String,
    is_boot: bool,
    is_system: bool,
}

#[cfg(any(windows, test))]
impl PhysicalDisk {
    fn is_usb_transport(&self) -> bool {
        self.bus_type.eq_ignore_ascii_case("USB")
            || self.pnp_device_id.to_ascii_uppercase().contains("USB")
    }
}

#[cfg(any(windows, test))]
fn parse_inventory(output: &str) -> Result<Vec<PhysicalDisk>, String> {
    let mut disks = Vec::new();
    for (line_index, line) in output.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }

        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() != 10 {
            return Err(format!(
                "invalid Windows disk inventory row {}: expected 10 tab-separated fields, got {}",
                line_index + 1,
                fields.len()
            ));
        }

        let index = fields[0].parse::<u32>().map_err(|_| {
            format!(
                "invalid disk index in Windows inventory row {}",
                line_index + 1
            )
        })?;
        let size_bytes = fields[3].parse::<u64>().map_err(|_| {
            format!(
                "invalid disk capacity in Windows inventory row {}",
                line_index + 1
            )
        })?;
        let logical_sector_size = fields[5].parse::<u32>().map_err(|_| {
            format!(
                "invalid logical sector size in Windows inventory row {}",
                line_index + 1
            )
        })?;
        let is_boot = parse_bool(fields[8]).ok_or_else(|| {
            format!(
                "invalid boot-disk flag in Windows inventory row {}",
                line_index + 1
            )
        })?;
        let is_system = parse_bool(fields[9]).ok_or_else(|| {
            format!(
                "invalid system-disk flag in Windows inventory row {}",
                line_index + 1
            )
        })?;

        if size_bytes == 0 {
            return Err(format!(
                "zero-sized physical disk reported in Windows inventory row {}",
                line_index + 1
            ));
        }
        if logical_sector_size == 0 {
            return Err(format!(
                "zero logical sector size reported in Windows inventory row {}",
                line_index + 1
            ));
        }
        if disks.iter().any(|disk: &PhysicalDisk| disk.index == index) {
            return Err(format!(
                "duplicate physical disk index {index} in Windows inventory"
            ));
        }

        disks.push(PhysicalDisk {
            index,
            model: fields[1].trim().to_owned(),
            serial_number: fields[2].trim().to_owned(),
            size_bytes,
            bus_type: fields[4].trim().to_owned(),
            logical_sector_size,
            unique_id: fields[6].trim().to_owned(),
            pnp_device_id: fields[7].trim().to_owned(),
            is_boot,
            is_system,
        });
    }

    disks.sort_by_key(|disk| disk.index);
    Ok(disks)
}

#[cfg(any(windows, test))]
fn parse_bool(value: &str) -> Option<bool> {
    if value.eq_ignore_ascii_case("true") {
        Some(true)
    } else if value.eq_ignore_ascii_case("false") {
        Some(false)
    } else {
        None
    }
}

#[cfg(windows)]
pub fn list_devices() -> Result<(), Box<dyn StdError>> {
    use std::process::Command;

    // Static PowerShell script: no user-provided text is interpolated or executed.
    // Get-Disk supplies BusType/geometry; Win32_DiskDrive supplies the PnP identity.
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$pnpByIndex = @{}
Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop | ForEach-Object {
    $pnpByIndex[[int]$_.Index] = [string]$_.PNPDeviceID
}
$disks = @(Get-Disk -ErrorAction Stop | Sort-Object -Property Number)
foreach ($disk in $disks) {
    $values = @(
        [string]$disk.Number,
        [string]$disk.FriendlyName,
        [string]$disk.SerialNumber,
        [string]$disk.Size,
        [string]$disk.BusType,
        [string]$disk.LogicalSectorSize,
        [string]$disk.UniqueId,
        [string]$pnpByIndex[[int]$disk.Number],
        [string]$disk.IsBoot,
        [string]$disk.IsSystem
    )
    $safeValues = @($values | ForEach-Object { [regex]::Replace($_, '[\t\r\n]', ' ') })
    [Console]::Out.WriteLine([string]::Join([string][char]9, [string[]]$safeValues))
}
"#;

    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
        ])
        .output()?;
    if !output.status.success() {
        let detail = String::from_utf8_lossy(&output.stderr);
        return Err(format!("Windows disk enumeration failed: {}", detail.trim()).into());
    }

    let stdout = String::from_utf8(output.stdout)?;
    let disks = parse_inventory(&stdout)
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error))?;

    println!("Windows physical disk inventory (metadata only; no disk access or writes).");
    if disks.is_empty() {
        println!("No physical disks were returned by Get-Disk.");
        return Ok(());
    }

    for disk in disks {
        println!("Disk number: {}", disk.index);
        println!("  Model: {}", nonempty(&disk.model));
        println!("  Serial: {}", nonempty(&disk.serial_number));
        println!("  Capacity: {} bytes", disk.size_bytes);
        println!("  Bus type: {}", nonempty(&disk.bus_type));
        println!("  Logical sector size: {} bytes", disk.logical_sector_size);
        println!("  Unique ID: {}", nonempty(&disk.unique_id));
        println!(
            "  USB transport: {}",
            if disk.is_usb_transport() { "yes" } else { "no" }
        );
        println!("  Boot disk: {}", disk.is_boot);
        println!("  System disk: {}", disk.is_system);
        println!("  PnP ID: {}", nonempty(&disk.pnp_device_id));
    }
    println!("Disk numbers can change after reconnecting; re-identify model, serial and capacity.");
    println!("This command does not open, lock, read, or modify any physical disk.");
    Ok(())
}

#[cfg(not(windows))]
pub fn list_devices() -> Result<(), Box<dyn StdError>> {
    Err("list-devices currently requires Windows (Get-Disk and Win32_DiskDrive via CIM)".into())
}

#[cfg(any(windows, test))]
fn parse_verified_geometry(output: &str) -> Result<(u64, u32), String> {
    let fields: Vec<&str> = output.trim().split('\t').collect();
    if fields.len() != 2 {
        return Err("Windows returned malformed physical disk geometry".to_owned());
    }
    let size_bytes = fields[0]
        .parse::<u64>()
        .map_err(|_| "Windows returned an invalid physical disk capacity".to_owned())?;
    let logical_sector_size = fields[1]
        .parse::<u32>()
        .map_err(|_| "Windows returned an invalid logical sector size".to_owned())?;
    if size_bytes == 0 || logical_sector_size == 0 {
        return Err("Windows returned zero-sized physical disk geometry".to_owned());
    }
    if !matches!(logical_sector_size, 512 | 1024 | 2048 | 4096) {
        return Err("Windows reported an unsupported logical sector size".to_owned());
    }
    Ok((size_bytes, logical_sector_size))
}

/// Run a static PowerShell disk script with caller values passed only through the environment.
#[cfg(windows)]
fn run_disk_script(
    script: &str,
    index: u32,
    expected_unique_id: &str,
    operation: &str,
) -> Result<String, Box<dyn StdError>> {
    use std::process::Command;

    if expected_unique_id.trim().is_empty() {
        return Err("an exact non-empty disk UniqueId is required".into());
    }
    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            script,
        ])
        .env("OPENFS_DISK_INDEX", index.to_string())
        .env("OPENFS_DISK_UNIQUE_ID", expected_unique_id)
        .output()?;
    if !output.status.success() {
        return Err(format!(
            "{operation} failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    Ok(String::from_utf8(output.stdout)?)
}

/// Validate a selected physical disk, dismount its mounted filesystem volumes, and report geometry.
///
/// The physical disk remains Online. OpenFS dismounts mounted filesystem volumes while preserving
/// their existing drive-letter/directory mount-point configuration. The caller must
/// then acquire an exclusive PhysicalDrive handle; failure to do so aborts the operation.
/// Boot and system disks are always rejected.
#[cfg(windows)]
pub fn prepare_disk_for_raw_access(
    index: u32,
    expected_model: &str,
    expected_serial: &str,
    expected_size: u64,
) -> Result<(u64, u32), Box<dyn StdError>> {
    if expected_model.trim().is_empty() || expected_serial.trim().is_empty() || expected_size == 0 {
        return Err("exact non-empty USB model, serial number and capacity are required".into());
    }

    // Use the composite media identity throughout; some USB bridges expose only
    // the generic UniqueId "USB", which is not a reliable per-device identifier.
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$index = [int]$env:OPENFS_DISK_INDEX

function Get-VerifiedOpenFsDisk {
    $disk = Get-Disk -Number $index -ErrorAction Stop
    $drive = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
        Where-Object { [int]$_.Index -eq $index } |
        Select-Object -First 1
    if ($null -eq $drive) { throw 'Windows could not confirm the physical disk PnP identity' }
    if ([string]$disk.FriendlyName -cne [string]$env:OPENFS_EXPECTED_MODEL) {
        throw 'physical disk model changed; refusing raw access'
    }
    if (([string]$drive.SerialNumber).Trim() -cne [string]$env:OPENFS_EXPECTED_SERIAL) {
        throw 'physical disk serial number changed; refusing raw access'
    }
    if ([uint64]$disk.Size -ne [uint64]$env:OPENFS_EXPECTED_SIZE) {
        throw 'physical disk capacity changed; refusing raw access'
    }
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.IsReadOnly -or $disk.IsOffline) {
        throw 'disk safety state prevents raw access'
    }
    $busType = [string]$disk.BusType
    $pnpId = [string]$drive.PNPDeviceID
    if ($busType -ine 'USB' -and $pnpId -notmatch '(?i)USB') {
        throw 'refusing raw access: target is not positively identified as USB'
    }
    return $disk
}

$disk = Get-VerifiedOpenFsDisk
if ([uint64]$disk.Size -eq 0 -or [uint32]$disk.LogicalSectorSize -eq 0) {
    throw 'Windows reported invalid physical disk geometry'
}

$partitions = @(Get-Partition -DiskNumber $index -ErrorAction Stop)
$volumePlans = @()
foreach ($partition in $partitions) {
    # Volume GUID paths are identifiers, not mount points.
    $mountPoints = @($partition.AccessPaths | Where-Object {
        $_ -and ($_ -notmatch '^\\\?\Volume\{')
    })
    if ($mountPoints.Count -gt 0) {
        # fsutil volume dismount accepts a drive-letter path. Refuse folder-only
        # mounts rather than using mountvol /p, which forcibly closes open handles.
        $drivePath = @($mountPoints | Where-Object { $_ -match '^[A-Za-z]:\\' } | Select-Object -First 1)
        if ($drivePath.Count -eq 0) {
            throw "partition $($partition.PartitionNumber) has a directory-only mount point; remove it or assign a temporary drive letter before raw access"
        }
        $volumePlans += [pscustomobject]@{
            DrivePath = ([string]$drivePath[0]).Substring(0, 2)
        }
    }
}

# Dismount only after validating every mounted partition has a supported path.
# Preserve mount-point configuration and refuse forced dismounts.
foreach ($plan in $volumePlans) {
    & "$env:SystemRoot\System32\fsutil.exe" volume dismount $plan.DrivePath 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "could not safely dismount volume $($plan.DrivePath); close applications using it and retry"
    }
}

# Re-read model, serial, capacity and safety state after dismounting.
$disk = Get-VerifiedOpenFsDisk
[Console]::Out.WriteLine("$([uint64]$disk.Size)" + [char]9 + "$([uint32]$disk.LogicalSectorSize)")
"#;

    use std::process::Command;
    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
        ])
        .env("OPENFS_DISK_INDEX", index.to_string())
        .env("OPENFS_EXPECTED_MODEL", expected_model)
        .env("OPENFS_EXPECTED_SERIAL", expected_serial)
        .env("OPENFS_EXPECTED_SIZE", expected_size.to_string())
        .output()?;
    if !output.status.success() {
        return Err(format!(
            "physical disk preflight failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    parse_verified_geometry(&String::from_utf8(output.stdout)?)
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error).into())
}

/// Verify a physical USB disk using a composite identity, not Windows UniqueId alone.
///
/// Some USB bridges report the generic UniqueId "USB". The model, hardware serial and
/// exact byte capacity are therefore all required and rechecked at every destructive boundary.
#[cfg(windows)]
pub fn verify_usb_identity(
    index: u32,
    expected_model: &str,
    expected_serial: &str,
    expected_size: u64,
) -> Result<(u64, u32), Box<dyn StdError>> {
    if expected_model.trim().is_empty() || expected_serial.trim().is_empty() || expected_size == 0 {
        return Err("exact non-empty USB model, serial number and capacity are required".into());
    }
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$index = [int]$env:OPENFS_DISK_INDEX
$disk = Get-Disk -Number $index -ErrorAction Stop
$drive = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
    Where-Object { [int]$_.Index -eq $index } |
    Select-Object -First 1
if ($null -eq $drive) { throw 'Windows could not confirm the physical disk PnP identity' }
if ([string]$disk.FriendlyName -cne [string]$env:OPENFS_EXPECTED_MODEL) {
    throw 'physical disk model changed; refusing destructive access'
}
if (([string]$drive.SerialNumber).Trim() -cne [string]$env:OPENFS_EXPECTED_SERIAL) {
    throw 'physical disk serial number changed; refusing destructive access'
}
if ([uint64]$disk.Size -ne [uint64]$env:OPENFS_EXPECTED_SIZE) {
    throw 'physical disk capacity changed; refusing destructive access'
}
if ($disk.IsBoot -or $disk.IsSystem -or $disk.IsReadOnly -or $disk.IsOffline) {
    throw 'disk safety state prevents destructive access'
}
$busType = [string]$disk.BusType
$pnpId = [string]$drive.PNPDeviceID
if ($busType -ine 'USB' -and $pnpId -notmatch '(?i)USB') {
    throw 'refusing destructive access: target is not positively identified as USB'
}
[Console]::Out.WriteLine("$([uint64]$disk.Size)" + [char]9 + "$([uint32]$disk.LogicalSectorSize)")
"#;
    use std::process::Command;
    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            SCRIPT,
        ])
        .env("OPENFS_DISK_INDEX", index.to_string())
        .env("OPENFS_EXPECTED_MODEL", expected_model)
        .env("OPENFS_EXPECTED_SERIAL", expected_serial)
        .env("OPENFS_EXPECTED_SIZE", expected_size.to_string())
        .output()?;
    if !output.status.success() {
        return Err(format!(
            "composite USB identity verification failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    parse_verified_geometry(&String::from_utf8(output.stdout)?)
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error).into())
}

/// Revalidate the disk identity and geometry after the exclusive raw handle is open.
///
/// This check intentionally does not enumerate or dismount partitions: the caller already
/// completed that work before opening PhysicalDrive, and Windows may deny new volume opens
/// while the exclusive raw handle is held.
#[cfg(windows)]
pub fn verify_disk_identity_geometry(
    index: u32,
    expected_unique_id: &str,
) -> Result<(u64, u32), Box<dyn StdError>> {
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$disk = Get-Disk -Number ([int]$env:OPENFS_DISK_INDEX) -ErrorAction Stop
if ([string]$disk.UniqueId -cne [string]$env:OPENFS_DISK_UNIQUE_ID) {
    throw 'disk UniqueId changed while opening it; refusing raw access'
}
if ($disk.IsBoot -or $disk.IsSystem -or $disk.IsReadOnly -or $disk.IsOffline) {
    throw 'disk safety state changed while opening it; refusing raw access'
}
if ([uint64]$disk.Size -eq 0 -or [uint32]$disk.LogicalSectorSize -eq 0) {
    throw 'Windows returned invalid physical disk geometry'
}
[Console]::Out.WriteLine("$([uint64]$disk.Size)" + [char]9 + "$([uint32]$disk.LogicalSectorSize)")
"#;
    parse_verified_geometry(&run_disk_script(
        SCRIPT,
        index,
        expected_unique_id,
        "physical disk identity recheck",
    )?)
    .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error).into())
}

#[cfg(all(not(windows), test))]
pub fn prepare_disk_for_raw_access(
    _index: u32,
    _expected_model: &str,
    _expected_serial: &str,
    _expected_size: u64,
) -> Result<(u64, u32), Box<dyn StdError>> {
    Err("raw physical disk access currently requires Windows".into())
}

#[cfg(windows)]
fn nonempty(value: &str) -> &str {
    if value.is_empty() {
        "(not reported)"
    } else {
        value
    }
}

#[cfg(test)]
mod tests {
    use super::parse_inventory;

    fn row(index: u32, bus: &str, pnp: &str, boot: &str, system: &str) -> String {
        format!(
            "{index}\tTest disk\tSERIAL-{index}\t123009761280\t{bus}\t512\tUNIQUE-{index}\t{pnp}\t{boot}\t{system}"
        )
    }

    #[test]
    fn parses_and_sorts_multiple_physical_disks() {
        let output = format!(
            "{}\n{}\n",
            row(2, "USB", r"USBSTOR\DISK&VEN_SANDISK", "False", "False"),
            row(0, "NVMe", r"SCSI\DISK&VEN_NVME", "True", "True")
        );
        let disks = parse_inventory(&output).unwrap();

        assert_eq!(disks.len(), 2);
        assert_eq!(disks[0].index, 0);
        assert!(disks[0].is_boot && disks[0].is_system);
        assert_eq!(disks[1].index, 2);
        assert_eq!(disks[1].size_bytes, 123_009_761_280);
        assert_eq!(disks[1].logical_sector_size, 512);
        assert!(disks[1].is_usb_transport());
        assert!(!disks[0].is_usb_transport());
    }

    #[test]
    fn identifies_uasp_usb_from_pnp_id_even_when_bus_is_scsi() {
        let disks =
            parse_inventory(&row(3, "SCSI", r"USB\VID_0781&PID_5581", "False", "False")).unwrap();
        assert!(disks[0].is_usb_transport());
    }

    #[test]
    fn rejects_malformed_rows_and_invalid_numbers() {
        assert!(parse_inventory("1\ttoo-few-fields").is_err());
        assert!(
            parse_inventory("x\tModel\tSerial\t1024\tUSB\t512\tID\tPNP\tFalse\tFalse").is_err()
        );
        assert!(parse_inventory("1\tModel\tSerial\tNaN\tUSB\t512\tID\tPNP\tFalse\tFalse").is_err());
        assert!(parse_inventory("1\tModel\tSerial\t0\tUSB\t512\tID\tPNP\tFalse\tFalse").is_err());
        assert!(parse_inventory("1\tModel\tSerial\t1024\tUSB\t0\tID\tPNP\tFalse\tFalse").is_err());
    }

    #[test]
    fn rejects_duplicate_disk_indices_and_invalid_safety_flags() {
        let duplicate = format!(
            "{}\n{}\n",
            row(1, "USB", "USBSTOR", "False", "False"),
            row(1, "USB", "USBSTOR", "False", "False")
        );
        assert!(parse_inventory(&duplicate).is_err());
        assert!(parse_inventory(&row(0, "USB", "USBSTOR", "unknown", "False")).is_err());
    }

    #[test]
    fn accepts_an_empty_inventory() {
        assert!(parse_inventory("").unwrap().is_empty());
        assert!(parse_inventory("\r\n").unwrap().is_empty());
    }

    #[test]
    fn usb_classification_uses_bus_type_or_pnp_identity() {
        let via_bus = parse_inventory(&row(0, "USB", "", "False", "False")).unwrap();
        let via_pnp = parse_inventory(&row(1, "SCSI", r"USBSTOR\DISK", "False", "False")).unwrap();
        assert!(via_bus[0].is_usb_transport());
        assert!(via_pnp[0].is_usb_transport());
    }

    #[test]
    fn empty_optional_identity_fields_are_preserved_as_empty() {
        let disks = parse_inventory(&row(0, "USB", "", "False", "False")).unwrap();
        assert_eq!(disks[0].serial_number, "SERIAL-0");
        assert_eq!(disks[0].unique_id, "UNIQUE-0");
        assert_eq!(disks[0].pnp_device_id, "");
    }

    #[test]
    fn safety_metadata_is_available_for_target_review() {
        let disks = parse_inventory(&row(0, "USB", "USBSTOR", "True", "False")).unwrap();
        assert!(disks[0].is_boot);
        assert!(!disks[0].is_system);
    }

    #[test]
    fn boolean_parser_is_case_insensitive_and_strict() {
        assert_eq!(super::parse_bool("TRUE"), Some(true));
        assert_eq!(super::parse_bool("false"), Some(false));
        assert_eq!(super::parse_bool("yes"), None);
    }

    #[test]
    fn verified_disk_geometry_parser_accepts_supported_sector_sizes() {
        assert_eq!(
            super::parse_verified_geometry("64000000000\t512\n").unwrap(),
            (64_000_000_000, 512)
        );
        assert_eq!(
            super::parse_verified_geometry("64000000000\t4096").unwrap(),
            (64_000_000_000, 4096)
        );
    }

    #[test]
    fn verified_disk_geometry_parser_rejects_ambiguous_or_unsupported_values() {
        assert!(super::parse_verified_geometry("").is_err());
        assert!(super::parse_verified_geometry("64000000000").is_err());
        assert!(super::parse_verified_geometry("64000000000\t512\textra").is_err());
        assert!(super::parse_verified_geometry("0\t512").is_err());
        assert!(super::parse_verified_geometry("64000000000\t0").is_err());
        assert!(super::parse_verified_geometry("64000000000\t520").is_err());
        assert!(super::parse_verified_geometry("unknown\t512").is_err());
    }

    #[cfg(not(windows))]
    #[test]
    fn physical_disk_preflight_is_explicitly_unsupported_on_non_windows_hosts() {
        let error = super::prepare_disk_for_raw_access(0, "USB Model", "SERIAL", 64_000_000_000).unwrap_err();
        assert!(error.to_string().contains("currently requires Windows"));
    }
}
