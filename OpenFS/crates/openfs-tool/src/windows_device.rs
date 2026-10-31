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

/// Verify a disk's identity and safety state immediately before opening it read/write.
///
/// The disk must be USB-attached, explicitly offline in Windows, writable, and neither
/// the boot nor system disk. The caller must supply the exact UniqueId printed by
/// `list-devices`; the disk index alone is not a stable identity.
#[cfg(windows)]
pub fn verify_offline_usb_disk(
    index: u32,
    expected_unique_id: &str,
) -> Result<(u64, u32), Box<dyn StdError>> {
    use std::process::Command;

    if expected_unique_id.trim().is_empty() {
        return Err("an exact non-empty disk UniqueId is required".into());
    }

    // Static script: all user-provided values are passed as environment data, never
    // interpolated into PowerShell source.
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$disk = Get-Disk -Number ([int]$env:OPENFS_DISK_INDEX) -ErrorAction Stop
if ([string]$disk.UniqueId -cne [string]$env:OPENFS_DISK_UNIQUE_ID) {
    throw 'disk UniqueId changed; re-run list-devices and verify the target'
}
$pnp = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
    Where-Object { [int]$_.Index -eq [int]$disk.Number } |
    Select-Object -First 1
$isUsb = ([string]$disk.BusType -eq 'USB') -or ([string]$pnp.PNPDeviceID -match '(^|\)USB')
if (-not $isUsb) { throw 'target disk is not identified as USB transport' }
if ($disk.IsBoot -or $disk.IsSystem) { throw 'refusing to open a boot or system disk' }
if (-not $disk.IsOffline) { throw 'target USB disk must first be set Offline in Windows Disk Management' }
if ($disk.IsReadOnly) { throw 'target USB disk is read-only' }
if ([uint64]$disk.Size -eq 0 -or [uint32]$disk.LogicalSectorSize -eq 0) {
    throw 'Windows reported invalid disk geometry'
}
[Console]::Out.WriteLine("$([uint64]$disk.Size)" + [char]9 + "$([uint32]$disk.LogicalSectorSize)")
"#;

    let output = Command::new("powershell.exe")
        .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .env("OPENFS_DISK_INDEX", index.to_string())
        .env("OPENFS_DISK_UNIQUE_ID", expected_unique_id)
        .output()?;
    if !output.status.success() {
        let detail = String::from_utf8_lossy(&output.stderr);
        return Err(format!("USB disk safety check failed: {}", detail.trim()).into());
    }
    let stdout = String::from_utf8(output.stdout)?;
    let fields: Vec<&str> = stdout.trim().split('\t').collect();
    if fields.len() != 2 {
        return Err("Windows returned malformed USB disk geometry".into());
    }
    let size_bytes = fields[0].parse::<u64>()?;
    let logical_sector_size = fields[1].parse::<u32>()?;
    if size_bytes == 0 || logical_sector_size == 0 {
        return Err("Windows returned zero-sized USB disk geometry".into());
    }
    Ok((size_bytes, logical_sector_size))
}

#[cfg(not(windows))]
pub fn verify_offline_usb_disk(
    _index: u32,
    _expected_unique_id: &str,
) -> Result<(u64, u32), Box<dyn StdError>> {
    Err("raw USB disk mounting currently requires Windows".into())
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
}
