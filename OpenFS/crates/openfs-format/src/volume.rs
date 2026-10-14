//! Canonical volume layout with a reserved persistent journal.
//!
//! The journal reserves two checkpoint blocks and a four-block redo ring before the
//! data region, and protects allocator bitmap changes.

use crate::{
    crc32c, decode_directory_entries, decode_metadata_records, format_journal, plan_recovery,
    DirectoryEntry, ExtentMap, JournalCheckpoint, JournalRecord, JournalRecordKind, JournalRegion, MetadataRecord,
    Namespace, ObjectId, PersistentInventoryDescriptor, PersistentJournal, RedoUpdate,
    DIRECTORY_ENTRY_SIZE, INVENTORY_DESCRIPTOR_MAGIC, JOURNAL_CHECKPOINT_SIZE,
    METADATA_RECORD_SIZE, SUPERBLOCK_SIZE,
};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

const MAGIC: &[u8; 8] = b"OPENFS\0\0";
const VERSION: u16 = 2;
const HEADER_SIZE: u16 = 48;
const CHECKSUM_OFFSET: usize = 4088;
const JOURNAL_RING_BLOCKS: u64 = 4;
const JOURNAL_RESERVED_BLOCKS: u64 = 2 + JOURNAL_RING_BLOCKS;
const INVENTORY_DISCOVERY_BATCH_BLOCKS: usize = 64;
const INVENTORY_BITMAP_CACHE_PAGES: usize = 8;

/// On-disk regions for format canonical. The journal occupies six consecutive blocks.
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
        let data_start = journal_start
            .checked_add(JOURNAL_RESERVED_BLOCKS)
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
                ring_blocks: JOURNAL_RING_BLOCKS,
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
            || (block >= self.bitmap_start && block < self.bitmap_start + self.bitmap_blocks)
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
    // Cache descriptor discovery for allocator guards. None means not checked yet;
    // Some(None) means checked and absent; Some(Some(...)) is the validated locator.
    inventory_descriptor_cache: Option<Option<(u64, PersistentInventoryDescriptor)>>,
}

impl OpenedVolume {
    /// Allocate and journal the bitmap update before exposing the new allocation.
    pub fn allocate_block<D: BlockDevice>(&mut self, device: &mut D) -> Result<u64> {
        self.validate_device(device)?;
        let block_size = self.volume.block_size as usize;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let first_bitmap = self.volume.layout.data_start / bits_per_block;
        let last_data_block = self.volume.layout.backup_superblock - 1;
        let last_bitmap = last_data_block / bits_per_block;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_block = self.volume.layout.bitmap_start + bitmap_index;
            let mut bitmap = vec![0_u8; block_size];
            device.read_blocks(bitmap_block, &mut bitmap)?;

            let bitmap_base = bitmap_index * bits_per_block;
            let first_block = bitmap_base.max(self.volume.layout.data_start);
            // Saturate the exclusive end: valid volume byte geometry does not imply
            // that the bitmap's wider bit-index range fits in u64.
            let end_block = bitmap_base
                .saturating_add(bits_per_block)
                .min(self.volume.layout.backup_superblock);
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(local_end.saturating_add(7) / 8)
                .map_err(|_| Error::ArithmeticOverflow)?;
            for (byte_index, byte) in bitmap
                .iter_mut()
                .enumerate()
                .take(end_byte)
                .skip(first_byte)
            {
                let byte_start = (byte_index as u64) * 8;
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
                    let local = byte_start + bit;
                    let block = bitmap_base + local;
                    // A failed descriptor publication may have staged descriptor bytes in
                    // a still-free block. Clear that reserved magic before exposing the
                    // block as a different allocation, otherwise discovery could mistake
                    // stale bytes for a live descriptor.
                    let mut existing = vec![0_u8; block_size];
                    device.read_blocks(block, &mut existing)?;
                    if existing.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
                        existing.fill(0);
                        device.write_blocks(block, &existing)?;
                        device.flush()?;
                    }
                    *byte |= 1_u8 << bit;
                    self.journal.commit(
                        device,
                        &[RedoUpdate {
                            target_block: bitmap_block,
                            contents: bitmap.clone(),
                        }],
                    )?;
                    return Ok(block);
                }
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
        let bitmap_block = self.volume.layout.bitmap_start + block / bits_per_block;
        let local = block % bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
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
        let last_data_block = self.volume.layout.backup_superblock - 1;
        let last_bitmap = last_data_block / bits_per_block;
        let mut free = 0_u64;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_block = self.volume.layout.bitmap_start + bitmap_index;
            let mut bitmap = vec![0_u8; self.volume.block_size as usize];
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let bitmap_base = bitmap_index * bits_per_block;
            let first_block = bitmap_base.max(self.volume.layout.data_start);
            let end_block = bitmap_base
                .saturating_add(bits_per_block)
                .min(self.volume.layout.backup_superblock);
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(local_end.saturating_add(7) / 8)
                .map_err(|_| Error::ArithmeticOverflow)?;
            for (byte_index, byte) in bitmap.iter().enumerate().take(end_byte).skip(first_byte) {
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
        let bitmap_block = self.volume.layout.bitmap_start + block / bits_per_block;
        let local = block % bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
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
        let Some((_, descriptor)) = self
            .inventory_descriptor_cache
            .as_ref()
            .expect("descriptor cache was initialized")
        else {
            return Ok(());
        };
        if blocks.iter().any(|block| {
            descriptor.metadata_blocks.contains(block)
                || descriptor.directory_blocks.contains(block)
        }) {
            return Err(Error::InvalidInput(
                "cannot free a block referenced by persistent inventory descriptor",
            ));
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
        let Some((_, descriptor)) = self
            .inventory_descriptor_cache
            .as_ref()
            .expect("descriptor cache was initialized")
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
        if self.discover_persistent_inventory(device)?.is_some() {
            return Err(Error::InvalidInput(
                "a persistent inventory descriptor already exists",
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
                let mut bitmap = vec![0_u8; block_size];
                device.read_blocks(self.volume.layout.bitmap_start + bitmap_index, &mut bitmap)?;
                entry.insert(bitmap);
            }
            let bitmap = allocation_bitmaps
                .get(&bitmap_index)
                .expect("bitmap page was cached");
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
        let last_data_block = layout.backup_superblock - 1;
        let last_bitmap = last_data_block / bits_per_bitmap_block;
        let mut selected = None;
        for bitmap_index in first_bitmap..=last_bitmap {
            let bitmap_block = layout.bitmap_start + bitmap_index;
            let mut bitmap = vec![0_u8; block_size];
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let bitmap_base = bitmap_index * bits_per_bitmap_block;
            let first_block = bitmap_base.max(layout.data_start);
            let end_block = bitmap_base
                .saturating_add(bits_per_bitmap_block)
                .min(layout.backup_superblock);
            let local_start = first_block - bitmap_base;
            let local_end = end_block - bitmap_base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(local_end.saturating_add(7) / 8)
                .map_err(|_| Error::ArithmeticOverflow)?;
            let mut candidate = None;
            for (byte_index, byte) in bitmap.iter().enumerate().take(end_byte).skip(first_byte) {
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
        self.inventory_descriptor_cache = Some(Some((descriptor_block, descriptor.clone())));
        Ok(descriptor_block)
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
        let bits_per_bitmap_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_bitmap_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_bitmap_block;
        let mut discovered = None;
        let mut pending_blocks: Vec<u64> = Vec::with_capacity(INVENTORY_DISCOVERY_BATCH_BLOCKS);
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
            let mut batch = vec![0_u8; byte_count];
            device.read_blocks(first_block, &mut batch)?;

            for (offset, block) in blocks.iter().copied().enumerate() {
                let byte_start = offset
                    .checked_mul(block_size)
                    .ok_or(Error::ArithmeticOverflow)?;
                let block_bytes = &batch[byte_start..byte_start + block_size];
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
                for inventory_block in descriptor
                    .metadata_blocks
                    .iter()
                    .chain(descriptor.directory_blocks.iter())
                {
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
                        let mut bitmap = vec![0_u8; block_size];
                        device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
                        entry.insert(bitmap);
                    }
                    let bitmap = allocation_bitmaps
                        .get(&bitmap_index)
                        .expect("bitmap page was cached");
                    let local_bit = *inventory_block % bits_per_bitmap_block;
                    let byte_index =
                        usize::try_from(local_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
                    if bitmap[byte_index] & (1_u8 << (local_bit % 8)) == 0 {
                        return Err(Error::Corrupt(
                            "inventory descriptor references a free inventory block",
                        ));
                    }
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
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_bitmap_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_bitmap_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; block_size];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_index, bitmap_byte) in
                bitmap.iter().enumerate().take(end_byte).skip(first_byte)
            {
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
        for root in &roots {
            if !self.is_data_block_allocated(device, *root)? {
                return Err(Error::Corrupt(
                    "extent-map root is marked free in the allocation bitmap",
                ));
            }
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
        let mut bytes = vec![0_u8; self.volume.block_size as usize];
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
        self.validate_extent_allocations(device, &map, logical_blocks)?;
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

        let mut output = vec![0_u8; read_len];
        let mut block_buffer = vec![0_u8; self.volume.block_size as usize];
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
            let destination_start =
                usize::try_from(copy_start - offset).map_err(|_| Error::ArithmeticOverflow)?;
            let destination_end =
                usize::try_from(copy_end - offset).map_err(|_| Error::ArithmeticOverflow)?;
            let within_start =
                usize::try_from(copy_start - block_start).map_err(|_| Error::ArithmeticOverflow)?;
            let within_end =
                usize::try_from(copy_end - block_start).map_err(|_| Error::ArithmeticOverflow)?;

            if let Some(physical) = map.and_then(|value| value.physical_block(logical)) {
                if physical < self.volume.layout.data_start
                    || physical >= self.volume.layout.backup_superblock
                {
                    return Err(Error::Corrupt("file extent points outside the data region"));
                }
                if !self.is_data_block_allocated(device, physical)? {
                    return Err(Error::Corrupt("file extent points to a free data block"));
                }
                device.read_blocks(physical, &mut block_buffer)?;
                output[destination_start..destination_end]
                    .copy_from_slice(&block_buffer[within_start..within_end]);
            }
            // Unmapped logical blocks remain zero-filled (sparse-file semantics).
        }
        Ok(output)
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
            let map = self.read_extent_map(device, root, record.size)?;
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
            if pair[0].1 > pair[1].0 && pair[0].2 != pair[1].2 {
                return Err(Error::Corrupt(
                    "files claim overlapping physical data blocks",
                ));
            }
        }
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
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        // validated_physical_ranges returns ranges in ascending physical order. Therefore
        // the bitmap page index can only stay the same or increase as we scan; one-page
        // caching is sufficient and avoids repeated BTreeMap lookups and cache eviction
        // for highly fragmented files.
        let mut cached_bitmap_index = None;
        let mut cached_bitmap = vec![0_u8; self.volume.block_size as usize];
        for (start, end) in ranges {
            for block in start..end {
                let bitmap_index = block / bits_per_block;
                if cached_bitmap_index != Some(bitmap_index) {
                    device.read_blocks(
                        self.volume.layout.bitmap_start + bitmap_index,
                        &mut cached_bitmap,
                    )?;
                    cached_bitmap_index = Some(bitmap_index);
                }
                let local = block % bits_per_block;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                if cached_bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                    return Err(Error::Corrupt("file extent points to a free data block"));
                }
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
                let mut bitmap = vec![0_u8; block_size];
                device.read_blocks(self.volume.layout.bitmap_start + bitmap_index, &mut bitmap)?;
                entry.insert(bitmap);
            }
            let bitmap = allocation_bitmaps
                .get(&bitmap_index)
                .expect("bitmap page was cached");
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
        let mut packed = Vec::with_capacity(required_blocks * block_size);
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
            let mut batch = vec![0_u8; run_bytes];
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
        let canonical = VolumeLayout::calculate(self.volume.block_size, self.volume.total_blocks)?;
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
    if primary != backup {
        return Err(Error::Corrupt("primary and backup superblocks disagree"));
    }
    let (block_size, total_blocks, _features, _uuid) = decode_superblock(&primary)?;
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "superblock geometry does not match device geometry",
        ));
    }
    let layout = VolumeLayout::calculate(block_size, total_blocks)?;
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
    for bitmap_index in first_bitmap..=last_bitmap {
        let bitmap_block = layout.bitmap_start + bitmap_index;
        let mut bitmap = vec![0_u8; block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let base = bitmap_index * bits_per_bitmap_block;
        let first = layout.data_start.max(base);
        let end = layout
            .backup_superblock
            .min(base.saturating_add(bits_per_bitmap_block));
        let first_byte =
            usize::try_from((first - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
        let end_byte = usize::try_from((end - base).saturating_add(7) / 8)
            .map_err(|_| Error::ArithmeticOverflow)?;
        for (byte_index, byte) in bitmap.iter().enumerate().take(end_byte).skip(first_byte) {
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
    let data_blocks = layout.data_blocks();
    if free_data_blocks > data_blocks {
        return Err(Error::Corrupt("free data block count exceeds data region"));
    }
    Ok(VolumeCheckReport {
        block_size,
        total_blocks,
        data_blocks,
        allocated_data_blocks: data_blocks - free_data_blocks,
        free_data_blocks,
        journal_generation,
        journal_checkpoint_sequence,
        pending_journal_records,
        recoverable_transactions,
    })
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
        let mut block = vec![0_u8; device.block_size() as usize];
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
            if a.generation > b.generation
                && a.generation.checked_sub(b.generation) != Some(1) =>
        {
            return Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && b.generation.checked_sub(a.generation) != Some(1) =>
        {
            return Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ));
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(a.generation == 2 && b.generation == 1
                    && a.checkpoint_sequence == 0 && a.next_transaction_id == 1
                    && b.next_transaction_id == 1) =>
        {
            return Err(Error::Corrupt(
                "checkpoint generation advanced without a durable journal record",
            ));
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(b.generation == 2 && a.generation == 1
                    && a.checkpoint_sequence == 0 && a.next_transaction_id == 1
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
    let mut malformed = Vec::<u64>::new();
    for slot in 0..region.ring_blocks {
        let mut block = vec![0_u8; block_size];
        device.read_blocks(region.ring_start + slot, &mut block)?;
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
            Err(_) => malformed.push(slot),
        }
    }
    newer.sort_by_key(|(_, record)| record.sequence);
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
        if malformed.iter().any(|slot| slot < last_slot) {
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
    let records: Vec<JournalRecord> = newer.into_iter().map(|(_, record)| record).collect();
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
    for index in 0..layout.bitmap_blocks {
        let first = index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = vec![0_u8; block_size];
        for block in first..first.saturating_add(bits_per_block) {
            if block >= layout.total_blocks
                || block < layout.data_start
                || block == layout.backup_superblock
            {
                let local = block - first;
                bitmap[(local / 8) as usize] |= 1 << (local % 8);
            }
        }
        device.write_blocks(layout.bitmap_start + index, &bitmap)?;
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
    if primary != backup {
        return Err(Error::Corrupt("primary and backup superblocks disagree"));
    }
    let (block_size, total_blocks, features, uuid) = decode_superblock(&primary)?;
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "superblock geometry does not match device geometry",
        ));
    }
    let layout = VolumeLayout::calculate(block_size, total_blocks)?;
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
        inventory_descriptor_cache: None,
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
    if version != VERSION {
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
    if bytes[48..CHECKSUM_OFFSET].iter().any(|b| *b != 0) || bytes[4092..].iter().any(|b| *b != 0) {
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

fn read_superblock<D: BlockDevice>(device: &mut D, block: u64) -> Result<[u8; SUPERBLOCK_SIZE]> {
    let mut buffer = vec![0; device.block_size() as usize];
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
    let mut buffer = vec![0; device.block_size() as usize];
    if buffer.len() < SUPERBLOCK_SIZE {
        return Err(Error::InvalidInput(
            "device block is smaller than superblock",
        ));
    }
    buffer[..SUPERBLOCK_SIZE].copy_from_slice(encoded);
    device.write_blocks(block, &buffer)
}

fn validate_bitmap<D: BlockDevice>(device: &mut D, layout: &VolumeLayout) -> Result<()> {
    let block_size = layout.block_size as usize;
    let bits_per_block = u64::from(layout.block_size) * 8;
    for index in 0..layout.bitmap_blocks {
        let first = index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = vec![0; block_size];
        device.read_blocks(layout.bitmap_start + index, &mut bitmap)?;
        for block in first..first + bits_per_block {
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
        check_volume, decode_superblock, encode_superblock, format_volume, open_volume,
        VolumeLayout,
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
            assert!(matches!(
                volume.publish_persistent_inventory(&mut device, &descriptor),
                Err(Error::InvalidInput(
                    "a persistent inventory descriptor already exists"
                ))
            ));

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
            let mut prefixes: Vec<usize> =
                (1..=crate::INVENTORY_DESCRIPTOR_MAGIC.len()).collect();
            prefixes.extend([
                48,
                block_size as usize / 2,
                block_size as usize - 1,
            ]);
            prefixes.sort_unstable();
            prefixes.dedup();
            for prefix_len in prefixes {
                let path = image_path(&format!(
                    "inventory-torn-prefix-{block_size}-{prefix_len}"
                ));
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
                    let mut volume =
                        format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
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
                    metadata_bytes[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&metadata_record);
                    device.write_blocks(metadata_block, &metadata_bytes).unwrap();

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
                    device.read_blocks(metadata_block, &mut persisted_metadata).unwrap();
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
                    let mut volume = open_volume(&mut device).unwrap();
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
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};

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
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};
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
            volume.free_block(&mut device, data_block).unwrap();
            assert!(matches!(
                volume.read_file_range(&mut device, &namespace, &maps, ObjectId(2), 0, 8),
                Err(Error::Corrupt("file extent points to a free data block"))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn reads_persistent_extent_maps_and_checks_bitmap_ownership() {
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};

        let path = image_path("persistent-extent-map-read");
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
                name: "payload.bin".to_owned(),
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
            let map = ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: data_block,
                block_count: 1,
            }])
            .unwrap();
            device
                .write_blocks(extent_root, &map.encode_block(4096).unwrap())
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
            let loaded = volume
                .read_namespace_extent_maps(&mut device, &namespace)
                .unwrap();
            assert_eq!(loaded.get(&ObjectId(2)).unwrap(), &map);

            volume.free_block(&mut device, data_block).unwrap();
            assert!(matches!(
                volume.read_namespace_extent_maps(&mut device, &namespace),
                Err(Error::Corrupt("file extent points to a free data block"))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn namespace_extent_maps_reject_cross_file_physical_aliases() {
        use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};

        let path = image_path("persistent-extent-map-alias");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
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
                size: 4096,
                extent_root_block: Some(first_root),
                generation: 1,
            };
            let second = MetadataRecord {
                id: ObjectId(3),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 4096,
                extent_root_block: Some(second_root),
                generation: 1,
            };
            let mut metadata_bytes = vec![0_u8; 4096];
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
            let mut directory_bytes = vec![0_u8; 4096];
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
            .encode_block(4096)
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
                    "files claim overlapping physical data blocks"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
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
    fn canonical_superblock_has_stable_golden_bytes_and_checksum() {
        let uuid = *b"openfs-test-0001";
        let features = 0x0102_0304_0506_0708;
        let encoded = encode_superblock(4096, 256, features, uuid).unwrap();

        assert_eq!(
            &encoded[..48],
            &[
                79, 80, 69, 78, 70, 83, 0, 0, 2, 0, 48, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 8,
                7, 6, 5, 4, 3, 2, 1, 111, 112, 101, 110, 102, 115, 45, 116, 101, 115, 116, 45, 48,
                48, 48, 49
            ]
        );
        assert_eq!(
            u32::from_le_bytes(encoded[4088..4092].try_into().unwrap()),
            0x2951_902d
        );
        assert!(encoded[48..4088].iter().all(|byte| *byte == 0));
        assert!(encoded[4092..].iter().all(|byte| *byte == 0));
        assert_eq!(
            decode_superblock(&encoded).unwrap(),
            (4096, 256, features, uuid)
        );
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
                Err(Error::UnsupportedVersion(2))
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
                12,
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
                let mut device = ImageDevice::create(&path, block_size, 12).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                assert_eq!(layout.data_blocks(), 3);

                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                let third = volume.allocate_block(&mut device).unwrap();
                assert_eq!([first, second, third], [8, 9, 10]);
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
                assert_eq!(reopened.volume.layout.data_start, 8);
                assert_eq!(reopened.volume.layout.backup_superblock, 11);
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
                    let recovered = open_volume(&mut device).unwrap();
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
                    }

                    // Flush errors can also leave an uncertain commit outcome. Check
                    // that replaying the image again preserves the same old-or-complete state.
                    drop(recovered);
                    drop(device);
                    let mut device = ImageDevice::open(&path, block_size).unwrap();
                    let recovered = open_volume(&mut device).unwrap();
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
            for fail_at in 0..4 {
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
            for fail_at in [4, 5] {
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
                        matches!(
                            open_volume(&mut device),
                            Err(Error::Corrupt("primary and backup superblocks disagree"))
                        ),
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
            ("superblock-reserved-header-byte", 48_usize),
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
            (0..8).chain(std::iter::once(255)).collect::<Vec<_>>()
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
            let layout = VolumeLayout::calculate(block_size, 12).unwrap();
            let mut corruptions = (0..layout.total_blocks)
                .filter(|block| layout.is_reserved(*block))
                .map(|block| ("reserved", block))
                .collect::<Vec<_>>();
            // Include the first padding bit in the same durable-replay matrix.
            corruptions.push(("padding", layout.total_blocks));
            for (kind, corrupted_block) in corruptions {
                let path = image_path(&format!("{kind}-replay-{block_size}-{corrupted_block}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 12).unwrap();
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
                let mut device = ImageDevice::create(&path, block_size, 12).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let layout = volume.volume.layout;
                assert_eq!(layout.data_blocks(), 3);

                let allocated: Vec<_> = (0..layout.data_blocks())
                    .map(|_| volume.allocate_block(&mut device).unwrap())
                    .collect();
                assert_eq!(allocated, vec![8, 9, 10]);
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
                (layout.journal.checkpoint_a, JournalCheckpoint {
                    generation: 4, checkpoint_sequence: 3, next_sequence: 4,
                    next_transaction_id: 2, head_block: 0, tail_block: 0,
                }),
                (layout.journal.checkpoint_b, JournalCheckpoint {
                    generation: 2, checkpoint_sequence: 0, next_sequence: 1,
                    next_transaction_id: 1, head_block: 0, tail_block: 0,
                }),
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
                generation: 2, checkpoint_sequence: 9, next_sequence: 10,
                next_transaction_id: 4, head_block: 0, tail_block: 0,
            };
            let older_encoded = older.encode().unwrap();
            let mut older_block = vec![0_u8; 4096];
            older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
            device.write_blocks(layout.journal.checkpoint_b, &older_block).unwrap();

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
            device.write_blocks(layout.journal.checkpoint_a, &checkpoint_block).unwrap();
            let older = JournalCheckpoint {
                generation: 2, checkpoint_sequence: 9, next_sequence: 10,
                next_transaction_id: 4, head_block: 0, tail_block: 0,
            };
            let older_encoded = older.encode().unwrap();
            let mut older_block = vec![0_u8; 4096];
            older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
            device.write_blocks(layout.journal.checkpoint_b, &older_block).unwrap();

            // ID six is greater than the checkpoint's next ID, but ID five
            // cannot be skipped by the single-writer journal protocol.
            let record = JournalRecord {
                kind: JournalRecordKind::Begin,
                sequence: 13,
                transaction_id: 6,
                payload: Vec::new(),
            };
            device.write_blocks(
                layout.journal.ring_start,
                &record.encode_block(4096).unwrap(),
            ).unwrap();
            device.flush().unwrap();

            assert!(matches!(check_volume(&mut device), Err(Error::Corrupt(_))));
        }
        fs::remove_file(&path).unwrap();
    }

    #[test]
    fn canonical_layout_regions_are_disjoint_across_supported_geometries() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for total_blocks in [12_u64, 13, 255, 256, 4097, 65_537, 1_000_003] {
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
        let minimum = VolumeLayout::calculate(4096, 12).unwrap();
        assert_eq!(minimum.bitmap_start, 1);
        assert_eq!(minimum.bitmap_blocks, 1);
        assert_eq!(minimum.journal.checkpoint_a, 2);
        assert_eq!(minimum.journal.checkpoint_b, 3);
        assert_eq!(minimum.journal.ring_start, 4);
        assert_eq!(minimum.journal.ring_blocks, 4);
        assert_eq!(minimum.data_start, 8);
        assert_eq!(minimum.backup_superblock, 11);
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
                VolumeLayout::calculate(block_size, 12),
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
    fn rejecting_descriptor_replacement_preserves_existing_image_at_every_block_size() {
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
                device.write_blocks(metadata_block, &metadata_bytes).unwrap();

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

                assert!(
                    matches!(
                        volume.publish_persistent_inventory(&mut device, &descriptor),
                        Err(Error::InvalidInput(
                            "a persistent inventory descriptor already exists"
                        ))
                    ),
                    "replacement must be refused at block size {block_size}"
                );
                drop(volume);
                drop(device);

                let bytes_after = std::fs::read(&path).unwrap();
                assert_eq!(
                    bytes_after, bytes_before,
                    "rejected replacement must preserve every image byte at block size {block_size}"
                );

                let mut reopened_device = ImageDevice::open(&path).unwrap();
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

}
