//! Read-only physical disk inventory for the Windows host.
//!
//! This module only asks Windows for device metadata through CIM/WMI. It does not
//! open physical-drive handles, lock volumes, read sectors, or issue disk writes.

use std::error::Error as StdError;

#[derive(Debug, Clone, PartialEq, Eq)]
struct PhysicalDisk {
    index: u32,
    model: String,
    serial_number: String,
    size_bytes: u64,
    interface_type: String,
    pnp_device_id: String,
}

impl PhysicalDisk {
    fn is_usb_transport(&self) -> bool {
        self.interface_type.eq_ignore_ascii_case("USB")
            || self.pnp_device_id.to_ascii_uppercase().contains("USB")
    }
}

fn parse_inventory(output: &str) -> Result<Vec<PhysicalDisk>, String> {
    let mut disks = Vec::new();
    for (line_index, line) in output.lines().enumerate() {
        if line.trim().is_empty() {
            continue;
        }

        let fields: Vec<&str> = line.split('\t').collect();
        if fields.len() != 6 {
            return Err(format!(
                "invalid Windows disk inventory row {}: expected 6 tab-separated fields, got {}",
                line_index + 1,
                fields.len()
            ));
        }

        let index = fields[0].parse::<u32>().map_err(|_| {
            format!("invalid disk index in Windows inventory row {}", line_index + 1)
        })?;
        let size_bytes = fields[3].parse::<u64>().map_err(|_| {
            format!("invalid disk capacity in Windows inventory row {}", line_index + 1)
        })?;
        if size_bytes == 0 {
            return Err(format!(
                "zero-sized physical disk reported in Windows inventory row {}",
                line_index + 1
            ));
        }
        if disks.iter().any(|disk: &PhysicalDisk| disk.index == index) {
            return Err(format!("duplicate physical disk index {index} in Windows inventory"));
        }

        disks.push(PhysicalDisk {
            index,
            model: fields[1].trim().to_owned(),
            serial_number: fields[2].trim().to_owned(),
            size_bytes,
            interface_type: fields[4].trim().to_owned(),
            pnp_device_id: fields[5].trim().to_owned(),
        });
    }

    disks.sort_by_key(|disk| disk.index);
    Ok(disks)
}

#[cfg(windows)]
pub fn list_devices() -> Result<(), Box<dyn StdError>> {
    use std::process::Command;

    // Static PowerShell script: no user-provided text is interpolated or executed.
    // CIM returns one row per Win32_DiskDrive; tabs/newlines in text fields are
    // normalized before crossing the process boundary.
    const SCRIPT: &str = r#"
$ErrorActionPreference = 'Stop'
$disks = @(Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop | Sort-Object -Property Index)
foreach ($disk in $disks) {
    $values = @(
        [string]$disk.Index,
        [string]$disk.Model,
        [string]$disk.SerialNumber,
        [string]$disk.Size,
        [string]$disk.InterfaceType,
        [string]$disk.PNPDeviceID
    )
    $safeValues = @($values | ForEach-Object { [regex]::Replace($_, '[\t\r\n]', ' ') })
    [Console]::Out.WriteLine([string]::Join([char]9, [string[]]$safeValues))
}
"#;

    let output = Command::new("powershell.exe")
        .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", SCRIPT])
        .output()?;
    if !output.status.success() {
        let detail = String::from_utf8_lossy(&output.stderr);
        return Err(format!("Windows CIM disk enumeration failed: {}", detail.trim()).into());
    }

    let stdout = String::from_utf8(output.stdout)?;
    let disks = parse_inventory(&stdout).map_err(|error| -> Box<dyn StdError> { error.into() })?;

    println!("Windows physical disk inventory (metadata only; no disk access or writes).");
    if disks.is_empty() {
        println!("No physical disks were returned by Win32_DiskDrive.");
        return Ok(());
    }

    for disk in disks {
        println!("Disk number: {}", disk.index);
        println!("  Model: {}", nonempty(&disk.model));
        println!("  Serial: {}", nonempty(&disk.serial_number));
        println!("  Capacity: {} bytes", disk.size_bytes);
        println!("  Interface: {}", nonempty(&disk.interface_type));
        println!(
            "  USB transport: {}",
            if disk.is_usb_transport() { "yes" } else { "not identified as USB" }
        );
        println!("  PnP ID: {}", nonempty(&disk.pnp_device_id));
    }
    println!("Disk numbers can change after reconnecting; re-identify model, serial and capacity.");
    println!("This command does not open, lock, read, or modify any physical disk.");
    Ok(())
}

#[cfg(not(windows))]
pub fn list_devices() -> Result<(), Box<dyn StdError>> {
    Err("list-devices currently requires Windows (Win32_DiskDrive via CIM)".into())
}

fn nonempty(value: &str) -> &str {
    if value.is_empty() {
        "(not reported)"
    } else {
        value
    }
}

#[cfg(test)]
mod tests {
    use super::{parse_inventory, PhysicalDisk};

    #[test]
    fn parses_and_sorts_multiple_physical_disks() {
        let disks = parse_inventory(
            "2\tSanDisk 3.2Gen1\t03020915081126092611\t123009761280\tUSB\tUSBSTOR\\\\DISK&VEN_SANDISK\n0\tNVMe Drive\tNVME-SERIAL\t1024209543168\tSCSI\tSCSI\\\\DISK\n",
        )
        .unwrap();

        assert_eq!(disks.len(), 2);
        assert_eq!(disks[0].index, 0);
        assert_eq!(disks[1].index, 2);
        assert_eq!(disks[1].size_bytes, 123_009_761_280);
        assert!(disks[1].is_usb_transport());
        assert!(!disks[0].is_usb_transport());
    }

    #[test]
    fn identifies_usb_from_pnp_id_when_interface_is_not_usb() {
        let disks = parse_inventory(
            "3\tUSB Attached SCSI\tSERIAL\t64000000\tSCSI\tUSB\\\\VID_0781&PID_5581\n",
        )
        .unwrap();
        assert!(disks[0].is_usb_transport());
    }

    #[test]
    fn rejects_malformed_rows_and_invalid_numbers() {
        assert!(parse_inventory("1\ttoo-few-fields").is_err());
        assert!(parse_inventory("x\tModel\tSerial\t1024\tUSB\tUSBSTOR").is_err());
        assert!(parse_inventory("1\tModel\tSerial\tNaN\tUSB\tUSBSTOR").is_err());
        assert!(parse_inventory("1\tModel\tSerial\t0\tUSB\tUSBSTOR").is_err());
    }

    #[test]
    fn rejects_duplicate_disk_indices() {
        let output = "1\tDisk A\tA\t1024\tUSB\tUSBSTOR\n1\tDisk B\tB\t2048\tUSB\tUSBSTOR\n";
        assert!(parse_inventory(output).is_err());
    }

    #[test]
    fn accepts_an_empty_inventory() {
        assert!(parse_inventory("").unwrap().is_empty());
        assert!(parse_inventory("\r\n").unwrap().is_empty());
    }

    #[test]
    fn usb_classification_uses_interface_or_pnp_identity() {
        let via_interface = PhysicalDisk {
            index: 0,
            model: "USB disk".into(),
            serial_number: String::new(),
            size_bytes: 1024,
            interface_type: "usb".into(),
            pnp_device_id: String::new(),
        };
        let via_pnp = PhysicalDisk {
            interface_type: "SCSI".into(),
            pnp_device_id: r"USBSTOR\DISK&VEN_TEST".into(),
            ..via_interface.clone()
        };
        assert!(via_interface.is_usb_transport());
        assert!(via_pnp.is_usb_transport());
    }
}
