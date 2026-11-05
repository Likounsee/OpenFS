//! Focused regression tests for Basic-mode repair operations.
use crate::{format_volume, open_volume, Error, ObjectId};
use openfs_block::{BlockDevice, ImageDevice};
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
            let mut volume =
                format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
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
fn basic_does_not_guess_ownership_when_namespace_is_absent_or_dangling() {
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

    // A dangling reference is corruption, not an invitation to free arbitrary blocks.
    // The existing verifier and repair path must fail closed on this condition.
    let image = TestImage::new("dangling-reference");
    {
        let mut device = ImageDevice::create(&image.0, 4096, 256).unwrap();
        let mut volume = format_volume(&mut device, *b"openfs-basic-001", 0).unwrap();
        volume
            .initialize_persistent_namespace(&mut device, ObjectId(1))
            .unwrap();
        let free_before = volume.free_data_block_count(&mut device).unwrap();
        assert_eq!(volume.reclaim_orphaned_blocks(&mut device).unwrap(), 0);
        assert_eq!(volume.free_data_block_count(&mut device).unwrap(), free_before);
        assert!(matches!(
            volume.free_block(&mut device, volume.volume.layout.backup_superblock),
            Err(Error::InvalidInput(_))
        ));
    }
}
