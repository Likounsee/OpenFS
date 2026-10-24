//! Safe-first host utility for OpenFS development images.
//!
//! This tool deliberately has no raw-device formatting command. Physical-device
//! support must wait for a journal-integrated on-disk format and stronger recovery
//! guarantees; image creation never truncates or overwrites an existing path.

use std::env;
use std::error::Error as StdError;
use std::fs::{self, OpenOptions};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use openfs_block::{BlockDevice, ImageDevice};
use openfs_format::{check_volume, format_volume, open_volume, ObjectId, ObjectKind};

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
    if let Err(error) = format_volume(&mut device, uuid, 0) {
        drop(device);
        let _ = fs::remove_file(path);
        return Err(error.into());
    }
    println!("OpenFS development image created.");
    println!("Path: {}", path.display());
    println!("Size: {size_mib} MiB ({block_count} blocks × {BLOCK_SIZE} bytes)");
    println!("UUID: {}", format_uuid(uuid));
    println!("Format: OpenFS journaled filesystem");
    println!("This command created a regular image file, not a physical USB volume.");
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
            println!("File '{}' already exists; left unchanged.", target);
            Ok(())
        }
        Err(_) => create_namespace_entry(path, block_size, target, false),
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

fn print_usage() {
    eprintln!("Usage:");
    eprintln!("  openfs create-image <path> <size-MiB>");
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
    use super::{audit_image, create_image, inspect_image, BLOCK_SIZE};
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
    let mut device = ImageDevice::open(path, block_size)?;
    let volume = open_volume(&mut device)?;
    if command == "cat" {
        // Keep plain output on the dedicated streaming reader path.
        return read_namespace_file(path, block_size, target);
    } else {
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
