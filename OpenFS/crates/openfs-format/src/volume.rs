//! Canonical volume layout with a reserved persistent journal.
//!
//! The journal reserves two checkpoint blocks and a versioned redo ring before the
//! data region, and protects allocator bitmap changes.

use crate::{
    crc32c, decode_directory_entries, decode_metadata_records, format_journal, plan_recovery,
    DirectoryEntry, ExtentMap, JournalCheckpoint, JournalRecord, JournalRecordKind, JournalRegion,
    MetadataRecord, Namespace, ObjectId, PersistentInventoryDescriptor, PersistentJournal,
    RedoUpdate, DIRECTORY_ENTRY_SIZE, INVENTORY_DESCRIPTOR_MAGIC, JOURNAL_CHECKPOINT_SIZE,
    METADATA_RECORD_SIZE, SUPERBLOCK_SIZE,
};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

const MAGIC: &[u8; 8] = b"OPENFS\0\0";
const VERSION: u16 = 4;
const LEGACY_VERSION: u16 = 2;
const PREVIOUS_VERSION: u16 = 3;
const INVENTORY_HINT_OFFSET: usize = 48;
const HEADER_SIZE: u16 = 48;
const CHECKSUM_OFFSET: usize = 4088;
const JOURNAL_RING_BLOCKS: u64 = 16;
const LEGACY_JOURNAL_RING_BLOCKS: u64 = 4;
const JOURNAL_RESERVED_BLOCKS: u64 = 2 + JOURNAL_RING_BLOCKS;
const INVENTORY_DISCOVERY_BATCH_BLOCKS: usize = 64;
const INVENTORY_BITMAP_CACHE_PAGES: usize = 8;

fn try_zeroed_block(block_size: usize, allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut block = Vec::new();
    block
        .try_reserve_exact(block_size)
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    block.resize(block_size, 0);
    Ok(block)
}

fn try_copy_block_numbers(blocks: &[u64], allocation_error: &'static str) -> Result<Vec<u64>> {
    let mut copy = Vec::new();
    copy.try_reserve_exact(blocks.len())
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    copy.extend_from_slice(blocks);
    Ok(copy)
}

fn try_join_block_numbers(
    first: &[u64],
    second: &[u64],
    allocation_error: &'static str,
) -> Result<Vec<u64>> {
    let count = first
        .len()
        .checked_add(second.len())
        .ok_or(Error::ArithmeticOverflow)?;
    let mut blocks = Vec::new();
    blocks
        .try_reserve_exact(count)
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    blocks.extend_from_slice(first);
    blocks.extend_from_slice(second);
    Ok(blocks)
}

fn try_single_block_list(block: u64, allocation_error: &'static str) -> Result<Vec<u64>> {
    let mut blocks = Vec::new();
    blocks
        .try_reserve_exact(1)
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    blocks.push(block);
    Ok(blocks)
}

fn try_copy_bytes(bytes: &[u8], allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut copy = Vec::new();
    copy.try_reserve_exact(bytes.len())
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    copy.extend_from_slice(bytes);
    Ok(copy)
}

fn try_clone_string(value: &str, allocation_error: &'static str) -> Result<String> {
    let mut copy = String::new();
    copy.try_reserve_exact(value.len())
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    copy.push_str(value);
    Ok(copy)
}

fn try_clone_namespace_records(namespace: &Namespace) -> Result<Vec<MetadataRecord>> {
    let count = namespace.objects().count();
    let mut records = Vec::new();
    records
        .try_reserve_exact(count)
        .map_err(|_| Error::InvalidInput("unable to allocate namespace metadata staging"))?;
    records.extend(namespace.objects().cloned());
    Ok(records)
}

fn try_clone_namespace_entries(namespace: &Namespace) -> Result<Vec<DirectoryEntry>> {
    let count = namespace.entries().count();
    let mut entries = Vec::new();
    entries
        .try_reserve_exact(count)
        .map_err(|_| Error::InvalidInput("unable to allocate namespace directory staging"))?;
    entries.extend(namespace.entries().cloned());
    Ok(entries)
}

/// On-disk regions for the canonical format. Journal capacity depends on the format version.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VolumeLayout {
    pub total_blocks: u64,
    pub block_size: u32,
    pub bitmap_start: u64,
    pub bitmap_blocks: u64,
    pub journal: JournalRegion,
    pub data_start: u64,
    pub backup_superblock: u64,
}

impl VolumeLayout {
    pub fn calculate(block_size: u32, total_blocks: u64) -> Result<Self> {
        Self::calculate_with_ring(block_size, total_blocks, JOURNAL_RING_BLOCKS)
    }

    pub(crate) fn calculate_for_version(
        block_size: u32,
        total_blocks: u64,
        version: u16,
    ) -> Result<Self> {
        let ring_blocks = if version == VERSION {
            JOURNAL_RING_BLOCKS
        } else {
            LEGACY_JOURNAL_RING_BLOCKS
        };
        Self::calculate_with_ring(block_size, total_blocks, ring_blocks)
    }

    fn calculate_with_ring(
        block_size: u32,
        total_blocks: u64,
        journal_ring_blocks: u64,
    ) -> Result<Self> {
        if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
            return Err(Error::InvalidInput("unsupported filesystem block size"));
        }
        if total_blocks < 12 {
            return Err(Error::InvalidInput(
                "volume is too small for reserved regions",
            ));
        }
        u64::from(block_size)
            .checked_mul(total_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        let bits_per_bitmap_block = u64::from(block_size)
            .checked_mul(8)
            .ok_or(Error::ArithmeticOverflow)?;
        let bitmap_blocks = total_blocks
            .checked_add(bits_per_bitmap_block - 1)
            .ok_or(Error::ArithmeticOverflow)?
            / bits_per_bitmap_block;
        let bitmap_start = 1_u64;
        let journal_start = bitmap_start
            .checked_add(bitmap_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        let reserved_journal_blocks = if journal_ring_blocks == JOURNAL_RING_BLOCKS {
            JOURNAL_RESERVED_BLOCKS
        } else {
            2_u64 + journal_ring_blocks
        };
        let data_start = journal_start
            .checked_add(reserved_journal_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        let backup_superblock = total_blocks - 1;
        if data_start >= backup_superblock {
            return Err(Error::InvalidInput(
                "volume has no data region after reserved blocks",
            ));
        }
        Ok(Self {
            total_blocks,
            block_size,
            bitmap_start,
            bitmap_blocks,
            journal: JournalRegion {
                checkpoint_a: journal_start,
                checkpoint_b: journal_start + 1,
                ring_start: journal_start + 2,
                ring_blocks: journal_ring_blocks,
            },
            data_start,
            backup_superblock,
        })
    }

    pub fn data_blocks(&self) -> u64 {
        self.backup_superblock - self.data_start
    }

    fn is_reserved(&self, block: u64) -> bool {
        block == 0
            || (block >= self.bitmap_start && block - self.bitmap_start < self.bitmap_blocks)
            || block == self.journal.checkpoint_a
            || block == self.journal.checkpoint_b
            || (block >= self.journal.ring_start
                && block - self.journal.ring_start < self.journal.ring_blocks)
            || block == self.backup_superblock
    }
}

/// Validated volume geometry and superblock fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Volume {
    pub block_size: u32,
    pub total_blocks: u64,
    pub features: u64,
    pub uuid: [u8; 16],
    pub layout: VolumeLayout,
}

/// Writable volume. Opening this value runs journal recovery before returning it.
#[derive(Debug)]
pub struct OpenedVolume {
    pub volume: Volume,
    pub journal: PersistentJournal,
    pub(crate) format_version: u16,
    // Cache descriptor discovery for allocator guards. None means not checked yet;
    // Some(None) means checked and absent; Some(Some(...)) is the validated locator.
    inventory_descriptor_cache: Option<Option<(u64, PersistentInventoryDescriptor)>>,
    inventory_descriptor_hint: Option<u64>,
}

impl OpenedVolume {
    /// Maximum number of distinct full-block redo updates that fit in one journal transaction.
    ///
    /// Large atomic writes should be sized against this bound before staging data. The
    /// returned value is advisory: it does not reserve the journal or guarantee that a
    /// later I/O operation will succeed.
    pub fn max_atomic_redo_updates<D: BlockDevice>(&self, device: &D) -> Result<usize> {
        self.validate_device(device)?;
        self.journal.max_update_count(device)
    }

    /// Maximum byte length for a single atomic range write starting at the given offset.
    ///
    /// The limit accounts for a potentially unaligned starting offset. It is a journal
    /// capacity bound only; callers must also stay within the existing file size and
    /// allocated extents. A zero result means no non-empty write fits at that offset.
    pub fn max_atomic_write_bytes_at_offset<D: BlockDevice>(
        &self,
        device: &D,
        offset: u64,
    ) -> Result<u64> {
        let max_blocks = u64::try_from(self.max_atomic_redo_updates(device)?)
            .map_err(|_| Error::ArithmeticOverflow)?;
        if max_blocks == 0 {
            return Ok(0);
        }
        let block_size = u64::from(self.volume.block_size);
        max_blocks
            .checked_mul(block_size)
            .ok_or(Error::ArithmeticOverflow)?
            .checked_sub(offset % block_size)
            .ok_or(Error::ArithmeticOverflow)
    }

    /// Allocate and journal the bitmap update before exposing the new allocation.
    pub fn allocate_block<D: BlockDevice>(&mut self, device: &mut D) -> Result<u64> {
        self.validate_device(device)?;
        let block_size = self.volume.block_size as usize;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let first_bitmap = self.volume.layout.data_start / bits_per_block;
        let last_data_block = self
            .volume
            .layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_block;
        // Bitmap pages are read sequentially; reuse one page buffer rather than
        // allocating a fresh block-sized Vec for every page in a large volume.
        let mut bitmap =
            try_zeroed_block(block_size, "unable to allocate allocator bitmap buffer")?;
        // Reuse the candidate-block buffer as well as the bitmap page. On a nearly
        // full volume, allocation may inspect many candidates; allocating a fresh
        // block-sized Vec for each candidate creates avoidable allocator pressure.
        let mut existing =
            try_zeroed_block(block_size, "unable to allocate candidate block buffer")?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_block = self
                .volume
                .layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;

            let bitmap_base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let first_block = bitmap_base.max(self.volume.layout.data_start);
            let end_block = self.volume.layout.backup_superblock.min(
                bitmap_base
                    .checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            crate::allocator_extents::validate_bitmap_reserved_bits(
                &bitmap,
                bitmap_base,
                first_block,
                end_block,
            )?;
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            // Select and mark a candidate before leaving the scan, then move the
            // bitmap page into the redo image. Cloning a full block here doubled
            // transient bitmap memory and copied every byte for each single-block
            // allocation, including allocations on nearly-full volumes.
            let mut selected = None;
            // Slice to the usable byte range directly: iterator skip() would still
            // walk the unused prefix of the bitmap page on later segments.
            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter_mut().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let byte_start = u64::try_from(byte_index)
                    .map_err(|_| Error::ArithmeticOverflow)?
                    .checked_mul(8)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut valid_mask = u8::MAX;
                if byte_index == first_byte {
                    valid_mask &= u8::MAX << (local_start % 8);
                }
                if byte_index + 1 == end_byte && local_end % 8 != 0 {
                    valid_mask &= (1_u8 << (local_end % 8)) - 1;
                }
                let available = !*byte & valid_mask;
                if available != 0 {
                    let bit = u64::from(available.trailing_zeros());
                    let local = byte_start
                        .checked_add(bit)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let block = bitmap_base
                        .checked_add(local)
                        .ok_or(Error::ArithmeticOverflow)?;
                    *byte |= 1_u8 << bit;
                    selected = Some(block);
                    break;
                }
            }
            if let Some(block) = selected {
                // Clear the whole block before publishing ownership. Besides removing
                // a stale inventory descriptor left by an interrupted publication, this
                // prevents a newly allocated file from observing data from a prior owner.
                // If the write or flush fails, the bitmap is still unchanged and the block
                // remains free from the filesystem's point of view.
                existing.fill(0);
                device.write_blocks(block, &existing)?;
                device.flush()?;
                self.journal.commit(
                    device,
                    &[RedoUpdate {
                        target_block: bitmap_block,
                        contents: bitmap,
                    }],
                )?;
                return Ok(block);
            }
        }
        Err(Error::InvalidInput("volume has no free data blocks"))
    }

    /// Query whether a valid data-region block is allocated.
    ///
    /// Reserved blocks are intentionally not accepted: this reports allocation state
    /// only for blocks that callers may own as file or directory data.
    pub fn is_data_block_allocated<D: BlockDevice>(
        &self,
        device: &mut D,
        block: u64,
    ) -> Result<bool> {
        self.validate_device(device)?;
        if block < self.volume.layout.data_start || block >= self.volume.layout.backup_superblock {
            return Err(Error::InvalidInput(
                "allocation query requires an in-range data block",
            ));
        }
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let bitmap_index = block / bits_per_block;
        let bitmap_base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let bitmap_block = self
            .volume
            .layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let local = block - bitmap_base;
        let mut bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        crate::allocator_extents::validate_bitmap_reserved_bits(
            &bitmap,
            bitmap_base,
            self.volume.layout.data_start.max(bitmap_base),
            self.volume.layout.backup_superblock.min(
                bitmap_base
                    .checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            ),
        )?;
        let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
        let mask = 1_u8 << (local % 8);
        Ok(bitmap[byte] & mask != 0)
    }

    /// Count currently free blocks in the usable data region.
    ///
    /// This is a read-only snapshot; callers must not use it as a reservation because
    /// another allocator operation may consume a free block immediately afterward.
    pub fn free_data_block_count<D: BlockDevice>(&self, device: &mut D) -> Result<u64> {
        self.validate_device(device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let first_bitmap = self.volume.layout.data_start / bits_per_block;
        let last_data_block = self
            .volume
            .layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_block;
        let mut free = 0_u64;
        // Reuse the bitmap buffer across pages to keep allocator memory churn bounded.
        let mut bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let bitmap_block = self
                .volume
                .layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let first_block = bitmap_base.max(self.volume.layout.data_start);
            let end_block = bitmap_base
                .checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?
                .min(self.volume.layout.backup_superblock);
            crate::allocator_extents::validate_bitmap_reserved_bits(
                &bitmap,
                bitmap_base,
                first_block,
                end_block,
            )?;
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            // Visit only bytes overlapping this data segment; the first bitmap
            // page may contain a large reserved prefix that is irrelevant to this count.
            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut valid_mask = u8::MAX;
                if byte_index == first_byte {
                    valid_mask &= u8::MAX << (local_start % 8);
                }
                if byte_index + 1 == end_byte && local_end % 8 != 0 {
                    valid_mask &= (1_u8 << (local_end % 8)) - 1;
                }
                let free_bits = (!*byte & valid_mask).count_ones() as u64;
                free = free
                    .checked_add(free_bits)
                    .ok_or(Error::ArithmeticOverflow)?;
            }
        }
        Ok(free)
    }

    /// Free a data block through the journal; reserved and already-free blocks are rejected.
    pub fn free_block<D: BlockDevice>(&mut self, device: &mut D, block: u64) -> Result<()> {
        self.validate_device(device)?;
        if block < self.volume.layout.data_start || block >= self.volume.layout.backup_superblock {
            return Err(Error::InvalidInput(
                "cannot free a reserved or out-of-range block",
            ));
        }
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let bitmap_index = block / bits_per_block;
        let bitmap_base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let bitmap_block = self
            .volume
            .layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let local = block - bitmap_base;
        let mut bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        crate::allocator_extents::validate_bitmap_reserved_bits(
            &bitmap,
            bitmap_base,
            self.volume.layout.data_start.max(bitmap_base),
            self.volume.layout.backup_superblock.min(
                bitmap_base
                    .checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            ),
        )?;
        let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
        let mask = 1_u8 << (local % 8);
        if bitmap[byte] & mask == 0 {
            return Err(Error::InvalidInput("data block is already free"));
        }
        self.ensure_inventory_blocks_releasable(device, &[block])?;
        crate::inventory_descriptor::reject_freeing_inventory_descriptor(
            device,
            block,
            self.volume.block_size,
        )?;
        bitmap[byte] &= !mask;
        self.journal.commit(
            device,
            &[RedoUpdate {
                target_block: bitmap_block,
                contents: bitmap,
            }],
        )?;
        Ok(())
    }

    /// Refuse to release blocks that the persistent inventory still owns.
    ///
    /// Discovery is scan-based today; check the whole batch before mutating any bitmap.
    pub(crate) fn ensure_inventory_blocks_releasable<D: BlockDevice>(
        &mut self,
        device: &mut D,
        blocks: &[u64],
    ) -> Result<()> {
        if self.inventory_descriptor_cache.is_none() {
            self.inventory_descriptor_cache = Some(self.discover_persistent_inventory(device)?);
        }
        let Some((_, descriptor)) =
            self.inventory_descriptor_cache
                .as_ref()
                .ok_or(Error::Corrupt(
                    "inventory descriptor cache was not initialized",
                ))?
        else {
            return Ok(());
        };
        let descriptor_block_count = descriptor
            .metadata_blocks
            .len()
            .checked_add(descriptor.directory_blocks.len())
            .ok_or(Error::ArithmeticOverflow)?;
        let mut descriptor_blocks = Vec::new();
        descriptor_blocks
            .try_reserve_exact(descriptor_block_count)
            .map_err(|_| Error::InvalidInput("unable to allocate inventory ownership index"))?;
        descriptor_blocks.extend(descriptor.metadata_blocks.iter().copied());
        descriptor_blocks.extend(descriptor.directory_blocks.iter().copied());
        descriptor_blocks.sort_unstable();
        if blocks
            .iter()
            .any(|block| descriptor_blocks.binary_search(block).is_ok())
        {
            return Err(Error::InvalidInput(
                "cannot free a block referenced by persistent inventory descriptor",
            ));
        }

        // The descriptor is the entry point to persistent file ownership. Build the
        // ownership index once per release batch instead of rescanning every object and
        // every extent for every block in a large file or directory removal.
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::Corrupt(
                "persistent inventory disappeared during block-release validation",
            ));
        };
        let mut extent_roots = Vec::new();
        extent_roots
            .try_reserve_exact(namespace.object_count())
            .map_err(|_| Error::InvalidInput("unable to allocate extent-root ownership index"))?;
        extent_roots.extend(
            namespace
                .objects()
                .filter_map(|record| record.extent_root_block),
        );
        extent_roots.sort_unstable();
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let range_count = extent_maps.values().try_fold(0_usize, |count, map| {
            count
                .checked_add(map.extents().len())
                .ok_or(Error::ArithmeticOverflow)
        })?;
        let mut owned_ranges = Vec::new();
        owned_ranges
            .try_reserve_exact(range_count)
            .map_err(|_| Error::InvalidInput("unable to allocate file ownership ranges"))?;
        for map in extent_maps.values() {
            for extent in map.extents() {
                let end = extent
                    .physical_start
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)?;
                owned_ranges.push((extent.physical_start, end));
            }
        }
        owned_ranges.sort_unstable();

        for block in blocks {
            if extent_roots.binary_search(block).is_ok() {
                return Err(Error::InvalidInput(
                    "cannot free a block referenced by a file extent map",
                ));
            }
            let candidate = owned_ranges.partition_point(|(start, _)| *start <= *block);
            if candidate > 0 && *block < owned_ranges[candidate - 1].1 {
                return Err(Error::InvalidInput(
                    "cannot free a block owned by persistent file data",
                ));
            }
        }
        Ok(())
    }

    /// Refuse to release a contiguous range containing blocks owned by the inventory.
    ///
    /// This checks the compact descriptor lists directly, avoiding a temporary vector
    /// proportional to the caller's requested range.
    pub(crate) fn ensure_inventory_range_releasable<D: BlockDevice>(
        &mut self,
        device: &mut D,
        start_block: u64,
        end_block: u64,
    ) -> Result<()> {
        if start_block >= end_block {
            return Ok(());
        }
        if self.inventory_descriptor_cache.is_none() {
            self.inventory_descriptor_cache = Some(self.discover_persistent_inventory(device)?);
        }
        let Some((_, descriptor)) =
            self.inventory_descriptor_cache
                .as_ref()
                .ok_or(Error::Corrupt(
                    "inventory descriptor cache was not initialized",
                ))?
        else {
            return Ok(());
        };
        if descriptor
            .metadata_blocks
            .iter()
            .chain(descriptor.directory_blocks.iter())
            .any(|block| *block >= start_block && *block < end_block)
        {
            return Err(Error::InvalidInput(
                "cannot free a block referenced by persistent inventory descriptor",
            ));
        }

        // Descriptor blocks are only the namespace entry point. File data and extent-map
        // roots are owned by the namespace they publish, so protect those too when callers
        // ask to release an entire physical range.
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::Corrupt(
                "persistent inventory disappeared during range-release validation",
            ));
        };
        if namespace.objects().any(|record| {
            record
                .extent_root_block
                .is_some_and(|block| block >= start_block && block < end_block)
        }) {
            return Err(Error::InvalidInput(
                "cannot free a range containing a file extent-map root",
            ));
        }

        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        for map in extent_maps.values() {
            for extent in map.extents() {
                let extent_end = extent
                    .physical_start
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)?;
                if extent.physical_start < end_block && start_block < extent_end {
                    return Err(Error::InvalidInput(
                        "cannot free a range containing persistent file data",
                    ));
                }
            }
        }
        Ok(())
    }

    /// Publish the first persistent inventory descriptor atomically with its allocation.
    ///
    /// The descriptor is flushed to a free block before its allocation bit is committed
    /// through the journal. Discovery scans allocated blocks only, so recovery exposes the
    /// descriptor only after the bitmap commit is durable. Existing descriptors are never
    /// overwritten; safe replacement must coordinate metadata and ownership with durable mutations.
    pub fn publish_persistent_inventory<D: BlockDevice>(
        &mut self,
        device: &mut D,
        descriptor: &PersistentInventoryDescriptor,
    ) -> Result<u64> {
        self.validate_device(device)?;
        if let Some((existing_block, existing_descriptor)) =
            self.discover_persistent_inventory(device)?
        {
            // Publication can have an uncertain outcome if the caller loses the
            // response after the durable bitmap commit. Treat an identical retry
            // as success without writing anything; never reinterpret a different
            // inventory as an idempotent retry.
            if existing_descriptor == *descriptor {
                persist_inventory_hint(device, existing_block)?;
                self.inventory_descriptor_hint = Some(existing_block);
                self.inventory_descriptor_cache = Some(Some((existing_block, existing_descriptor)));
                return Ok(existing_block);
            }
            return Err(Error::InvalidInput(
                "a different persistent inventory descriptor already exists",
            ));
        }
        let encoded = descriptor.encode(self.volume.block_size as usize)?;
        let mut claimed = std::collections::BTreeSet::new();
        let mut allocation_bitmaps = std::collections::BTreeMap::<u64, Vec<u8>>::new();
        let bits_per_bitmap_block = u64::from(self.volume.block_size) * 8;
        let block_size = self.volume.block_size as usize;
        for block in descriptor
            .metadata_blocks
            .iter()
            .chain(descriptor.directory_blocks.iter())
        {
            if *block < self.volume.layout.data_start
                || *block >= self.volume.layout.backup_superblock
            {
                return Err(Error::InvalidInput(
                    "inventory descriptor references a block outside the data region",
                ));
            }
            if !claimed.insert(*block) {
                return Err(Error::InvalidInput(
                    "inventory descriptor contains overlapping block lists",
                ));
            }

            // Keep bitmap-cache memory bounded even when an inventory spans many
            // bitmap pages. Clearing is safe: it only trades cache hits for device reads.
            let bitmap_index = *block / bits_per_bitmap_block;
            if !allocation_bitmaps.contains_key(&bitmap_index)
                && allocation_bitmaps.len() >= INVENTORY_BITMAP_CACHE_PAGES
            {
                allocation_bitmaps.clear();
            }
            if let std::collections::btree_map::Entry::Vacant(entry) =
                allocation_bitmaps.entry(bitmap_index)
            {
                let base = bitmap_index
                    .checked_mul(bits_per_bitmap_block)
                    .ok_or(Error::ArithmeticOverflow)?;
                let bitmap_block = self
                    .volume
                    .layout
                    .bitmap_start
                    .checked_add(bitmap_index)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut bitmap = try_zeroed_block(
                    block_size,
                    "allocation failed while preparing persistent I/O buffer",
                )?;
                device.read_blocks(bitmap_block, &mut bitmap)?;
                let segment_start = self.volume.layout.data_start.max(base);
                let segment_end = self.volume.layout.backup_superblock.min(
                    base.checked_add(bits_per_bitmap_block)
                        .ok_or(Error::ArithmeticOverflow)?,
                );
                crate::allocator_extents::validate_bitmap_reserved_bits(
                    &bitmap,
                    base,
                    segment_start,
                    segment_end,
                )?;
                entry.insert(bitmap);
            }
            let bitmap = allocation_bitmaps
                .get(&bitmap_index)
                .ok_or(Error::Corrupt("allocation bitmap page cache is incomplete"))?;
            let local_bit = *block % bits_per_bitmap_block;
            let byte_index =
                usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte_index] & (1_u8 << (local_bit % 8)) == 0 {
                return Err(Error::InvalidInput(
                    "inventory descriptor references a free inventory block",
                ));
            }
        }
        // Validate the complete namespace and global block ownership before consuming
        // another block. Otherwise a descriptor could make a corrupt or ambiguous
        // inventory discoverable even though publication itself succeeded.
        let report = self.verify_persistent_inventory(
            device,
            descriptor.root_id,
            &descriptor.metadata_blocks,
            descriptor.metadata_record_count,
            &descriptor.directory_blocks,
            descriptor.directory_entry_count,
        )?;
        if !report.is_consistent() {
            return Err(Error::InvalidInput(
                "persistent inventory has unresolved allocation ownership anomalies",
            ));
        }

        // Prepare the descriptor in a free block and flush it before committing the
        // allocation bit. Discovery scans allocated blocks only, so the bitmap journal
        // commit is the visibility point: after recovery, either the block is still free
        // (and the staged bytes are ignored) or it is allocated with durable descriptor data.
        // A later allocation clears stale descriptor magic before reusing an uncommitted
        // staging block, preventing it from becoming a false positive.
        let layout = self.volume.layout;
        let bits_per_bitmap_block = u64::from(self.volume.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_bitmap_block;
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_bitmap_block;
        let mut selected = None;
        // Reuse one page buffer across the bitmap scan; every read fills a full block.
        let mut bitmap = try_zeroed_block(
            block_size,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let bitmap_base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let first_block = bitmap_base.max(layout.data_start);
            let end_block = bitmap_base
                .checked_add(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?
                .min(layout.backup_superblock);
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut candidate = None;
            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut valid_mask = u8::MAX;
                if byte_index == first_byte {
                    valid_mask &= u8::MAX << (local_start % 8);
                }
                if byte_index + 1 == end_byte && local_end % 8 != 0 {
                    valid_mask &= (1_u8 << (local_end % 8)) - 1;
                }
                let available = !*byte & valid_mask;
                if available != 0 {
                    candidate = Some((byte_index, available.trailing_zeros()));
                    break;
                }
            }
            if let Some((byte_index, bit)) = candidate {
                let local = (byte_index as u64) * 8 + u64::from(bit);
                let descriptor_block = bitmap_base + local;
                if claimed.contains(&descriptor_block) {
                    return Err(Error::Corrupt(
                        "allocator selected a block already owned by the inventory",
                    ));
                }
                bitmap[byte_index] |= 1_u8 << bit;
                selected = Some((descriptor_block, bitmap_block, bitmap));
                break;
            }
        }
        let (descriptor_block, bitmap_block, bitmap) =
            selected.ok_or(Error::InvalidInput("volume has no free data blocks"))?;
        device.write_blocks(descriptor_block, &encoded)?;
        device.flush()?;
        self.journal.commit(
            device,
            &[RedoUpdate {
                target_block: bitmap_block,
                contents: bitmap,
            }],
        )?;
        persist_inventory_hint(device, descriptor_block)?;
        self.inventory_descriptor_hint = Some(descriptor_block);
        self.inventory_descriptor_cache = Some(Some((descriptor_block, descriptor.clone())));
        Ok(descriptor_block)
    }

    /// Atomically replace a persistent descriptor and reconcile its block ownership.
    ///
    /// The replacement descriptor is verified against the current volume before any writes.
    /// Blocks removed from the inventory and the old descriptor are released, while a new
    /// descriptor block is allocated. A single journal transaction updates every affected
    /// bitmap page, and its capacity is checked before the new descriptor is staged. Recovery
    /// therefore exposes either the old or new descriptor with a matching ownership state.
    pub fn replace_persistent_inventory_descriptor<D: BlockDevice>(
        &mut self,
        device: &mut D,
        descriptor: &PersistentInventoryDescriptor,
    ) -> Result<u64> {
        self.validate_device(device)?;
        let (old_block, old_descriptor) =
            self.discover_persistent_inventory(device)?
                .ok_or(Error::InvalidInput(
                    "no persistent inventory descriptor exists to replace",
                ))?;

        if old_descriptor == *descriptor {
            // A retry after the bitmap transaction committed but locator publication failed
            // must remain idempotent and repair the hint without creating another descriptor.
            persist_inventory_hint(device, old_block)?;
            self.inventory_descriptor_hint = Some(old_block);
            self.inventory_descriptor_cache = Some(Some((old_block, old_descriptor)));
            return Ok(old_block);
        }

        let encoded = descriptor.encode(self.volume.block_size as usize)?;
        let report = self.verify_persistent_inventory(
            device,
            descriptor.root_id,
            &descriptor.metadata_blocks,
            descriptor.metadata_record_count,
            &descriptor.directory_blocks,
            descriptor.directory_entry_count,
        )?;

        // Under the proposed inventory, only the old descriptor and old inventory blocks
        // omitted from the replacement may become orphaned. Reject unrelated orphans,
        // dangling references, and truncated diagnostics rather than guessing ownership.
        let old_inventory_blocks = old_descriptor
            .metadata_blocks
            .iter()
            .chain(old_descriptor.directory_blocks.iter())
            .copied()
            .collect::<std::collections::BTreeSet<_>>();
        let new_inventory_blocks = descriptor
            .metadata_blocks
            .iter()
            .chain(descriptor.directory_blocks.iter())
            .copied()
            .collect::<std::collections::BTreeSet<_>>();
        let expected_releasable = old_inventory_blocks
            .difference(&new_inventory_blocks)
            .copied()
            .collect::<std::collections::BTreeSet<_>>();
        if report.dangling_reference_count() != 0
            || report.allocation.orphaned_allocations_truncated
            || report.allocation.orphaned_allocation_count
                != report.allocation.orphaned_allocations.len() as u64
            || !report.allocation.orphaned_allocations.contains(&old_block)
            || report
                .allocation
                .orphaned_allocations
                .iter()
                .any(|block| *block != old_block && !expected_releasable.contains(block))
        {
            return Err(Error::InvalidInput(
                "replacement inventory has unresolved allocation ownership anomalies",
            ));
        }
        let released_blocks = report
            .allocation
            .orphaned_allocations
            .iter()
            .copied()
            .filter(|block| *block != old_block)
            .collect::<std::collections::BTreeSet<_>>();

        let layout = self.volume.layout;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let bits_per_bitmap_block = u64::from(self.volume.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_bitmap_block;
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_bitmap_block;

        // The replacement descriptor may live on any bitmap page. Stage it while free;
        // the multi-page journal transaction below is the sole publication commit point.
        let mut new_block = None;
        let mut candidate_bitmap = None;
        for bitmap_index in first_bitmap..=last_bitmap {
            let mut bitmap = try_zeroed_block(
                block_size,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let first = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_bitmap_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let local_start = first - base;
            let local_end = end - base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut candidate_local = None;
            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut valid_mask = u8::MAX;
                if byte_index == first_byte {
                    valid_mask &= u8::MAX << (local_start % 8);
                }
                if byte_index + 1 == end_byte && local_end % 8 != 0 {
                    valid_mask &= (1_u8 << (local_end % 8)) - 1;
                }
                let available = !*byte & valid_mask;
                if available != 0 {
                    let local = (byte_index as u64) * 8 + u64::from(available.trailing_zeros());
                    candidate_local = Some(local);
                    break;
                }
            }
            if let Some(local) = candidate_local {
                new_block = Some(base + local);
                // Move the already-read page into the transaction cache without copying it.
                candidate_bitmap = Some((bitmap_index, bitmap));
                break;
            }
        }
        let new_block = new_block.ok_or(Error::InvalidInput(
            "volume has no free block for replacement descriptor",
        ))?;
        // Assemble one redo image per affected bitmap page. A single journal COMMIT makes
        // all ownership changes visible together, including inventory pages on distant
        // bitmap segments. Preflight the bounded ring before staging descriptor bytes, so
        // an oversized transition is rejected without modifying the image.
        let mut bitmaps = std::collections::BTreeMap::<u64, Vec<u8>>::new();
        if let Some((bitmap_index, bitmap)) = candidate_bitmap {
            bitmaps.insert(bitmap_index, bitmap);
        }
        let changed_blocks = std::iter::once((old_block, false))
            .chain(std::iter::once((new_block, true)))
            .chain(released_blocks.iter().copied().map(|block| (block, false)));
        for (block, allocate) in changed_blocks {
            if block < layout.data_start || block >= layout.backup_superblock {
                return Err(Error::Corrupt(
                    "replacement ownership transition references a block outside the data region",
                ));
            }
            let bitmap_index = block / bits_per_bitmap_block;
            if let std::collections::btree_map::Entry::Vacant(entry) = bitmaps.entry(bitmap_index) {
                let mut bitmap = try_zeroed_block(
                    block_size,
                    "allocation failed while preparing persistent I/O buffer",
                )?;
                let bitmap_block = layout
                    .bitmap_start
                    .checked_add(bitmap_index)
                    .ok_or(Error::ArithmeticOverflow)?;
                device.read_blocks(bitmap_block, &mut bitmap)?;
                entry.insert(bitmap);
            }
            let bitmap = bitmaps
                .get_mut(&bitmap_index)
                .ok_or(Error::Corrupt("staged allocation bitmap page is missing"))?;
            let local = block % bits_per_bitmap_block;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let mask = 1_u8 << (local % 8);
            if allocate {
                if bitmap[byte] & mask != 0 {
                    return Err(Error::Corrupt(
                        "replacement descriptor candidate is already allocated",
                    ));
                }
                bitmap[byte] |= mask;
            } else {
                if bitmap[byte] & mask == 0 {
                    return Err(Error::Corrupt(
                        "replacement ownership block is already free",
                    ));
                }
                bitmap[byte] &= !mask;
            }
        }
        let mut updates = Vec::new();
        updates.try_reserve_exact(bitmaps.len()).map_err(|_| {
            Error::InvalidInput("allocation failed while staging persistent namespace data")
        })?;
        for (bitmap_index, contents) in bitmaps {
            let target_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            updates.push(RedoUpdate {
                target_block,
                contents,
            });
        }
        self.journal.required_record_count(device, updates.len())?;

        // Stage the complete descriptor only after the whole ownership transaction is
        // known to fit. It remains invisible while its allocation bit is still clear.
        device.write_blocks(new_block, &encoded)?;
        device.flush()?;

        // An I/O error from commit can leave its decision uncertain. Drop all cached
        // discovery state; callers must reopen and let journal recovery choose old or new.
        self.inventory_descriptor_cache = None;
        // Exact commit point: journal.commit flushes the COMMIT record before applying
        // home bitmap pages. Before that durable flush recovery retains old ownership;
        // after it, recovery must replay every bitmap image even if a later write/flush fails.
        self.journal.commit(device, &updates)?;
        persist_inventory_hint(device, new_block)?;
        self.inventory_descriptor_hint = Some(new_block);
        self.inventory_descriptor_cache = Some(Some((new_block, descriptor.clone())));
        Ok(new_block)
    }

    /// Discover a unique persistent inventory descriptor by scanning allocated data blocks.
    ///
    /// The superblock has no fixed inventory region, so discovery scans allocated data
    /// blocks for the descriptor magic. It rejects multiple
    /// descriptors, malformed descriptor records, unallocated inventory blocks, and aliases
    /// between the descriptor itself and its inventory lists.
    pub fn discover_persistent_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
    ) -> Result<Option<(u64, PersistentInventoryDescriptor)>> {
        self.validate_device(device)?;
        // Validate reserved and padding bits before trusting any allocation entry.
        // Do not materialize allocated_data_block_inventory here: on a large volume that
        // vector scales with every allocated block, even though discovery only needs a
        // bounded batch of candidate blocks at a time.
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;

        // A locator narrows the likely descriptor location, but cannot establish uniqueness:
        // another allocated block may contain a second valid descriptor. Discovery therefore
        // scans allocated data blocks authoritatively. Avoid validating the hinted descriptor
        // and reconstructing its namespace here, because the scan below performs those checks
        // and would otherwise repeat the same I/O and parsing work.
        let bits_per_bitmap_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_bitmap_block;
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_bitmap_block;
        let mut discovered = None;
        let mut pending_blocks: Vec<u64> = Vec::new();
        pending_blocks
            .try_reserve_exact(INVENTORY_DISCOVERY_BATCH_BLOCKS)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while preparing inventory discovery batch")
            })?;
        // Descriptor references often share bitmap pages. Keep only the pages actually
        // needed to validate a candidate descriptor's inventory references.
        let mut allocation_bitmaps = std::collections::BTreeMap::<u64, Vec<u8>>::new();

        let mut inspect_batch = |blocks: &[u64], device: &mut D| -> Result<()> {
            if blocks.is_empty() {
                return Ok(());
            }
            let first_block = blocks[0];
            let byte_count = blocks
                .len()
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut batch = try_zeroed_block(
                byte_count,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            device.read_blocks(first_block, &mut batch)?;

            for (offset, block) in blocks.iter().copied().enumerate() {
                let byte_start = offset
                    .checked_mul(block_size)
                    .ok_or(Error::ArithmeticOverflow)?;
                let byte_end = byte_start
                    .checked_add(block_size)
                    .ok_or(Error::ArithmeticOverflow)?;
                let block_bytes = batch.get(byte_start..byte_end).ok_or(Error::Corrupt(
                    "inventory discovery batch bounds are invalid",
                ))?;
                if block_bytes.get(..INVENTORY_DESCRIPTOR_MAGIC.len())
                    != Some(INVENTORY_DESCRIPTOR_MAGIC.as_slice())
                {
                    continue;
                }
                let descriptor = PersistentInventoryDescriptor::decode(block_bytes)?;
                if descriptor.metadata_blocks.contains(&block)
                    || descriptor.directory_blocks.contains(&block)
                {
                    return Err(Error::Corrupt(
                        "inventory descriptor aliases one of its inventory blocks",
                    ));
                }
                let mut unique_references = std::collections::BTreeSet::new();
                for inventory_block in descriptor
                    .metadata_blocks
                    .iter()
                    .chain(descriptor.directory_blocks.iter())
                {
                    if !unique_references.insert(*inventory_block) {
                        return Err(Error::Corrupt(
                            "inventory descriptor contains duplicate block ownership",
                        ));
                    }
                    if *inventory_block < layout.data_start
                        || *inventory_block >= layout.backup_superblock
                    {
                        return Err(Error::Corrupt(
                            "inventory descriptor references a block outside the data region",
                        ));
                    }
                    let bitmap_index = *inventory_block / bits_per_bitmap_block;
                    // Descriptor references can span the whole data region. Bound retained
                    // bitmap pages independently from the descriptor's number of references.
                    if !allocation_bitmaps.contains_key(&bitmap_index)
                        && allocation_bitmaps.len() >= INVENTORY_BITMAP_CACHE_PAGES
                    {
                        allocation_bitmaps.clear();
                    }
                    if let std::collections::btree_map::Entry::Vacant(entry) =
                        allocation_bitmaps.entry(bitmap_index)
                    {
                        let mut bitmap = try_zeroed_block(
                            block_size,
                            "allocation failed while preparing persistent I/O buffer",
                        )?;
                        let bitmap_block = layout
                            .bitmap_start
                            .checked_add(bitmap_index)
                            .ok_or(Error::ArithmeticOverflow)?;
                        device.read_blocks(bitmap_block, &mut bitmap)?;
                        entry.insert(bitmap);
                    }
                    let bitmap = allocation_bitmaps
                        .get(&bitmap_index)
                        .ok_or(Error::Corrupt("allocation bitmap page cache is incomplete"))?;
                    let local_bit = *inventory_block % bits_per_bitmap_block;
                    let byte_index =
                        usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
                    if bitmap[byte_index] & (1_u8 << (local_bit % 8)) == 0 {
                        return Err(Error::Corrupt(
                            "inventory descriptor references a free inventory block",
                        ));
                    }
                }
                // Apply the same namespace validation to fallback discovery as to the
                // locator fast path. Otherwise a stale hint could turn a rejected
                // descriptor into an accepted one merely by forcing a full scan.
                if descriptor.metadata_record_count != 0 || descriptor.directory_entry_count != 0 {
                    self.reconstruct_namespace(
                        device,
                        descriptor.root_id,
                        &descriptor.metadata_blocks,
                        descriptor.metadata_record_count,
                        &descriptor.directory_blocks,
                        descriptor.directory_entry_count,
                    )
                    .map_err(|_| {
                        Error::Corrupt(
                            "persistent inventory descriptor contains an invalid namespace",
                        )
                    })?;
                }
                if discovered.is_some() {
                    return Err(Error::Corrupt(
                        "multiple persistent inventory descriptors were discovered",
                    ));
                }
                discovered = Some((block, descriptor));
            }
            Ok(())
        };

        // Walk bitmap pages and set bits directly. At most one bitmap page, one cached
        // descriptor-reference page per touched bitmap, and 64 candidate block numbers
        // are retained; the number of allocated blocks no longer controls peak memory.
        // Keep the page buffer outside the loop to avoid one allocation per bitmap page.
        let mut bitmap = try_zeroed_block(
            block_size,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_bitmap_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_offset, bitmap_byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let byte_start = u64::try_from(byte_index)
                    .map_err(|_| Error::ArithmeticOverflow)?
                    .checked_mul(8)
                    .ok_or(Error::ArithmeticOverflow)?;
                let first_valid_bit = first_local.saturating_sub(byte_start).min(8);
                let valid_bits = end_local.saturating_sub(byte_start).min(8);
                let lower_mask = u8::MAX << first_valid_bit;
                let upper_mask = if valid_bits == 8 {
                    u8::MAX
                } else {
                    (1_u8 << valid_bits) - 1
                };
                let mut bits = *bitmap_byte & lower_mask & upper_mask;
                while bits != 0 {
                    let bit = u64::from(bits.trailing_zeros());
                    let block = base
                        .checked_add(byte_start)
                        .and_then(|value| value.checked_add(bit))
                        .ok_or(Error::ArithmeticOverflow)?;
                    if pending_blocks
                        .last()
                        .is_some_and(|previous| previous.checked_add(1) != Some(block))
                        || pending_blocks.len() == INVENTORY_DISCOVERY_BATCH_BLOCKS
                    {
                        inspect_batch(&pending_blocks, device)?;
                        pending_blocks.clear();
                    }
                    pending_blocks.push(block);
                    bits &= bits - 1;
                }
            }
        }
        inspect_batch(&pending_blocks, device)?;
        drop(inspect_batch);
        Ok(discovered)
    }

    /// Discover the persistent descriptor and reconstruct its namespace in one operation.
    ///
    /// Returns None when no descriptor exists. A malformed or ambiguous descriptor is
    /// an error; a discovered descriptor is never guessed from partial record streams.
    pub fn reconstruct_discovered_namespace<D: BlockDevice>(
        &self,
        device: &mut D,
    ) -> Result<Option<(u64, Namespace)>> {
        let Some((descriptor_block, descriptor)) = self.discover_persistent_inventory(device)?
        else {
            return Ok(None);
        };
        let namespace = self.reconstruct_namespace(
            device,
            descriptor.root_id,
            &descriptor.metadata_blocks,
            descriptor.metadata_record_count,
            &descriptor.directory_blocks,
            descriptor.directory_entry_count,
        )?;
        Ok(Some((descriptor_block, namespace)))
    }

    /// Resolve a path in the discovered on-disk namespace and return its metadata.
    ///
    /// This is a convenience bridge for callers that should not have to locate the
    /// inventory descriptor or reconstruct the namespace themselves. Discovery and
    /// validation are performed on each call; this does not retain stale metadata
    /// across mutations or claim to provide a cached lookup.
    pub fn stat_persistent_path<D: BlockDevice>(
        &self,
        device: &mut D,
        path: &str,
    ) -> Result<MetadataRecord> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent namespace is not initialized",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        namespace
            .object(object_id)
            .cloned()
            .ok_or(Error::Corrupt("resolved path has no metadata record"))
    }

    /// List one page of a persistent directory by its absolute or relative path.
    ///
    /// Entries are returned in lexical name order. The exclusive `after_name` cursor
    /// and nonzero `limit` follow the in-memory namespace paging contract. The
    /// descriptor and complete namespace are rediscovered and validated on every call.
    pub fn list_persistent_directory<D: BlockDevice>(
        &self,
        device: &mut D,
        directory_path: &str,
        after_name: Option<&str>,
        limit: usize,
    ) -> Result<Vec<DirectoryEntry>> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent namespace is not initialized",
            ));
        };
        let directory_id = namespace.resolve_path(directory_path)?;
        namespace.list_directory(directory_id, after_name, limit)
    }

    /// Create an empty file beneath a directory resolved by its persistent path.
    ///
    /// This path-oriented entry point complements the object-ID API for callers such as
    /// command-line tools and future OS adapters. It resolves the parent and publishes the
    /// updated inventory after Namespace::insert validates the new entry.
    /// Create multiple empty files/directories in one persistent namespace publication.
    ///
    /// Each tuple is (parent object ID, new object ID, kind, name). A parent may be an
    /// existing directory or a directory created earlier in this same batch, allowing a
    /// shallow-to-deep tree import without publishing each intermediate state. All changes
    /// are validated in memory, then made visible by one copy-on-write inventory replacement.
    /// Only regular files and directories are supported. An empty batch is a no-op.
    pub fn create_persistent_objects<D: BlockDevice>(
        &mut self,
        device: &mut D,
        objects: &[(ObjectId, ObjectId, crate::ObjectKind, String)],
    ) -> Result<Vec<ObjectId>> {
        if objects.is_empty() {
            return Ok(Vec::new());
        }
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "no persistent namespace exists for object creation",
            ));
        };
        self.create_persistent_objects_in_namespace(device, namespace, objects)
    }

    /// Create several empty files/directories under one persistent parent path.
    ///
    /// This convenience form resolves the parent once; see create_persistent_objects for
    /// batches that include children under different parents.
    pub fn create_persistent_objects_at_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        parent_path: &str,
        objects: &[(ObjectId, crate::ObjectKind, String)],
    ) -> Result<Vec<ObjectId>> {
        if objects.is_empty() {
            return Ok(Vec::new());
        }
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "no persistent namespace exists for object creation",
            ));
        };
        let parent_id = namespace.resolve_path(parent_path)?;
        let mut expanded = Vec::new();
        expanded.try_reserve_exact(objects.len()).map_err(|_| {
            Error::InvalidInput("allocation failed while expanding persistent object batch")
        })?;
        for (object_id, kind, name) in objects {
            expanded.push((
                parent_id,
                *object_id,
                *kind,
                try_clone_string(
                    name,
                    "allocation failed while copying persistent object name",
                )?,
            ));
        }
        self.create_persistent_objects_in_namespace(device, namespace, &expanded)
    }

    fn create_persistent_objects_in_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        mut namespace: Namespace,
        objects: &[(ObjectId, ObjectId, crate::ObjectKind, String)],
    ) -> Result<Vec<ObjectId>> {
        let mut created = Vec::new();
        created.try_reserve_exact(objects.len()).map_err(|_| {
            Error::InvalidInput("allocation failed while staging persistent namespace data")
        })?;
        let mut seen_ids = Vec::new();
        seen_ids.try_reserve_exact(objects.len()).map_err(|_| {
            Error::InvalidInput("allocation failed while validating persistent object IDs")
        })?;
        seen_ids.extend(objects.iter().map(|(_, object_id, _, _)| *object_id));
        seen_ids.sort_unstable();
        if seen_ids.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::InvalidInput(
                "an object ID may be created only once per batch",
            ));
        }
        for (parent_id, object_id, kind, name) in objects {
            if !matches!(kind, crate::ObjectKind::File | crate::ObjectKind::Directory) {
                return Err(Error::InvalidInput(
                    "persistent creation supports only files and directories",
                ));
            }
            let record = MetadataRecord {
                id: *object_id,
                parent_id: parent_id.0,
                kind: *kind,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            namespace.insert(
                DirectoryEntry {
                    parent_id: parent_id.0,
                    object_id: object_id.0,
                    kind: *kind,
                    name: name.clone(),
                },
                record,
            )?;
            created.push(*object_id);
        }
        // The sole publication is the visibility point for the whole batch: callers
        // observe either the previous inventory or all requested children. A reported
        // I/O failure can still occur after the descriptor switch became durable.
        if let Err(error) = self.replace_persistent_namespace_inventory(device, &namespace) {
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if objects.iter().all(|(parent_id, object_id, kind, name)| {
                        active.object(*object_id).is_some_and(|record| {
                            record.parent_id == parent_id.0
                                && record.kind == *kind
                                && record.size == 0
                                && record.extent_root_block.is_none()
                                && record.generation == 1
                                && active.lookup(*parent_id, name)
                                    .is_some_and(|entry| {
                                        entry.object_id == object_id.0 && entry.kind == *kind
                                    })
                        })
                    }) => return Ok(created),
                Ok(Some((_, active)))
                    if objects.iter().all(|(_, object_id, _, _)| active.object(*object_id).is_none()) =>
                {
                    return Err(error);
                }
                _ => return Err(Error::Corrupt(
                    "batch creation publication outcome is uncertain; preserve the image and recover before further writes",
                )),
            }
        }
        Ok(created)
    }

    /// Create an empty file beneath a directory resolved by its persistent path.
    pub fn create_persistent_file_at_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        parent_path: &str,
        object_id: ObjectId,
        name: &str,
    ) -> Result<ObjectId> {
        self.create_persistent_objects_at_path(
            device,
            parent_path,
            &[(object_id, crate::ObjectKind::File, name.to_owned())],
        )?
        .into_iter()
        .next()
        .ok_or(Error::Corrupt("single-file creation returned no object ID"))
    }

    /// Create an empty directory beneath a directory resolved by its persistent path.
    pub fn create_persistent_directory_at_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        parent_path: &str,
        object_id: ObjectId,
        name: &str,
    ) -> Result<ObjectId> {
        self.create_persistent_objects_at_path(
            device,
            parent_path,
            &[(object_id, crate::ObjectKind::Directory, name.to_owned())],
        )?
        .into_iter()
        .next()
        .ok_or(Error::Corrupt(
            "single-directory creation returned no object ID",
        ))
    }

    /// Remove a persistent file or empty directory addressed by its path.
    ///
    /// Resolution is performed against the currently published namespace; the object-ID
    /// removal implementation then re-discovers the inventory before mutating storage.
    pub fn remove_persistent_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
    ) -> Result<()> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent removal requires a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        self.remove_persistent_object_from_namespace(device, namespace, object_id)
    }

    /// Rename or move an object using paths for both its current and destination parent.
    pub fn rename_persistent_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
        new_parent_path: &str,
        new_name: &str,
    ) -> Result<()> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent rename requires a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        let new_parent_id = namespace.resolve_path(new_parent_path)?;
        self.rename_persistent_object_from_namespace(
            device,
            namespace,
            object_id,
            new_parent_id,
            new_name,
        )
    }

    /// Rename or move several objects with a single namespace publication.
    ///
    /// Each tuple is (object ID, destination parent ID, new name). Changes are applied
    /// to a reconstructed in-memory namespace first; a failed validation leaves storage
    /// untouched, and a valid batch is published once rather than once per rename.
    pub fn rename_persistent_objects<D: BlockDevice>(
        &mut self,
        device: &mut D,
        changes: &[(ObjectId, ObjectId, String)],
    ) -> Result<()> {
        if changes.is_empty() {
            return Ok(());
        }
        let Some((_, mut namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent rename requires a published inventory",
            ));
        };

        // A single object may appear only once: otherwise the requested final state
        // is ambiguous and "all changes committed" cannot be established after an I/O error.
        let original_entries = namespace
            .entries()
            .map(|entry| (entry.object_id, (entry.parent_id, entry.name.clone())))
            .collect::<std::collections::BTreeMap<_, _>>();
        let mut seen = std::collections::BTreeSet::new();
        let mut original_locations = Vec::new();
        original_locations
            .try_reserve_exact(changes.len())
            .map_err(|_| {
                Error::InvalidInput("allocation failed while staging persistent namespace data")
            })?;
        for (object_id, _, _) in changes {
            if !seen.insert(*object_id) {
                return Err(Error::InvalidInput(
                    "an object may be renamed only once per batch",
                ));
            }
            let record = namespace
                .object(*object_id)
                .ok_or(Error::InvalidInput("object does not exist"))?;
            let (parent_id, name) = original_entries
                .get(&object_id.0)
                .ok_or(Error::Corrupt("renamed object has no directory entry"))?;
            if *parent_id != record.parent_id {
                return Err(Error::Corrupt(
                    "renamed object's entry disagrees with its metadata",
                ));
            }
            original_locations.push((*object_id, record.parent_id, name.clone()));
        }

        for (object_id, parent_id, name) in changes {
            namespace.rename(*object_id, *parent_id, name)?;
        }
        if let Err(error) = self.replace_persistent_namespace_inventory(device, &namespace) {
            // A late I/O error may follow a durable descriptor switch. Distinguish a
            // committed batch, a definitely uncommitted batch, and an uncertain image.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if changes.iter().all(|(object_id, parent_id, name)| {
                        active.object(*object_id).is_some_and(|record| {
                            record.parent_id == parent_id.0
                                && active.lookup(*parent_id, name)
                                    .is_some_and(|entry| entry.object_id == object_id.0)
                        })
                    }) => return Ok(()),
                Ok(Some((_, active)))
                    if original_locations.iter().all(|(object_id, parent_id, name)| {
                        active.object(*object_id).is_some_and(|record| {
                            record.parent_id == *parent_id
                                && active.lookup(ObjectId(*parent_id), name)
                                    .is_some_and(|entry| entry.object_id == object_id.0)
                        })
                    }) => return Err(error),
                _ => return Err(Error::Corrupt(
                    "batch rename publication outcome is uncertain; preserve the image and recover before further writes",
                )),
            }
        }
        Ok(())
    }

    /// Remove multiple files or empty directories with one namespace publication.
    ///
    /// File data and extent-map roots are reclaimed only after the new namespace is
    /// discoverable, so a failed pre-publication validation cannot free live blocks.
    pub fn remove_persistent_objects<D: BlockDevice>(
        &mut self,
        device: &mut D,
        object_ids: &[ObjectId],
    ) -> Result<()> {
        if object_ids.is_empty() {
            return Ok(());
        }
        let Some((_, mut namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent removal requires a published inventory",
            ));
        };

        // Collect and validate every target against the same original namespace before
        // mutating the in-memory view. This also lets us load extent maps only once.
        let mut records = Vec::new();
        records.try_reserve_exact(object_ids.len()).map_err(|_| {
            Error::InvalidInput("allocation failed while staging persistent namespace data")
        })?;
        let mut removal_order = Vec::new();
        removal_order
            .try_reserve_exact(object_ids.len())
            .map_err(|_| {
                Error::InvalidInput("allocation failed while staging persistent namespace data")
            })?;
        let mut seen = std::collections::BTreeSet::new();
        let mut needs_extent_maps = false;
        for object_id in object_ids {
            if !seen.insert(*object_id) {
                return Err(Error::InvalidInput(
                    "an object may be removed only once per batch",
                ));
            }
            if *object_id == namespace.root_id() {
                return Err(Error::InvalidInput("the namespace root cannot be removed"));
            }
            let record = namespace
                .object(*object_id)
                .cloned()
                .ok_or(Error::InvalidInput("object does not exist"))?;
            let depth = namespace
                .path_of(*object_id)?
                .split('/')
                .filter(|component| !component.is_empty())
                .count();
            removal_order.push((depth, *object_id));
            if record.kind == crate::ObjectKind::File {
                if record.extent_root_block.is_some() {
                    needs_extent_maps = true;
                } else if record.size != 0 {
                    return Err(Error::Corrupt(
                        "non-empty file is missing its extent-map root",
                    ));
                }
            }
            records.push(record);
        }
        // Process descendants before their ancestors, so a batch can remove a whole
        // subtree regardless of the caller's input order. Unrequested children still
        // make Namespace::remove reject their parent.
        removal_order.sort_by_key(|(depth, _)| std::cmp::Reverse(*depth));

        let extent_maps = if needs_extent_maps {
            Some(self.read_namespace_extent_maps(device, &namespace)?)
        } else {
            None
        };
        // Count the complete reclamation set before materializing block IDs. Large
        // removals can cover many thousands of blocks; exact reservation avoids repeated
        // Vec growth while preserving the all-or-nothing validation path below.
        let reclaim_capacity = records.iter().try_fold(0_u64, |capacity, record| {
            let Some(_) = record.extent_root_block else {
                return Ok(capacity);
            };
            let maps = extent_maps
                .as_ref()
                .ok_or(Error::Corrupt("file extent maps were not loaded"))?;
            let map = maps
                .get(&record.id)
                .ok_or(Error::Corrupt("file extent map is missing"))?;
            let data_blocks = map.extents().iter().try_fold(0_u64, |count, extent| {
                count
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)
            })?;
            capacity
                .checked_add(data_blocks)
                .and_then(|count| count.checked_add(1))
                .ok_or(Error::ArithmeticOverflow)
        })?;
        let reclaim_capacity =
            usize::try_from(reclaim_capacity).map_err(|_| Error::ArithmeticOverflow)?;
        let mut reclaim_blocks = Vec::new();
        reclaim_blocks
            .try_reserve_exact(reclaim_capacity)
            .map_err(|_| Error::InvalidInput("unable to reserve old file block inventory"))?;
        for record in &records {
            if let Some(root) = record.extent_root_block {
                let maps = extent_maps
                    .as_ref()
                    .ok_or(Error::Corrupt("file extent maps were not loaded"))?;
                let map = maps
                    .get(&record.id)
                    .ok_or(Error::Corrupt("file extent map is missing"))?;
                for extent in map.extents() {
                    for delta in 0..extent.block_count {
                        let block = extent
                            .physical_start
                            .checked_add(delta)
                            .ok_or(Error::ArithmeticOverflow)?;
                        reclaim_blocks.try_reserve(1).map_err(|_| {
                            Error::InvalidInput(
                                "allocation failed while staging reclaimed file blocks",
                            )
                        })?;
                        reclaim_blocks.push(block);
                    }
                }
                reclaim_blocks.try_reserve(1).map_err(|_| {
                    Error::InvalidInput("allocation failed while staging reclaimed extent roots")
                })?;
                reclaim_blocks.push(root);
            }
        }
        // No persistent block is reclaimed until every requested removal validates.
        for (_, object_id) in &removal_order {
            namespace.remove(*object_id)?;
        }
        if let Err(error) = self.replace_persistent_namespace_inventory(device, &namespace) {
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if object_ids.iter().all(|id| active.object(*id).is_none()) =>
                {
                    self.reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
                        .map_err(|_| Error::Corrupt(
                            "objects were removed but some blocks could not be reclaimed",
                        ))?;
                    return Ok(());
                }
                Ok(Some((_, active)))
                    if object_ids.iter().all(|id| active.object(*id).is_some()) =>
                {
                    return Err(error);
                }
                _ => return Err(Error::Corrupt(
                    "batch removal publication outcome is uncertain; preserve the image and recover before further writes",
                )),
            }
        }
        self.reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .map_err(|_| {
                Error::Corrupt("objects were removed but some blocks could not be reclaimed")
            })?;
        Ok(())
    }

    /// Initialize the first durable namespace on a freshly formatted volume.
    ///
    /// A single root-directory metadata record is staged and flushed before its inventory
    /// descriptor is published. The descriptor's bitmap commit is the visibility point,
    /// so a crash leaves either no discoverable namespace or a complete root namespace.
    /// This operation refuses to replace an existing inventory.
    pub fn initialize_persistent_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        root_id: ObjectId,
    ) -> Result<()> {
        self.validate_device(device)?;
        ObjectId::new(root_id.0)?;
        if self.discover_persistent_inventory(device)?.is_some() {
            return Err(Error::InvalidInput("a persistent namespace already exists"));
        }

        let root = MetadataRecord {
            id: root_id,
            parent_id: 0,
            kind: crate::ObjectKind::Directory,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        };
        let encoded = root.encode()?;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let mut contents = try_zeroed_block(
            block_size,
            "unable to allocate persistent root metadata buffer",
        )?;
        contents[..encoded.len()].copy_from_slice(&encoded);

        // Allocate and flush the root record before making it discoverable. Until the
        // descriptor is committed, this block is an unreferenced staging allocation.
        let root_block = self.allocate_block(device)?;
        if let Err(error) = (|| -> Result<()> {
            device.write_blocks(root_block, &contents)?;
            device.flush()
        })() {
            if self
                .reclaim_persistent_blocks(device, &[root_block])
                .is_err()
            {
                return Err(Error::Corrupt(
                    "failed to reclaim staged root metadata after initialization I/O failure",
                ));
            }
            return Err(error);
        }

        let descriptor = PersistentInventoryDescriptor {
            root_id,
            metadata_blocks: try_single_block_list(
                root_block,
                "allocation failed while staging persistent root inventory",
            )?,
            metadata_record_count: 1,
            directory_blocks: Vec::new(),
            directory_entry_count: 0,
        };
        if let Err(error) = self.publish_persistent_inventory(device, &descriptor) {
            // The descriptor publication may have committed before a later flush or
            // locator-hint error was reported. Never free the root record until discovery
            // proves that no active inventory refers to it.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if active.root_id() == root_id
                        && active.objects().count() == 1
                        && active.object(root_id).is_some_and(|record| record == &root) =>
                {
                    return Ok(());
                }
                Ok(None) => {
                    if self
                        .reclaim_persistent_blocks(device, &[root_block])
                        .is_err()
                    {
                        return Err(Error::Corrupt(
                            "namespace initialization failed and staged root metadata could not be reclaimed",
                        ));
                    }
                    return Err(error);
                }
                _ => {
                    // Discovery itself is inconclusive, so retain the staged block rather
                    // than risk freeing metadata that a committed descriptor may reference.
                    return Err(error);
                }
            }
        }
        Ok(())
    }

    /// Create an empty file in the persistent namespace.
    ///
    /// The namespace inventory is reconstructed from disk, updated in memory, and then
    /// published through copy-on-write inventory replacement. This makes the empty file
    /// discoverable after reopening the volume. File data allocation and non-empty writes
    /// are deliberately separate operations and are not performed here.
    pub fn create_persistent_file<D: BlockDevice>(
        &mut self,
        device: &mut D,
        parent_id: ObjectId,
        object_id: ObjectId,
        name: &str,
    ) -> Result<ObjectId> {
        let (_, mut namespace) =
            self.reconstruct_discovered_namespace(device)?
                .ok_or(Error::InvalidInput(
                    "no persistent namespace exists for file creation",
                ))?;
        let record = MetadataRecord {
            id: object_id,
            parent_id: parent_id.0,
            kind: crate::ObjectKind::File,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        };
        let entry = DirectoryEntry {
            parent_id: parent_id.0,
            object_id: object_id.0,
            kind: crate::ObjectKind::File,
            name: name.to_owned(),
        };
        namespace.insert(entry, record)?;
        self.replace_persistent_namespace_inventory(device, &namespace)?;
        Ok(object_id)
    }

    /// Create an empty directory in the persistent namespace.
    ///
    /// The directory entry and metadata record are validated together and published as a
    /// copy-on-write inventory, so the directory becomes visible only when the new inventory
    /// descriptor is committed. Directory creation allocates no data extents.
    pub fn create_persistent_directory<D: BlockDevice>(
        &mut self,
        device: &mut D,
        parent_id: ObjectId,
        object_id: ObjectId,
        name: &str,
    ) -> Result<ObjectId> {
        let (_, mut namespace) =
            self.reconstruct_discovered_namespace(device)?
                .ok_or(Error::InvalidInput(
                    "no persistent namespace exists for directory creation",
                ))?;
        let record = MetadataRecord {
            id: object_id,
            parent_id: parent_id.0,
            kind: crate::ObjectKind::Directory,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        };
        let entry = DirectoryEntry {
            parent_id: parent_id.0,
            object_id: object_id.0,
            kind: crate::ObjectKind::Directory,
            name: name.to_owned(),
        };
        namespace.insert(entry, record)?;
        self.replace_persistent_namespace_inventory(device, &namespace)?;
        Ok(object_id)
    }

    /// Reclaim a set of blocks with one journal transaction per bitmap segment.
    ///
    /// File deletion, recursive removal, truncation, and replacement can retire many
    /// blocks at once. Freeing each block independently needlessly rewrites the same
    /// bitmap and journal for every block. Validate the complete list before mutating it,
    /// then delegate each segment to the allocator's atomic batch-free operation.
    fn reclaim_persistent_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        blocks: &[u64],
    ) -> Result<()> {
        let mut sorted = try_copy_block_numbers(
            blocks,
            "allocation failed while staging persistent block reclamation",
        )?;
        self.reclaim_persistent_blocks_in_place(device, &mut sorted)
    }

    /// Reclaim a caller-owned block list without allocating a second list proportional
    /// to the file size. Callers retain ownership of the list, so sorting it in place is safe.
    fn reclaim_persistent_blocks_in_place<D: BlockDevice>(
        &mut self,
        device: &mut D,
        sorted: &mut [u64],
    ) -> Result<()> {
        if sorted.is_empty() {
            return Ok(());
        }
        let layout = self.volume.layout;
        let bits_per_block = u64::from(self.volume.block_size)
            .checked_mul(8)
            .ok_or(Error::ArithmeticOverflow)?;
        sorted.sort_unstable();
        if sorted.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::Corrupt(
                "persistent storage reclamation contains duplicate blocks",
            ));
        }
        // Validate every physical address before freeing the first bitmap segment.
        // This prevents a malformed later segment from causing avoidable partial cleanup.
        if sorted
            .iter()
            .any(|block| *block < layout.data_start || *block >= layout.backup_superblock)
        {
            return Err(Error::Corrupt(
                "persistent storage reclamation contains a non-data block",
            ));
        }
        // Preflight the complete reclamation set before the first bitmap write.
        // Segment-local validation alone can leave earlier segments freed if a later
        // segment contains an already-free block or a block still owned by the inventory.
        self.ensure_inventory_blocks_releasable(device, sorted)?;

        // Preflight allocation state once per bitmap segment, not once per block.
        // Reclamation can contain thousands of blocks; a per-block bitmap read would
        // turn this safety check into thousands of avoidable device reads.
        let mut bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        let mut checked_start = 0;
        while checked_start < sorted.len() {
            let bitmap_index = sorted[checked_start] / bits_per_block;
            let mut checked_end = checked_start + 1;
            while checked_end < sorted.len() {
                if sorted[checked_end] / bits_per_block != bitmap_index {
                    break;
                }
                checked_end += 1;
            }
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let segment_start = layout.data_start.max(base);
            let segment_end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            crate::allocator_extents::validate_bitmap_reserved_bits(
                &bitmap,
                base,
                segment_start,
                segment_end,
            )?;
            // Check whole contiguous runs through the shared byte-mask helper.
            // This keeps the global all-segments preflight intact while avoiding
            // per-block bit arithmetic for large, mostly contiguous files.
            let segment_blocks = &sorted[checked_start..checked_end];
            let mut run_start = segment_blocks[0];
            let mut previous = segment_blocks[0];
            for &block in segment_blocks.iter().skip(1) {
                if previous.checked_add(1) == Some(block) {
                    previous = block;
                    continue;
                }
                if !crate::allocator_extents::bitmap_range_matches(
                    &bitmap,
                    run_start - base,
                    previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - base,
                    true,
                )? {
                    return Err(Error::Corrupt(
                        "persistent storage reclamation contains an already-free block",
                    ));
                }
                run_start = block;
                previous = block;
            }
            if !crate::allocator_extents::bitmap_range_matches(
                &bitmap,
                run_start - base,
                previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - base,
                true,
            )? {
                return Err(Error::Corrupt(
                    "persistent storage reclamation contains an already-free block",
                ));
            }
            checked_start = checked_end;
        }

        // Check inventory descriptor protection once per contiguous run, matching the
        // allocator's own batch-free validation and avoiding one descriptor read per block.
        let mut run_start = sorted[0];
        let mut previous = sorted[0];
        for &block in sorted.iter().skip(1) {
            if previous.checked_add(1) == Some(block) {
                previous = block;
                continue;
            }
            crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
                device,
                run_start,
                previous.checked_add(1).ok_or(Error::ArithmeticOverflow)?,
                self.volume.block_size,
            )?;
            run_start = block;
            previous = block;
        }
        crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
            device,
            run_start,
            previous.checked_add(1).ok_or(Error::ArithmeticOverflow)?,
            self.volume.block_size,
        )?;

        let mut start = 0;
        while start < sorted.len() {
            let bitmap_segment = sorted[start] / bits_per_block;
            let mut end = start + 1;
            while end < sorted.len() && sorted[end] / bits_per_block == bitmap_segment {
                end += 1;
            }
            self.free_blocks_sorted(device, &sorted[start..end])?;
            start = end;
        }
        Ok(())
    }

    /// Remove a persistent file or an empty directory.
    ///
    /// For files with allocated data, the validated extent map is captured first. The
    /// namespace removal is published before any file block is freed, so a failed or
    /// uncertain publication cannot leave a still-referenced block available to the allocator.
    pub fn remove_persistent_object<D: BlockDevice>(
        &mut self,
        device: &mut D,
        object_id: ObjectId,
    ) -> Result<()> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent removal requires a published inventory",
            ));
        };
        self.remove_persistent_object_from_namespace(device, namespace, object_id)
    }

    /// Remove an object using a namespace that has already been reconstructed by the caller.
    /// Keeping the publication/reclamation sequence shared avoids a second full namespace scan
    /// for path-oriented operations without weakening the post-publication recovery checks.
    fn remove_persistent_object_from_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        mut namespace: Namespace,
        object_id: ObjectId,
    ) -> Result<()> {
        self.validate_device(device)?;
        let record = namespace
            .object(object_id)
            .cloned()
            .ok_or(Error::InvalidInput("object does not exist"))?;
        let mut reclaim_blocks = Vec::new();
        if record.kind == crate::ObjectKind::File {
            if let Some(root) = record.extent_root_block {
                let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
                let map = extent_maps
                    .get(&object_id)
                    .ok_or(Error::Corrupt("file extent map is missing"))?;
                for extent in map.extents() {
                    for delta in 0..extent.block_count {
                        let block = extent
                            .physical_start
                            .checked_add(delta)
                            .ok_or(Error::ArithmeticOverflow)?;
                        reclaim_blocks.try_reserve(1).map_err(|_| {
                            Error::InvalidInput(
                                "allocation failed while staging reclaimed file blocks",
                            )
                        })?;
                        reclaim_blocks.push(block);
                    }
                }
                reclaim_blocks.try_reserve(1).map_err(|_| {
                    Error::InvalidInput("allocation failed while staging reclaimed extent roots")
                })?;
                reclaim_blocks.push(root);
            } else if record.size != 0 {
                return Err(Error::Corrupt(
                    "non-empty file is missing its extent-map root",
                ));
            }
        }

        namespace.remove(object_id)?;
        let publication = self.replace_persistent_namespace_inventory(device, &namespace);
        if let Err(error) = publication {
            // A reported I/O error can occur after the descriptor was committed. Only
            // reclaim blocks if rediscovery proves the removed object is no longer live.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active))) if active.object(object_id).is_none() => {
                    if self
                        .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
                        .is_err()
                    {
                        return Err(Error::Corrupt(
                            "file was removed but some of its blocks could not be reclaimed",
                        ));
                    }
                    // The descriptor now proves the requested removal committed despite
                    // the reported I/O error; report the durable outcome, not the late error.
                    return Ok(());
                }
                _ => return Err(error),
            }
        }

        if self
            .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .is_err()
        {
            return Err(Error::Corrupt(
                "file was removed but some of its blocks could not be reclaimed",
            ));
        }
        Ok(())
    }

    /// Remove a directory subtree addressed by its persistent path.
    ///
    /// The existing tree-removal operation performs the full ownership preflight and
    /// publishes the namespace change before reclaiming any data blocks.
    pub fn remove_persistent_tree_at_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
    ) -> Result<()> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent tree removal requires a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        self.remove_persistent_tree_from_namespace(device, namespace, object_id)
    }

    /// Remove a persistent directory and all descendants, reclaiming file storage.
    ///
    /// The complete subtree is removed in one copy-on-write namespace publication. Data
    /// blocks and extent-map roots are reclaimed only after rediscovery proves that the
    /// published inventory no longer references the removed objects.
    pub fn remove_persistent_tree<D: BlockDevice>(
        &mut self,
        device: &mut D,
        object_id: ObjectId,
    ) -> Result<()> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent tree removal requires a published inventory",
            ));
        };
        self.remove_persistent_tree_from_namespace(device, namespace, object_id)
    }

    /// Remove a subtree from an already reconstructed namespace. The full ownership
    /// preflight and publish-before-reclaim ordering remain centralized in this helper.
    fn remove_persistent_tree_from_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        namespace: Namespace,
        object_id: ObjectId,
    ) -> Result<()> {
        self.validate_device(device)?;
        if object_id == namespace.root_id() {
            return Err(Error::InvalidInput("cannot remove the namespace root"));
        }
        let target = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("object does not exist"))?;
        if target.kind != crate::ObjectKind::Directory {
            return Err(Error::InvalidInput(
                "recursive removal requires a directory",
            ));
        }

        // Resolve descendants from metadata, not from path strings, so unusual valid names
        // cannot make a child escape the subtree calculation.
        // Build parent-to-children adjacency once, then walk the subtree. Repeatedly
        // rescanning every metadata record for each depth makes deep trees quadratic.
        let mut children_by_parent: std::collections::BTreeMap<ObjectId, Vec<ObjectId>> =
            std::collections::BTreeMap::new();
        for record in namespace.objects() {
            if record.id != namespace.root_id() {
                let parent = ObjectId::new(record.parent_id)
                    .map_err(|_| Error::Corrupt("subtree object has an invalid parent"))?;
                let children = children_by_parent.entry(parent).or_default();
                children.try_reserve(1).map_err(|_| {
                    Error::InvalidInput("allocation failed while indexing namespace children")
                })?;
                children.push(record.id);
            }
        }
        let mut removed_ids = std::collections::BTreeSet::new();
        let mut pending = Vec::new();
        pending.try_reserve_exact(1).map_err(|_| {
            Error::InvalidInput("allocation failed while staging namespace subtree traversal")
        })?;
        pending.push(object_id);
        while let Some(current) = pending.pop() {
            if !removed_ids.insert(current) {
                continue;
            }
            if let Some(children) = children_by_parent.get(&current) {
                pending.try_reserve(children.len()).map_err(|_| {
                    Error::InvalidInput("allocation failed while expanding namespace subtree")
                })?;
                pending.extend(children.iter().copied());
            }
        }

        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let mut reclaim_blocks = Vec::new();
        for id in &removed_ids {
            let record = namespace
                .object(*id)
                .ok_or(Error::Corrupt("subtree metadata disappeared"))?;
            if record.kind != crate::ObjectKind::File {
                continue;
            }
            if let Some(root) = record.extent_root_block {
                let map = extent_maps
                    .get(id)
                    .ok_or(Error::Corrupt("file extent map is missing"))?;
                for extent in map.extents() {
                    for delta in 0..extent.block_count {
                        reclaim_blocks.push(
                            extent
                                .physical_start
                                .checked_add(delta)
                                .ok_or(Error::ArithmeticOverflow)?,
                        );
                    }
                }
                reclaim_blocks.push(root);
            } else if record.size != 0 {
                return Err(Error::Corrupt(
                    "non-empty file is missing its extent-map root",
                ));
            }
        }

        let mut records = Vec::new();
        let remaining_record_count = namespace
            .objects()
            .filter(|record| !removed_ids.contains(&record.id))
            .count();
        records
            .try_reserve_exact(remaining_record_count)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while staging remaining namespace objects")
            })?;
        records.extend(
            namespace
                .objects()
                .filter(|record| !removed_ids.contains(&record.id))
                .cloned(),
        );
        let mut entries = Vec::new();
        let remaining_entry_count = namespace
            .entries()
            .filter(|entry| !removed_ids.contains(&ObjectId(entry.object_id)))
            .count();
        entries
            .try_reserve_exact(remaining_entry_count)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while staging remaining namespace entries")
            })?;
        entries.extend(
            namespace
                .entries()
                .filter(|entry| !removed_ids.contains(&ObjectId(entry.object_id)))
                .cloned(),
        );
        let updated_namespace = Namespace::from_records(namespace.root_id(), records, entries)?;
        let publication = self.replace_persistent_namespace_inventory(device, &updated_namespace);
        if let Err(error) = publication {
            // Publication can report an I/O error after the new descriptor committed.
            // Reclaim only if the active inventory proves every removed ID is absent.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if removed_ids.iter().all(|id| active.object(*id).is_none()) =>
                {
                    if self
                        .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
                        .is_err()
                    {
                        return Err(Error::Corrupt(
                            "tree was removed but some file blocks could not be reclaimed",
                        ));
                    }
                    return Ok(());
                }
                _ => return Err(error),
            }
        }

        if self
            .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .is_err()
        {
            return Err(Error::Corrupt(
                "tree was removed but some file blocks could not be reclaimed",
            ));
        }
        Ok(())
    }

    /// Backwards-compatible name for removing a persistent object.
    ///
    /// Unlike the historical implementation, files with allocated extents are supported.
    pub fn remove_persistent_empty_object<D: BlockDevice>(
        &mut self,
        device: &mut D,
        object_id: ObjectId,
    ) -> Result<()> {
        self.remove_persistent_object(device, object_id)
    }

    /// Rename or move an object in the persistent namespace.
    ///
    /// The destination entry and the object's parent metadata are changed together in
    /// memory, then published as a copy-on-write inventory. The old inventory remains
    /// authoritative until descriptor publication commits, so a crash exposes either
    /// the old name/location or the new one, never a partially updated pair.
    pub fn rename_persistent_object<D: BlockDevice>(
        &mut self,
        device: &mut D,
        object_id: ObjectId,
        new_parent_id: ObjectId,
        new_name: &str,
    ) -> Result<()> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent rename requires a published inventory",
            ));
        };
        self.rename_persistent_object_from_namespace(
            device,
            namespace,
            object_id,
            new_parent_id,
            new_name,
        )
    }

    /// Rename or move an object using an already reconstructed namespace. The shared helper
    /// keeps the same cycle checks, no-op handling and uncertain-publication recovery while
    /// allowing path-oriented callers to avoid reconstructing the inventory twice.
    fn rename_persistent_object_from_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        mut namespace: Namespace,
        object_id: ObjectId,
        new_parent_id: ObjectId,
        new_name: &str,
    ) -> Result<()> {
        self.validate_device(device)?;
        let record = namespace
            .object(object_id)
            .cloned()
            .ok_or(Error::InvalidInput("object does not exist"))?;
        // Avoid copy-on-write inventory publication when the requested name and parent
        // already identify this object. The in-memory rename path is also a no-op, but
        // without this guard it would still allocate and flush replacement metadata.
        if record.parent_id == new_parent_id.0
            && namespace
                .lookup(new_parent_id, new_name)
                .is_some_and(|entry| entry.object_id == object_id.0)
        {
            return Ok(());
        }
        let old_parent_id = ObjectId::new(record.parent_id)
            .map_err(|_| Error::Corrupt("renamed object has an invalid parent"))?;
        let old_name = namespace
            .entries()
            .find(|entry| entry.object_id == object_id.0)
            .map(|entry| entry.name.clone())
            .ok_or(Error::Corrupt("renamed object has no directory entry"))?;
        namespace.rename(object_id, new_parent_id, new_name)?;
        if let Err(error) = self.replace_persistent_namespace_inventory(device, &namespace) {
            // The descriptor may have committed even if a later journal/device operation
            // returned an error. Report success only if rediscovery proves the new edge is live.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.parent_id == new_parent_id.0
                            && active
                                .lookup(new_parent_id, new_name)
                                .is_some_and(|entry| entry.object_id == object_id.0)
                    }) =>
                {
                    return Ok(());
                }
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.parent_id == old_parent_id.0
                            && active
                                .lookup(old_parent_id, &old_name)
                                .is_some_and(|entry| entry.object_id == object_id.0)
                    }) =>
                {
                    return Err(error);
                }
                _ => {
                    return Err(Error::Corrupt(
                        "rename publication outcome is uncertain; preserve the image and recover before further writes",
                    ));
                }
            }
        }
        Ok(())
    }

    /// Persist a complete in-memory namespace by copy-on-write inventory replacement.
    ///
    /// Fresh metadata and directory streams are allocated and flushed before the descriptor
    /// transaction publishes them. The previous inventory remains authoritative until that
    /// transaction commits. If publication returns an error with an uncertain outcome, the
    /// descriptor is rediscovered before deciding whether staged blocks can be released.
    ///
    /// This persists namespace metadata only; it does not allocate file extents or persist
    /// file contents. Callers must supply a validated namespace whose file extents already
    /// have valid on-disk ownership.
    pub fn replace_persistent_namespace_inventory<D: BlockDevice>(
        &mut self,
        device: &mut D,
        namespace: &Namespace,
    ) -> Result<u64> {
        self.validate_device(device)?;
        namespace.validate()?;

        // The on-disk descriptor has a fixed number of block references. Reject an
        // oversized namespace from its fixed-width record counts before serializing the
        // complete inventory or reading every file's extent map. Besides avoiding large
        // temporary allocations, this makes capacity failure cheap and side-effect free.
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let descriptor_capacity = block_size
            .checked_sub(48)
            .ok_or(Error::ArithmeticOverflow)?
            / 8;
        let metadata_bytes_needed = namespace
            .object_count()
            .checked_mul(METADATA_RECORD_SIZE)
            .ok_or(Error::ArithmeticOverflow)?;
        let directory_bytes_needed = namespace
            .entry_count()
            .checked_mul(DIRECTORY_ENTRY_SIZE)
            .ok_or(Error::ArithmeticOverflow)?;
        let blocks_needed = |byte_count: usize| -> Result<usize> {
            Ok(byte_count
                .checked_add(block_size - 1)
                .ok_or(Error::ArithmeticOverflow)?
                / block_size)
        };
        let metadata_blocks_needed = blocks_needed(metadata_bytes_needed)?;
        let directory_blocks_needed = blocks_needed(directory_bytes_needed)?;
        if metadata_blocks_needed
            .checked_add(directory_blocks_needed)
            .ok_or(Error::ArithmeticOverflow)?
            > descriptor_capacity
        {
            return Err(Error::InvalidInput(
                "namespace inventory exceeds descriptor block capacity",
            ));
        }

        // Check that replacement has an authoritative starting point before scanning
        // every file's extent map. On an uninitialized volume, this avoids expensive
        // ownership I/O and all serialization/staging work.
        if self.discover_persistent_inventory(device)?.is_none() {
            return Err(Error::InvalidInput(
                "no persistent inventory descriptor exists to replace",
            ));
        }

        // Do not publish metadata that points at missing, free, overlapping, or
        // otherwise malformed extent maps. Validate the complete incoming ownership
        // graph while the previous inventory is still authoritative.
        self.read_namespace_extent_maps(device, namespace)?;

        // Counts and fixed record widths were checked above, so reserve the exact
        // serialized sizes once. This avoids geometric Vec growth and transient copies
        // when replacing a large namespace near the descriptor's capacity limit.
        let mut metadata_bytes = Vec::new();
        metadata_bytes
            .try_reserve_exact(metadata_bytes_needed)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while serializing namespace metadata")
            })?;
        for record in namespace.objects() {
            metadata_bytes.extend_from_slice(&record.encode()?);
        }
        let mut directory_bytes = Vec::new();
        directory_bytes
            .try_reserve_exact(directory_bytes_needed)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while serializing namespace entries")
            })?;
        for entry in namespace.entries() {
            directory_bytes.extend_from_slice(&entry.encode()?);
        }

        // The fixed-width preflight above guarantees the serialized streams fit the
        // descriptor's reference budget; keep the actual lengths as a defensive format
        // consistency check in case record encodings ever change.
        if blocks_needed(metadata_bytes.len())?
            .checked_add(blocks_needed(directory_bytes.len())?)
            .ok_or(Error::ArithmeticOverflow)?
            > descriptor_capacity
        {
            return Err(Error::InvalidInput(
                "namespace inventory exceeds descriptor block capacity",
            ));
        }

        // Reserve rollback tracking before allocating any on-disk staging blocks. The
        // descriptor capacity is the upper bound for both streams combined, so cleanup
        // never needs a fresh allocation after metadata has already been written.
        let mut staged_blocks = Vec::new();
        staged_blocks
            .try_reserve_exact(descriptor_capacity)
            .map_err(|_| {
                Error::InvalidInput("allocation failed while tracking staged inventory blocks")
            })?;

        let metadata_blocks = self.stage_packed_inventory(device, &metadata_bytes)?;
        staged_blocks.extend_from_slice(&metadata_blocks);
        let directory_blocks = match self.stage_packed_inventory(device, &directory_bytes) {
            Ok(blocks) => {
                staged_blocks.extend_from_slice(&blocks);
                blocks
            }
            Err(error) => {
                if self
                    .reclaim_persistent_blocks(device, &staged_blocks)
                    .is_err()
                {
                    return Err(Error::Corrupt(
                        "failed to reclaim staged inventory after directory staging failed",
                    ));
                }
                return Err(error);
            }
        };

        // Descriptor-owned copies also allocate. Make those allocations fallible and
        // reclaim the already-staged streams if either copy cannot be prepared.
        let descriptor_metadata_blocks = match try_copy_block_numbers(
            &metadata_blocks,
            "allocation failed while preparing inventory descriptor metadata",
        ) {
            Ok(blocks) => blocks,
            Err(error) => {
                if self
                    .reclaim_persistent_blocks(device, &staged_blocks)
                    .is_err()
                {
                    return Err(Error::Corrupt(
                        "failed to reclaim staged inventory after descriptor allocation failed",
                    ));
                }
                return Err(error);
            }
        };
        let descriptor_directory_blocks = match try_copy_block_numbers(
            &directory_blocks,
            "allocation failed while preparing inventory descriptor directory",
        ) {
            Ok(blocks) => blocks,
            Err(error) => {
                if self
                    .reclaim_persistent_blocks(device, &staged_blocks)
                    .is_err()
                {
                    return Err(Error::Corrupt(
                        "failed to reclaim staged inventory after descriptor allocation failed",
                    ));
                }
                return Err(error);
            }
        };
        let descriptor = PersistentInventoryDescriptor {
            root_id: namespace.root_id(),
            metadata_blocks: descriptor_metadata_blocks,
            metadata_record_count: namespace.object_count(),
            directory_blocks: descriptor_directory_blocks,
            directory_entry_count: namespace.entry_count(),
        };
        if let Err(error) = descriptor.encode(block_size) {
            if self
                .reclaim_persistent_blocks(device, &staged_blocks)
                .is_err()
            {
                return Err(Error::Corrupt(
                    "failed to reclaim inventory blocks after descriptor validation failed",
                ));
            }
            return Err(error);
        }

        match self.replace_persistent_inventory_descriptor(device, &descriptor) {
            Ok(block) => Ok(block),
            Err(error) => {
                // The journal can report an I/O error after the commit point. Rediscover
                // exactly once: reclaim staged streams only when the descriptor proves they
                // are not active, and report success if it proves this publication committed.
                match self.discover_persistent_inventory(device) {
                    Ok(Some((block, active))) if active == descriptor => Ok(block),
                    Ok(Some((_, _))) | Ok(None) => {
                        if self
                            .reclaim_persistent_blocks(device, &staged_blocks)
                            .is_err()
                        {
                            return Err(Error::Corrupt(
                                "failed to reclaim staged inventory after publication failed",
                            ));
                        }
                        Err(error)
                    }
                    Err(_) => Err(error),
                }
            }
        }
    }

    fn stage_packed_inventory<D: BlockDevice>(
        &mut self,
        device: &mut D,
        bytes: &[u8],
    ) -> Result<Vec<u64>> {
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let block_count = bytes
            .len()
            .checked_add(block_size - 1)
            .ok_or(Error::ArithmeticOverflow)?
            / block_size;
        let mut blocks = Vec::new();
        blocks
            .try_reserve_exact(block_count)
            .map_err(|_| Error::InvalidInput("allocation failed while staging inventory blocks"))?;
        // Inventory streams may span many blocks. Reuse one zero-padded block buffer
        // instead of allocating a fresh Vec for every chunk; clear it before each copy
        // so a short final chunk cannot expose bytes left by the preceding chunk.
        let mut padded = try_zeroed_block(
            block_size,
            "allocation failed while staging inventory block contents",
        )?;
        let result = (|| {
            for chunk in bytes.chunks(block_size) {
                padded.fill(0);
                padded[..chunk.len()].copy_from_slice(chunk);
                let block = self.allocate_block(device)?;
                blocks.push(block);
                device.write_blocks(block, &padded)?;
            }
            device.flush()?;
            Ok(())
        })();
        if let Err(error) = result {
            if self.reclaim_persistent_blocks(device, &blocks).is_err() {
                return Err(Error::Corrupt(
                    "failed to reclaim partially staged inventory blocks",
                ));
            }
            return Err(error);
        }
        Ok(blocks)
    }

    /// Read a caller-located, packed metadata inventory from allocated data blocks.
    ///
    /// The blocks are supplied in stream order and must contain exactly enough blocks
    /// for the requested fixed-width records. The final block may have unused bytes;
    /// only the requested record bytes are decoded. The disk format does not yet assign
    /// a canonical metadata-inventory region.
    pub fn read_metadata_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
        blocks: &[u64],
        record_count: usize,
    ) -> Result<Vec<MetadataRecord>> {
        let bytes =
            self.read_packed_inventory(device, blocks, record_count, METADATA_RECORD_SIZE)?;
        decode_metadata_records(&bytes)
    }

    /// Read a caller-located packed directory-entry inventory from allocated data blocks.
    ///
    /// The block list is in stream order and must be exactly sized for the requested
    /// number of entries. This validates block ownership state and record framing, but
    /// does not imply that a persistent directory index or region allocator exists.
    pub fn read_directory_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
        blocks: &[u64],
        entry_count: usize,
    ) -> Result<Vec<DirectoryEntry>> {
        let bytes =
            self.read_packed_inventory(device, blocks, entry_count, DIRECTORY_ENTRY_SIZE)?;
        decode_directory_entries(&bytes)
    }

    /// Reconstruct a validated namespace from caller-located on-disk record streams.
    ///
    /// Metadata and directory streams must occupy disjoint allocated data blocks. This
    /// is the first disk-backed reconstruction path, but the caller still supplies the
    /// inventory block locations; no automatic metadata discovery is performed.
    pub fn reconstruct_namespace<D: BlockDevice>(
        &self,
        device: &mut D,
        root_id: ObjectId,
        metadata_blocks: &[u64],
        metadata_count: usize,
        directory_blocks: &[u64],
        directory_entry_count: usize,
    ) -> Result<Namespace> {
        let mut claimed = std::collections::BTreeSet::new();
        for block in metadata_blocks.iter().chain(directory_blocks.iter()) {
            if !claimed.insert(*block) {
                return Err(Error::Corrupt(
                    "metadata and directory inventories share a physical block",
                ));
            }
        }
        let records = self.read_metadata_inventory(device, metadata_blocks, metadata_count)?;
        let entries =
            self.read_directory_inventory(device, directory_blocks, directory_entry_count)?;
        let namespace = Namespace::from_records(root_id, records, entries)?;
        let roots = namespace.validated_extent_root_inventory(
            self.volume.layout.data_start,
            self.volume.layout.backup_superblock,
        )?;
        for root in &roots {
            if claimed.contains(root) {
                return Err(Error::Corrupt(
                    "extent-map root aliases a metadata or directory inventory block",
                ));
            }
        }
        self.validate_namespace_extent_roots(device, &namespace)?;
        Ok(namespace)
    }

    /// Validate every extent-map root referenced by a namespace against the allocation bitmap.
    ///
    /// Returns the sorted, unique roots after checking data-region bounds and requiring
    /// each root block to be allocated. This does not decode the extent-map pages.
    pub fn validate_namespace_extent_roots<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
    ) -> Result<Vec<u64>> {
        self.validate_device(device)?;
        let roots = namespace.validated_extent_root_inventory(
            self.volume.layout.data_start,
            self.volume.layout.backup_superblock,
        )?;
        // Roots are sorted and unique. Read each bitmap page once, then validate
        // consecutive roots as ranges instead of doing a division/modulo and bit
        // lookup for every file whose extent map shares that page.
        let bits_per_bitmap_block = u64::from(self.volume.block_size) * 8;
        let mut bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        let mut index = 0;
        while index < roots.len() {
            let bitmap_index = roots[index] / bits_per_bitmap_block;
            let bitmap_base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let bitmap_block = self
                .volume
                .layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            crate::allocator_extents::validate_bitmap_reserved_bits(
                &bitmap,
                bitmap_base,
                self.volume.layout.data_start.max(bitmap_base),
                self.volume.layout.backup_superblock.min(
                    bitmap_base
                        .checked_add(bits_per_bitmap_block)
                        .ok_or(Error::ArithmeticOverflow)?,
                ),
            )?;

            let mut segment_end = index + 1;
            while segment_end < roots.len()
                && roots[segment_end] / bits_per_bitmap_block == bitmap_index
            {
                segment_end += 1;
            }
            let segment_roots = &roots[index..segment_end];
            let mut run_start = segment_roots[0];
            let mut previous = segment_roots[0];
            for &root in segment_roots.iter().skip(1) {
                if previous.checked_add(1) == Some(root) {
                    previous = root;
                    continue;
                }
                if !crate::allocator_extents::bitmap_range_matches(
                    &bitmap,
                    run_start - bitmap_base,
                    previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - bitmap_base,
                    true,
                )? {
                    return Err(Error::Corrupt(
                        "extent-map root is marked free in the allocation bitmap",
                    ));
                }
                run_start = root;
                previous = root;
            }
            if !crate::allocator_extents::bitmap_range_matches(
                &bitmap,
                run_start - bitmap_base,
                previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - bitmap_base,
                true,
            )? {
                return Err(Error::Corrupt(
                    "extent-map root is marked free in the allocation bitmap",
                ));
            }
            index = segment_end;
        }
        Ok(roots)
    }

    /// Read and validate one on-disk extent-map page for a file.
    ///
    /// The root and every referenced data block must be allocated and lie in the
    /// allocatable data region. The extent map must fit the file's logical size, and
    /// its own root may not also be claimed as file data.
    pub fn read_extent_map<D: BlockDevice>(
        &self,
        device: &mut D,
        root_block: u64,
        file_size: u64,
    ) -> Result<ExtentMap> {
        self.validate_device(device)?;
        if root_block < self.volume.layout.data_start
            || root_block >= self.volume.layout.backup_superblock
        {
            return Err(Error::Corrupt("extent-map root is outside the data region"));
        }
        if !self.is_data_block_allocated(device, root_block)? {
            return Err(Error::Corrupt(
                "extent-map root is marked free in the allocation bitmap",
            ));
        }
        self.read_extent_map_after_root_validation(device, root_block, file_size, true)
    }

    /// Decode an extent map after the caller has validated the root's range and
    /// allocation state. Namespace loading defers data-allocation checks until all maps
    /// are decoded, so bitmap pages can be scanned once for the complete volume inventory.
    fn read_extent_map_after_root_validation<D: BlockDevice>(
        &self,
        device: &mut D,
        root_block: u64,
        file_size: u64,
        validate_allocations: bool,
    ) -> Result<ExtentMap> {
        let mut bytes = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        device.read_blocks(root_block, &mut bytes)?;
        let map = ExtentMap::decode_block(&bytes)?;
        let logical_blocks = file_size / u64::from(self.volume.block_size)
            + u64::from(file_size % u64::from(self.volume.block_size) != 0);
        map.validate_file_size(file_size, self.volume.block_size)?;
        map.validate(
            logical_blocks,
            self.volume.layout.data_start,
            self.volume.layout.backup_superblock,
        )?;
        if map
            .validated_physical_ranges(
                logical_blocks,
                self.volume.layout.data_start,
                self.volume.layout.backup_superblock,
            )?
            .iter()
            .any(|(start, end)| *start <= root_block && root_block < *end)
        {
            return Err(Error::Corrupt("extent-map root aliases its own file data"));
        }
        if validate_allocations {
            self.validate_extent_allocations(device, &map, logical_blocks)?;
        }
        Ok(map)
    }

    /// Read a byte range from a file using a reconstructed namespace and loaded extent maps.
    ///
    /// Reads are bounded by the recorded file size: offsets at or past EOF return an empty
    /// vector, and requests extending past EOF are shortened. Sparse holes are zero-filled.
    /// Every physical block touched is rechecked against the live allocation bitmap.
    pub fn read_file_range<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
        extent_maps: &std::collections::BTreeMap<ObjectId, ExtentMap>,
        object_id: ObjectId,
        offset: u64,
        length: usize,
    ) -> Result<Vec<u8>> {
        self.validate_device(device)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file object does not exist"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("object is not a regular file"));
        }
        // Validate namespace identity before accepting EOF or zero-length reads.
        let Some((_, persistent_namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "file reads require a published persistent inventory",
            ));
        };
        if namespace.root_id() != persistent_namespace.root_id()
            || namespace.objects().ne(persistent_namespace.objects())
            || namespace
                .entries()
                .map(|entry| {
                    (
                        entry.parent_id,
                        entry.object_id,
                        entry.kind,
                        entry.name.as_str(),
                    )
                })
                .ne(persistent_namespace.entries().map(|entry| {
                    (
                        entry.parent_id,
                        entry.object_id,
                        entry.kind,
                        entry.name.as_str(),
                    )
                }))
        {
            return Err(Error::Corrupt(
                "supplied namespace metadata does not match persistent inventory",
            ));
        }
        self.read_file_range_from_validated_namespace(
            device,
            namespace,
            extent_maps,
            object_id,
            offset,
            length,
            true,
        )
    }

    /// Read using a namespace and extent inventory reconstructed and validated by the caller.
    /// This avoids repeating the full inventory/ownership scan on path-based reads.
    #[allow(clippy::too_many_arguments)]
    fn read_file_range_from_validated_namespace<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
        extent_maps: &std::collections::BTreeMap<ObjectId, ExtentMap>,
        object_id: ObjectId,
        offset: u64,
        length: usize,
        validate_extent_graph: bool,
    ) -> Result<Vec<u8>> {
        self.validate_device(device)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file object does not exist"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("object is not a regular file"));
        }
        if length == 0 || offset >= record.size {
            return Ok(Vec::new());
        }
        let requested = u64::try_from(length).map_err(|_| Error::ArithmeticOverflow)?;
        let read_len = usize::try_from((record.size - offset).min(requested))
            .map_err(|_| Error::ArithmeticOverflow)?;
        let end = offset
            .checked_add(read_len as u64)
            .ok_or(Error::ArithmeticOverflow)?;
        let block_size = u64::from(self.volume.block_size);
        let first_logical = offset / block_size;
        let last_logical = (end - 1) / block_size;
        let map = extent_maps.get(&object_id);
        if record.extent_root_block.is_some() {
            let loaded_map = map.ok_or(Error::Corrupt(
                "file extent map is missing from loaded inventory",
            ))?;
            // Public callers may supply an arbitrary map, so the standalone API must
            // compare it with disk. Path-based callers pass maps just loaded and checked
            // by read_namespace_extent_maps; rereading the root here would duplicate I/O.
            if validate_extent_graph {
                let root_block = record.extent_root_block.ok_or(Error::Corrupt(
                    "extent-map root disappeared during validation",
                ))?;
                let persisted_map = self.read_extent_map(device, root_block, record.size)?;
                if loaded_map != &persisted_map {
                    return Err(Error::Corrupt(
                        "loaded extent map does not match persistent extent-map root",
                    ));
                }
            }
        }
        if record.extent_root_block.is_none() && map.is_some_and(|value| !value.is_empty()) {
            return Err(Error::Corrupt(
                "file has extents without a persistent extent-map root",
            ));
        }

        if validate_extent_graph {
            // Validate the complete namespace ownership graph before exposing file data.
            // Checking only this file's map would miss a different file claiming overlapping
            // physical blocks, allowing reads from an ambiguous/corrupt allocation graph.
            let persisted_maps = self.read_namespace_extent_maps(device, namespace)?;
            if &persisted_maps != extent_maps {
                return Err(Error::Corrupt(
                    "loaded extent maps do not match persistent namespace inventory",
                ));
            }
        }

        let mut output = try_zeroed_block(read_len, "unable to allocate file range read buffer")?;
        // A request contained in one extent is physically contiguous by construction.
        // Inspect extent descriptors rather than probing every logical block, keeping
        // large sequential reads O(number of extents) instead of O(number of blocks).
        if offset % block_size == 0 && read_len as u64 % block_size == 0 {
            let requested_blocks =
                u64::try_from(read_len).map_err(|_| Error::ArithmeticOverflow)? / block_size;
            let requested_end = first_logical
                .checked_add(requested_blocks)
                .ok_or(Error::ArithmeticOverflow)?;
            if let Some(extent) = map.and_then(|extent_map| {
                extent_map.extents().iter().find(|extent| {
                    extent.logical_start <= first_logical
                        && extent
                            .logical_start
                            .checked_add(extent.block_count)
                            .is_some_and(|extent_end| extent_end >= requested_end)
                })
            }) {
                let physical_start = extent
                    .physical_start
                    .checked_add(first_logical - extent.logical_start)
                    .ok_or(Error::ArithmeticOverflow)?;
                device.read_blocks(physical_start, &mut output)?;
                return Ok(output);
            }
        }
        // Bound scratch memory while coalescing physically adjacent blocks into
        // larger device reads. This reduces I/O calls for fragmented files without
        // allocating a second buffer proportional to the full requested file range.
        // Cap scratch memory at 1 MiB while adapting block count to the device's
        // filesystem block size. Small blocks get larger batches; large blocks do not
        // multiply a fixed block count into several MiB of transient memory.
        const MAX_READ_BATCH_BYTES: usize = 1024 * 1024;
        const MAX_READ_BATCH_BLOCKS: usize = 128;
        let requested_blocks = last_logical
            .checked_sub(first_logical)
            .and_then(|count| count.checked_add(1))
            .ok_or(Error::ArithmeticOverflow)?;
        let block_len = usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let max_batch_blocks = (MAX_READ_BATCH_BYTES / block_len).clamp(1, MAX_READ_BATCH_BLOCKS);
        let batch_blocks = requested_blocks
            .min(u64::try_from(max_batch_blocks).map_err(|_| Error::ArithmeticOverflow)?);
        let batch_bytes = usize::try_from(
            batch_blocks
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?,
        )
        .map_err(|_| Error::ArithmeticOverflow)?;
        let mut block_buffer = try_zeroed_block(
            batch_bytes,
            "unable to allocate file range read scratch buffer",
        )?;
        let mut logical = first_logical;
        while logical <= last_logical {
            let Some(physical_start) = map.and_then(|value| value.physical_block(logical)) else {
                // Unmapped logical blocks remain zero-filled (sparse-file semantics).
                logical = logical.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
                continue;
            };

            let mut run_blocks = 1_u64;
            while run_blocks < batch_blocks {
                let next_logical = logical
                    .checked_add(run_blocks)
                    .ok_or(Error::ArithmeticOverflow)?;
                if next_logical > last_logical {
                    break;
                }
                let expected_physical = physical_start
                    .checked_add(run_blocks)
                    .ok_or(Error::ArithmeticOverflow)?;
                if map.and_then(|value| value.physical_block(next_logical))
                    != Some(expected_physical)
                {
                    break;
                }
                run_blocks += 1;
            }

            let run_start = logical
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let run_end = logical
                .checked_add(run_blocks)
                .and_then(|block| block.checked_mul(block_size))
                .ok_or(Error::ArithmeticOverflow)?;
            let copy_start = offset.max(run_start);
            let copy_end = end.min(run_end);
            let run_bytes = usize::try_from(
                run_blocks
                    .checked_mul(block_size)
                    .ok_or(Error::ArithmeticOverflow)?,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;
            // The scratch buffer is sized for at least one block and reused for
            // every contiguous run, including unaligned first and last blocks.
            device.read_blocks(physical_start, &mut block_buffer[..run_bytes])?;
            let source_start =
                usize::try_from(copy_start - run_start).map_err(|_| Error::ArithmeticOverflow)?;
            let source_end =
                usize::try_from(copy_end - run_start).map_err(|_| Error::ArithmeticOverflow)?;
            let destination_start =
                usize::try_from(copy_start - offset).map_err(|_| Error::ArithmeticOverflow)?;
            let destination_end =
                usize::try_from(copy_end - offset).map_err(|_| Error::ArithmeticOverflow)?;
            output[destination_start..destination_end]
                .copy_from_slice(&block_buffer[source_start..source_end]);

            logical = logical
                .checked_add(run_blocks)
                .ok_or(Error::ArithmeticOverflow)?;
        }
        Ok(output)
    }
    /// Write bytes into an existing file's already-allocated extents.
    ///
    /// This operation does not extend files, allocate blocks, or update metadata. It
    /// rejects ranges that cross EOF or touch sparse holes before writing any bytes.
    /// The complete set of touched data blocks is committed through the redo journal.
    /// Ranges that exceed journal capacity are rejected before any data write. If I/O
    /// fails after the commit record is durable, drop this volume and reopen it so journal
    /// recovery can finish applying the transaction before further mutations.
    pub fn write_file_range<D: BlockDevice>(
        &mut self,
        device: &mut D,
        namespace: &Namespace,
        extent_maps: &std::collections::BTreeMap<ObjectId, ExtentMap>,
        object_id: ObjectId,
        offset: u64,
        data: &[u8],
    ) -> Result<()> {
        self.validate_device(device)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file object does not exist"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("object is not a regular file"));
        }
        // Even a no-op write must not accept stale or caller-forged namespace
        // metadata. Validate the persistent identity before the empty-data fast path.
        let Some((_, persistent_namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "file writes require a published persistent inventory",
            ));
        };
        if namespace.root_id() != persistent_namespace.root_id()
            || namespace.objects().ne(persistent_namespace.objects())
            || namespace
                .entries()
                .map(|entry| {
                    (
                        entry.parent_id,
                        entry.object_id,
                        entry.kind,
                        entry.name.as_str(),
                    )
                })
                .ne(persistent_namespace.entries().map(|entry| {
                    (
                        entry.parent_id,
                        entry.object_id,
                        entry.kind,
                        entry.name.as_str(),
                    )
                }))
        {
            return Err(Error::Corrupt(
                "supplied namespace metadata does not match persistent inventory",
            ));
        }
        if data.is_empty() {
            return Ok(());
        }
        let write_len = u64::try_from(data.len()).map_err(|_| Error::ArithmeticOverflow)?;
        let end = offset
            .checked_add(write_len)
            .ok_or(Error::ArithmeticOverflow)?;
        if offset >= record.size || end > record.size {
            return Err(Error::InvalidInput(
                "write range exceeds existing file size",
            ));
        }
        // Validate the complete persistent ownership graph, not just the target
        // file's map. A corrupt map that aliases another file's data must never be
        // allowed to redirect writes into another object's blocks.
        let persisted_maps = self.read_namespace_extent_maps(device, namespace)?;
        if &persisted_maps != extent_maps {
            return Err(Error::Corrupt(
                "loaded extent maps do not match persistent namespace inventory",
            ));
        }

        let map = persisted_maps.get(&object_id);

        let block_size = u64::from(self.volume.block_size);
        let first_logical = offset / block_size;
        let last_logical = (end - 1) / block_size;
        let map = map.ok_or(Error::InvalidInput(
            "cannot write into a sparse hole without allocating extents",
        ))?;

        // The persisted map was loaded by read_namespace_extent_maps above, which
        // validates every physical range, allocation bit, root alias and cross-file
        // overlap. Only the write-specific condition remains to preflight here: every
        // touched logical block must exist, so a sparse hole cannot cause a partial write.
        for logical in first_logical..=last_logical {
            map.physical_block(logical).ok_or(Error::InvalidInput(
                "cannot write into a sparse hole without allocating extents",
            ))?;
        }

        let block_len = self.volume.block_size as usize;
        let touched_blocks = usize::try_from(last_logical - first_logical + 1)
            .map_err(|_| Error::ArithmeticOverflow)?;
        // Capacity is checked before staging or modifying any data block. A bounded
        // journal must never accept a prefix of a logical write just because the tail
        // of the range cannot fit in the ring.
        self.journal.required_record_count(device, touched_blocks)?;

        let mut updates = Vec::new();
        updates
            .try_reserve_exact(touched_blocks)
            .map_err(|_| Error::InvalidInput("unable to reserve file write redo updates"))?;
        let mut block_buffer =
            try_zeroed_block(block_len, "unable to allocate file write block buffer")?;
        let mut source_offset = 0_usize;
        for logical in first_logical..=last_logical {
            let block_start = logical
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let copy_start = offset.max(block_start);
            let copy_end = end.min(
                block_start
                    .checked_add(block_size)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let within_start =
                usize::try_from(copy_start - block_start).map_err(|_| Error::ArithmeticOverflow)?;
            let within_end =
                usize::try_from(copy_end - block_start).map_err(|_| Error::ArithmeticOverflow)?;
            let chunk_len = within_end - within_start;
            let physical = map.physical_block(logical).ok_or(Error::Corrupt(
                "target extent disappeared after write-range preflight",
            ))?;
            let source_end = source_offset
                .checked_add(chunk_len)
                .ok_or(Error::ArithmeticOverflow)?;
            let source_chunk = data.get(source_offset..source_end).ok_or(Error::Corrupt(
                "write-range source accounting exceeds the supplied data",
            ))?;
            if within_start == 0 && within_end == block_len {
                block_buffer.copy_from_slice(source_chunk);
            } else {
                device.read_blocks(physical, &mut block_buffer)?;
                block_buffer[within_start..within_end].copy_from_slice(source_chunk);
            }
            updates.push(RedoUpdate {
                target_block: physical,
                contents: try_copy_bytes(
                    &block_buffer,
                    "unable to allocate file write redo image",
                )?,
            });
            source_offset = source_end;
        }
        if source_offset != data.len() {
            return Err(Error::Corrupt(
                "write-range source accounting does not match the supplied data",
            ));
        }
        self.journal.commit(device, &updates)?;
        Ok(())
    }

    /// Read the complete logical contents of a file, including zero-filled sparse holes.
    ///
    /// This allocates a buffer of the file's full byte length; prefer read_file_range for
    /// large files or when reading untrusted file sizes.
    pub fn read_file_contents<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
        extent_maps: &std::collections::BTreeMap<ObjectId, ExtentMap>,
        object_id: ObjectId,
    ) -> Result<Vec<u8>> {
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file object does not exist"))?;
        let length = usize::try_from(record.size).map_err(|_| Error::ArithmeticOverflow)?;
        self.read_file_range(device, namespace, extent_maps, object_id, 0, length)
    }

    /// Resolve a namespace path and read a bounded byte range from the referenced file.
    pub fn read_path_range<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
        extent_maps: &std::collections::BTreeMap<ObjectId, ExtentMap>,
        path: &str,
        offset: u64,
        length: usize,
    ) -> Result<Vec<u8>> {
        let object_id = namespace.resolve_path(path)?;
        self.read_file_range(device, namespace, extent_maps, object_id, offset, length)
    }

    /// Read a file range by path using only the published on-disk inventory.
    ///
    /// Unlike `read_path_range`, this entry point reconstructs the namespace and extent
    /// maps itself, so callers do not need to cache or supply potentially stale metadata.
    /// It is read-only and requires a valid persistent inventory descriptor.
    pub fn read_persistent_path_range<D: BlockDevice>(
        &self,
        device: &mut D,
        path: &str,
        offset: u64,
        length: usize,
    ) -> Result<Vec<u8>> {
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent path reads require a published inventory",
            ));
        };
        // Resolve and validate the requested object before loading every extent map.
        // Invalid paths and directory reads should fail without volume-wide extent I/O.
        let object_id = namespace.resolve_path(path)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::Corrupt("resolved file metadata is missing"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path does not identify a regular file"));
        }
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        self.read_file_range_from_validated_namespace(
            device,
            &namespace,
            &extent_maps,
            object_id,
            offset,
            length,
            false,
        )
    }

    /// Stream a bounded range of a persistent file without allocating the requested length.
    ///
    /// Namespace and extent ownership are validated once before output begins. Memory use
    /// remains bounded by one filesystem block even when the requested range is very large.
    pub fn stream_persistent_path_range_to_writer<D: BlockDevice, W: std::io::Write>(
        &self,
        device: &mut D,
        path: &str,
        offset: u64,
        length: u64,
        writer: &mut W,
    ) -> Result<u64> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent file reads require a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file metadata is missing"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path is not a regular file"));
        }
        // Validate the persisted ownership graph even for an empty or out-of-EOF
        // range. A no-byte read must not become a way to bypass corruption detection.
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let map = extent_maps.get(&object_id);
        if record.extent_root_block.is_some() && map.is_none() {
            return Err(Error::Corrupt(
                "file extent map is missing from loaded inventory",
            ));
        }
        if record.extent_root_block.is_none() && map.is_some_and(|value| !value.is_empty()) {
            return Err(Error::Corrupt(
                "file has extents without a persistent extent-map root",
            ));
        }

        // read_namespace_extent_maps already validates every extent map against the
        // allocation bitmap and global ownership graph; do not scan the same bits again.
        let block_size = u64::from(self.volume.block_size);
        if length == 0 || offset >= record.size {
            writer.flush()?;
            return Ok(0);
        }

        let remaining_len = (record.size - offset).min(length);
        let end = offset
            .checked_add(remaining_len)
            .ok_or(Error::ArithmeticOverflow)?;
        let first_logical = offset / block_size;
        let last_logical = (end - 1) / block_size;
        let block_len = usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
        // One block-sized buffer is enough for both allocated and sparse ranges.
        // Reusing it avoids a second allocation proportional to the filesystem block size.
        // Batch contiguous full-block reads while keeping memory bounded.
        // Partial first/last blocks are emitted separately to preserve range boundaries.
        // Keep each streaming scratch buffer near 1 MiB regardless of the
        // filesystem block size, avoiding tiny device requests on 4 KiB volumes.
        const STREAM_BATCH_BYTES: usize = 1024 * 1024;
        const MAX_STREAM_BATCH_BLOCKS: usize = 64;
        let stream_batch_blocks =
            (STREAM_BATCH_BYTES / block_len).clamp(1, MAX_STREAM_BATCH_BLOCKS);
        let batch_capacity = block_len
            .checked_mul(stream_batch_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut block_buffer =
            try_zeroed_block(block_len, "unable to allocate persistent path read buffer")?;
        let mut batch_buffer = try_zeroed_block(
            batch_capacity,
            "unable to allocate persistent path read batch",
        )?;
        let mut written = 0_u64;
        let mut logical = first_logical;
        while logical <= last_logical {
            let block_start = logical
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let block_end = block_start
                .checked_add(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let copy_start = offset.max(block_start);
            let copy_end = end.min(block_end);
            let within_start =
                usize::try_from(copy_start - block_start).map_err(|_| Error::ArithmeticOverflow)?;
            let within_end =
                usize::try_from(copy_end - block_start).map_err(|_| Error::ArithmeticOverflow)?;
            if within_start == 0 && within_end == block_len {
                let first_physical = map.and_then(|value| value.physical_block(logical));
                let max_blocks =
                    usize::try_from((last_logical - logical + 1).min(stream_batch_blocks as u64))
                        .map_err(|_| Error::ArithmeticOverflow)?;
                let mut run_blocks = 1_usize;
                while run_blocks < max_blocks {
                    let delta = u64::try_from(run_blocks).map_err(|_| Error::ArithmeticOverflow)?;
                    let next_logical = logical
                        .checked_add(delta)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let next_start = next_logical
                        .checked_mul(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let next_end = next_start
                        .checked_add(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    if next_start < offset || next_end > end {
                        break;
                    }
                    match first_physical {
                        Some(physical) => {
                            let expected = physical
                                .checked_add(delta)
                                .ok_or(Error::ArithmeticOverflow)?;
                            if map.and_then(|value| value.physical_block(next_logical))
                                != Some(expected)
                            {
                                break;
                            }
                        }
                        None if map
                            .and_then(|value| value.physical_block(next_logical))
                            .is_none() => {}
                        None => break,
                    }
                    run_blocks += 1;
                }
                let run_bytes = run_blocks
                    .checked_mul(block_len)
                    .ok_or(Error::ArithmeticOverflow)?;
                if let Some(physical) = first_physical {
                    device.read_blocks(physical, &mut batch_buffer[..run_bytes])?;
                } else {
                    batch_buffer[..run_bytes].fill(0);
                }
                writer.write_all(&batch_buffer[..run_bytes])?;
                let run_blocks =
                    u64::try_from(run_blocks).map_err(|_| Error::ArithmeticOverflow)?;
                written = written
                    .checked_add(u64::try_from(run_bytes).map_err(|_| Error::ArithmeticOverflow)?)
                    .ok_or(Error::ArithmeticOverflow)?;
                logical = logical
                    .checked_add(run_blocks)
                    .ok_or(Error::ArithmeticOverflow)?;
            } else {
                if let Some(physical) = map.and_then(|value| value.physical_block(logical)) {
                    device.read_blocks(physical, &mut block_buffer)?;
                } else {
                    block_buffer.fill(0);
                }
                writer.write_all(&block_buffer[within_start..within_end])?;
                written = written
                    .checked_add(
                        u64::try_from(within_end - within_start)
                            .map_err(|_| Error::ArithmeticOverflow)?,
                    )
                    .ok_or(Error::ArithmeticOverflow)?;
                logical = logical.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            }
        }
        writer.flush()?;
        Ok(written)
    }

    /// Stream a persistent file to a host writer after validating its metadata once.
    ///
    /// Unlike repeated calls to `read_persistent_path_range`, this checks the published
    /// namespace, extent-map root, and complete ownership graph only once. File contents
    /// are then emitted one filesystem block at a time, so memory use is bounded by the
    /// filesystem block size even for very large files.
    pub fn stream_persistent_file_to_writer<D: BlockDevice, W: std::io::Write>(
        &self,
        device: &mut D,
        path: &str,
        writer: &mut W,
    ) -> Result<u64> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent file reads require a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::InvalidInput("file metadata is missing"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path is not a regular file"));
        }

        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let map = extent_maps.get(&object_id);
        if record.extent_root_block.is_some() && map.is_none() {
            return Err(Error::Corrupt(
                "file extent map is missing from loaded inventory",
            ));
        }
        if record.extent_root_block.is_none() && map.is_some_and(|value| !value.is_empty()) {
            return Err(Error::Corrupt(
                "file has extents without a persistent extent-map root",
            ));
        }

        // read_namespace_extent_maps has already checked all roots, physical ranges,
        // allocation bits and cross-file aliases before any bytes are emitted.
        let block_size = u64::from(self.volume.block_size);
        let block_len =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        // Bound memory independently of file size while amortizing device calls for
        // contiguous extents. Fragmented blocks and sparse holes remain separate runs.
        // Keep each streaming scratch buffer near 1 MiB regardless of the
        // filesystem block size, avoiding tiny device requests on 4 KiB volumes.
        const STREAM_BATCH_BYTES: usize = 1024 * 1024;
        const MAX_STREAM_BATCH_BLOCKS: usize = 64;
        let stream_batch_blocks =
            (STREAM_BATCH_BYTES / block_len).clamp(1, MAX_STREAM_BATCH_BLOCKS);
        let batch_capacity = block_len
            .checked_mul(stream_batch_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut batch_buffer = try_zeroed_block(
            batch_capacity,
            "unable to allocate persistent stream read batch",
        )?;
        let mut logical = 0_u64;
        let mut remaining = record.size;
        while remaining > 0 {
            if let Some(physical) = map.and_then(|value| value.physical_block(logical)) {
                let mut run_blocks = 1_usize;
                let max_blocks =
                    usize::try_from((remaining / block_size).min(stream_batch_blocks as u64))
                        .map_err(|_| Error::ArithmeticOverflow)?;
                while run_blocks < max_blocks {
                    let next_logical = logical
                        .checked_add(
                            u64::try_from(run_blocks).map_err(|_| Error::ArithmeticOverflow)?,
                        )
                        .ok_or(Error::ArithmeticOverflow)?;
                    let expected_physical = physical
                        .checked_add(
                            u64::try_from(run_blocks).map_err(|_| Error::ArithmeticOverflow)?,
                        )
                        .ok_or(Error::ArithmeticOverflow)?;
                    if map.and_then(|value| value.physical_block(next_logical))
                        != Some(expected_physical)
                    {
                        break;
                    }
                    run_blocks += 1;
                }
                let bytes_to_read = run_blocks
                    .checked_mul(block_len)
                    .ok_or(Error::ArithmeticOverflow)?;
                device.read_blocks(physical, &mut batch_buffer[..bytes_to_read])?;
                let bytes_to_write = usize::try_from(
                    remaining
                        .min(u64::try_from(bytes_to_read).map_err(|_| Error::ArithmeticOverflow)?),
                )
                .map_err(|_| Error::ArithmeticOverflow)?;
                writer.write_all(&batch_buffer[..bytes_to_write])?;
                let consumed_blocks =
                    u64::try_from(run_blocks).map_err(|_| Error::ArithmeticOverflow)?;
                let consumed_bytes =
                    u64::try_from(bytes_to_write).map_err(|_| Error::ArithmeticOverflow)?;
                logical = logical
                    .checked_add(consumed_blocks)
                    .ok_or(Error::ArithmeticOverflow)?;
                remaining -= consumed_bytes;
            } else {
                // Emit a bounded run of sparse zeros without issuing disk reads.
                let mut hole_blocks = 1_usize;
                let max_blocks =
                    usize::try_from((remaining / block_size).min(stream_batch_blocks as u64))
                        .map_err(|_| Error::ArithmeticOverflow)?;
                while hole_blocks < max_blocks {
                    let next_logical = logical
                        .checked_add(
                            u64::try_from(hole_blocks).map_err(|_| Error::ArithmeticOverflow)?,
                        )
                        .ok_or(Error::ArithmeticOverflow)?;
                    if map
                        .and_then(|value| value.physical_block(next_logical))
                        .is_some()
                    {
                        break;
                    }
                    hole_blocks += 1;
                }
                let bytes_to_write = usize::try_from(
                    remaining.min(
                        u64::try_from(
                            hole_blocks
                                .checked_mul(block_len)
                                .ok_or(Error::ArithmeticOverflow)?,
                        )
                        .map_err(|_| Error::ArithmeticOverflow)?,
                    ),
                )
                .map_err(|_| Error::ArithmeticOverflow)?;
                batch_buffer[..bytes_to_write].fill(0);
                writer.write_all(&batch_buffer[..bytes_to_write])?;
                let consumed_blocks =
                    u64::try_from(hole_blocks).map_err(|_| Error::ArithmeticOverflow)?;
                logical = logical
                    .checked_add(consumed_blocks)
                    .ok_or(Error::ArithmeticOverflow)?;
                remaining -=
                    u64::try_from(bytes_to_write).map_err(|_| Error::ArithmeticOverflow)?;
            }
        }
        writer.flush()?;
        Ok(record.size)
    }

    /// Shrink a persistent file and reclaim blocks no longer covered by its new length.
    ///
    /// Extending is intentionally rejected: it needs a separate policy for sparse growth.
    /// The replacement extent map and namespace metadata are published before old blocks
    /// are reclaimed. If publication has an uncertain outcome, staged and old blocks are
    /// retained rather than risking freeing storage still referenced by the active inventory.
    pub fn truncate_persistent_file<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
        new_size: u64,
    ) -> Result<()> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent truncation requires a published inventory",
            ));
        };
        // Resolve and validate the operation before loading extent maps for every file.
        // Invalid paths, directories, extensions and no-op truncations need no extent I/O.
        let object_id = namespace.resolve_path(path)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::Corrupt("resolved file metadata is missing"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path does not identify a regular file"));
        }
        if new_size > record.size {
            return Err(Error::InvalidInput(
                "persistent truncation cannot extend a file",
            ));
        }
        if new_size == record.size {
            return Ok(());
        }
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let next_generation = record
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let block_size = u64::from(self.volume.block_size);
        let block_size_usize =
            usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let mut updated_map = extent_maps
            .get(&object_id)
            .cloned()
            .ok_or(Error::Corrupt("file extent map is missing"))?;
        let keep_blocks = new_size / block_size + u64::from(new_size % block_size != 0);
        let removed_extents = updated_map.truncate(keep_blocks)?;
        let mut reclaim_blocks = Vec::new();
        for extent in removed_extents {
            for delta in 0..extent.block_count {
                reclaim_blocks.push(
                    extent
                        .physical_start
                        .checked_add(delta)
                        .ok_or(Error::ArithmeticOverflow)?,
                );
            }
        }

        let remainder =
            usize::try_from(new_size % block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let old_root = record.extent_root_block;
        let new_root = if updated_map.is_empty() {
            None
        } else {
            let encoded = updated_map.encode_block(block_size_usize)?;
            let root = self.allocate_block(device)?;
            if let Err(error) = device
                .write_blocks(root, &encoded)
                .and_then(|()| device.flush())
            {
                if self.free_block(device, root).is_err() {
                    return Err(Error::Corrupt(
                        "failed to reclaim staged extent-map root after truncation staging failed",
                    ));
                }
                return Err(error);
            }
            Some(root)
        };

        let updated_namespace = (|| -> Result<Namespace> {
            let mut records = try_clone_namespace_records(&namespace)?;
            let updated =
                records
                    .iter_mut()
                    .find(|item| item.id == object_id)
                    .ok_or(Error::Corrupt(
                        "file metadata disappeared during truncation",
                    ))?;
            updated.size = new_size;
            updated.extent_root_block = new_root;
            updated.generation = next_generation;
            let entries = try_clone_namespace_entries(&namespace)?;
            Namespace::from_records(namespace.root_id(), records, entries)
        })();
        let updated_namespace = match updated_namespace {
            Ok(namespace) => namespace,
            Err(error) => {
                if let Some(root) = new_root {
                    if self.reclaim_persistent_blocks(device, &[root]).is_err() {
                        return Err(Error::Corrupt(
                            "failed to reclaim staged extent-map root after truncation metadata preparation failed",
                        ));
                    }
                }
                return Err(error);
            }
        };

        if let Err(error) = self.replace_persistent_namespace_inventory(device, &updated_namespace)
        {
            // Publication can fail after its commit point. Only reclaim the staged root
            // after disk inspection proves that the old version is still authoritative.
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.extent_root_block == new_root
                            && item.size == new_size
                            && item.generation == next_generation
                    }) =>
                {
                    // The new generation is already authoritative. The requested
                    // truncation is committed, so do not report a false failure.
                    return Ok(());
                }
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.extent_root_block == old_root
                            && item.size == record.size
                            && item.generation == record.generation
                    }) =>
                {
                    // Only the exact old generation proves that the staged root is
                    // unreachable and can safely be reclaimed.
                    if let Some(root) = new_root {
                        if self.reclaim_persistent_blocks(device, &[root]).is_err() {
                            return Err(Error::Corrupt(
                                "truncation publication failed and staged extent-map root could not be reclaimed",
                            ));
                        }
                    }
                    return Err(error);
                }
                _ => Err(Error::Corrupt(
                    "truncation publication outcome is uncertain; staged and previous storage were retained",
                ))?,
            }
        }

        // Clear bytes beyond EOF only after publishing the shorter length. Doing this
        // earlier could corrupt the still-authoritative old file if publication failed.
        let tail_cleanup = (|| -> Result<()> {
            if remainder != 0 {
                if let Some(physical) = updated_map.physical_block(new_size / block_size) {
                    let mut contents = try_zeroed_block(
                        block_size_usize,
                        "allocation failed while preparing persistent I/O buffer",
                    )?;
                    device.read_blocks(physical, &mut contents)?;
                    contents[remainder..].fill(0);
                    self.journal.commit(
                        device,
                        &[RedoUpdate {
                            target_block: physical,
                            contents,
                        }],
                    )?;
                }
            }
            Ok(())
        })();

        // The shorter size is already published. Reclaim old storage even if clearing
        // bytes past EOF fails; those bytes are no longer visible through the file API.
        if let Some(root) = old_root {
            reclaim_blocks.push(root);
        }
        if self
            .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .is_err()
        {
            return Err(Error::Corrupt(
                "file was truncated but some old blocks could not be reclaimed",
            ));
        }
        tail_cleanup?;
        Ok(())
    }

    /// Truncate a regular file identified by its persistent path.
    ///
    /// Path resolution is kept at the volume boundary so callers do not need to rebuild
    /// the namespace or translate a path to an internal object ID themselves.
    pub fn truncate_persistent_path<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
        new_size: u64,
    ) -> Result<()> {
        // The canonical truncation routine already resolves the path, validates the
        // target and loads the namespace. Delegate directly so this wrapper does not
        // perform a redundant full inventory reconstruction.
        self.truncate_persistent_file(device, path, new_size)
    }

    /// Replace a persistent file from a reader without buffering the complete input.
    ///
    /// Data is staged one filesystem block at a time. The new extent map is flushed before
    /// namespace publication, and the previous allocation is reclaimed only after publication
    /// is known to have succeeded.
    pub fn write_persistent_file_from_reader<D: BlockDevice, R: std::io::Read>(
        &mut self,
        device: &mut D,
        path: &str,
        reader: &mut R,
    ) -> Result<u64> {
        self.validate_device(device)?;
        let (_, namespace) =
            self.reconstruct_discovered_namespace(device)?
                .ok_or(Error::InvalidInput(
                    "persistent file writes require a published inventory",
                ))?;
        let object_id = namespace.resolve_path(path)?;
        let existing = namespace
            .object(object_id)
            .ok_or(Error::Corrupt("resolved file metadata is missing"))?;
        if existing.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path does not identify a regular file"));
        }
        let next_generation = existing
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;

        // Validate the complete current extent ownership before staging replacement
        // data. This prevents an import from writing new blocks while the old file's
        // map is malformed or aliases storage owned by another file.
        let current_maps = self.read_namespace_extent_maps(device, &namespace)?;
        let reclaim_capacity = if existing.extent_root_block.is_some() {
            let old_map = current_maps
                .get(&object_id)
                .ok_or(Error::Corrupt("file extent map is missing"))?;
            let data_blocks = old_map.extents().iter().try_fold(0_u64, |count, extent| {
                count
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)
            })?;
            usize::try_from(
                data_blocks
                    .checked_add(1)
                    .ok_or(Error::ArithmeticOverflow)?,
            )
            .map_err(|_| Error::ArithmeticOverflow)?
        } else {
            0
        };
        // Replacement reclamation is prepared before staging new data. Reserve the
        // exact old-data/root inventory up front so large overwrites do not repeatedly
        // reallocate this list while the new extents are being written.
        let mut reclaim_blocks = Vec::new();
        reclaim_blocks
            .try_reserve_exact(reclaim_capacity)
            .map_err(|_| Error::InvalidInput("unable to reserve old file block inventory"))?;
        if let Some(old_root) = existing.extent_root_block {
            let maps = &current_maps;
            let old_map = maps
                .get(&object_id)
                .ok_or(Error::Corrupt("file extent map is missing"))?;
            for extent in old_map.extents() {
                for delta in 0..extent.block_count {
                    reclaim_blocks.push(
                        extent
                            .physical_start
                            .checked_add(delta)
                            .ok_or(Error::ArithmeticOverflow)?,
                    );
                }
            }
            reclaim_blocks.push(old_root);
        } else if existing.size != 0 {
            return Err(Error::Corrupt(
                "non-empty file is missing its extent-map root",
            ));
        }

        let mut allocated = Vec::new();
        let staged = (|| -> Result<(Option<u64>, u64)> {
            let mut extent_map = ExtentMap::default();
            let mut file_size = 0_u64;
            let mut logical_block = 0_u64;
            // Keep import staging bounded by bytes, not just block count: block sizes
            // vary by format, so a fixed count can either create excessive scratch memory
            // or issue unnecessarily small allocation/write batches.
            // The cap also stays below the bitmap segment's supported batch-allocation limit.
            const STAGING_BATCH_BYTES: usize = 1024 * 1024;
            const MAX_STAGING_BLOCKS: usize = 64;
            let staging_blocks = (STAGING_BATCH_BYTES / block_size).clamp(1, MAX_STAGING_BLOCKS);
            // Each block retains its exact valid-byte count so the final partial block
            // contributes only its logical payload to the file length.
            let staging_bytes = block_size
                .checked_mul(staging_blocks)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut contents = try_zeroed_block(
                staging_bytes,
                "unable to allocate reader import staging buffer",
            )?;
            // Reuse the small per-batch length table rather than allocating it for
            // every group in a large reader-driven import.
            let mut valid_lengths = Vec::new();
            valid_lengths
                .try_reserve_exact(staging_blocks)
                .map_err(|_| Error::InvalidInput("unable to reserve reader import length table"))?;
            loop {
                valid_lengths.clear();
                for slot in 0..staging_blocks {
                    let start = slot
                        .checked_mul(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let end = start
                        .checked_add(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let block_contents = &mut contents[start..end];
                    let mut filled = 0_usize;
                    while filled < block_size {
                        let count = match reader.read(&mut block_contents[filled..]) {
                            Ok(count) => count,
                            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {
                                continue
                            }
                            Err(error) => return Err(error.into()),
                        };
                        if count == 0 {
                            break;
                        }
                        filled = filled.checked_add(count).ok_or(Error::ArithmeticOverflow)?;
                    }
                    if filled == 0 {
                        break;
                    }
                    // Clear the tail of every partial block so reused staging bytes never
                    // become visible past EOF if the image is inspected below block size.
                    if filled < block_size {
                        block_contents[filled..].fill(0);
                    }
                    valid_lengths.push(filled);
                    if filled < block_size {
                        break;
                    }
                }
                if valid_lengths.is_empty() {
                    break;
                }

                let count =
                    u64::try_from(valid_lengths.len()).map_err(|_| Error::ArithmeticOverflow)?;
                // Reserve rollback tracking before persisting any allocation in this
                // batch. Otherwise growing the tracking Vec happens only after blocks
                // have been allocated, leaving no recoverable inventory if reservation
                // itself fails under memory pressure.
                allocated.try_reserve(valid_lengths.len()).map_err(|_| {
                    Error::InvalidInput("unable to reserve allocation rollback inventory")
                })?;
                let (blocks, already_tracked) = match self.allocate_blocks(device, count) {
                    Ok(blocks) => (blocks, false),
                    // On fragmented volumes no single bitmap segment may have enough
                    // room for the whole group. Fall back to single-block allocation
                    // rather than rejecting a valid import with free blocks available.
                    Err(Error::InvalidInput(
                        "no bitmap segment has enough free data blocks for this batch",
                    )) => {
                        let mut blocks = Vec::new();
                        blocks.try_reserve_exact(valid_lengths.len()).map_err(|_| {
                            Error::InvalidInput("unable to reserve fallback allocation batch")
                        })?;
                        for _ in 0..valid_lengths.len() {
                            let block = self.allocate_block(device)?;
                            // Track each successful allocation immediately: a later
                            // fallback allocation can fail and still require rollback.
                            allocated.push(block);
                            blocks.push(block);
                        }
                        (blocks, true)
                    }
                    Err(error) => return Err(error),
                };
                if !already_tracked {
                    allocated.extend(blocks.iter().copied());
                }
                let mut batch_extents = Vec::new();
                batch_extents
                    .try_reserve_exact(blocks.len())
                    .map_err(|_| Error::InvalidInput("unable to reserve staged file extents"))?;
                for (slot, (block, filled)) in blocks.iter().zip(valid_lengths.iter()).enumerate() {
                    let start = slot
                        .checked_mul(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    let end = start
                        .checked_add(block_size)
                        .ok_or(Error::ArithmeticOverflow)?;
                    device.write_blocks(*block, &contents[start..end])?;
                    batch_extents.push(crate::Extent {
                        logical_start: logical_block,
                        physical_start: *block,
                        block_count: 1,
                    });
                    file_size = file_size
                        .checked_add(u64::try_from(*filled).map_err(|_| Error::ArithmeticOverflow)?)
                        .ok_or(Error::ArithmeticOverflow)?;
                    logical_block = logical_block
                        .checked_add(1)
                        .ok_or(Error::ArithmeticOverflow)?;
                }
                extent_map.push_coalescing_batch(batch_extents)?;
                if valid_lengths
                    .last()
                    .is_some_and(|filled| *filled < block_size)
                {
                    break;
                }
            }

            if logical_block == 0 {
                return Ok((None, 0));
            }
            let encoded = extent_map.encode_block(block_size)?;
            // As with data batches, make rollback bookkeeping infallible before
            // allocating the extent-map root on disk.
            allocated.try_reserve(1).map_err(|_| {
                Error::InvalidInput("unable to reserve allocation rollback inventory")
            })?;
            let root = self.allocate_block(device)?;
            allocated.push(root);
            device.write_blocks(root, &encoded)?;
            device.flush()?;
            Ok((Some(root), file_size))
        })();

        let (new_root, file_size) = match staged {
            Ok(result) => result,
            Err(error) => {
                if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                    return Err(Error::Corrupt(
                        "failed to roll back staged reader-based file allocations",
                    ));
                }
                return Err(error);
            }
        };

        let updated_namespace = (|| -> Result<Namespace> {
            let mut records = try_clone_namespace_records(&namespace)?;
            let updated = records
                .iter_mut()
                .find(|item| item.id == object_id)
                .ok_or(Error::Corrupt("file metadata disappeared during update"))?;
            updated.size = file_size;
            updated.extent_root_block = new_root;
            updated.generation = next_generation;
            Namespace::from_records(namespace.root_id(), records, namespace.entries().cloned())
        })();
        let updated_namespace = match updated_namespace {
            Ok(updated) => updated,
            Err(error) => {
                if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                    return Err(Error::Corrupt(
                        "failed to reclaim reader-based allocations after metadata preparation failed",
                    ));
                }
                return Err(error);
            }
        };

        if let Err(error) = self.replace_persistent_namespace_inventory(device, &updated_namespace)
        {
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.extent_root_block == new_root
                            && item.size == file_size
                            && item.generation == next_generation
                    }) =>
                {
                    // The inventory switch is already durable: preserve the new
                    // allocation and report the committed operation as successful.
                    return Ok(file_size);
                }
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|item| {
                        item.extent_root_block == existing.extent_root_block
                            && item.size == existing.size
                            && item.generation == existing.generation
                    }) =>
                {
                    if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                        return Err(Error::Corrupt(
                            "reader-based file publication failed and staged allocations could not be reclaimed",
                        ));
                    }
                    return Err(error);
                }
                _ => {
                    return Err(Error::Corrupt(
                        "reader-based file publication outcome is uncertain; preserve the image and recover before further writes",
                    ));
                }
            }
        }

        if self
            .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .is_err()
        {
            return Err(Error::Corrupt(
                "reader-based file replacement succeeded but old blocks could not be fully reclaimed",
            ));
        }
        Ok(file_size)
    }

    /// Replace the complete contents of a persistent file, spanning multiple blocks if needed.
    ///
    /// New data blocks and the checksummed extent map are staged and flushed before the
    /// copy-on-write namespace publication. Only after the new contents are published are
    /// the previous data blocks and extent-map root reclaimed. If publication has an uncertain
    /// outcome, both generations are retained rather than risking freeing live file data.
    /// Passing an empty slice truncates the file to zero bytes.
    pub fn write_persistent_file_contents<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
        data: &[u8],
    ) -> Result<()> {
        self.validate_device(device)?;
        let (_, namespace) =
            self.reconstruct_discovered_namespace(device)?
                .ok_or(Error::InvalidInput(
                    "persistent file writes require a published inventory",
                ))?;
        let object_id = namespace.resolve_path(path)?;
        self.write_persistent_file_contents_from_namespace(device, namespace, object_id, data)
    }

    /// Replace file contents from a namespace already reconstructed by the caller.
    /// This keeps the staging/publication/reclamation path shared while avoiding a second
    /// inventory scan for operations that have already resolved the target path.
    fn write_persistent_file_contents_from_namespace<D: BlockDevice>(
        &mut self,
        device: &mut D,
        namespace: Namespace,
        object_id: ObjectId,
        data: &[u8],
    ) -> Result<()> {
        let existing = namespace
            .object(object_id)
            .ok_or(Error::Corrupt("resolved file metadata is missing"))?;
        if existing.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path does not identify a regular file"));
        }
        // Replacing an already-empty file with empty contents changes no persistent
        // state; avoid a generation bump and copy-on-write inventory churn.
        if data.is_empty() && existing.size == 0 && existing.extent_root_block.is_none() {
            return Ok(());
        }
        let next_generation = existing
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let file_size = u64::try_from(data.len()).map_err(|_| Error::ArithmeticOverflow)?;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;

        // Capture the old allocation before staging anything, so reclamation never relies
        // on stale caller metadata and all ownership checks happen before publication.
        let mut reclaim_blocks = Vec::new();
        if let Some(old_root) = existing.extent_root_block {
            let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
            let old_map = extent_maps
                .get(&object_id)
                .ok_or(Error::Corrupt("file extent map is missing"))?;
            for extent in old_map.extents() {
                for delta in 0..extent.block_count {
                    reclaim_blocks.push(
                        extent
                            .physical_start
                            .checked_add(delta)
                            .ok_or(Error::ArithmeticOverflow)?,
                    );
                }
            }
            reclaim_blocks.push(old_root);
        } else if existing.size != 0 {
            return Err(Error::Corrupt(
                "non-empty file is missing its extent-map root",
            ));
        }

        let block_count = data
            .len()
            .checked_add(block_size - 1)
            .ok_or(Error::ArithmeticOverflow)?
            / block_size;
        let mut allocated = Vec::new();
        allocated
            .try_reserve_exact(block_count.saturating_add(1))
            .map_err(|_| {
                Error::InvalidInput("allocation failed while staging persistent namespace data")
            })?;
        let staged = (|| -> Result<Option<u64>> {
            if data.is_empty() {
                return Ok(None);
            }
            let mut extent_map = ExtentMap::default();
            // Allocate data in small bitmap batches: each successful batch journals one
            // bitmap image instead of committing once per file block. If fragmentation
            // leaves no segment with enough free blocks for a batch, fall back to single
            // allocations rather than rejecting a file that could still fit on the volume.
            const ALLOCATION_BATCH: usize = 64;
            let chunks = data.chunks(block_size);
            let block_count = chunks.len();
            for remaining in (0..block_count).step_by(ALLOCATION_BATCH) {
                let batch_count = (block_count - remaining).min(ALLOCATION_BATCH);
                match self.allocate_blocks(device, batch_count as u64) {
                    Ok(blocks) => allocated.extend(blocks),
                    Err(Error::InvalidInput(_)) => {
                        for _ in 0..batch_count {
                            allocated.push(self.allocate_block(device)?);
                        }
                    }
                    Err(error) => return Err(error),
                }
            }

            // Keep memory usage independent of file length: one block buffer is
            // reused for every staged block, with the tail cleared so the final
            // partial block cannot expose bytes from the preceding chunk.
            let mut bytes = try_zeroed_block(
                block_size,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            for (index, chunk) in data.chunks(block_size).enumerate() {
                bytes[..chunk.len()].copy_from_slice(chunk);
                if chunk.len() < block_size {
                    // Full chunks overwrite the whole buffer. Only the final partial
                    // chunk needs clearing, avoiding an unnecessary full-block memset
                    // for every complete block in large replacements.
                    bytes[chunk.len()..].fill(0);
                }
                let block = *allocated
                    .get(index)
                    .ok_or(Error::Corrupt("staged file allocation list is incomplete"))?;
                device.write_blocks(block, &bytes)?;
                extent_map.push_coalescing(crate::Extent {
                    logical_start: u64::try_from(index).map_err(|_| Error::ArithmeticOverflow)?,
                    physical_start: block,
                    block_count: 1,
                })?;
            }

            let extent_bytes = extent_map.encode_block(block_size)?;
            let extent_root = self.allocate_block(device)?;
            allocated.push(extent_root);
            device.write_blocks(extent_root, &extent_bytes)?;
            device.flush()?;
            Ok(Some(extent_root))
        })();

        let extent_root = match staged {
            Ok(root) => root,
            Err(error) => {
                // No published namespace can reference these allocations yet. Attempt
                // every rollback, but surface cleanup failure instead of hiding a leak.
                if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                    return Err(Error::Corrupt(
                        "failed to roll back staged persistent file allocations",
                    ));
                }
                return Err(error);
            }
        };

        let updated_namespace = (|| -> Result<Namespace> {
            let mut records = try_clone_namespace_records(&namespace)?;
            let updated = records
                .iter_mut()
                .find(|record| record.id == object_id)
                .ok_or(Error::Corrupt("file metadata disappeared during update"))?;
            updated.size = file_size;
            updated.extent_root_block = extent_root;
            updated.generation = next_generation;
            let entries = try_clone_namespace_entries(&namespace)?;
            Namespace::from_records(namespace.root_id(), records, entries)
        })();
        let updated_namespace = match updated_namespace {
            Ok(namespace) => namespace,
            Err(error) => {
                // These allocations are still unpublished, so rollback is safe.
                if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                    return Err(Error::Corrupt(
                        "failed to reclaim staged persistent file allocations after metadata preparation failed",
                    ));
                }
                return Err(error);
            }
        };

        // A publication error may be reported after the journal commit point. Rediscover
        // the active file version before deciding whether staged data can be reclaimed.
        if let Err(error) = self.replace_persistent_namespace_inventory(device, &updated_namespace)
        {
            match self.reconstruct_discovered_namespace(device) {
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|record| {
                        record.extent_root_block == extent_root
                            && record.size == file_size
                            && record.generation == next_generation
                    }) =>
                {
                    // The exact new version is active; keep its allocations and report success.
                    return Ok(());
                }
                Ok(Some((_, active)))
                    if active.object(object_id).is_some_and(|record| {
                        record.extent_root_block == existing.extent_root_block
                            && record.size == existing.size
                            && record.generation == existing.generation
                    }) =>
                {
                    // Only the exact old version proves that staged allocations are unreachable.
                    if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                        return Err(Error::Corrupt(
                            "file publication failed and staged data could not be reclaimed",
                        ));
                    }
                    return Err(error);
                }
                _ => {
                    // The publication outcome is unknown. Retain staged blocks to avoid
                    // freeing data that may already be referenced by the active inventory.
                    return Err(error);
                }
            }
        }

        if self
            .reclaim_persistent_blocks_in_place(device, &mut reclaim_blocks)
            .is_err()
        {
            return Err(Error::Corrupt(
                "file contents were replaced but some old blocks could not be reclaimed",
            ));
        }
        Ok(())
    }

    /// Write bytes to a file resolved from the published on-disk inventory.
    ///
    /// The namespace and extent maps are reconstructed from disk rather than supplied by
    /// the caller. Writes inside the existing file must stay within its allocated extents.
    /// A write beginning at EOF appends data and may allocate new extents when the final
    /// allocated block has insufficient space. Sparse-hole writes and ranges beyond EOF
    /// that do not begin exactly at EOF are rejected.
    pub fn write_persistent_path_range<D: BlockDevice>(
        &mut self,
        device: &mut D,
        path: &str,
        offset: u64,
        data: &[u8],
    ) -> Result<()> {
        self.validate_device(device)?;
        let Some((_, namespace)) = self.reconstruct_discovered_namespace(device)? else {
            return Err(Error::InvalidInput(
                "persistent path writes require a published inventory",
            ));
        };
        let object_id = namespace.resolve_path(path)?;
        let record = namespace
            .object(object_id)
            .ok_or(Error::Corrupt("resolved file metadata is missing"))?;
        if record.kind != crate::ObjectKind::File {
            return Err(Error::InvalidInput("path does not identify a regular file"));
        }
        // An empty range write must not create a new metadata generation or rewrite
        // the persistent inventory. Resolve the path first so invalid paths still fail.
        if data.is_empty() {
            return Ok(());
        }
        // An empty file has no extent map to inspect. Route its first append directly
        // through copy-on-write replacement rather than scanning every file's extents.
        if offset == record.size && record.size == 0 && record.extent_root_block.is_none() {
            return self
                .write_persistent_file_contents_from_namespace(device, namespace, object_id, data);
        }
        // Extent ownership is expensive to reconstruct, so defer it until the path and
        // operation are known to require actual data I/O.
        let extent_maps = self.read_namespace_extent_maps(device, &namespace)?;
        // Appends that fit in the current final block need no extent-map replacement.
        // Larger appends stage new data and a new extent root before publishing the new size.
        if !data.is_empty() && offset == record.size {
            let block_size = u64::from(self.volume.block_size);
            let data_len = u64::try_from(data.len()).map_err(|_| Error::ArithmeticOverflow)?;
            let end = offset
                .checked_add(data_len)
                .ok_or(Error::ArithmeticOverflow)?;

            let old_root = record.extent_root_block.ok_or(Error::Corrupt(
                "non-empty file is missing its extent-map root",
            ))?;
            let map = extent_maps
                .get(&object_id)
                .ok_or(Error::Corrupt("file extent map is missing"))?;

            let tail_offset = offset % block_size;
            let tail_capacity = if tail_offset == 0 {
                0
            } else {
                block_size - tail_offset
            };
            if tail_capacity > 0 && data_len <= tail_capacity {
                let physical_block =
                    map.physical_block(offset / block_size)
                        .ok_or(Error::InvalidInput(
                            "append would extend into a sparse hole",
                        ))?;
                let in_block =
                    usize::try_from(tail_offset).map_err(|_| Error::ArithmeticOverflow)?;
                // Prepare every fallible metadata change before modifying the old
                // tail in place. In particular, generation overflow must not leave
                // unpublished bytes beyond EOF in an otherwise unchanged file.
                let next_generation = record
                    .generation
                    .checked_add(1)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut records = try_clone_namespace_records(&namespace)?;
                let updated = records
                    .iter_mut()
                    .find(|item| item.id == object_id)
                    .ok_or(Error::Corrupt("file metadata disappeared during append"))?;
                updated.size = end;
                updated.generation = next_generation;
                let entries = try_clone_namespace_entries(&namespace)?;
                let updated_namespace =
                    Namespace::from_records(namespace.root_id(), records, entries)?;
                let mut block = try_zeroed_block(
                    self.volume.block_size as usize,
                    "allocation failed while preparing persistent I/O buffer",
                )?;
                device.read_blocks(physical_block, &mut block)?;
                let finish = in_block
                    .checked_add(data.len())
                    .ok_or(Error::ArithmeticOverflow)?;
                block[in_block..finish].copy_from_slice(data);
                // The data block is journaled before the namespace size is published.
                // If this write fails after a durable commit record, or publication later
                // proves the old generation is still active, bytes past the old EOF must be
                // scrubbed. Otherwise a shorter retrying append could expose the tail left
                // by this failed append.
                let commit_result = self.journal.commit(
                    device,
                    &[RedoUpdate {
                        target_block: physical_block,
                        contents: block,
                    }],
                );
                if let Err(error) = commit_result {
                    let cleanup = (|| -> Result<()> {
                        let mut current = try_zeroed_block(
                            self.volume.block_size as usize,
                            "allocation failed while preparing persistent I/O buffer",
                        )?;
                        device.read_blocks(physical_block, &mut current)?;
                        current[in_block..].fill(0);
                        self.journal.commit(
                            device,
                            &[RedoUpdate {
                                target_block: physical_block,
                                contents: current,
                            }],
                        )?;
                        Ok(())
                    })();
                    if cleanup.is_err() {
                        return Err(Error::Corrupt(
                            "append data write failed and bytes beyond EOF could not be cleared",
                        ));
                    }
                    return Err(error);
                }

                if let Err(error) =
                    self.replace_persistent_namespace_inventory(device, &updated_namespace)
                {
                    // Publication can fail after its journal commit point. Resolve the
                    // active generation before reporting whether the append took effect.
                    match self.reconstruct_discovered_namespace(device) {
                        Ok(Some((_, active)))
                            if active.object(object_id).is_some_and(|item| {
                                item.size == end
                                    && item.generation == next_generation
                                    && item.extent_root_block == record.extent_root_block
                            }) =>
                        {
                            return Err(error);
                        }
                        Ok(Some((_, active)))
                            if active.object(object_id).is_some_and(|item| {
                                item.size == record.size
                                    && item.generation == record.generation
                                    && item.extent_root_block == record.extent_root_block
                            }) =>
                        {
                            // The old generation is still authoritative, so the in-place
                            // tail update is not visible yet. Restore the zero-after-EOF
                            // invariant before allowing another append attempt.
                            let cleanup = (|| -> Result<()> {
                                let mut current = try_zeroed_block(
                                    self.volume.block_size as usize,
                                    "allocation failed while preparing persistent I/O buffer",
                                )?;
                                device.read_blocks(physical_block, &mut current)?;
                                current[in_block..].fill(0);
                                self.journal.commit(
                                    device,
                                    &[RedoUpdate {
                                        target_block: physical_block,
                                        contents: current,
                                    }],
                                )?;
                                Ok(())
                            })();
                            if cleanup.is_err() {
                                return Err(Error::Corrupt(
                                    "append publication failed with the old generation active, but bytes beyond EOF could not be cleared",
                                ));
                            }
                            return Err(error);
                        }
                        _ => {
                            return Err(Error::Corrupt(
                                "append metadata publication outcome is uncertain; preserve the image and recover before further writes",
                            ));
                        }
                    }
                }
                return Ok(());
            }

            let next_generation = record
                .generation
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow)?;
            let block_size_usize =
                usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
            let max_data_blocks = data
                .len()
                .checked_add(block_size_usize - 1)
                .ok_or(Error::ArithmeticOverflow)?
                / block_size_usize;
            let tracking_capacity = max_data_blocks
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut allocated = Vec::new();
            // Reserve enough ownership-tracking space before the first disk mutation.
            // Otherwise a later Vec growth could abort while newly allocated blocks
            // have already been committed in the bitmap and cannot be rolled back.
            allocated
                .try_reserve_exact(tracking_capacity)
                .map_err(|_| {
                    Error::InvalidInput("allocation failed while tracking appended data blocks")
                })?;
            // Appending across blocks may modify the old final block before the new
            // extent map is published. Remember even an errored journal commit as a
            // possible durable tail write so every pre-publication failure can restore
            // the old EOF invariant.
            let mut modified_old_tail = false;
            let staged = (|| -> Result<u64> {
                let mut updated_map = map.clone();
                let mut consumed = 0_usize;
                let mut next_logical = offset / block_size;

                if tail_capacity > 0 {
                    let physical = map.physical_block(next_logical).ok_or(Error::InvalidInput(
                        "append would extend into a sparse hole",
                    ))?;
                    let tail_bytes = usize::try_from(tail_capacity.min(data_len))
                        .map_err(|_| Error::ArithmeticOverflow)?;
                    let tail_offset_usize =
                        usize::try_from(tail_offset).map_err(|_| Error::ArithmeticOverflow)?;
                    let mut block = try_zeroed_block(
                        block_size_usize,
                        "allocation failed while preparing persistent I/O buffer",
                    )?;
                    device.read_blocks(physical, &mut block)?;
                    let finish = tail_offset_usize
                        .checked_add(tail_bytes)
                        .ok_or(Error::ArithmeticOverflow)?;
                    block[tail_offset_usize..finish].copy_from_slice(&data[..tail_bytes]);
                    modified_old_tail = true;
                    self.journal.commit(
                        device,
                        &[RedoUpdate {
                            target_block: physical,
                            contents: block,
                        }],
                    )?;
                    consumed = tail_bytes;
                    next_logical = next_logical
                        .checked_add(1)
                        .ok_or(Error::ArithmeticOverflow)?;
                }

                // Allocate appended data in bounded groups to amortize bitmap updates
                // without making temporary memory proportional to the entire append.
                // Every successful fallback allocation is tracked immediately for rollback.
                const APPEND_BATCH_BLOCKS: usize = 8;
                let mut contents = try_zeroed_block(
                    block_size_usize,
                    "allocation failed while preparing persistent I/O buffer",
                )?;
                let batch_bytes = block_size_usize
                    .checked_mul(APPEND_BATCH_BLOCKS)
                    .ok_or(Error::ArithmeticOverflow)?;
                for byte_group in data[consumed..].chunks(batch_bytes) {
                    let chunk_count = byte_group.len() / block_size_usize
                        + if byte_group.len() % block_size_usize != 0 {
                            1
                        } else {
                            0
                        };
                    let count =
                        u64::try_from(chunk_count).map_err(|_| Error::ArithmeticOverflow)?;
                    let (blocks, already_tracked) = match self.allocate_blocks(device, count) {
                        Ok(blocks) => (blocks, false),
                        Err(Error::InvalidInput(
                            "no bitmap segment has enough free data blocks for this batch",
                        )) => {
                            let mut blocks = Vec::new();
                            blocks.try_reserve_exact(chunk_count).map_err(|_| {
                                Error::InvalidInput(
                                    "allocation failed while staging persistent namespace data",
                                )
                            })?;
                            for _ in 0..chunk_count {
                                let block = self.allocate_block(device)?;
                                allocated.push(block);
                                blocks.push(block);
                            }
                            (blocks, true)
                        }
                        Err(error) => return Err(error),
                    };
                    if !already_tracked {
                        allocated.extend(blocks.iter().copied());
                    }
                    for (chunk, block) in byte_group.chunks(block_size_usize).zip(blocks.iter()) {
                        contents.fill(0);
                        contents[..chunk.len()].copy_from_slice(chunk);
                        device.write_blocks(*block, &contents)?;
                        updated_map.insert_coalescing(crate::Extent {
                            logical_start: next_logical,
                            physical_start: *block,
                            block_count: 1,
                        })?;
                        next_logical = next_logical
                            .checked_add(1)
                            .ok_or(Error::ArithmeticOverflow)?;
                    }
                }

                let encoded = updated_map.encode_block(block_size_usize)?;
                let new_root = self.allocate_block(device)?;
                allocated.push(new_root);
                device.write_blocks(new_root, &encoded)?;
                device.flush()?;
                Ok(new_root)
            })();

            let new_root = match staged {
                Ok(root) => root,
                Err(error) => {
                    if modified_old_tail {
                        let cleanup = (|| -> Result<()> {
                            let physical = map
                                .physical_block(offset / block_size)
                                .ok_or(Error::Corrupt("append tail disappeared during rollback"))?;
                            let mut current = try_zeroed_block(
                                block_size_usize,
                                "allocation failed while preparing persistent I/O buffer",
                            )?;
                            device.read_blocks(physical, &mut current)?;
                            let tail_start = usize::try_from(tail_offset)
                                .map_err(|_| Error::ArithmeticOverflow)?;
                            current[tail_start..].fill(0);
                            self.journal.commit(
                                device,
                                &[RedoUpdate {
                                    target_block: physical,
                                    contents: current,
                                }],
                            )?;
                            Ok(())
                        })();
                        if cleanup.is_err() {
                            return Err(Error::Corrupt(
                                "append staging failed and bytes beyond the old EOF could not be cleared",
                            ));
                        }
                    }
                    if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                        return Err(Error::Corrupt(
                            "failed to roll back staged append allocations",
                        ));
                    }
                    return Err(error);
                }
            };

            let updated_namespace = (|| -> Result<Namespace> {
                let mut records = try_clone_namespace_records(&namespace)?;
                let updated = records
                    .iter_mut()
                    .find(|item| item.id == object_id)
                    .ok_or(Error::Corrupt("file metadata disappeared during append"))?;
                updated.size = end;
                updated.extent_root_block = Some(new_root);
                updated.generation = next_generation;
                let entries = try_clone_namespace_entries(&namespace)?;
                Namespace::from_records(namespace.root_id(), records, entries)
            })();
            let updated_namespace = match updated_namespace {
                Ok(namespace) => namespace,
                Err(error) => {
                    // The old final block may already contain unpublished append bytes.
                    // Restore its zero-after-EOF invariant before returning to the caller.
                    if modified_old_tail {
                        let cleanup = (|| -> Result<()> {
                            let physical =
                                map.physical_block(offset / block_size)
                                    .ok_or(Error::Corrupt(
                                        "append tail disappeared during metadata rollback",
                                    ))?;
                            let mut current = try_zeroed_block(
                                block_size_usize,
                                "allocation failed while preparing persistent I/O buffer",
                            )?;
                            device.read_blocks(physical, &mut current)?;
                            let tail_start = usize::try_from(tail_offset)
                                .map_err(|_| Error::ArithmeticOverflow)?;
                            current[tail_start..].fill(0);
                            self.journal.commit(
                                device,
                                &[RedoUpdate {
                                    target_block: physical,
                                    contents: current,
                                }],
                            )?;
                            Ok(())
                        })();
                        if cleanup.is_err() {
                            let _ = self.reclaim_persistent_blocks(device, &allocated);
                            return Err(Error::Corrupt(
                                "append metadata preparation failed and bytes beyond EOF could not be cleared",
                            ));
                        }
                    }
                    if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                        return Err(Error::Corrupt(
                            "failed to reclaim staged append allocations after metadata preparation failed",
                        ));
                    }
                    return Err(error);
                }
            };

            match self.replace_persistent_namespace_inventory(device, &updated_namespace) {
                Ok(_) => {
                    // Publication has removed the old root from the live namespace, so it
                    // is now safe to release. If freeing fails, the new file remains valid.
                    self.free_block(device, old_root)?;
                    return Ok(());
                }
                Err(error) => {
                    // The journal may have committed despite a later I/O error. Rediscover
                    // before deciding whether staged allocations are safe to roll back.
                    match self.reconstruct_discovered_namespace(device) {
                        Ok(Some((_, active)))
                            if active
                                .object(object_id)
                                .is_some_and(|item| item.extent_root_block == Some(new_root)) =>
                        {
                            if self.free_block(device, old_root).is_err() {
                                return Err(Error::Corrupt(
                                    "append was published but the previous extent-map root could not be reclaimed",
                                ));
                            }
                            return Err(error);
                        }
                        Ok(Some((_, active)))
                            if active
                                .object(object_id)
                                .is_some_and(|item| item.extent_root_block == Some(old_root)) =>
                        {
                            if modified_old_tail {
                                let cleanup = (|| -> Result<()> {
                                    let physical = map.physical_block(offset / block_size).ok_or(
                                        Error::Corrupt("append tail disappeared during rollback"),
                                    )?;
                                    let mut current = try_zeroed_block(
                                        block_size_usize,
                                        "allocation failed while preparing persistent I/O buffer",
                                    )?;
                                    device.read_blocks(physical, &mut current)?;
                                    let tail_start = usize::try_from(tail_offset)
                                        .map_err(|_| Error::ArithmeticOverflow)?;
                                    current[tail_start..].fill(0);
                                    self.journal.commit(
                                        device,
                                        &[RedoUpdate {
                                            target_block: physical,
                                            contents: current,
                                        }],
                                    )?;
                                    Ok(())
                                })();
                                if cleanup.is_err() {
                                    return Err(Error::Corrupt(
                                        "append publication failed with the old generation active, but bytes beyond EOF could not be cleared",
                                    ));
                                }
                            }
                            if self.reclaim_persistent_blocks(device, &allocated).is_err() {
                                return Err(Error::Corrupt(
                                    "failed to roll back unpublished append allocations",
                                ));
                            }
                            return Err(error);
                        }
                        _ => return Err(Error::Corrupt(
                            "append publication outcome is uncertain; staged blocks were retained",
                        )),
                    }
                }
            }
        }

        self.write_file_range(device, &namespace, &extent_maps, object_id, offset, data)
    }

    /// Load every file's extent-map page and validate global physical ownership.
    ///
    /// This detects extent runs that alias another file's map root or data, validates
    /// each run against the bitmap, and returns maps keyed by stable object ID. It still
    /// depends on a namespace reconstructed from caller-located metadata inventories.
    pub fn read_namespace_extent_maps<D: BlockDevice>(
        &self,
        device: &mut D,
        namespace: &Namespace,
    ) -> Result<std::collections::BTreeMap<ObjectId, ExtentMap>> {
        self.validate_device(device)?;
        let roots = self.validate_namespace_extent_roots(device, namespace)?;
        let mut root_owners = std::collections::BTreeMap::new();
        for record in namespace.objects() {
            if let Some(root) = record.extent_root_block {
                root_owners.insert(root, record.id);
            }
        }
        let mut maps = std::collections::BTreeMap::new();
        let mut owned_ranges: Vec<(u64, u64, ObjectId)> = Vec::new();
        for record in namespace.objects() {
            let Some(root) = record.extent_root_block else {
                continue;
            };
            // validate_namespace_extent_roots already checked this root's range and
            // allocation bit in a bitmap-page-cached pass above.
            let map =
                self.read_extent_map_after_root_validation(device, root, record.size, false)?;
            let logical_blocks = record.size / u64::from(self.volume.block_size)
                + u64::from(record.size % u64::from(self.volume.block_size) != 0);
            for (start, end) in map.validated_physical_ranges(
                logical_blocks,
                self.volume.layout.data_start,
                self.volume.layout.backup_superblock,
            )? {
                // The root inventory is sorted and unique. Binary search avoids a
                // roots-by-extents scan on filesystems with many small files.
                let root_index = roots.partition_point(|root_block| *root_block < start);
                if roots
                    .get(root_index)
                    .is_some_and(|root_block| *root_block < end)
                {
                    return Err(Error::Corrupt("file data aliases an extent-map root"));
                }
                owned_ranges.push((start, end, record.id));
            }
            maps.insert(record.id, map);
        }
        owned_ranges.sort_unstable_by_key(|(start, end, owner)| (*start, *end, *owner));
        for pair in owned_ranges.windows(2) {
            // Physical blocks must have exactly one logical owner. Checking only
            // cross-file overlaps misses two extents in the same file that alias
            // the same storage, which can make writes to one logical range silently
            // corrupt another range in that file.
            if pair[0].1 > pair[1].0 {
                return Err(Error::Corrupt(
                    "file extents claim overlapping physical data blocks",
                ));
            }
        }
        // Validate every file's physical allocations in one ordered bitmap scan instead
        // of rereading the same bitmap pages separately for every file.
        let mut physical_ranges = Vec::new();
        physical_ranges
            .try_reserve_exact(owned_ranges.len())
            .map_err(|_| Error::InvalidInput("unable to allocate physical ownership ranges"))?;
        physical_ranges.extend(owned_ranges.iter().map(|(start, end, _)| (*start, *end)));
        self.validate_physical_ranges_allocated(device, &physical_ranges)?;
        // Keep this explicit: every root must map to exactly one object, even if a future
        // Namespace implementation changes its validation guarantees.
        if root_owners.len() != roots.len() {
            return Err(Error::Corrupt(
                "extent-map root ownership inventory disagrees",
            ));
        }
        Ok(maps)
    }

    fn validate_extent_allocations<D: BlockDevice>(
        &self,
        device: &mut D,
        map: &ExtentMap,
        logical_blocks: u64,
    ) -> Result<()> {
        let ranges = map.validated_physical_ranges(
            logical_blocks,
            self.volume.layout.data_start,
            self.volume.layout.backup_superblock,
        )?;
        self.validate_physical_ranges_allocated(device, &ranges)
    }

    /// Validate ordered, non-overlapping physical ranges using one cached bitmap page.
    /// Callers must validate range bounds and ownership overlaps before entering here.
    fn validate_physical_ranges_allocated<D: BlockDevice>(
        &self,
        device: &mut D,
        ranges: &[(u64, u64)],
    ) -> Result<()> {
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        // validated_physical_ranges returns ranges in ascending physical order. Therefore
        // the bitmap page index can only stay the same or increase as we scan; one-page
        // caching is sufficient and avoids repeated BTreeMap lookups and cache eviction
        // for highly fragmented files.
        let mut cached_bitmap_index = None;
        let mut cached_bitmap = try_zeroed_block(
            self.volume.block_size as usize,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        for &(start, end) in ranges {
            let mut cursor = start;
            while cursor < end {
                let bitmap_index = cursor / bits_per_block;
                let base = bitmap_index
                    .checked_mul(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?;
                if cached_bitmap_index != Some(bitmap_index) {
                    let bitmap_block = self
                        .volume
                        .layout
                        .bitmap_start
                        .checked_add(bitmap_index)
                        .ok_or(Error::ArithmeticOverflow)?;
                    device.read_blocks(bitmap_block, &mut cached_bitmap)?;
                    let segment_start = self.volume.layout.data_start.max(base);
                    let segment_end = self.volume.layout.backup_superblock.min(
                        base.checked_add(bits_per_block)
                            .ok_or(Error::ArithmeticOverflow)?,
                    );
                    crate::allocator_extents::validate_bitmap_reserved_bits(
                        &cached_bitmap,
                        base,
                        segment_start,
                        segment_end,
                    )?;
                    cached_bitmap_index = Some(bitmap_index);
                }

                // Check allocation a byte at a time instead of visiting every physical
                // block individually. Large contiguous extents now need O(bytes) bitmap
                // work, while masks keep the first and last partial bytes exact.
                let segment_end = end.min(
                    base.checked_add(bits_per_block)
                        .ok_or(Error::ArithmeticOverflow)?,
                );
                let local_start = cursor - base;
                let local_end = segment_end - base;
                let first_byte =
                    usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
                let end_byte =
                    usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                        .map_err(|_| Error::ArithmeticOverflow)?;
                // Start iteration at the first byte in this extent segment.
                // Using enumerate().take().skip() on the full bitmap walks every
                // preceding byte again for each fragmented extent, making validation
                // needlessly quadratic within a bitmap page.
                for (relative_index, bitmap_byte) in
                    cached_bitmap[first_byte..end_byte].iter().enumerate()
                {
                    let byte_index = first_byte + relative_index;
                    let mut mask = u8::MAX;
                    if byte_index == first_byte {
                        mask &= u8::MAX << (local_start % 8);
                    }
                    if byte_index + 1 == end_byte && local_end % 8 != 0 {
                        mask &= (1_u8 << (local_end % 8)) - 1;
                    }
                    if bitmap_byte & mask != mask {
                        return Err(Error::Corrupt("file extent points to a free data block"));
                    }
                }
                cursor = segment_end;
            }
        }
        Ok(())
    }

    fn read_packed_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
        blocks: &[u64],
        record_count: usize,
        record_size: usize,
    ) -> Result<Vec<u8>> {
        self.validate_device(device)?;
        let required_bytes = record_count
            .checked_mul(record_size)
            .ok_or(Error::ArithmeticOverflow)?;
        let block_size =
            usize::try_from(self.volume.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        let required_blocks = required_bytes
            .checked_add(block_size - 1)
            .ok_or(Error::ArithmeticOverflow)?
            / block_size;
        if u64::try_from(required_blocks).map_err(|_| Error::ArithmeticOverflow)?
            > self.volume.layout.data_blocks()
        {
            return Err(Error::InvalidInput(
                "inventory exceeds the usable data region",
            ));
        }
        if blocks.len() != required_blocks {
            return Err(Error::InvalidInput(
                "inventory block list does not match its record count",
            ));
        }
        let mut seen = std::collections::BTreeSet::new();
        let mut allocation_bitmaps = std::collections::BTreeMap::<u64, Vec<u8>>::new();
        let bits_per_bitmap_block = u64::from(self.volume.block_size) * 8;
        for block in blocks {
            if !seen.insert(*block) {
                return Err(Error::Corrupt("inventory repeats a physical block"));
            }
            if *block < self.volume.layout.data_start
                || *block >= self.volume.layout.backup_superblock
            {
                return Err(Error::Corrupt("inventory block is outside the data region"));
            }

            let bitmap_index = *block / bits_per_bitmap_block;
            if !allocation_bitmaps.contains_key(&bitmap_index)
                && allocation_bitmaps.len() >= INVENTORY_BITMAP_CACHE_PAGES
            {
                allocation_bitmaps.clear();
            }
            if let std::collections::btree_map::Entry::Vacant(entry) =
                allocation_bitmaps.entry(bitmap_index)
            {
                let base = bitmap_index
                    .checked_mul(bits_per_bitmap_block)
                    .ok_or(Error::ArithmeticOverflow)?;
                let bitmap_block = self
                    .volume
                    .layout
                    .bitmap_start
                    .checked_add(bitmap_index)
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut bitmap = try_zeroed_block(
                    block_size,
                    "allocation failed while preparing persistent I/O buffer",
                )?;
                device.read_blocks(bitmap_block, &mut bitmap)?;
                let segment_start = self.volume.layout.data_start.max(base);
                let segment_end = self.volume.layout.backup_superblock.min(
                    base.checked_add(bits_per_bitmap_block)
                        .ok_or(Error::ArithmeticOverflow)?,
                );
                crate::allocator_extents::validate_bitmap_reserved_bits(
                    &bitmap,
                    base,
                    segment_start,
                    segment_end,
                )?;
                entry.insert(bitmap);
            }
            let bitmap = allocation_bitmaps
                .get(&bitmap_index)
                .ok_or(Error::Corrupt("allocation bitmap page cache is incomplete"))?;
            let local_bit = *block % bits_per_bitmap_block;
            let byte_index =
                usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte_index] & (1_u8 << (local_bit % 8)) == 0 {
                return Err(Error::Corrupt(
                    "inventory block is marked free in the bitmap",
                ));
            }
        }

        // Preserve caller-defined stream order, while coalescing adjacent physical
        // blocks into bounded reads. Non-adjacent blocks remain separate runs.
        let packed_capacity = required_blocks
            .checked_mul(block_size)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut packed = Vec::new();
        packed.try_reserve_exact(packed_capacity).map_err(|_| {
            Error::InvalidInput("allocation failed while staging persistent namespace data")
        })?;
        let mut cursor = 0;
        while cursor < blocks.len() {
            let mut end = cursor + 1;
            while end < blocks.len()
                && end - cursor < INVENTORY_DISCOVERY_BATCH_BLOCKS
                && blocks[end - 1].checked_add(1) == Some(blocks[end])
            {
                end += 1;
            }
            let run_blocks = end - cursor;
            let run_bytes = run_blocks
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut batch = try_zeroed_block(
                run_bytes,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            device.read_blocks(blocks[cursor], &mut batch)?;
            packed.extend_from_slice(&batch);
            cursor = end;
        }
        packed.truncate(required_bytes);
        Ok(packed)
    }

    fn validate_device<D: BlockDevice>(&self, device: &D) -> Result<()> {
        if device.block_size() != self.volume.block_size
            || device.block_count() != self.volume.total_blocks
        {
            return Err(Error::InvalidInput(
                "device geometry does not match opened volume",
            ));
        }
        let canonical = VolumeLayout::calculate_for_version(
            self.volume.block_size,
            self.volume.total_blocks,
            self.format_version,
        )?;
        if self.volume.layout != canonical {
            return Err(Error::InvalidInput(
                "volume layout was modified after validation",
            ));
        }
        Ok(())
    }
}

/// Result of a non-mutating volume check.
///
/// This verifies the primary/backup superblocks, canonical geometry, and reserved/padding
/// bitmap invariants. It deliberately does not replay the journal or claim namespace
/// ownership integrity; use `open_volume` when recovery is intended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VolumeCheckReport {
    pub block_size: u32,
    pub total_blocks: u64,
    pub uuid: [u8; 16],
    pub bitmap_start: u64,
    pub bitmap_blocks: u64,
    pub data_start: u64,
    pub backup_superblock: u64,
    pub data_blocks: u64,
    pub allocated_data_blocks: u64,
    pub free_data_blocks: u64,
    /// Generation of the newest valid journal checkpoint copy.
    pub journal_generation: u64,
    /// Highest journal sequence known to have been applied to home blocks.
    pub journal_checkpoint_sequence: u64,
    /// Valid post-checkpoint journal records found in the bounded ring.
    pub pending_journal_records: u64,
    /// Fully committed transactions represented by the pending records.
    pub recoverable_transactions: u64,
    /// Whether a persistent namespace inventory descriptor was found in allocated data blocks.
    pub persistent_inventory_present: bool,
}

/// Inspect static volume metadata without writing to the device.
///
/// Unlike `open_volume`, this function does not recover or checkpoint the journal.
/// It is suitable for a read-only first-pass checker, not a full crash-recovery verdict.
pub fn check_volume<D: BlockDevice>(device: &mut D) -> Result<VolumeCheckReport> {
    if device.block_size() < SUPERBLOCK_SIZE as u32 {
        return Err(Error::InvalidInput(
            "device block is smaller than the superblock record",
        ));
    }
    let last = device
        .block_count()
        .checked_sub(1)
        .ok_or(Error::InvalidInput("empty device"))?;
    let primary = read_superblock(device, 0)?;
    let backup = read_superblock(device, last)?;
    // A locator update can leave different but valid hints in the two copies. Decode
    // both independently so the tolerated difference never masks a bad backup CRC.
    decode_superblock(&backup)?;
    if !superblocks_match(&primary, &backup) {
        return Err(Error::Corrupt("primary and backup superblocks disagree"));
    }
    let (block_size, total_blocks, _features, uuid) = decode_superblock(&primary)?;
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "superblock geometry does not match device geometry",
        ));
    }
    let version = u16::from_le_bytes([primary[8], primary[9]]);
    let layout = VolumeLayout::calculate_for_version(block_size, total_blocks, version)?;
    validate_bitmap(device, &layout)?;
    let (
        journal_generation,
        journal_checkpoint_sequence,
        pending_journal_records,
        recoverable_transactions,
    ) = check_journal_read_only(device, layout.journal)?;

    let bits_per_bitmap_block = u64::from(block_size) * 8;
    let first_bitmap = layout.data_start / bits_per_bitmap_block;
    let last_data_block = layout.backup_superblock - 1;
    let last_bitmap = last_data_block / bits_per_bitmap_block;
    let mut free_data_blocks = 0_u64;
    let mut bitmap = try_zeroed_block(
        block_size as usize,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for bitmap_index in first_bitmap..=last_bitmap {
        let bitmap_block = layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let base = bitmap_index
            .checked_mul(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let first = layout.data_start.max(base);
        let end = layout.backup_superblock.min(
            base.checked_add(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        let first_byte =
            usize::try_from((first - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
        let end_byte = usize::try_from(
            (end - base)
                .checked_add(7)
                .ok_or(Error::ArithmeticOverflow)?
                / 8,
        )
        .map_err(|_| Error::ArithmeticOverflow)?;
        for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
            let byte_index = first_byte
                .checked_add(byte_offset)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut mask = u8::MAX;
            if byte_index == first_byte {
                mask &= u8::MAX << ((first - base) % 8);
            }
            if byte_index + 1 == end_byte && (end - base) % 8 != 0 {
                mask &= (1_u8 << ((end - base) % 8)) - 1;
            }
            free_data_blocks = free_data_blocks
                .checked_add((!*byte & mask).count_ones() as u64)
                .ok_or(Error::ArithmeticOverflow)?;
        }
    }
    let persistent_inventory_present = check_inventory_descriptor_read_only(device, &layout)?;
    let data_blocks = layout.data_blocks();
    if free_data_blocks > data_blocks {
        return Err(Error::Corrupt("free data block count exceeds data region"));
    }
    Ok(VolumeCheckReport {
        block_size,
        total_blocks,
        uuid,
        bitmap_start: layout.bitmap_start,
        bitmap_blocks: layout.bitmap_blocks,
        data_start: layout.data_start,
        backup_superblock: layout.backup_superblock,
        data_blocks,
        allocated_data_blocks: data_blocks - free_data_blocks,
        free_data_blocks,
        journal_generation,
        journal_checkpoint_sequence,
        pending_journal_records,
        recoverable_transactions,
        persistent_inventory_present,
    })
}

/// Find and minimally validate the persistent namespace descriptor without opening the
/// writable volume or replaying its journal. Full namespace reconstruction is performed by
/// normal volume operations; this read-only pass at least catches duplicate or malformed
/// descriptors and references to free/out-of-range inventory blocks.
fn check_inventory_descriptor_read_only<D: BlockDevice>(
    device: &mut D,
    layout: &VolumeLayout,
) -> Result<bool> {
    const SCAN_BATCH_BLOCKS: u64 = 64;

    let block_size = usize::try_from(layout.block_size).map_err(|_| Error::ArithmeticOverflow)?;
    let bits_per_bitmap_block = u64::from(layout.block_size) * 8;
    let mut discovered: Option<(u64, PersistentInventoryDescriptor)> = None;
    // Cache only bitmap pages needed by descriptor references. A descriptor can span
    // many pages, but the cache remains bounded to the current candidate's references.
    let mut reference_bitmaps = std::collections::BTreeMap::<u64, Vec<u8>>::new();
    let mut batch_start = layout.data_start;
    // Reuse the bitmap page while scanning batches; only descriptor-reference pages
    // are copied into the bounded cache below.
    let mut scan_bitmap = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;

    while batch_start < layout.backup_superblock {
        let batch_end = layout
            .backup_superblock
            .min(batch_start.saturating_add(SCAN_BATCH_BLOCKS));
        let first_bitmap_index = batch_start / bits_per_bitmap_block;
        let last_bitmap_index = (batch_end - 1) / bits_per_bitmap_block;
        let mut allocated = std::collections::BTreeSet::new();
        for bitmap_index in first_bitmap_index..=last_bitmap_index {
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut scan_bitmap)?;
            let bitmap_base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = batch_start.max(bitmap_base);
            let end = batch_end.min(bitmap_base.saturating_add(bits_per_bitmap_block));
            let first_byte = usize::try_from((start - bitmap_base) / 8)
                .map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(
                (end - bitmap_base)
                    .checked_add(7)
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;
            for (byte_offset, value) in scan_bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
                let byte_base = bitmap_base
                    .checked_add(
                        u64::try_from(byte_index)
                            .map_err(|_| Error::ArithmeticOverflow)?
                            .checked_mul(8)
                            .ok_or(Error::ArithmeticOverflow)?,
                    )
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut bits = *value;
                let lower_bit = start.saturating_sub(byte_base).min(8);
                if lower_bit > 0 {
                    bits &= u8::MAX << lower_bit;
                }
                let upper_bit = end.saturating_sub(byte_base).min(8);
                if upper_bit < 8 {
                    bits &= (1_u8 << upper_bit) - 1;
                }
                while bits != 0 {
                    let bit = u64::from(bits.trailing_zeros());
                    let block = byte_base
                        .checked_add(bit)
                        .ok_or(Error::ArithmeticOverflow)?;
                    allocated.insert(block);
                    bits &= bits - 1;
                }
            }
        }

        // Read only allocated runs, not the entire data region. Free blocks cannot
        // contain a live descriptor, so reading them only adds avoidable device I/O.
        let mut allocated_blocks = allocated.into_iter().peekable();
        while let Some(run_start) = allocated_blocks.next() {
            let mut run_end = run_start.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            while allocated_blocks.peek().is_some_and(|next| *next == run_end) {
                allocated_blocks.next();
                run_end = run_end.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            }
            let run_count =
                usize::try_from(run_end - run_start).map_err(|_| Error::ArithmeticOverflow)?;
            let run_bytes = run_count
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let mut contents = try_zeroed_block(
                run_bytes,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            device.read_blocks(run_start, &mut contents)?;
            for (run_offset, block_contents) in contents.chunks_exact(block_size).enumerate() {
                if !block_contents.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
                    continue;
                }
                let block = run_start
                    .checked_add(u64::try_from(run_offset).map_err(|_| Error::ArithmeticOverflow)?)
                    .ok_or(Error::ArithmeticOverflow)?;
                let descriptor = PersistentInventoryDescriptor::decode(block_contents)?;
                if discovered.is_some() {
                    return Err(Error::Corrupt(
                        "multiple persistent inventory descriptors were discovered",
                    ));
                }
                let mut references = std::collections::BTreeSet::new();
                for inventory_block in descriptor
                    .metadata_blocks
                    .iter()
                    .chain(descriptor.directory_blocks.iter())
                {
                    if *inventory_block < layout.data_start
                        || *inventory_block >= layout.backup_superblock
                        || *inventory_block == block
                        || !references.insert(*inventory_block)
                    {
                        return Err(Error::Corrupt(
                            "persistent inventory descriptor has invalid block ownership",
                        ));
                    }
                    let bitmap_index = *inventory_block / bits_per_bitmap_block;
                    if !reference_bitmaps.contains_key(&bitmap_index) {
                        if reference_bitmaps.len() >= INVENTORY_BITMAP_CACHE_PAGES {
                            reference_bitmaps.pop_first();
                        }
                        let mut bitmap = try_zeroed_block(
                            block_size,
                            "allocation failed while preparing persistent I/O buffer",
                        )?;
                        let bitmap_block = layout
                            .bitmap_start
                            .checked_add(bitmap_index)
                            .ok_or(Error::ArithmeticOverflow)?;
                        device.read_blocks(bitmap_block, &mut bitmap)?;
                        reference_bitmaps.insert(bitmap_index, bitmap);
                    }
                    let local_bit = *inventory_block % bits_per_bitmap_block;
                    let byte_index =
                        usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
                    if reference_bitmaps[&bitmap_index][byte_index] & (1_u8 << (local_bit % 8)) == 0
                    {
                        return Err(Error::Corrupt(
                            "persistent inventory descriptor references a free block",
                        ));
                    }
                }
                discovered = Some((block, descriptor));
            }
        }
        batch_start = batch_end;
    }

    // A valid descriptor is only a locator. Decode the referenced streams and rebuild
    // the namespace here so check_volume catches corrupt metadata and directory graphs
    // without opening the writable volume or replaying the journal.
    let Some((_descriptor_block, descriptor)) = discovered else {
        return Ok(false);
    };
    let metadata_bytes = read_inventory_stream_read_only(
        device,
        layout,
        &descriptor.metadata_blocks,
        descriptor.metadata_record_count,
        METADATA_RECORD_SIZE,
    )?;
    let directory_bytes = read_inventory_stream_read_only(
        device,
        layout,
        &descriptor.directory_blocks,
        descriptor.directory_entry_count,
        DIRECTORY_ENTRY_SIZE,
    )?;
    let records = decode_metadata_records(&metadata_bytes)?;
    let entries = decode_directory_entries(&directory_bytes)?;
    let namespace = Namespace::from_records(descriptor.root_id, records, entries)?;
    let roots =
        namespace.validated_extent_root_inventory(layout.data_start, layout.backup_superblock)?;
    let claimed_count = descriptor
        .metadata_blocks
        .len()
        .checked_add(descriptor.directory_blocks.len())
        .ok_or(Error::ArithmeticOverflow)?;
    let mut claimed = Vec::new();
    claimed
        .try_reserve_exact(claimed_count)
        .map_err(|_| Error::InvalidInput("unable to allocate inventory ownership index"))?;
    claimed.extend(descriptor.metadata_blocks.iter().copied());
    claimed.extend(descriptor.directory_blocks.iter().copied());
    claimed.sort_unstable();
    if roots.iter().any(|root| claimed.binary_search(root).is_ok()) {
        return Err(Error::Corrupt(
            "extent-map root aliases a metadata or directory inventory block",
        ));
    }
    for root in &roots {
        let bitmap_index = *root / bits_per_bitmap_block;
        if !reference_bitmaps.contains_key(&bitmap_index) {
            if reference_bitmaps.len() >= INVENTORY_BITMAP_CACHE_PAGES {
                reference_bitmaps.pop_first();
            }
            let mut bitmap = try_zeroed_block(
                block_size,
                "allocation failed while preparing persistent I/O buffer",
            )?;
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            reference_bitmaps.insert(bitmap_index, bitmap);
        }
        let local_bit = *root % bits_per_bitmap_block;
        let byte_index = usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
        if reference_bitmaps[&bitmap_index][byte_index] & (1_u8 << (local_bit % 8)) == 0 {
            return Err(Error::Corrupt(
                "extent-map root is marked free in the allocation bitmap",
            ));
        }
    }

    // Reconcile physical ownership across inventory streams, extent-map pages, and file
    // data. Keep ranges compressed through overlap detection and reconcile allocation
    // ownership with byte masks in a single bitmap pass.
    let mut owned_ranges = Vec::<(u64, u64)>::new();
    for block in descriptor
        .metadata_blocks
        .iter()
        .chain(descriptor.directory_blocks.iter())
        .chain(std::iter::once(&_descriptor_block))
        .chain(roots.iter())
    {
        let end = block.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        owned_ranges.push((*block, end));
    }
    let mut extent_map_bytes = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for record in namespace.objects() {
        let Some(root) = record.extent_root_block else {
            continue;
        };
        device.read_blocks(root, &mut extent_map_bytes)?;
        let map = ExtentMap::decode_block(&extent_map_bytes)?;
        map.validate_file_size(record.size, layout.block_size)?;
        map.validate_physical_bounds(layout.data_start, layout.backup_superblock)?;
        for extent in map.extents() {
            let end = extent
                .physical_start
                .checked_add(extent.block_count)
                .ok_or(Error::ArithmeticOverflow)?;
            owned_ranges.push((extent.physical_start, end));
        }
    }
    owned_ranges.sort_unstable();
    let mut furthest_owned_end = 0_u64;
    for (start, end) in &owned_ranges {
        if *start < furthest_owned_end {
            return Err(Error::Corrupt(
                "persistent inventory, extent-map pages, or file data have overlapping physical ownership",
            ));
        }
        furthest_owned_end = furthest_owned_end.max(*end);
    }
    let mut owner_index = 0_usize;
    let first_bitmap = layout.data_start / bits_per_bitmap_block;
    let last_bitmap = (layout.backup_superblock - 1) / bits_per_bitmap_block;
    let mut bitmap = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for bitmap_index in first_bitmap..=last_bitmap {
        let bitmap_block = layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let bitmap_base = bitmap_index
            .checked_mul(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let scan_start = layout.data_start.max(bitmap_base);
        let scan_end = layout.backup_superblock.min(
            bitmap_base
                .checked_add(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        let first_byte = usize::try_from((scan_start - bitmap_base) / 8)
            .map_err(|_| Error::ArithmeticOverflow)?;
        let end_byte = usize::try_from(
            (scan_end - bitmap_base)
                .checked_add(7)
                .ok_or(Error::ArithmeticOverflow)?
                / 8,
        )
        .map_err(|_| Error::ArithmeticOverflow)?;
        for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
            let byte_index = first_byte
                .checked_add(byte_offset)
                .ok_or(Error::ArithmeticOverflow)?;
            let byte_base = bitmap_base
                .checked_add(
                    u64::try_from(byte_index)
                        .map_err(|_| Error::ArithmeticOverflow)?
                        .checked_mul(8)
                        .ok_or(Error::ArithmeticOverflow)?,
                )
                .ok_or(Error::ArithmeticOverflow)?;
            let byte_end = byte_base.checked_add(8).ok_or(Error::ArithmeticOverflow)?;
            let valid_start = scan_start.max(byte_base);
            let valid_end = scan_end.min(byte_end);
            let mut valid_mask = u8::MAX << (valid_start - byte_base);
            let upper_bit = valid_end - byte_base;
            if upper_bit < 8 {
                valid_mask &= (1_u8 << upper_bit) - 1;
            }

            while owner_index < owned_ranges.len() && owned_ranges[owner_index].1 <= byte_base {
                owner_index += 1;
            }
            let mut owner_mask = 0_u8;
            let mut candidate = owner_index;
            while candidate < owned_ranges.len() && owned_ranges[candidate].0 < byte_end {
                let (range_start, range_end) = owned_ranges[candidate];
                let overlap_start = range_start.max(byte_base);
                let overlap_end = range_end.min(byte_end);
                if overlap_start < overlap_end {
                    let low = u32::try_from(overlap_start - byte_base)
                        .map_err(|_| Error::ArithmeticOverflow)?;
                    let high = u32::try_from(overlap_end - byte_base)
                        .map_err(|_| Error::ArithmeticOverflow)?;
                    let lower_mask = u8::MAX << low;
                    let upper_mask = if high == 8 {
                        u8::MAX
                    } else {
                        (1_u8 << high) - 1
                    };
                    owner_mask |= lower_mask & upper_mask;
                }
                candidate += 1;
            }
            owner_mask &= valid_mask;
            if *byte & owner_mask != owner_mask {
                return Err(Error::Corrupt(
                    "persistent inventory or file extent references a free block",
                ));
            }
            if *byte & valid_mask & !owner_mask != 0 {
                return Err(Error::Corrupt(
                    "allocation bitmap contains an allocated data block with no persistent owner",
                ));
            }
        }
    }
    Ok(true)
}

/// Read an inventory stream during a non-mutating volume check. The descriptor's
/// block order defines stream order; contiguous runs are coalesced into bounded I/O.
fn read_inventory_stream_read_only<D: BlockDevice>(
    device: &mut D,
    layout: &VolumeLayout,
    blocks: &[u64],
    record_count: usize,
    record_size: usize,
) -> Result<Vec<u8>> {
    let block_size = usize::try_from(layout.block_size).map_err(|_| Error::ArithmeticOverflow)?;
    let required_bytes = record_count
        .checked_mul(record_size)
        .ok_or(Error::ArithmeticOverflow)?;
    let required_blocks = required_bytes
        .checked_add(block_size - 1)
        .ok_or(Error::ArithmeticOverflow)?
        / block_size;
    if blocks.len() != required_blocks {
        return Err(Error::Corrupt(
            "persistent inventory block count does not match its record count",
        ));
    }
    let mut seen = std::collections::BTreeSet::new();
    for block in blocks {
        if *block < layout.data_start || *block >= layout.backup_superblock || !seen.insert(*block)
        {
            return Err(Error::Corrupt(
                "persistent inventory stream has invalid block ownership",
            ));
        }
    }
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(
            required_blocks
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?,
        )
        .map_err(|_| {
            Error::InvalidInput("allocation failed while staging persistent namespace data")
        })?;
    let mut cursor = 0;
    while cursor < blocks.len() {
        let mut end = cursor + 1;
        while end < blocks.len()
            && end - cursor < INVENTORY_DISCOVERY_BATCH_BLOCKS
            && blocks[end - 1].checked_add(1) == Some(blocks[end])
        {
            end += 1;
        }
        let run_bytes = (end - cursor)
            .checked_mul(block_size)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut batch = try_zeroed_block(
            run_bytes,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        device.read_blocks(blocks[cursor], &mut batch)?;
        bytes.extend_from_slice(&batch);
        cursor = end;
    }
    bytes.truncate(required_bytes);
    Ok(bytes)
}

/// Validate the persisted journal without replaying records or writing checkpoints.
///
/// Mirrors the journal engine's checkpoint selection and ring-order checks. A malformed
/// terminal append may be a torn write and is tolerated only when no later valid record
/// follows it; committed redo payloads are decoded as journal records, but are not applied.
fn check_journal_read_only<D: BlockDevice>(
    device: &mut D,
    region: JournalRegion,
) -> Result<(u64, u64, u64, u64)> {
    fn read_copy<D: BlockDevice>(
        device: &mut D,
        block_number: u64,
        region: JournalRegion,
    ) -> Result<JournalCheckpoint> {
        let block_size =
            usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
        if block_size < JOURNAL_CHECKPOINT_SIZE {
            return Err(Error::InvalidInput(
                "device block is too small for journal checkpoints",
            ));
        }
        let mut block = try_zeroed_block(
            block_size,
            "allocation failed while preparing persistent I/O buffer",
        )?;
        device.read_blocks(block_number, &mut block)?;
        if block[JOURNAL_CHECKPOINT_SIZE..]
            .iter()
            .any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("checkpoint block padding must be zero"));
        }
        let checkpoint = JournalCheckpoint::decode(&block[..JOURNAL_CHECKPOINT_SIZE])?;
        // Match the recovery engine: validate each copy before choosing by generation,
        // so a checksum-valid but unusable newer copy cannot hide a valid older copy.
        if checkpoint.head_block >= region.ring_blocks
            || checkpoint.tail_block >= region.ring_blocks
            || checkpoint.head_block != 0
            || checkpoint.tail_block != 0
        {
            return Err(Error::Corrupt(
                "unsupported journal checkpoint ring position",
            ));
        }
        if checkpoint.checkpoint_sequence.checked_add(1) != Some(checkpoint.next_sequence) {
            return Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree",
            ));
        }
        Ok(checkpoint)
    }

    let first = read_copy(device, region.checkpoint_a, region);
    let second = read_copy(device, region.checkpoint_b, region);
    let (checkpoint, _) = match (first, second) {
        (Ok(a), Ok(b)) if a.generation == b.generation && a != b => {
            return Err(Error::Corrupt(
                "checkpoint copies conflict at the same generation",
            ));
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation && a.generation.checked_sub(b.generation) != Some(1) =>
        {
            return Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation && b.generation.checked_sub(a.generation) != Some(1) =>
        {
            return Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ));
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(a.generation == 2
                    && b.generation == 1
                    && a.checkpoint_sequence == 0
                    && a.next_transaction_id == 1
                    && b.next_transaction_id == 1) =>
        {
            return Err(Error::Corrupt(
                "checkpoint generation advanced without a durable journal record",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(b.generation == 2
                    && a.generation == 1
                    && a.checkpoint_sequence == 0
                    && a.next_transaction_id == 1
                    && b.next_transaction_id == 1) =>
        {
            return Err(Error::Corrupt(
                "checkpoint generation advanced without a durable journal record",
            ));
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && a.checkpoint_sequence > b.checkpoint_sequence
                && a.next_transaction_id == b.next_transaction_id =>
        {
            return Err(Error::Corrupt(
                "journal sequence advanced without a transaction ID",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && b.checkpoint_sequence > a.checkpoint_sequence
                && b.next_transaction_id == a.next_transaction_id =>
        {
            return Err(Error::Corrupt(
                "journal sequence advanced without a transaction ID",
            ));
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && (a.checkpoint_sequence < b.checkpoint_sequence
                    || a.next_transaction_id < b.next_transaction_id) =>
        {
            return Err(Error::Corrupt(
                "newer journal checkpoint regresses durable counters",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && (b.checkpoint_sequence < a.checkpoint_sequence
                    || b.next_transaction_id < a.next_transaction_id) =>
        {
            return Err(Error::Corrupt(
                "newer journal checkpoint regresses durable counters",
            ));
        }
        (Ok(a), Ok(b)) if a.generation >= b.generation => (a, 0),
        (Ok(_), Ok(b)) => (b, 1),
        (Ok(a), Err(_)) => (a, 0),
        (Err(_), Ok(b)) => (b, 1),
        (Err(_), Err(_)) => {
            return Err(Error::Corrupt("both journal checkpoint copies are invalid"));
        }
    };

    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    let mut newer = Vec::<(u64, JournalRecord)>::new();
    // Recovery diagnostics need only the earliest malformed slot, not a vector of
    // every bad slot. Reuse one block buffer and keep scan memory bounded.
    let mut first_malformed_slot = None;
    let mut block = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for slot in 0..region.ring_blocks {
        let block_number = region
            .ring_start
            .checked_add(slot)
            .ok_or(Error::ArithmeticOverflow)?;
        device.read_blocks(block_number, &mut block)?;
        if block.iter().all(|byte| *byte == 0) {
            continue;
        }
        match JournalRecord::decode_block(&block) {
            Ok(record) if record.sequence <= checkpoint.checkpoint_sequence => {}
            Ok(record) => {
                if record.transaction_id < checkpoint.next_transaction_id {
                    return Err(Error::Corrupt(
                        "post-checkpoint journal record reuses a checkpointed transaction ID",
                    ));
                }
                newer.push((slot, record));
            }
            Err(_) => {
                first_malformed_slot =
                    Some(first_malformed_slot.map_or(slot, |first: u64| first.min(slot)));
            }
        }
    }
    newer.sort_unstable_by_key(|(_, record)| record.sequence);
    // A checkpoint records the exact next transaction ID. Because the journal
    // permits only one writer, BEGIN records must consume IDs without gaps.
    let mut expected_transaction_id = checkpoint.next_transaction_id;
    for (_, record) in &newer {
        if record.kind == JournalRecordKind::Begin {
            if record.transaction_id != expected_transaction_id {
                return Err(Error::Corrupt(
                    "journal transaction IDs are not contiguous after checkpoint",
                ));
            }
            expected_transaction_id = expected_transaction_id
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow)?;
        }
    }
    if let Some((last_slot, _)) = newer.iter().max_by_key(|(slot, _)| *slot) {
        if first_malformed_slot.is_some_and(|slot| slot < *last_slot) {
            return Err(Error::Corrupt(
                "malformed journal block appears before a later valid record",
            ));
        }
    }
    for (index, (slot, _)) in newer.iter().enumerate() {
        if *slot != index as u64 {
            return Err(Error::Corrupt(
                "post-checkpoint journal records are not contiguous from ring start",
            ));
        }
    }
    let mut records = Vec::new();
    records
        .try_reserve_exact(newer.len())
        .map_err(|_| Error::InvalidInput("unable to allocate journal recovery records"))?;
    records.extend(newer.into_iter().map(|(_, record)| record));
    let committed = plan_recovery(&records, checkpoint.checkpoint_sequence)?;
    Ok((
        checkpoint.generation,
        checkpoint.checkpoint_sequence,
        records.len() as u64,
        committed.len() as u64,
    ))
}

/// Format a device as canonical. Destructive; call only on a disposable image or explicitly
/// selected device. Physical-device formatting is not exposed by the host utility.
pub fn format_volume<D: BlockDevice>(
    device: &mut D,
    uuid: [u8; 16],
    features: u64,
) -> Result<OpenedVolume> {
    let layout = VolumeLayout::calculate(device.block_size(), device.block_count())?;
    let encoded = encode_superblock(layout.block_size, layout.total_blocks, features, uuid)?;
    write_superblock(device, layout.backup_superblock, &encoded)?;
    let block_size = layout.block_size as usize;
    let bits_per_block = u64::from(layout.block_size) * 8;
    let mut bitmap = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for index in 0..layout.bitmap_blocks {
        // Each bitmap page is independent; clear the reused buffer before rebuilding it.
        bitmap.fill(0);
        let first = index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        for block in first..first.saturating_add(bits_per_block) {
            if block >= layout.total_blocks
                || block < layout.data_start
                || block == layout.backup_superblock
            {
                let local = block - first;
                bitmap[(local / 8) as usize] |= 1 << (local % 8);
            }
        }
        let bitmap_block = layout
            .bitmap_start
            .checked_add(index)
            .ok_or(Error::ArithmeticOverflow)?;
        device.write_blocks(bitmap_block, &bitmap)?;
    }
    device.flush()?;
    format_journal(device, layout.journal)?;
    device.flush()?;
    write_superblock(device, 0, &encoded)?;
    device.flush()?;
    open_volume(device)
}

/// Validate a volume and recover committed journal records before returning writable state.
pub fn open_volume<D: BlockDevice>(device: &mut D) -> Result<OpenedVolume> {
    if device.block_size() < SUPERBLOCK_SIZE as u32 {
        return Err(Error::InvalidInput(
            "device block is smaller than the superblock record",
        ));
    }
    let last = device
        .block_count()
        .checked_sub(1)
        .ok_or(Error::InvalidInput("empty device"))?;
    let primary = read_superblock(device, 0)?;
    let backup = read_superblock(device, last)?;
    // Validate each copy's checksum and encoding independently before comparing them.
    // The locator hint may legitimately differ after an interrupted backup-first update,
    // but that exception must never allow a damaged backup checksum to go unnoticed.
    decode_superblock(&backup)?;
    if !superblocks_match(&primary, &backup) {
        return Err(Error::Corrupt("primary and backup superblocks disagree"));
    }
    let (block_size, total_blocks, features, uuid) = decode_superblock(&primary)?;
    let inventory_descriptor_hint =
        decode_inventory_hint(&primary).or(decode_inventory_hint(&backup));
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "superblock geometry does not match device geometry",
        ));
    }
    let version = u16::from_le_bytes([primary[8], primary[9]]);
    let layout = VolumeLayout::calculate_for_version(block_size, total_blocks, version)?;
    validate_bitmap(device, &layout)?;
    let journal = crate::journal_engine::open_journal_protected(
        device,
        layout.journal,
        &[0, layout.backup_superblock],
    )?;
    // Recovery may replay a bitmap home-block image. Validate reserved bits again
    // after replay before exposing writable volume state.
    validate_bitmap(device, &layout)?;
    Ok(OpenedVolume {
        volume: Volume {
            block_size,
            total_blocks,
            features,
            uuid,
            layout,
        },
        journal,
        format_version: version,
        inventory_descriptor_cache: None,
        inventory_descriptor_hint,
    })
}

fn encode_superblock(
    block_size: u32,
    total_blocks: u64,
    features: u64,
    uuid: [u8; 16],
) -> Result<[u8; SUPERBLOCK_SIZE]> {
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() || total_blocks < 2 {
        return Err(Error::InvalidInput("invalid superblock geometry"));
    }
    u64::from(block_size)
        .checked_mul(total_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    let mut bytes = [0_u8; SUPERBLOCK_SIZE];
    bytes[..8].copy_from_slice(MAGIC);
    bytes[8..10].copy_from_slice(&VERSION.to_le_bytes());
    bytes[10..12].copy_from_slice(&HEADER_SIZE.to_le_bytes());
    bytes[12..16].copy_from_slice(&block_size.to_le_bytes());
    bytes[16..24].copy_from_slice(&total_blocks.to_le_bytes());
    bytes[24..32].copy_from_slice(&features.to_le_bytes());
    bytes[32..48].copy_from_slice(&uuid);
    let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
    bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].copy_from_slice(&checksum.to_le_bytes());
    Ok(bytes)
}

fn decode_superblock(bytes: &[u8; SUPERBLOCK_SIZE]) -> Result<(u32, u64, u64, [u8; 16])> {
    if &bytes[..8] != MAGIC {
        return Err(Error::Corrupt("bad superblock magic"));
    }
    let version = u16::from_le_bytes([bytes[8], bytes[9]]);
    if version != VERSION && version != PREVIOUS_VERSION && version != LEGACY_VERSION {
        return Err(Error::UnsupportedVersion(version));
    }
    if u16::from_le_bytes([bytes[10], bytes[11]]) != HEADER_SIZE {
        return Err(Error::Corrupt("unexpected superblock header size"));
    }
    let stored = u32::from_le_bytes(
        bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
            .try_into()
            .unwrap(),
    );
    if stored != crc32c(&bytes[..CHECKSUM_OFFSET]) {
        return Err(Error::Corrupt("superblock CRC32C mismatch"));
    }
    let reserved_start = if version >= PREVIOUS_VERSION {
        INVENTORY_HINT_OFFSET + 8
    } else {
        48
    };
    if bytes[reserved_start..CHECKSUM_OFFSET]
        .iter()
        .any(|b| *b != 0)
        || bytes[4092..].iter().any(|b| *b != 0)
    {
        return Err(Error::Corrupt("reserved superblock bytes must be zero"));
    }
    let block_size = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    let total_blocks = u64::from_le_bytes(bytes[16..24].try_into().unwrap());
    let features = u64::from_le_bytes(bytes[24..32].try_into().unwrap());
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() || total_blocks < 2 {
        return Err(Error::Corrupt("invalid superblock geometry"));
    }
    u64::from(block_size)
        .checked_mul(total_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    let mut uuid = [0_u8; 16];
    uuid.copy_from_slice(&bytes[32..48]);
    Ok((block_size, total_blocks, features, uuid))
}

fn decode_inventory_hint(bytes: &[u8; SUPERBLOCK_SIZE]) -> Option<u64> {
    if u16::from_le_bytes([bytes[8], bytes[9]]) < PREVIOUS_VERSION {
        return None;
    }
    let hint = u64::from_le_bytes(
        bytes[INVENTORY_HINT_OFFSET..INVENTORY_HINT_OFFSET + 8]
            .try_into()
            .ok()?,
    );
    (hint != 0).then_some(hint)
}

fn superblocks_match(primary: &[u8; SUPERBLOCK_SIZE], backup: &[u8; SUPERBLOCK_SIZE]) -> bool {
    if primary == backup {
        return true;
    }
    let primary_version = u16::from_le_bytes([primary[8], primary[9]]);
    let backup_version = u16::from_le_bytes([backup[8], backup[9]]);
    if primary_version != backup_version
        || (primary_version != VERSION && primary_version != PREVIOUS_VERSION)
    {
        return false;
    }
    primary[..INVENTORY_HINT_OFFSET] == backup[..INVENTORY_HINT_OFFSET]
        && primary[INVENTORY_HINT_OFFSET + 8..CHECKSUM_OFFSET]
            == backup[INVENTORY_HINT_OFFSET + 8..CHECKSUM_OFFSET]
        && primary[4092..] == backup[4092..]
}

fn persist_inventory_hint<D: BlockDevice>(device: &mut D, hint: u64) -> Result<()> {
    let last = device
        .block_count()
        .checked_sub(1)
        .ok_or(Error::InvalidInput("empty device"))?;
    let primary = read_superblock(device, 0)?;
    // Do not turn a damaged superblock into a seemingly valid one by recomputing its
    // checksum during a locator update. Both copies must be independently valid and
    // agree on every field except the locator/checksum pair before either is modified.
    decode_superblock(&primary)?;
    let version = u16::from_le_bytes([primary[8], primary[9]]);
    // Legacy v2 volumes retain scan-based discovery and are never modified to store a hint.
    if version == LEGACY_VERSION {
        return Ok(());
    }
    if version != VERSION && version != PREVIOUS_VERSION {
        return Err(Error::UnsupportedVersion(version));
    }
    let backup = read_superblock(device, last)?;
    decode_superblock(&backup)?;
    if !superblocks_match(&primary, &backup) {
        return Err(Error::Corrupt(
            "primary and backup superblocks disagree before locator update",
        ));
    }
    for block in [last, 0] {
        let mut encoded = read_superblock(device, block)?;
        // The previous hint may differ after an interrupted backup-first update, but
        // the independently checksummed stable superblock fields must still match.
        decode_superblock(&encoded)?;
        if !superblocks_match(&primary, &encoded) {
            return Err(Error::Corrupt("superblock changed during locator update"));
        }
        encoded[INVENTORY_HINT_OFFSET..INVENTORY_HINT_OFFSET + 8]
            .copy_from_slice(&hint.to_le_bytes());
        let checksum = crc32c(&encoded[..CHECKSUM_OFFSET]);
        encoded[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].copy_from_slice(&checksum.to_le_bytes());
        write_superblock(device, block, &encoded)?;
        device.flush()?;
    }
    Ok(())
}

fn read_superblock<D: BlockDevice>(device: &mut D, block: u64) -> Result<[u8; SUPERBLOCK_SIZE]> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    if block_size < SUPERBLOCK_SIZE {
        return Err(Error::InvalidInput(
            "device block is smaller than the superblock",
        ));
    }
    let mut buffer = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    device.read_blocks(block, &mut buffer)?;
    let mut record = [0; SUPERBLOCK_SIZE];
    record.copy_from_slice(&buffer[..SUPERBLOCK_SIZE]);
    Ok(record)
}

fn write_superblock<D: BlockDevice>(
    device: &mut D,
    block: u64,
    encoded: &[u8; SUPERBLOCK_SIZE],
) -> Result<()> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    if block_size < SUPERBLOCK_SIZE {
        return Err(Error::InvalidInput(
            "device block is smaller than the superblock",
        ));
    }
    let mut buffer = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    buffer[..SUPERBLOCK_SIZE].copy_from_slice(encoded);
    device.write_blocks(block, &buffer)
}

fn validate_bitmap<D: BlockDevice>(device: &mut D, layout: &VolumeLayout) -> Result<()> {
    let block_size = layout.block_size as usize;
    let bits_per_block = u64::from(layout.block_size) * 8;
    let mut bitmap = try_zeroed_block(
        block_size,
        "allocation failed while preparing persistent I/O buffer",
    )?;
    for index in 0..layout.bitmap_blocks {
        let first = index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let end = first
            .checked_add(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let bitmap_block = layout
            .bitmap_start
            .checked_add(index)
            .ok_or(Error::ArithmeticOverflow)?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in first..end {
            if layout.is_reserved(block) || block >= layout.total_blocks {
                let local = block - first;
                if bitmap[(local / 8) as usize] & (1 << (local % 8)) == 0 {
                    return Err(Error::Corrupt(
                        "allocation bitmap marks a reserved or padding block free",
                    ));
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        check_volume, crc32c, decode_inventory_hint, decode_superblock, encode_superblock,
        format_volume, open_volume, persist_inventory_hint, read_superblock, VolumeLayout,
        CHECKSUM_OFFSET, HEADER_SIZE, INVENTORY_HINT_OFFSET, MAGIC, VERSION,
    };
    use crate::{Extent, ExtentMap, JournalCheckpoint};
    use openfs_block::{BlockDevice, ImageDevice, RawDevice};
    use openfs_core::Error;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn image_path(label: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("openfs-{label}-{}-{nonce}.img", std::process::id()))
    }

    struct FailNextFlush<D> {
        inner: D,
        fail_next_flush: bool,
    }

    impl<D: BlockDevice> BlockDevice for FailNextFlush<D> {
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
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            if self.fail_next_flush {
                self.fail_next_flush = false;
                return Err(Error::Io(std::io::Error::other(
                    "injected persistent-inventory publication flush failure",
                )));
            }
            self.inner.flush()
        }
    }

    struct FailNextWrite<D> {
        inner: D,
        fail_next_write: bool,
    }

    impl<D: BlockDevice> BlockDevice for FailNextWrite<D> {
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
            if self.fail_next_write {
                self.fail_next_write = false;
                return Err(Error::Io(std::io::Error::other(
                    "injected persistent-inventory descriptor write failure",
                )));
            }
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    /// Persist a descriptor-write prefix, then report an I/O error to model a torn write.
    struct TornNextWrite<D> {
        inner: D,
        armed: bool,
        prefix_len: usize,
    }

    impl<D: BlockDevice> BlockDevice for TornNextWrite<D> {
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
            if self.armed {
                self.armed = false;
                // Preserve the bytes beyond the persisted prefix: a torn write changes
                // only the prefix that reached storage, not the untouched tail.
                let mut torn = vec![0_u8; input.len()];
                self.inner.read_blocks(first_block, &mut torn)?;
                let copied = self.prefix_len.min(input.len());
                torn[..copied].copy_from_slice(&input[..copied]);
                self.inner.write_blocks(first_block, &torn)?;
                return Err(Error::Io(std::io::Error::other(
                    "injected torn persistent-inventory descriptor write",
                )));
            }
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    struct FailNthFlush<D> {
        inner: D,
        armed: bool,
        flushes_before_failure: usize,
    }

    impl<D: BlockDevice> BlockDevice for FailNthFlush<D> {
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
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            if self.armed {
                if self.flushes_before_failure == 0 {
                    self.armed = false;
                    return Err(Error::Io(std::io::Error::other(
                        "injected inventory journal commit flush failure",
                    )));
                }
                self.flushes_before_failure -= 1;
            }
            self.inner.flush()
        }
    }

    #[test]
    fn discovery_fails_closed_on_corrupt_allocated_descriptor() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("corrupt-allocated-descriptor");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let descriptor_block = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut metadata_bytes = vec![0_u8; 4096];
            metadata_bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
            device
                .write_blocks(metadata_block, &metadata_bytes)
                .unwrap();

            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![metadata_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let mut encoded = descriptor.encode(4096).unwrap();
            // Keep the self-identifying magic intact but invalidate the descriptor CRC.
            encoded[48] ^= 0x40;
            device.write_blocks(descriptor_block, &encoded).unwrap();

            assert!(
                matches!(
                    volume.discover_persistent_inventory(&mut device),
                    Err(Error::Corrupt("inventory descriptor CRC32C mismatch"))
                ),
                "a corrupt allocated descriptor must not be silently ignored"
            );
            assert!(volume
                .is_data_block_allocated(&mut device, descriptor_block)
                .unwrap());
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovery_fails_closed_for_corrupt_descriptors_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor, INVENTORY_DESCRIPTOR_MAGIC};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for corruption in ["checksum", "reserved", "padding", "version"] {
                let path = image_path(&format!("descriptor-corruption-{corruption}-{block_size}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 64).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let descriptor_block = volume.allocate_block(&mut device).unwrap();
                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![],
                        metadata_record_count: 0,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    let mut encoded = descriptor.encode(block_size as usize).unwrap();
                    match corruption {
                        "checksum" => encoded[48] ^= 0x40,
                        "reserved" => encoded[36] = 1,
                        "padding" => {
                            let last = encoded.len() - 1;
                            encoded[last] = 1;
                            encoded[40..44].fill(0);
                            let checksum = crate::crc32c(&encoded);
                            encoded[40..44].copy_from_slice(&checksum.to_le_bytes());
                        }
                        "version" => encoded[8..10].copy_from_slice(&2_u16.to_le_bytes()),
                        _ => unreachable!("test cases are fixed"),
                    }
                    assert!(encoded.starts_with(INVENTORY_DESCRIPTOR_MAGIC));
                    device.write_blocks(descriptor_block, &encoded).unwrap();
                    let free_before = volume.free_data_block_count(&mut device).unwrap();

                    let result = volume.discover_persistent_inventory(&mut device);
                    match corruption {
                        "checksum" => assert!(matches!(
                            result,
                            Err(Error::Corrupt("inventory descriptor CRC32C mismatch"))
                        )),
                        "reserved" => assert!(matches!(
                            result,
                            Err(Error::Corrupt(
                                "inventory descriptor reserved bytes must be zero"
                            ))
                        )),
                        "padding" => assert!(matches!(
                            result,
                            Err(Error::Corrupt("inventory descriptor padding must be zero"))
                        )),
                        "version" => assert!(matches!(result, Err(Error::UnsupportedVersion(2)))),
                        _ => unreachable!("test cases are fixed"),
                    }
                    assert_eq!(
                        volume.free_data_block_count(&mut device).unwrap(),
                        free_before,
                        "discovery must not mutate allocation state"
                    );
                    assert!(volume
                        .is_data_block_allocated(&mut device, descriptor_block)
                        .unwrap());
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn creates_empty_persistent_file_and_finds_it_after_reopen() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("persistent-empty-file-create");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let root = ObjectId(1);
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let root_record = MetadataRecord {
                id: root,
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut bytes = vec![0_u8; 4096];
            bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root_record.encode().unwrap());
            device.write_blocks(metadata_block, &bytes).unwrap();
            volume
                .publish_persistent_inventory(
                    &mut device,
                    &PersistentInventoryDescriptor {
                        root_id: root,
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    },
                )
                .unwrap();

            assert_eq!(
                volume
                    .create_persistent_file(&mut device, root, ObjectId(2), "hello.txt")
                    .unwrap(),
                ObjectId(2)
            );
            // A duplicate name must fail before publishing a replacement inventory.
            assert!(volume
                .create_persistent_file(&mut device, root, ObjectId(3), "hello.txt")
                .is_err());
            // Rename must publish the directory entry and parent metadata as one
            // inventory replacement, and the old path must stop resolving.
            volume
                .rename_persistent_object(&mut device, ObjectId(2), root, "renamed.txt")
                .unwrap();
            let (_, namespace) = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert!(namespace.resolve_path("/hello.txt").is_err());
            assert_eq!(namespace.resolve_path("/renamed.txt").unwrap(), ObjectId(2));
            assert_eq!(namespace.object(ObjectId(2)).unwrap().generation, 2);

            // Appending to a newly created empty file must allocate its first
            // extent and publish the data through the normal copy-on-write path.
            volume
                .write_persistent_path_range(&mut device, "/renamed.txt", 0, b"x")
                .unwrap();
            let (_, namespace) = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(namespace.object(ObjectId(2)).unwrap().size, 1);
            assert_eq!(
                volume
                    .read_persistent_path_range(&mut device, "/renamed.txt", 0, 1)
                    .unwrap(),
                b"x"
            );

            drop(volume);
            let reopened = open_volume(&mut device).unwrap();
            let (_, namespace) = reopened
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(namespace.resolve_path("/renamed.txt").unwrap(), ObjectId(2));
            assert_eq!(namespace.object(ObjectId(2)).unwrap().size, 1);
            assert_eq!(
                reopened
                    .read_persistent_path_range(&mut device, "/renamed.txt", 0, 1)
                    .unwrap(),
                b"x"
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn writes_initial_persistent_file_contents_and_reads_after_reopen() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("persistent-file-initial-write");
        let expected = b"OpenFS persistent data";
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let root = ObjectId(1);
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let root_record = MetadataRecord {
                id: root,
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut bytes = vec![0_u8; 4096];
            bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root_record.encode().unwrap());
            device.write_blocks(metadata_block, &bytes).unwrap();
            volume
                .publish_persistent_inventory(
                    &mut device,
                    &PersistentInventoryDescriptor {
                        root_id: root,
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    },
                )
                .unwrap();

            volume
                .create_persistent_file(&mut device, root, ObjectId(2), "hello.txt")
                .unwrap();
            volume
                .write_persistent_file_contents(&mut device, "/hello.txt", expected)
                .unwrap();
            assert_eq!(
                volume
                    .read_persistent_path_range(&mut device, "/hello.txt", 0, expected.len())
                    .unwrap(),
                expected
            );
            let (_, namespace) = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(
                namespace.object(ObjectId(2)).unwrap().size,
                expected.len() as u64
            );
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            let reopened = open_volume(&mut device).unwrap();
            assert_eq!(
                reopened
                    .read_persistent_path_range(&mut device, "/hello.txt", 0, expected.len())
                    .unwrap(),
                expected
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn replaces_persistent_namespace_inventory_and_survives_reopen() {
        use crate::{
            DirectoryEntry, MetadataRecord, Namespace, ObjectId, ObjectKind,
            PersistentInventoryDescriptor,
        };

        let path = image_path("namespace-inventory-copy-on-write");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let root = ObjectId(1);
            let initial_metadata_block = volume.allocate_block(&mut device).unwrap();
            let root_record = MetadataRecord {
                id: root,
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut initial_bytes = vec![0_u8; 4096];
            initial_bytes[..crate::METADATA_RECORD_SIZE]
                .copy_from_slice(&root_record.encode().unwrap());
            device
                .write_blocks(initial_metadata_block, &initial_bytes)
                .unwrap();
            volume
                .publish_persistent_inventory(
                    &mut device,
                    &PersistentInventoryDescriptor {
                        root_id: root,
                        metadata_blocks: vec![initial_metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    },
                )
                .unwrap();

            let mut namespace = Namespace::new(root).unwrap();
            namespace
                .insert(
                    DirectoryEntry {
                        parent_id: 1,
                        object_id: 2,
                        kind: ObjectKind::File,
                        name: "hello.txt".to_owned(),
                    },
                    MetadataRecord {
                        id: ObjectId(2),
                        parent_id: 1,
                        kind: ObjectKind::File,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    },
                )
                .unwrap();

            volume
                .replace_persistent_namespace_inventory(&mut device, &namespace)
                .unwrap();
            let (_, reopened_namespace) = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(
                reopened_namespace.resolve_path("/hello.txt").unwrap(),
                ObjectId(2)
            );
            assert_eq!(reopened_namespace.object_count(), 2);
            assert_eq!(reopened_namespace.entry_count(), 1);

            drop(volume);
            let reopened = open_volume(&mut device).unwrap();
            let (_, persisted) = reopened
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(persisted.resolve_path("/hello.txt").unwrap(), ObjectId(2));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovers_unique_persistent_inventory_descriptor() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("inventory-descriptor-discovery");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut metadata_bytes = vec![0; 4096];
            metadata_bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
            device
                .write_blocks(metadata_block, &metadata_bytes)
                .unwrap();

            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![metadata_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let descriptor_block = volume
                .publish_persistent_inventory(&mut device, &descriptor)
                .unwrap();
            assert_eq!(
                volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap(),
                descriptor_block,
                "publishing the same inventory twice must be idempotent"
            );

            let found = volume
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(found.0, descriptor_block);
            assert_eq!(found.1, descriptor);

            let reconstructed = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(reconstructed.0, descriptor_block);
            assert_eq!(reconstructed.1.root_id(), ObjectId(1));
            assert_eq!(reconstructed.1.objects().count(), 1);

            // The descriptor and namespace must remain discoverable after reopening the image.
            drop(volume);
            let reopened = open_volume(&mut device).unwrap();
            let reopened_descriptor = reopened
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(reopened_descriptor.0, descriptor_block);
            assert_eq!(reopened_descriptor.1, descriptor);
            let reopened_namespace = reopened
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(reopened_namespace.0, descriptor_block);
            assert_eq!(reopened_namespace.1.root_id(), ObjectId(1));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovers_descriptor_across_batched_allocated_block_reads() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("inventory-descriptor-batch-boundary");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let allocated = volume.allocate_blocks(&mut device, 66).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut metadata_bytes = vec![0_u8; 4096];
            metadata_bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
            device.write_blocks(allocated[0], &metadata_bytes).unwrap();

            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![allocated[0]],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let encoded = descriptor.encode(4096).unwrap();
            let descriptor_block = allocated[65];
            device.write_blocks(descriptor_block, &encoded).unwrap();

            let found = volume
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(found.0, descriptor_block);
            assert_eq!(found.1, descriptor);

            let namespace = volume
                .reconstruct_discovered_namespace(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(namespace.0, descriptor_block);
            assert_eq!(namespace.1.root_id(), ObjectId(1));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn discovers_late_descriptor_among_fragmented_allocated_blocks() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        let path = image_path("inventory-descriptor-fragmented-discovery");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let allocated = volume.allocate_blocks(&mut device, 129).unwrap();

            // Leave 65 allocated candidates separated by free blocks. The descriptor
            // sits at the last candidate, exercising sparse bitmap-bit enumeration.
            for index in (1..128).step_by(2) {
                volume.free_block(&mut device, allocated[index]).unwrap();
            }
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![],
                metadata_record_count: 0,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let descriptor_block = allocated[128];
            device
                .write_blocks(descriptor_block, &descriptor.encode(4096).unwrap())
                .unwrap();

            let found = volume
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(found.0, descriptor_block);
            assert_eq!(found.1, descriptor);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn locator_does_not_trust_duplicate_inventory_ownership() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        let path = image_path("locator-duplicate-inventory-ownership");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let second_metadata_block = volume.allocate_block(&mut device).unwrap();
            let descriptor_block = volume.allocate_block(&mut device).unwrap();
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![metadata_block, second_metadata_block],
                metadata_record_count: 4096 / crate::METADATA_RECORD_SIZE + 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let mut encoded = descriptor.encode(4096).unwrap();
            // Produce a checksum-valid but ownership-invalid record: the encoder itself
            // correctly refuses duplicate physical blocks, so corrupt the second reference
            // after encoding and recompute the descriptor checksum.
            encoded[56..64].copy_from_slice(&metadata_block.to_le_bytes());
            encoded[40..44].fill(0);
            let checksum = crate::crc32c(&encoded);
            encoded[40..44].copy_from_slice(&checksum.to_le_bytes());
            device.write_blocks(descriptor_block, &encoded).unwrap();
            device.flush().unwrap();
            crate::volume::persist_inventory_hint(&mut device, descriptor_block).unwrap();

            assert!(matches!(
                volume.discover_persistent_inventory(&mut device),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn locator_and_fallback_scan_reject_invalid_populated_namespace() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for hint_mode in ["valid", "absent", "stale"] {
            let path = image_path(match hint_mode {
                "valid" => "locator-invalid-populated-namespace",
                "absent" => "scan-invalid-populated-namespace",
                _ => "stale-locator-invalid-populated-namespace",
            });
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let descriptor_block = volume.allocate_block(&mut device).unwrap();

                // The descriptor checksum and allocation references are valid, but the
                // referenced metadata stream is not a valid root namespace record.
                device.write_blocks(metadata_block, &[0_u8; 4096]).unwrap();
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
                device.flush().unwrap();
                match hint_mode {
                    "valid" => {
                        crate::volume::persist_inventory_hint(&mut device, descriptor_block)
                            .unwrap();
                    }
                    "stale" => {
                        // Point the persisted hint at an allocated non-descriptor block.
                        // Discovery must reject the hint and apply namespace validation
                        // when the full scan finds the actual descriptor.
                        crate::volume::persist_inventory_hint(&mut device, metadata_block).unwrap();
                    }
                    _ => {}
                }
                if hint_mode != "absent" {
                    // Reload the volume so discovery reads the persisted hint from disk;
                    // the original in-memory volume predates this superblock update.
                    volume = open_volume(&mut device).unwrap();
                }

                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut device),
                        Err(Error::Corrupt(
                            "persistent inventory descriptor contains an invalid namespace"
                        ))
                    ),
                    "invalid namespace was accepted with hint mode {hint_mode}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovery_read_failures_are_reported_and_retryable_with_a_locator() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        // Discovery performs three reads for this one-block empty inventory: bitmap
        // validation, candidate bitmap enumeration, and the candidate block batch.
        for fail_at in 0..3 {
            let path = image_path(&format!("inventory-discovery-read-failure-{fail_at}"));
            {
                let image = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut image = image;
                let mut volume = format_volume(&mut image, *b"openfs-test-0001", 0).unwrap();
                let root_block = volume.allocate_block(&mut image).unwrap();
                let root = crate::MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: crate::ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut root_bytes = vec![0_u8; 4096];
                root_bytes[..80].copy_from_slice(&root.encode().unwrap());
                image.write_blocks(root_block, &root_bytes).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![root_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let descriptor_block = volume
                    .publish_persistent_inventory(&mut image, &descriptor)
                    .unwrap();
                let mut failing = FailReadAt {
                    inner: image,
                    read_index: 0,
                    fail_at,
                };
                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut failing),
                        Err(Error::Io(_))
                    ),
                    "discovery must propagate injected read failure at read {fail_at}"
                );
                let found = volume
                    .discover_persistent_inventory(&mut failing)
                    .unwrap()
                    .unwrap();
                assert_eq!(found.0, descriptor_block);
                assert_eq!(found.1, descriptor);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovery_batch_read_failures_are_retryable() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        // A full bitmap page is scanned into batches of 64 candidates. With 130
        // consecutive allocations, reads 2, 3, and 4 hit each candidate batch.
        for fail_at in 2..5 {
            let path = image_path(&format!("inventory-discovery-batch-read-failure-{fail_at}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let allocated = volume.allocate_blocks(&mut device, 130).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let descriptor_block = *allocated.last().unwrap();
                device
                    .write_blocks(descriptor_block, &descriptor.encode(4096).unwrap())
                    .unwrap();
                device.flush().unwrap();

                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at,
                };
                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut failing),
                        Err(Error::Io(_))
                    ),
                    "discovery must propagate candidate-batch read failure at read {fail_at}"
                );
                let found = volume
                    .discover_persistent_inventory(&mut failing)
                    .unwrap()
                    .unwrap();
                assert_eq!(found.0, descriptor_block);
                assert_eq!(found.1, descriptor);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovery_namespace_read_failures_are_retryable() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        // Descriptor discovery reads are followed by packed metadata and directory reads.
        // These indices target the namespace-stream reads in this fixture.
        {
            let fail_at = 3;
            let path = image_path(&format!(
                "inventory-discovery-namespace-read-failure-{fail_at}"
            ));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                // Keep the descriptor first so its scan batch cannot be mistaken for
                // either packed namespace-stream read.
                let descriptor_block = volume.allocate_block(&mut device).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let directory_block = volume.allocate_block(&mut device).unwrap();

                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let file = MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: 12,
                    extent_root_block: None,
                    generation: 1,
                };
                let entry = DirectoryEntry {
                    parent_id: 1,
                    object_id: 2,
                    kind: ObjectKind::File,
                    name: "hello.txt".to_owned(),
                };
                let mut metadata_bytes = vec![0_u8; 4096];
                metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
                metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let mut directory_bytes = vec![0_u8; 4096];
                directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 2,
                    directory_blocks: vec![directory_block],
                    directory_entry_count: 1,
                };
                device
                    .write_blocks(descriptor_block, &descriptor.encode(4096).unwrap())
                    .unwrap();
                device.flush().unwrap();

                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at,
                };
                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut failing),
                        Err(Error::Io(_))
                    ),
                    "namespace discovery must propagate injected read failure at read {fail_at}"
                );
                let found = volume
                    .discover_persistent_inventory(&mut failing)
                    .unwrap()
                    .unwrap();
                assert_eq!(found.0, descriptor_block);
                assert_eq!(found.1, descriptor);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn out_of_range_locator_does_not_hide_discoverable_inventory() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        let path = image_path("out-of-range-inventory-locator");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let root_block = volume.allocate_block(&mut device).unwrap();
            let root = crate::MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: crate::ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut root_bytes = vec![0_u8; 4096];
            root_bytes[..80].copy_from_slice(&root.encode().unwrap());
            device.write_blocks(root_block, &root_bytes).unwrap();
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![root_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let descriptor_block = volume
                .publish_persistent_inventory(&mut device, &descriptor)
                .unwrap();

            // A damaged/stale locator may point beyond the device. The hint is advisory;
            // authoritative discovery must still find the unique allocated descriptor.
            let invalid_hint = device.block_count() + 17;
            crate::volume::persist_inventory_hint(&mut device, invalid_hint).unwrap();
            volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.inventory_descriptor_hint, Some(invalid_hint));

            let discovered = volume
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(discovered, (descriptor_block, descriptor));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn locator_fast_path_rejects_a_second_persistent_inventory_descriptor() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        let path = image_path("inventory-descriptor-ambiguous-with-locator");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let root_block = volume.allocate_block(&mut device).unwrap();
            let root = crate::MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: crate::ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut root_bytes = vec![0_u8; 4096];
            root_bytes[..80].copy_from_slice(&root.encode().unwrap());
            device.write_blocks(root_block, &root_bytes).unwrap();
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![root_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let first = volume
                .publish_persistent_inventory(&mut device, &descriptor)
                .unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            assert_ne!(first, second);
            device
                .write_blocks(second, &descriptor.encode(4096).unwrap())
                .unwrap();

            assert!(
                matches!(
                    volume.discover_persistent_inventory(&mut device),
                    Err(Error::Corrupt(
                        "multiple persistent inventory descriptors were discovered"
                    ))
                ),
                "a valid locator must not hide a second allocated descriptor"
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn rejects_ambiguous_multiple_persistent_inventory_descriptors() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096, 8192, 16384, 32768, 65536] {
            let path = image_path("inventory-descriptor-ambiguous-discovery");
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let descriptor_blocks = volume.allocate_blocks(&mut device, 2).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let encoded = descriptor.encode(block_size as usize).unwrap();
                for block in descriptor_blocks {
                    device.write_blocks(block, &encoded).unwrap();
                }

                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut device),
                        Err(Error::Corrupt(
                            "multiple persistent inventory descriptors were discovered"
                        ))
                    ),
                    "ambiguous descriptors must be rejected for block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn refuses_to_publish_invalid_inventory_without_modifying_image_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("invalid-inventory-no-write-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                // The allocated metadata block deliberately contains no valid record.
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let free_before = volume.free_data_block_count(&mut device).unwrap();
                let image_len = block_size as usize * device.block_count() as usize;
                let mut before = vec![0_u8; image_len];
                device.read_blocks(0, &mut before).unwrap();

                assert!(
                    volume
                        .publish_persistent_inventory(&mut device, &descriptor)
                        .is_err(),
                    "invalid metadata inventory was published at block size {block_size}"
                );

                let mut after = vec![0_u8; image_len];
                device.read_blocks(0, &mut after).unwrap();
                assert_eq!(
                    after, before,
                    "rejected invalid inventory modified image bytes at block size {block_size}"
                );
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "rejected invalid inventory consumed a block at block size {block_size}"
                );
                assert!(
                    volume
                        .discover_persistent_inventory(&mut device)
                        .unwrap()
                        .is_none(),
                    "rejected invalid inventory became discoverable at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn descriptor_publication_is_atomic_across_every_journal_write_failure() {
        use crate::{
            MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
            INVENTORY_DESCRIPTOR_MAGIC,
        };

        // The bitmap redo image occupies two chunks at the supported block sizes, so
        // the four-block ring holds begin, both chunks, and commit. Fail each journal,
        // home, and checkpoint write; reopen must expose old state or the complete descriptor.
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..=6 {
                let path = image_path(&format!("descriptor-atomic-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let metadata_block = volume.allocate_block(&mut device).unwrap();
                    let root = MetadataRecord {
                        id: ObjectId(1),
                        parent_id: 0,
                        kind: ObjectKind::Directory,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let mut metadata_bytes = vec![0; block_size as usize];
                    metadata_bytes[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&root.encode().unwrap());
                    device
                        .write_blocks(metadata_block, &metadata_bytes)
                        .unwrap();

                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    let free_before = volume.free_data_block_count(&mut device).unwrap();
                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at,
                    };
                    assert!(
                        matches!(
                            volume.publish_persistent_inventory(&mut failing, &descriptor),
                            Err(Error::Io(_))
                        ),
                        "write {fail_at} should fail (block size {block_size})"
                    );

                    let mut device = failing.inner;
                    drop(volume);
                    let mut recovered = open_volume(&mut device).unwrap();
                    let discovered = recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap();
                    let free_after = recovered.free_data_block_count(&mut device).unwrap();
                    if fail_at <= 4 {
                        assert!(
                            discovered.is_none(),
                            "uncommitted descriptor must remain invisible (block size {block_size}, write {fail_at})"
                        );
                        assert_eq!(
                            free_after, free_before,
                            "uncommitted descriptor must not consume an allocation (block size {block_size}, write {fail_at})"
                        );

                        // Reusing the staged block must erase the uncommitted descriptor
                        // magic before allocation makes that block part of the live volume.
                        let reused = recovered.allocate_block(&mut device).unwrap();
                        let mut reused_contents = vec![0_u8; block_size as usize];
                        device.read_blocks(reused, &mut reused_contents).unwrap();
                        assert!(
                            !reused_contents.starts_with(INVENTORY_DESCRIPTOR_MAGIC),
                            "reused block must not retain staged descriptor magic (block size {block_size}, write {fail_at})"
                        );
                        assert!(
                            recovered
                                .discover_persistent_inventory(&mut device)
                                .unwrap()
                                .is_none(),
                            "reused staging block must not publish a false descriptor (block size {block_size}, write {fail_at})"
                        );
                    } else {
                        assert_eq!(
                            free_after,
                            free_before - 1,
                            "committed descriptor allocation must survive recovery (block size {block_size}, write {fail_at})"
                        );
                        let (descriptor_block, found) = discovered.unwrap_or_else(|| {
                            panic!(
                                "committed descriptor missing after recovery (block size {block_size}, write {fail_at})"
                            )
                        });
                        assert_eq!(found, descriptor);
                        assert!(recovered
                            .is_data_block_allocated(&mut device, descriptor_block)
                            .unwrap());
                        assert_eq!(
                            recovered
                                .publish_persistent_inventory(&mut device, &descriptor)
                                .unwrap(),
                            descriptor_block,
                            "retry after an uncertain committed publication must reuse the existing descriptor (block size {block_size}, write {fail_at})"
                        );
                    }

                    // Recovery must be idempotent: a second close/reopen cannot change
                    // whether the descriptor is visible or who owns its allocation.
                    drop(recovered);
                    drop(device);
                    let mut device = ImageDevice::open(&path, block_size).unwrap();
                    let recovered = open_volume(&mut device).unwrap();
                    let rediscovered = recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap();
                    if fail_at <= 4 {
                        assert!(
                            rediscovered.is_none(),
                            "uncommitted descriptor became visible after a second reopen (block size {block_size}, write {fail_at})"
                        );
                    } else {
                        let (descriptor_block, found) = rediscovered.unwrap_or_else(|| {
                            panic!(
                                "committed descriptor disappeared after a second reopen (block size {block_size}, write {fail_at})"
                            )
                        });
                        assert_eq!(found, descriptor);
                        assert!(recovered
                            .is_data_block_allocated(&mut device, descriptor_block)
                            .unwrap());
                        assert_eq!(
                            recovered.free_data_block_count(&mut device).unwrap(),
                            free_before - 1,
                            "committed allocation changed after a second reopen (block size {block_size}, write {fail_at})"
                        );
                    }
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn refuses_to_publish_when_existing_allocations_have_no_owner() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("orphan-allocation-publication-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let orphan_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let free_before = volume.free_data_block_count(&mut device).unwrap();

                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::InvalidInput(
                            "persistent inventory has unresolved allocation ownership anomalies"
                        ))
                    ),
                    "publication must reject orphan blocks at block size {block_size}"
                );
                assert!(
                    volume
                        .is_data_block_allocated(&mut device, orphan_block)
                        .unwrap(),
                    "failed publication must not free the orphan block at block size {block_size}"
                );
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "failed publication must not consume a descriptor block at block size {block_size}"
                );
                assert!(
                    volume
                        .discover_persistent_inventory(&mut device)
                        .unwrap()
                        .is_none(),
                    "failed publication must remain undiscoverable at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn publication_journal_commit_flush_failure_recovers_committed_inventory() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("inventory-journal-commit-flush-{block_size}"));
            let metadata_block;
            let free_before;
            let descriptor = {
                let raw = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut device = FailNthFlush {
                    inner: raw,
                    armed: false,
                    flushes_before_failure: 2,
                };
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                metadata_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                free_before = volume.free_data_block_count(&mut device).unwrap();
                // Publication flushes the staged descriptor first, then the journal's
                // begin/update records. Fail the next flush, after the commit record write.
                device.armed = true;
                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::Io(_))
                    ),
                    "injected journal commit flush must report an I/O error at block size {block_size}"
                );
                descriptor
            };

            // The image backend exposes writes made before the injected flush error.
            // Reopening must replay the committed redo record, so the reported error has
            // an uncertain outcome: the descriptor can legitimately become visible.
            {
                let mut device = ImageDevice::open(&path, block_size).unwrap();
                let volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before - 1,
                    "journal recovery must apply the committed descriptor allocation at block size {block_size}"
                );
                let discovered = volume
                    .discover_persistent_inventory(&mut device)
                    .unwrap()
                    .expect("committed descriptor must be discoverable after journal recovery");
                assert_eq!(discovered.1, descriptor);
                assert!(
                    volume
                        .is_data_block_allocated(&mut device, discovered.0)
                        .unwrap(),
                    "recovered descriptor block must be allocated at block size {block_size}"
                );
            }

            // The commit-flush error is explicitly an uncertain outcome. A second
            // recovery must not replay the allocation twice or lose the descriptor.
            {
                let mut device = ImageDevice::open(&path, block_size).unwrap();
                let volume = open_volume(&mut device).unwrap();
                let (block, discovered) = volume
                    .discover_persistent_inventory(&mut device)
                    .unwrap()
                    .expect("committed descriptor must survive a second recovery");
                assert_eq!(discovered, descriptor);
                assert!(volume.is_data_block_allocated(&mut device, block).unwrap());
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before - 1,
                    "second recovery must not apply the descriptor allocation twice at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn publication_descriptor_write_failure_survives_reopen_and_remains_retryable() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("inventory-publication-write-reopen-{block_size}"));
            let metadata_block;
            let free_before;
            {
                let raw = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut device = FailNextWrite {
                    inner: raw,
                    fail_next_write: false,
                };
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                metadata_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                free_before = volume.free_data_block_count(&mut device).unwrap();
                device.fail_next_write = true;

                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::Io(_))
                    ),
                    "injected descriptor write must fail publication at block size {block_size}"
                );
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "failed descriptor write must not allocate a block at block size {block_size}"
                );
                assert!(
                    volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                    "failed descriptor write must remain undiscoverable before reopen at block size {block_size}"
                );
            }

            {
                let mut device = ImageDevice::open(&path, block_size).unwrap();
                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "failed descriptor write must leave allocation state unchanged after reopen at block size {block_size}"
                );
                assert!(
                    volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                    "failed descriptor write must remain undiscoverable after reopen at block size {block_size}"
                );
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let descriptor_block = volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before - 1,
                    "retry must consume exactly one descriptor block at block size {block_size}"
                );
                assert_eq!(
                    volume.discover_persistent_inventory(&mut device).unwrap(),
                    Some((descriptor_block, descriptor)),
                    "retry must publish a discoverable descriptor at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn torn_descriptor_write_prefixes_are_recovered_and_reusable_at_every_block_size() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            // Exercise every non-empty prefix of the descriptor magic, then
            // representative header, midpoint, and near-full-block tears.
            let mut prefixes: Vec<usize> = (1..=crate::INVENTORY_DESCRIPTOR_MAGIC.len()).collect();
            prefixes.extend([48, block_size as usize / 2, block_size as usize - 1]);
            prefixes.sort_unstable();
            prefixes.dedup();
            for prefix_len in prefixes {
                let path = image_path(&format!("inventory-torn-prefix-{block_size}-{prefix_len}"));
                let metadata_block;
                let metadata_record;
                let free_before;
                let descriptor_block;
                let descriptor = {
                    let raw = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut device = TornNextWrite {
                        inner: raw,
                        armed: false,
                        prefix_len,
                    };
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    metadata_block = volume.allocate_block(&mut device).unwrap();
                    let root = MetadataRecord {
                        id: ObjectId(1),
                        parent_id: 0,
                        kind: ObjectKind::Directory,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    metadata_record = root.encode().unwrap();
                    let mut metadata_bytes = vec![0_u8; block_size as usize];
                    metadata_bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&metadata_record);
                    device
                        .write_blocks(metadata_block, &metadata_bytes)
                        .unwrap();

                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    free_before = volume.free_data_block_count(&mut device).unwrap();
                    device.armed = true;
                    assert!(
                        matches!(
                            volume.publish_persistent_inventory(&mut device, &descriptor),
                            Err(Error::Io(_))
                        ),
                        "torn prefix {prefix_len} must fail publication at block size {block_size}"
                    );
                    drop(volume);
                    descriptor
                };

                {
                    let mut device = ImageDevice::open(&path, block_size).unwrap();
                    let mut volume = open_volume(&mut device).unwrap();
                    assert_eq!(
                        volume.free_data_block_count(&mut device).unwrap(),
                        free_before,
                        "torn prefix {prefix_len} must not allocate a descriptor block at block size {block_size}"
                    );
                    assert!(
                        volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                        "torn prefix {prefix_len} must not be discoverable after reopen at block size {block_size}"
                    );
                    let mut persisted_metadata = vec![0_u8; block_size as usize];
                    device
                        .read_blocks(metadata_block, &mut persisted_metadata)
                        .unwrap();
                    assert_eq!(
                        &persisted_metadata[..crate::METADATA_RECORD_SIZE],
                        metadata_record.as_slice(),
                        "torn prefix {prefix_len} changed the unrelated root metadata at block size {block_size}"
                    );

                    // The staged candidate is the first free block. Allocation must
                    // scrub a complete descriptor magic before transferring its ownership.
                    let reused = volume.allocate_block(&mut device).unwrap();
                    let mut contents = vec![0_u8; block_size as usize];
                    device.read_blocks(reused, &mut contents).unwrap();
                    assert!(
                        !contents.starts_with(crate::INVENTORY_DESCRIPTOR_MAGIC),
                        "reused block retained descriptor magic after torn prefix {prefix_len} at block size {block_size}"
                    );
                    assert!(
                        volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                        "reusing torn staging bytes must not publish a descriptor at block size {block_size}"
                    );
                    // Return the probe allocation so publication's strict ownership
                    // verifier does not mistake this test-only allocation for an orphan.
                    volume.free_block(&mut device, reused).unwrap();

                    descriptor_block = volume
                        .publish_persistent_inventory(&mut device, &descriptor)
                        .unwrap();
                    assert_eq!(
                        volume.discover_persistent_inventory(&mut device).unwrap(),
                        Some((descriptor_block, descriptor.clone())),
                        "publication retry must succeed after torn prefix {prefix_len} at block size {block_size}"
                    );
                    assert_eq!(
                        volume.free_data_block_count(&mut device).unwrap(),
                        free_before - 1,
                        "successful retry must consume exactly one descriptor block after torn prefix {prefix_len} at block size {block_size}"
                    );
                    drop(volume);
                    drop(device);
                }

                // Recovery must be repeatable after the successful retry, not merely
                // correct in the still-open instance that performed the publication.
                {
                    let mut device = ImageDevice::open(&path, block_size).unwrap();
                    let volume = open_volume(&mut device).unwrap();
                    assert_eq!(
                        volume.discover_persistent_inventory(&mut device).unwrap(),
                        Some((descriptor_block, descriptor.clone())),
                        "successful retry must remain discoverable after reopen at block size {block_size}"
                    );
                    assert_eq!(
                        volume.free_data_block_count(&mut device).unwrap(),
                        free_before - 1,
                        "reopen must preserve exactly one descriptor allocation after torn prefix {prefix_len} at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn publication_flush_failure_survives_reopen_and_remains_retryable() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("inventory-publication-flush-reopen-{block_size}"));
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let free_before;
            let metadata_block;
            {
                let raw = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut device = FailNextFlush {
                    inner: raw,
                    fail_next_flush: false,
                };
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                metadata_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    metadata_blocks: vec![metadata_block],
                    ..descriptor.clone()
                };
                free_before = volume.free_data_block_count(&mut device).unwrap();
                device.fail_next_flush = true;

                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::Io(_))
                    ),
                    "injected staging flush must fail publication at block size {block_size}"
                );
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "failed staging flush must not commit descriptor allocation at block size {block_size}"
                );
                assert!(
                    volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                    "unallocated staged bytes must not be discoverable before reopen at block size {block_size}"
                );
            }

            // Reopen the image with a fresh device and recover the journal, rather than
            // relying on the pre-failure in-memory volume state.
            {
                let mut device = ImageDevice::open(&path, block_size).unwrap();
                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before,
                    "failed publication must leave the descriptor allocation free after reopen at block size {block_size}"
                );
                assert!(
                    volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                    "failed publication must remain undiscoverable after reopen at block size {block_size}"
                );

                let descriptor = PersistentInventoryDescriptor {
                    metadata_blocks: vec![metadata_block],
                    ..descriptor
                };
                let descriptor_block = volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before - 1,
                    "successful retry must consume exactly one descriptor block at block size {block_size}"
                );
                assert_eq!(
                    volume.discover_persistent_inventory(&mut device).unwrap(),
                    Some((descriptor_block, descriptor)),
                    "successful retry must publish a discoverable descriptor at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn rejects_multiple_persistent_inventory_descriptors_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("duplicate-inventory-descriptors-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let encoded = descriptor.encode(block_size as usize).unwrap();
                device.write_blocks(first, &encoded).unwrap();
                device.write_blocks(second, &encoded).unwrap();

                assert!(
                    matches!(
                        volume.discover_persistent_inventory(&mut device),
                        Err(Error::Corrupt(
                            "multiple persistent inventory descriptors were discovered"
                        ))
                    ),
                    "discovery must reject two descriptors at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovery_finds_sparse_descriptor_near_end_of_volume_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("sparse-descriptor-near-end-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                let descriptor_block = layout.backup_superblock - 1;
                let bits_per_bitmap_block = u64::from(block_size) * 8;
                let bitmap_block = layout.bitmap_start + descriptor_block / bits_per_bitmap_block;
                let local_bit = descriptor_block % bits_per_bitmap_block;
                let mut bitmap = vec![0_u8; block_size as usize];
                device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                bitmap[(local_bit / 8) as usize] |= 1_u8 << (local_bit % 8);
                device.write_blocks(bitmap_block, &bitmap).unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                device
                    .write_blocks(
                        descriptor_block,
                        &descriptor.encode(block_size as usize).unwrap(),
                    )
                    .unwrap();
                device.flush().unwrap();

                assert_eq!(
                    volume.discover_persistent_inventory(&mut device).unwrap(),
                    Some((descriptor_block, descriptor)),
                    "sparse descriptor at the final data block must be found at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn discovery_rejects_descriptor_aliasing_its_inventory_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for alias_kind in ["metadata", "directory"] {
                let path = image_path(&format!(
                    "descriptor-inventory-alias-{alias_kind}-{block_size}"
                ));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let descriptor_block = volume.allocate_block(&mut device).unwrap();
                    let descriptor = match alias_kind {
                        "metadata" => PersistentInventoryDescriptor {
                            root_id: ObjectId(1),
                            metadata_blocks: vec![descriptor_block],
                            metadata_record_count: 1,
                            directory_blocks: vec![],
                            directory_entry_count: 0,
                        },
                        "directory" => PersistentInventoryDescriptor {
                            root_id: ObjectId(1),
                            metadata_blocks: vec![],
                            metadata_record_count: 0,
                            directory_blocks: vec![descriptor_block],
                            directory_entry_count: 1,
                        },
                        _ => unreachable!("test cases are fixed"),
                    };
                    device
                        .write_blocks(
                            descriptor_block,
                            &descriptor.encode(block_size as usize).unwrap(),
                        )
                        .unwrap();

                    assert!(
                        matches!(
                            volume.discover_persistent_inventory(&mut device),
                            Err(Error::Corrupt(
                                "inventory descriptor aliases one of its inventory blocks"
                            ))
                        ),
                        "discovery accepted descriptor/{alias_kind} alias at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn discovery_rejects_descriptors_that_reference_freed_inventory_blocks() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for freed_inventory in ["metadata", "directory"] {
                let path = image_path(&format!(
                    "discovery-freed-{freed_inventory}-inventory-{block_size}"
                ));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let metadata_block = volume.allocate_block(&mut device).unwrap();
                    let directory_block = volume.allocate_block(&mut device).unwrap();
                    let descriptor_block = volume.allocate_block(&mut device).unwrap();
                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![directory_block],
                        directory_entry_count: 1,
                    };
                    let encoded = descriptor.encode(block_size as usize).unwrap();
                    device.write_blocks(descriptor_block, &encoded).unwrap();

                    let freed_block = match freed_inventory {
                        "metadata" => metadata_block,
                        "directory" => directory_block,
                        _ => unreachable!("test cases are fixed"),
                    };
                    // Simulate on-disk bitmap corruption directly. The public allocator
                    // now correctly refuses to create this invalid state itself.
                    let bits_per_bitmap_block = u64::from(block_size) * 8;
                    let bitmap_block =
                        volume.volume.layout.bitmap_start + freed_block / bits_per_bitmap_block;
                    let local_bit = freed_block % bits_per_bitmap_block;
                    let mut bitmap = vec![0_u8; block_size as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    bitmap[(local_bit / 8) as usize] &= !(1_u8 << (local_bit % 8));
                    device.write_blocks(bitmap_block, &bitmap).unwrap();

                    assert!(
                        matches!(
                            volume.discover_persistent_inventory(&mut device),
                            Err(Error::Corrupt(
                                "inventory descriptor references a free inventory block"
                            ))
                        ),
                        "discovery accepted a freed {freed_inventory} block at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn reads_persistent_record_inventories_and_reconstructs_namespace() {
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};

        let path = image_path("persistent-namespace-reconstruction");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let directory_block = volume.allocate_block(&mut device).unwrap();

            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let file = MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 12,
                extent_root_block: None,
                generation: 1,
            };
            let entry = DirectoryEntry {
                parent_id: 1,
                object_id: 2,
                kind: ObjectKind::File,
                name: "hello.txt".to_owned(),
            };
            let mut metadata_bytes = vec![0_u8; 4096];
            metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
            metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
            device
                .write_blocks(metadata_block, &metadata_bytes)
                .unwrap();
            let mut directory_bytes = vec![0_u8; 4096];
            directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
            device
                .write_blocks(directory_block, &directory_bytes)
                .unwrap();
            device.flush().unwrap();

            let namespace = volume
                .reconstruct_namespace(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    2,
                    &[directory_block],
                    1,
                )
                .unwrap();
            assert_eq!(namespace.resolve_path("/hello.txt").unwrap(), ObjectId(2));
            assert_eq!(namespace.object_count(), 2);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn reads_file_ranges_across_fragmented_extents_and_sparse_holes() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        let path = image_path("file-range-sparse-read");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let directory_block = volume.allocate_block(&mut device).unwrap();
            let extent_root = volume.allocate_block(&mut device).unwrap();
            let first_data = volume.allocate_block(&mut device).unwrap();
            let second_data = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let file = MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 12_000,
                extent_root_block: Some(extent_root),
                generation: 1,
            };
            let entry = DirectoryEntry {
                parent_id: 1,
                object_id: 2,
                kind: ObjectKind::File,
                name: "sparse.bin".into(),
            };
            let mut metadata = vec![0; 4096];
            metadata[..80].copy_from_slice(&root.encode().unwrap());
            metadata[80..160].copy_from_slice(&file.encode().unwrap());
            device.write_blocks(metadata_block, &metadata).unwrap();
            let mut directory = vec![0; 4096];
            directory[..128].copy_from_slice(&entry.encode().unwrap());
            device.write_blocks(directory_block, &directory).unwrap();
            device.write_blocks(first_data, &vec![0xA5; 4096]).unwrap();
            device.write_blocks(second_data, &vec![0x5A; 4096]).unwrap();
            let map = ExtentMap::new(vec![
                Extent {
                    logical_start: 0,
                    physical_start: first_data,
                    block_count: 1,
                },
                Extent {
                    logical_start: 2,
                    physical_start: second_data,
                    block_count: 1,
                },
            ])
            .unwrap();
            device
                .write_blocks(extent_root, &map.encode_block(4096).unwrap())
                .unwrap();
            volume
                .publish_persistent_inventory(
                    &mut device,
                    &PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 2,
                        directory_blocks: vec![directory_block],
                        directory_entry_count: 1,
                    },
                )
                .unwrap();
            let namespace = volume
                .reconstruct_namespace(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    2,
                    &[directory_block],
                    1,
                )
                .unwrap();
            let maps = volume
                .read_namespace_extent_maps(&mut device, &namespace)
                .unwrap();
            let full = volume
                .read_file_contents(&mut device, &namespace, &maps, ObjectId(2))
                .unwrap();
            assert_eq!(&full[..4096], &vec![0xA5; 4096]);
            assert!(full[4096..8192].iter().all(|byte| *byte == 0));
            assert_eq!(&full[8192..], &vec![0x5A; 3808]);
            assert_eq!(
                volume
                    .read_path_range(&mut device, &namespace, &maps, "/sparse.bin", 4094, 6)
                    .unwrap(),
                vec![0xA5, 0xA5, 0, 0, 0, 0],
            );
            assert!(volume
                .read_file_range(&mut device, &namespace, &maps, ObjectId(2), 12_000, 32)
                .unwrap()
                .is_empty());
            assert_eq!(
                volume
                    .read_file_range(&mut device, &namespace, &maps, ObjectId(2), 11_998, 20)
                    .unwrap()
                    .len(),
                2
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn file_reads_reject_directories_missing_maps_and_freed_extents() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };
        use std::collections::BTreeMap;

        let path = image_path("file-range-invalid");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let directory_block = volume.allocate_block(&mut device).unwrap();
            let extent_root = volume.allocate_block(&mut device).unwrap();
            let data_block = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let file = MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 4096,
                extent_root_block: Some(extent_root),
                generation: 1,
            };
            let entry = DirectoryEntry {
                parent_id: 1,
                object_id: 2,
                kind: ObjectKind::File,
                name: "data.bin".into(),
            };
            let mut metadata = vec![0; 4096];
            metadata[..80].copy_from_slice(&root.encode().unwrap());
            metadata[80..160].copy_from_slice(&file.encode().unwrap());
            device.write_blocks(metadata_block, &metadata).unwrap();
            let mut directory = vec![0; 4096];
            directory[..128].copy_from_slice(&entry.encode().unwrap());
            device.write_blocks(directory_block, &directory).unwrap();
            let map = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: data_block,
                block_count: 1,
            }])
            .unwrap();
            device
                .write_blocks(extent_root, &map.encode_block(4096).unwrap())
                .unwrap();
            device.write_blocks(data_block, &vec![0xCD; 4096]).unwrap();
            volume
                .publish_persistent_inventory(
                    &mut device,
                    &PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 2,
                        directory_blocks: vec![directory_block],
                        directory_entry_count: 1,
                    },
                )
                .unwrap();
            let namespace = volume
                .reconstruct_namespace(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    2,
                    &[directory_block],
                    1,
                )
                .unwrap();
            let maps = volume
                .read_namespace_extent_maps(&mut device, &namespace)
                .unwrap();

            // An allocated-but-unrelated block is not a valid substitute for the map
            // named by the file's on-disk metadata. Without this comparison, a caller
            // could redirect reads by supplying a forged in-memory extent map.
            let mut forged_maps = maps.clone();
            forged_maps.insert(
                ObjectId(2),
                crate::ExtentMap::new(vec![crate::Extent {
                    logical_start: 0,
                    physical_start: metadata_block,
                    block_count: 1,
                }])
                .unwrap(),
            );
            assert!(matches!(
                volume.read_file_range(&mut device, &namespace, &forged_maps, ObjectId(2), 0, 8),
                Err(Error::Corrupt(
                    "loaded extent map does not match persistent extent-map root"
                ))
            ));

            assert!(matches!(
                volume.read_file_range(&mut device, &namespace, &maps, ObjectId(1), 0, 8),
                Err(Error::InvalidInput("object is not a regular file"))
            ));
            assert!(matches!(
                volume.read_file_range(&mut device, &namespace, &maps, ObjectId(99), 0, 8),
                Err(Error::InvalidInput("file object does not exist"))
            ));
            assert!(matches!(
                volume.read_file_range(
                    &mut device,
                    &namespace,
                    &BTreeMap::new(),
                    ObjectId(2),
                    0,
                    8
                ),
                Err(Error::Corrupt(
                    "file extent map is missing from loaded inventory"
                ))
            ));
            // Persistent ownership prevents freeing data still referenced by a file.
            assert!(matches!(
                volume.free_block(&mut device, data_block),
                Err(Error::InvalidInput(
                    "cannot free a block owned by persistent file data"
                ))
            ));
            assert!(volume
                .is_data_block_allocated(&mut device, data_block)
                .unwrap());
            assert_eq!(
                volume
                    .read_file_range(&mut device, &namespace, &maps, ObjectId(2), 0, 8)
                    .unwrap(),
                vec![0xCD; 8]
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn reads_persistent_extent_maps_and_checks_bitmap_ownership() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("persistent-extent-map-read-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let directory_block = volume.allocate_block(&mut device).unwrap();
                let extent_root = volume.allocate_block(&mut device).unwrap();
                let data_block = volume.allocate_block(&mut device).unwrap();
                let second_data_block = volume.allocate_block(&mut device).unwrap();
                let block_len = block_size as usize;
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let file = MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: u64::from(block_size) * 2,
                    extent_root_block: Some(extent_root),
                    generation: 1,
                };
                let entry = DirectoryEntry {
                    parent_id: 1,
                    object_id: 2,
                    kind: ObjectKind::File,
                    name: "payload.bin".to_owned(),
                };
                let mut metadata_bytes = vec![0_u8; block_len];
                metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
                metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let mut directory_bytes = vec![0_u8; block_len];
                directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();
                let map = ExtentMap::new(vec![
                    Extent {
                        logical_start: 0,
                        physical_start: data_block,
                        block_count: 1,
                    },
                    Extent {
                        logical_start: 1,
                        physical_start: second_data_block,
                        block_count: 1,
                    },
                ])
                .unwrap();
                device
                    .write_blocks(extent_root, &map.encode_block(block_len).unwrap())
                    .unwrap();
                let payload: Vec<u8> = (0..block_len * 2)
                    .map(|index| (index % 251) as u8)
                    .collect();
                device
                    .write_blocks(data_block, &payload[..block_len])
                    .unwrap();
                device
                    .write_blocks(second_data_block, &payload[block_len..])
                    .unwrap();
                device.flush().unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 2,
                    directory_blocks: vec![directory_block],
                    directory_entry_count: 1,
                };
                volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap();

                // Metadata, extent maps, and file bytes survive reopen and are
                // rediscovered from the on-disk inventory descriptor.
                drop(volume);
                let mut volume = open_volume(&mut device).unwrap();
                let (_, namespace) = volume
                    .reconstruct_discovered_namespace(&mut device)
                    .unwrap()
                    .expect("persistent namespace must be discoverable after reopen");
                let loaded = volume
                    .read_namespace_extent_maps(&mut device, &namespace)
                    .unwrap();
                assert_eq!(loaded.get(&ObjectId(2)).unwrap(), &map);
                assert_eq!(
                    volume
                        .read_file_contents(&mut device, &namespace, &loaded, ObjectId(2))
                        .unwrap(),
                    payload
                );
                // The public persistent-path entry point must independently rediscover
                // metadata after reopen, rather than requiring caller-supplied caches.
                assert_eq!(
                    volume
                        .read_persistent_path_range(&mut device, "/payload.bin", 17, 64)
                        .unwrap(),
                    payload[17..81]
                );
                // Path-based writes resolve their own persistent metadata and retain the
                // same bounded-write semantics as the lower-level object-ID interface.
                let replacement = [0xA5; 64];
                volume
                    .write_persistent_path_range(&mut device, "/payload.bin", 17, &replacement)
                    .unwrap();
                assert_eq!(
                    volume
                        .read_persistent_path_range(&mut device, "/payload.bin", 17, 64)
                        .unwrap(),
                    replacement
                );

                // The path-based update must survive closing and reopening the volume.
                drop(volume);
                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume
                        .read_persistent_path_range(&mut device, "/payload.bin", 17, 64)
                        .unwrap(),
                    replacement
                );
                let mut expected = payload.clone();
                expected[17..81].copy_from_slice(&replacement);
                let (_, namespace) = volume
                    .reconstruct_discovered_namespace(&mut device)
                    .unwrap()
                    .expect("persistent namespace must survive write and reopen");
                let loaded = volume
                    .read_namespace_extent_maps(&mut device, &namespace)
                    .unwrap();

                // Snapshot the exact in-file prefix that an invalid cross-EOF write
                // would corrupt if it were partially applied.
                let before_eof_write = volume
                    .read_persistent_path_range(
                        &mut device,
                        "/payload.bin",
                        (block_len * 2 - 10) as u64,
                        10,
                    )
                    .unwrap();
                assert_eq!(before_eof_write.len(), 10);
                assert!(matches!(
                    volume.write_persistent_path_range(
                        &mut device,
                        "/payload.bin",
                        (block_len * 2 - 10) as u64,
                        &[0xCC; 64],
                    ),
                    Err(Error::InvalidInput(
                        "write range exceeds existing file size",
                    ))
                ));
                assert_eq!(
                    volume
                        .read_persistent_path_range(
                            &mut device,
                            "/payload.bin",
                            (block_len * 2 - 10) as u64,
                            10,
                        )
                        .unwrap(),
                    before_eof_write
                );
                assert_eq!(
                    volume
                        .read_persistent_path_range(&mut device, "/payload.bin", 17, 64)
                        .unwrap(),
                    replacement
                );

                // A forged in-memory record must not authorize writes even when
                // its extent map still matches the real on-disk map.
                let mut forged_file = namespace.object(ObjectId(2)).unwrap().clone();
                forged_file.generation += 1;
                let forged_namespace = crate::Namespace::from_records(
                    namespace.root_id(),
                    [namespace.object(ObjectId(1)).unwrap().clone(), forged_file],
                    [namespace
                        .lookup(ObjectId(1), "payload.bin")
                        .unwrap()
                        .clone()],
                )
                .unwrap();

                // Matching object records are insufficient: a caller-supplied namespace
                // with a forged directory name must not pass persistent identity checks.
                let mut renamed_entry = namespace
                    .lookup(ObjectId(1), "payload.bin")
                    .unwrap()
                    .clone();
                renamed_entry.name = "renamed.bin".to_owned();
                let forged_directory_namespace = crate::Namespace::from_records(
                    namespace.root_id(),
                    namespace.objects().cloned(),
                    [renamed_entry],
                )
                .unwrap();
                assert!(matches!(
                    volume.read_file_range(
                        &mut device,
                        &forged_directory_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        8,
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));
                assert!(matches!(
                    volume.write_file_range(
                        &mut device,
                        &forged_directory_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        &[0xCC; 8],
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));

                let mut before_forged_write = vec![0_u8; block_len];
                device
                    .read_blocks(data_block, &mut before_forged_write)
                    .unwrap();
                assert!(matches!(
                    volume.write_file_range(
                        &mut device,
                        &forged_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        &[0xCC; 8],
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));
                let mut after_forged_write = vec![0_u8; block_len];
                device
                    .read_blocks(data_block, &mut after_forged_write)
                    .unwrap();
                assert_eq!(after_forged_write, before_forged_write);

                // Reads must also reject caller-supplied metadata that is not the
                // namespace published on disk, even when its extent map is unchanged.
                assert!(matches!(
                    volume.read_file_range(
                        &mut device,
                        &forged_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        8,
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));

                // Even a zero-length read must not silently accept stale metadata.
                assert!(matches!(
                    volume.read_file_range(
                        &mut device,
                        &forged_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        0,
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));

                // Empty writes are no-ops only for the actual persistent namespace.
                assert!(matches!(
                    volume.write_file_range(
                        &mut device,
                        &forged_namespace,
                        &loaded,
                        ObjectId(2),
                        0,
                        &[],
                    ),
                    Err(Error::Corrupt(
                        "supplied namespace metadata does not match persistent inventory"
                    ))
                ));

                // Partial writes preserve the surrounding bytes and are visible
                // through the normal persistent read path after a flush.
                let replacement = vec![0xA5; 97];
                let write_offset = block_len as u64 - 32;
                volume
                    .write_file_range(
                        &mut device,
                        &namespace,
                        &loaded,
                        ObjectId(2),
                        write_offset,
                        &replacement,
                    )
                    .unwrap();
                let write_start = write_offset as usize;
                expected[write_start..write_start + replacement.len()]
                    .copy_from_slice(&replacement);
                assert_eq!(
                    volume
                        .read_file_contents(&mut device, &namespace, &loaded, ObjectId(2))
                        .unwrap(),
                    expected
                );

                // A successful write must remain visible after closing the volume
                // and rediscovering the namespace from its on-disk descriptor.
                drop(volume);
                let mut volume = open_volume(&mut device).unwrap();
                let (_, namespace) = volume
                    .reconstruct_discovered_namespace(&mut device)
                    .unwrap()
                    .expect("persistent namespace must survive write and reopen");
                let loaded = volume
                    .read_namespace_extent_maps(&mut device, &namespace)
                    .unwrap();
                assert_eq!(
                    volume
                        .read_file_contents(&mut device, &namespace, &loaded, ObjectId(2))
                        .unwrap(),
                    expected,
                    "written bytes must survive close/reopen at block size {block_size}"
                );

                // A durable COMMIT followed by a failed home-block write must be
                // completed by normal reopen/recovery, never exposed as a partial write.
                let recovery_offset = block_len as u64 - 16;
                let recovery_payload = vec![0x3C; 97];
                // UPDATE records may be chunked differently for each supported block size.
                // The record count is also the write index of the first home-block update.
                let fail_after_commit = volume.journal.required_record_count(&device, 2).unwrap();
                let mut failing = FailWriteAt {
                    inner: device,
                    write_index: 0,
                    fail_at: fail_after_commit,
                };
                assert!(matches!(
                    volume.write_file_range(
                        &mut failing,
                        &namespace,
                        &loaded,
                        ObjectId(2),
                        recovery_offset,
                        &recovery_payload,
                    ),
                    Err(Error::Io(_))
                ));
                drop(volume);
                let mut device = failing.inner;
                let mut volume = open_volume(&mut device).unwrap();
                let (_, namespace) = volume
                    .reconstruct_discovered_namespace(&mut device)
                    .unwrap()
                    .expect("committed file write must remain discoverable after recovery");
                let loaded = volume
                    .read_namespace_extent_maps(&mut device, &namespace)
                    .unwrap();
                let recovery_start = usize::try_from(recovery_offset).unwrap();
                expected[recovery_start..recovery_start + recovery_payload.len()]
                    .copy_from_slice(&recovery_payload);
                assert_eq!(
                    volume
                        .read_file_contents(&mut device, &namespace, &loaded, ObjectId(2))
                        .unwrap(),
                    expected,
                    "reopen must replay every block of a committed write at block size {block_size}"
                );

                assert!(matches!(
                    volume.write_file_range(
                        &mut device,
                        &namespace,
                        &loaded,
                        ObjectId(2),
                        (block_len * 2) as u64 - 2,
                        &[1, 2, 3],
                    ),
                    Err(Error::InvalidInput(
                        "write range exceeds existing file size"
                    ))
                ));

                assert!(matches!(
                    volume.free_block(&mut device, extent_root),
                    Err(Error::InvalidInput(
                        "cannot free a block referenced by a file extent map"
                    ))
                ));
                assert!(matches!(
                    volume.free_block(&mut device, data_block),
                    Err(Error::InvalidInput(
                        "cannot free a block owned by persistent file data"
                    ))
                ));
                assert!(volume
                    .is_data_block_allocated(&mut device, data_block)
                    .unwrap());
                assert!(volume
                    .is_data_block_allocated(&mut device, second_data_block)
                    .unwrap());
                assert_eq!(
                    volume
                        .read_file_contents(&mut device, &namespace, &loaded, ObjectId(2))
                        .unwrap(),
                    expected
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn namespace_extent_maps_reject_cross_file_physical_aliases() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("persistent-extent-map-alias-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let directory_block = volume.allocate_block(&mut device).unwrap();
                let first_root = volume.allocate_block(&mut device).unwrap();
                let second_root = volume.allocate_block(&mut device).unwrap();
                let shared_data = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let first = MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: u64::from(block_size),
                    extent_root_block: Some(first_root),
                    generation: 1,
                };
                let second = MetadataRecord {
                    id: ObjectId(3),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: u64::from(block_size),
                    extent_root_block: Some(second_root),
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                for (index, record) in [root, first, second].iter().enumerate() {
                    metadata_bytes[index * 80..(index + 1) * 80]
                        .copy_from_slice(&record.encode().unwrap());
                }
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let entries = [
                    DirectoryEntry {
                        parent_id: 1,
                        object_id: 2,
                        kind: ObjectKind::File,
                        name: "first.bin".to_owned(),
                    },
                    DirectoryEntry {
                        parent_id: 1,
                        object_id: 3,
                        kind: ObjectKind::File,
                        name: "second.bin".to_owned(),
                    },
                ];
                let mut directory_bytes = vec![0_u8; block_size as usize];
                for (index, entry) in entries.iter().enumerate() {
                    directory_bytes[index * 128..(index + 1) * 128]
                        .copy_from_slice(&entry.encode().unwrap());
                }
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();
                let map = ExtentMap::new(vec![Extent {
                    logical_start: 0,
                    physical_start: shared_data,
                    block_count: 1,
                }])
                .unwrap()
                .encode_block(block_size as usize)
                .unwrap();
                device.write_blocks(first_root, &map).unwrap();
                device.write_blocks(second_root, &map).unwrap();

                let namespace = volume
                    .reconstruct_namespace(
                        &mut device,
                        ObjectId(1),
                        &[metadata_block],
                        3,
                        &[directory_block],
                        2,
                    )
                    .unwrap();
                assert!(matches!(
                    volume.read_namespace_extent_maps(&mut device, &namespace),
                    Err(Error::Corrupt(
                        "file extents claim overlapping physical data blocks"
                    ))
                ));

                // Publication must also reject the alias before making an invalid
                // inventory durable.
                assert!(matches!(
                    volume.publish_persistent_inventory(
                        &mut device,
                        &PersistentInventoryDescriptor {
                            root_id: ObjectId(1),
                            metadata_blocks: vec![metadata_block],
                            metadata_record_count: 3,
                            directory_blocks: vec![directory_block],
                            directory_entry_count: 2,
                        },
                    ),
                    Err(Error::Corrupt(
                        "file extents claim overlapping physical data blocks"
                    ))
                ));

                // The public read/write APIs require a published inventory, so this
                // deliberately unpublished corrupt fixture cannot be used to exercise
                // those entry points. Whole-namespace alias rejection is covered above
                // by read_namespace_extent_maps and by refusing publication.
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn namespace_extent_map_root_checks_scale_across_all_supported_block_sizes() {
        use crate::{DirectoryEntry, MetadataRecord, Namespace, ObjectId, ObjectKind};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("many-extent-roots-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 512).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let file_count = 32_usize;
                let allocated = volume
                    .allocate_blocks(&mut device, (file_count * 2) as u64)
                    .unwrap();
                let mut namespace = Namespace::new(ObjectId(1)).unwrap();

                for (index, pair) in allocated.chunks_exact(2).enumerate() {
                    let object_id = ObjectId((index + 2) as u128);
                    let name = format!("file-{index:02}.bin");
                    let record = MetadataRecord {
                        id: object_id,
                        parent_id: 1,
                        kind: ObjectKind::File,
                        flags: 0,
                        size: u64::from(block_size),
                        extent_root_block: Some(pair[0]),
                        generation: 1,
                    };
                    namespace
                        .insert(
                            DirectoryEntry {
                                parent_id: 1,
                                object_id: object_id.0,
                                kind: ObjectKind::File,
                                name,
                            },
                            record,
                        )
                        .unwrap();
                    let map = ExtentMap::new(vec![Extent {
                        logical_start: 0,
                        physical_start: pair[1],
                        block_count: 1,
                    }])
                    .unwrap();
                    device
                        .write_blocks(pair[0], &map.encode_block(block_size as usize).unwrap())
                        .unwrap();
                }

                let maps = volume
                    .read_namespace_extent_maps(&mut device, &namespace)
                    .unwrap();
                assert_eq!(maps.len(), file_count, "block size {block_size}");

                // A data range that aliases a root late in the sorted root inventory must
                // still be rejected; this exercises the binary-search boundary, not just
                // the common case where roots and data are disjoint.
                let last_root = allocated[allocated.len() - 2];
                let first_root = allocated[0];
                let aliased = ExtentMap::new(vec![Extent {
                    logical_start: 0,
                    physical_start: first_root,
                    block_count: 1,
                }])
                .unwrap();
                device
                    .write_blocks(
                        last_root,
                        &aliased.encode_block(block_size as usize).unwrap(),
                    )
                    .unwrap();
                assert!(
                    matches!(
                        volume.read_namespace_extent_maps(&mut device, &namespace),
                        Err(Error::Corrupt("file data aliases an extent-map root"))
                    ),
                    "block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn namespace_extent_roots_must_be_allocated_and_not_alias_inventory_blocks() {
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};

        let path = image_path("namespace-extent-root-bitmap");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let directory_block = volume.allocate_block(&mut device).unwrap();
            let extent_root = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let file = MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 12,
                extent_root_block: Some(extent_root),
                generation: 1,
            };
            let entry = DirectoryEntry {
                parent_id: 1,
                object_id: 2,
                kind: ObjectKind::File,
                name: "hello.txt".to_owned(),
            };
            let mut metadata_bytes = vec![0_u8; 4096];
            metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
            metadata_bytes[80..160].copy_from_slice(&file.encode().unwrap());
            device
                .write_blocks(metadata_block, &metadata_bytes)
                .unwrap();
            let mut directory_bytes = vec![0_u8; 4096];
            directory_bytes[..128].copy_from_slice(&entry.encode().unwrap());
            device
                .write_blocks(directory_block, &directory_bytes)
                .unwrap();

            let namespace = volume
                .reconstruct_namespace(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    2,
                    &[directory_block],
                    1,
                )
                .unwrap();
            assert_eq!(
                volume
                    .validate_namespace_extent_roots(&mut device, &namespace)
                    .unwrap(),
                vec![extent_root]
            );

            volume.free_block(&mut device, extent_root).unwrap();
            assert!(matches!(
                volume.validate_namespace_extent_roots(&mut device, &namespace),
                Err(Error::Corrupt(
                    "extent-map root is marked free in the allocation bitmap"
                ))
            ));

            let alias_file = MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 12,
                extent_root_block: Some(metadata_block),
                generation: 1,
            };
            let mut aliased_metadata_bytes = vec![0_u8; 4096];
            aliased_metadata_bytes[..80].copy_from_slice(&root.encode().unwrap());
            aliased_metadata_bytes[80..160].copy_from_slice(&alias_file.encode().unwrap());
            device
                .write_blocks(metadata_block, &aliased_metadata_bytes)
                .unwrap();
            assert!(matches!(
                volume.reconstruct_namespace(
                    &mut device,
                    ObjectId(1),
                    &[metadata_block],
                    2,
                    &[directory_block],
                    1,
                ),
                Err(Error::Corrupt(
                    "extent-map root aliases a metadata or directory inventory block"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn persistent_inventory_reader_rejects_free_duplicate_and_aliased_blocks() {
        let path = image_path("persistent-inventory-invalid-blocks");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let allocated = volume.allocate_block(&mut device).unwrap();
            let free = allocated + 1;
            assert!(matches!(
                volume.read_metadata_inventory(&mut device, &[free], 1),
                Err(Error::Corrupt(
                    "inventory block is marked free in the bitmap"
                ))
            ));
            assert!(matches!(
                volume.read_metadata_inventory(&mut device, &[allocated, allocated], 52),
                Err(Error::Corrupt("inventory repeats a physical block"))
            ));
            assert!(matches!(
                volume.reconstruct_namespace(
                    &mut device,
                    crate::ObjectId(1),
                    &[allocated],
                    1,
                    &[allocated],
                    1,
                ),
                Err(Error::Corrupt(
                    "metadata and directory inventories share a physical block"
                ))
            ));
            assert!(matches!(
                volume.read_metadata_inventory(&mut device, &[0], 1),
                Err(Error::Corrupt("inventory block is outside the data region"))
            ));
            assert!(matches!(
                volume.read_metadata_inventory(&mut device, &[], 1),
                Err(Error::InvalidInput(
                    "inventory block list does not match its record count"
                ))
            ));
            assert!(matches!(
                volume.read_metadata_inventory(
                    &mut device,
                    &[],
                    usize::MAX / crate::METADATA_RECORD_SIZE,
                ),
                Err(Error::ArithmeticOverflow)
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn locator_update_refuses_to_rewrite_a_corrupt_superblock_checksum() {
        let path = image_path("locator-update-corrupt-superblock-checksum");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let backup_block = volume.volume.layout.backup_superblock;
            drop(volume);

            let mut backup = vec![0_u8; 4096];
            device.read_blocks(backup_block, &mut backup).unwrap();
            backup[CHECKSUM_OFFSET] ^= 1;
            device.write_blocks(backup_block, &backup).unwrap();
            device.flush().unwrap();

            let bytes_before = fs::read(&path).unwrap();
            assert!(matches!(
                super::persist_inventory_hint(&mut device, 42),
                Err(Error::Corrupt("superblock CRC32C mismatch"))
            ));
            assert_eq!(
                fs::read(&path).unwrap(),
                bytes_before,
                "a locator update must not normalize a damaged superblock checksum"
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn locator_update_rejects_primary_corruption_and_superblock_field_mismatch() {
        for damage in ["primary-checksum", "stable-field-mismatch"] {
            let path = image_path(&format!("locator-update-{damage}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                drop(volume);

                let mut primary = vec![0_u8; 4096];
                device.read_blocks(0, &mut primary).unwrap();
                let expected_error = if damage == "primary-checksum" {
                    primary[CHECKSUM_OFFSET] ^= 1;
                    "superblock CRC32C mismatch"
                } else {
                    // Keep the primary copy internally valid while changing a stable
                    // field that must match the independently checksummed backup.
                    primary[24] ^= 1;
                    let checksum = super::crc32c(&primary[..CHECKSUM_OFFSET]);
                    primary[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
                        .copy_from_slice(&checksum.to_le_bytes());
                    "primary and backup superblocks disagree before locator update"
                };
                device.write_blocks(0, &primary).unwrap();
                device.flush().unwrap();

                let bytes_before = fs::read(&path).unwrap();
                assert!(
                    matches!(
                        super::persist_inventory_hint(&mut device, 42),
                        Err(Error::Corrupt(message)) if message == expected_error
                    ),
                    "locator update must reject {damage}"
                );
                assert_eq!(
                    fs::read(&path).unwrap(),
                    bytes_before,
                    "locator update must not write either superblock after detecting {damage}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_superblock_has_stable_golden_bytes_and_checksum() {
        let uuid = *b"openfs-test-0001";
        let features = 0x0102_0304_0506_0708;
        let encoded = encode_superblock(4096, 256, features, uuid).unwrap();

        assert_eq!(
            &encoded[..48],
            &[
                79, 80, 69, 78, 70, 83, 0, 0, 4, 0, 48, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 8,
                7, 6, 5, 4, 3, 2, 1, 111, 112, 101, 110, 102, 115, 45, 116, 101, 115, 116, 45, 48,
                48, 48, 49
            ]
        );
        assert_eq!(
            u32::from_le_bytes(encoded[4088..4092].try_into().unwrap()),
            0x34a7_71a5
        );
        assert!(encoded[48..4088].iter().all(|byte| *byte == 0));
        assert!(encoded[4092..].iter().all(|byte| *byte == 0));
        assert_eq!(
            decode_superblock(&encoded).unwrap(),
            (4096, 256, features, uuid)
        );
    }

    #[test]
    fn version_two_volume_discovers_inventory_without_persistent_hint() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        let path = image_path("version-two-inventory-compatibility");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let metadata_block = volume.allocate_block(&mut device).unwrap();
            let root = MetadataRecord {
                id: ObjectId(1),
                parent_id: 0,
                kind: ObjectKind::Directory,
                flags: 0,
                size: 0,
                extent_root_block: None,
                generation: 1,
            };
            let mut metadata_bytes = vec![0; 4096];
            metadata_bytes[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
            device
                .write_blocks(metadata_block, &metadata_bytes)
                .unwrap();
            let descriptor = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![metadata_block],
                metadata_record_count: 1,
                directory_blocks: vec![],
                directory_entry_count: 0,
            };
            let descriptor_block = volume
                .publish_persistent_inventory(&mut device, &descriptor)
                .unwrap();
            drop(volume);

            // Reconstruct a version-2 layout: it has the original four-block redo ring
            // and no locator hint. The descriptor must still be found by the compatibility
            // scan, with the formerly version-4-only reserved blocks returned to the free set.
            let mut bitmap = vec![0_u8; 4096];
            device.read_blocks(1, &mut bitmap).unwrap();
            for block in 8_u64..20 {
                bitmap[(block / 8) as usize] &= !(1_u8 << (block % 8));
            }
            device.write_blocks(1, &bitmap).unwrap();
            for superblock in [0_u64, 255] {
                let mut bytes = vec![0_u8; 4096];
                device.read_blocks(superblock, &mut bytes).unwrap();
                bytes[8..10].copy_from_slice(&2_u16.to_le_bytes());
                bytes[48..56].fill(0);
                bytes[56..CHECKSUM_OFFSET].fill(0);
                let checksum = crate::crc32c(&bytes[..CHECKSUM_OFFSET]);
                bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
                    .copy_from_slice(&checksum.to_le_bytes());
                device.write_blocks(superblock, &bytes).unwrap();
            }
            device.flush().unwrap();

            let volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.format_version, 2);
            assert_eq!(
                volume.volume.layout,
                VolumeLayout::calculate_for_version(4096, 256, 2).unwrap()
            );
            assert_eq!(volume.volume.layout.data_start, 8);
            let found = volume
                .discover_persistent_inventory(&mut device)
                .unwrap()
                .unwrap();
            assert_eq!(found.0, descriptor_block);
            assert_eq!(found.1, descriptor);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_format_open_allocate_free_roundtrips_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("block-size-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 32).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                assert_eq!(second, first + 1);
                volume.free_block(&mut device, first).unwrap();
                device.flush().unwrap();
                drop(volume);

                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
                assert_eq!(volume.allocate_block(&mut device).unwrap(), second + 1);
                device.flush().unwrap();
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_journals_allocator_updates_across_reopen_and_keeps_legacy_separate() {
        let path = image_path("roundtrip");
        let first;
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            first = volume.allocate_block(&mut device).unwrap();
            assert!(first >= volume.volume.layout.data_start);
            assert!(volume.free_block(&mut device, 0).is_err());
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            assert!(matches!(
                crate::open_legacy_volume(&mut device),
                Err(Error::UnsupportedVersion(4))
            ));
            let mut volume = open_volume(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            assert_ne!(first, second);
            volume.free_block(&mut device, first).unwrap();
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_layout_bitmap_boundaries_keep_regions_disjoint() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let bits_per_bitmap_block = u64::from(block_size) * 8;
            for total_blocks in [
                22,
                bits_per_bitmap_block - 1,
                bits_per_bitmap_block,
                bits_per_bitmap_block + 1,
                bits_per_bitmap_block * 2 + 17,
            ] {
                let layout = VolumeLayout::calculate(block_size, total_blocks).unwrap();
                assert_eq!(
                    layout.bitmap_blocks,
                    total_blocks.div_ceil(bits_per_bitmap_block),
                    "block_size={block_size}, total_blocks={total_blocks}"
                );
                assert_eq!(layout.bitmap_start, 1);
                assert_eq!(
                    layout.journal.checkpoint_a,
                    layout.bitmap_start + layout.bitmap_blocks
                );
                assert_eq!(layout.journal.checkpoint_b, layout.journal.checkpoint_a + 1);
                assert_eq!(layout.journal.ring_start, layout.journal.checkpoint_b + 1);
                assert_eq!(
                    layout.data_start,
                    layout.journal.ring_start + layout.journal.ring_blocks
                );
                assert!(layout.data_start < layout.backup_superblock);
                assert_eq!(layout.data_blocks(), total_blocks - layout.data_start - 1);
                assert_eq!(layout.backup_superblock, total_blocks - 1);
            }
        }
    }

    #[test]
    fn canonical_allocator_rejects_mutated_public_layout_before_io() {
        for mutation in 0..10 {
            let path = image_path(&format!("mutated-layout-{mutation}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let original_layout = volume.volume.layout;
                // The one bitmap block and six journal blocks occupy blocks 1..8
                // for this 256-block, 4 KiB test volume.
                let mut allocator_metadata_before = vec![0; 7 * 4096];
                device
                    .read_blocks(1, &mut allocator_metadata_before)
                    .unwrap();

                // Every layout field is public, so guard every field against
                // post-validation changes before allocator I/O can occur.
                match mutation {
                    0 => volume.volume.layout.total_blocks += 1,
                    1 => volume.volume.layout.block_size = 8192,
                    2 => volume.volume.layout.bitmap_start += 1,
                    3 => volume.volume.layout.bitmap_blocks += 1,
                    4 => volume.volume.layout.journal.checkpoint_a += 1,
                    5 => volume.volume.layout.journal.checkpoint_b += 1,
                    6 => volume.volume.layout.journal.ring_start += 1,
                    7 => volume.volume.layout.journal.ring_blocks += 1,
                    8 => volume.volume.layout.data_start += 1,
                    9 => volume.volume.layout.backup_superblock -= 1,
                    _ => unreachable!(),
                }

                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput(
                        "volume layout was modified after validation"
                    ))
                ));
                assert!(matches!(
                    volume.free_block(&mut device, 8),
                    Err(Error::InvalidInput(
                        "volume layout was modified after validation"
                    ))
                ));

                let mut allocator_metadata_after = vec![0; 7 * 4096];
                device
                    .read_blocks(1, &mut allocator_metadata_after)
                    .unwrap();
                assert_eq!(
                    allocator_metadata_after, allocator_metadata_before,
                    "rejected operations modified bitmap or journal for field {mutation}"
                );

                let reopened = open_volume(&mut device).unwrap();
                assert_eq!(
                    reopened.volume.layout.total_blocks,
                    original_layout.total_blocks
                );
                assert_eq!(
                    reopened.volume.layout.block_size,
                    original_layout.block_size
                );
                assert_eq!(
                    reopened.volume.layout.bitmap_start,
                    original_layout.bitmap_start
                );
                assert_eq!(
                    reopened.volume.layout.bitmap_blocks,
                    original_layout.bitmap_blocks
                );
                assert_eq!(reopened.volume.layout.journal, original_layout.journal);
                assert_eq!(
                    reopened.volume.layout.data_start,
                    original_layout.data_start
                );
                assert_eq!(
                    reopened.volume.layout.backup_superblock,
                    original_layout.backup_superblock
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_allocator_rejects_device_geometry_mismatch_without_mutation() {
        let volume_path = image_path("geometry-volume");
        let other_path = image_path("geometry-other");
        {
            let mut original = ImageDevice::create(&volume_path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut original, *b"openfs-test-0001", 0).unwrap();
            let mut wrong_count = ImageDevice::create(&other_path, 4096, 257).unwrap();
            assert!(matches!(
                volume.allocate_block(&mut wrong_count),
                Err(Error::InvalidInput(
                    "device geometry does not match opened volume"
                ))
            ));
            assert!(matches!(
                volume.free_block(&mut wrong_count, volume.volume.layout.data_start),
                Err(Error::InvalidInput(
                    "device geometry does not match opened volume"
                ))
            ));

            let wrong_size_path = image_path("geometry-wrong-size");
            {
                let mut wrong_size = ImageDevice::create(&wrong_size_path, 8192, 256).unwrap();
                assert!(matches!(
                    volume.allocate_block(&mut wrong_size),
                    Err(Error::InvalidInput(
                        "device geometry does not match opened volume"
                    ))
                ));
                assert!(matches!(
                    volume.free_block(&mut wrong_size, volume.volume.layout.data_start),
                    Err(Error::InvalidInput(
                        "device geometry does not match opened volume"
                    ))
                ));
            }
            fs::remove_file(wrong_size_path).unwrap();

            let reopened = open_volume(&mut original).unwrap();
            assert_eq!(
                reopened.volume.layout.data_start,
                volume.volume.layout.data_start
            );
        }
        fs::remove_file(volume_path).unwrap();
        fs::remove_file(other_path).unwrap();
    }

    #[test]
    fn canonical_allocator_exhaustion_and_reuse_preserve_data_region_bounds() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("allocator-exhaustion-{block_size}"));
            {
                // The minimum valid canonical geometry has exactly three data blocks.
                let mut device = ImageDevice::create(&path, block_size, 24).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                assert_eq!(layout.data_blocks(), 3);

                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                let third = volume.allocate_block(&mut device).unwrap();
                assert_eq!([first, second, third], [20, 21, 22]);
                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));

                volume.free_block(&mut device, second).unwrap();
                assert_eq!(volume.allocate_block(&mut device).unwrap(), second);
                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));

                drop(volume);
                let mut reopened = open_volume(&mut device).unwrap();
                assert_eq!(reopened.volume.layout.data_start, 20);
                assert_eq!(reopened.volume.layout.backup_superblock, 23);
                assert!(matches!(
                    reopened.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));
                reopened.free_block(&mut device, first).unwrap();
                assert_eq!(reopened.allocate_block(&mut device).unwrap(), first);
            }
            fs::remove_file(path).unwrap();
        }
    }

    struct FailNthWrite<D> {
        inner: D,
        writes_until_failure: Option<usize>,
        flushes_until_failure: Option<usize>,
    }

    impl<D: BlockDevice> BlockDevice for FailNthWrite<D> {
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
            if let Some(remaining) = self.writes_until_failure.as_mut() {
                if *remaining == 0 {
                    self.writes_until_failure = None;
                    return Err(Error::Io(std::io::Error::other(
                        "injected batch-free journal write failure",
                    )));
                }
                *remaining -= 1;
            }
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            if self.flushes_until_failure == Some(0) {
                self.flushes_until_failure = None;
                return Err(Error::Io(std::io::Error::other(
                    "injected multi-page journal flush failure",
                )));
            }
            if let Some(remaining) = self.flushes_until_failure.as_mut() {
                *remaining -= 1;
            }
            self.inner.flush()
        }
    }

    #[test]
    fn allocator_batch_free_recovers_all_or_none_after_write_and_flush_failures() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for (fail_kind, fail_at) in (0..=6)
                .map(|index| ("write", index))
                .chain((0..=6).map(|index| ("flush", index)))
            {
                let path = image_path(&format!(
                    "batch-free-crash-{block_size}-{fail_kind}-{fail_at}"
                ));
                {
                    let image = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut device = FailNthWrite {
                        inner: image,
                        writes_until_failure: None,
                        flushes_until_failure: None,
                    };
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let blocks = [
                        volume.allocate_block(&mut device).unwrap(),
                        volume.allocate_block(&mut device).unwrap(),
                        volume.allocate_block(&mut device).unwrap(),
                    ];
                    match fail_kind {
                        "write" => device.writes_until_failure = Some(fail_at),
                        "flush" => device.flushes_until_failure = Some(fail_at),
                        _ => unreachable!("failure kinds are fixed"),
                    }

                    let result = volume.free_blocks(&mut device, &blocks);
                    assert!(
                        result.is_ok() || matches!(result, Err(Error::Io(_))),
                        "unexpected batch-free error at block size {block_size}, {fail_kind} {fail_at}: {result:?}"
                    );
                    drop(volume);
                    drop(device);

                    let mut reopened_device = ImageDevice::open(&path, block_size).unwrap();
                    let reopened = open_volume(&mut reopened_device).unwrap();
                    let states = blocks.map(|block| {
                        reopened
                            .is_data_block_allocated(&mut reopened_device, block)
                            .unwrap()
                    });
                    assert!(
                        states == [true, true, true] || states == [false, false, false],
                        "batch free must recover all blocks allocated or all free; block size {block_size}, {fail_kind} {fail_at}, states {states:?}"
                    );
                    drop(reopened);
                    drop(reopened_device);

                    // A second reopen must not replay recovery differently or change
                    // the all-or-none result established by the first recovery.
                    let mut second_device = ImageDevice::open(&path, block_size).unwrap();
                    let second_open = open_volume(&mut second_device).unwrap();
                    let second_states = blocks.map(|block| {
                        second_open
                            .is_data_block_allocated(&mut second_device, block)
                            .unwrap()
                    });
                    assert_eq!(
                        second_states, states,
                        "reopening after recovery must be idempotent at block size {block_size}, {fail_kind} {fail_at}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn allocator_batch_free_is_atomic_and_rejects_invalid_batches_without_mutation() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("allocator-batch-free-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                let third = volume.allocate_block(&mut device).unwrap();
                let free_before = volume.free_data_block_count(&mut device).unwrap();

                assert!(matches!(
                    volume.free_blocks(&mut device, &[first, first]),
                    Err(Error::InvalidInput("batch free list contains duplicates"))
                ));
                assert!(matches!(
                    volume.free_blocks(&mut device, &[first, u64::MAX]),
                    Err(Error::InvalidInput("batch free contains a non-data block"))
                ));
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before
                );

                volume
                    .free_blocks(&mut device, &[first, second, third])
                    .unwrap();
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_before + 3
                );
                drop(volume);

                let mut reopened = open_volume(&mut device).unwrap();
                let recovered = [
                    reopened.allocate_block(&mut device).unwrap(),
                    reopened.allocate_block(&mut device).unwrap(),
                    reopened.allocate_block(&mut device).unwrap(),
                ];
                assert_eq!(recovered, [first, second, third]);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_allocator_advances_to_next_bitmap_segment() {
        let bits_per_bitmap_block = 4096_u64 * 8;
        let path = image_path("allocator-bitmap-boundary");
        {
            let mut device = ImageDevice::create(&path, 4096, bits_per_bitmap_block + 12).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            assert_eq!(layout.bitmap_blocks, 2);

            // Occupy every data block represented by the first bitmap block.
            let mut bitmap = vec![0_u8; device.block_size() as usize];
            device
                .read_blocks(layout.bitmap_start, &mut bitmap)
                .unwrap();
            for block in layout.data_start..bits_per_bitmap_block {
                let local = block % bits_per_bitmap_block;
                bitmap[(local / 8) as usize] |= 1_u8 << (local % 8);
            }
            device.write_blocks(layout.bitmap_start, &bitmap).unwrap();
            device.flush().unwrap();

            let mut volume = open_volume(&mut device).unwrap();
            let allocated = volume.allocate_block(&mut device).unwrap();
            assert_eq!(allocated, bits_per_bitmap_block);
            volume.free_block(&mut device, allocated).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), allocated);
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                bits_per_bitmap_block + 1
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_allocator_rejects_double_free_and_out_of_range_blocks() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("invalid-free-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let allocated = volume.allocate_block(&mut device).unwrap();
                let first_data_block = volume.volume.layout.data_start;
                let backup_superblock = volume.volume.layout.backup_superblock;

                volume.free_block(&mut device, allocated).unwrap();
                assert!(matches!(
                    volume.free_block(&mut device, allocated),
                    Err(Error::InvalidInput("data block is already free"))
                ));

                // Probe every reserved block, both sides of the data interval,
                // and the integer limit. None may reach the journaled bitmap update.
                for invalid in (0..first_data_block).chain([backup_superblock, u64::MAX]) {
                    assert!(
                        matches!(
                            volume.free_block(&mut device, invalid),
                            Err(Error::InvalidInput(
                                "cannot free a reserved or out-of-range block"
                            ))
                        ),
                        "accepted invalid free of block {invalid} at block size {block_size}"
                    );
                }

                // Invalid free attempts must not damage the journal or bitmap.
                let mut reopened = open_volume(&mut device).unwrap();
                assert_eq!(reopened.journal.checkpoint().checkpoint_sequence, 8);
                assert_eq!(
                    reopened.allocate_block(&mut device).unwrap(),
                    allocated,
                    "invalid free attempts must not alter the freed block's bitmap state"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    struct FailReadAt<D> {
        inner: D,
        read_index: usize,
        fail_at: usize,
    }

    impl<D: openfs_block::BlockDevice> openfs_block::BlockDevice for FailReadAt<D> {
        fn block_size(&self) -> u32 {
            self.inner.block_size()
        }

        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }

        fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> openfs_core::Result<()> {
            let index = self.read_index;
            self.read_index += 1;
            if index == self.fail_at {
                return Err(Error::Io(std::io::Error::other(
                    "injected allocator bitmap-read failure",
                )));
            }
            self.inner.read_blocks(first_block, output)
        }

        fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> openfs_core::Result<()> {
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    struct FailWriteAt<D> {
        inner: D,
        write_index: usize,
        fail_at: usize,
    }

    impl<D: openfs_block::BlockDevice> openfs_block::BlockDevice for FailWriteAt<D> {
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
            let index = self.write_index;
            self.write_index += 1;
            if index == self.fail_at {
                return Err(Error::Io(std::io::Error::other(
                    "injected allocator home-write failure",
                )));
            }
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    struct FailFlushAt<D> {
        inner: D,
        flush_index: usize,
        fail_at: usize,
    }

    impl<D: openfs_block::BlockDevice> openfs_block::BlockDevice for FailFlushAt<D> {
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
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            let index = self.flush_index;
            self.flush_index += 1;
            if index == self.fail_at {
                return Err(Error::Io(std::io::Error::other(
                    "injected descriptor publication flush failure",
                )));
            }
            self.inner.flush()
        }
    }

    struct TearWriteAt<D> {
        inner: D,
        write_index: usize,
        fail_at: usize,
        prefix_bytes: usize,
    }

    impl<D: openfs_block::BlockDevice> openfs_block::BlockDevice for TearWriteAt<D> {
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
            let index = self.write_index;
            self.write_index += 1;
            if index != self.fail_at {
                return self.inner.write_blocks(first_block, input);
            }
            let block_size =
                usize::try_from(self.inner.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
            if input.len() != block_size || self.prefix_bytes >= input.len() {
                return Err(Error::InvalidInput(
                    "test tear must target one block with a shorter prefix",
                ));
            }
            let mut mixed = vec![0; block_size];
            self.inner.read_blocks(first_block, &mut mixed)?;
            mixed[..self.prefix_bytes].copy_from_slice(&input[..self.prefix_bytes]);
            self.inner.write_blocks(first_block, &mixed)?;
            Err(Error::Io(std::io::Error::other(
                "injected torn descriptor block write",
            )))
        }

        fn flush(&mut self) -> openfs_core::Result<()> {
            self.inner.flush()
        }
    }

    #[test]
    fn descriptor_publication_flush_failures_recover_old_or_complete_state() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..=4 {
                let path = image_path(&format!("descriptor-flush-failure-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let metadata_block = volume.allocate_block(&mut device).unwrap();
                    let root = MetadataRecord {
                        id: ObjectId(1),
                        parent_id: 0,
                        kind: ObjectKind::Directory,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let mut metadata_bytes = vec![0; block_size as usize];
                    metadata_bytes[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&root.encode().unwrap());
                    device
                        .write_blocks(metadata_block, &metadata_bytes)
                        .unwrap();
                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    let free_before = volume.free_data_block_count(&mut device).unwrap();
                    let mut failing = FailFlushAt {
                        inner: device,
                        flush_index: 0,
                        fail_at,
                    };
                    assert!(
                        matches!(
                            volume.publish_persistent_inventory(&mut failing, &descriptor),
                            Err(Error::Io(_))
                        ),
                        "flush {fail_at} should fail (block size {block_size})"
                    );
                    let mut device = failing.inner;
                    drop(volume);
                    let mut recovered = open_volume(&mut device).unwrap();
                    let found = recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap();
                    let free_after = recovered.free_data_block_count(&mut device).unwrap();
                    if fail_at <= 1 {
                        assert!(
                            found.is_none(),
                            "uncommitted descriptor became visible (block size {block_size}, flush {fail_at})"
                        );
                        assert_eq!(free_after, free_before);
                    } else {
                        let (block, decoded) = found.unwrap_or_else(|| {
                            panic!(
                                "committed descriptor was lost after flush failure (block size {block_size}, flush {fail_at})"
                            )
                        });
                        assert_eq!(decoded, descriptor);
                        assert!(recovered
                            .is_data_block_allocated(&mut device, block)
                            .unwrap());
                        assert_eq!(free_after, free_before - 1);
                        assert_eq!(
                            recovered
                                .publish_persistent_inventory(&mut device, &descriptor)
                                .unwrap(),
                            block,
                            "retry after an uncertain flush outcome must reuse the committed descriptor (block size {block_size}, flush {fail_at})"
                        );
                        assert_eq!(
                            recovered.free_data_block_count(&mut device).unwrap(),
                            free_after,
                            "retry must not allocate a duplicate descriptor (block size {block_size}, flush {fail_at})"
                        );
                    }

                    // Flush errors can also leave an uncertain commit outcome. Check
                    // that replaying the image again preserves the same old-or-complete state.
                    drop(recovered);
                    drop(device);
                    let mut device = ImageDevice::open(&path, block_size).unwrap();
                    let mut recovered = open_volume(&mut device).unwrap();
                    let rediscovered = recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap();
                    if fail_at <= 1 {
                        assert!(
                            rediscovered.is_none(),
                            "uncommitted descriptor became visible after a second reopen (block size {block_size}, flush {fail_at})"
                        );
                        assert_eq!(
                            recovered.free_data_block_count(&mut device).unwrap(),
                            free_before,
                            "uncommitted allocation changed after a second reopen (block size {block_size}, flush {fail_at})"
                        );
                    } else {
                        let (block, decoded) = rediscovered.unwrap_or_else(|| {
                            panic!(
                                "committed descriptor disappeared after a second reopen (block size {block_size}, flush {fail_at})"
                            )
                        });
                        assert_eq!(decoded, descriptor);
                        assert!(recovered
                            .is_data_block_allocated(&mut device, block)
                            .unwrap());
                        assert_eq!(
                            recovered.free_data_block_count(&mut device).unwrap(),
                            free_before - 1,
                            "committed allocation changed after a second reopen (block size {block_size}, flush {fail_at})"
                        );
                    }
                    // Retry after the second reopen as well. Before the commit point,
                    // this must finish publication; after it, it must remain idempotent.
                    let retried_block = recovered
                        .publish_persistent_inventory(&mut device, &descriptor)
                        .unwrap();
                    let (published_block, published_descriptor) = recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap()
                        .unwrap();
                    assert_eq!(retried_block, published_block);
                    assert_eq!(published_descriptor, descriptor);
                    assert_eq!(
                        recovered.free_data_block_count(&mut device).unwrap(),
                        free_before - 1,
                        "retry after a second reopen must leave exactly one descriptor allocation (block size {block_size}, flush {fail_at})"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn torn_descriptor_staging_writes_remain_invisible_and_safe_to_reuse() {
        use crate::{
            MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
            INVENTORY_DESCRIPTOR_MAGIC,
        };

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let prefixes = [
                0,
                INVENTORY_DESCRIPTOR_MAGIC.len().saturating_sub(1),
                INVENTORY_DESCRIPTOR_MAGIC.len(),
                (block_size as usize) / 2,
                (block_size as usize) - 1,
            ];
            for (case, prefix_bytes) in prefixes.into_iter().enumerate() {
                let path = image_path(&format!("descriptor-torn-stage-{block_size}-{case}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let metadata_block = volume.allocate_block(&mut device).unwrap();
                    let root = MetadataRecord {
                        id: ObjectId(1),
                        parent_id: 0,
                        kind: ObjectKind::Directory,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let mut metadata_bytes = vec![0; block_size as usize];
                    metadata_bytes[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&root.encode().unwrap());
                    device
                        .write_blocks(metadata_block, &metadata_bytes)
                        .unwrap();
                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    let free_before = volume.free_data_block_count(&mut device).unwrap();
                    let mut failing = TearWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at: 0,
                        prefix_bytes,
                    };
                    assert!(matches!(
                        volume.publish_persistent_inventory(&mut failing, &descriptor),
                        Err(Error::Io(_))
                    ));
                    let mut device = failing.inner;
                    drop(volume);
                    let mut recovered = open_volume(&mut device).unwrap();
                    assert!(recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap()
                        .is_none());
                    assert_eq!(
                        recovered.free_data_block_count(&mut device).unwrap(),
                        free_before
                    );
                    let reused = recovered.allocate_block(&mut device).unwrap();
                    let mut bytes = vec![0; block_size as usize];
                    device.read_blocks(reused, &mut bytes).unwrap();
                    assert!(!bytes.starts_with(INVENTORY_DESCRIPTOR_MAGIC));
                    assert!(recovered
                        .discover_persistent_inventory(&mut device)
                        .unwrap()
                        .is_none());
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_bitmap_read_failures_do_not_mutate_allocator_state() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("bitmap-read-failure-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let initial_checkpoint = volume.journal.checkpoint();

                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at: 0,
                };
                assert!(matches!(
                    volume.allocate_block(&mut failing),
                    Err(Error::Io(_))
                ));
                assert_eq!(
                    volume.journal.checkpoint(),
                    initial_checkpoint,
                    "failed bitmap read must not advance journal state (block size {block_size})"
                );
                let mut device = failing.inner;
                drop(volume);

                let mut volume = open_volume(&mut device).unwrap();
                let first = volume.allocate_block(&mut device).unwrap();
                let checkpoint_after_allocate = volume.journal.checkpoint();

                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at: 0,
                };
                assert!(matches!(
                    volume.free_block(&mut failing, first),
                    Err(Error::Io(_))
                ));
                assert_eq!(
                    volume.journal.checkpoint(),
                    checkpoint_after_allocate,
                    "failed free bitmap read must not advance journal state (block size {block_size})"
                );
                let mut device = failing.inner;
                drop(volume);

                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.allocate_block(&mut device).unwrap(),
                    first + 1,
                    "failed free must preserve the existing allocation (block size {block_size})"
                );
                volume.free_block(&mut device, first).unwrap();
                assert_eq!(
                    volume.allocate_block(&mut device).unwrap(),
                    first,
                    "block must be reusable after a later successful free (block size {block_size})"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_allocation_keeps_bitmap_unchanged_when_commit_fails_before_durable_record() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..5 {
                let path = image_path(&format!("allocation-precommit-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let first = volume.volume.layout.data_start;
                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at,
                    };
                    let mut volume = volume;
                    assert!(
                        matches!(volume.allocate_block(&mut failing), Err(Error::Io(_))),
                        "expected injected failure at write {fail_at}, block size {block_size}"
                    );
                    let mut device = failing.inner;
                    drop(volume);

                    let mut recovered = open_volume(&mut device).unwrap();
                    assert_eq!(
                        recovered.allocate_block(&mut device).unwrap(),
                        first,
                        "pre-commit failure at write {fail_at}, block size {block_size}, must not consume the block"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn canonical_allocation_recovers_after_durable_commit_and_home_write_failure() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in [5, 6] {
                let path = image_path(&format!("allocation-recovery-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let first = volume.volume.layout.data_start;
                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at,
                    };
                    let mut volume = volume;
                    assert!(
                        matches!(volume.allocate_block(&mut failing), Err(Error::Io(_))),
                        "expected injected failure at write {fail_at}, block size {block_size}"
                    );
                    let mut device = failing.inner;
                    drop(volume);

                    // Failures after the durable commit, including checkpoint write,
                    // must preserve the allocation and make recovery repeatable.
                    drop(open_volume(&mut device).unwrap());
                    let mut recovered = open_volume(&mut device).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
                    recovered.free_block(&mut device, first).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn refused_inventory_block_alias_preserves_exact_image_bytes_at_every_block_size() {
        use crate::{ObjectId, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("inventory-alias-no-write-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let owned_block = volume.allocate_block(&mut device).unwrap();
                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![owned_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![owned_block],
                    directory_entry_count: 1,
                };

                let image_len = block_size as usize * device.block_count() as usize;
                let mut before = vec![0_u8; image_len];
                device.read_blocks(0, &mut before).unwrap();

                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::InvalidInput(
                            "inventory descriptor contains duplicate physical blocks"
                        ))
                    ),
                    "cross-list block alias was not rejected during descriptor validation at block size {block_size}"
                );

                let mut after = vec![0_u8; image_len];
                device.read_blocks(0, &mut after).unwrap();
                assert_eq!(
                    after, before,
                    "rejected inventory alias modified the image at block size {block_size}"
                );
                assert!(
                    volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                    "rejected inventory alias must remain undiscoverable at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_rejects_primary_or_backup_superblock_mismatch_without_writes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let total_blocks = 32_u64;
            for (label, block) in [("primary", 0_u64), ("backup", total_blocks - 1)] {
                let path = image_path(&format!("superblock-mismatch-{label}-{block_size}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, total_blocks).unwrap();
                    format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();

                    let mut bytes = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(block, &mut bytes).unwrap();
                    bytes[32] ^= 0x01;
                    device.write_blocks(block, &bytes).unwrap();
                    device.flush().unwrap();

                    let image_len = device.block_size() as usize * device.block_count() as usize;
                    let mut before = vec![0_u8; image_len];
                    device.read_blocks(0, &mut before).unwrap();

                    assert!(
                        matches!(open_volume(&mut device), Err(Error::Corrupt(_))),
                        "mismatch was not rejected for {label} at block size {block_size}"
                    );

                    let mut after = vec![0_u8; image_len];
                    device.read_blocks(0, &mut after).unwrap();
                    assert_eq!(
                        after, before,
                        "rejected open modified the image for {label} at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_rejects_matching_superblocks_with_bad_checksum() {
        let path = image_path("superblock-bad-checksum");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();

            for block in [0_u64, 255] {
                let mut bytes = vec![0_u8; device.block_size() as usize];
                device.read_blocks(block, &mut bytes).unwrap();
                bytes[40] ^= 0x01;
                device.write_blocks(block, &bytes).unwrap();
            }
            device.flush().unwrap();

            assert!(matches!(
                open_volume(&mut device),
                Err(Error::Corrupt("superblock CRC32C mismatch"))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_rejects_nonzero_reserved_superblock_bytes_with_valid_checksum() {
        for (label, offset) in [
            ("superblock-reserved-header-byte", 56_usize),
            ("superblock-reserved-tail-byte", 4092_usize),
        ] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();

                for block in [0_u64, 255] {
                    let mut bytes = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(block, &mut bytes).unwrap();
                    bytes[offset] = 1;
                    let checksum = crate::crc32c(&bytes[..4088]);
                    bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                    device.write_blocks(block, &bytes).unwrap();
                }
                device.flush().unwrap();

                assert!(matches!(
                    open_volume(&mut device),
                    Err(Error::Corrupt("reserved superblock bytes must be zero"))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_rejects_invalid_superblock_headers_and_geometry() {
        let cases: [(&str, usize, &[u8], &'static str); 2] = [
            ("bad-magic", 0, b"BADMAGIC", "bad superblock magic"),
            (
                "bad-header-size",
                10,
                &[0x31, 0x00],
                "unexpected superblock header size",
            ),
        ];

        for (label, offset, replacement, expected) in cases {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                for block in [0_u64, 255] {
                    let mut bytes = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(block, &mut bytes).unwrap();
                    bytes[offset..offset + replacement.len()].copy_from_slice(replacement);
                    if offset != 0 {
                        let checksum = crate::crc32c(&bytes[..4088]);
                        bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                    }
                    device.write_blocks(block, &bytes).unwrap();
                }
                device.flush().unwrap();
                assert!(matches!(
                    open_volume(&mut device),
                    Err(Error::Corrupt(message)) if message == expected
                ));
            }
            fs::remove_file(path).unwrap();
        }

        let path = image_path("unsupported-version");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            for block in [0_u64, 255] {
                let mut bytes = vec![0_u8; device.block_size() as usize];
                device.read_blocks(block, &mut bytes).unwrap();
                bytes[8..10].copy_from_slice(&99_u16.to_le_bytes());
                let checksum = crate::crc32c(&bytes[..4088]);
                bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                device.write_blocks(block, &bytes).unwrap();
            }
            device.flush().unwrap();
            assert!(matches!(
                open_volume(&mut device),
                Err(Error::UnsupportedVersion(99))
            ));
        }
        fs::remove_file(path).unwrap();

        for (label, offset, encoded) in [
            (
                "invalid-block-size",
                12_usize,
                3000_u32.to_le_bytes().to_vec(),
            ),
            (
                "invalid-block-count",
                16_usize,
                1_u64.to_le_bytes().to_vec(),
            ),
        ] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                for block in [0_u64, 255] {
                    let mut bytes = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(block, &mut bytes).unwrap();
                    bytes[offset..offset + encoded.len()].copy_from_slice(&encoded);
                    let checksum = crate::crc32c(&bytes[..4088]);
                    bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                    device.write_blocks(block, &bytes).unwrap();
                }
                device.flush().unwrap();
                assert!(matches!(
                    open_volume(&mut device),
                    Err(Error::Corrupt("invalid superblock geometry"))
                ));
            }
            fs::remove_file(path).unwrap();
        }

        // A checksum-valid superblock must still reject byte-size overflow
        // before comparing its claimed geometry with the backing device.
        let path = image_path("overflowing-byte-geometry");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            for block in [0_u64, 255] {
                let mut bytes = vec![0_u8; device.block_size() as usize];
                device.read_blocks(block, &mut bytes).unwrap();
                bytes[16..24].copy_from_slice(&u64::MAX.to_le_bytes());
                let checksum = crate::crc32c(&bytes[..4088]);
                bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                device.write_blocks(block, &bytes).unwrap();
            }
            device.flush().unwrap();
            assert!(matches!(
                open_volume(&mut device),
                Err(Error::ArithmeticOverflow)
            ));
        }
        fs::remove_file(path).unwrap();

        for (label, offset, value) in [
            ("block-size-mismatch", 12_usize, 8192_u64),
            ("block-count-mismatch", 16_usize, 255_u64),
        ] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                for block in [0_u64, 255] {
                    let mut bytes = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(block, &mut bytes).unwrap();
                    let encoded = if offset == 12 {
                        (value as u32).to_le_bytes().to_vec()
                    } else {
                        value.to_le_bytes().to_vec()
                    };
                    bytes[offset..offset + encoded.len()].copy_from_slice(&encoded);
                    let checksum = crate::crc32c(&bytes[..4088]);
                    bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
                    device.write_blocks(block, &bytes).unwrap();
                }
                device.flush().unwrap();
                assert!(matches!(
                    open_volume(&mut device),
                    Err(Error::Corrupt(
                        "superblock geometry does not match device geometry"
                    ))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_rejects_reserved_and_padding_bitmap_corruption_on_open() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let bits_per_bitmap_block = u64::from(block_size) * 8;
            for (label, bit) in [
                ("reserved-bit", 0_u64),
                ("first-padding-bit", 256_u64),
                ("last-padding-bit", bits_per_bitmap_block - 1),
            ] {
                let path = image_path(&format!("{label}-{block_size}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let bitmap_block =
                        volume.volume.layout.bitmap_start + bit / bits_per_bitmap_block;
                    let local = bit % bits_per_bitmap_block;
                    let mut bitmap = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
                    device.write_blocks(bitmap_block, &bitmap).unwrap();
                    device.flush().unwrap();

                    assert!(
                        matches!(
                            open_volume(&mut device),
                            Err(Error::Corrupt(
                                "allocation bitmap marks a reserved or padding block free"
                            ))
                        ),
                        "{label} corruption was accepted for block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_rejects_backup_and_padding_corruption_in_later_bitmap_segment() {
        // 32,770 blocks at 4 KiB require two bitmap blocks. The backup
        // superblock is the final in-range bit; the next bit is padding.
        let total_blocks = 32_770_u64;
        let bits_per_bitmap_block = 4096_u64 * 8;
        for (label, corrupted_block) in [
            ("backup-superblock", total_blocks - 1),
            ("padding-after-backup", total_blocks),
        ] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, total_blocks).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                assert_eq!(volume.volume.layout.bitmap_blocks, 2);

                let bitmap_index = corrupted_block / bits_per_bitmap_block;
                let local = corrupted_block % bits_per_bitmap_block;
                assert_eq!(bitmap_index, 1);
                let bitmap_block = volume.volume.layout.bitmap_start + bitmap_index;
                let mut bitmap = vec![0_u8; device.block_size() as usize];
                device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
                device.write_blocks(bitmap_block, &bitmap).unwrap();
                device.flush().unwrap();

                assert!(
                    matches!(
                        open_volume(&mut device),
                        Err(Error::Corrupt(
                            "allocation bitmap marks a reserved or padding block free"
                        ))
                    ),
                    "accepted {label} corruption in second bitmap segment"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_rejects_each_reserved_block_marked_free() {
        let reserved_blocks = (0..256_u64)
            .filter(|block| {
                VolumeLayout::calculate(4096, 256)
                    .unwrap()
                    .is_reserved(*block)
            })
            .collect::<Vec<_>>();
        assert_eq!(
            reserved_blocks,
            (0..20).chain(std::iter::once(255)).collect::<Vec<_>>()
        );

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let bits_per_bitmap_block = u64::from(block_size) * 8;
            for reserved in &reserved_blocks {
                let path = image_path(&format!("reserved-block-{reserved}-{block_size}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let bitmap_block =
                        volume.volume.layout.bitmap_start + *reserved / bits_per_bitmap_block;
                    let local = *reserved % bits_per_bitmap_block;
                    let mut bitmap = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
                    device.write_blocks(bitmap_block, &bitmap).unwrap();
                    device.flush().unwrap();

                    assert!(
                        matches!(
                            open_volume(&mut device),
                            Err(Error::Corrupt(
                                "allocation bitmap marks a reserved or padding block free"
                            ))
                        ),
                        "reserved block {reserved} was accepted as free at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_allocator_reuses_freed_blocks_in_order_after_reopen() {
        let path = image_path("allocator-reuse-order");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first = volume.volume.layout.data_start;
            let allocated = (0..40)
                .map(|_| volume.allocate_block(&mut device).unwrap())
                .collect::<Vec<_>>();
            assert_eq!(allocated, (first..first + 40).collect::<Vec<_>>());

            for block in allocated.iter().step_by(2) {
                volume.free_block(&mut device, *block).unwrap();
            }
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume(&mut device).unwrap();
            for block in allocated.iter().step_by(2) {
                assert_eq!(volume.allocate_block(&mut device).unwrap(), *block);
            }
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                first + allocated.len() as u64
            );
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                first + allocated.len() as u64 + 1
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_rejects_each_reserved_bitmap_corruption_replayed_from_journal() {
        // Exercise every reserved bit through durable journal replay, not just
        // the primary superblock bit. A minimum-size volume keeps this matrix cheap.
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let layout = VolumeLayout::calculate(block_size, 24).unwrap();
            let mut corruptions = (0..layout.total_blocks)
                .filter(|block| layout.is_reserved(*block))
                .map(|block| ("reserved", block))
                .collect::<Vec<_>>();
            // Include the first padding bit in the same durable-replay matrix.
            corruptions.push(("padding", layout.total_blocks));
            for (kind, corrupted_block) in corruptions {
                let path = image_path(&format!("{kind}-replay-{block_size}-{corrupted_block}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 24).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let bits_per_bitmap_block = u64::from(block_size) * 8;
                    let bitmap_block =
                        volume.volume.layout.bitmap_start + corrupted_block / bits_per_bitmap_block;
                    let local = corrupted_block % bits_per_bitmap_block;
                    let mut bitmap = vec![0_u8; device.block_size() as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));

                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at: 4, // Commit is durable; the home-block write fails.
                    };
                    let mut volume = volume;
                    assert!(matches!(
                        volume.journal.commit(
                            &mut failing,
                            &[crate::RedoUpdate {
                                target_block: bitmap_block,
                                contents: bitmap,
                            }],
                        ),
                        Err(Error::Io(_))
                    ));
                    let mut device = failing.inner;
                    drop(volume);

                    assert!(
                        matches!(
                            open_volume(&mut device),
                            Err(Error::Corrupt(
                                "allocation bitmap marks a reserved or padding block free"
                            ))
                        ),
                        "replay accepted {kind} bit {corrupted_block} as free for block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_rejects_later_segment_reserved_and_padding_corruption_replayed_from_journal() {
        let total_blocks = 4096_u64 * 8 + 2;
        let bits_per_bitmap_block = 4096_u64 * 8;
        // The same post-replay validation must reject both the backup-superblock
        // bit and padding immediately beyond the volume in a later bitmap segment.
        for (label, corrupted_block) in [
            ("backup-superblock", total_blocks - 1),
            ("padding-after-volume", total_blocks),
        ] {
            let path = image_path(&format!("later-segment-{label}-bitmap-replay"));
            {
                let mut device = ImageDevice::create(&path, 4096, total_blocks).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                assert_eq!(volume.volume.layout.bitmap_blocks, 2);

                let bitmap_index = corrupted_block / bits_per_bitmap_block;
                let bitmap_block = volume.volume.layout.bitmap_start + bitmap_index;
                let local = corrupted_block % bits_per_bitmap_block;
                assert_eq!(bitmap_index, 1);
                let mut bitmap = vec![0_u8; device.block_size() as usize];
                device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));

                let mut failing = FailWriteAt {
                    inner: device,
                    write_index: 0,
                    fail_at: 4,
                };
                let mut volume = volume;
                assert!(matches!(
                    volume.journal.commit(
                        &mut failing,
                        &[crate::RedoUpdate {
                            target_block: bitmap_block,
                            contents: bitmap,
                        }],
                    ),
                    Err(Error::Io(_))
                ));
                let mut device = failing.inner;
                drop(volume);

                assert!(
                    matches!(
                        open_volume(&mut device),
                        Err(Error::Corrupt(
                            "allocation bitmap marks a reserved or padding block free"
                        ))
                    ),
                    "replay must reject {label} corruption in the second bitmap segment"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_free_keeps_allocation_when_commit_fails_before_durable_record() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..4 {
                let path = image_path(&format!("free-precommit-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let first = volume.allocate_block(&mut device).unwrap();

                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at,
                    };
                    assert!(
                        matches!(volume.free_block(&mut failing, first), Err(Error::Io(_))),
                        "expected injected failure at write {fail_at}, block size {block_size}"
                    );
                    let mut device = failing.inner;
                    drop(volume);

                    let mut recovered = open_volume(&mut device).unwrap();
                    assert_eq!(
                        recovered.allocate_block(&mut device).unwrap(),
                        first + 1,
                        "pre-commit free failure at write {fail_at}, block size {block_size}, must preserve allocation"
                    );
                    recovered.free_block(&mut device, first).unwrap();
                    assert_eq!(
                        recovered.allocate_block(&mut device).unwrap(),
                        first,
                        "preserved block must be reusable after successful free"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn canonical_free_recovers_after_durable_commit_and_home_write_failure() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in [4, 5] {
                let path = image_path(&format!("free-recovery-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let first = volume.allocate_block(&mut device).unwrap();

                    let mut failing = FailWriteAt {
                        inner: device,
                        write_index: 0,
                        fail_at,
                    };
                    assert!(
                        matches!(volume.free_block(&mut failing, first), Err(Error::Io(_))),
                        "expected injected failure at write {fail_at}, block size {block_size}"
                    );
                    let mut device = failing.inner;
                    drop(volume);

                    // Recovery must be repeatable after both home-write and
                    // checkpoint-write failures following a durable free.
                    drop(open_volume(&mut device).unwrap());
                    let mut recovered = open_volume(&mut device).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn canonical_format_allocate_and_reopen_through_raw_device_backend() {
        let path = image_path("raw-device-roundtrip");
        {
            // A disposable regular file exercises RawDevice's exact-geometry path
            // without touching a physical disk or USB device.
            let image = ImageDevice::create(&path, 4096, 256).unwrap();
            drop(image);
        }

        let first;
        {
            let mut device = RawDevice::open(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            first = volume.allocate_block(&mut device).unwrap();
            device.flush().unwrap();
        }
        {
            let mut device = RawDevice::open(&path, 4096, 256).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            assert_ne!(first, second);
            volume.free_block(&mut device, first).unwrap();
            device.flush().unwrap();
        }
        {
            let mut device = RawDevice::open(&path, 4096, 256).unwrap();
            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_minimum_volume_exhausts_and_reuses_only_data_blocks() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("minimum-volume-exhaustion-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 24).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                assert_eq!(layout.data_blocks(), 3);

                let allocated: Vec<_> = (0..layout.data_blocks())
                    .map(|_| volume.allocate_block(&mut device).unwrap())
                    .collect();
                assert_eq!(allocated, vec![20, 21, 22]);
                let checkpoint_when_full = volume.journal.checkpoint().checkpoint_sequence;
                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));
                assert_eq!(
                    volume.journal.checkpoint().checkpoint_sequence,
                    checkpoint_when_full,
                    "failed allocation on a full volume must not commit a journal transaction"
                );

                volume.free_block(&mut device, allocated[1]).unwrap();
                assert_eq!(volume.allocate_block(&mut device).unwrap(), allocated[1]);
                let checkpoint_when_full_again = volume.journal.checkpoint().checkpoint_sequence;
                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));
                assert_eq!(
                    volume.journal.checkpoint().checkpoint_sequence,
                    checkpoint_when_full_again,
                    "repeated exhaustion must not mutate journal state"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_allocator_handles_last_data_block_after_bitmap_segment_boundary() {
        for block_size in [4096_u32, 8192] {
            let total_blocks = 65_537_u64;
            let path = image_path(&format!("last-data-block-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, total_blocks).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                assert!(layout.bitmap_blocks > 1);
                let last_data = layout.backup_superblock - 1;
                let bits_per_block = u64::from(block_size) * 8;

                // Mark every data block allocated except the final legal data block.
                // Keep the on-disk reserved/padding bits produced by the formatter.
                for bitmap_index in 0..layout.bitmap_blocks {
                    let bitmap_block = layout.bitmap_start + bitmap_index;
                    let bitmap_base = bitmap_index * bits_per_block;
                    let first = layout.data_start.max(bitmap_base);
                    let end = last_data.min(bitmap_base + bits_per_block);
                    let mut bitmap = vec![0_u8; block_size as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    for block in first..end {
                        let local = block - bitmap_base;
                        bitmap[(local / 8) as usize] |= 1 << (local % 8);
                    }
                    device.write_blocks(bitmap_block, &bitmap).unwrap();
                }
                let mut volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.allocate_block(&mut device).unwrap(),
                    last_data,
                    "allocator must return the final valid data block (block size {block_size})"
                );
                assert!(matches!(
                    volume.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));
                volume.free_block(&mut device, last_data).unwrap();
                drop(volume);

                let mut reopened = open_volume(&mut device).unwrap();
                assert_eq!(
                    reopened.allocate_block(&mut device).unwrap(),
                    last_data,
                    "last data block must remain reusable after reopen (block size {block_size})"
                );
                assert!(matches!(
                    reopened.allocate_block(&mut device),
                    Err(Error::InvalidInput("volume has no free data blocks"))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_later_bitmap_segment_read_failures_preserve_allocator_state() {
        for block_size in [4096_u32] {
            let path = image_path(&format!("later-bitmap-read-failure-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 65_537).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                let bits_per_block = u64::from(block_size) * 8;
                assert!(layout.bitmap_blocks >= 2);

                // Consume every data block represented by the first bitmap segment.
                let first_end = bits_per_block.min(layout.backup_superblock);
                let mut bitmap = vec![0_u8; block_size as usize];
                device
                    .read_blocks(layout.bitmap_start, &mut bitmap)
                    .unwrap();
                for block in layout.data_start..first_end {
                    let bit = block;
                    bitmap[(bit / 8) as usize] |= 1 << (bit % 8);
                }
                device.write_blocks(layout.bitmap_start, &bitmap).unwrap();
                device.flush().unwrap();

                let mut volume = open_volume(&mut device).unwrap();
                let initial_checkpoint = volume.journal.checkpoint();
                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at: 1,
                };
                assert!(matches!(
                    volume.allocate_block(&mut failing),
                    Err(Error::Io(_))
                ));
                assert_eq!(
                    volume.journal.checkpoint(),
                    initial_checkpoint,
                    "failure reading a later bitmap segment must not advance the journal (block size {block_size})"
                );
                let mut device = failing.inner;
                drop(volume);

                let mut recovered = open_volume(&mut device).unwrap();
                let later_block = recovered.allocate_block(&mut device).unwrap();
                assert!(later_block >= bits_per_block);
                let checkpoint_after_allocate = recovered.journal.checkpoint();

                let mut failing = FailReadAt {
                    inner: device,
                    read_index: 0,
                    fail_at: 0,
                };
                assert!(matches!(
                    recovered.free_block(&mut failing, later_block),
                    Err(Error::Io(_))
                ));
                assert_eq!(
                    recovered.journal.checkpoint(),
                    checkpoint_after_allocate,
                    "failed later-segment free must not advance the journal (block size {block_size})"
                );
                let mut device = failing.inner;
                drop(recovered);

                let mut recovered = open_volume(&mut device).unwrap();
                assert_eq!(
                    recovered.allocate_block(&mut device).unwrap(),
                    later_block + 1,
                    "failed free must preserve the later-segment allocation (block size {block_size})"
                );
                recovered.free_block(&mut device, later_block).unwrap();
                assert_eq!(
                    recovered.allocate_block(&mut device).unwrap(),
                    later_block,
                    "later-segment block must be reusable after a successful free (block size {block_size})"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_allocator_queries_report_free_space_and_block_state() {
        let path = image_path("allocator-state-queries");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first_data = volume.volume.layout.data_start;
            let initial_free = volume.free_data_block_count(&mut device).unwrap();
            assert_eq!(initial_free, volume.volume.layout.data_blocks());
            assert!(!volume
                .is_data_block_allocated(&mut device, first_data)
                .unwrap());

            let allocated = volume.allocate_block(&mut device).unwrap();
            assert_eq!(allocated, first_data);
            assert!(volume
                .is_data_block_allocated(&mut device, allocated)
                .unwrap());
            assert_eq!(
                volume.free_data_block_count(&mut device).unwrap(),
                initial_free - 1
            );

            volume.free_block(&mut device, allocated).unwrap();
            assert!(!volume
                .is_data_block_allocated(&mut device, allocated)
                .unwrap());
            assert_eq!(
                volume.free_data_block_count(&mut device).unwrap(),
                initial_free
            );
            assert!(matches!(
                volume.is_data_block_allocated(&mut device, 0),
                Err(Error::InvalidInput(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_contiguous_allocator_is_atomic_and_persistent() {
        let path = image_path("contiguous-allocator");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first = volume.volume.layout.data_start;
            let initial_free = volume.free_data_block_count(&mut device).unwrap();

            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 3).unwrap(),
                Some(first)
            );
            let allocated = volume.allocate_contiguous_blocks(&mut device, 3).unwrap();
            assert_eq!(allocated, first);
            for block in first..first + 3 {
                assert!(volume.is_data_block_allocated(&mut device, block).unwrap());
            }
            assert_eq!(
                volume.free_data_block_count(&mut device).unwrap(),
                initial_free - 3
            );
            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 3).unwrap(),
                Some(first + 3)
            );
            volume.validate_allocation_bitmap(&mut device).unwrap();
            drop(volume);

            let mut reopened = open_volume(&mut device).unwrap();
            for block in first..first + 3 {
                assert!(reopened
                    .is_data_block_allocated(&mut device, block)
                    .unwrap());
            }
            reopened.free_block(&mut device, first + 1).unwrap();
            assert_eq!(
                reopened
                    .find_contiguous_free_blocks(&mut device, 2)
                    .unwrap(),
                Some(first + 3)
            );
            assert!(matches!(
                reopened.allocate_contiguous_blocks(&mut device, 0),
                Err(Error::InvalidInput(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_contiguous_allocator_rejects_unrepresentable_extents_without_mutation() {
        let path = image_path("contiguous-allocator-limits");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let before = volume.journal.checkpoint();
            assert!(matches!(
                volume.find_contiguous_free_blocks(&mut device, 0),
                Err(Error::InvalidInput(_))
            ));
            assert!(matches!(
                volume.allocate_contiguous_blocks(&mut device, 4096_u64 * 8 + 1),
                Err(Error::InvalidInput(_))
            ));
            assert_eq!(volume.journal.checkpoint(), before);
            let first = volume.volume.layout.data_start;
            assert_eq!(
                volume.allocate_contiguous_blocks(&mut device, 3).unwrap(),
                first
            );
            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 3).unwrap(),
                Some(first + 3)
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_batch_allocator_and_free_are_atomic_and_persistent() {
        let path = image_path("batch-allocator");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first = volume.volume.layout.data_start;
            let before = volume.allocation_stats(&mut device).unwrap();
            assert_eq!(
                before.total_data_blocks,
                before.allocated_data_blocks + before.free_data_blocks
            );

            let blocks = volume.allocate_blocks(&mut device, 4).unwrap();
            assert_eq!(blocks.len(), 4);
            assert!(blocks.windows(2).all(|pair| pair[0] < pair[1]));
            assert_eq!(blocks[0], first);
            let after_allocate = volume.allocation_stats(&mut device).unwrap();
            assert_eq!(after_allocate.free_data_blocks, before.free_data_blocks - 4);
            assert_eq!(
                after_allocate.allocated_data_blocks,
                before.allocated_data_blocks + 4
            );

            assert!(matches!(
                volume.free_blocks(&mut device, &[blocks[0], blocks[0]]),
                Err(Error::InvalidInput(_))
            ));
            assert!(matches!(
                volume.free_blocks(
                    &mut device,
                    &[blocks[0], volume.volume.layout.backup_superblock]
                ),
                Err(Error::InvalidInput(_))
            ));
            volume.free_blocks(&mut device, &blocks).unwrap();
            assert_eq!(volume.allocation_stats(&mut device).unwrap(), before);
            drop(volume);

            let mut reopened = open_volume(&mut device).unwrap();
            for block in &blocks {
                assert!(!reopened
                    .is_data_block_allocated(&mut device, *block)
                    .unwrap());
            }
            let after_reopen = reopened.allocate_blocks(&mut device, 4).unwrap();
            assert_eq!(after_reopen, blocks);
            assert!(matches!(
                reopened.allocate_blocks(&mut device, 0),
                Err(Error::InvalidInput(_))
            ));
            assert!(matches!(
                reopened.free_blocks(&mut device, &[]),
                Err(Error::InvalidInput(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_bitmap_validator_rejects_padding_corruption_in_second_segment() {
        // 4096-byte bitmap blocks cover 32768 volume blocks each. Keep the
        // image sparse so this regression exercises the second segment without
        // requiring a large physical test fixture.
        let path = image_path("bitmap-validator-second-segment-padding");
        {
            let mut device = ImageDevice::create(&path, 4096, 32_770).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            volume.validate_allocation_bitmap(&mut device).unwrap();

            let corrupt_block = 32_770_u64;
            let bitmap_index = corrupt_block / (4096_u64 * 8);
            let local = corrupt_block % (4096_u64 * 8);
            assert_eq!(bitmap_index, 1);
            let bitmap_block = layout.bitmap_start + bitmap_index;
            let mut bitmap = vec![0_u8; 4096];
            device.read_blocks(bitmap_block, &mut bitmap).unwrap();
            bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
            device.write_blocks(bitmap_block, &bitmap).unwrap();

            assert!(matches!(
                volume.validate_allocation_bitmap(&mut device),
                Err(Error::Corrupt(_))
            ));
            assert!(matches!(
                volume.allocated_data_block_inventory(&mut device),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_public_bitmap_validator_rejects_reserved_and_padding_corruption_for_all_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for (label, corrupt_block) in [("reserved", 0_u64), ("padding", 300_u64)] {
                let path = image_path(&format!("bitmap-validator-{block_size}-{label}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                    let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    volume.validate_allocation_bitmap(&mut device).unwrap();

                    let bits_per_bitmap_block = u64::from(block_size) * 8;
                    let bitmap_index = corrupt_block / bits_per_bitmap_block;
                    let local = corrupt_block % bits_per_bitmap_block;
                    let bitmap_block = volume.volume.layout.bitmap_start + bitmap_index;
                    let mut bitmap = vec![0_u8; block_size as usize];
                    device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                    bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
                    device.write_blocks(bitmap_block, &bitmap).unwrap();

                    assert!(
                        matches!(
                            volume.validate_allocation_bitmap(&mut device),
                            Err(Error::Corrupt(_))
                        ),
                        "validator accepted {label} corruption at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn canonical_batch_allocator_roundtrips_all_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("batch-roundtrip-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let initial = volume.allocation_stats(&mut device).unwrap();
                let blocks = volume.allocate_blocks(&mut device, 4).unwrap();
                assert_eq!(blocks.len(), 4);
                assert_eq!(
                    volume
                        .allocation_stats(&mut device)
                        .unwrap()
                        .free_data_blocks,
                    initial.free_data_blocks - 4
                );
                volume.free_blocks(&mut device, &blocks).unwrap();
                assert_eq!(volume.allocation_stats(&mut device).unwrap(), initial);
                volume.validate_allocation_bitmap(&mut device).unwrap();
                drop(volume);

                let mut reopened = open_volume(&mut device).unwrap();
                assert_eq!(reopened.allocation_stats(&mut device).unwrap(), initial);
                let allocated_again = reopened.allocate_blocks(&mut device, 4).unwrap();
                assert_eq!(allocated_again, blocks);
                reopened.free_blocks(&mut device, &allocated_again).unwrap();
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_read_only_checker_reports_allocator_state_and_rejects_mirror_mismatch() {
        let path = image_path("read-only-checker");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            device.flush().unwrap();
            let bytes_before_check = fs::read(&path).unwrap();
            let initial = check_volume(&mut device).unwrap();
            assert_eq!(
                fs::read(&path).unwrap(),
                bytes_before_check,
                "read-only checker must not modify the image"
            );
            assert_eq!(initial.data_blocks, initial.free_data_blocks);
            assert_eq!(initial.allocated_data_blocks, 0);
            assert_eq!(initial.journal_generation, 2);
            assert_eq!(initial.journal_checkpoint_sequence, 0);
            assert_eq!(initial.pending_journal_records, 0);
            assert_eq!(initial.recoverable_transactions, 0);

            volume.allocate_blocks(&mut device, 3).unwrap();
            let after_allocate = check_volume(&mut device).unwrap();
            assert_eq!(after_allocate.block_size, 4096);
            assert_eq!(after_allocate.total_blocks, 512);
            assert_eq!(after_allocate.allocated_data_blocks, 3);
            assert!(after_allocate.journal_generation > initial.journal_generation);
            assert!(after_allocate.journal_checkpoint_sequence > 0);
            assert_eq!(after_allocate.pending_journal_records, 0);
            assert_eq!(after_allocate.recoverable_transactions, 0);
            assert_eq!(
                after_allocate.free_data_blocks,
                initial.free_data_blocks - 3
            );
            assert_eq!(
                after_allocate.allocated_data_blocks + after_allocate.free_data_blocks,
                after_allocate.data_blocks
            );

            let mut primary = vec![0_u8; 4096];
            device.read_blocks(0, &mut primary).unwrap();
            primary[100] ^= 1;
            device.write_blocks(0, &primary).unwrap();
            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_read_only_checker_validates_journal_copies_for_all_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("checker-journal-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                drop(volume);
                device.flush().unwrap();

                let mut latest = vec![0_u8; block_size as usize];
                device
                    .read_blocks(layout.journal.checkpoint_b, &mut latest)
                    .unwrap();
                latest[0] ^= 1;
                device
                    .write_blocks(layout.journal.checkpoint_b, &latest)
                    .unwrap();
                device.flush().unwrap();

                let before_check = fs::read(&path).unwrap();
                let report = check_volume(&mut device).unwrap();
                assert_eq!(report.journal_generation, 1);
                assert_eq!(report.journal_checkpoint_sequence, 0);
                assert_eq!(report.pending_journal_records, 0);
                assert_eq!(report.recoverable_transactions, 0);
                assert_eq!(
                    fs::read(&path).unwrap(),
                    before_check,
                    "journal checking must remain read-only for block size {block_size}"
                );

                let mut other = vec![0_u8; block_size as usize];
                device
                    .read_blocks(layout.journal.checkpoint_a, &mut other)
                    .unwrap();
                other[0] ^= 1;
                device
                    .write_blocks(layout.journal.checkpoint_a, &other)
                    .unwrap();
                assert!(
                    matches!(check_volume(&mut device), Err(Error::Corrupt(_))),
                    "checker accepted two invalid checkpoint copies at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn read_only_checker_rejects_checkpoint_counter_regression() {
        let path = image_path("checker-counter-regression");
        {
            let mut device = ImageDevice::create(&path, 4096, 257).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            drop(volume);

            // The lower generation claims durable progress; the higher generation
            // must not roll either sequence or transaction IDs backward.
            for (block, checkpoint) in [
                (
                    layout.journal.checkpoint_a,
                    JournalCheckpoint {
                        generation: 9,
                        checkpoint_sequence: 4,
                        next_sequence: 5,
                        next_transaction_id: 3,
                        head_block: 0,
                        tail_block: 0,
                    },
                ),
                (
                    layout.journal.checkpoint_b,
                    JournalCheckpoint {
                        generation: 10,
                        checkpoint_sequence: 0,
                        next_sequence: 1,
                        next_transaction_id: 1,
                        head_block: 0,
                        tail_block: 0,
                    },
                ),
            ] {
                let encoded = checkpoint.encode().unwrap();
                let mut block_bytes = vec![0_u8; 4096];
                block_bytes[..encoded.len()].copy_from_slice(&encoded);
                device.write_blocks(block, &block_bytes).unwrap();
            }
            device.flush().unwrap();
            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn read_only_checker_rejects_checkpoint_generation_gaps() {
        let path = image_path("checker-generation-gap");
        {
            let mut device = ImageDevice::create(&path, 4096, 257).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            drop(volume);
            for (block_number, checkpoint) in [
                (
                    layout.journal.checkpoint_a,
                    JournalCheckpoint {
                        generation: 4,
                        checkpoint_sequence: 3,
                        next_sequence: 4,
                        next_transaction_id: 2,
                        head_block: 0,
                        tail_block: 0,
                    },
                ),
                (
                    layout.journal.checkpoint_b,
                    JournalCheckpoint {
                        generation: 2,
                        checkpoint_sequence: 0,
                        next_sequence: 1,
                        next_transaction_id: 1,
                        head_block: 0,
                        tail_block: 0,
                    },
                ),
            ] {
                let encoded = checkpoint.encode().unwrap();
                let mut block = vec![0_u8; 4096];
                block[..encoded.len()].copy_from_slice(&encoded);
                device.write_blocks(block_number, &block).unwrap();
            }
            device.flush().unwrap();
            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn read_only_checker_rejects_post_checkpoint_transaction_id_reuse() {
        use crate::{JournalRecord, JournalRecordKind};

        let path = image_path("checker-transaction-id-reuse");
        {
            let mut device = ImageDevice::create(&path, 4096, 257).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            drop(volume);

            // The checkpoint says IDs below five have already been consumed. A
            // post-checkpoint record cannot legally reuse transaction ID one.
            let checkpoint = JournalCheckpoint {
                generation: 3,
                checkpoint_sequence: 12,
                next_sequence: 13,
                next_transaction_id: 5,
                head_block: 0,
                tail_block: 0,
            };
            let encoded = checkpoint.encode().unwrap();
            let mut checkpoint_block = vec![0_u8; 4096];
            checkpoint_block[..encoded.len()].copy_from_slice(&encoded);
            device
                .write_blocks(layout.journal.checkpoint_a, &checkpoint_block)
                .unwrap();
            let older = JournalCheckpoint {
                generation: 2,
                checkpoint_sequence: 9,
                next_sequence: 10,
                next_transaction_id: 4,
                head_block: 0,
                tail_block: 0,
            };
            let older_encoded = older.encode().unwrap();
            let mut older_block = vec![0_u8; 4096];
            older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
            device
                .write_blocks(layout.journal.checkpoint_b, &older_block)
                .unwrap();

            let record = JournalRecord {
                kind: JournalRecordKind::Begin,
                sequence: 13,
                transaction_id: 1,
                payload: Vec::new(),
            };
            device
                .write_blocks(
                    layout.journal.ring_start,
                    &record.encode_block(4096).unwrap(),
                )
                .unwrap();
            device.flush().unwrap();

            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn read_only_checker_rejects_transaction_id_gaps_after_checkpoint() {
        use crate::{JournalRecord, JournalRecordKind};

        let path = image_path("checker-transaction-id-gap");
        {
            let mut device = ImageDevice::create(&path, 4096, 257).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            drop(volume);

            let checkpoint = JournalCheckpoint {
                generation: 3,
                checkpoint_sequence: 12,
                next_sequence: 13,
                next_transaction_id: 5,
                head_block: 0,
                tail_block: 0,
            };
            let encoded = checkpoint.encode().unwrap();
            let mut checkpoint_block = vec![0_u8; 4096];
            checkpoint_block[..encoded.len()].copy_from_slice(&encoded);
            device
                .write_blocks(layout.journal.checkpoint_a, &checkpoint_block)
                .unwrap();
            let older = JournalCheckpoint {
                generation: 2,
                checkpoint_sequence: 9,
                next_sequence: 10,
                next_transaction_id: 4,
                head_block: 0,
                tail_block: 0,
            };
            let older_encoded = older.encode().unwrap();
            let mut older_block = vec![0_u8; 4096];
            older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
            device
                .write_blocks(layout.journal.checkpoint_b, &older_block)
                .unwrap();

            // ID six is greater than the checkpoint's next ID, but ID five
            // cannot be skipped by the single-writer journal protocol.
            let record = JournalRecord {
                kind: JournalRecordKind::Begin,
                sequence: 13,
                transaction_id: 6,
                payload: Vec::new(),
            };
            device
                .write_blocks(
                    layout.journal.ring_start,
                    &record.encode_block(4096).unwrap(),
                )
                .unwrap();
            device.flush().unwrap();

            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn canonical_layout_regions_are_disjoint_across_supported_geometries() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for total_blocks in [22_u64, 23, 255, 256, 4097, 65_537, 1_000_003] {
                let layout = VolumeLayout::calculate(block_size, total_blocks).unwrap();
                let bitmap_end = layout.bitmap_start + layout.bitmap_blocks;
                let checkpoint_end = layout.journal.checkpoint_a + 1;
                let ring_end = layout.journal.ring_start + layout.journal.ring_blocks;

                assert_eq!(layout.bitmap_start, 1);
                assert_eq!(bitmap_end, layout.journal.checkpoint_a);
                assert_eq!(layout.journal.checkpoint_b, checkpoint_end);
                assert_eq!(layout.journal.ring_start, layout.journal.checkpoint_b + 1);
                assert_eq!(ring_end, layout.data_start);
                assert!(layout.data_start < layout.backup_superblock);
                assert_eq!(layout.backup_superblock, total_blocks - 1);
                assert_eq!(
                    layout.data_blocks(),
                    layout.backup_superblock - layout.data_start
                );

                for block in 0..total_blocks {
                    let should_be_reserved =
                        block < layout.data_start || block == layout.backup_superblock;
                    assert_eq!(
                        layout.is_reserved(block),
                        should_be_reserved,
                        "reserved-region mismatch: block_size={block_size}, total_blocks={total_blocks}, block={block}"
                    );
                }
            }
        }
    }

    #[test]
    fn canonical_allocator_persists_mixed_allocate_free_sequence_across_reopens() {
        let path = image_path("mixed-allocation-reopen-sequence");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first = volume.allocate_block(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            let third = volume.allocate_block(&mut device).unwrap();
            assert_eq!(second, first + 1);
            assert_eq!(third, second + 1);

            volume.free_block(&mut device, second).unwrap();
            volume.free_block(&mut device, first).unwrap();
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
            assert_eq!(volume.allocate_block(&mut device).unwrap(), second);
            assert_eq!(volume.allocate_block(&mut device).unwrap(), third + 1);
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), third + 2);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_allocated_block_inventory_tracks_persistent_bitmap_state() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("allocated-block-inventory-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                assert!(volume
                    .allocated_data_block_inventory(&mut device)
                    .unwrap()
                    .is_empty());

                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                let third = volume.allocate_block(&mut device).unwrap();
                assert_eq!(
                    volume.allocated_data_block_inventory(&mut device).unwrap(),
                    vec![first, second, third]
                );

                volume.free_block(&mut device, second).unwrap();
                device.flush().unwrap();
                drop(volume);

                let volume = open_volume(&mut device).unwrap();
                assert_eq!(
                    volume.allocated_data_block_inventory(&mut device).unwrap(),
                    vec![first, third]
                );
                assert_eq!(
                    volume
                        .allocation_stats(&mut device)
                        .unwrap()
                        .allocated_data_blocks,
                    2
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_free_inventory_and_fragmentation_stats_track_allocations_and_holes() {
        let path = image_path("free-inventory-fragmentation");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            let initial_free = volume.free_data_block_inventory(&mut device).unwrap();
            assert_eq!(initial_free.len() as u64, layout.data_blocks());
            assert_eq!(initial_free.first(), Some(&layout.data_start));
            assert_eq!(initial_free.last(), Some(&(layout.backup_superblock - 1)));
            let initial_fragments = volume.free_extent_stats(&mut device).unwrap();
            assert_eq!(initial_fragments.free_extent_count, 1);
            assert_eq!(
                initial_fragments.largest_free_extent_blocks,
                layout.data_blocks()
            );

            let blocks = volume.allocate_blocks(&mut device, 3).unwrap();
            let free_after = volume.free_data_block_inventory(&mut device).unwrap();
            assert_eq!(free_after.len() as u64, layout.data_blocks() - 3);
            assert!(!free_after.contains(&blocks[0]));
            assert!(!free_after.contains(&blocks[1]));
            assert!(!free_after.contains(&blocks[2]));
            let fragmented = volume.free_extent_stats(&mut device).unwrap();
            assert_eq!(fragmented.free_extent_count, 1);
            assert_eq!(
                fragmented.largest_free_extent_blocks,
                layout.data_blocks() - 3
            );

            volume.free_blocks(&mut device, &[blocks[1]]).unwrap();
            let holes = volume.free_data_block_inventory(&mut device).unwrap();
            assert!(holes.contains(&blocks[1]));
            let split = volume.free_extent_stats(&mut device).unwrap();
            assert_eq!(split.free_extent_count, 2);
            assert_eq!(split.largest_free_extent_blocks, layout.data_blocks() - 3);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_range_based_ownership_report_handles_fragmentation_and_freed_references() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("range-ownership-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 512).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let blocks = volume.allocate_blocks(&mut device, 6).unwrap();
                let map = ExtentMap::new(vec![
                    Extent {
                        logical_start: 0,
                        physical_start: blocks[2],
                        block_count: 4,
                    },
                    Extent {
                        logical_start: 5,
                        physical_start: blocks[0],
                        block_count: 2,
                    },
                ])
                .unwrap();

                volume
                    .validate_extent_map_ownership(&mut device, &[(&map, 7)])
                    .unwrap();

                let orphan = volume.allocate_blocks(&mut device, 1).unwrap()[0];
                volume.free_block(&mut device, blocks[1]).unwrap();
                let report = volume
                    .extent_map_ownership_report(&mut device, &[(&map, 7)])
                    .unwrap();
                assert_eq!(report.orphaned_allocations, vec![orphan]);
                assert_eq!(report.orphaned_allocation_count, 1);
                assert_eq!(report.dangling_references, vec![blocks[1]]);
                assert_eq!(report.dangling_reference_count, 1);
                assert!(!report.is_consistent());
                assert!(matches!(
                    volume.validate_extent_map_ownership(&mut device, &[(&map, 5)]),
                    Err(Error::Corrupt(_))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn canonical_extent_map_ownership_bridge_checks_bitmap_and_cross_file_aliases() {
        let path = image_path("extent-map-ownership-bridge");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let blocks = volume.allocate_blocks(&mut device, 4).unwrap();
            let first_map = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: blocks[0],
                block_count: 2,
            }])
            .unwrap();
            let second_map = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: blocks[2],
                block_count: 2,
            }])
            .unwrap();

            volume
                .validate_extent_map_ownership(&mut device, &[(&first_map, 2), (&second_map, 2)])
                .unwrap();

            // Diagnostic mode identifies the exact allocated blocks with no discovered owner.
            let orphan_report = volume
                .extent_map_ownership_report(&mut device, &[(&first_map, 2)])
                .unwrap();
            assert_eq!(
                orphan_report.orphaned_allocations,
                vec![blocks[2], blocks[3]]
            );
            assert!(orphan_report.dangling_references.is_empty());
            assert!(!orphan_report.is_consistent());

            // Strict mode remains available when a caller only needs pass/fail.
            assert!(matches!(
                volume.validate_extent_map_ownership(&mut device, &[(&first_map, 2)]),
                Err(Error::Corrupt(_))
            ));

            // Reusing a physical block in a second file is rejected even if the bitmap is set.
            let alias_map = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: blocks[0],
                block_count: 1,
            }])
            .unwrap();
            assert!(matches!(
                volume.validate_extent_map_ownership(
                    &mut device,
                    &[(&first_map, 2), (&alias_map, 1)],
                ),
                Err(Error::Corrupt(_))
            ));

            // An extent pointing into metadata or past the data region is rejected before bitmap comparison.
            let out_of_region = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: volume.volume.layout.backup_superblock,
                block_count: 1,
            }])
            .unwrap();
            assert!(matches!(
                volume.validate_extent_map_ownership(&mut device, &[(&out_of_region, 1)]),
                Err(Error::Corrupt(_))
            ));

            // A freed block still referenced by an extent is a dangling ownership reference.
            volume.free_block(&mut device, blocks[3]).unwrap();
            let dangling = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: blocks[3],
                block_count: 1,
            }])
            .unwrap();
            let dangling_report = volume
                .extent_map_ownership_report(&mut device, &[(&dangling, 1)])
                .unwrap();
            assert_eq!(
                dangling_report.orphaned_allocations,
                vec![blocks[0], blocks[1], blocks[2]]
            );
            assert_eq!(dangling_report.dangling_references, vec![blocks[3]]);
            assert!(matches!(
                volume.validate_extent_map_ownership(&mut device, &[(&dangling, 1)]),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_ownership_report_bounds_large_orphan_and_dangling_samples() {
        let path = image_path("bounded-ownership-report");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let blocks = volume.allocate_blocks(&mut device, 100).unwrap();

            let orphaned = volume
                .allocation_ownership_report(&mut device, &[])
                .unwrap();
            assert_eq!(orphaned.orphaned_allocation_count, 100);
            assert_eq!(orphaned.orphaned_allocations, blocks[..64]);
            assert!(orphaned.orphaned_allocations_truncated);
            assert_eq!(orphaned.dangling_reference_count, 0);
            assert!(!orphaned.dangling_references_truncated);
            assert!(!orphaned.is_consistent());

            volume.free_blocks(&mut device, &blocks).unwrap();
            let dangling = volume
                .allocation_ownership_report(&mut device, &blocks)
                .unwrap();
            assert_eq!(dangling.orphaned_allocation_count, 0);
            assert!(dangling.orphaned_allocations.is_empty());
            assert!(!dangling.orphaned_allocations_truncated);
            assert_eq!(dangling.dangling_reference_count, 100);
            assert_eq!(dangling.dangling_references, blocks[..64]);
            assert!(dangling.dangling_references_truncated);
            assert!(!dangling.is_consistent());
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_allocation_ownership_validator_detects_orphans_and_dangling_references() {
        let path = image_path("ownership-inventory");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let first = volume.allocate_block(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();

            volume
                .validate_allocation_ownership(&mut device, &[first, second])
                .unwrap();
            assert!(matches!(
                volume.validate_allocation_ownership(&mut device, &[first]),
                Err(Error::Corrupt(_))
            ));
            assert!(matches!(
                volume.validate_allocation_ownership(&mut device, &[first, first, second]),
                Err(Error::Corrupt(_))
            ));

            volume.free_block(&mut device, second).unwrap();
            assert!(matches!(
                volume.validate_allocation_ownership(&mut device, &[first, second]),
                Err(Error::Corrupt(_))
            ));
            assert!(matches!(
                volume.validate_allocation_ownership(
                    &mut device,
                    &[first, volume.volume.layout.backup_superblock]
                ),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_layout_rejects_too_small_volumes() {
        assert!(VolumeLayout::calculate(4096, 11).is_err());
    }

    #[test]
    fn canonical_layout_pins_minimum_geometry_and_rejects_invalid_or_overflowing_inputs() {
        let minimum = VolumeLayout::calculate(4096, 24).unwrap();
        assert_eq!(minimum.bitmap_start, 1);
        assert_eq!(minimum.bitmap_blocks, 1);
        assert_eq!(minimum.journal.checkpoint_a, 2);
        assert_eq!(minimum.journal.checkpoint_b, 3);
        assert_eq!(minimum.journal.ring_start, 4);
        assert_eq!(minimum.journal.ring_blocks, 16);
        assert_eq!(minimum.data_start, 20);
        assert_eq!(minimum.backup_superblock, 23);
        assert_eq!(minimum.data_blocks(), 3);

        for block_size in [0, 512, 2048, 4097, 131_072] {
            assert!(
                matches!(
                    VolumeLayout::calculate(block_size, 256),
                    Err(Error::InvalidInput(_))
                ),
                "unexpectedly accepted block size {block_size}"
            );
        }

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            assert!(matches!(
                VolumeLayout::calculate(block_size, u64::MAX),
                Err(Error::ArithmeticOverflow)
            ));
            assert!(matches!(
                VolumeLayout::calculate(block_size, u64::MAX / u64::from(block_size) + 1),
                Err(Error::ArithmeticOverflow)
            ));
            assert!(matches!(
                VolumeLayout::calculate(block_size, 22),
                Ok(layout) if layout.data_blocks() > 0
            ));
        }
    }

    #[test]
    fn refused_descriptor_publication_preserves_exact_image_bytes_across_block_sizes() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!(
                "publication-rejection-byte-preservation-{block_size}"
            ));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let orphan_block = volume.allocate_block(&mut device).unwrap();

                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };

                device.flush().unwrap();
                let bytes_before = std::fs::read(&path).unwrap();
                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::InvalidInput(
                            "persistent inventory has unresolved allocation ownership anomalies"
                        ))
                    ),
                    "publication must reject an unowned allocation at block size {block_size}"
                );
                assert!(
                    volume.is_data_block_allocated(&mut device, orphan_block).unwrap(),
                    "rejected publication must preserve the orphan allocation at block size {block_size}"
                );
                drop(volume);
                drop(device);

                let bytes_after = std::fs::read(&path).unwrap();
                assert_eq!(
                    bytes_after, bytes_before,
                    "rejected publication must not change any persisted image byte at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn descriptor_replacement_recovers_old_or_new_descriptor_after_write_failures() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for (fail_kind, fail_at) in (0..=6)
                .map(|index| ("write", index))
                .chain((0..=6).map(|index| ("flush", index)))
            {
                let path = image_path(&format!(
                    "descriptor-replace-crash-{block_size}-{fail_kind}-{fail_at}"
                ));
                {
                    let image = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut device = FailNthWrite {
                        inner: image,
                        writes_until_failure: None,
                        flushes_until_failure: None,
                    };
                    let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                    let metadata_block = volume.allocate_block(&mut device).unwrap();
                    let directory_block = volume.allocate_block(&mut device).unwrap();
                    let root = MetadataRecord {
                        id: ObjectId(1),
                        parent_id: 0,
                        kind: ObjectKind::Directory,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let child = MetadataRecord {
                        id: ObjectId(2),
                        parent_id: 1,
                        kind: ObjectKind::File,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let second_child = MetadataRecord {
                        id: ObjectId(3),
                        parent_id: 1,
                        kind: ObjectKind::File,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let entry = DirectoryEntry {
                        parent_id: 1,
                        object_id: 2,
                        kind: ObjectKind::File,
                        name: "child.txt".to_owned(),
                    };
                    let second_entry = DirectoryEntry {
                        parent_id: 1,
                        object_id: 3,
                        kind: ObjectKind::File,
                        name: "second.txt".to_owned(),
                    };
                    let mut metadata_bytes = vec![0_u8; block_size as usize];
                    metadata_bytes[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&root.encode().unwrap());
                    metadata_bytes[crate::METADATA_RECORD_SIZE..2 * crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&child.encode().unwrap());
                    metadata_bytes
                        [2 * crate::METADATA_RECORD_SIZE..3 * crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&second_child.encode().unwrap());
                    device
                        .write_blocks(metadata_block, &metadata_bytes)
                        .unwrap();
                    let mut directory_bytes = vec![0_u8; block_size as usize];
                    directory_bytes[..crate::DIRECTORY_ENTRY_SIZE]
                        .copy_from_slice(&entry.encode().unwrap());
                    directory_bytes[crate::DIRECTORY_ENTRY_SIZE..2 * crate::DIRECTORY_ENTRY_SIZE]
                        .copy_from_slice(&second_entry.encode().unwrap());
                    device
                        .write_blocks(directory_block, &directory_bytes)
                        .unwrap();

                    let old_descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 2,
                        directory_blocks: vec![directory_block],
                        directory_entry_count: 1,
                    };
                    let old_block = volume
                        .publish_persistent_inventory(&mut device, &old_descriptor)
                        .unwrap();
                    let new_descriptor = PersistentInventoryDescriptor {
                        metadata_record_count: 3,
                        directory_entry_count: 2,
                        ..old_descriptor.clone()
                    };
                    match fail_kind {
                        "write" => device.writes_until_failure = Some(fail_at),
                        "flush" => device.flushes_until_failure = Some(fail_at),
                        _ => unreachable!("failure kinds are fixed"),
                    }
                    let result = volume
                        .replace_persistent_inventory_descriptor(&mut device, &new_descriptor);
                    assert!(
                        result.is_ok() || matches!(result, Err(Error::Io(_))),
                        "unexpected replacement error at block size {block_size}, {fail_kind} {fail_at}: {result:?}"
                    );
                    drop(volume);
                    drop(device);

                    let mut reopened_device = ImageDevice::open(&path, block_size).unwrap();
                    let reopened = open_volume(&mut reopened_device).unwrap();
                    let (descriptor_block, descriptor) = reopened
                        .discover_persistent_inventory(&mut reopened_device)
                        .unwrap()
                        .unwrap();
                    assert!(
                        (descriptor_block == old_block && descriptor == old_descriptor)
                            || (descriptor_block != old_block && descriptor == new_descriptor),
                        "recovery must expose exactly old or new descriptor at block size {block_size}, {fail_kind} {fail_at}: {descriptor:?}"
                    );
                    assert!(reopened
                        .verify_discovered_persistent_inventory(&mut reopened_device)
                        .unwrap()
                        .unwrap()
                        .is_consistent());
                    drop(reopened);
                    drop(reopened_device);

                    // Recovery must converge: another close/open cycle must retain
                    // the exact descriptor selected by the first recovery.
                    let mut second_device = ImageDevice::open(&path, block_size).unwrap();
                    let second_open = open_volume(&mut second_device).unwrap();
                    let second_discovered = second_open
                        .discover_persistent_inventory(&mut second_device)
                        .unwrap()
                        .unwrap();
                    assert_eq!(
                        second_discovered,
                        (descriptor_block, descriptor.clone()),
                        "descriptor recovery must be idempotent at block size {block_size}, {fail_kind} {fail_at}"
                    );
                    assert!(second_open
                        .verify_discovered_persistent_inventory(&mut second_device)
                        .unwrap()
                        .unwrap()
                        .is_consistent());
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn descriptor_replacement_recovers_old_or_new_across_bitmap_pages_at_each_write_boundary() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        // A 4 KiB bitmap page covers 32,768 blocks. The old descriptor and one metadata
        // block live on page 2; the replacement is staged on page 1. Exercise every write
        // and flush boundary with a real inventory-list ownership change.
        let failures = (0..=12)
            .map(|fail_at| (true, fail_at))
            .chain((0..=6).map(|fail_at| (false, fail_at)))
            .collect::<Vec<_>>();
        for (fail_write, fail_at) in failures {
            let failure_kind = if fail_write { "write" } else { "flush" };
            let path = image_path(&format!(
                "descriptor-replacement-multi-page-crash-{failure_kind}-{fail_at}"
            ));
            {
                let image = ImageDevice::create(&path, 4096, 32_780).unwrap();
                let mut device = FailNthWrite {
                    inner: image,
                    writes_until_failure: None,
                    flushes_until_failure: None,
                };
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_capacity = 4096 / crate::METADATA_RECORD_SIZE;
                let entries_per_block = 4096 / crate::DIRECTORY_ENTRY_SIZE;
                assert!(metadata_capacity > 1 && entries_per_block > 0);
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let old_directory_block_count = metadata_capacity.div_ceil(entries_per_block);
                let new_directory_block_count = (metadata_capacity - 1).div_ceil(entries_per_block);
                let mut directory_blocks = Vec::new();
                for _ in 0..old_directory_block_count {
                    directory_blocks.push(volume.allocate_block(&mut device).unwrap());
                }

                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; 4096];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                let mut high_metadata_bytes = vec![0_u8; 4096];
                let mut directory_contents = vec![vec![0_u8; 4096]; old_directory_block_count];

                for child_index in 0..metadata_capacity {
                    let child_id = child_index as u128 + 2;
                    let child = MetadataRecord {
                        id: ObjectId(child_id),
                        parent_id: 1,
                        kind: ObjectKind::File,
                        flags: 0,
                        size: 0,
                        extent_root_block: None,
                        generation: 1,
                    };
                    let encoded_child = child.encode().unwrap();
                    if child_index + 1 < metadata_capacity {
                        let start = (child_index + 1) * crate::METADATA_RECORD_SIZE;
                        metadata_bytes[start..start + crate::METADATA_RECORD_SIZE]
                            .copy_from_slice(&encoded_child);
                    } else {
                        // Inventories are packed byte streams, not per-block record arrays.
                        // The final record begins in the last bytes of the first block and
                        // continues at offset zero in the next block.
                        let fragment_start = metadata_capacity * crate::METADATA_RECORD_SIZE;
                        let first_fragment_len = 4096 - fragment_start;
                        metadata_bytes[fragment_start..]
                            .copy_from_slice(&encoded_child[..first_fragment_len]);
                        high_metadata_bytes[..crate::METADATA_RECORD_SIZE - first_fragment_len]
                            .copy_from_slice(&encoded_child[first_fragment_len..]);
                    }

                    let entry = DirectoryEntry {
                        parent_id: 1,
                        object_id: child_id,
                        kind: ObjectKind::File,
                        name: format!("child-{child_index:03}"),
                    };
                    let encoded_entry = entry.encode().unwrap();
                    let directory_index = child_index / entries_per_block;
                    let offset = (child_index % entries_per_block) * crate::DIRECTORY_ENTRY_SIZE;
                    directory_contents[directory_index]
                        [offset..offset + crate::DIRECTORY_ENTRY_SIZE]
                        .copy_from_slice(&encoded_entry);
                }
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                for (block, contents) in directory_blocks.iter().zip(directory_contents.iter()) {
                    device.write_blocks(*block, contents).unwrap();
                }

                let old_block = 32_770_u64;
                let high_metadata_block = 32_769_u64;
                let old_descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block, high_metadata_block],
                    metadata_record_count: metadata_capacity + 1,
                    directory_blocks: directory_blocks.clone(),
                    directory_entry_count: metadata_capacity,
                };
                let bits_per_bitmap_block = 4096_u64 * 8;
                let bitmap_index = old_block / bits_per_bitmap_block;
                let bitmap_block = volume.volume.layout.bitmap_start + bitmap_index;
                let mut bitmap = vec![0_u8; 4096];
                device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                for block in [high_metadata_block, old_block] {
                    let local = block % bits_per_bitmap_block;
                    bitmap[(local / 8) as usize] |= 1_u8 << (local % 8);
                }
                device.write_blocks(bitmap_block, &bitmap).unwrap();
                device
                    .write_blocks(high_metadata_block, &high_metadata_bytes)
                    .unwrap();
                device
                    .write_blocks(old_block, &old_descriptor.encode(4096).unwrap())
                    .unwrap();
                device.flush().unwrap();
                crate::volume::persist_inventory_hint(&mut device, old_block).unwrap();
                assert!(matches!(
                    volume.verify_discovered_persistent_inventory(&mut device).unwrap(),
                    Some(report) if report.is_consistent()
                ));

                let new_descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: metadata_capacity,
                    directory_blocks: directory_blocks[..new_directory_block_count].to_vec(),
                    directory_entry_count: metadata_capacity - 1,
                };
                if fail_write {
                    device.writes_until_failure = Some(fail_at);
                } else {
                    device.flushes_until_failure = Some(fail_at);
                }
                let result =
                    volume.replace_persistent_inventory_descriptor(&mut device, &new_descriptor);
                assert!(
                    result.is_ok() || matches!(result, Err(Error::Io(_))),
                    "unexpected replacement error at {failure_kind} {fail_at}: {result:?}"
                );
                drop(volume);
                drop(device);

                // Recovery must publish the old descriptor before COMMIT or the new one
                // after COMMIT, never a mixed bitmap/descriptor state.
                let mut reopened_device = ImageDevice::open(&path, 4096).unwrap();
                let reopened = open_volume(&mut reopened_device).unwrap();
                let (found_block, found_descriptor) = reopened
                    .discover_persistent_inventory(&mut reopened_device)
                    .unwrap()
                    .unwrap();
                assert!(
                    (found_block == old_block && found_descriptor == old_descriptor)
                        || (found_block != old_block && found_descriptor == new_descriptor),
                    "recovery exposed a mixed state at {failure_kind} {fail_at}: {found_descriptor:?}"
                );
                if found_block == old_block {
                    assert!(reopened
                        .is_data_block_allocated(&mut reopened_device, old_block)
                        .unwrap());
                    assert!(reopened
                        .is_data_block_allocated(&mut reopened_device, high_metadata_block)
                        .unwrap());
                } else {
                    assert!(!reopened
                        .is_data_block_allocated(&mut reopened_device, old_block)
                        .unwrap());
                    assert!(!reopened
                        .is_data_block_allocated(&mut reopened_device, high_metadata_block)
                        .unwrap());
                    assert!(reopened
                        .is_data_block_allocated(&mut reopened_device, found_block)
                        .unwrap());
                }
                assert!(
                    matches!(
                        reopened
                            .verify_discovered_persistent_inventory(&mut reopened_device)
                            .unwrap_or_else(|error| {
                                panic!(
                                    "verification failed at {failure_kind} {fail_at}: {error:?}"
                                )
                            }),
                        Some(report) if report.is_consistent()
                    ),
                    "recovered inventory is inconsistent at {failure_kind} {fail_at}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn repeated_descriptor_publication_is_idempotent_and_different_replacement_is_rejected() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("descriptor-replacement-rejected-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();

                let descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let descriptor_block = volume
                    .publish_persistent_inventory(&mut device, &descriptor)
                    .unwrap();
                device.flush().unwrap();
                let bytes_before = std::fs::read(&path).unwrap();

                assert_eq!(
                    volume.publish_persistent_inventory(&mut device, &descriptor).unwrap(),
                    descriptor_block,
                    "identical publication retry must return the existing block at block size {block_size}"
                );
                let different_descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(2),
                    ..descriptor.clone()
                };
                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &different_descriptor),
                        Err(Error::InvalidInput(
                            "a different persistent inventory descriptor already exists"
                        ))
                    ),
                    "a different inventory must not replace the published descriptor at block size {block_size}"
                );
                let changed_ownership = PersistentInventoryDescriptor {
                    metadata_blocks: vec![],
                    metadata_record_count: 0,
                    ..descriptor.clone()
                };
                assert!(
                    volume
                        .replace_persistent_inventory_descriptor(&mut device, &changed_ownership)
                        .is_err(),
                    "replacement must reject an inventory that cannot reconstruct a valid namespace at block size {block_size}"
                );
                drop(volume);
                drop(device);

                let bytes_after = std::fs::read(&path).unwrap();
                assert_eq!(
                    bytes_after, bytes_before,
                    "rejected replacement must preserve every image byte at block size {block_size}"
                );

                let mut reopened_device = ImageDevice::open(&path, block_size).unwrap();
                let reopened_volume = open_volume(&mut reopened_device).unwrap();
                let discovered = reopened_volume
                    .discover_persistent_inventory(&mut reopened_device)
                    .unwrap()
                    .unwrap();
                assert_eq!(discovered.0, descriptor_block);
                assert_eq!(discovered.1, descriptor);
                drop(reopened_volume);
                drop(reopened_device);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn locator_update_write_and_flush_failures_leave_repairable_superblocks() {
        for (fail_write, fail_at) in [(true, 0), (true, 1), (false, 0), (false, 1)] {
            let path = image_path(&format!("locator-update-failure-{fail_write}-{fail_at}"));
            {
                let image = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut device = FailNthWrite {
                    inner: image,
                    writes_until_failure: None,
                    flushes_until_failure: None,
                };
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let old_hint = volume.volume.layout.data_start;
                let new_hint = old_hint + 1;
                drop(volume);

                if fail_write {
                    device.writes_until_failure = Some(fail_at);
                } else {
                    device.flushes_until_failure = Some(fail_at);
                }
                assert!(
                    matches!(persist_inventory_hint(&mut device, new_hint), Err(Error::Io(_))),
                    "expected injected locator update failure at write={fail_write}, index={fail_at}"
                );
                drop(device);

                // A crash after either superblock write/flush must not make the volume
                // unopenable. A retry must converge both independently checksummed copies.
                let mut reopened_device = ImageDevice::open(&path, 4096).unwrap();
                let reopened = open_volume(&mut reopened_device).unwrap();
                drop(reopened);
                persist_inventory_hint(&mut reopened_device, new_hint).unwrap();

                let primary = read_superblock(&mut reopened_device, 0).unwrap();
                let last_block = reopened_device.block_count() - 1;
                let backup = read_superblock(&mut reopened_device, last_block).unwrap();
                assert_eq!(decode_inventory_hint(&primary), Some(new_hint));
                assert_eq!(decode_inventory_hint(&backup), Some(new_hint));
                decode_superblock(&primary).unwrap();
                decode_superblock(&backup).unwrap();
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn backup_superblock_checksum_is_validated_even_when_locator_hints_may_differ() {
        let path = image_path("backup-superblock-checksum");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let backup = volume.volume.layout.backup_superblock;
            let first_hint = volume.volume.layout.data_start;
            let second_hint = first_hint + 1;
            drop(volume);

            // Simulate the durable intermediate state of a backup-first locator update:
            // both superblocks are independently valid, but their hint values differ.
            persist_inventory_hint(&mut device, first_hint).unwrap();
            let mut bytes = vec![0_u8; 4096];
            device.read_blocks(backup, &mut bytes).unwrap();
            bytes[INVENTORY_HINT_OFFSET..INVENTORY_HINT_OFFSET + 8]
                .copy_from_slice(&second_hint.to_le_bytes());
            let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
            bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].copy_from_slice(&checksum.to_le_bytes());
            device.write_blocks(backup, &bytes).unwrap();
            device.flush().unwrap();

            // Differing but independently valid hints must not make the volume unopenable.
            drop(open_volume(&mut device).unwrap());

            // A later backup checksum failure must still be detected, even though the
            // primary and backup hint fields differ and the primary copy is valid.
            bytes[CHECKSUM_OFFSET] ^= 0x80;
            device.write_blocks(backup, &bytes).unwrap();
            device.flush().unwrap();

            assert!(matches!(
                open_volume(&mut device),
                Err(Error::Corrupt("superblock CRC32C mismatch"))
            ));
            assert!(matches!(
                check_volume(&mut device),
                Err(Error::Corrupt("superblock CRC32C mismatch"))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn canonical_superblock_has_golden_bytes_for_every_supported_block_size() {
        let fixtures = [
            (4_096_u32, 0x9eda_07d8_u32),
            (8_192_u32, 0x23d6_fef0_u32),
            (16_384_u32, 0x5c23_7a51_u32),
            (32_768_u32, 0xa3c8_7313_u32),
            (65_536_u32, 0x59f2_1766_u32),
        ];
        let uuid = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];
        let features = 0x0102_0304_0506_0708_u64;

        for (block_size, expected_crc) in fixtures {
            let encoded = encode_superblock(block_size, 256, features, uuid).unwrap();
            let mut expected_header = [0_u8; 48];
            expected_header[..8].copy_from_slice(MAGIC);
            expected_header[8..10].copy_from_slice(&VERSION.to_le_bytes());
            expected_header[10..12].copy_from_slice(&HEADER_SIZE.to_le_bytes());
            expected_header[12..16].copy_from_slice(&block_size.to_le_bytes());
            expected_header[16..24].copy_from_slice(&256_u64.to_le_bytes());
            expected_header[24..32].copy_from_slice(&features.to_le_bytes());
            expected_header[32..48].copy_from_slice(&uuid);

            assert_eq!(
                &encoded[..48],
                &expected_header,
                "golden header mismatch for block size {block_size}"
            );
            assert_eq!(
                u32::from_le_bytes(
                    encoded[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
                        .try_into()
                        .unwrap()
                ),
                expected_crc,
                "golden CRC32C mismatch for block size {block_size}"
            );
            assert!(
                encoded[48..CHECKSUM_OFFSET].iter().all(|byte| *byte == 0),
                "reserved bytes must remain zero for block size {block_size}"
            );
            assert!(
                encoded[CHECKSUM_OFFSET + 4..].iter().all(|byte| *byte == 0),
                "superblock tail must remain zero for block size {block_size}"
            );
            assert_eq!(
                decode_superblock(&encoded).unwrap(),
                (block_size, 256, features, uuid),
                "golden superblock must round-trip for block size {block_size}"
            );

            // Pin the bytes emitted by the real formatter, not just the standalone codec.
            let path = image_path(&format!("superblock-golden-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let volume = format_volume(&mut device, uuid, features).unwrap();
                for block in [0_u64, 255_u64] {
                    let mut on_disk = vec![0_u8; block_size as usize];
                    device.read_blocks(block, &mut on_disk).unwrap();
                    assert_eq!(
                        &on_disk[..encoded.len()],
                        &encoded,
                        "formatted superblock differs from golden bytes at block {block}, block size {block_size}"
                    );
                    assert!(
                        on_disk[encoded.len()..].iter().all(|byte| *byte == 0),
                        "formatted superblock padding must remain zero at block {block}, block size {block_size}"
                    );
                }
                drop(volume);
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn formatted_volume_image_is_deterministic_at_every_supported_block_size() {
        let uuid = *b"openfs-golden-01";
        let features = 0x0102_0304_0506_0708_u64;
        let total_blocks = 64_u64;

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let first_path = image_path(&format!("deterministic-format-first-{block_size}"));
            let second_path = image_path(&format!("deterministic-format-second-{block_size}"));
            let image_len = usize::try_from(
                u64::from(block_size)
                    .checked_mul(total_blocks)
                    .expect("test image size fits in u64"),
            )
            .expect("test image size fits in usize");

            let first_image = {
                let mut device =
                    ImageDevice::create(&first_path, block_size, total_blocks).unwrap();
                let volume = format_volume(&mut device, uuid, features).unwrap();
                let mut image = vec![0_u8; image_len];
                device.read_blocks(0, &mut image).unwrap();
                assert_eq!(volume.volume.block_size, block_size);
                assert!(check_volume(&mut device).is_ok());
                image
            };

            let second_image = {
                let mut device =
                    ImageDevice::create(&second_path, block_size, total_blocks).unwrap();
                let volume = format_volume(&mut device, uuid, features).unwrap();
                let mut image = vec![0_u8; image_len];
                device.read_blocks(0, &mut image).unwrap();
                assert_eq!(volume.volume.block_size, block_size);
                assert!(check_volume(&mut device).is_ok());
                image
            };

            assert_eq!(
                first_image, second_image,
                "formatting must be byte-for-byte deterministic at block size {block_size}"
            );
            fs::remove_file(first_path).unwrap();
            fs::remove_file(second_path).unwrap();
        }
    }
    #[test]
    fn descriptor_replacement_preserves_shared_inventory_and_publishes_new_entries() {
        use crate::{
            DirectoryEntry, MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor,
        };

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("descriptor-replacement-shared-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 512).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();

                let metadata_block = volume.allocate_block(&mut device).unwrap();
                let root = MetadataRecord {
                    id: ObjectId(1),
                    parent_id: 0,
                    kind: ObjectKind::Directory,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let child = MetadataRecord {
                    id: ObjectId(2),
                    parent_id: 1,
                    kind: ObjectKind::File,
                    flags: 0,
                    size: 0,
                    extent_root_block: None,
                    generation: 1,
                };
                let entry = DirectoryEntry {
                    parent_id: 1,
                    object_id: 2,
                    kind: ObjectKind::File,
                    name: "note.txt".to_owned(),
                };

                // Initially publish only the root. Then extend the same packed metadata
                // block and add a directory stream, so replacement must retain one
                // shared inventory block while atomically adding the new ownership.
                let mut metadata_bytes = vec![0_u8; block_size as usize];
                metadata_bytes[..crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&root.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let old_descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 1,
                    directory_blocks: vec![],
                    directory_entry_count: 0,
                };
                let old_descriptor_block = volume
                    .publish_persistent_inventory(&mut device, &old_descriptor)
                    .unwrap();
                // Do not allocate the new directory block until it is about to be
                // published; otherwise it is an orphan under the old descriptor.
                let directory_block = volume.allocate_block(&mut device).unwrap();

                metadata_bytes[crate::METADATA_RECORD_SIZE..2 * crate::METADATA_RECORD_SIZE]
                    .copy_from_slice(&child.encode().unwrap());
                device
                    .write_blocks(metadata_block, &metadata_bytes)
                    .unwrap();
                let mut directory_bytes = vec![0_u8; block_size as usize];
                directory_bytes[..crate::DIRECTORY_ENTRY_SIZE]
                    .copy_from_slice(&entry.encode().unwrap());
                device
                    .write_blocks(directory_block, &directory_bytes)
                    .unwrap();

                let new_descriptor = PersistentInventoryDescriptor {
                    root_id: ObjectId(1),
                    metadata_blocks: vec![metadata_block],
                    metadata_record_count: 2,
                    directory_blocks: vec![directory_block],
                    directory_entry_count: 1,
                };
                let new_descriptor_block = volume
                    .replace_persistent_inventory_descriptor(&mut device, &new_descriptor)
                    .unwrap();
                assert_ne!(
                    new_descriptor_block, old_descriptor_block,
                    "replacement must publish a distinct descriptor at block size {block_size}"
                );
                assert!(
                    volume
                        .is_data_block_allocated(&mut device, metadata_block)
                        .unwrap(),
                    "shared metadata inventory must remain allocated at block size {block_size}"
                );
                assert!(
                    volume
                        .is_data_block_allocated(&mut device, directory_block)
                        .unwrap(),
                    "new directory inventory must remain allocated at block size {block_size}"
                );
                assert!(
                    !volume
                        .is_data_block_allocated(&mut device, old_descriptor_block)
                        .unwrap(),
                    "old descriptor must be released at block size {block_size}"
                );

                // Reopen the volume before rediscovery to prove the replacement is durable
                // through the normal open/recovery path, not only in the original handle.
                drop(volume);
                let volume = open_volume(&mut device).unwrap();

                let discovered = volume
                    .discover_persistent_inventory(&mut device)
                    .unwrap()
                    .unwrap();
                assert_eq!(discovered.0, new_descriptor_block);
                assert_eq!(discovered.1, new_descriptor);
                let namespace = volume
                    .reconstruct_discovered_namespace(&mut device)
                    .unwrap()
                    .unwrap()
                    .1;
                assert_eq!(
                    namespace.lookup(ObjectId(1), "note.txt").unwrap().object_id,
                    2
                );
                assert!(volume
                    .verify_discovered_persistent_inventory(&mut device)
                    .unwrap()
                    .unwrap()
                    .is_consistent());
            }
            fs::remove_file(path).unwrap();
        }
    }
}
