//! Read-only end-to-end verification for caller-located V2 inventories.
//!
//! This composes the existing on-disk decoders, namespace graph validation, extent-page
//! validation, and bitmap ownership reconciliation. It intentionally does not guess where
//! inventories live: V2 has not yet assigned canonical persistent inventory regions.

use std::collections::BTreeMap;

use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

use crate::{
    AllocationOwnershipReport, Extent, ExtentMap, Namespace, ObjectId, OpenedVolumeV2,
};

/// Structured result of verifying caller-located persistent namespace inventories.
///
/// Structural/format failures are returned as `Err` and are fatal to this verification
/// pass. A successful report may still contain recoverable allocation anomalies (orphaned
/// allocated blocks or references to free blocks) in `allocation`.
#[derive(Debug, Clone)]
pub struct PersistentInventoryReport {
    /// Reconstructed namespace, validated for duplicate IDs/names and graph consistency.
    pub namespace: Namespace,
    /// Every decoded file extent map, keyed by stable object ID.
    pub extent_maps: BTreeMap<ObjectId, ExtentMap>,
    /// Deterministic bitmap-versus-owner diagnostics, including bounded block samples.
    pub allocation: AllocationOwnershipReport,
    /// Number of metadata records successfully decoded.
    pub metadata_record_count: usize,
    /// Number of directory entries successfully decoded.
    pub directory_entry_count: usize,
    /// Number of distinct owner ranges submitted to the bitmap reconciliation.
    pub owner_map_count: usize,
}

impl PersistentInventoryReport {
    /// True only when structural validation succeeded and every data block has one owner.
    pub fn is_consistent(&self) -> bool {
        self.allocation.is_consistent()
    }

    /// Number of orphaned allocated blocks, including entries beyond the diagnostic sample.
    pub fn orphaned_block_count(&self) -> u64 {
        self.allocation.orphaned_allocation_count
    }

    /// Number of owner references to blocks marked free, including entries beyond the sample.
    pub fn dangling_reference_count(&self) -> u64 {
        self.allocation.dangling_reference_count
    }
}

impl OpenedVolumeV2 {
    /// Verify packed namespace inventories, extent-map pages, and allocation ownership.
    ///
    /// The caller supplies the inventory block lists and record counts because the current
    /// V2 format has no canonical metadata-region descriptor. Fatal record, graph, bounds,
    /// checksum, root, and cross-owner alias errors return `Err`. Orphaned allocations and
    /// dangling references are returned as deterministic recoverable diagnostics.
    pub fn verify_persistent_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
        root_id: ObjectId,
        metadata_blocks: &[u64],
        metadata_record_count: usize,
        directory_blocks: &[u64],
        directory_entry_count: usize,
    ) -> Result<PersistentInventoryReport> {
        let namespace = self.reconstruct_namespace(
            device,
            root_id,
            metadata_blocks,
            metadata_record_count,
            directory_blocks,
            directory_entry_count,
        )?;
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;

        // Treat inventory blocks and extent-map roots as ordinary physical owners too.
        // This catches aliases between index pages and file data instead of misreporting
        // those allocated blocks as orphans. Keep ranges compressed: each physical extent
        // remains one range in the bitmap reconciliation rather than expanding all blocks.
        let mut index_extents = Vec::new();
        let mut logical_cursor = 0_u64;
        for block in metadata_blocks.iter().chain(directory_blocks.iter()) {
            index_extents.push(Extent {
                logical_start: logical_cursor,
                physical_start: *block,
                block_count: 1,
            });
            logical_cursor = logical_cursor.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        }
        for record in namespace.objects() {
            if let Some(root) = record.extent_root_block {
                index_extents.push(Extent {
                    logical_start: logical_cursor,
                    physical_start: root,
                    block_count: 1,
                });
                logical_cursor = logical_cursor.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            }
        }
        let index_map = ExtentMap::new(index_extents)?;
        let mut owned_maps: Vec<(ExtentMap, u64)> = Vec::with_capacity(extent_maps.len() + 1);
        owned_maps.push((index_map, logical_cursor));
        for (object_id, map) in &extent_maps {
            let record = namespace
                .object(*object_id)
                .ok_or(Error::Corrupt("extent map has no namespace object"))?;
            let block_size = u64::from(self.volume.block_size);
            let logical_blocks = record.size / block_size
                + u64::from(record.size % block_size != 0);
            owned_maps.push((map.clone(), logical_blocks));
        }
        let map_refs: Vec<(&ExtentMap, u64)> = owned_maps
            .iter()
            .map(|(map, logical_blocks)| (map, *logical_blocks))
            .collect();
        let allocation = self.extent_map_ownership_report(device, &map_refs)?;

        Ok(PersistentInventoryReport {
            namespace,
            extent_maps,
            allocation,
            metadata_record_count,
            directory_entry_count,
            owner_map_count: owned_maps.len(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::PersistentInventoryReport;
    use crate::{format_volume_v2, MetadataRecord, ObjectId, ObjectKind};
    use openfs_block::{BlockDevice, ImageDevice};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn image_path(label: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "openfs-inventory-verifier-{label}-{}-{nonce}.img",
            std::process::id()
        ))
    }

    fn root_record() -> MetadataRecord {
        MetadataRecord {
            id: ObjectId(1),
            parent_id: 0,
            kind: ObjectKind::Directory,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        }
    }

    fn write_root_inventory(device: &mut ImageDevice, block: u64) {
        let record = root_record().encode().unwrap();
        let mut bytes = vec![0; device.block_size() as usize];
        bytes[..record.len()].copy_from_slice(&record);
        device.write_blocks(block, &bytes).unwrap();
    }

    #[test]
    fn verifies_persistent_root_inventory_and_reports_orphans_without_failing() {
        let path = image_path("orphan-report");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let orphan_block = volume.allocate_block(&mut device).unwrap();
            write_root_inventory(&mut device, metadata_block);

            let report: PersistentInventoryReport = volume
                .verify_persistent_inventory(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    1,
                    &[],
                    0,
                )
                .unwrap();
            assert_eq!(report.namespace.root_id(), ObjectId(1));
            assert_eq!(report.metadata_record_count, 1);
            assert_eq!(report.directory_entry_count, 0);
            assert_eq!(report.extent_maps.len(), 0);
            assert_eq!(report.allocation.orphaned_allocations, vec![orphan_block]);
            assert_eq!(report.orphaned_block_count(), 1);
            assert_eq!(report.dangling_reference_count(), 0);
            assert!(!report.is_consistent());
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn reports_consistent_inventory_when_all_allocations_have_owners() {
        let path = image_path("clean-report");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            write_root_inventory(&mut device, metadata_block);

            let report = volume
                .verify_persistent_inventory(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    1,
                    &[],
                    0,
                )
                .unwrap();
            assert!(report.is_consistent());
            assert_eq!(report.allocation.orphaned_allocation_count, 0);
            assert_eq!(report.allocation.dangling_reference_count, 0);
        }
        fs::remove_file(path).unwrap();
    }
}
