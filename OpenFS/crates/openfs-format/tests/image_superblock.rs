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

#[test]
fn superblock_matches_v1_golden_bytes_and_checksum() {
    let encoded = Superblock {
        block_size: 4096,
        total_blocks: 16,
        features: 0,
        uuid: *b"openfs-test-uuid",
    }
    .encode()
    .unwrap();

    // Golden bytes pin the v1 field offsets, little-endian encoding and CRC32C.
    assert_eq!(
        &encoded[..48],
        &[
            0x4f, 0x50, 0x45, 0x4e, 0x46, 0x53, 0x00, 0x00, // magic
            0x01, 0x00, 0x30, 0x00, // version and header size
            0x00, 0x10, 0x00, 0x00, // block size = 4096
            0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // total blocks = 16
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // features
            0x6f, 0x70, 0x65, 0x6e, 0x66, 0x73, 0x2d, 0x74, // UUID
            0x65, 0x73, 0x74, 0x2d, 0x75, 0x75, 0x69, 0x64,
        ]
    );
    assert_eq!(&encoded[4088..4092], &[0x46, 0x74, 0x39, 0xe1]);
    assert!(encoded[48..4088].iter().all(|byte| *byte == 0));
    assert!(encoded[4092..].iter().all(|byte| *byte == 0));
    assert_eq!(
        Superblock::decode(&encoded).unwrap(),
        Superblock {
            block_size: 4096,
            total_blocks: 16,
            features: 0,
            uuid: *b"openfs-test-uuid",
        }
    );
}
