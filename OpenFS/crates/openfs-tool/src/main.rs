//! Safe-first host utility for OpenFS development images.
//!
//! This tool deliberately has no raw-device formatting command. Physical-device
//! support must wait for a journal-integrated on-disk format and stronger recovery
//! guarantees; image creation never truncates or overwrites an existing path.

use std::env;
use std::error::Error as StdError;
use std::fs::{self, OpenOptions};
use std::io::Write;
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
            let block_size = args
                .next()
                .ok_or("missing block size (4096, 8192, 16384, 32768, or 65536)")?
                .parse::<u32>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            check_image(Path::new(&path), block_size)
        }
        Some("recover-image") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = args
                .next()
                .ok_or("missing block size (4096, 8192, 16384, 32768, or 65536)")?
                .parse::<u32>()?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            recover_image(Path::new(&path), block_size)
        }
        Some("init-namespace") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = args
                .next()
                .ok_or("missing block size (4096, 8192, 16384, 32768, or 65536)")?
                .parse::<u32>()?;
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
            create_namespace_entry(Path::new(&path), block_size, &target, command == "mkdir")
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
        Some("cat") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing file path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            read_namespace_file(Path::new(&path), block_size, &target)
        }
        Some("export") => {
            let path = args.next().ok_or("missing image path")?;
            let block_size = parse_block_size(args.next())?;
            let target = args.next().ok_or("missing namespace file path")?;
            let output = args.next().ok_or("missing host output path")?;
            if args.next().is_some() {
                return Err("too many arguments".into());
            }
            export_namespace_file(
                Path::new(&path),
                block_size,
                &target,
                Path::new(&output),
            )
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
        println!("{kind}\t{}\t{}", entry.object_id, entry.name);
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
    let length = usize::try_from(record.size).map_err(|_| "file is too large for this host")?;
    let data = volume.read_persistent_path_range(&mut device, target, 0, length)?;
    std::io::Write::write_all(&mut std::io::stdout().lock(), &data)?;
    Ok(())
}

fn export_namespace_file(
    image_path: &Path,
    block_size: u32,
    target: &str,
    output_path: &Path,
) -> Result<(), Box<dyn StdError>> {
    const CHUNK_SIZE: usize = 1024 * 1024;

    let mut device = ImageDevice::open(image_path, block_size)?;
    let volume = open_volume(&mut device)?;
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

    // Never silently overwrite a host file during export.
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output_path)?;
    let result = (|| -> Result<(), Box<dyn StdError>> {
        let mut offset = 0_u64;
        while offset < record.size {
            let remaining = record.size - offset;
            let chunk_len = usize::try_from(remaining.min(CHUNK_SIZE as u64))?;
            let bytes = volume.read_persistent_path_range(
                &mut device,
                target,
                offset,
                chunk_len,
            )?;
            if bytes.len() != chunk_len {
                return Err("short read while exporting persistent file".into());
            }
            output.write_all(&bytes)?;
            offset = offset
                .checked_add(u64::try_from(bytes.len())?)
                .ok_or("export offset overflow")?;
        }
        output.flush()?;
        Ok(())
    })();

    if let Err(error) = result {
        drop(output);
        let _ = fs::remove_file(output_path);
        return Err(error);
    }
    println!(
        "Exported '{}' ({} bytes) to '{}'.",
        target,
        record.size,
        output_path.display()
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
    eprintln!("  openfs recover-image <path> <block-size>");
    eprintln!("  openfs init-namespace <path> <block-size>");
    eprintln!("  openfs mkdir <image> <block-size> <path>");
    eprintln!("  openfs touch <image> <block-size> <path>");
    eprintln!("  openfs ls <image> <block-size> [path]");
    eprintln!("  openfs cat <image> <block-size> <file-path>");
    eprintln!("  openfs export <image> <block-size> <file-path> <host-output-path>");
    eprintln!("  openfs stat <image> <block-size> <path>");
    eprintln!("  openfs write <image> <block-size> <file-path> <text...>");
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
    use super::{create_image, inspect_image, BLOCK_SIZE};
    use openfs_block::{BlockDevice, ImageDevice};
    use openfs_format::{format_volume, open_volume};
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

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
        let path = unique_path("current-check");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            format_volume(&mut device, *b"openfs-tool-test", 0).unwrap();
            device.flush().unwrap();
        }
        let before = fs::read(&path).unwrap();
        assert!(super::check_image(&path, BLOCK_SIZE).is_ok());
        assert_eq!(fs::read(&path).unwrap(), before);
        assert!(super::check_image(&path, 512).is_err());
        fs::remove_file(path).unwrap();
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
        assert!(!zero_path.exists());
        assert!(!huge_path.exists());
    }
}
