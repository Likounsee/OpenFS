//! Read-only end-to-end verification for caller-located persistent inventories.
//!
//! This composes the existing on-disk decoders, namespace graph validation, extent-page
//! validation, and bitmap ownership reconciliation. It intentionally does not guess where
//! inventories live: the format has not yet assigned canonical persistent inventory regions.

use std::collections::BTreeMap;

use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

use crate::{AllocationOwnershipReport, Extent, ExtentMap, Namespace, ObjectId, OpenedVolume};

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
    /// Descriptor block discovered on disk, if verification started from auto-discovery.
    pub descriptor_block: Option<u64>,
}

/// Inputs describing one persistent inventory and the optional descriptor that owns it.
struct InventorySources<'a> {
    root_id: ObjectId,
    metadata_blocks: &'a [u64],
    metadata_record_count: usize,
    directory_blocks: &'a [u64],
    directory_entry_count: usize,
    descriptor_block: Option<u64>,
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

impl OpenedVolume {
    /// Verify caller-located packed inventories, extent-map pages, and allocation ownership.
    ///
    /// The caller supplies inventory block lists and record counts. Orphaned allocations and
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
        self.verify_persistent_inventory_with_sources(
            device,
            InventorySources {
                root_id,
                metadata_blocks,
                metadata_record_count,
                directory_blocks,
                directory_entry_count,
                descriptor_block: None,
            },
        )
    }

    /// Discover a descriptor, reconstruct its namespace, and reconcile all known owners.
    /// The descriptor block itself is included in ownership accounting.
    pub fn verify_discovered_persistent_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
    ) -> Result<Option<PersistentInventoryReport>> {
        let Some((descriptor_block, descriptor)) = self.discover_persistent_inventory(device)?
        else {
            return Ok(None);
        };
        let report = self.verify_persistent_inventory_with_sources(
            device,
            InventorySources {
                root_id: descriptor.root_id,
                metadata_blocks: &descriptor.metadata_blocks,
                metadata_record_count: descriptor.metadata_record_count,
                directory_blocks: &descriptor.directory_blocks,
                directory_entry_count: descriptor.directory_entry_count,
                descriptor_block: Some(descriptor_block),
            },
        )?;
        Ok(Some(report))
    }

    fn verify_persistent_inventory_with_sources<D: BlockDevice>(
        &self,
        device: &mut D,
        sources: InventorySources<'_>,
    ) -> Result<PersistentInventoryReport> {
        let namespace = self.reconstruct_namespace(
            device,
            sources.root_id,
            sources.metadata_blocks,
            sources.metadata_record_count,
            sources.directory_blocks,
            sources.directory_entry_count,
        )?;
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;

        // Treat inventory blocks and extent-map roots as ordinary physical owners too.
        // This catches aliases between index pages and file data instead of misreporting
        // those allocated blocks as orphans. Keep ranges compressed: each physical extent
        // remains one range in the bitmap reconciliation rather than expanding all blocks.
        let mut index_extents = Vec::new();
        let mut logical_cursor = 0_u64;
        for block in sources
            .metadata_blocks
            .iter()
            .chain(sources.directory_blocks.iter())
            .chain(sources.descriptor_block.iter())
        {
            index_extents.push(Extent {
                logical_start: logical_cursor,
                physical_start: *block,
                block_count: 1,
            });
            logical_cursor = logical_cursor
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow)?;
        }
        for record in namespace.objects() {
            if let Some(root) = record.extent_root_block {
                index_extents.push(Extent {
                    logical_start: logical_cursor,
                    physical_start: root,
                    block_count: 1,
                });
                logical_cursor = logical_cursor
                    .checked_add(1)
                    .ok_or(Error::ArithmeticOverflow)?;
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
            let logical_blocks =
                record.size / block_size + u64::from(record.size % block_size != 0);
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
            metadata_record_count: sources.metadata_record_count,
            directory_entry_count: sources.directory_entry_count,
            owner_map_count: owned_maps.len(),
            descriptor_block: sources.descriptor_block,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::PersistentInventoryReport;
    use crate::{
        format_volume, DirectoryEntry, Extent, ExtentMap, MetadataRecord, ObjectId, ObjectKind,
        PersistentInventoryDescriptor,
    };
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
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let orphan_block = volume.allocate_block(&mut device).unwrap();
            write_root_inventory(&mut device, metadata_block);

            let report: PersistentInventoryReport = volume
                .verify_persistent_inventory(&mut device, ObjectId(1), &[metadata_block], 1, &[], 0)
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
    fn discovered_verification_counts_the_descriptor_as_an_owned_block() {
        use crate::PersistentInventoryDescriptor;

        let path = image_path("discovered-clean-report");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let descriptor_block = volume.allocate_block(&mut device).unwrap();
            write_root_inventory(&mut device, metadata_block);
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![metadata_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            device
                .write_blocks(descriptor_block, &descriptor.encode(4096).unwrap())
                .unwrap();

            let report = volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(report.descriptor_block, Some(descriptor_block));
            assert!(report.is_consistent());
            assert_eq!(report.allocation.orphaned_allocation_count, 0);
            assert_eq!(report.allocation.dangling_reference_count, 0);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn reports_consistent_inventory_when_all_allocations_have_owners() {
        let path = image_path("clean-report");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            write_root_inventory(&mut device, metadata_block);

            let report = volume
                .verify_persistent_inventory(&mut device, ObjectId(1), &[metadata_block], 1, &[], 0)
                .unwrap();
            assert!(report.is_consistent());
            assert_eq!(report.allocation.orphaned_allocation_count, 0);
            assert_eq!(report.allocation.dangling_reference_count, 0);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovered_verification_rejects_file_data_aliasing_descriptor_at_every_block_size() {
        for block_size in [4096_u32, 8192, 16384, 32768, 65536] {
            let path = image_path(&format!("descriptor-data-alias-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let directory_block = volume.allocate_block(&mut device).unwrap();
                let extent_root_block = volume.allocate_block(&mut device).unwrap();
                let descriptor_block = volume.allocate_block(&mut device).unwrap();

                let root = root_record();
                let file = MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: 1,
                    extent_root_block: Some(extent_root_block),
                    generation: 1,
                };
                let mut metadata_bytes = vec![0; block_size as usize];
                metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
                metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();

                let entry = DirectoryEntry {
                    parent_id: 1,
                    object_id: 2,
                    kind: ObjectKind::File,
                    name: "file.bin".to_owned(),
                };
                let mut directory_bytes = vec![0; block_size as usize];
                directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();

                let map = ExtentMap::new(vec![Extent {
                    logical_start: 0,
                    physical_start: descriptor_block,
                    block_count: 1,
                }])
                .unwrap();
                device
                    .write_blocks(
                        extent_root_block,
                        &map.encode_block(block_size as usize).unwrap(),
                    )
                    .unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 2,
                    directory_blocks: vec![directory_block],
                    directory_entry_count: 1,
                };
                device
                    .write_blocks(
                        descriptor_block,
                        &descriptor.encode(block_size as usize).unwrap(),
                    )
                    .unwrap();

                assert!(
                    matches!(
                        volume.verify_discovered_persistent_inventory(&mut device),
                        Err(openfs_core::Error::Corrupt(_))
                    ),
                    "descriptor/data ownership alias must fail closed at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn caller_supplied_inventory_rejects_file_data_aliasing_metadata_at_every_block_size() {
        for block_size in [4096_u32, 8192, 16384, 32768, 65536] {
            let path = image_path(&format!("caller-inventory-data-alias-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let directory_block = volume.allocate_block(&mut device).unwrap();
                let extent_root_block = volume.allocate_block(&mut device).unwrap();

                let root = root_record();
                let file = crate::MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: crate::ObjectKind::File,
                    flags: 0,
                    size: 1,
                    extent_root_block: Some(extent_root_block),
                    generation: 1,
                };
                let mut metadata_bytes = vec![0; block_size as usize];
                metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
                metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();

                let entry = DirectoryEntry {
                    parent_id: 1,
                    object_id: 2,
                    kind: crate::ObjectKind::File,
                    name: "file.bin".to_owned(),
                };
                let mut directory_bytes = vec![0; block_size as usize];
                directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();

                let map = ExtentMap::new(vec![Extent {
                    logical_start: 0,
                    physical_start: metadata_block,
                    block_count: 1,
                }])
                .unwrap();
                device
                    .write_blocks(
                        extent_root_block,
                        &map.encode_block(block_size as usize).unwrap(),
                    )
                    .unwrap();

                assert!(
                    matches!(
                        volume.verify_persistent_inventory(
                            &mut device,
                            ObjectId(1),
                            &[metadata_block],
                            2,
                            &[directory_block],
                            1,
                        ),
                        Err(openfs_core::Error::Corrupt(_))
                    ),
                    "file data must not alias metadata inventory at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn caller_supplied_inventory_rejects_metadata_directory_block_alias_at_every_block_size() {
        for block_size in [4096_u32, 8192, 16384, 32768, 65536] {
            let path = image_path(&format!("caller-inventory-alias-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                write_root_inventory(&mut device, metadata_block);

                // The caller-located API must enforce the same physical ownership
                // contract as descriptor-discovered verification, even if the
                // directory inventory currently claims zero live entries.
                assert!(
                    matches!(
                        volume.verify_persistent_inventory(
                            &mut device,
                            ObjectId(1),
                            &[metadata_block],
                            1,
                            &[metadata_block],
                            0,
                        ),
                        Err(openfs_core::Error::Corrupt(_))
                    ),
                    "metadata/directory block alias must fail closed at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovered_verification_returns_none_when_no_descriptor_exists() {
        let path = image_path("no-descriptor");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();

            assert!(volume
                .verify_discovered_persistent_inventory(&mut device)
                .unwrap()
                .is_none());
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovered_verification_rejects_allocated_descriptor_with_bad_checksum() {
        let path = image_path("bad-descriptor-checksum");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let descriptor_block = volume.allocate_block(&mut device).unwrap();
            let descriptor = crate::PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![],
                metadata_record_count: 0,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let mut bytes = descriptor.encode(4096).unwrap();
            bytes[40] ^= 1;
            device.write_blocks(descriptor_block, &bytes).unwrap();

            assert!(matches!(
                volume.verify_discovered_persistent_inventory(&mut device),
                Err(openfs_core::Error::Corrupt(
                    "inventory descriptor CRC32C mismatch"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovered_verification_rejects_semantically_invalid_descriptor_with_valid_checksum() {
        for block_size in [4096_u32, 8192, 16384, 32768, 65536] {
            let path = image_path(&format!("invalid-descriptor-root-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let descriptor_block = volume.allocate_block(&mut device).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let mut bytes = descriptor.encode(block_size as usize).unwrap();
                bytes[12..28].fill(0);
                bytes[40..44].fill(0);
                let checksum = crate::crc32c(&bytes);
                bytes[40..44].copy_from_slice(&checksum.to_le_bytes());
                device.write_blocks(descriptor_block, &bytes).unwrap();

                assert!(matches!(
                    volume.verify_discovered_persistent_inventory(&mut device),
                    Err(openfs_core::Error::Corrupt(
                        "inventory descriptor root ID is zero"
                    ))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    struct ReadOnlyAuditDevice<D> {
        inner: D,
        writes: usize,
        flushes: usize,
    }

    impl<D> ReadOnlyAuditDevice<D> {
        fn new(inner: D) -> Self {
            Self {
                inner,
                writes: 0,
                flushes: 0,
            }
        }
    }

    impl<D: BlockDevice> BlockDevice for ReadOnlyAuditDevice<D> {
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
            self.writes += 1;
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.flushes += 1;
            self.inner.flush()
        }
    }

    #[test]
    fn persistent_inventory_verification_performs_no_writes_or_flushes() {
        for block_size in [4096_u32, 8192, 16384, 32768, 65536] {
            let path = image_path(&format!("verifier-read-only-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                write_root_inventory(&mut device, metadata_block);

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap();

                // Start counting only after constructing and publishing the valid fixture:
                // neither caller-located nor descriptor-discovered verification may write
                // metadata, update the journal, or flush the device.
                let mut audited = ReadOnlyAuditDevice::new(device);
                let caller_report = volume
                    .verify_persistent_inventory(
                        &mut audited,
                        ObjectId(1),
                        &[metadata_block],
                        1,
                        &[],
                        0,
                    )
                    .unwrap();
                let discovered_report = volume
                    .verify_discovered_persistent_inventory(&mut audited)
                    .unwrap()
                    .expect("published inventory descriptor must be discovered");

                assert!(caller_report.is_consistent(), "caller report at block size {block_size}");
                assert!(
                    discovered_report.is_consistent(),
                    "discovered report at block size {block_size}"
                );
                assert_eq!(
                    audited.writes, 0,
                    "verification wrote at block size {block_size}"
                );
                assert_eq!(
                    audited.flushes, 0,
                    "verification flushed at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }
}
