use openfs_block::{BlockDevice, ImageDevice};
use openfs_format::Superblock;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_ID: AtomicU64 = AtomicU64::new(0);

struct TempImage(PathBuf);

impl TempImage {
    fn new() -> Self {
        let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
        let path =
            std::env::temp_dir().join(format!("openfs-superblock-{}-{id}.img", std::process::id()));
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
fn encoded_superblock_survives_image_write_flush_and_reopen() {
    let image = TempImage::new();
    let expected = Superblock {
        block_size: 4096,
        total_blocks: 16,
        features: 0,
        uuid: *b"openfs-test-uuid",
    };
    {
        let mut device = ImageDevice::create(&image.0, 4096, 16).unwrap();
        let encoded = expected.encode().unwrap();
        device.write_blocks(0, &encoded).unwrap();
        device.flush().unwrap();
    }

    let mut device = ImageDevice::open(&image.0, 4096).unwrap();
    let mut bytes = vec![0; 4096];
    device.read_blocks(0, &mut bytes).unwrap();
    assert_eq!(Superblock::decode(&bytes).unwrap(), expected);
}
