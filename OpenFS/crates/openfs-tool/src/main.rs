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
use openfs_format::{check_volume_v2, format_volume, open_volume};

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

/// Read-only V2 consistency check. Unlike opening a writable volume, this never replays
/// pending journal records and never changes the image.
fn check_image(path: &Path, block_size: u32) -> Result<(), Box<dyn StdError>> {
    if ![4096, 8192, 16_384, 32_768, 65_536].contains(&block_size) {
        return Err("unsupported block size; choose 4096, 8192, 16384, 32768, or 65536".into());
    }
    let mut device = ImageDevice::open(path, block_size)?;
    let report = check_volume_v2(&mut device)?;
    println!("OpenFS V2 volume check passed (read-only; no recovery performed).");
    println!("Path: {}", path.display());
    println!("Block size: {} bytes", report.block_size);
    println!("Total blocks: {}", report.total_blocks);
    println!("Data blocks: {}", report.data_blocks);
    println!("Allocated data blocks: {}", report.allocated_data_blocks);
    println!("Free data blocks: {}", report.free_data_blocks);
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
    println!("The check validates superblocks, geometry, bitmap invariants, and journal framing.");
    println!("It does not validate file ownership or namespace integrity.");
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
    eprintln!(
        "check-image is read-only and accepts block sizes 4096, 8192, 16384, 32768, or 65536."
    );
    eprintln!("create-image refuses to overwrite an existing file.");
}

#[cfg(test)]
mod tests {
    use super::{create_image, inspect_image, BLOCK_SIZE};
    use openfs_block::{BlockDevice, ImageDevice};
    use openfs_format::{format_volume_v2, open_volume};
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
            let volume = open_volume(&mut device).unwrap();
            volume.allocate_block(&mut device).unwrap()
        };
        let second = {
            let mut device = ImageDevice::open(&path, BLOCK_SIZE).unwrap();
            let volume = open_volume(&mut device).unwrap();
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
    fn checks_v2_image_without_modifying_it() {
        let path = unique_path("v2-check");
        {
            let mut device = ImageDevice::create(&path, BLOCK_SIZE, 256).unwrap();
            format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
