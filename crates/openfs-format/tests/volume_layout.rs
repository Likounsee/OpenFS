use openfs_block::{BlockDevice, ImageDevice};
use openfs_core::Error;
use openfs_format::{format_volume, open_volume, VolumeLayout};
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
static NEXT_ID: AtomicU64 = AtomicU64::new(0);
struct TempImage(PathBuf);
impl TempImage {
    fn new() -> Self {
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        let path =
            std::env::temp_dir().join(format!("openfs-volume-{}-{id}.img", std::process::id()));
        let _ = fs::remove_file(&path);
        Self(path)
    }
}
impl Drop for TempImage {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

#[test]
fn layout_reserves_mirrors_bitmap_and_data() {
    let layout = VolumeLayout::calculate(4096, 128).unwrap();
    assert_eq!(layout.bitmap_start, 1);
    assert_eq!(layout.bitmap_blocks, 1);
    assert_eq!(layout.data_start, 2);
    assert_eq!(layout.backup_superblock, 127);
    assert_eq!(layout.data_blocks(), 125);
    assert!(VolumeLayout::calculate(4096, 3).is_err());
}

#[test]
fn formats_reopens_and_checks_mirrored_superblocks_and_bitmap() {
    let image = TempImage::new();
    let uuid = *b"openfs-volume-01";
    {
        let mut device = ImageDevice::create(&image.0, 4096, 128).unwrap();
        let formatted = format_volume(&mut device, uuid, 0).unwrap();
        assert_eq!(formatted.layout.data_blocks(), 125);
    }
    let mut device = ImageDevice::open(&image.0, 4096).unwrap();
    let opened = open_volume(&mut device).unwrap();
    assert_eq!(opened.superblock.uuid, uuid);
    assert_eq!(opened.superblock.total_blocks, 128);
    assert_eq!(opened.layout.bitmap_blocks, 1);
    let mut bitmap = vec![0; 4096];
    device.read_blocks(1, &mut bitmap).unwrap();
    for reserved in [0_usize, 1, 127] {
        assert_ne!(bitmap[reserved / 8] & (1 << (reserved % 8)), 0);
    }
}

#[test]
fn rejects_disagreeing_backup_and_corrupt_reserved_bitmap_bits() {
    let image = TempImage::new();
    let mut device = ImageDevice::create(&image.0, 4096, 128).unwrap();
    format_volume(&mut device, *b"openfs-volume-01", 0).unwrap();
    let mut backup = vec![0; 4096];
    device.read_blocks(127, &mut backup).unwrap();
    backup[32] ^= 1;
    device.write_blocks(127, &backup).unwrap();
    device.flush().unwrap();
    assert!(matches!(open_volume(&mut device), Err(Error::Corrupt(_))));
    format_volume(&mut device, *b"openfs-volume-01", 0).unwrap();
    let mut bitmap = vec![0; 4096];
    device.read_blocks(1, &mut bitmap).unwrap();
    bitmap[0] &= !1;
    device.write_blocks(1, &bitmap).unwrap();
    device.flush().unwrap();
    assert!(matches!(open_volume(&mut device), Err(Error::Corrupt(_))));
}

#[test]
fn rejects_unformatted_images() {
    let image = TempImage::new();
    let mut device = ImageDevice::create(&image.0, 4096, 128).unwrap();
    assert!(matches!(open_volume(&mut device), Err(Error::Corrupt(_))));
}


#[test]
fn allocator_first_fit_release_and_reopen_persistence() {
    let image = TempImage::new();
    let mut device = ImageDevice::create(&image.0, 4096, 128).unwrap();
    format_volume(&mut device, *b"openfs-volume-01", 0).unwrap();
    let volume = open_volume(&mut device).unwrap();

    let first = volume.allocate_block(&mut device).unwrap();
    let second = volume.allocate_block(&mut device).unwrap();
    assert_eq!((first, second), (2, 3));
    volume.free_block(&mut device, first).unwrap();
    assert_eq!(volume.allocate_block(&mut device).unwrap(), first);

    device.flush().unwrap();
    drop(volume);
    let reopened = open_volume(&mut device).unwrap();
    assert_eq!(reopened.allocate_block(&mut device).unwrap(), 4);
}

#[test]
fn allocator_rejects_reserved_double_free_and_exhaustion() {
    let image = TempImage::new();
    let mut device = ImageDevice::create(&image.0, 4096, 8).unwrap();
    format_volume(&mut device, *b"openfs-volume-01", 0).unwrap();
    let volume = open_volume(&mut device).unwrap();

    assert!(matches!(volume.free_block(&mut device, 0), Err(Error::InvalidInput(_))));
    assert!(matches!(volume.free_block(&mut device, 2), Err(Error::InvalidInput(_))));
    let mut allocated = Vec::new();
    while let Ok(block) = volume.allocate_block(&mut device) {
        allocated.push(block);
    }
    assert_eq!(allocated, vec![2, 3, 4, 5, 6]);
    assert!(matches!(volume.allocate_block(&mut device), Err(Error::InvalidInput(_))));
    assert!(matches!(volume.free_block(&mut device, 3), Ok(())));
    assert!(matches!(volume.free_block(&mut device, 3), Err(Error::InvalidInput(_))));
}

#[test]
fn allocator_handles_multiple_bitmap_blocks_and_rejects_wrong_device() {
    let image = TempImage::new();
    let mut device = ImageDevice::create(&image.0, 4096, 40_000).unwrap();
    format_volume(&mut device, *b"openfs-volume-01", 0).unwrap();
    let volume = open_volume(&mut device).unwrap();
    assert_eq!(volume.layout.bitmap_blocks, 2);
    assert_eq!(volume.allocate_block(&mut device).unwrap(), volume.layout.data_start);

    let other_image = TempImage::new();
    let mut other = ImageDevice::create(&other_image.0, 4096, 64).unwrap();
    assert!(matches!(volume.allocate_block(&mut other), Err(Error::InvalidInput(_))));
}
