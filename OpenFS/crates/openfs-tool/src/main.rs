//! Safe-first host utility for OpenFS development images.
//!
//! This tool deliberately has no raw-device formatting command. Physical-device
//! support must wait for a journal-integrated on-disk format and stronger recovery
//! guarantees; image creation never truncates or overwrites an existing path.

use std::env;
use std::error::Error as StdError;
use std::fs;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use openfs_block::ImageDevice;
use openfs_format::{format_volume, open_volume};

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
    println!("Format: v1 (development only; no persistent file namespace or integrated journal)");
    println!("This command created a regular image file, not a physical USB volume.");
    Ok(())
}

fn inspect_image(path: &Path) -> Result<(), Box<dyn StdError>> {
    let mut device = ImageDevice::open(path, BLOCK_SIZE)?;
    let volume = open_volume(&mut device)?;
    println!("OpenFS volume validated; no filesystem writes were performed.");
    println!("Path: {}", path.display());
    println!("Block size: {} bytes", volume.layout.block_size);
    println!("Total blocks: {}", volume.layout.total_blocks);
    println!(
        "Bitmap: blocks {}..{}",
        volume.layout.bitmap_start,
        volume.layout.bitmap_start + volume.layout.bitmap_blocks
    );
    println!(
        "Data region: blocks {}..{}",
        volume.layout.data_start, volume.layout.backup_superblock
    );
    println!("Data blocks: {}", volume.layout.data_blocks());
    println!("UUID: {}", format_uuid(volume.superblock.uuid));
    println!("Format: v1 (development only; no persistent file namespace or integrated journal)");
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
    eprintln!("create-image refuses to overwrite an existing file.");
}
