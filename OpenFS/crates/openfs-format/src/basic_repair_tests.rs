//! Focused regression tests for Basic-mode repair operations.
use crate::{format_volume, open_volume, ObjectId, VolumeSession};
use openfs_block::ImageDevice;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_IMAGE_ID: AtomicU64 = AtomicU64::new(0);

struct TestImage(PathBuf);

impl TestImage {
    fn new(label: &str) -> Self {
        let id = NEXT_IMAGE_ID.fetch_add(1, Ordering::Relaxed);
        Self(std::env::temp_dir().join(format!(
            "openfs-basic-repair-{}-{label}-{id}.img",
            std::process::id()
        )))
    }
}

impl Drop for TestImage {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

#[test]
fn basic_reclaims_only_unowned_allocations_and_survives_remount() {
    for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
        let image = TestImage::new(&format!("orphan-reclaim-{block_size}"));
        {
            let mut device = ImageDevice::create(&image.0, block_size, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
            volume
                .initialize_persistent_namespace(&mut device, ObjectId(1))
                .unwrap();

            let free_before = volume.free_data_block_count(&mut device).unwrap();
            let orphan_a = volume.allocate_block(&mut device).unwrap();
            let orphan_b = volume.allocate_block(&mut device).unwrap();
            assert_ne!(orphan_a, orphan_b);
            assert_eq!(
                volume.free_data_block_count(&mut device).unwrap(),
                free_before - 2
            );

            assert_eq!(volume.reclaim_orphaned_blocks(&mut device).unwrap(), 2);
            assert_eq!(
                volume.free_data_block_count(&mut device).unwrap(),
                free_before,
                "Basic reclamation must return both orphan blocks (block size {block_size})"
            );
            assert!(volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .unwrap()
                .is_consistent());
            let report = crate::check_volume(&mut device).unwrap();
            assert!(!report.journal_enabled);
            assert_eq!(report.pending_journal_records, 0);
        }

        {
            let mut device = ImageDevice::open(&image.0, block_size).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.reclaim_orphaned_blocks(&mut device).unwrap(), 0);
            assert!(volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .unwrap()
                .is_consistent());
            let report = crate::check_volume(&mut device).unwrap();
            assert!(!report.journal_enabled);
            assert_eq!(report.pending_journal_records, 0);
        }
    }
}

#[test]
fn basic_does_not_guess_ownership_when_namespace_is_absent() {
    let image = TestImage::new("no-namespace");
    {
        let mut device = ImageDevice::create(&image.0, 4096, 128).unwrap();
        let mut volume = format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
        let orphan = volume.allocate_block(&mut device).unwrap();
        let free_before_repair = volume.free_data_block_count(&mut device).unwrap();

        // Without a published namespace there is no trustworthy ownership graph.
        assert_eq!(volume.reclaim_orphaned_blocks(&mut device).unwrap(), 0);
        assert_eq!(
            volume.free_data_block_count(&mut device).unwrap(),
            free_before_repair
        );
        assert!(orphan >= volume.volume.layout.data_start);
    }
}

#[test]
fn basic_tree_removal_refuses_read_only_descendants_without_mutating_image() {
    use crate::METADATA_FLAG_READ_ONLY;
    use openfs_block::BlockDevice;

    let image = TestImage::new("readonly-subtree");
    {
        let mut device = ImageDevice::create(&image.0, 4096, 256).unwrap();
        let mut volume = format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
        volume
            .initialize_persistent_namespace(&mut device, ObjectId(1))
            .unwrap();
        volume
            .create_persistent_directory_at_path(&mut device, "/", ObjectId(2), "protected")
            .unwrap();
        volume
            .create_persistent_file_at_path(&mut device, "/protected", ObjectId(3), "important.txt")
            .unwrap();
        volume
            .write_persistent_file_contents(
                &mut device,
                "/protected/important.txt",
                b"must survive recursive deletion",
            )
            .unwrap();
        volume
            .set_persistent_attributes(
                &mut device,
                "/protected/important.txt",
                METADATA_FLAG_READ_ONLY,
                true,
            )
            .unwrap();
        device.flush().unwrap();
    }

    {
        let mut device = ImageDevice::open(&image.0, 4096).unwrap();
        let mut before = vec![0_u8; 4096 * 256];
        device.read_blocks(0, &mut before).unwrap();
        let mut volume = open_volume(&mut device).unwrap();

        assert!(matches!(
            volume.remove_persistent_tree_at_path(&mut device, "/protected"),
            Err(crate::Error::InvalidInput(
                "cannot remove a subtree containing a read-only file"
            ))
        ));

        let mut after = vec![0_u8; 4096 * 256];
        device.read_blocks(0, &mut after).unwrap();
        assert_eq!(
            after, before,
            "refusing recursive deletion must preserve every image byte"
        );
        assert_eq!(
            volume
                .read_persistent_path_range(&mut device, "/protected/important.txt", 0, 64,)
                .unwrap(),
            b"must survive recursive deletion"
        );
        assert!(volume
            .verify_discovered_persistent_inventory(&mut device)
            .unwrap()
            .unwrap()
            .is_consistent());
    }
}

#[test]
fn basic_streamed_replacement_rolls_back_when_source_reader_fails() {
    use std::io::{self, Read};

    struct FailingReader {
        remaining: usize,
    }

    impl Read for FailingReader {
        fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
            if self.remaining == 0 {
                return Err(io::Error::other("injected source read failure"));
            }
            let count = output.len().min(self.remaining);
            output[..count].fill(0xA5);
            self.remaining -= count;
            Ok(count)
        }
    }

    let original = b"the previous Basic file must remain intact";
    for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
        let image = TestImage::new(&format!("stream-replacement-reader-failure-{block_size}"));
        {
            let mut device = ImageDevice::create(&image.0, block_size, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
            volume
                .initialize_persistent_namespace(&mut device, ObjectId(1))
                .unwrap();
            device.flush().unwrap();
        }

        {
            let device = ImageDevice::open(&image.0, block_size).unwrap();
            let mut session = VolumeSession::open(device).unwrap();
            session.create_file_auto("/", "data.bin").unwrap();
            session.write_file("/data.bin", original).unwrap();

            let mut source = FailingReader {
                remaining: block_size as usize * 2,
            };
            assert!(matches!(
                session.write_file_from_reader("/data.bin", &mut source),
                Err(crate::Error::Io(_))
            ));
            assert_eq!(session.read_file("/data.bin").unwrap(), original);
            let mut device = session.unmount().unwrap();
            let volume = open_volume(&mut device).unwrap();
            let ownership = volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert!(ownership.is_consistent());
            assert_eq!(ownership.orphaned_block_count(), 0);
        }

        {
            let device = ImageDevice::open(&image.0, block_size).unwrap();
            let mut session = VolumeSession::open(device).unwrap();
            assert_eq!(session.read_file("/data.bin").unwrap(), original);
            let mut device = session.unmount().unwrap();
            let volume = open_volume(&mut device).unwrap();
            let ownership = volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert!(ownership.is_consistent());
            assert_eq!(ownership.orphaned_block_count(), 0);
        }
    }
}
