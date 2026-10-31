//! Safe-first host utility for OpenFS development images.
//!
//! This tool deliberately has no raw-device formatting command. Physical-device
//! support must wait for a journal-integrated on-disk format and stronger recovery
//! guarantees; image creation never truncates or overwrites an existing path.

use std::collections::HashSet;
use std::env;
use std::error::Error as StdError;
use std::fs::{self, OpenOptions};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use openfs_block::{BlockDevice, ImageDevice, ReadOnlyRawDevice};
use openfs_format::{check_volume, format_volume, open_volume, ObjectId, ObjectKind, VolumeSession};

mod windows_device;
#[cfg(windows)]
mod windows_mount;

const BLOCK_SIZE: u32 = 4096;
const BYTES_PER_MIB: u64 = 1024 * 1024;
const MAX_IMAGE_SIZE_MIB: u64 = 1024 * 1024;

fn main() {
    if let Err(error) = run() {
        eprintln!("openfs: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn StdError>> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        Some("mount") => {
            let image = args.next().ok_or("missing image path")?;
            let mount_point = args.next().ok_or("missing mount point (for example X:)")?;
            let block_size = parse_block_size(args.next())?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            #[cfg(windows)]
            {
                windows_mount::mount_image(Path::new(&image), &mount_point, block_size)
            }
            #[cfg(not(windows))]
            {
                let _ = (image, mount_point, block_size);
                Err("mount is currently supported only on Windows through Dokany".into())
            }
        }
        Some("unmount") => {
            let mount_point = args.next().ok_or("missing mount point (for example X:)")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            #[cfg(windows)]
            {
                windows_mount::unmount_drive(&mount_point)
            }
            #[cfg(not(windows))]
            {
                let _ = mount_point;
                Err("unmount is currently supported only on Windows through Dokany".into())
            }
        }
        Some("list-devices") => {
            if args.next().is_some() {
                return Err("list-devices does not accept arguments".into());
            }
            windows_device::list_devices()
        }
        Some("probe-device") => {
            let path = args.next().ok_or("missing device path")?;
            let block_size = parse_probe_block_size(args.next())?;
            let block_count = args.next().ok_or("missing block count")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            probe_read_only_device(Path::new(&path), block_size, block_count)
        }
        Some("inspect-device") => {
            let path = args.next().ok_or("missing device path")?;
            let block_size = parse_probe_block_size(args.next())?;
            let block_count = args.next().ok_or("missing block count")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            inspect_raw_gpt(Path::new(&path), block_size, block_count)
        }
        Some("diagnose-device") => {
            let path = args.next().ok_or("missing device path")?;
            let block_size = parse_probe_block_size(args.next())?;
            let block_count = args.next().ok_or("missing block count")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            diagnose_raw_gpt(Path::new(&path), block_size, block_count)
        }
        Some("inspect-device-json") => {
            let path = args.next().ok_or("missing device path")?;
            let block_size = parse_probe_block_size(args.next())?;
            let block_count = args.next().ok_or("missing block count")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            inspect_raw_gpt_json(Path::new(&path), block_size, block_count)
        }
        Some("scan-device") => {
            let path = args.next().ok_or("missing device path")?;
            let block_size = parse_probe_block_size(args.next())?;
            let block_count = args.next().ok_or("missing block count")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            let (checksum, bytes) =
                scan_read_only_device(Path::new(&path), block_size, block_count)?;
            println!("Read-only full-device scan: OK");
            println!("Bytes scanned: {bytes}");
            println!("CRC32/IEEE: {checksum:08x}");
            println!("No writes were performed.");
            Ok(())
        }
        Some("create-image") => {
            let path = args.next().ok_or("missing image path")?;
            let size_mib = args
                .next()
                .ok_or("missing image size in MiB")?
                .parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            create_image(Path::new(&path), size_mib)
        }
        Some("inspect-image") => {
            let path = args.next().ok_or("missing image path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            inspect_image(Path::new(&path))
        }
        Some("check-image") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(
                // Reject unsupported sizes before opening or mutating an image.
                args.next(),
            )?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            check_image(Path::new(&path), block_size)
        }
        Some("audit-image") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            audit_image(Path::new(&path), block_size)
        }
        Some("recover-image") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(
                // Reject unsupported sizes before opening or mutating an image.
                args.next(),
            )?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            recover_image(Path::new(&path), block_size)
        }
        Some("init-namespace") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(
                // Reject unsupported sizes before opening or mutating an image.
                args.next(),
            )?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            initialize_namespace_image(Path::new(&path), block_size)
        }
        Some(command @ ("mkdir" | "touch")) => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            if command == "mkdir" {
                create_namespace_entry(Path::new(&path), block_size, &target, true)
            } else {
                touch_namespace_file(Path::new(&path), block_size, &target)
            }
        }
        Some("mkdir-p") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            create_namespace_directories(Path::new(&path), block_size, &target)
        }
        Some("ls") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().unwrap_or_else(|| "/".to_owned());
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            list_namespace(Path::new(&path), block_size, &target)
        }
        Some("tree") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().unwrap_or_else(|| "/".to_owned());
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            tree_namespace(Path::new(&path), block_size, &target)
        }
        Some("find") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let root = args.next().ok_or("missing root namespace path")?;
            let needle = args.next().ok_or("missing search text")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            find_namespace_entries(Path::new(&path), block_size, &root, &needle)
        }
        Some("du") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().unwrap_or_else(|| "/".to_owned());
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            report_namespace_usage(Path::new(&path), block_size, &target)
        }
        Some("space") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            report_space(Path::new(&path), block_size)
        }
        Some(command @ ("cat" | "checksum" | "verify-file")) => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let expected_crc32c = args.next();
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            let expected_crc32c = expected_crc32c
                .map(|value| parse_crc32c(&value))
                .transpose()?;
            run_checksum_argument_policy(command, expected_crc32c)?;
            read_or_checksum_namespace_file(
                command,
                Path::new(&path),
                block_size,
                &target,
                expected_crc32c,
            )
        }
        Some("read-range") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let offset = args.next().ok_or("missing byte offset")?.parse::<u64>()?;
            let length = args.next().ok_or("missing byte length")?.parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            read_namespace_file_range(Path::new(&path), block_size, &target, offset, length)
        }
        Some("checksum-range") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let offset = args.next().ok_or("missing byte offset")?.parse::<u64>()?;
            let length = args.next().ok_or("missing byte length")?.parse::<u64>()?;
            let expected_crc32c = args.next().map(|value| parse_crc32c(&value)).transpose()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            checksum_namespace_file_range(
                Path::new(&path),
                block_size,
                &target,
                offset,
                length,
                expected_crc32c,
            )
        }
        Some("copy") => {
            let image = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let source = args.next().ok_or("missing source file path")?;
            let destination = args.next().ok_or("missing destination file path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            copy_namespace_file(Path::new(&image), block_size, &source, &destination)
        }
        Some("export") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace file path")?;
            let output = args.next().ok_or("missing host output path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            export_namespace_file(Path::new(&path), block_size, &target, Path::new(&output))
        }
        Some("import") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let input = args.next().ok_or("missing host input path")?;
            let target = args.next().ok_or("missing namespace file path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            import_host_file(Path::new(&path), block_size, Path::new(&input), &target)
        }
        Some("stat") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            stat_namespace_entry(Path::new(&path), block_size, &target)
        }
        Some("write") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let data = args.collect::<Vec<_>>().join(" ");
            write_namespace_file(Path::new(&path), block_size, &target, data.as_bytes())
        }
        Some("write-range") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let offset = args.next().ok_or("missing byte offset")?.parse::<u64>()?;
            let data = args.collect::<Vec<_>>().join(" ");
            write_namespace_file_range(
                Path::new(&path),
                block_size,
                &target,
                offset,
                data.as_bytes(),
            )
        }
        Some("write-stdin") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            write_namespace_file_from_stdin(Path::new(&path), block_size, &target)
        }
        Some("append") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let data = args.collect::<Vec<_>>().join(" ");
            append_namespace_file(Path::new(&path), block_size, &target, data.as_bytes())
        }
        Some("truncate") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            let new_size = args
                .next()
                .ok_or("missing new file size in bytes")?
                .parse::<u64>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            truncate_namespace_file(Path::new(&path), block_size, &target, new_size)
        }
        Some("rm") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            remove_namespace_entry(Path::new(&path), block_size, &target)
        }
        Some("mv") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let old_path = args.next().ok_or("missing source path")?;
            let new_path = args.next().ok_or("missing destination path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            move_namespace_entry(Path::new(&path), block_size, &old_path, &new_path)
        }
        Some("help" | "--help" | "-h") => {
            print_usage();
            Ok(())
        }
        _ => {
            print_usage();
            Err("invalid command".into())
        }
    }
}

fn create_image(path: &Path, size_mib: u64) -> Result<(), Box<dyn StdError>> {
    if size_mib == 0 || size_mib > MAX_IMAGE_SIZE_MIB {
        return Err("image size must be between 1 MiB and 1 TiB".into());
    }
    let byte_len = size_mib
        .checked_mul(BYTES_PER_MIB)
        .ok_or("image size overflows the supported range")?;
    let block_count = byte_len / u64::from(BLOCK_SIZE);

    // ImageDevice::create uses create_new: an existing path is never truncated.
    let mut device = ImageDevice::create(path, BLOCK_SIZE, block_count)?;
    let uuid = development_uuid();

    // A freshly formatted volume must already have a persistent root namespace
    // before it can be opened by VolumeSession or mounted through Dokany.
    let initialization = (|| -> openfs_core::Result<()> {
        format_volume(&mut device, uuid, 0)?;
        let mut volume = open_volume(&mut device)?;
        volume.initialize_persistent_namespace(&mut device, ObjectId::new(1)?)?;
        device.flush()
    })();
    if let Err(error) = initialization {
        drop(device);
        let _ = fs::remove_file(path);
        return Err(error.into());
    }

    println!("OpenFS development image created and initialized.");
    println!("Path: {}", path.display());
    println!("Size: {size_mib} MiB ({block_count} blocks × {BLOCK_SIZE} bytes)");
    println!("UUID: {}", format_uuid(uuid));
    println!("Format: OpenFS journaled filesystem (versioned journal + persistent root namespace)");
    println!("The image is ready to mount; this command created a regular image file, not a physical USB volume.");
    Ok(())
}

fn inspect_image(path: &Path) -> Result<(), Box<dyn StdError>> {
    // Keep inspection strictly read-only. open_volume() replays the journal and may
    // write recovered home blocks/checkpoints, which is inappropriate for an inspector.
    let mut device = ImageDevice::open(path, BLOCK_SIZE)?;
    let report = check_volume(&mut device)?;
    println!("OpenFS volume inspected (read-only; no recovery performed).");
    println!("Path: {}", path.display());
    println!("Block size: {} bytes", report.block_size);
    println!("Total blocks: {}", report.total_blocks);
    println!(
        "Bitmap: blocks {}..{}",
        report.bitmap_start,
        report.bitmap_start + report.bitmap_blocks
    );
    println!(
        "Data region: blocks {}..{}",
        report.data_start, report.backup_superblock
    );
    println!("Data blocks: {}", report.data_blocks);
    println!("Allocated data blocks: {}", report.allocated_data_blocks);
    println!("Free data blocks: {}", report.free_data_blocks);
    println!("UUID: {}", format_uuid(report.uuid));
    println!("Format: OpenFS journaled filesystem");
    println!(
        "Pending journal records: {}",
        report.pending_journal_records
    );
    println!(
        "Recoverable committed transactions: {}",
        report.recoverable_transactions
    );
    println!(
        "Namespace inventory descriptor present: {}",
        report.persistent_inventory_present
    );
    Ok(())
}

/// Reject audits that would need to open a volume with pending journal records.
///
/// `open_volume` is a recovery-capable writable path. Keep the audit from silently
/// replaying a pending transaction: users must invoke `recover-image` explicitly.
fn ensure_audit_journal_clean(
    pending_journal_records: u64,
    recoverable_transactions: u64,
) -> Result<(), Box<dyn StdError>> {
    if pending_journal_records > 0 || recoverable_transactions > 0 {
        return Err(
            "audit-image refuses to open a volume with pending journal records; back up the image and run recover-image explicitly"
                .into(),
        );
    }
    Ok(())
}

/// Verify persistent namespace structure and reconcile bitmap allocation ownership.
///
/// A read-only journal preflight runs before the recovery-capable volume open. If the
/// journal is not clean, this command refuses to continue; use check-image for read-only
/// inspection and recover-image when journal replay is explicitly intended.
fn audit_image(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let preflight = check_volume(&mut device)?;
    ensure_audit_journal_clean(
        preflight.pending_journal_records,
        preflight.recoverable_transactions,
    )?;
    let volume = open_volume(&mut device)?;
    let Some(report) = volume.verify_discovered_persistent_inventory(&mut device)? else {
        return Err("no discoverable persistent namespace inventory".into());
    };

    println!("OpenFS persistent inventory audit completed.");
    println!("Path: {}", path.display());
    println!("Root object ID: {}", report.namespace.root_id().0);
    println!("Metadata records: {}", report.metadata_record_count);
    println!("Directory entries: {}", report.directory_entry_count);
    println!("File extent maps: {}", report.extent_maps.len());
    println!("Owned map groups checked: {}", report.owner_map_count);
    println!(
        "Orphaned allocated blocks: {}",
        report.orphaned_block_count()
    );
    println!(
        "References to free blocks: {}",
        report.dangling_reference_count()
    );
    if !report.allocation.orphaned_allocations.is_empty() {
        println!(
            "Sample orphaned block numbers: {:?}",
            report.allocation.orphaned_allocations
        );
    }
    if report.allocation.orphaned_allocations_truncated {
        println!("Additional orphaned block numbers were omitted from the diagnostic sample.");
    }
    if !report.allocation.dangling_references.is_empty() {
        println!(
            "Sample dangling-reference block numbers: {:?}",
            report.allocation.dangling_references
        );
    }
    if report.allocation.dangling_references_truncated {
        println!(
            "Additional dangling-reference block numbers were omitted from the diagnostic sample."
        );
    }
    println!(
        "Allocation ownership consistent: {}",
        report.is_consistent()
    );

    if !report.is_consistent() {
        return Err("persistent inventory has allocation ownership anomalies; preserve the image before repair".into());
    }
    Ok(())
}

/// Read-only consistency check. Unlike opening a writable volume, this never replays
/// pending journal records and never changes the image.
fn check_image(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    if ![4096, 8192, 16_384, 32_768, 65_536].contains(&block_size) {
        return Err("unsupported block size; choose 4096, 8192, 16384, 32768, or 65536".into());
    }
    let mut device = ImageDevice::open(path, block_size)?;
    let report = check_volume(&mut device)?;
    println!("OpenFS volume check completed (read-only; no recovery performed).");
    println!("Path: {}", path.display());
    println!("Block size: {} bytes", report.block_size);
    println!("Total blocks: {}", report.total_blocks);
    println!(
        "Bitmap: blocks {}..{}",
        report.bitmap_start,
        report.bitmap_start + report.bitmap_blocks
    );
    println!(
        "Data region: blocks {}..{}",
        report.data_start, report.backup_superblock
    );
    println!("Data blocks: {}", report.data_blocks);
    println!("Allocated data blocks: {}", report.allocated_data_blocks);
    println!("Free data blocks: {}", report.free_data_blocks);
    println!("UUID: {}", format_uuid(report.uuid));
    println!("Journal generation: {}", report.journal_generation);
    println!(
        "Journal checkpoint sequence: {}",
        report.journal_checkpoint_sequence
    );
    println!(
        "Pending journal records: {}",
        report.pending_journal_records
    );
    println!(
        "Recoverable committed transactions: {}",
        report.recoverable_transactions
    );
    if report.pending_journal_records > 0 {
        println!(
            "WARNING: the journal contains {} record(s) beyond its checkpoint.",
            report.pending_journal_records
        );
        println!(
            "Because this check is read-only, it did not replay or discard any journal records."
        );
    }
    if report.recoverable_transactions > 0 {
        println!(
            "WARNING: {} committed journal transaction(s) may need recovery.",
            report.recoverable_transactions
        );
        println!("Open the volume through the recovery path before normal use.");
    }
    println!("The check validates superblocks, geometry, bitmap invariants, journal framing, and the persistent inventory descriptor.");
    println!("A present descriptor is not a substitute for full namespace and file-data ownership validation.");
    Ok(())
}

fn parse_block_size(value: Option<String>) -> Result<u32, Box<dyn StdError>> {
    let block_size = value
        .ok_or("missing block size (4096, 8192, 16384, 32768, or 65536)")?
        .parse::<u32>()?;
    if ![4096, 8192, 16_384, 32_768, 65_536].contains(&block_size) {
        return Err("unsupported block size; choose 4096, 8192, 16384, 32768, or 65536".into());
    }
    Ok(block_size)
}

fn split_parent_path(path: &str) -> Result<(&str, &str), Box<dyn StdError>> {
    let trimmed = path.trim_end_matches('/');
    if trimmed.is_empty() {
        return Err("the namespace root cannot be used as a new entry name".into());
    }
    let (parent, name) = match trimmed.rsplit_once('/') {
        Some(("", name)) => ("/", name),
        Some((parent, name)) => (parent, name),
        None => ("/", trimmed),
    };
    if name.is_empty() || name == "." || name == ".." {
        return Err("invalid final path component".into());
    }
    Ok((parent, name))
}

fn create_namespace_entry(
    path: &Path,
    block_size: u32,
    target: &str,
    directory: bool,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let (parent_path, name) = split_parent_path(target)?;
    let parent_id = namespace.resolve_path(parent_path)?;
    let next_id = namespace
        .objects()
        .map(|record| record.id.0)
        .max()
        .unwrap_or(0)
        .checked_add(1)
        .ok_or("object ID space exhausted")?;
    let object_id = ObjectId::new(next_id)?;
    if directory {
        volume.create_persistent_directory(&mut device, parent_id, object_id, name)?;
    } else {
        volume.create_persistent_file(&mut device, parent_id, object_id, name)?;
    }
    device.flush()?;
    println!(
        "Created {} '{}' (object ID {}).",
        if directory { "directory" } else { "file" },
        target,
        object_id.0
    );
    Ok(())
}

/// Create a file only when it is missing. Existing regular files are left untouched,
/// while a directory at the requested path is reported as a type error.
fn touch_namespace_file(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    // Release the writable volume (and its single-writer lease) before delegating
    // creation to create_namespace_entry, which opens the image independently.
    let exists = {
        let mut device = ImageDevice::open(path, block_size)?;
        let volume = open_volume(&mut device)?;
        let (_, namespace) = volume
            .reconstruct_discovered_namespace(&mut device)?
            .ok_or("no persistent namespace; run init-namespace first")?;
        match namespace.resolve_path(target) {
            Ok(object_id) => {
                let record = namespace
                    .object(object_id)
                    .ok_or("object metadata is missing")?;
                if record.kind != ObjectKind::File {
                    return Err("touch target already exists and is not a regular file".into());
                }
                true
            }
            Err(_) => false,
        }
    };

    if exists {
        println!("File '{}' already exists; left unchanged.", target);
        Ok(())
    } else {
        create_namespace_entry(path, block_size, target, false)
    }
}

/// Create missing directories in a path, accepting existing directories and
/// refusing to traverse through regular files.
fn create_namespace_directories(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let normalized = target.trim_matches('/');
    if normalized.is_empty() {
        let mut device = ImageDevice::open(path, block_size)?;
        let volume = open_volume(&mut device)?;
        let (_, namespace) = volume
            .reconstruct_discovered_namespace(&mut device)?
            .ok_or("no persistent namespace; run init-namespace first")?;
        let root = namespace
            .object(namespace.root_id())
            .ok_or("namespace root metadata is missing")?;
        if root.kind != ObjectKind::Directory {
            return Err("namespace root is not a directory".into());
        }
        println!("Directory '{}' already exists.", target);
        return Ok(());
    }

    let mut current = String::new();
    let mut created = 0_u64;
    for component in normalized.split('/') {
        if component.is_empty() {
            continue;
        }
        if component == "." || component == ".." {
            return Err("mkdir-p does not accept '.' or '..' path components".into());
        }
        current.push('/');
        current.push_str(component);

        let existing_kind = {
            let mut device = ImageDevice::open(path, block_size)?;
            let volume = open_volume(&mut device)?;
            let (_, namespace) = volume
                .reconstruct_discovered_namespace(&mut device)?
                .ok_or("no persistent namespace; run init-namespace first")?;
            namespace
                .resolve_path(&current)
                .ok()
                .and_then(|id| namespace.object(id).map(|record| record.kind))
        };

        match existing_kind {
            Some(ObjectKind::Directory) => continue,
            Some(ObjectKind::File) => {
                return Err(format!("cannot create '{}' through a regular file", current).into())
            }
            None => {
                create_namespace_entry(path, block_size, &current, true)?;
                created = created.checked_add(1).ok_or("directory count overflows")?;
            }
        }
    }
    println!(
        "mkdir-p completed for '{}': {created} directory/directories created.",
        target
    );
    Ok(())
}

fn list_namespace(path: &Path, block_size: u32, target: &str) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let directory_id = namespace.resolve_path(target)?;
    let record = namespace
        .object(directory_id)
        .ok_or("directory metadata is missing")?;
    if record.kind != ObjectKind::Directory {
        return Err("path is not a directory".into());
    }
    for entry in namespace.children(directory_id) {
        let kind = match entry.kind {
            ObjectKind::Directory => "dir ",
            ObjectKind::File => "file",
        };
        let record = namespace
            .object(ObjectId(entry.object_id))
            .ok_or("directory entry references missing object metadata")?;
        println!(
            "{kind}\t{}\t{} bytes\tgeneration {}\t{}",
            entry.object_id, record.size, record.generation, entry.name
        );
    }
    Ok(())
}

/// Print a namespace subtree without recursive calls, so deeply nested trees
/// cannot exhaust the process stack. The persistent inventory is reconstructed and
/// validated by the same path used by ls/stat before traversal begins.
fn tree_namespace(path: &Path, block_size: u32, target: &str) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let root_id = namespace.resolve_path(target)?;
    let root_record = namespace
        .object(root_id)
        .ok_or("tree root metadata is missing")?;
    println!("{}", namespace.path_of(root_id)?);

    let mut stack: Vec<(ObjectId, usize)> = if root_record.kind == ObjectKind::Directory {
        vec![(root_id, 0)]
    } else {
        Vec::new()
    };
    while let Some((directory_id, depth)) = stack.pop() {
        let children = namespace
            .children(directory_id)
            .cloned()
            .collect::<Vec<_>>();
        let mut child_directories = Vec::new();
        for entry in &children {
            let child_id = ObjectId(entry.object_id);
            let record = namespace
                .object(child_id)
                .ok_or("directory entry references missing object metadata")?;
            let indent = "  ".repeat(depth + 1);
            let suffix = if record.kind == ObjectKind::Directory {
                "/"
            } else {
                ""
            };
            println!("{indent}{}{suffix}", entry.name);
            if record.kind == ObjectKind::Directory {
                child_directories.push(child_id);
            }
        }
        // Stack is LIFO: reverse the sorted child list to visit subtrees in lexical order.
        for child_id in child_directories.into_iter().rev() {
            stack.push((child_id, depth + 1));
        }
    }
    Ok(())
}

/// Search a namespace subtree by substring, traversing iteratively to avoid stack overflow.
fn find_namespace_entries(
    path: &Path,
    block_size: u32,
    root: &str,
    needle: &str,
) -> Result<(), Box<dyn StdError>> {
    if needle.is_empty() {
        return Err("search text must not be empty".into());
    }
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let root_id = namespace.resolve_path(root)?;
    let mut pending = vec![root_id];
    let mut matches = 0_u64;

    while let Some(object_id) = pending.pop() {
        let record = namespace
            .object(object_id)
            .ok_or("namespace entry references missing object metadata")?;
        let full_path = namespace.path_of(object_id)?;
        if full_path.contains(needle) {
            let kind = match record.kind {
                ObjectKind::Directory => "directory",
                ObjectKind::File => "file",
            };
            println!("{kind}\t{} bytes\t{full_path}", record.size);
            matches = matches.checked_add(1).ok_or("match count overflows")?;
        }
        if record.kind == ObjectKind::Directory {
            let children = namespace
                .children(object_id)
                .map(|entry| entry.object_id)
                .collect::<Vec<_>>();
            // Reverse push preserves the namespace's lexical order during traversal.
            for child in children.into_iter().rev() {
                pending.push(ObjectId(child));
            }
        }
    }
    println!("Matches: {matches}");
    Ok(())
}

/// Summarize logical and physically allocated file bytes for a namespace subtree.
fn report_namespace_usage(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let root_id = namespace.resolve_path(target)?;
    let extent_maps = volume.read_namespace_extent_maps(&mut device, &namespace)?;
    let block_bytes = u64::from(block_size);
    let mut pending = vec![root_id];
    let mut files = 0_u64;
    let mut directories = 0_u64;
    let mut logical_bytes = 0_u64;
    let mut allocated_bytes = 0_u64;
    let mut sparse_bytes = 0_u64;

    while let Some(object_id) = pending.pop() {
        let record = namespace
            .object(object_id)
            .ok_or("namespace entry references missing object metadata")?;
        match record.kind {
            ObjectKind::Directory => {
                directories = directories
                    .checked_add(1)
                    .ok_or("directory count overflows")?;
                let children = namespace
                    .children(object_id)
                    .map(|entry| entry.object_id)
                    .collect::<Vec<_>>();
                for child in children.into_iter().rev() {
                    pending.push(ObjectId(child));
                }
            }
            ObjectKind::File => {
                files = files.checked_add(1).ok_or("file count overflows")?;
                logical_bytes = logical_bytes
                    .checked_add(record.size)
                    .ok_or("logical usage overflows")?;
                let allocated_blocks = match extent_maps.get(&object_id) {
                    Some(map) => map.allocated_blocks()?,
                    None if record.size == 0 && record.extent_root_block.is_none() => 0,
                    None => {
                        return Err("file extent map is missing from persistent inventory".into())
                    }
                };
                let file_allocated_bytes = allocated_blocks
                    .checked_mul(block_bytes)
                    .ok_or("allocated usage overflows")?;
                allocated_bytes = allocated_bytes
                    .checked_add(file_allocated_bytes)
                    .ok_or("allocated usage overflows")?;
                let allocated_logical_bytes = match extent_maps.get(&object_id) {
                    Some(map) => map.extents().iter().try_fold(0_u64, |total, extent| {
                        let start = extent
                            .logical_start
                            .checked_mul(block_bytes)
                            .ok_or("logical extent start overflows")?;
                        let end_block = extent
                            .logical_start
                            .checked_add(extent.block_count)
                            .ok_or("logical extent end overflows")?;
                        let end = end_block
                            .checked_mul(block_bytes)
                            .ok_or("logical extent end overflows")?;
                        total
                            .checked_add(
                                record.size.min(end).saturating_sub(start.min(record.size)),
                            )
                            .ok_or("allocated logical usage overflows")
                    })?,
                    None => 0,
                };
                sparse_bytes = sparse_bytes
                    .checked_add(record.size - allocated_logical_bytes)
                    .ok_or("sparse usage overflows")?;
            }
        }
    }

    println!("OpenFS namespace usage");
    println!("Path: {}", namespace.path_of(root_id)?);
    println!("Directories: {directories}");
    println!("Files: {files}");
    println!("Logical file bytes: {logical_bytes}");
    println!("Allocated data bytes: {allocated_bytes}");
    println!("Sparse logical bytes: {sparse_bytes}");
    println!(
        "Note: allocated data excludes extent-map metadata and filesystem journal/bitmap overhead."
    );
    Ok(())
}

/// Report usable capacity and fragmentation from the on-disk allocation bitmap.
fn report_space(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let allocation = volume.allocation_stats(&mut device)?;
    let fragmentation = volume.free_extent_stats(&mut device)?;
    let block_bytes = u64::from(block_size);
    let total_bytes = allocation
        .total_data_blocks
        .checked_mul(block_bytes)
        .ok_or("total capacity overflows byte count")?;
    let allocated_bytes = allocation
        .allocated_data_blocks
        .checked_mul(block_bytes)
        .ok_or("allocated capacity overflows byte count")?;
    let free_bytes = allocation
        .free_data_blocks
        .checked_mul(block_bytes)
        .ok_or("free capacity overflows byte count")?;

    println!("OpenFS data-space report");
    println!("Total data blocks: {}", allocation.total_data_blocks);
    println!(
        "Allocated data blocks: {}",
        allocation.allocated_data_blocks
    );
    println!("Free data blocks: {}", allocation.free_data_blocks);
    println!("Total data capacity: {total_bytes} bytes");
    println!("Allocated data capacity: {allocated_bytes} bytes");
    println!("Free data capacity: {free_bytes} bytes");
    println!("Free-space extents: {}", fragmentation.free_extent_count);
    println!(
        "Largest free extent: {} blocks ({} bytes)",
        fragmentation.largest_free_extent_blocks,
        fragmentation
            .largest_free_extent_blocks
            .checked_mul(block_bytes)
            .ok_or("largest extent byte count overflows")?
    );
    if allocation.free_data_blocks == 0 {
        println!("Free-space ratio: 0.00%");
        println!("Free-space fragmentation: 0.00%");
    } else {
        let ratio =
            (allocation.free_data_blocks as f64 / allocation.total_data_blocks as f64) * 100.0;
        println!("Free-space ratio: {ratio:.2}%");
        let fragmented_free_blocks = allocation
            .free_data_blocks
            .checked_sub(fragmentation.largest_free_extent_blocks)
            .ok_or("largest free extent exceeds total free blocks")?;
        let fragmentation_ratio =
            (fragmented_free_blocks as f64 / allocation.free_data_blocks as f64) * 100.0;
        println!("Free-space fragmentation: {fragmentation_ratio:.2}%");
    }
    Ok(())
}

fn read_namespace_file(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let mut stdout = std::io::stdout().lock();
    volume.stream_persistent_file_to_writer(&mut device, target, &mut stdout)?;
    Ok(())
}

fn read_namespace_file_range(
    path: &Path,
    block_size: u32,
    target: &str,
    offset: u64,
    length: u64,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let mut stdout = std::io::stdout().lock();
    volume.stream_persistent_path_range_to_writer(
        &mut device,
        target,
        offset,
        length,
        &mut stdout,
    )?;
    Ok(())
}

/// Compute CRC32C for a byte range without buffering the entire range in memory.
fn checksum_namespace_file_range(
    path: &Path,
    block_size: u32,
    target: &str,
    offset: u64,
    length: u64,
    expected_crc32c: Option<u32>,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let mut checksum = Crc32cWriter::default();
    let bytes = volume.stream_persistent_path_range_to_writer(
        &mut device,
        target,
        offset,
        length,
        &mut checksum,
    )?;
    let actual = checksum.finish();
    println!("File: {target}");
    println!(
        "Range: {offset}..{}",
        offset.checked_add(bytes).ok_or("range end overflows")?
    );
    println!("Size: {bytes} bytes");
    println!("CRC32C: {actual:08x}");
    if let Some(expected) = expected_crc32c {
        if actual != expected {
            println!("Checksum verification: FAILED (expected {expected:08x})");
            return Err("CRC32C checksum mismatch".into());
        }
        println!("Checksum verification: OK");
    }
    Ok(())
}

/// Copy one persistent file to another without buffering its contents in RAM.
fn copy_namespace_file(
    image_path: &Path,
    block_size: u32,
    source_path: &str,
    destination_path: &str,
) -> Result<(), Box<dyn StdError>> {
    if source_path == destination_path {
        return Err("source and destination paths must differ".into());
    }

    let mut device = ImageDevice::open(image_path, block_size)?;
    let mut volume = open_volume(&mut device)?;

    // A temporary file bounds memory use for large files and also makes a copy
    // safe when source and destination share the same volume and allocator.
    let temp_path = unique_temporary_path("copy");
    let mut temporary = OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(&temp_path)?;
    let copy_result = (|| -> Result<u64, Box<dyn StdError>> {
        let copied =
            volume.stream_persistent_file_to_writer(&mut device, source_path, &mut temporary)?;
        temporary.sync_all()?;
        use std::io::{Seek, SeekFrom};
        temporary.seek(SeekFrom::Start(0))?;
        let written = volume.write_persistent_file_from_reader(
            &mut device,
            destination_path,
            &mut temporary,
        )?;
        if written != copied {
            return Err("copied byte count changed while reading the staged file".into());
        }
        device.flush()?;
        Ok(written)
    })();

    drop(temporary);
    let _ = fs::remove_file(&temp_path);
    let copied = copy_result?;
    println!("Copied '{source_path}' to '{destination_path}' ({copied} bytes).");
    Ok(())
}

/// Choose a fresh host temporary path; create_new provides the actual collision guard.
fn unique_temporary_path(label: &str) -> std::path::PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    std::env::temp_dir().join(format!("openfs-{label}-{}-{nonce}.tmp", std::process::id()))
}

fn export_namespace_file(
    image_path: &Path,
    block_size: u32,
    target: &str,
    output_path: &Path,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(image_path, block_size)?;
    let volume = open_volume(&mut device)?;

    // Never silently overwrite a host file during export.
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output_path)?;
    let result = (|| -> Result<u64, Box<dyn StdError>> {
        let exported_size =
            volume.stream_persistent_file_to_writer(&mut device, target, &mut output)?;
        output.sync_all()?;
        Ok(exported_size)
    })();

    let exported_size = match result {
        Ok(size) => size,
        Err(error) => {
            drop(output);
            let _ = fs::remove_file(output_path);
            return Err(error);
        }
    };
    println!(
        "Exported '{}' ({} bytes) to '{}'.",
        target,
        exported_size,
        output_path.display()
    );
    Ok(())
}

/// Import a host file into an existing OpenFS regular file.
fn import_host_file(
    image_path: &Path,
    block_size: u32,
    input_path: &Path,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    // Reject importing the image into itself. Streaming would otherwise read from
    // a file while OpenFS is modifying that same file underneath the source handle.
    let canonical_input = fs::canonicalize(input_path)?;
    let canonical_image = fs::canonicalize(image_path)?;
    if canonical_input == canonical_image {
        return Err("host source and OpenFS image refer to the same file".into());
    }

    // Open the source before mutating the image. The format layer streams one block
    // at a time and only publishes the replacement after all source bytes are staged.
    let mut source = fs::File::open(input_path)?;
    let mut device = ImageDevice::open(image_path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let imported_bytes =
        volume.write_persistent_file_from_reader(&mut device, target, &mut source)?;
    device.flush()?;
    println!(
        "Imported '{}' ({} bytes) into '{}'.",
        input_path.display(),
        imported_bytes,
        target
    );
    Ok(())
}

fn stat_namespace_entry(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let object_id = namespace.resolve_path(target)?;
    let record = namespace
        .object(object_id)
        .ok_or("object metadata is missing")?;
    let kind = match record.kind {
        ObjectKind::Directory => "directory",
        ObjectKind::File => "file",
    };
    println!("Path: {}", namespace.path_of(object_id)?);
    println!("Object ID: {}", record.id.0);
    println!("Type: {kind}");
    println!("Size: {} bytes", record.size);
    println!("Generation: {}", record.generation);
    match record.extent_root_block {
        Some(block) => println!("Extent-map root block: {block}"),
        None => println!("Extent-map root block: none"),
    }

    if record.kind == ObjectKind::File {
        let maps = volume.read_namespace_extent_maps(&mut device, &namespace)?;
        let map = match maps.get(&object_id) {
            Some(map) => map,
            None if record.size == 0 && record.extent_root_block.is_none() => {
                println!("Logical blocks: 0");
                println!("Allocated data blocks: 0");
                println!("Allocated data bytes: 0");
                println!("Physical extents: 0");
                println!("Sparse logical bytes: 0");
                println!("Unused capacity in final allocated block: 0 bytes");
                return Ok(());
            }
            None => return Err("file extent map is missing from the persistent inventory".into()),
        };
        let allocated_blocks = map.extents().iter().try_fold(0_u64, |total, extent| {
            total
                .checked_add(extent.block_count)
                .ok_or("allocated block count overflows")
        })?;
        let block_size_bytes = u64::from(block_size);
        let allocated_bytes = allocated_blocks
            .checked_mul(block_size_bytes)
            .ok_or("allocated byte count overflows")?;
        let logical_blocks =
            record.size / block_size_bytes + u64::from(record.size % block_size_bytes != 0);
        println!("Logical blocks: {logical_blocks}");
        println!("Allocated data blocks: {allocated_blocks}");
        println!("Allocated data bytes: {allocated_bytes}");
        println!("Physical extents: {}", map.extents().len());
        let allocated_logical_bytes = map.extents().iter().try_fold(0_u64, |total, extent| {
            let start = extent
                .logical_start
                .checked_mul(block_size_bytes)
                .ok_or("logical extent start overflows")?;
            let end_block = extent
                .logical_start
                .checked_add(extent.block_count)
                .ok_or("logical extent end overflows")?;
            let end = end_block
                .checked_mul(block_size_bytes)
                .ok_or("logical extent byte end overflows")?;
            total
                .checked_add(record.size.min(end).saturating_sub(start.min(record.size)))
                .ok_or("allocated logical byte count overflows")
        })?;
        println!(
            "Sparse logical bytes: {}",
            record.size - allocated_logical_bytes
        );

        // Sparse holes are logical bytes without physical allocation, not unused
        // capacity in the final block. Report only the tail slack of the final
        // logical block, and only when that block is physically allocated.
        let tail_slack = if record.size % block_size_bytes != 0
            && logical_blocks > 0
            && map.physical_block(logical_blocks - 1).is_some()
        {
            block_size_bytes - record.size % block_size_bytes
        } else {
            0
        };
        println!("Unused capacity in final allocated block: {tail_slack} bytes");
    } else {
        let mut child_count = 0_u64;
        let mut file_count = 0_u64;
        let mut directory_count = 0_u64;
        for child in namespace.children(object_id) {
            child_count = child_count
                .checked_add(1)
                .ok_or("directory child count overflows")?;
            match child.kind {
                ObjectKind::File => {
                    file_count = file_count
                        .checked_add(1)
                        .ok_or("directory file count overflows")?;
                }
                ObjectKind::Directory => {
                    directory_count = directory_count
                        .checked_add(1)
                        .ok_or("directory subdirectory count overflows")?;
                }
            }
        }
        println!("Direct children: {child_count}");
        println!("Direct files: {file_count}");
        println!("Direct subdirectories: {directory_count}");

        // Aggregate the whole subtree iteratively: deep directory trees must not
        // consume the process call stack, and every total is overflow-checked.
        let mut pending = vec![object_id];
        let mut visited = std::collections::BTreeSet::new();
        let mut subtree_files = 0_u64;
        let mut subtree_directories = 0_u64;
        let mut subtree_logical_bytes = 0_u64;
        while let Some(parent_id) = pending.pop() {
            if !visited.insert(parent_id) {
                return Err("directory graph contains a cycle or duplicate traversal".into());
            }
            for child in namespace.children(parent_id) {
                match child.kind {
                    ObjectKind::File => {
                        let child_id = ObjectId(child.object_id);
                        let child_record = namespace
                            .object(child_id)
                            .ok_or("directory entry references missing file metadata")?;
                        subtree_files = subtree_files
                            .checked_add(1)
                            .ok_or("subtree file count overflows")?;
                        subtree_logical_bytes = subtree_logical_bytes
                            .checked_add(child_record.size)
                            .ok_or("subtree logical byte count overflows")?;
                    }
                    ObjectKind::Directory => {
                        subtree_directories = subtree_directories
                            .checked_add(1)
                            .ok_or("subtree directory count overflows")?;
                        pending.push(ObjectId(child.object_id));
                    }
                }
            }
        }
        println!("Recursive files: {subtree_files}");
        println!("Recursive subdirectories: {subtree_directories}");
        println!("Subtree logical bytes: {subtree_logical_bytes}");
    }
    Ok(())
}

fn write_namespace_file(
    path: &Path,
    block_size: u32,
    target: &str,
    data: &[u8],
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    volume.write_persistent_file_contents(&mut device, target, data)?;
    device.flush()?;
    println!("Wrote {} byte(s) to '{}'.", data.len(), target);
    Ok(())
}

fn write_namespace_file_from_stdin(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let stdin = std::io::stdin();
    let mut input = stdin.lock();
    let written = volume.write_persistent_file_from_reader(&mut device, target, &mut input)?;
    device.flush()?;
    println!("Wrote {written} byte(s) from standard input to '{target}'.");
    Ok(())
}

fn write_namespace_file_range(
    path: &Path,
    block_size: u32,
    target: &str,
    offset: u64,
    data: &[u8],
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    volume.write_persistent_path_range(&mut device, target, offset, data)?;
    device.flush()?;
    println!(
        "Wrote {} byte(s) at offset {} in '{}'.",
        data.len(),
        offset,
        target
    );
    Ok(())
}

fn append_namespace_file(
    path: &Path,
    block_size: u32,
    target: &str,
    data: &[u8],
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let object_id = namespace.resolve_path(target)?;
    let record = namespace
        .object(object_id)
        .ok_or("file metadata is missing")?;
    if record.kind != ObjectKind::File {
        return Err("path is not a regular file".into());
    }
    let offset = record.size;
    let appended_size = u64::try_from(data.len())?;
    let new_size = offset
        .checked_add(appended_size)
        .ok_or("appended file size overflows the supported range")?;
    volume.write_persistent_path_range(&mut device, target, offset, data)?;
    device.flush()?;
    println!(
        "Appended {} byte(s) to '{}' (new size: {} bytes).",
        data.len(),
        target,
        new_size
    );
    Ok(())
}

fn truncate_namespace_file(
    path: &Path,
    block_size: u32,
    target: &str,
    new_size: u64,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    volume.truncate_persistent_file(&mut device, target, new_size)?;
    device.flush()?;
    println!("Truncated '{target}' to {new_size} byte(s).");
    Ok(())
}

fn remove_namespace_entry(
    path: &Path,
    block_size: u32,
    target: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let object_id = namespace.resolve_path(target)?;
    if object_id == namespace.root_id() {
        return Err("cannot remove the namespace root".into());
    }
    let record = namespace
        .object(object_id)
        .ok_or("object metadata is missing")?;
    if record.kind == ObjectKind::Directory {
        volume.remove_persistent_tree(&mut device, object_id)?;
    } else {
        volume.remove_persistent_object(&mut device, object_id)?;
    }
    device.flush()?;
    println!("Removed '{target}'.");
    Ok(())
}

fn move_namespace_entry(
    path: &Path,
    block_size: u32,
    source: &str,
    destination: &str,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    let (_, namespace) = volume
        .reconstruct_discovered_namespace(&mut device)?
        .ok_or("no persistent namespace; run init-namespace first")?;
    let object_id = namespace.resolve_path(source)?;
    if object_id == namespace.root_id() {
        return Err("cannot move the namespace root".into());
    }
    let (parent_path, name) = split_parent_path(destination)?;
    let parent_id = namespace.resolve_path(parent_path)?;
    volume.rename_persistent_object(&mut device, object_id, parent_id, name)?;
    device.flush()?;
    println!("Moved '{source}' to '{destination}'.");
    Ok(())
}

/// Create the first persistent root namespace in an existing development image.
fn initialize_namespace_image(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    if ![4096, 8192, 16_384, 32_768, 65_536].contains(&block_size) {
        return Err("unsupported block size; choose 4096, 8192, 16384, 32768, or 65536".into());
    }

    let mut device = ImageDevice::open(path, block_size)?;
    let mut volume = open_volume(&mut device)?;
    volume.initialize_persistent_namespace(&mut device, ObjectId::new(1)?)?;
    device.flush()?;

    println!("OpenFS persistent namespace initialized.");
    println!("Path: {}", path.display());
    println!("Root object ID: 1");
    println!("The image now has a durable root directory; no files or directories were created.");
    Ok(())
}

/// Explicitly open an image through the journal recovery path.
///
/// Unlike check-image, this command may write recovered home blocks and a journal
/// checkpoint. Users should keep a copy of the image before running it.
fn recover_image(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    if ![4096, 8192, 16_384, 32_768, 65_536].contains(&block_size) {
        return Err("unsupported block size; choose 4096, 8192, 16384, 32768, or 65536".into());
    }

    let mut device = ImageDevice::open(path, block_size)?;
    println!("WARNING: recover-image may modify the image while replaying the journal.");
    println!("Path: {}", path.display());
    let volume = open_volume(&mut device)?;
    device.flush()?;
    let report = check_volume(&mut device)?;

    println!("OpenFS image recovery path completed.");
    println!("UUID: {}", format_uuid(volume.volume.uuid));
    println!(
        "Journal records remaining after recovery: {}",
        report.pending_journal_records
    );
    println!(
        "Recoverable committed transactions remaining: {}",
        report.recoverable_transactions
    );
    if report.pending_journal_records > 0 || report.recoverable_transactions > 0 {
        return Err(
            "journal entries remain after recovery; preserve the image and investigate before use"
                .into(),
        );
    }
    println!(
        "Journal checkpoint is current; this command does not validate file ownership or namespace integrity."
    );
    Ok(())
}

fn development_uuid() -> [u8; 16] {
    // A volume label only; this is not a cryptographic random identifier.
    let time = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    let mixed = time ^ (u128::from(std::process::id()) << 64);
    mixed.to_le_bytes()
}

fn format_uuid(uuid: [u8; 16]) -> String {
    let hex = uuid
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<String>();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

fn parse_probe_block_size(value: Option<String>) -> Result<u32, Box<dyn StdError>> {
    let size = value.ok_or("missing block size")?.parse::<u32>()?;
    if !(512..=65_536).contains(&size) || !size.is_power_of_two() {
        return Err("probe block size must be a power of two from 512 through 65536".into());
    }
    Ok(size)
}

/// Inspect GPT metadata from a physical device without issuing any writes.
fn inspect_raw_gpt_report(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<GptInspection, Box<dyn StdError>> {
    if block_size < 512 {
        return Err("GPT inspection requires a logical block size of at least 512 bytes".into());
    }
    if block_count < 3 {
        return Err("device is too small to contain GPT metadata".into());
    }

    let mut device = ReadOnlyRawDevice::open(path, block_size, block_count)?;
    let mut mbr = vec![0_u8; block_size as usize];
    device.read_blocks(0, &mut mbr)?;
    validate_protective_mbr(&mbr, block_count)?;

    let mut header = vec![0_u8; block_size as usize];
    device.read_blocks(1, &mut header)?;
    if header.get(0..8) != Some(b"EFI PART") {
        return Err(
            "primary GPT header signature is missing; no GPT partition layout can be trusted"
                .into(),
        );
    }

    let revision = read_u32_le(&header, 8)?;
    if revision != 0x0001_0000 {
        return Err(format!("unsupported GPT header revision 0x{revision:08x}").into());
    }
    let header_size = read_u32_le(&header, 12)?;
    if !(92..=block_size).contains(&header_size) {
        return Err("GPT header size is outside the supported bounds".into());
    }
    if read_u32_le(&header, 20)? != 0 {
        return Err("GPT header reserved field must be zero".into());
    }
    if header[56..72].iter().all(|byte| *byte == 0) {
        return Err("GPT disk GUID must not be zero".into());
    }
    let expected_header_crc = read_u32_le(&header, 16)?;
    let mut crc_header = header[..header_size as usize].to_vec();
    crc_header[16..20].fill(0);
    let actual_header_crc = crc32_ieee(&crc_header);
    if actual_header_crc != expected_header_crc {
        return Err(format!(
            "primary GPT header CRC mismatch (stored {expected_header_crc:08x}, computed {actual_header_crc:08x})"
        )
        .into());
    }

    let current_lba = read_u64_le(&header, 24)?;
    let backup_lba = read_u64_le(&header, 32)?;
    let first_usable = read_u64_le(&header, 40)?;
    let last_usable = read_u64_le(&header, 48)?;
    let entries_lba = read_u64_le(&header, 72)?;
    let entry_count = read_u32_le(&header, 80)?;
    let entry_size = read_u32_le(&header, 84)?;
    let expected_entries_crc = read_u32_le(&header, 88)?;

    if current_lba != 1
        || backup_lba != block_count - 1
        || first_usable < 2
        || first_usable > last_usable
        || last_usable >= backup_lba
    {
        return Err("GPT header contains invalid device geometry".into());
    }
    if entry_count == 0 || entry_count > 4096 || entry_size < 128 || entry_size % 8 != 0 {
        return Err("GPT partition-entry geometry is invalid or exceeds safety limits".into());
    }
    let entries_bytes = u64::from(entry_count)
        .checked_mul(u64::from(entry_size))
        .ok_or("GPT partition-entry byte length overflow")?;
    let entries_blocks = entries_bytes
        .checked_add(u64::from(block_size) - 1)
        .ok_or("GPT partition-entry block count overflow")?
        / u64::from(block_size);
    let entries_end = entries_lba
        .checked_add(entries_blocks)
        .ok_or("GPT partition-entry range overflow")?;
    if entries_lba < 2 || entries_end > block_count || entries_bytes > 16 * 1024 * 1024 {
        return Err("GPT partition-entry array lies outside the device or exceeds 16 MiB".into());
    }

    let padded_entries_bytes = entries_blocks
        .checked_mul(u64::from(block_size))
        .ok_or("GPT padded partition-entry length overflow")?;
    let mut entries = vec![0_u8; usize::try_from(padded_entries_bytes)?];
    device.read_blocks(entries_lba, &mut entries)?;
    let entries = &entries[..usize::try_from(entries_bytes)?];
    let actual_entries_crc = crc32_ieee(entries);
    if actual_entries_crc != expected_entries_crc {
        return Err(format!(
            "GPT partition-entry CRC mismatch (stored {expected_entries_crc:08x}, computed {actual_entries_crc:08x})"
        )
        .into());
    }

    // Validate the backup copy too. A valid primary GPT alone does not prove that
    // the secondary header and its partition array are usable for recovery.
    if entries_end > first_usable {
        return Err("primary GPT partition-entry array overlaps the usable LBA range".into());
    }
    let mut backup_header = vec![0_u8; block_size as usize];
    device.read_blocks(backup_lba, &mut backup_header)?;
    if backup_header.get(0..8) != Some(b"EFI PART") {
        return Err("backup GPT header signature is missing".into());
    }
    let backup_revision = read_u32_le(&backup_header, 8)?;
    if backup_revision != revision {
        return Err("primary and backup GPT revisions disagree".into());
    }
    let backup_header_size = read_u32_le(&backup_header, 12)?;
    if backup_header_size != header_size || !(92..=block_size).contains(&backup_header_size) {
        return Err("backup GPT header size disagrees with the primary header".into());
    }
    if read_u32_le(&backup_header, 20)? != 0 {
        return Err("backup GPT header reserved field must be zero".into());
    }
    let expected_backup_header_crc = read_u32_le(&backup_header, 16)?;
    let mut crc_backup_header = backup_header[..backup_header_size as usize].to_vec();
    crc_backup_header[16..20].fill(0);
    let actual_backup_header_crc = crc32_ieee(&crc_backup_header);
    if actual_backup_header_crc != expected_backup_header_crc {
        return Err(format!(
            "backup GPT header CRC mismatch (stored {expected_backup_header_crc:08x}, computed {actual_backup_header_crc:08x})"
        ).into());
    }
    let backup_current_lba = read_u64_le(&backup_header, 24)?;
    let backup_primary_lba = read_u64_le(&backup_header, 32)?;
    let backup_first_usable = read_u64_le(&backup_header, 40)?;
    let backup_last_usable = read_u64_le(&backup_header, 48)?;
    let backup_entries_lba = read_u64_le(&backup_header, 72)?;
    let backup_entry_count = read_u32_le(&backup_header, 80)?;
    let backup_entry_size = read_u32_le(&backup_header, 84)?;
    let backup_entries_crc = read_u32_le(&backup_header, 88)?;
    if backup_current_lba != backup_lba
        || backup_primary_lba != current_lba
        || backup_first_usable != first_usable
        || backup_last_usable != last_usable
        || backup_header.get(56..72) != header.get(56..72)
        || backup_entry_count != entry_count
        || backup_entry_size != entry_size
        || backup_entries_crc != expected_entries_crc
    {
        return Err("primary and backup GPT headers disagree".into());
    }
    let backup_entries_end = backup_entries_lba
        .checked_add(entries_blocks)
        .ok_or("backup GPT partition-entry range overflow")?;
    if backup_entries_lba <= last_usable || backup_entries_end != backup_lba {
        return Err("backup GPT partition-entry array is outside its expected region".into());
    }
    let mut backup_entries = vec![0_u8; usize::try_from(padded_entries_bytes)?];
    device.read_blocks(backup_entries_lba, &mut backup_entries)?;
    let backup_entries = &backup_entries[..usize::try_from(entries_bytes)?];
    let actual_backup_entries_crc = crc32_ieee(backup_entries);
    if actual_backup_entries_crc != expected_entries_crc || backup_entries != entries {
        return Err(
            "backup GPT partition-entry array is corrupt or differs from the primary copy".into(),
        );
    }

    let partitions =
        validate_gpt_partitions(entries, entry_count, entry_size, first_usable, last_usable)?;
    let capacity_bytes = u64::from(block_size)
        .checked_mul(block_count)
        .ok_or("device capacity overflows u64")?;
    Ok(GptInspection {
        device: path.display().to_string(),
        block_size,
        block_count,
        capacity_bytes,
        backup_header_lba: backup_lba,
        first_usable_lba: first_usable,
        last_usable_lba: last_usable,
        partitions,
    })
}

/// Diagnose MBR and both GPT copies independently, without modifying the device.
fn diagnose_raw_gpt(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<(), Box<dyn StdError>> {
    if block_size < 512 || block_count < 3 {
        return Err("GPT diagnosis requires at least three blocks of 512 bytes".into());
    }

    let mut device = ReadOnlyRawDevice::open(path, block_size, block_count)?;
    let mut mbr = vec![0_u8; block_size as usize];
    device.read_blocks(0, &mut mbr)?;

    println!("Read-only MBR/GPT diagnostic");
    println!("Device: {}", path.display());
    println!("Capacity: {} bytes", u64::from(block_size) * block_count);
    println!(
        "MBR signature: {}",
        if mbr.get(510..512) == Some(&[0x55, 0xaa]) {
            "0x55AA (present)"
        } else {
            "INVALID / MISSING"
        }
    );
    for index in 0..4 {
        let offset = 446 + index * 16;
        let entry = &mbr[offset..offset + 16];
        let partition_type = entry[4];
        let start_lba = u32::from_le_bytes(entry[8..12].try_into()?);
        let sectors = u32::from_le_bytes(entry[12..16].try_into()?);
        if partition_type != 0 || entry[0] != 0 || start_lba != 0 || sectors != 0 {
            println!(
                "MBR entry {}: boot=0x{:02x}, type=0x{:02x}, start_lba={}, sectors={}",
                index + 1,
                entry[0],
                partition_type,
                start_lba,
                sectors
            );
            if partition_type == 0xEE {
                let expected = (block_count - 1).min(u64::from(u32::MAX)) as u32;
                println!(
                    "  Protective coverage: observed={} sectors, expected={} sectors ({})",
                    sectors,
                    expected,
                    if sectors == expected {
                        "OK"
                    } else {
                        "MISMATCH"
                    }
                );
            }
        }
    }
    match validate_protective_mbr(&mbr, block_count) {
        Ok(()) => println!("Protective MBR validation: OK"),
        Err(error) => println!("Protective MBR validation: FAILED ({error})"),
    }

    let backup_lba = block_count - 1;
    let mut primary_report = None;
    let mut backup_report = None;
    for (label, lba) in [("PRIMARY", 1), ("BACKUP", backup_lba)] {
        println!();
        println!("{label} GPT copy at LBA {lba}:");
        match diagnose_gpt_copy(&mut device, block_size, block_count, lba, label) {
            Ok(partitions) => {
                println!("{label} GPT copy diagnostic: completed");
                if label == "PRIMARY" {
                    primary_report = Some(partitions);
                } else {
                    backup_report = Some(partitions);
                }
            }
            Err(error) => println!("{label} GPT copy diagnostic: FAILED ({error})"),
        }
    }
    match (primary_report, backup_report) {
        (Some(primary), Some(backup)) if primary == backup && primary.0.is_empty() => {
            println!(
                "Primary/backup partition semantics: MATCH (both copies contain no partitions)"
            );
            println!("GPT partition table state: EMPTY");
            println!(
                "No filesystem can be associated through GPT because no partition entries exist."
            );
            println!("An empty GPT does not prove that the device contains no recoverable data.");
            probe_unpartitioned_filesystem_candidates(&mut device, block_size, block_count)?;
        }
        (Some(primary), Some(backup)) if primary == backup => {
            println!("Primary/backup partition semantics: MATCH");
        }
        (Some(_), Some(_)) => {
            println!("Primary/backup partition semantics: MISMATCH");
        }
        _ => println!("Primary/backup partition semantics: NOT COMPARABLE (one copy failed)"),
    }
    println!();
    println!("No writes were performed.");
    Ok(())
}

/// Validated GPT copy details returned by the diagnostic reader.
type GptCopyDiagnostic = (Vec<GptPartition>, Vec<u8>, [u8; 16], u64, u64);

/// Validate and report one GPT header and its referenced partition-entry array.
fn diagnose_gpt_copy(
    device: &mut ReadOnlyRawDevice,
    block_size: u32,
    block_count: u64,
    header_lba: u64,
    label: &str,
) -> Result<GptCopyDiagnostic, Box<dyn StdError>> {
    println!("{label} GPT copy details:");
    let mut header = vec![0_u8; block_size as usize];
    device.read_blocks(header_lba, &mut header)?;
    if header.get(0..8) != Some(b"EFI PART") {
        return Err("EFI PART signature is missing".into());
    }

    let revision = read_u32_le(&header, 8)?;
    let header_size = read_u32_le(&header, 12)?;
    if revision != 0x0001_0000 {
        return Err(format!("unsupported GPT revision 0x{revision:08x}").into());
    }
    if !(92..=block_size).contains(&header_size) {
        return Err(format!("header size {header_size} is outside 92..={block_size}").into());
    }
    if read_u32_le(&header, 20)? != 0 {
        return Err("reserved header field is non-zero".into());
    }
    if header[56..72].iter().all(|byte| *byte == 0) {
        return Err("disk GUID is zero".into());
    }
    let stored_header_crc = read_u32_le(&header, 16)?;
    let mut crc_header = header[..header_size as usize].to_vec();
    crc_header[16..20].fill(0);
    let computed_header_crc = crc32_ieee(&crc_header);

    let current_lba = read_u64_le(&header, 24)?;
    let alternate_lba = read_u64_le(&header, 32)?;
    let first_usable = read_u64_le(&header, 40)?;
    let last_usable = read_u64_le(&header, 48)?;
    let entries_lba = read_u64_le(&header, 72)?;
    let entry_count = read_u32_le(&header, 80)?;
    let entry_size = read_u32_le(&header, 84)?;
    let stored_entries_crc = read_u32_le(&header, 88)?;

    println!("  Revision: 0x{revision:08x}");
    println!("  Header size: {header_size}");
    println!("  Header LBA fields: current={current_lba}, alternate={alternate_lba}");
    println!("  Usable LBA range: {first_usable}..={last_usable}");
    println!("  Disk GUID: {:02x?}", &header[56..72]);
    println!("  Entry array: LBA={entries_lba}, count={entry_count}, entry_size={entry_size}");
    println!(
        "  Header CRC: stored={stored_header_crc:08x}, computed={computed_header_crc:08x} ({})",
        if stored_header_crc == computed_header_crc {
            "OK"
        } else {
            "MISMATCH"
        }
    );

    let expected_alternate = if header_lba == 1 { block_count - 1 } else { 1 };
    if current_lba != header_lba || alternate_lba != expected_alternate {
        return Err(
            "header current/alternate LBA fields do not match the physical copy locations".into(),
        );
    }
    if first_usable < 2 || first_usable > last_usable || last_usable >= block_count - 1 {
        return Err("usable LBA range is outside the device or invalid".into());
    }
    if entry_count == 0 || entry_count > 4096 || entry_size < 128 || entry_size % 8 != 0 {
        return Err("partition-entry geometry is invalid or exceeds safety limits".into());
    }
    let entries_bytes = u64::from(entry_count)
        .checked_mul(u64::from(entry_size))
        .ok_or("partition-entry byte length overflow")?;
    if entries_bytes > 16 * 1024 * 1024 {
        return Err("partition-entry array exceeds 16 MiB safety limit".into());
    }
    let entries_blocks = entries_bytes
        .checked_add(u64::from(block_size) - 1)
        .ok_or("partition-entry block count overflow")?
        / u64::from(block_size);
    let entries_end = entries_lba
        .checked_add(entries_blocks)
        .ok_or("partition-entry range overflow")?;
    if entries_lba == 0 || entries_end > block_count {
        return Err("partition-entry array lies outside the device".into());
    }
    if (header_lba == 1 && (entries_lba < 2 || entries_end > first_usable))
        || (header_lba != 1 && (entries_lba <= last_usable || entries_end != header_lba))
    {
        return Err(
            "partition-entry array overlaps usable space or is outside its expected GPT region"
                .into(),
        );
    }
    let padded_bytes = usize::try_from(
        entries_blocks
            .checked_mul(u64::from(block_size))
            .ok_or("padded partition-entry byte length overflow")?,
    )?;
    let mut entries = vec![0_u8; padded_bytes];
    device.read_blocks(entries_lba, &mut entries)?;
    let entries = &entries[..usize::try_from(entries_bytes)?];
    let actual_entries_crc = crc32_ieee(entries);
    println!("  Entry-array CRC: stored={stored_entries_crc:08x}, computed={actual_entries_crc:08x} ({})",
        if stored_entries_crc == actual_entries_crc { "OK" } else { "MISMATCH" });

    if stored_header_crc != computed_header_crc {
        return Err("header CRC mismatch".into());
    }
    if stored_entries_crc != actual_entries_crc {
        return Err("partition-entry array CRC mismatch".into());
    }

    let partitions =
        validate_gpt_partitions(entries, entry_count, entry_size, first_usable, last_usable)?;
    println!(
        "  Semantic partition entries: {} populated, valid",
        partitions.len()
    );
    if partitions.is_empty() {
        println!("  GPT partition table state: EMPTY (all partition entries are unused)");
    } else {
        println!("  GPT partition table state: POPULATED");
    }
    for (index, first, last, name) in &partitions {
        println!("    Partition {index}: LBA {first}..={last}, name={name:?}");
        match probe_partition_filesystem(device, block_size, *first, *last) {
            Ok(signature) => println!(
                "      Filesystem signature: {signature} (signature probe only; not an integrity check)"
            ),
            Err(error) => println!("      Filesystem signature probe: FAILED ({error})"),
        }
    }
    Ok((
        partitions,
        entries.to_vec(),
        header[56..72].try_into()?,
        first_usable,
        last_usable,
    ))
}

/// Read at most the first 2 KiB of a validated GPT partition, without writing.
fn probe_partition_filesystem(
    device: &mut ReadOnlyRawDevice,
    block_size: u32,
    first_lba: u64,
    last_lba: u64,
) -> Result<&'static str, Box<dyn StdError>> {
    let partition_blocks = last_lba
        .checked_sub(first_lba)
        .and_then(|length| length.checked_add(1))
        .ok_or("invalid partition range during filesystem probe")?;
    let required_bytes = 2048_u64;
    let required_blocks = required_bytes
        .checked_add(u64::from(block_size) - 1)
        .ok_or("filesystem probe block count overflow")?
        / u64::from(block_size);
    let blocks_to_read = partition_blocks.min(required_blocks);
    let buffer_bytes = blocks_to_read
        .checked_mul(u64::from(block_size))
        .ok_or("filesystem probe buffer size overflow")?;
    let mut buffer = vec![0_u8; usize::try_from(buffer_bytes)?];
    device.read_blocks(first_lba, &mut buffer)?;
    Ok(identify_filesystem_signature(&buffer))
}

/// Identify a few well-known on-disk signatures; this is not full filesystem validation.
fn identify_filesystem_signature(bytes: &[u8]) -> &'static str {
    let boot_signature = bytes.get(510..512) == Some(&[0x55, 0xaa]);
    if boot_signature && bytes.get(3..11) == Some(b"NTFS    ") {
        return "NTFS";
    }
    if boot_signature && bytes.get(3..11) == Some(b"EXFAT   ") {
        return "exFAT";
    }
    if boot_signature && bytes.get(54..62) == Some(b"FAT12   ") {
        return "FAT12";
    }
    if boot_signature && bytes.get(54..62) == Some(b"FAT16   ") {
        return "FAT16";
    }
    if boot_signature && bytes.get(82..90) == Some(b"FAT32   ") {
        return "FAT32";
    }
    if bytes.get(1080..1082) == Some(&[0x53, 0xef]) {
        return "ext2/3/4";
    }
    "unknown / unsupported signature"
}

/// Perform conservative structural checks on a FAT32 BIOS Parameter Block.
/// This is not a complete FAT32 consistency check and never writes to the device.
fn validate_fat32_boot_sector(
    bytes: &[u8],
    device_block_size: u32,
    remaining_device_blocks: u64,
) -> Result<(), &'static str> {
    if bytes.len() < 512 || bytes.get(510..512) != Some(&[0x55, 0xaa]) {
        return Err("missing 512-byte boot-sector signature");
    }
    let bytes_per_sector = u16::from_le_bytes([bytes[11], bytes[12]]);
    if !matches!(bytes_per_sector, 512 | 1024 | 2048 | 4096) {
        return Err("invalid bytes-per-sector field");
    }
    if u32::from(bytes_per_sector) != device_block_size {
        return Err("BPB sector size differs from device logical sector size");
    }
    if bytes.len() < usize::from(bytes_per_sector) {
        return Err("boot-sector buffer is shorter than the BPB sector size");
    }

    let sectors_per_cluster = bytes[13];
    if sectors_per_cluster == 0 || !sectors_per_cluster.is_power_of_two() {
        return Err("sectors-per-cluster is not a non-zero power of two");
    }
    let reserved_sectors = u16::from_le_bytes([bytes[14], bytes[15]]);
    let fat_count = bytes[16];
    let root_entries = u16::from_le_bytes([bytes[17], bytes[18]]);
    let total_sectors_16 = u16::from_le_bytes([bytes[19], bytes[20]]);
    let media = bytes[21];
    let fat_size_16 = u16::from_le_bytes([bytes[22], bytes[23]]);
    let total_sectors_32 = u32::from_le_bytes(bytes[32..36].try_into().unwrap());
    let fat_size_32 = u32::from_le_bytes(bytes[36..40].try_into().unwrap());
    let ext_flags = u16::from_le_bytes([bytes[40], bytes[41]]);
    let fs_version = u16::from_le_bytes([bytes[42], bytes[43]]);
    let root_cluster = u32::from_le_bytes(bytes[44..48].try_into().unwrap());
    let fsinfo_sector = u16::from_le_bytes([bytes[48], bytes[49]]);
    let backup_boot_sector = u16::from_le_bytes([bytes[50], bytes[51]]);

    if reserved_sectors == 0 || !matches!(fat_count, 1 | 2) {
        return Err("invalid reserved-sector or FAT-count field");
    }
    if root_entries != 0 || total_sectors_16 != 0 || fat_size_16 != 0 {
        return Err("FAT32 legacy BPB fields are inconsistent");
    }
    if total_sectors_32 == 0 || fat_size_32 == 0 {
        return Err("FAT32 total sectors or FAT size is zero");
    }
    if !matches!(media, 0xf0 | 0xf8..=0xff) {
        return Err("FAT32 media descriptor is invalid");
    }
    if fs_version != 0 {
        return Err("unsupported FAT32 filesystem version");
    }
    // Bits 4..6 and 8..15 are reserved. When mirroring is disabled, bits 0..3
    // select the active FAT and must name an existing copy.
    if ext_flags & 0xff70 != 0
        || (ext_flags & 0x0080 != 0 && u32::from(ext_flags & 0x000f) >= u32::from(fat_count))
    {
        return Err("FAT32 extended flags contain reserved bits or an invalid active FAT");
    }
    if fsinfo_sector != 0xffff && fsinfo_sector >= reserved_sectors {
        return Err("FSInfo sector lies outside reserved region");
    }
    if backup_boot_sector != 0xffff && backup_boot_sector >= reserved_sectors {
        return Err("backup boot sector lies outside reserved region");
    }

    let fat_area_sectors = u64::from(fat_count)
        .checked_mul(u64::from(fat_size_32))
        .ok_or("FAT32 FAT-area size overflows")?;
    let first_data_sector = u64::from(reserved_sectors)
        .checked_add(fat_area_sectors)
        .ok_or("FAT32 first data sector overflows")?;
    let total_sectors = u64::from(total_sectors_32);
    if first_data_sector >= total_sectors {
        return Err("FAT32 metadata consumes the entire volume");
    }
    let data_sectors = total_sectors - first_data_sector;
    let cluster_count = data_sectors / u64::from(sectors_per_cluster);

    // FAT32 is identified by its data-cluster count, not by the text label or
    // the BPB field layout alone. Also keep cluster IDs below the reserved range.
    if !(65_525..=0x0fff_ffee).contains(&cluster_count) {
        return Err("data-cluster count is outside the FAT32 range");
    }
    if u64::from(root_cluster) < 2 || u64::from(root_cluster) > cluster_count + 1 {
        return Err("FAT32 root cluster lies outside the data-cluster range");
    }

    let fat_bytes = u64::from(fat_size_32)
        .checked_mul(u64::from(bytes_per_sector))
        .ok_or("FAT32 FAT byte length overflows")?;
    let fat_entries = fat_bytes / 4;
    if fat_entries < cluster_count + 2 {
        return Err("FAT32 allocation table cannot address every data cluster");
    }

    let volume_bytes = total_sectors
        .checked_mul(u64::from(bytes_per_sector))
        .ok_or("FAT32 volume byte length overflows")?;
    let available_bytes = remaining_device_blocks
        .checked_mul(u64::from(device_block_size))
        .ok_or("remaining device byte length overflows")?;
    if volume_bytes > available_bytes {
        return Err("FAT32 volume extends beyond the device");
    }
    Ok(())
}

/// Build a small, deterministic set of plausible volume-start offsets.
/// This is deliberately not a whole-device scan: only the first/last 16 MiB
/// and the GPT first-usable LBA are sampled, at MiB boundaries.
fn unpartitioned_probe_lbas(block_size: u32, block_count: u64) -> Vec<u64> {
    if block_size == 0 || block_count < 3 {
        return Vec::new();
    }
    let blocks_per_mib = (1024 * 1024_u64).div_ceil(u64::from(block_size));
    let mut candidates = vec![34_u64];
    for mib in [1_u64, 2, 4, 8, 16] {
        let offset = blocks_per_mib.saturating_mul(mib);
        candidates.push(offset);
        if let Some(tail) = block_count.checked_sub(offset) {
            candidates.push(tail);
        }
    }
    candidates.sort_unstable();
    candidates.dedup();
    candidates.retain(|lba| *lba >= 2 && *lba < block_count - 1);
    candidates
}

/// Sample at most eleven 2-KiB regions from an empty-GPT device, read-only.
/// A match is only a signature candidate, never proof of a recoverable volume.
fn probe_unpartitioned_filesystem_candidates(
    device: &mut ReadOnlyRawDevice,
    block_size: u32,
    block_count: u64,
) -> Result<(), Box<dyn StdError>> {
    let candidates = unpartitioned_probe_lbas(block_size, block_count);
    println!("Bounded unpartitioned-space signature probe:");
    println!(
        "  Sampling {} candidate LBAs (not a full-device scan)",
        candidates.len()
    );
    let blocks_to_read = 2048_u64.div_ceil(u64::from(block_size)).max(1);
    let buffer_len = usize::try_from(blocks_to_read * u64::from(block_size))?;
    let mut buffer = vec![0_u8; buffer_len];
    let mut matches = 0_usize;
    for lba in &candidates {
        buffer.fill(0);
        device.read_blocks(*lba, &mut buffer)?;
        let signature = identify_filesystem_signature(&buffer);
        if signature == "FAT32" {
            let remaining_blocks = block_count.saturating_sub(*lba);
            match validate_fat32_boot_sector(&buffer, block_size, remaining_blocks) {
                Ok(()) => {
                    matches += 1;
                    println!("  Candidate at LBA {lba}: FAT32 BPB structurally plausible (still unverified)");
                }
                Err(reason) => println!(
                    "  Candidate at LBA {lba}: FAT32 text signature only; BPB rejected ({reason})"
                ),
            }
        } else if signature != "unknown / unsupported signature" {
            matches += 1;
            println!("  Candidate at LBA {lba}: {signature} (unverified signature)");
        }
    }
    if matches == 0 {
        println!("  No recognized filesystem signatures at sampled offsets.");
    }
    println!(
        "  This bounded probe can miss volumes at other offsets and may produce false positives."
    );
    Ok(())
}

fn inspect_raw_gpt(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<(), Box<dyn StdError>> {
    let report = inspect_raw_gpt_report(path, block_size, block_count)?;
    println!("Read-only GPT inspection");
    println!("Device: {}", report.device);
    println!("Capacity: {} bytes", report.capacity_bytes);
    println!("Logical block size: {} bytes", report.block_size);
    println!("Logical block count: {}", report.block_count);
    println!("Protective MBR: valid (0xEE, starts at LBA 1, covers device)");
    println!("Primary GPT header CRC: OK");
    println!("Backup GPT header CRC: OK");
    println!("Primary/backup GPT metadata: consistent");
    println!("Backup partition array CRC: OK");
    println!("Partition array CRC: OK");
    println!("Protective MBR: OK");
    println!("Backup GPT header LBA: {}", report.backup_header_lba);
    println!(
        "Usable LBA range: {}..={}",
        report.first_usable_lba, report.last_usable_lba
    );
    for (index, first, last, name) in &report.partitions {
        println!("Partition {index}: LBA {first}..={last} ({name})");
    }
    println!("Populated GPT entries: {}", report.partitions.len());
    println!("No writes were performed.");
    Ok(())
}

fn inspect_raw_gpt_json(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<(), Box<dyn StdError>> {
    let report = inspect_raw_gpt_report(path, block_size, block_count)?;
    println!("{{");
    println!("  \"status\": \"ok\",");
    println!("  \"read_only\": true,");
    println!("  \"device\": \"{}\",", json_escape(&report.device));
    println!("  \"block_size\": {},", report.block_size);
    println!("  \"block_count\": {},", report.block_count);
    println!("  \"capacity_bytes\": {},", report.capacity_bytes);
    println!("  \"backup_header_lba\": {},", report.backup_header_lba);
    println!("  \"first_usable_lba\": {},", report.first_usable_lba);
    println!("  \"last_usable_lba\": {},", report.last_usable_lba);
    println!("  \"partitions\": [");
    for (position, (index, first, last, name)) in report.partitions.iter().enumerate() {
        let comma = if position + 1 == report.partitions.len() {
            ""
        } else {
            ","
        };
        println!(
            "    {{\"index\": {index}, \"first_lba\": {first}, \"last_lba\": {last}, \"name\": \"{}\"}}{comma}",
            json_escape(name)
        );
    }
    println!("  ],");
    println!("  \"writes_performed\": false");
    println!("}}");
    Ok(())
}

fn json_escape(value: &str) -> String {
    let mut escaped = String::with_capacity(value.len());
    for character in value.chars() {
        match character {
            '"' => escaped.push_str("\\\""),
            '\\' => escaped.push_str("\\\\"),
            '\n' => escaped.push_str("\\n"),
            '\r' => escaped.push_str("\\r"),
            '\t' => escaped.push_str("\\t"),
            character if character <= '\u{001f}' => {
                use std::fmt::Write as _;
                let _ = write!(escaped, "\\u{:04x}", character as u32);
            }
            character => escaped.push(character),
        }
    }
    escaped
}

fn validate_protective_mbr(mbr: &[u8], block_count: u64) -> Result<(), Box<dyn StdError>> {
    if block_count < 3 || mbr.len() < 512 {
        return Err("device is too small for a protective MBR".into());
    }
    if mbr.get(510..512) != Some(&[0x55, 0xaa]) {
        return Err("protective MBR signature 0x55AA is missing".into());
    }

    let expected_sectors = (block_count - 1).min(u64::from(u32::MAX)) as u32;
    let mut protective_entries = 0_u32;
    for index in 0..4 {
        let offset = 446 + index * 16;
        let entry = &mbr[offset..offset + 16];
        let partition_type = entry[4];
        if partition_type == 0 {
            continue;
        }
        if partition_type != 0xEE {
            return Err("hybrid or non-protective MBR entries are not accepted".into());
        }
        protective_entries += 1;
        let start_lba = u32::from_le_bytes(entry[8..12].try_into()?);
        let sector_count = u32::from_le_bytes(entry[12..16].try_into()?);
        if entry[0] != 0 {
            return Err(format!(
                "protective MBR entry has invalid boot indicator 0x{:02x}; expected 0x00",
                entry[0]
            )
            .into());
        }
        if start_lba != 1 {
            return Err(
                format!("protective MBR entry starts at LBA {start_lba}; expected LBA 1").into(),
            );
        }
        if sector_count != expected_sectors {
            return Err(format!(
                "protective MBR coverage mismatch: observed {sector_count} sectors, expected {expected_sectors} for {block_count} logical blocks (0xFFFFFFFF saturation is valid only when the device exceeds the 32-bit LBA range)"
            )
            .into());
        }
    }
    if protective_entries != 1 {
        return Err("MBR must contain exactly one protective 0xEE partition".into());
    }
    Ok(())
}

type GptPartition = (u32, u64, u64, String);

struct GptInspection {
    device: String,
    block_size: u32,
    block_count: u64,
    capacity_bytes: u64,
    backup_header_lba: u64,
    first_usable_lba: u64,
    last_usable_lba: u64,
    partitions: Vec<GptPartition>,
}

fn validate_gpt_partitions(
    entries: &[u8],
    entry_count: u32,
    entry_size: u32,
    first_usable: u64,
    last_usable: u64,
) -> Result<Vec<GptPartition>, Box<dyn StdError>> {
    if entry_size < 128 || entry_size % 8 != 0 {
        return Err("GPT partition-entry size is invalid".into());
    }
    let expected_len = u64::from(entry_count)
        .checked_mul(u64::from(entry_size))
        .ok_or("GPT partition-entry length overflow")?;
    if usize::try_from(expected_len)? != entries.len() {
        return Err("GPT partition-entry array length does not match its geometry".into());
    }

    let mut partition_guids = HashSet::new();
    let mut ranges = Vec::new();
    let mut partitions = Vec::new();
    for index in 0..entry_count {
        let start = usize::try_from(u64::from(index) * u64::from(entry_size))?;
        let entry = &entries[start..start + entry_size as usize];
        if entry[..16].iter().all(|byte| *byte == 0) {
            if entry.iter().any(|byte| *byte != 0) {
                return Err(
                    format!("unused GPT entry {} contains non-zero data", index + 1).into(),
                );
            }
            continue;
        }
        if entry[16..32].iter().all(|byte| *byte == 0) {
            return Err(format!("GPT partition entry {} has a zero unique GUID", index + 1).into());
        }
        let unique_guid: [u8; 16] = entry[16..32].try_into()?;
        if !partition_guids.insert(unique_guid) {
            return Err(
                format!("GPT partition entry {} duplicates a unique GUID", index + 1).into(),
            );
        }

        let first = u64::from_le_bytes(entry[32..40].try_into()?);
        let last = u64::from_le_bytes(entry[40..48].try_into()?);
        if first > last || first < first_usable || last > last_usable {
            return Err(
                format!("GPT partition entry {} has an invalid LBA range", index + 1).into(),
            );
        }
        let name = decode_gpt_name(&entry[56..entry.len().min(128)]);
        ranges.push((first, last, index + 1));
        partitions.push((index + 1, first, last, name));
    }

    ranges.sort_unstable_by_key(|range| range.0);
    for pair in ranges.windows(2) {
        if pair[1].0 <= pair[0].1 {
            return Err(format!("GPT partitions {} and {} overlap", pair[0].2, pair[1].2).into());
        }
    }
    Ok(partitions)
}

fn read_u32_le(bytes: &[u8], offset: usize) -> Result<u32, Box<dyn StdError>> {
    Ok(u32::from_le_bytes(
        bytes
            .get(offset..offset + 4)
            .ok_or("truncated GPT field")?
            .try_into()?,
    ))
}

fn read_u64_le(bytes: &[u8], offset: usize) -> Result<u64, Box<dyn StdError>> {
    Ok(u64::from_le_bytes(
        bytes
            .get(offset..offset + 8)
            .ok_or("truncated GPT field")?
            .try_into()?,
    ))
}

fn crc32_ieee(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

fn crc32_ieee_update(mut crc: u32, bytes: &[u8]) -> u32 {
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = if crc & 1 == 1 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    crc
}

/// Read every logical block with a bounded 4 MiB buffer and return a whole-device CRC32.
fn scan_read_only_device(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<(u32, u64), Box<dyn StdError>> {
    if block_count == 0 {
        return Err("device must contain at least one block".into());
    }
    let mut device = ReadOnlyRawDevice::open(path, block_size, block_count)?;
    let target_chunk_bytes = 4 * 1024 * 1024_u64;
    let chunk_blocks = (target_chunk_bytes / u64::from(block_size)).max(1);
    let chunk_bytes = usize::try_from(chunk_blocks * u64::from(block_size))?;
    let mut buffer = vec![0_u8; chunk_bytes];
    let mut crc = !0_u32;
    let mut next_block = 0_u64;
    while next_block < block_count {
        let blocks = (block_count - next_block).min(chunk_blocks);
        let bytes = usize::try_from(blocks * u64::from(block_size))?;
        device.read_blocks(next_block, &mut buffer[..bytes])?;
        crc = crc32_ieee_update(crc, &buffer[..bytes]);
        next_block = next_block
            .checked_add(blocks)
            .ok_or("device scan block offset overflow")?;
    }
    let total_bytes = u64::from(block_size)
        .checked_mul(block_count)
        .ok_or("device scan byte count overflow")?;
    Ok((!crc, total_bytes))
}

fn decode_gpt_name(bytes: &[u8]) -> String {
    let units = bytes
        .chunks_exact(2)
        .take(36)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]));
    let decoded = char::decode_utf16(units)
        .take_while(|value| !matches!(value, Ok('\0')))
        .map(|value| value.unwrap_or(char::REPLACEMENT_CHARACTER));
    decoded.collect::<String>()
}

fn probe_read_only_device(
    path: &Path,
    block_size: u32,
    block_count: u64,
) -> Result<(), Box<dyn StdError>> {
    let mut device = ReadOnlyRawDevice::open(path, block_size, block_count)?;
    let mut first = vec![0_u8; block_size as usize];
    let mut last = vec![0_u8; block_size as usize];
    device.read_blocks(0, &mut first)?;
    device.read_blocks(block_count - 1, &mut last)?;
    println!("Read-only device probe: OK");
    println!("Logical block size: {} bytes", device.block_size());
    println!("Block count: {}", device.block_count());
    println!(
        "Capacity: {} bytes",
        u64::from(device.block_size()) * device.block_count()
    );
    println!("First block prefix: {:02x?}", &first[..first.len().min(16)]);
    println!("Last block prefix: {:02x?}", &last[..last.len().min(16)]);
    println!("No writes were performed.");
    Ok(())
}

fn print_usage() {
    eprintln!("Usage:");
    eprintln!("  openfs list-devices  (read-only physical disk inventory; Windows only)");
    eprintln!("  openfs create-image <path> <size-MiB>  (formats and initializes a mount-ready image)");
    eprintln!("  openfs mount <image> <drive-letter> [block-size]  (Windows + Dokany)");
    eprintln!("  openfs unmount <drive-letter>  (Windows + Dokany)");
    eprintln!("  openfs probe-device <path> <block-size> <block-count>  (read-only)");
    eprintln!(
        "  openfs inspect-device <path> <block-size> <block-count>  (read-only GPT validation)"
    );
    eprintln!(
        "  openfs inspect-device-json <path> <block-size> <block-count>  (read-only GPT validation as JSON)"
    );
    eprintln!("  openfs diagnose-device <path> <block-size> <block-count>  (read-only MBR/GPT semantic diagnostics)");
    eprintln!("  openfs scan-device <path> <block-size> <block-count>  (read-only full-device CRC32 scan)");
    eprintln!("  openfs inspect-image <path>");
    eprintln!("  openfs check-image <path> <block-size>");
    eprintln!("  openfs audit-image <path> <block-size>");
    eprintln!("  openfs recover-image <path> <block-size>");
    eprintln!("  openfs init-namespace <path> <block-size>");
    eprintln!("  openfs mkdir <image> <block-size> <path>");
    eprintln!("  openfs mkdir-p <image> <block-size> <path>  (create missing parent directories)");
    eprintln!("  openfs touch <image> <block-size> <path>");
    eprintln!("  openfs ls <image> <block-size> [path]");
    eprintln!("  openfs tree <image> <block-size> [path]");
    eprintln!("  openfs find <image> <block-size> <root-path> <substring>");
    eprintln!("  openfs du <image> <block-size> [path]  (recursive logical and allocated usage)");
    eprintln!("  openfs space <image> <block-size>");
    eprintln!("  openfs cat <image> <block-size> <file-path>");
    eprintln!("  openfs checksum <image> <block-size> <file-path> [expected-crc32c-hex]");
    eprintln!("  openfs verify-file <image> <block-size> <file-path> <expected-crc32c-hex>");
    eprintln!("  checksum with an expected CRC32C exits with an error on mismatch.");
    eprintln!("  openfs read-range <image> <block-size> <file-path> <offset> <length>");
    eprintln!("  openfs checksum-range <image> <block-size> <file-path> <offset> <length> [expected-crc32c-hex]");
    eprintln!("  openfs copy <image> <block-size> <source-file-path> <destination-file-path>");
    eprintln!("  copy streams through a temporary host file; destination must already exist.");
    eprintln!("  openfs export <image> <block-size> <file-path> <host-output-path>");
    eprintln!("  openfs import <image> <block-size> <host-input-path> <file-path>");
    eprintln!("  openfs stat <image> <block-size> <path>");
    eprintln!("  openfs write <image> <block-size> <file-path> <text...>");
    eprintln!("  openfs write-range <image> <block-size> <file-path> <offset> <text...>");
    eprintln!("  openfs write-stdin <image> <block-size> <file-path>  (replace file from stdin)");
    eprintln!("  openfs append <image> <block-size> <file-path> <text...>");
    eprintln!("  openfs truncate <image> <block-size> <file-path> <new-size-bytes>");
    eprintln!("  openfs mv <image> <block-size> <source-path> <destination-path>");
    eprintln!("  openfs rm <image> <block-size> <path>");
    eprintln!(
        "check-image is read-only and accepts block sizes 4096, 8192, 16384, 32768, or 65536."
    );
    eprintln!("recover-image may modify an image while replaying the journal; back it up first.");
    eprintln!("create-image refuses to overwrite an existing file.");
}

#[cfg(test)]
mod tests {
    use super::{audit_image, create_image, inspect_image, ObjectKind, VolumeSession, BLOCK_SIZE};
    use openfs_block::{BlockDevice, ImageDevice};
    use openfs_format::{format_volume, open_journal, open_volume, RedoUpdate};
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::time::{SystemTime, UNIX_EPOCH};

    struct FailWriteAt<D> {
        inner: D,
        writes: usize,
        fail_at: usize,
    }

    impl<D: BlockDevice> BlockDevice for FailWriteAt<D> {
        fn block_size(&self) -> u32 {
            self.inner.block_size()
        }

        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }

        fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> openfs_core::Result<()> {
            self.inner.read_blocks(first_block, output)
        }

        fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> openfs_core::Result<()> {
            let operation = self.writes;
            self.writes += 1;
            if operation == self.fail_at {
                return Err(std::io::Error::other("injected home-write failure").into());
            }
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    impl<D> FailWriteAt<D> {
        fn into_inner(self) -> D {
            self.inner
        }
    }

    fn unique_path(label: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "openfs-tool-{label}-{}-{nonce}.img",
            std::process::id()
        ))
    }

    #[test]
    fn touch_is_idempotent_for_files_and_rejects_directories() {
        let path = unique_path("touch-idempotent");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
            device.flush().unwrap();
        }
        super::initialize_namespace_image(&path, BLOCK_SIZE).unwrap();
        super::touch_namespace_file(&path, BLOCK_SIZE, "/note.txt").unwrap();
        super::write_namespace_file(&path, BLOCK_SIZE, "/note.txt", b"preserve me").unwrap();
        super::touch_namespace_file(&path, BLOCK_SIZE, "/note.txt").unwrap();
        {
            let mut device = ImageDevice::open(&path, BLOCK_SIZE).unwrap();
            let volume = open_volume(&mut device).unwrap();
            assert_eq!(
                volume
                    .read_persistent_path_range(&mut device, "/note.txt", 0, 64)
                    .unwrap(),
                b"preserve me"
            );
        }
        super::create_namespace_entry(&path, BLOCK_SIZE, "/folder", true).unwrap();
        assert!(super::touch_namespace_file(&path, BLOCK_SIZE, "/folder").is_err());
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn mkdir_p_creates_nested_directories_idempotently_and_rejects_file_components() {
        let path = unique_path("mkdir-p");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
            device.flush().unwrap();
        }
        super::initialize_namespace_image(&path, BLOCK_SIZE).unwrap();

        super::create_namespace_directories(&path, BLOCK_SIZE, "/alpha/beta/gamma").unwrap();
        super::create_namespace_directories(&path, BLOCK_SIZE, "alpha/beta/gamma/").unwrap();
        {
            let mut device = ImageDevice::open(&path, BLOCK_SIZE).unwrap();
            let volume = open_volume(&mut device).unwrap();
            let (_, namespace) = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(
                namespace
                    .object(namespace.resolve_path("/alpha/beta/gamma").unwrap())
                    .unwrap()
                    .kind,
                super::ObjectKind::Directory
            );
        }
        super::create_namespace_entry(&path, BLOCK_SIZE, "/alpha/regular-file", false).unwrap();
        assert!(super::create_namespace_directories(
            &path,
            BLOCK_SIZE,
            "/alpha/regular-file/child"
        )
        .is_err());
        assert!(
            super::create_namespace_directories(&path, BLOCK_SIZE, "/alpha/../escape").is_err()
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn scan_device_reads_all_blocks_with_streaming_crc32() {
        let path = unique_path("scan-device");
        {
            let mut device = ImageDevice::create(&path, 512, 19).unwrap();
            let mut contents = vec![0_u8; 512 * 19];
            for (index, byte) in contents.iter_mut().enumerate() {
                *byte = (index.wrapping_mul(37) & 0xff) as u8;
            }
            device.write_blocks(0, &contents).unwrap();
            device.flush().unwrap();
            let expected = super::crc32_ieee(&contents);
            // Release ImageDevice's exclusive lock before opening the same fixture
            // through the read-only scan path. Windows enforces the byte-range lock.
            drop(device);
            let (actual, bytes) = super::scan_read_only_device(&path, 512, 19).unwrap();
            assert_eq!(actual, expected);
            assert_eq!(bytes, 512 * 19);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn find_rejects_empty_search_text_before_opening_an_image() {
        assert!(super::find_namespace_entries(
            Path::new("image-does-not-need-to-exist.img"),
            BLOCK_SIZE,
            "/",
            "",
        )
        .is_err());
    }

    #[test]
    fn audit_refuses_pending_journal_state_before_recovery_capable_open() {
        assert!(super::ensure_audit_journal_clean(1, 0).is_err());
        assert!(super::ensure_audit_journal_clean(0, 1).is_err());
        assert!(super::ensure_audit_journal_clean(0, 0).is_ok());
    }

    #[test]
    fn audit_refuses_pending_redo_without_modifying_the_image() {
        let path = unique_path("audit-pending-redo");
        let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
        let volume = format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
        let region = volume.volume.layout.journal;
        let target = volume.volume.layout.data_start;
        drop(volume);

        let mut journal = open_journal(&mut device, region).unwrap();
        let redo = RedoUpdate {
            target_block: target,
            contents: vec![0xA5; BLOCK_SIZE as usize],
        };
        // BEGIN + two UPDATE chunks + COMMIT are writes 0..=3. Fail the
        // following home-block write so the committed redo remains pending.
        let mut failing = FailWriteAt {
            inner: device,
            writes: 0,
            fail_at: 4,
        };
        assert!(journal.commit(&mut failing, &[redo]).is_err());
        drop(journal);
        let device = failing.into_inner();
        drop(device);

        let before = fs::read(&path).unwrap();
        assert!(
            audit_image(&path, BLOCK_SIZE).is_err(),
            "audit must reject a committed transaction that still needs replay"
        );
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "refused audit must not replay redo or write a checkpoint"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn check_image_rejects_corrupt_journal_without_modifying_the_image() {
        let path = unique_path("check-corrupt-journal");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
            let region = volume.volume.layout.journal;
            drop(volume);

            // Corrupt both checkpoint copies. Arbitrary bytes in an unused ring slot
            // are intentionally ignored, so the corruption must invalidate journal
            // state that the read-only checker actually consumes.
            let corrupt_checkpoint = vec![0xA5; BLOCK_SIZE as usize];
            device
                .write_blocks(region.checkpoint_a, &corrupt_checkpoint)
                .unwrap();
            device
                .write_blocks(region.checkpoint_b, &corrupt_checkpoint)
                .unwrap();
            device.flush().unwrap();
        }

        let before = fs::read(&path).unwrap();
        assert!(
            super::check_image(&path, BLOCK_SIZE).is_err(),
            "check-image must reject malformed journal records"
        );
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "a failed read-only check must not alter a corrupt image"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn check_image_uses_valid_checkpoint_when_other_is_corrupt_without_modifying_image() {
        for block_size in [4096, 8192, 16_384, 32_768, 65_536] {
            let path = unique_path(&format!("check-one-corrupt-checkpoint-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let volume = format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
                let checkpoint_a = volume.volume.layout.journal.checkpoint_a;
                drop(volume);

                // A single damaged checkpoint copy is recoverable from its valid peer.
                let corrupt_checkpoint = vec![0xA5; block_size as usize];
                device
                    .write_blocks(checkpoint_a, &corrupt_checkpoint)
                    .unwrap();
                device.flush().unwrap();
            }

            let before = fs::read(&path).unwrap();
            assert!(
                super::check_image(&path, block_size).is_ok(),
                "check-image should accept one valid checkpoint copy at block size {block_size}"
            );
            assert_eq!(
                fs::read(&path).unwrap(),
                before,
                "read-only checking must not repair the damaged checkpoint at block size {block_size}"
            );
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn check_image_reports_pending_redo_without_modifying_the_image() {
        let path = unique_path("check-pending-redo");
        let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
        let volume = format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
        let region = volume.volume.layout.journal;
        let target = volume.volume.layout.data_start;
        drop(volume);

        let mut journal = open_journal(&mut device, region).unwrap();
        let redo = RedoUpdate {
            target_block: target,
            contents: vec![0x5A; BLOCK_SIZE as usize],
        };
        let mut failing = FailWriteAt {
            inner: device,
            writes: 0,
            fail_at: 4,
        };
        assert!(journal.commit(&mut failing, &[redo]).is_err());
        drop(journal);
        drop(failing.into_inner());

        let before = fs::read(&path).unwrap();
        assert!(
            super::check_image(&path, BLOCK_SIZE).is_ok(),
            "read-only check should report a pending transaction without recovering it"
        );
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "check-image must not replay redo or publish a checkpoint"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn audit_does_not_rewrite_a_damaged_checkpoint_when_peer_is_valid() {
        for block_size in [4096, 8192, 16_384, 32_768, 65_536] {
            let path = unique_path(&format!("audit-one-corrupt-checkpoint-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let volume = format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
                let checkpoint_a = volume.volume.layout.journal.checkpoint_a;
                drop(volume);

                // Audit must remain read-only even if opening the journal could choose
                // the other valid checkpoint copy.
                let corrupt_checkpoint = vec![0xA5; block_size as usize];
                device
                    .write_blocks(checkpoint_a, &corrupt_checkpoint)
                    .unwrap();
                device.flush().unwrap();
            }

            let before = fs::read(&path).unwrap();
            assert!(
                audit_image(&path, block_size).is_err(),
                "audit should report the absent inventory at block size {block_size}"
            );
            assert_eq!(
                fs::read(&path).unwrap(),
                before,
                "audit must not rewrite a damaged checkpoint at block size {block_size}"
            );
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn audit_of_clean_volume_without_inventory_does_not_modify_image() {
        let path = unique_path("audit-no-inventory");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
            device.flush().unwrap();
        }

        let before = fs::read(&path).unwrap();
        let result = audit_image(&path, BLOCK_SIZE);
        assert!(
            result.is_err(),
            "audit should report that the persistent inventory is absent"
        );
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "an unsuccessful audit must not mutate a clean image"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn creates_and_reopens_a_valid_development_image() {
        let path = unique_path("roundtrip");
        let result = create_image(&path, 1);
        assert!(result.is_ok(), "image creation failed: {result:?}");
        assert!(inspect_image(&path).is_ok(), "created image did not reopen");

        {
            let mut session =
                VolumeSession::open(ImageDevice::open(&path, BLOCK_SIZE).unwrap()).unwrap();
            let root = session.stat("/").unwrap();
            assert_eq!(root.kind, ObjectKind::Directory);
            session.create_file_auto("/", "roundtrip.txt").unwrap();
            session
                .write_file("/roundtrip.txt", b"OpenFS survives an unmount/remount")
                .unwrap();
            session.flush().unwrap();
        }

        {
            let mut session =
                VolumeSession::open(ImageDevice::open(&path, BLOCK_SIZE).unwrap()).unwrap();
            assert_eq!(
                session.read_file("/roundtrip.txt").unwrap(),
                b"OpenFS survives an unmount/remount"
            );
            session.flush().unwrap();
        }

        fs::remove_file(path).unwrap();
    }

    #[test]
    fn allocation_state_survives_closing_and_reopening_the_image() {
        let path = unique_path("allocator");
        create_image(&path, 1).unwrap();

        let first = {
            let mut device = ImageDevice::open(&path, BLOCK_SIZE).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            volume.allocate_block(&mut device).unwrap()
        };
        let second = {
            let mut device = ImageDevice::open(&path, BLOCK_SIZE).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            let next = volume.allocate_block(&mut device).unwrap();
            assert_ne!(next, first);
            volume.free_block(&mut device, first).unwrap();
            let reused = volume.allocate_block(&mut device).unwrap();
            assert_eq!(reused, first);
            next
        };
        assert_ne!(second, first);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn checks_current_image_without_modifying_it() {
        for block_size in [4096, 8192, 16_384, 32_768, 65_536] {
            let path = unique_path(&format!("current-check-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
                device.flush().unwrap();
            }
            let before = fs::read(&path).unwrap();
            assert!(
                super::check_image(&path, block_size).is_ok(),
                "check-image should accept a valid image at block size {block_size}"
            );
            assert_eq!(
                fs::read(&path).unwrap(),
                before,
                "check-image must not modify a valid image at block size {block_size}"
            );
            fs::remove_file(path).unwrap();
        }

        let invalid_path = unique_path("current-check-invalid-block-size");
        assert!(super::check_image(&invalid_path, 512).is_err());
        assert!(super::audit_image(&invalid_path, 512).is_err());
        assert!(super::recover_image(&invalid_path, 512).is_err());
        assert!(super::initialize_namespace_image(&invalid_path, 512).is_err());
        assert!(
            !invalid_path.exists(),
            "invalid sizes must not create an image"
        );
    }

    #[test]
    fn refuses_to_overwrite_existing_file_and_preserves_contents() {
        let path = unique_path("no-overwrite");
        let original = b"preserve this unrelated file";
        fs::write(&path, original).unwrap();

        assert!(create_image(&path, 1).is_err());
        assert_eq!(fs::read(&path).unwrap(), original);

        fs::remove_file(path).unwrap();
    }

    #[test]
    fn rejects_zero_and_excessive_image_sizes_before_creating_files() {
        let zero_path = unique_path("zero");
        let huge_path = unique_path("huge");

        assert!(create_image(&zero_path, 0).is_err());
        assert!(create_image(&huge_path, u64::MAX).is_err());
        assert!(!zero_path.exists() && !huge_path.exists());
        assert!(super::parse_block_size(Some("512".into())).is_err());
    }

    #[test]
    fn audit_image_accepts_valid_persistent_inventory_at_every_block_size() {
        for block_size in [4096, 8192, 16_384, 32_768, 65_536] {
            let path = unique_path(&format!("audit-valid-inventory-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
                device.flush().unwrap();
            }

            super::initialize_namespace_image(&path, block_size).unwrap();

            // Exercise the user-facing persistent file workflow as one vertical slice:
            // create namespace entries, write, append, truncate, close/reopen, then audit.
            super::create_namespace_entry(&path, block_size, "/docs", true).unwrap();
            super::create_namespace_entry(&path, block_size, "/docs/note.txt", false).unwrap();
            let original_payload = b"persistent file payload";
            super::write_namespace_file(&path, block_size, "/docs/note.txt", original_payload)
                .unwrap();
            super::append_namespace_file(&path, block_size, "/docs/note.txt", b" + appended")
                .unwrap();
            super::truncate_namespace_file(
                &path,
                block_size,
                "/docs/note.txt",
                original_payload.len() as u64,
            )
            .unwrap();

            super::create_namespace_entry(&path, block_size, "/docs/copy.txt", false).unwrap();
            super::copy_namespace_file(&path, block_size, "/docs/note.txt", "/docs/copy.txt")
                .unwrap();
            {
                let mut copy_device = ImageDevice::open(&path, block_size).unwrap();
                let copy_volume = open_volume(&mut copy_device).unwrap();
                assert_eq!(
                    copy_volume
                        .read_persistent_path_range(&mut copy_device, "/docs/copy.txt", 0, 64,)
                        .unwrap(),
                    original_payload,
                    "copy must preserve file bytes at block size {block_size}"
                );
            }
            // Include persistent rename and removal in the same cross-block-size workflow.
            super::create_namespace_entry(&path, block_size, "/docs/archive", true).unwrap();
            super::create_namespace_entry(&path, block_size, "/docs/discard.txt", false).unwrap();
            super::write_namespace_file(&path, block_size, "/docs/discard.txt", b"remove me")
                .unwrap();
            super::move_namespace_entry(
                &path,
                block_size,
                "/docs/note.txt",
                "/docs/archive/note.txt",
            )
            .unwrap();
            super::remove_namespace_entry(&path, block_size, "/docs/discard.txt").unwrap();

            {
                let mut reopened_device = ImageDevice::open(&path, block_size).unwrap();
                let reopened_volume = open_volume(&mut reopened_device).unwrap();
                assert_eq!(
                    reopened_volume
                        .read_persistent_path_range(
                            &mut reopened_device,
                            "/docs/archive/note.txt",
                            0,
                            64,
                        )
                        .unwrap(),
                    original_payload,
                    "create/write/append/truncate/move must survive reopen at block size {block_size}"
                );
                assert!(
                    reopened_volume
                        .read_persistent_path_range(&mut reopened_device, "/docs/note.txt", 0, 64,)
                        .is_err(),
                    "renamed source path must not remain published at block size {block_size}"
                );
                assert!(
                    reopened_volume
                        .read_persistent_path_range(
                            &mut reopened_device,
                            "/docs/discard.txt",
                            0,
                            64,
                        )
                        .is_err(),
                    "removed file must remain absent after reopen at block size {block_size}"
                );
            }
            assert!(
                super::report_namespace_usage(&path, block_size, "/docs").is_ok(),
                "recursive namespace usage should accept the persistent tree at block size {block_size}"
            );
            assert!(
                super::check_image(&path, block_size).is_ok(),
                "read-only check should accept the persistent file workflow at block size {block_size}"
            );
            let before_audit = fs::read(&path).unwrap();
            assert!(
                super::audit_image(&path, block_size).is_ok(),
                "audit-image should accept a valid persistent inventory at block size {block_size}"
            );
            assert_eq!(
                fs::read(&path).unwrap(),
                before_audit,
                "successful audit must not modify a valid image at block size {block_size}"
            );

            fs::remove_file(path).unwrap();
        }
    }
}

/// Stream a file to a CRC32C sink without buffering the entire file in memory.
fn read_or_checksum_namespace_file(
    command: &str,
    path: &Path,
    block_size: u32,
    target: &str,
    expected_crc32c: Option<u32>,
) -> Result<(), Box<dyn StdError>> {
    if command == "cat" {
        // The dedicated reader owns the image handle; do not retain a second
        // exclusive writer lock while it opens the same image.
        return read_namespace_file(path, block_size, target);
    }

    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    let mut checksum = Crc32cWriter::default();
    let bytes = volume.stream_persistent_file_to_writer(&mut device, target, &mut checksum)?;
    let actual = checksum.finish();
    println!("File: {target}");
    println!("Size: {bytes} bytes");
    println!("CRC32C: {actual:08x}");
    if let Some(expected) = expected_crc32c {
        if actual != expected {
            println!("Checksum verification: FAILED (expected {expected:08x})");
            return Err("CRC32C checksum mismatch".into());
        }
        println!("Checksum verification: OK");
    }
    Ok(())
}

/// Validate whether a command accepts or requires an expected CRC32C value.
fn run_checksum_argument_policy(
    command: &str,
    expected: Option<u32>,
) -> Result<(), Box<dyn StdError>> {
    if (command == "cat" && expected.is_some()) || (command == "verify-file" && expected.is_none())
    {
        return Err("cat rejects checksums; verify-file requires an expected CRC32C".into());
    }
    Ok(())
}

/// Parse a CRC32C value as one to eight hexadecimal digits, optionally prefixed by 0x.
fn parse_crc32c(value: &str) -> Result<u32, Box<dyn StdError>> {
    let digits = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
        .unwrap_or(value);
    if digits.is_empty() || digits.len() > 8 || !digits.bytes().all(|byte| byte.is_ascii_hexdigit())
    {
        return Err(
            "expected CRC32C as 1-8 hexadecimal digits (optionally prefixed with 0x)".into(),
        );
    }
    Ok(u32::from_str_radix(digits, 16)?)
}

/// Incremental CRC32C writer. This is for accidental-corruption detection,
/// not cryptographic authentication.
#[derive(Default)]
struct Crc32cWriter {
    state: u32,
}

impl Crc32cWriter {
    fn finish(&self) -> u32 {
        self.state
    }
}

impl std::io::Write for Crc32cWriter {
    fn write(&mut self, buffer: &[u8]) -> std::io::Result<usize> {
        let mut crc = !self.state;
        for byte in buffer {
            crc ^= u32::from(*byte);
            for _ in 0..8 {
                crc = if crc & 1 == 1 {
                    (crc >> 1) ^ 0x82f6_3b78
                } else {
                    crc >> 1
                };
            }
        }
        self.state = !crc;
        Ok(buffer.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

#[cfg(test)]
mod checksum_tests {
    use super::Crc32cWriter;
    use std::io::Write;

    #[test]
    fn verify_file_requires_expected_crc32c_and_cat_rejects_one() {
        assert!(super::run_checksum_argument_policy("verify-file", None).is_err());
        assert!(super::run_checksum_argument_policy("verify-file", Some(0)).is_ok());
        assert!(super::run_checksum_argument_policy("cat", Some(0)).is_err());
        assert!(super::run_checksum_argument_policy("cat", None).is_ok());
    }

    #[test]
    fn crc32c_matches_the_standard_check_value() {
        let mut writer = Crc32cWriter::default();
        writer.write_all(b"123456789").unwrap();
        assert_eq!(writer.finish(), 0xe306_9283);
    }

    #[test]
    fn crc32c_of_empty_input_is_zero() {
        assert_eq!(Crc32cWriter::default().finish(), 0);
    }

    #[test]
    fn parses_crc32c_with_optional_prefix_and_rejects_malformed_values() {
        assert_eq!(super::parse_crc32c("e3069283").unwrap(), 0xe306_9283);
        assert_eq!(super::parse_crc32c("0xE3069283").unwrap(), 0xe306_9283);
        assert_eq!(super::parse_crc32c("f").unwrap(), 15);
        assert!(super::parse_crc32c("").is_err());
        assert!(super::parse_crc32c("0x").is_err());
        assert!(super::parse_crc32c("123456789").is_err());
        assert!(super::parse_crc32c("zzzz").is_err());
    }
}

#[cfg(test)]
mod gpt_tests {
    use super::{
        crc32_ieee, identify_filesystem_signature, read_u32_le, read_u64_le,
        unpartitioned_probe_lbas, validate_fat32_boot_sector, validate_gpt_partitions,
        validate_protective_mbr,
    };

    fn protective_mbr(block_count: u64) -> Vec<u8> {
        let mut mbr = vec![0_u8; 512];
        mbr[446 + 4] = 0xEE;
        mbr[446 + 8..446 + 12].copy_from_slice(&1_u32.to_le_bytes());
        let sectors = (block_count - 1).min(u64::from(u32::MAX)) as u32;
        mbr[446 + 12..446 + 16].copy_from_slice(&sectors.to_le_bytes());
        mbr[510..512].copy_from_slice(&[0x55, 0xAA]);
        mbr
    }

    fn partition_entry(unique_guid: u8, first: u64, last: u64) -> [u8; 128] {
        let mut entry = [0_u8; 128];
        entry[..16].copy_from_slice(&[0xA5; 16]);
        entry[16..32].copy_from_slice(&[unique_guid; 16]);
        entry[32..40].copy_from_slice(&first.to_le_bytes());
        entry[40..48].copy_from_slice(&last.to_le_bytes());
        entry
    }

    #[test]
    fn ieee_crc32_matches_the_standard_check_value() {
        assert_eq!(crc32_ieee(b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32_ieee(b""), 0);
    }

    #[test]
    fn little_endian_gpt_fields_are_bounds_checked() {
        assert_eq!(
            read_u32_le(&[0x78, 0x56, 0x34, 0x12], 0).unwrap(),
            0x1234_5678
        );
        assert_eq!(
            read_u64_le(&[0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01], 0).unwrap(),
            0x0102_0304_0506_0708
        );
        assert!(read_u32_le(&[0, 1, 2], 0).is_err());
        assert!(read_u64_le(&[0; 7], 0).is_err());
    }

    #[test]
    fn accepts_a_standard_protective_mbr_and_rejects_bad_signature() {
        let mbr = protective_mbr(4096);
        assert!(validate_protective_mbr(&mbr, 4096).is_ok());

        let mut corrupt = mbr.clone();
        corrupt[510] = 0;
        assert!(validate_protective_mbr(&corrupt, 4096).is_err());
    }

    #[test]
    fn protective_mbr_coverage_saturates_only_above_the_32_bit_lba_limit() {
        let under_limit = protective_mbr(240_253_440);
        assert!(validate_protective_mbr(&under_limit, 240_253_440).is_ok());

        let mut overclaimed = under_limit.clone();
        overclaimed[446 + 12..446 + 16].copy_from_slice(&u32::MAX.to_le_bytes());
        let error = validate_protective_mbr(&overclaimed, 240_253_440).unwrap_err();
        assert!(
            error
                .to_string()
                .contains("observed 4294967295 sectors, expected 240253439"),
            "under-limit saturation must report the exact mismatch: {error}"
        );
        assert!(
            error
                .to_string()
                .contains("device exceeds the 32-bit LBA range"),
            "diagnostic must explain when 0xFFFFFFFF is valid: {error}"
        );

        let above_limit = protective_mbr(u64::from(u32::MAX) + 100);
        assert!(validate_protective_mbr(&above_limit, u64::from(u32::MAX) + 100).is_ok());
    }

    #[test]
    fn rejects_missing_incomplete_and_hybrid_protective_mbr_entries() {
        let mut mbr = protective_mbr(4096);
        mbr[446 + 8..446 + 12].copy_from_slice(&2_u32.to_le_bytes());
        assert!(validate_protective_mbr(&mbr, 4096).is_err());

        let mut mbr = protective_mbr(4096);
        mbr[446 + 12..446 + 16].copy_from_slice(&100_u32.to_le_bytes());
        assert!(validate_protective_mbr(&mbr, 4096).is_err());

        let mut mbr = protective_mbr(4096);
        mbr[462 + 4] = 0x07;
        assert!(validate_protective_mbr(&mbr, 4096).is_err());
    }

    #[test]
    fn recognizes_ntfs_exfat_and_ext_superblock_signatures() {
        let mut ntfs = vec![0_u8; 2048];
        ntfs[3..11].copy_from_slice(b"NTFS    ");
        ntfs[510..512].copy_from_slice(&[0x55, 0xaa]);
        assert_eq!(identify_filesystem_signature(&ntfs), "NTFS");

        let mut exfat = vec![0_u8; 2048];
        exfat[3..11].copy_from_slice(b"EXFAT   ");
        exfat[510..512].copy_from_slice(&[0x55, 0xaa]);
        assert_eq!(identify_filesystem_signature(&exfat), "exFAT");

        let mut ext = vec![0_u8; 2048];
        ext[1080..1082].copy_from_slice(&[0x53, 0xef]);
        assert_eq!(identify_filesystem_signature(&ext), "ext2/3/4");

        assert_eq!(
            identify_filesystem_signature(&[0_u8; 2048]),
            "unknown / unsupported signature"
        );
    }

    fn plausible_fat32_boot_sector() -> Vec<u8> {
        let mut boot = vec![0_u8; 2048];
        boot[82..90].copy_from_slice(b"FAT32   ");
        boot[510..512].copy_from_slice(&[0x55, 0xaa]);
        boot[11..13].copy_from_slice(&512_u16.to_le_bytes());
        boot[13] = 8;
        boot[14..16].copy_from_slice(&32_u16.to_le_bytes());
        boot[16] = 2;
        boot[21] = 0xf8;
        boot[32..36].copy_from_slice(&600_000_u32.to_le_bytes());
        boot[36..40].copy_from_slice(&6_000_u32.to_le_bytes());
        boot[44..48].copy_from_slice(&2_u32.to_le_bytes());
        boot[48..50].copy_from_slice(&1_u16.to_le_bytes());
        boot[50..52].copy_from_slice(&6_u16.to_le_bytes());
        boot
    }

    #[test]
    fn fat32_probe_rejects_signature_only_and_accepts_spec_sized_bpb() {
        let mut boot = plausible_fat32_boot_sector();
        boot[11..13].fill(0);
        assert!(validate_fat32_boot_sector(&boot, 512, 700_000).is_err());

        boot = plausible_fat32_boot_sector();
        assert!(validate_fat32_boot_sector(&boot, 512, 700_000).is_ok());
        assert!(validate_fat32_boot_sector(&boot, 4096, 700_000).is_err());
        assert!(validate_fat32_boot_sector(&boot, 512, 100).is_err());
    }

    #[test]
    fn fat32_probe_rejects_non_fat32_cluster_count_and_undersized_fat() {
        let mut boot = plausible_fat32_boot_sector();
        boot[32..36].copy_from_slice(&8_000_u32.to_le_bytes());
        boot[36..40].copy_from_slice(&100_u32.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 10_000),
            Err("data-cluster count is outside the FAT32 range")
        );

        boot = plausible_fat32_boot_sector();
        boot[36..40].copy_from_slice(&100_u32.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 700_000),
            Err("FAT32 allocation table cannot address every data cluster")
        );
    }

    #[test]
    fn fat32_probe_rejects_invalid_root_cluster_media_and_version() {
        let mut boot = plausible_fat32_boot_sector();
        boot[44..48].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 700_000),
            Err("FAT32 root cluster lies outside the data-cluster range")
        );

        boot = plausible_fat32_boot_sector();
        boot[21] = 0xf7;
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 700_000),
            Err("FAT32 media descriptor is invalid")
        );

        boot = plausible_fat32_boot_sector();
        boot[42..44].copy_from_slice(&1_u16.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 700_000),
            Err("unsupported FAT32 filesystem version")
        );
    }

    #[test]
    fn fat32_probe_validates_active_fat_and_boot_sector_buffer_length() {
        let mut boot = plausible_fat32_boot_sector();
        boot[40..42].copy_from_slice(&0x0082_u16.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 512, 700_000),
            Err("FAT32 extended flags contain reserved bits or an invalid active FAT")
        );

        boot = plausible_fat32_boot_sector();
        assert_eq!(
            validate_fat32_boot_sector(&boot[..512], 4096, 700_000),
            Err("BPB sector size differs from device logical sector size")
        );

        boot = plausible_fat32_boot_sector();
        boot[11..13].copy_from_slice(&4096_u16.to_le_bytes());
        assert_eq!(
            validate_fat32_boot_sector(&boot, 4096, 700_000),
            Err("boot-sector buffer is shorter than the BPB sector size")
        );
    }

    #[test]
    fn unpartitioned_probe_offsets_are_bounded_deduplicated_and_in_range() {
        let offsets = unpartitioned_probe_lbas(512, 240_253_440);
        assert!(offsets.contains(&34));
        assert!(offsets.contains(&2048));
        assert!(offsets.contains(&(240_253_440 - 2048)));
        assert!(offsets.iter().all(|lba| *lba >= 2 && *lba < 240_253_439));
        assert!(offsets.windows(2).all(|pair| pair[0] < pair[1]));
        assert!(offsets.len() <= 11);
    }

    #[test]
    fn unpartitioned_probe_rejects_invalid_geometry() {
        assert!(unpartitioned_probe_lbas(0, 100).is_empty());
        assert!(unpartitioned_probe_lbas(512, 2).is_empty());
    }

    #[test]
    fn accepts_a_well_formed_empty_gpt_partition_array() {
        let entries = vec![0_u8; 128 * 128];
        let partitions = validate_gpt_partitions(&entries, 128, 128, 34, 400).unwrap();
        assert!(partitions.is_empty());
    }

    #[test]
    fn accepts_adjacent_partitions_and_rejects_overlapping_ranges() {
        let first = partition_entry(1, 34, 100);
        let second = partition_entry(2, 101, 200);
        let mut entries = Vec::from(first);
        entries.extend_from_slice(&second);
        assert_eq!(
            validate_gpt_partitions(&entries, 2, 128, 34, 400)
                .unwrap()
                .len(),
            2
        );

        let overlapping = partition_entry(2, 100, 200);
        entries[128..256].copy_from_slice(&overlapping);
        assert!(validate_gpt_partitions(&entries, 2, 128, 34, 400).is_err());
    }

    #[test]
    fn rejects_zero_or_duplicate_partition_guids_and_invalid_ranges() {
        let mut zero_guid = partition_entry(1, 34, 100);
        zero_guid[16..32].fill(0);
        assert!(validate_gpt_partitions(&zero_guid, 1, 128, 34, 400).is_err());

        let entry = partition_entry(1, 34, 100);
        let mut duplicate = Vec::from(entry);
        duplicate.extend_from_slice(&entry);
        assert!(validate_gpt_partitions(&duplicate, 2, 128, 34, 400).is_err());

        let reversed = partition_entry(2, 100, 34);
        assert!(validate_gpt_partitions(&reversed, 1, 128, 34, 400).is_err());

        let out_of_bounds = partition_entry(3, 34, 401);
        assert!(validate_gpt_partitions(&out_of_bounds, 1, 128, 34, 400).is_err());
    }

    #[test]
    fn rejects_nonzero_data_in_unused_entries_and_mismatched_array_lengths() {
        let mut unused = [0_u8; 128];
        unused[127] = 1;
        assert!(validate_gpt_partitions(&unused, 1, 128, 34, 400).is_err());
        assert!(validate_gpt_partitions(&[], 1, 128, 34, 400).is_err());
    }

    fn valid_gpt_image(label: &str) -> std::path::PathBuf {
        valid_gpt_image_with_block_size(label, 512)
    }

    fn valid_gpt_image_with_block_size(label: &str, block_size: usize) -> std::path::PathBuf {
        use std::fs;
        use std::time::{SystemTime, UNIX_EPOCH};

        const BLOCK_COUNT: usize = 100;
        const ENTRY_BYTES: usize = 4 * 128;

        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "openfs-gpt-{label}-{}-{nonce}.img",
            std::process::id()
        ));

        let mut image = vec![0_u8; block_size * BLOCK_COUNT];
        let mbr = protective_mbr(BLOCK_COUNT as u64);
        image[..512].copy_from_slice(&mbr);

        let mut entries = vec![0_u8; ENTRY_BYTES];
        entries[..128].copy_from_slice(&partition_entry(1, 3, 10));
        let entries_crc = crc32_ieee(&entries);
        image[2 * block_size..2 * block_size + ENTRY_BYTES].copy_from_slice(&entries);
        image[98 * block_size..98 * block_size + ENTRY_BYTES].copy_from_slice(&entries);

        for (header_lba, alternate_lba, entries_lba) in [(1_usize, 99_u64, 2_u64), (99, 1, 98)] {
            let start = header_lba * block_size;
            let header = &mut image[start..start + block_size];
            header[..8].copy_from_slice(b"EFI PART");
            header[8..12].copy_from_slice(&0x0001_0000_u32.to_le_bytes());
            header[12..16].copy_from_slice(&92_u32.to_le_bytes());
            header[24..32].copy_from_slice(&(header_lba as u64).to_le_bytes());
            header[32..40].copy_from_slice(&alternate_lba.to_le_bytes());
            header[40..48].copy_from_slice(&3_u64.to_le_bytes());
            header[48..56].copy_from_slice(&97_u64.to_le_bytes());
            header[56..72].copy_from_slice(&[0x5A; 16]);
            header[72..80].copy_from_slice(&entries_lba.to_le_bytes());
            header[80..84].copy_from_slice(&4_u32.to_le_bytes());
            header[84..88].copy_from_slice(&128_u32.to_le_bytes());
            header[88..92].copy_from_slice(&entries_crc.to_le_bytes());
            let header_crc = crc32_ieee(&header[..92]);
            header[16..20].copy_from_slice(&header_crc.to_le_bytes());
        }

        fs::write(&path, image).unwrap();
        path
    }

    #[test]
    fn inspect_report_contains_validated_device_geometry_and_partitions() {
        use std::fs;

        let path = valid_gpt_image("report");
        let report = super::inspect_raw_gpt_report(&path, 512, 100).unwrap();
        assert_eq!(report.block_size, 512);
        assert_eq!(report.block_count, 100);
        assert_eq!(report.capacity_bytes, 51_200);
        assert_eq!(report.backup_header_lba, 99);
        assert_eq!(report.first_usable_lba, 3);
        assert_eq!(report.last_usable_lba, 97);
        assert_eq!(report.partitions.len(), 1);
        assert_eq!(report.partitions[0].0, 1);
        assert_eq!(report.partitions[0].1, 3);
        assert_eq!(report.partitions[0].2, 10);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn json_escape_handles_quotes_slashes_and_control_characters() {
        assert_eq!(super::json_escape("quote\""), "quote\\\"");
        assert_eq!(super::json_escape("slash\\"), "slash\\\\");
        assert_eq!(
            super::json_escape("line\nnext\tend\r"),
            "line\\nnext\\tend\\r"
        );
        assert_eq!(super::json_escape("\u{0001}"), "\\u0001");
    }

    #[test]
    fn inspect_accepts_consistent_primary_and_backup_gpt_without_writing() {
        use std::fs;

        let path = valid_gpt_image("valid");
        let before = fs::read(&path).unwrap();
        assert!(super::inspect_raw_gpt(&path, 512, 100).is_ok());
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "GPT inspection must never modify the source device or image"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn inspect_accepts_4k_logical_sector_gpt_without_writing() {
        use std::fs;

        let path = valid_gpt_image_with_block_size("valid-4k", 4096);
        let before = fs::read(&path).unwrap();
        assert!(super::inspect_raw_gpt(&path, 4096, 100).is_ok());
        assert_eq!(
            fs::read(&path).unwrap(),
            before,
            "4K-sector GPT inspection must not modify the source image"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn inspect_rejects_primary_header_crc_corruption() {
        use std::fs;

        let path = valid_gpt_image("primary-header-crc");
        let mut image = fs::read(&path).unwrap();
        image[512 + 40] ^= 0x01;
        fs::write(&path, image).unwrap();

        assert!(super::inspect_raw_gpt(&path, 512, 100)
            .unwrap_err()
            .to_string()
            .contains("primary GPT header CRC mismatch"));
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn inspect_rejects_backup_partition_array_corruption() {
        use std::fs;

        let path = valid_gpt_image("backup-array");
        let mut image = fs::read(&path).unwrap();
        image[98 * 512 + 17] ^= 0x80;
        fs::write(&path, image).unwrap();

        assert!(super::inspect_raw_gpt(&path, 512, 100)
            .unwrap_err()
            .to_string()
            .contains("backup GPT partition-entry array is corrupt"));
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn inspect_rejects_backup_header_crc_corruption() {
        use std::fs;

        let path = valid_gpt_image("backup-header-crc");
        let mut image = fs::read(&path).unwrap();
        image[99 * 512 + 48] ^= 0x01;
        fs::write(&path, image).unwrap();

        assert!(super::inspect_raw_gpt(&path, 512, 100)
            .unwrap_err()
            .to_string()
            .contains("backup GPT header CRC mismatch"));
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn gpt_copy_diagnostics_are_independent_of_an_invalid_protective_mbr() {
        use std::fs;

        let path = valid_gpt_image("independent-copy-diagnostic");
        let mut image = fs::read(&path).unwrap();
        image[446] = 0x80; // Invalid boot indicator; leave both GPT copies intact.
        fs::write(&path, image).unwrap();

        let mbr = {
            let bytes = fs::read(&path).unwrap();
            bytes[..512].to_vec()
        };
        assert!(super::validate_protective_mbr(&mbr, 100).is_err());

        let mut device = super::ReadOnlyRawDevice::open(&path, 512, 100).unwrap();
        assert!(super::diagnose_gpt_copy(&mut device, 512, 100, 1, "PRIMARY").is_ok());
        assert!(super::diagnose_gpt_copy(&mut device, 512, 100, 99, "BACKUP").is_ok());
        drop(device);

        fs::remove_file(path).unwrap();
    }
}
