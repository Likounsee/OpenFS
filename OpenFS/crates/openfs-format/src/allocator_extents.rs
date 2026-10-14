//! Atomic contiguous allocation helpers for volumes.
//!
//! The current on-disk journal can atomically update one bitmap block per transaction.
//! Extents are therefore deliberately constrained to a single bitmap segment.

use crate::{OpenedVolume, RedoUpdate, VolumeLayout, INVENTORY_DESCRIPTOR_MAGIC};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

/// Clear stale descriptor signatures from free blocks before making them allocated.
///
/// A failed descriptor publication may leave a staged descriptor in a block whose
/// allocation bit was never committed. Every allocator path must scrub that signature
/// before reusing such a block, or discovery can mistake stale bytes for a live root.
fn clear_stale_descriptor_magic<D: BlockDevice>(
    device: &mut D,
    block_size: u32,
    blocks: &[u64],
) -> Result<()> {
    let mut changed = false;
    let mut contents = vec![0_u8; block_size as usize];
    for block in blocks {
        device.read_blocks(*block, &mut contents)?;
        if contents.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
            contents.fill(0);
            device.write_blocks(*block, &contents)?;
            changed = true;
        }
    }
    if changed {
        // Scrubbing must be durable before the bitmap commit makes these blocks
        // visible to descriptor discovery as allocated data.
        device.flush()?;
    }
    Ok(())
}

/// Read-only snapshot of usable data-space capacity.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AllocationStats {
    pub total_data_blocks: u64,
    pub allocated_data_blocks: u64,
    pub free_data_blocks: u64,
}

/// One currently free contiguous run, constrained to a single bitmap segment.
///
/// The run can be used as a placement hint; it is not reserved and may change after
/// another allocator operation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FreeExtent {
    pub start_block: u64,
    pub block_count: u64,
}

/// Read-only fragmentation summary for free space in the data region.
///
/// A free extent is a consecutive run of free data blocks within one bitmap segment,
/// matching the current allocator's single-bitmap-block transaction limit.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FreeExtentStats {
    pub free_extent_count: u64,
    pub largest_free_extent_blocks: u64,
}

/// Bounded diagnostics for reserved or out-of-volume bits incorrectly marked free.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AllocationBitmapReport {
    /// Number of usable data blocks currently marked free.
    pub free_data_block_count: u64,
    /// Number of usable data blocks currently marked allocated.
    pub allocated_data_block_count: u64,
    /// Total invalid free bits found, including those beyond the returned sample.
    pub invalid_free_block_count: u64,
    /// Lowest offending block numbers, capped at 64 entries.
    pub first_invalid_free_blocks: Vec<u64>,
    /// True when more invalid block numbers exist than the sample contains.
    pub truncated: bool,
}

/// Detailed result of comparing persistent block ownership with the allocation bitmap.
///
/// Orphaned blocks are marked allocated but absent from the supplied ownership inventory.
/// Dangling references are present in the inventory but marked free in the bitmap.
/// Maximum number of block numbers retained in each ownership-diagnostic sample.
pub const OWNERSHIP_DIAGNOSTIC_SAMPLE_LIMIT: usize = 64;

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AllocationOwnershipReport {
    /// Sample of allocated blocks absent from the supplied ownership inventory.
    pub orphaned_allocations: Vec<u64>,
    /// Sample of referenced blocks marked free in the allocation bitmap.
    pub dangling_references: Vec<u64>,
    /// Total orphaned allocated blocks, including entries beyond the sample.
    pub orphaned_allocation_count: u64,
    /// Total dangling references, including entries beyond the sample.
    pub dangling_reference_count: u64,
    /// True when additional orphaned block numbers were omitted from the sample.
    pub orphaned_allocations_truncated: bool,
    /// True when additional dangling block numbers were omitted from the sample.
    pub dangling_references_truncated: bool,
}

impl AllocationOwnershipReport {
    /// Return true when every allocated data block has exactly one supplied owner.
    ///
    /// Consistency uses the total counts, not the bounded diagnostic samples.
    pub fn is_consistent(&self) -> bool {
        self.orphaned_allocation_count == 0 && self.dangling_reference_count == 0
    }

    fn record_orphan(&mut self, block: u64) -> Result<()> {
        self.orphaned_allocation_count = self
            .orphaned_allocation_count
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        if self.orphaned_allocations.len() < OWNERSHIP_DIAGNOSTIC_SAMPLE_LIMIT {
            self.orphaned_allocations.push(block);
        } else {
            self.orphaned_allocations_truncated = true;
        }
        Ok(())
    }

    fn record_dangling_reference(&mut self, block: u64) -> Result<()> {
        self.dangling_reference_count = self
            .dangling_reference_count
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        if self.dangling_references.len() < OWNERSHIP_DIAGNOSTIC_SAMPLE_LIMIT {
            self.dangling_references.push(block);
        } else {
            self.dangling_references_truncated = true;
        }
        Ok(())
    }
}

impl OpenedVolume {
    /// Return total, allocated, and free data-block counts from the current bitmap.
    pub fn allocation_stats<D: BlockDevice>(&self, device: &mut D) -> Result<AllocationStats> {
        let report = self.allocation_bitmap_report(device)?;
        if report.invalid_free_block_count != 0 {
            return Err(Error::Corrupt(
                "allocation bitmap marks a reserved or padding block free",
            ));
        }
        let total = self.volume.layout.data_blocks();
        let accounted = report
            .free_data_block_count
            .checked_add(report.allocated_data_block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        if accounted != total {
            return Err(Error::Corrupt(
                "allocation bitmap data-block counts do not match the usable data region",
            ));
        }
        Ok(AllocationStats {
            total_data_blocks: total,
            allocated_data_blocks: report.allocated_data_block_count,
            free_data_blocks: report.free_data_block_count,
        })
    }

    /// Allocate several data blocks atomically and return their block numbers.
    ///
    /// All selected blocks are represented by one bitmap home-block image, so this
    /// operation is bounded to one bitmap segment and one journal redo update.
    pub fn allocate_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        count: u64,
    ) -> Result<Vec<u64>> {
        validate_device(self, device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if count == 0 || count > bits_per_block {
            return Err(Error::InvalidInput(
                "batch allocation count must fit within one bitmap block",
            ));
        }
        let layout = self.volume.layout;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        for index in first_bitmap..=last_bitmap {
            let base = index * bits_per_block;
            let start = layout.data_start.max(base);
            let end = layout
                .backup_superblock
                .min(base.saturating_add(bits_per_block));
            if end - start < count {
                continue;
            }
            let bitmap_block = layout.bitmap_start + index;
            let mut bitmap = vec![0_u8; self.volume.block_size as usize];
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let mut selected = Vec::with_capacity(count as usize);
            for block in start..end {
                let local = block - base;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                let mask = 1_u8 << (local % 8);
                if bitmap[byte] & mask == 0 {
                    selected.push(block);
                    if selected.len() as u64 == count {
                        break;
                    }
                }
            }
            if selected.len() as u64 != count {
                continue;
            }
            // Refresh the bitmap before touching candidate contents. A second opener may
            // have allocated one of these blocks since the initial free-space scan.
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let mut selection_changed = false;
            for block in &selected {
                let local = *block - base;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                if bitmap[byte] & (1_u8 << (local % 8)) != 0 {
                    selection_changed = true;
                    break;
                }
            }
            if selection_changed {
                return Err(Error::Corrupt(
                    "batch allocation candidates changed during allocation",
                ));
            }
            // Staged descriptors left by interrupted publication must be scrubbed
            // before this allocation becomes visible in the bitmap.
            clear_stale_descriptor_magic(device, self.volume.block_size, &selected)?;
            for block in &selected {
                let local = block - base;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                bitmap[byte] |= 1_u8 << (local % 8);
            }
            self.journal.commit(
                device,
                &[RedoUpdate {
                    target_block: bitmap_block,
                    contents: bitmap,
                }],
            )?;
            return Ok(selected);
        }
        Err(Error::InvalidInput(
            "no bitmap segment has enough free data blocks for this batch",
        ))
    }

    /// Free several data blocks atomically using one bitmap redo image.
    ///
    /// Duplicate blocks, non-data blocks, blocks in different bitmap segments, and
    /// already-free blocks are rejected before any mutation is journaled.
    pub fn free_blocks<D: BlockDevice>(&mut self, device: &mut D, blocks: &[u64]) -> Result<()> {
        validate_device(self, device)?;
        if blocks.is_empty() {
            return Err(Error::InvalidInput("batch free list must not be empty"));
        }
        let layout = self.volume.layout;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let mut sorted = blocks.to_vec();
        sorted.sort_unstable();
        sorted.dedup();
        if sorted.len() != blocks.len() {
            return Err(Error::InvalidInput("batch free list contains duplicates"));
        }
        if sorted
            .iter()
            .any(|block| *block < layout.data_start || *block >= layout.backup_superblock)
        {
            return Err(Error::InvalidInput("batch free contains a non-data block"));
        }
        let bitmap_index = sorted[0] / bits_per_block;
        if sorted
            .iter()
            .any(|block| *block / bits_per_block != bitmap_index)
        {
            return Err(Error::InvalidInput(
                "batch free blocks must belong to one bitmap segment",
            ));
        }
        let bitmap_block = layout.bitmap_start + bitmap_index;
        let base = bitmap_index * bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in &sorted {
            let local = *block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let mask = 1_u8 << (local % 8);
            if bitmap[byte] & mask == 0 {
                return Err(Error::InvalidInput(
                    "batch free contains an already-free block",
                ));
            }
        }
        self.ensure_inventory_blocks_releasable(device, &sorted)?;
        for block in &sorted {
            crate::inventory_descriptor::reject_freeing_inventory_descriptor(
                device,
                *block,
                self.volume.block_size,
            )?;
        }
        for block in &sorted {
            let local = *block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            bitmap[byte] &= !(1_u8 << (local % 8));
        }
        self.journal
            .commit(
                device,
                &[RedoUpdate {
                    target_block: bitmap_block,
                    contents: bitmap,
                }],
            )
            .map(|_| ())
    }

    /// Free a contiguous data extent atomically with one bitmap journal update.
    ///
    /// The entire range must be allocated, contained in the data region, and represented
    /// by one bitmap block. Invalid ranges are rejected before the bitmap is journaled.
    pub fn free_contiguous_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        start_block: u64,
        count: u64,
    ) -> Result<()> {
        validate_device(self, device)?;
        if count == 0 {
            return Err(Error::InvalidInput(
                "contiguous free extent length must be nonzero",
            ));
        }
        let layout = self.volume.layout;
        let end_block = start_block
            .checked_add(count)
            .ok_or(Error::ArithmeticOverflow)?;
        if start_block < layout.data_start || end_block > layout.backup_superblock {
            return Err(Error::InvalidInput(
                "contiguous free extent contains a non-data block",
            ));
        }
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if start_block / bits_per_block != (end_block - 1) / bits_per_block {
            return Err(Error::InvalidInput(
                "contiguous free extent must belong to one bitmap segment",
            ));
        }
        let bitmap_index = start_block / bits_per_block;
        let bitmap_block = layout.bitmap_start + bitmap_index;
        let base = bitmap_index * bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in start_block..end_block {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                return Err(Error::InvalidInput(
                    "contiguous free extent contains an already-free block",
                ));
            }
        }
        self.ensure_inventory_range_releasable(device, start_block, end_block)?;
        crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
            device,
            start_block,
            end_block,
            self.volume.block_size,
        )?;
        for block in start_block..end_block {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            bitmap[byte] &= !(1_u8 << (local % 8));
        }
        self.journal
            .commit(
                device,
                &[RedoUpdate {
                    target_block: bitmap_block,
                    contents: bitmap,
                }],
            )
            .map(|_| ())
    }

    /// Find the first free contiguous extent wholly contained in one bitmap block.
    /// Returns its first data block, or None if no extent of this size exists.
    pub fn find_contiguous_free_blocks<D: BlockDevice>(
        &self,
        device: &mut D,
        count: u64,
    ) -> Result<Option<u64>> {
        validate_device(self, device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if count == 0 || count > bits_per_block {
            return Err(Error::InvalidInput(
                "contiguous extent length must fit within one bitmap block",
            ));
        }
        let layout = self.volume.layout;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        for index in first_bitmap..=last_bitmap {
            let mut bitmap = vec![0_u8; self.volume.block_size as usize];
            device.read_blocks(layout.bitmap_start + index, &mut bitmap)?;
            let base = index * bits_per_block;
            let start = layout.data_start.max(base);
            let end = layout
                .backup_superblock
                .min(base.saturating_add(bits_per_block));
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut run_start = start;
            let mut run_len = 0_u64;
            let mut previous_free = None;
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                while free_bits != 0 {
                    let bit = u64::from(free_bits.trailing_zeros());
                    let block = base
                        .checked_add(byte_start)
                        .and_then(|value| value.checked_add(bit))
                        .ok_or(Error::ArithmeticOverflow)?;
                    if previous_free.and_then(|previous: u64| previous.checked_add(1))
                        == Some(block)
                    {
                        run_len = run_len.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
                    } else {
                        run_start = block;
                        run_len = 1;
                    }
                    if run_len >= count {
                        return Ok(Some(run_start));
                    }
                    previous_free = Some(block);
                    free_bits &= free_bits - 1;
                }
            }
        }
        Ok(None)
    }

    /// Atomically allocate a contiguous extent and return its first data block.
    /// The extent must fit within one bitmap block to fit the bounded canonical journal.
    pub fn allocate_contiguous_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        count: u64,
    ) -> Result<u64> {
        validate_device(self, device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if count == 0 || count > bits_per_block {
            return Err(Error::InvalidInput(
                "contiguous extent length must fit within one bitmap block",
            ));
        }
        let start = self
            .find_contiguous_free_blocks(device, count)?
            .ok_or(Error::InvalidInput("no contiguous free data extent"))?;
        let end = start.checked_add(count).ok_or(Error::ArithmeticOverflow)?;
        if start / bits_per_block != (end - 1) / bits_per_block {
            return Err(Error::Corrupt(
                "contiguous allocation crosses an allocation bitmap segment",
            ));
        }
        let bitmap_block = self.volume.layout.bitmap_start + start / bits_per_block;
        let base = (start / bits_per_block) * bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in start..end {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte] & (1_u8 << (local % 8)) != 0 {
                return Err(Error::Corrupt(
                    "contiguous extent changed during allocation",
                ));
            }
        }
        // Validate the entire reservation before touching block contents. If the bitmap
        // changed since the free-run scan, fail without clearing a block that is now owned.
        let selected: Vec<u64> = (start..end).collect();
        clear_stale_descriptor_magic(device, self.volume.block_size, &selected)?;
        for block in start..end {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            bitmap[byte] |= 1_u8 << (local % 8);
        }
        self.journal.commit(
            device,
            &[RedoUpdate {
                target_block: bitmap_block,
                contents: bitmap,
            }],
        )?;
        Ok(start)
    }

    /// Return every currently free data block in ascending block-number order.
    ///
    /// This is a read-only allocator inventory, not a promise that all listed blocks can be
    /// allocated together: each journaled batch must fit within one bitmap segment.
    pub fn free_data_block_inventory<D: BlockDevice>(&self, device: &mut D) -> Result<Vec<u64>> {
        validate_device(self, device)?;
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut free = Vec::new();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                while free_bits != 0 {
                    let bit = u64::from(free_bits.trailing_zeros());
                    free.push(
                        base.checked_add(byte_start)
                            .and_then(|block| block.checked_add(bit))
                            .ok_or(Error::ArithmeticOverflow)?,
                    );
                    free_bits &= free_bits - 1;
                }
            }
        }
        Ok(free)
    }

    /// Return free contiguous runs in ascending order, split at bitmap-segment boundaries.
    ///
    /// This read-only inventory is suitable for placement decisions and diagnostics. Returned
    /// extents are snapshots only; they are not reservations, and the current journal limits
    /// atomic allocation to a single bitmap segment.
    pub fn free_extent_inventory<D: BlockDevice>(&self, device: &mut D) -> Result<Vec<FreeExtent>> {
        validate_device(self, device)?;
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut extents = Vec::new();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut current: Option<FreeExtent> = None;
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                while free_bits != 0 {
                    let bit = u64::from(free_bits.trailing_zeros());
                    let block = base
                        .checked_add(byte_start)
                        .and_then(|value| value.checked_add(bit))
                        .ok_or(Error::ArithmeticOverflow)?;
                    match current {
                        Some(mut extent)
                            if extent.start_block.checked_add(extent.block_count)
                                == Some(block) =>
                        {
                            extent.block_count = extent
                                .block_count
                                .checked_add(1)
                                .ok_or(Error::ArithmeticOverflow)?;
                            current = Some(extent);
                        }
                        Some(extent) => {
                            extents.push(extent);
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: 1,
                            });
                        }
                        None => {
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: 1,
                            });
                        }
                    }
                    free_bits &= free_bits - 1;
                }
            }
            if let Some(extent) = current {
                extents.push(extent);
            }
        }
        Ok(extents)
    }

    /// Find the smallest free extent that can satisfy a requested contiguous allocation.
    ///
    /// Ties are resolved by choosing the lower starting block. The result is only a
    /// placement hint: it does not reserve blocks, and callers must still allocate atomically.
    pub fn find_best_fit_free_extent<D: BlockDevice>(
        &self,
        device: &mut D,
        count: u64,
    ) -> Result<Option<FreeExtent>> {
        validate_device(self, device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if count == 0 || count > bits_per_block {
            return Err(Error::InvalidInput(
                "best-fit extent length must fit within one bitmap block",
            ));
        }
        Ok(self
            .free_extent_inventory(device)?
            .into_iter()
            .filter(|extent| extent.block_count >= count)
            .min_by_key(|extent| (extent.block_count, extent.start_block)))
    }

    /// Atomically allocate a contiguous extent from the smallest fitting free run.
    ///
    /// Selection is deterministic (smallest extent, then lowest block number). The selected
    /// bitmap is reread and every target bit is checked before the journal is committed.
    /// Like other allocator transactions, the extent must fit in one bitmap segment.
    pub fn allocate_best_fit_contiguous_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        count: u64,
    ) -> Result<u64> {
        validate_device(self, device)?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        if count == 0 || count > bits_per_block {
            return Err(Error::InvalidInput(
                "best-fit allocation length must fit within one bitmap block",
            ));
        }
        let extent = self
            .find_best_fit_free_extent(device, count)?
            .ok_or(Error::InvalidInput("no contiguous free data extent"))?;
        let end = extent
            .start_block
            .checked_add(count)
            .ok_or(Error::ArithmeticOverflow)?;
        if end > self.volume.layout.backup_superblock
            || extent.start_block / bits_per_block != (end - 1) / bits_per_block
        {
            return Err(Error::Corrupt(
                "best-fit extent crosses an allocation bitmap segment",
            ));
        }
        let bitmap_block = self.volume.layout.bitmap_start + extent.start_block / bits_per_block;
        let base = (extent.start_block / bits_per_block) * bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in extent.start_block..end {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte] & (1_u8 << (local % 8)) != 0 {
                return Err(Error::Corrupt("best-fit extent changed during allocation"));
            }
        }
        // Match every other allocation path: interrupted descriptor staging must not
        // become discoverable when a stale free block is allocated through best-fit.
        let selected: Vec<u64> = (extent.start_block..end).collect();
        clear_stale_descriptor_magic(device, self.volume.block_size, &selected)?;
        for block in extent.start_block..end {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            bitmap[byte] |= 1_u8 << (local % 8);
        }
        self.journal.commit(
            device,
            &[RedoUpdate {
                target_block: bitmap_block,
                contents: bitmap,
            }],
        )?;
        Ok(extent.start_block)
    }

    /// Measure free-space fragmentation without modifying the volume.
    ///
    /// Extents are counted independently per bitmap segment because allocation transactions
    /// cannot currently update multiple bitmap home blocks atomically.
    pub fn free_extent_stats<D: BlockDevice>(&self, device: &mut D) -> Result<FreeExtentStats> {
        validate_device(self, device)?;
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut stats = FreeExtentStats::default();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut run_len = 0_u64;
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                let mut cursor = 0_u32;
                while free_bits != 0 {
                    let run_start = free_bits.trailing_zeros();
                    if run_start > cursor && run_len != 0 {
                        stats.free_extent_count = stats
                            .free_extent_count
                            .checked_add(1)
                            .ok_or(Error::ArithmeticOverflow)?;
                        stats.largest_free_extent_blocks =
                            stats.largest_free_extent_blocks.max(run_len);
                        run_len = 0;
                    }
                    let run_width = (free_bits >> run_start).trailing_ones().min(8 - run_start);
                    if run_start == 0 && cursor == 0 {
                        run_len = run_len
                            .checked_add(u64::from(run_width))
                            .ok_or(Error::ArithmeticOverflow)?;
                    } else {
                        run_len = u64::from(run_width);
                    }
                    cursor = run_start + run_width;
                    let run_mask = (((1_u16 << run_width) - 1) << run_start) as u8;
                    free_bits &= !run_mask;
                    if cursor < 8 {
                        stats.free_extent_count = stats
                            .free_extent_count
                            .checked_add(1)
                            .ok_or(Error::ArithmeticOverflow)?;
                        stats.largest_free_extent_blocks =
                            stats.largest_free_extent_blocks.max(run_len);
                        run_len = 0;
                    }
                }
                if free_bits == 0 && cursor == 0 && run_len != 0 {
                    stats.free_extent_count = stats
                        .free_extent_count
                        .checked_add(1)
                        .ok_or(Error::ArithmeticOverflow)?;
                    stats.largest_free_extent_blocks =
                        stats.largest_free_extent_blocks.max(run_len);
                    run_len = 0;
                }
            }
            if run_len != 0 {
                stats.free_extent_count = stats
                    .free_extent_count
                    .checked_add(1)
                    .ok_or(Error::ArithmeticOverflow)?;
                stats.largest_free_extent_blocks = stats.largest_free_extent_blocks.max(run_len);
            }
        }
        Ok(stats)
    }

    /// Return every currently allocated data block in ascending block-number order.
    ///
    /// This is a bitmap inventory, not an ownership scan: it can enumerate allocations
    /// but cannot determine whether a persistent object actually references each block.
    /// The returned vector uses memory proportional to the number of allocated blocks.
    pub fn allocated_data_block_inventory<D: BlockDevice>(
        &self,
        device: &mut D,
    ) -> Result<Vec<u64>> {
        validate_device(self, device)?;
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut allocated = Vec::new();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
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
                    allocated.push(
                        base.checked_add(byte_start)
                            .and_then(|block| block.checked_add(bit))
                            .ok_or(Error::ArithmeticOverflow)?,
                    );
                    bits &= bits - 1;
                }
            }
        }
        Ok(allocated)
    }

    /// Return detailed bitmap/ownership mismatches for a caller-discovered set of extent maps.
    ///
    /// Each tuple contains an extent map and the owning file's logical block count. Every map
    /// is checked for logical bounds, physical data-region bounds, and internal physical overlap.
    /// The combined inventory is compared with the bitmap. Duplicate owners and invalid block
    /// addresses are rejected; ordinary orphan/dangling mismatches are returned for diagnostics.
    /// This does not discover or decode persistent metadata; callers must supply every live map.
    pub fn extent_map_ownership_report<D: BlockDevice>(
        &self,
        device: &mut D,
        extent_maps: &[(&crate::ExtentMap, u64)],
    ) -> Result<AllocationOwnershipReport> {
        validate_device(self, device)?;
        let layout = self.volume.layout;
        let mut owned_ranges = Vec::new();
        for (map, logical_blocks) in extent_maps {
            owned_ranges.extend(map.validated_physical_ranges(
                *logical_blocks,
                layout.data_start,
                layout.backup_superblock,
            )?);
        }
        owned_ranges.sort_unstable();
        if owned_ranges.windows(2).any(|pair| pair[0].1 > pair[1].0) {
            return Err(Error::Corrupt(
                "a data block is referenced by multiple owners",
            ));
        }

        // Merge adjacent runs across maps too, so the bitmap checker visits each
        // physical ownership run once even when a file boundary splits that run.
        let mut coalesced_ranges: Vec<(u64, u64)> = Vec::with_capacity(owned_ranges.len());
        for (start, end) in owned_ranges {
            if let Some(last) = coalesced_ranges.last_mut() {
                if last.1 == start {
                    last.1 = end;
                    continue;
                }
            }
            coalesced_ranges.push((start, end));
        }
        self.allocation_ownership_range_report(device, &coalesced_ranges)
    }

    /// Compare physical owner ranges against the bitmap without expanding each range to
    /// one entry per block. The ranges must be sorted, non-overlapping, and in the data region.
    fn allocation_ownership_range_report<D: BlockDevice>(
        &self,
        device: &mut D,
        owned_ranges: &[(u64, u64)],
    ) -> Result<AllocationOwnershipReport> {
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        if owned_ranges.iter().any(|(start, end)| {
            start >= end || *start < layout.data_start || *end > layout.backup_superblock
        }) || owned_ranges.windows(2).any(|pair| pair[0].1 > pair[1].0)
        {
            return Err(Error::Corrupt(
                "invalid or overlapping physical owner ranges",
            ));
        }

        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut range_index = 0_usize;
        let mut report = AllocationOwnershipReport::default();

        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let first_byte =
                usize::try_from((start - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(
                end.checked_sub(base)
                    .and_then(|value| value.checked_add(7))
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_index, byte) in bitmap.iter().enumerate().take(end_byte).skip(first_byte) {
                let byte_base = base
                    .checked_add(
                        u64::try_from(byte_index)
                            .map_err(|_| Error::ArithmeticOverflow)?
                            .checked_mul(8)
                            .ok_or(Error::ArithmeticOverflow)?,
                    )
                    .ok_or(Error::ArithmeticOverflow)?;
                let byte_end = byte_base.checked_add(8).ok_or(Error::ArithmeticOverflow)?;
                let first_valid = start.saturating_sub(byte_base).min(8);
                let end_valid = end.saturating_sub(byte_base).min(8);
                let lower_mask = u8::MAX << first_valid;
                let data_upper_mask = if end_valid == 8 {
                    u8::MAX
                } else {
                    (1_u8 << end_valid) - 1
                };
                let data_mask = lower_mask & data_upper_mask;

                while range_index < owned_ranges.len() && owned_ranges[range_index].1 <= byte_base {
                    range_index += 1;
                }
                let mut owner_mask = 0_u8;
                let mut candidate = range_index;
                while candidate < owned_ranges.len() && owned_ranges[candidate].0 < byte_end {
                    let (range_start, range_end) = owned_ranges[candidate];
                    let overlap_start = range_start.max(byte_base);
                    let overlap_end = range_end.min(byte_end);
                    if overlap_start < overlap_end {
                        let low = u32::try_from(overlap_start - byte_base)
                            .map_err(|_| Error::ArithmeticOverflow)?;
                        let high = u32::try_from(overlap_end - byte_base)
                            .map_err(|_| Error::ArithmeticOverflow)?;
                        let low_mask = u8::MAX << low;
                        let high_mask = if high == 8 {
                            u8::MAX
                        } else {
                            (1_u8 << high) - 1
                        };
                        owner_mask |= low_mask & high_mask;
                    }
                    candidate += 1;
                }

                let mut orphaned = *byte & data_mask & !owner_mask;
                while orphaned != 0 {
                    let bit = u64::from(orphaned.trailing_zeros());
                    report.record_orphan(
                        byte_base
                            .checked_add(bit)
                            .ok_or(Error::ArithmeticOverflow)?,
                    )?;
                    orphaned &= orphaned - 1;
                }
                let mut dangling = !*byte & data_mask & owner_mask;
                while dangling != 0 {
                    let bit = u64::from(dangling.trailing_zeros());
                    report.record_dangling_reference(
                        byte_base
                            .checked_add(bit)
                            .ok_or(Error::ArithmeticOverflow)?,
                    )?;
                    dangling &= dangling - 1;
                }
            }
        }
        Ok(report)
    }

    /// Validate physical ownership from a caller-discovered set of extent maps against the bitmap.
    ///
    /// Returns corruption when any allocated block lacks an owner or any owner references a free
    /// block. Use extent_map_ownership_report when diagnostics should list the mismatched blocks.
    pub fn validate_extent_map_ownership<D: BlockDevice>(
        &self,
        device: &mut D,
        extent_maps: &[(&crate::ExtentMap, u64)],
    ) -> Result<()> {
        let report = self.extent_map_ownership_report(device, extent_maps)?;
        if report.is_consistent() {
            Ok(())
        } else {
            Err(Error::Corrupt(
                "allocation bitmap and extent-map ownership disagree",
            ))
        }
    }

    /// Return orphaned allocations and dangling ownership references for diagnostics.
    ///
    /// owned_data_blocks must list each discovered owner block exactly once. Duplicate
    /// entries and addresses outside the allocatable data region are malformed ownership input
    /// and return an error. Bitmap reserved/padding corruption is also rejected.
    pub fn allocation_ownership_report<D: BlockDevice>(
        &self,
        device: &mut D,
        owned_data_blocks: &[u64],
    ) -> Result<AllocationOwnershipReport> {
        validate_device(self, device)?;
        self.validate_allocation_bitmap(device)?;
        let layout = self.volume.layout;
        let mut owned = owned_data_blocks.to_vec();
        owned.sort_unstable();
        if owned.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::Corrupt(
                "a data block is referenced by multiple owners",
            ));
        }
        if owned
            .iter()
            .any(|block| *block < layout.data_start || *block >= layout.backup_superblock)
        {
            return Err(Error::Corrupt("metadata references a non-data block"));
        }

        // Merge the sorted ownership inventory with allocated bitmap bits. Iterating only set
        // bits avoids a per-data-block scan on mostly empty volumes while preserving ascending,
        // deterministic diagnostics.
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut owner_index = 0_usize;
        let mut report = AllocationOwnershipReport::default();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let first_byte =
                usize::try_from((start - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(
                end.checked_sub(base)
                    .and_then(|value| value.checked_add(7))
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_index, byte) in bitmap.iter().enumerate().take(end_byte).skip(first_byte) {
                let byte_base = base
                    .checked_add(
                        u64::try_from(byte_index)
                            .map_err(|_| Error::ArithmeticOverflow)?
                            .checked_mul(8)
                            .ok_or(Error::ArithmeticOverflow)?,
                    )
                    .ok_or(Error::ArithmeticOverflow)?;
                let first_valid = start.saturating_sub(byte_base).min(8);
                let end_valid = end.saturating_sub(byte_base).min(8);
                let lower_mask = u8::MAX << first_valid;
                let upper_mask = if end_valid == 8 {
                    u8::MAX
                } else {
                    (1_u8 << end_valid) - 1
                };
                let mut allocated_bits = *byte & lower_mask & upper_mask;
                while allocated_bits != 0 {
                    let bit = u64::from(allocated_bits.trailing_zeros());
                    let block = byte_base
                        .checked_add(bit)
                        .ok_or(Error::ArithmeticOverflow)?;
                    while owner_index < owned.len() && owned[owner_index] < block {
                        report.record_dangling_reference(owned[owner_index])?;
                        owner_index += 1;
                    }
                    if owner_index < owned.len() && owned[owner_index] == block {
                        owner_index += 1;
                    } else {
                        report.record_orphan(block)?;
                    }
                    allocated_bits &= allocated_bits - 1;
                }
            }
        }
        while owner_index < owned.len() {
            report.record_dangling_reference(owned[owner_index])?;
            owner_index += 1;
        }
        Ok(report)
    }

    /// Verify that the allocation bitmap exactly matches the caller's discovered ownership map.
    ///
    /// This convenience API fails when the detailed ownership report contains any mismatch.
    pub fn validate_allocation_ownership<D: BlockDevice>(
        &self,
        device: &mut D,
        owned_data_blocks: &[u64],
    ) -> Result<()> {
        let report = self.allocation_ownership_report(device, owned_data_blocks)?;
        if report.is_consistent() {
            Ok(())
        } else {
            Err(Error::Corrupt(
                "allocation bitmap and persistent block ownership disagree",
            ))
        }
    }
    /// Return a bounded diagnostic sample of free reserved and padding bitmap bits.
    ///
    /// The report counts every invalid bit but retains at most 64 block numbers, keeping
    /// memory usage bounded even when a damaged bitmap contains many invalid bits.
    pub fn allocation_bitmap_report<D: BlockDevice>(
        &self,
        device: &mut D,
    ) -> Result<AllocationBitmapReport> {
        validate_device(self, device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let mut report = AllocationBitmapReport::default();
        for index in 0..layout.bitmap_blocks {
            let base = index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let end = base
                .checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let data_start = layout.data_start.max(base);
            let data_end = layout.backup_superblock.min(end);
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + index, &mut bitmap)?;
            let first_data_bit = data_start - base;
            let end_data_bit = data_end - base;
            let first_data_byte =
                usize::try_from(first_data_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_data_byte = usize::try_from(end_data_bit.saturating_add(7) / 8)
                .map_err(|_| Error::ArithmeticOverflow)?;
            for (byte_index, byte) in bitmap.iter().enumerate() {
                let mut data_mask = 0_u8;
                if byte_index >= first_data_byte && byte_index < end_data_byte {
                    data_mask = u8::MAX;
                    if byte_index == first_data_byte {
                        data_mask &= u8::MAX << (first_data_bit % 8);
                    }
                    if byte_index + 1 == end_data_byte && end_data_bit % 8 != 0 {
                        data_mask &= (1_u8 << (end_data_bit % 8)) - 1;
                    }
                }
                let data_bits = *byte & data_mask;
                report.allocated_data_block_count = report
                    .allocated_data_block_count
                    .checked_add(u64::from(data_bits.count_ones()))
                    .ok_or(Error::ArithmeticOverflow)?;
                report.free_data_block_count = report
                    .free_data_block_count
                    .checked_add(u64::from((!*byte & data_mask).count_ones()))
                    .ok_or(Error::ArithmeticOverflow)?;
                let mut invalid = !*byte & !data_mask;
                report.invalid_free_block_count = report
                    .invalid_free_block_count
                    .checked_add(u64::from(invalid.count_ones()))
                    .ok_or(Error::ArithmeticOverflow)?;
                while invalid != 0 && report.first_invalid_free_blocks.len() < 64 {
                    let bit = u64::from(invalid.trailing_zeros());
                    let block = base
                        .checked_add(
                            u64::try_from(byte_index)
                                .map_err(|_| Error::ArithmeticOverflow)?
                                .checked_mul(8)
                                .ok_or(Error::ArithmeticOverflow)?,
                        )
                        .and_then(|value| value.checked_add(bit))
                        .ok_or(Error::ArithmeticOverflow)?;
                    report.first_invalid_free_blocks.push(block);
                    invalid &= invalid - 1;
                }
            }
        }
        report.truncated =
            report.invalid_free_block_count > report.first_invalid_free_blocks.len() as u64;
        Ok(report)
    }

    /// Check reserved and padding bits in the allocation bitmap without repairing it.
    ///
    /// This strict convenience method retains the historical error contract. Use
    /// allocation_bitmap_report to inspect a bounded list of offending block numbers.
    pub fn validate_allocation_bitmap<D: BlockDevice>(&self, device: &mut D) -> Result<()> {
        let report = self.allocation_bitmap_report(device)?;
        if report.invalid_free_block_count != 0 {
            return Err(Error::Corrupt(
                "allocation bitmap marks a reserved or padding block free",
            ));
        }
        Ok(())
    }
}

fn validate_device<D: BlockDevice>(volume: &OpenedVolume, device: &D) -> Result<()> {
    if device.block_size() != volume.volume.block_size
        || device.block_count() != volume.volume.total_blocks
    {
        return Err(Error::InvalidInput(
            "device geometry does not match opened volume",
        ));
    }
    let canonical = VolumeLayout::calculate(volume.volume.block_size, volume.volume.total_blocks)?;
    if volume.volume.layout != canonical {
        return Err(Error::InvalidInput(
            "volume layout was modified after validation",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod allocation_bitmap_report_tests {
    use super::*;
    use crate::format_volume;
    use openfs_block::{BlockDevice, ImageDevice};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn image_path(label: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "openfs-bitmap-report-{label}-{}-{nonce}.img",
            std::process::id()
        ))
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

        fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
            self.inner.read_blocks(first_block, output)
        }

        fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
            self.inner.write_blocks(first_block, input)
        }

        fn flush(&mut self) -> Result<()> {
            if self.fail_next_flush {
                self.fail_next_flush = false;
                return Err(Error::Io(std::io::Error::other(
                    "injected descriptor-scrub flush failure",
                )));
            }
            self.inner.flush()
        }
    }

    #[test]
    fn stale_descriptor_scrub_flush_failure_keeps_allocation_free() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for allocation_kind in ["single", "batch", "contiguous", "best-fit"] {
                let path = image_path(&format!(
                    "stale-flush-failure-{allocation_kind}-{block_size}"
                ));
                {
                    let device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume = format_volume(
                        &mut FailNextFlush {
                            inner: device,
                            fail_next_flush: false,
                        },
                        *b"openfs-test-0001",
                        0,
                    )
                    .unwrap();
                    let staged_block = volume.volume.layout.data_start;
                    let mut device = FailNextFlush {
                        inner: ImageDevice::open(&path, block_size).unwrap(),
                        fail_next_flush: false,
                    };
                    let mut stale = vec![0_u8; block_size as usize];
                    stale[..INVENTORY_DESCRIPTOR_MAGIC.len()]
                        .copy_from_slice(INVENTORY_DESCRIPTOR_MAGIC);
                    device.write_blocks(staged_block, &stale).unwrap();
                    device.flush().unwrap();
                    device.fail_next_flush = true;

                    let first_attempt = match allocation_kind {
                        "single" => volume.allocate_block(&mut device).map(|_| ()),
                        "batch" => volume.allocate_blocks(&mut device, 1).map(|_| ()),
                        "contiguous" => volume
                            .allocate_contiguous_blocks(&mut device, 1)
                            .map(|_| ()),
                        "best-fit" => volume
                            .allocate_best_fit_contiguous_blocks(&mut device, 1)
                            .map(|_| ()),
                        _ => unreachable!(),
                    };
                    assert!(
                        matches!(first_attempt, Err(Error::Io(_))),
                        "scrub flush must fail allocation closed: {allocation_kind}, block size {block_size}"
                    );
                    assert!(
                        !volume.is_data_block_allocated(&mut device, staged_block).unwrap(),
                        "failed scrub must leave the staged block free: {allocation_kind}, block size {block_size}"
                    );

                    let retry = match allocation_kind {
                        "single" => vec![volume.allocate_block(&mut device).unwrap()],
                        "batch" => volume.allocate_blocks(&mut device, 1).unwrap(),
                        "contiguous" => {
                            vec![volume.allocate_contiguous_blocks(&mut device, 1).unwrap()]
                        }
                        "best-fit" => {
                            vec![volume
                                .allocate_best_fit_contiguous_blocks(&mut device, 1)
                                .unwrap()]
                        }
                        _ => unreachable!(),
                    };
                    assert_eq!(retry, vec![staged_block]);
                    assert!(
                        volume.discover_persistent_inventory(&mut device).unwrap().is_none(),
                        "retry must not publish the stale descriptor: {allocation_kind}, block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn all_free_paths_preserve_the_persistent_inventory_descriptor() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("descriptor-free-protection-{block_size}"));
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
                let mut metadata = vec![0_u8; block_size as usize];
                metadata[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
                device.write_blocks(metadata_block, &metadata).unwrap();
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

                for result in [
                    volume.free_block(&mut device, descriptor_block),
                    volume.free_blocks(&mut device, &[descriptor_block]),
                    volume.free_contiguous_blocks(&mut device, descriptor_block, 1),
                ] {
                    assert!(
                        matches!(
                            result,
                            Err(Error::InvalidInput(
                                "cannot free a persistent inventory descriptor block"
                            ))
                        ),
                        "generic free API must preserve descriptor ownership at block size {block_size}"
                    );
                    assert!(
                        volume
                            .is_data_block_allocated(&mut device, descriptor_block)
                            .unwrap(),
                        "failed free must leave descriptor allocated at block size {block_size}"
                    );
                }
                assert_eq!(
                    volume.discover_persistent_inventory(&mut device).unwrap(),
                    Some((descriptor_block, descriptor)),
                    "descriptor must remain discoverable at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn descriptor_range_guard_scans_across_bounded_read_batches() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("descriptor-range-batch-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let start = volume.volume.layout.data_start;
                let descriptor_block = start + 64;
                let mut contents = vec![0_u8; block_size as usize];
                contents[..crate::INVENTORY_DESCRIPTOR_MAGIC.len()]
                    .copy_from_slice(crate::INVENTORY_DESCRIPTOR_MAGIC);
                device.write_blocks(descriptor_block, &contents).unwrap();

                assert!(matches!(
                    crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
                        &mut device,
                        start,
                        descriptor_block + 1,
                        block_size,
                    ),
                    Err(Error::InvalidInput(
                        "cannot free a persistent inventory descriptor block"
                    ))
                ), "descriptor in second batch was missed at block size {block_size}");
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn allocator_free_apis_preserve_persistent_inventory_blocks() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("inventory-block-free-guard-{block_size}"));
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
                let mut metadata = vec![0_u8; block_size as usize];
                metadata[..crate::METADATA_RECORD_SIZE].copy_from_slice(&root.encode().unwrap());
                device.write_blocks(metadata_block, &metadata).unwrap();
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
                    volume.free_block(&mut device, metadata_block),
                    Err(Error::InvalidInput(
                        "cannot free a block referenced by persistent inventory descriptor"
                    ))
                ));
                assert!(matches!(
                    volume.free_blocks(&mut device, &[metadata_block, descriptor_block]),
                    Err(Error::InvalidInput(
                        "cannot free a block referenced by persistent inventory descriptor"
                    ))
                ));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, metadata_block, 2),
                    Err(Error::InvalidInput(
                        "cannot free a block referenced by persistent inventory descriptor"
                    ))
                ));

                // The descriptor's own block is not listed in its payload, so test its
                // independent signature guard rather than relying on the metadata guard
                // to reject a range before the descriptor block is reached.
                assert!(matches!(
                    volume.free_block(&mut device, descriptor_block),
                    Err(Error::InvalidInput(
                        "cannot free a persistent inventory descriptor block"
                    ))
                ));
                assert!(matches!(
                    volume.free_blocks(&mut device, &[descriptor_block]),
                    Err(Error::InvalidInput(
                        "cannot free a persistent inventory descriptor block"
                    ))
                ));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, descriptor_block, 1),
                    Err(Error::InvalidInput(
                        "cannot free a persistent inventory descriptor block"
                    ))
                ));
                for block in [metadata_block, descriptor_block] {
                    assert!(
                        volume.is_data_block_allocated(&mut device, block).unwrap(),
                        "rejected range free must preserve every allocation"
                    );
                }
                assert!(volume
                    .is_data_block_allocated(&mut device, descriptor_block)
                    .unwrap());
                assert_eq!(
                    volume.discover_persistent_inventory(&mut device).unwrap(),
                    Some((descriptor_block, descriptor)),
                    "all rejected frees must leave the namespace discoverable for block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn every_allocation_path_scrubs_stale_descriptor_staging() {
        use crate::{MetadataRecord, ObjectId, ObjectKind, PersistentInventoryDescriptor};

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for allocation_kind in ["single", "batch", "contiguous", "best-fit"] {
                let path = image_path(&format!("stale-descriptor-{allocation_kind}-{block_size}"));
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
                    let mut metadata = vec![0_u8; block_size as usize];
                    metadata[..crate::METADATA_RECORD_SIZE]
                        .copy_from_slice(&root.encode().unwrap());
                    device.write_blocks(metadata_block, &metadata).unwrap();
                    let descriptor = PersistentInventoryDescriptor {
                        root_id: ObjectId(1),
                        metadata_blocks: vec![metadata_block],
                        metadata_record_count: 1,
                        directory_blocks: vec![],
                        directory_entry_count: 0,
                    };
                    let staged_block = volume.volume.layout.data_start + 1;
                    assert!(!volume
                        .is_data_block_allocated(&mut device, staged_block)
                        .unwrap());
                    device
                        .write_blocks(
                            staged_block,
                            &descriptor.encode(block_size as usize).unwrap(),
                        )
                        .unwrap();
                    device.flush().unwrap();

                    let allocated = match allocation_kind {
                        "single" => vec![volume.allocate_block(&mut device).unwrap()],
                        "batch" => volume.allocate_blocks(&mut device, 1).unwrap(),
                        "contiguous" => {
                            vec![volume.allocate_contiguous_blocks(&mut device, 1).unwrap()]
                        }
                        "best-fit" => {
                            vec![volume
                                .allocate_best_fit_contiguous_blocks(&mut device, 1)
                                .unwrap()]
                        }
                        _ => unreachable!(),
                    };
                    assert_eq!(allocated, vec![staged_block]);
                    assert!(volume
                        .is_data_block_allocated(&mut device, staged_block)
                        .unwrap());
                    assert_eq!(
                        volume.discover_persistent_inventory(&mut device).unwrap(),
                        None,
                        "stale staged descriptor must not become discoverable after {allocation_kind} allocation at block size {block_size}"
                    );
                }
                fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn bitmap_report_lists_exact_reserved_and_padding_bits() {
        let path = image_path("exact");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = crate::format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            let mut bitmap = vec![0_u8; 4096];
            device
                .read_blocks(layout.bitmap_start, &mut bitmap)
                .unwrap();
            for block in [0_u64, layout.backup_superblock, 300] {
                bitmap[(block / 8) as usize] &= !(1_u8 << (block % 8));
            }
            device.write_blocks(layout.bitmap_start, &bitmap).unwrap();

            let report = volume.allocation_bitmap_report(&mut device).unwrap();
            assert_eq!(report.invalid_free_block_count, 3);
            assert_eq!(report.free_data_block_count, layout.data_blocks());
            assert_eq!(report.allocated_data_block_count, 0);
            assert_eq!(report.first_invalid_free_blocks, vec![0, 255, 300]);
            assert!(!report.truncated);
            assert!(matches!(
                volume.validate_allocation_bitmap(&mut device),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn contiguous_free_round_trip_and_invalid_ranges_across_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("contiguous-free-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                assert!(matches!(
                    volume.allocate_contiguous_blocks(&mut device, u64::from(block_size) * 8 + 1),
                    Err(Error::InvalidInput(_))
                ));
                let start = volume.allocate_contiguous_blocks(&mut device, 5).unwrap();
                let before = volume.allocation_stats(&mut device).unwrap();
                assert_eq!(before.allocated_data_blocks, 5);

                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, start, 0),
                    Err(Error::InvalidInput(_))
                ));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, start - 1, 2),
                    Err(Error::InvalidInput(_))
                ));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, start, u64::MAX),
                    Err(Error::ArithmeticOverflow)
                ));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, start, 6),
                    Err(Error::InvalidInput(
                        "contiguous free extent contains an already-free block"
                    ))
                ));
                assert_eq!(
                    volume.allocation_stats(&mut device).unwrap(),
                    before,
                    "invalid free attempts must not change allocation state, block_size={block_size}"
                );

                volume
                    .free_contiguous_blocks(&mut device, start, 5)
                    .unwrap();
                let after = volume.allocation_stats(&mut device).unwrap();
                assert_eq!(after.allocated_data_blocks, 0);
                assert_eq!(after.free_data_blocks, before.free_data_blocks + 5);
                assert!((start..start + 5)
                    .all(|block| { !volume.is_data_block_allocated(&mut device, block).unwrap() }));
                assert!(matches!(
                    volume.free_contiguous_blocks(&mut device, start, 5),
                    Err(Error::InvalidInput(
                        "contiguous free extent contains an already-free block"
                    ))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn bitmap_run_search_handles_byte_boundaries_and_fragmentation() {
        let path = image_path("bitmap-run-search");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let data_start = volume.volume.layout.data_start;
            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 17).unwrap(),
                Some(data_start),
                "a free run must span bitmap-byte boundaries"
            );

            let first = volume.allocate_contiguous_blocks(&mut device, 3).unwrap();
            assert_eq!(first, data_start);
            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 17).unwrap(),
                Some(data_start + 3),
                "search must resume after an allocated prefix"
            );

            let isolated = volume.allocate_contiguous_blocks(&mut device, 1).unwrap();
            assert_eq!(isolated, data_start + 3);
            volume.allocate_contiguous_blocks(&mut device, 1).unwrap();
            volume.allocate_contiguous_blocks(&mut device, 1).unwrap();
            assert_eq!(
                volume.find_contiguous_free_blocks(&mut device, 17).unwrap(),
                Some(data_start + 6),
                "an allocated gap must terminate the preceding free run"
            );

            let extents = volume.free_extent_inventory(&mut device).unwrap();
            assert!(extents.iter().all(|extent| extent.block_count > 0));
            assert!(extents
                .windows(2)
                .all(|pair| { pair[0].start_block + pair[0].block_count <= pair[1].start_block }));
            assert!(extents.iter().all(|extent| {
                extent.start_block / (u64::from(4096_u32) * 8)
                    == (extent.start_block + extent.block_count - 1) / (u64::from(4096_u32) * 8)
            }));
            assert_eq!(isolated, data_start + 3);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn free_inventory_matches_allocation_state_across_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("free-inventory-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let allocated = volume.allocate_blocks(&mut device, 9).unwrap();
                volume.free_blocks(&mut device, &allocated[2..5]).unwrap();

                let allocated_inventory =
                    volume.allocated_data_block_inventory(&mut device).unwrap();
                let free_inventory = volume.free_data_block_inventory(&mut device).unwrap();
                assert_eq!(
                    allocated_inventory,
                    vec![
                        allocated[0],
                        allocated[1],
                        allocated[5],
                        allocated[6],
                        allocated[7],
                        allocated[8]
                    ]
                );
                assert_eq!(
                    free_inventory.len() as u64,
                    volume.volume.layout.data_blocks() - 6
                );
                let free_extents = volume.free_extent_inventory(&mut device).unwrap();
                assert_eq!(
                    free_extents,
                    vec![
                        FreeExtent {
                            start_block: allocated[2],
                            block_count: 3,
                        },
                        FreeExtent {
                            start_block: allocated[8] + 1,
                            block_count: volume.volume.layout.backup_superblock
                                - (allocated[8] + 1),
                        },
                    ],
                    "block_size={block_size}"
                );
                assert_eq!(
                    volume.find_best_fit_free_extent(&mut device, 3).unwrap(),
                    Some(FreeExtent {
                        start_block: allocated[2],
                        block_count: 3,
                    }),
                    "best-fit should choose the exact-sized hole for block_size={block_size}"
                );
                assert_eq!(
                    volume.find_best_fit_free_extent(&mut device, 4).unwrap(),
                    Some(FreeExtent {
                        start_block: allocated[8] + 1,
                        block_count: volume.volume.layout.backup_superblock
                            - (allocated[8] + 1),
                    }),
                    "best-fit should choose the smallest extent that fits for block_size={block_size}"
                );
                assert_eq!(
                    volume
                        .find_best_fit_free_extent(&mut device, volume.volume.layout.data_blocks(),)
                        .unwrap(),
                    None,
                    "best-fit must report no suitable extent for block_size={block_size}"
                );
                assert!(matches!(
                    volume.find_best_fit_free_extent(&mut device, 0),
                    Err(Error::InvalidInput(_))
                ));
                let fragmentation = volume.free_extent_stats(&mut device).unwrap();
                assert_eq!(
                    fragmentation.free_extent_count,
                    free_extents.len() as u64,
                    "block_size={block_size}"
                );
                assert_eq!(
                    fragmentation.free_extent_count, 2,
                    "block_size={block_size}"
                );
                assert_eq!(
                    fragmentation.largest_free_extent_blocks,
                    volume.volume.layout.data_blocks() - 9,
                    "block_size={block_size}"
                );
                assert!(free_inventory.windows(2).all(|pair| pair[0] < pair[1]));
                assert!(free_inventory
                    .iter()
                    .all(|block| !allocated_inventory.contains(block)));
                for block in allocated_inventory.iter().chain(free_inventory.iter()) {
                    assert_eq!(
                        volume.is_data_block_allocated(&mut device, *block).unwrap(),
                        allocated_inventory.contains(block)
                    );
                }
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_inventory.len() as u64
                );
                let before_best_fit = volume.allocation_stats(&mut device).unwrap();
                assert_eq!(
                    volume
                        .allocate_best_fit_contiguous_blocks(&mut device, 3)
                        .unwrap(),
                    allocated[2],
                    "best-fit allocation should reuse the exact-sized hole for block_size={block_size}"
                );
                assert_eq!(
                    volume
                        .allocation_stats(&mut device)
                        .unwrap()
                        .allocated_data_blocks,
                    before_best_fit.allocated_data_blocks + 3,
                    "best-fit allocation should account for all committed blocks"
                );
                let after_best_fit = volume.allocated_data_block_inventory(&mut device).unwrap();
                assert!(
                    (allocated[2]..allocated[2] + 3).all(|block| after_best_fit.contains(&block))
                );
                assert!(matches!(
                    volume.allocate_best_fit_contiguous_blocks(&mut device, 0),
                    Err(Error::InvalidInput(_))
                ));
                assert!(matches!(
                    volume.allocate_best_fit_contiguous_blocks(
                        &mut device,
                        volume.volume.layout.data_blocks(),
                    ),
                    Err(Error::InvalidInput("no contiguous free data extent"))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn bitmap_report_counts_free_and_allocated_data_blocks() {
        let path = image_path("counts");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let total = volume.volume.layout.data_blocks();
            let allocated = volume.allocate_blocks(&mut device, 5).unwrap();
            volume.free_blocks(&mut device, &allocated[1..3]).unwrap();

            let report = volume.allocation_bitmap_report(&mut device).unwrap();
            assert_eq!(report.invalid_free_block_count, 0);
            assert_eq!(report.allocated_data_block_count, 3);
            assert_eq!(report.free_data_block_count, total - 3);
            assert_eq!(
                volume.allocation_stats(&mut device).unwrap(),
                AllocationStats {
                    total_data_blocks: total,
                    allocated_data_blocks: 3,
                    free_data_blocks: total - 3,
                }
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn bitmap_counts_match_allocator_state_for_all_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("all-sizes-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let total = volume.volume.layout.data_blocks();
                let allocated = volume.allocate_blocks(&mut device, 3).unwrap();
                volume.free_blocks(&mut device, &allocated[1..2]).unwrap();

                let report = volume.allocation_bitmap_report(&mut device).unwrap();
                assert_eq!(
                    report.invalid_free_block_count, 0,
                    "block_size={block_size}"
                );
                assert_eq!(
                    report.allocated_data_block_count, 2,
                    "block_size={block_size}"
                );
                assert_eq!(
                    report.free_data_block_count,
                    total - 2,
                    "block_size={block_size}"
                );
                assert_eq!(
                    volume.allocation_stats(&mut device).unwrap(),
                    AllocationStats {
                        total_data_blocks: total,
                        allocated_data_blocks: 2,
                        free_data_blocks: total - 2,
                    },
                    "block_size={block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn allocation_stats_rejects_reserved_and_padding_bitmap_corruption() {
        let path = image_path("stats-corrupt");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let layout = volume.volume.layout;
            let mut bitmap = vec![0_u8; 4096];
            device
                .read_blocks(layout.bitmap_start, &mut bitmap)
                .unwrap();
            for block in [0_u64, layout.backup_superblock, 300] {
                bitmap[(block / 8) as usize] &= !(1_u8 << (block % 8));
            }
            device.write_blocks(layout.bitmap_start, &bitmap).unwrap();

            assert!(matches!(
                volume.allocation_stats(&mut device),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.free_data_block_inventory(&mut device),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.free_extent_stats(&mut device),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.find_best_fit_free_extent(&mut device, 1),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.free_extent_inventory(&mut device),
                Err(Error::Corrupt(
                    "allocation bitmap marks a reserved or padding block free"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn bitmap_report_bounds_samples_for_heavily_corrupted_padding() {
        let path = image_path("bounded");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
            let mut bitmap = vec![0_u8; 4096];
            device
                .read_blocks(volume.volume.layout.bitmap_start, &mut bitmap)
                .unwrap();
            for block in 256_u64..336 {
                bitmap[(block / 8) as usize] &= !(1_u8 << (block % 8));
            }
            device
                .write_blocks(volume.volume.layout.bitmap_start, &bitmap)
                .unwrap();

            let report = volume.allocation_bitmap_report(&mut device).unwrap();
            assert_eq!(report.invalid_free_block_count, 80);
            assert_eq!(report.first_invalid_free_blocks.len(), 64);
            assert_eq!(
                report.first_invalid_free_blocks,
                (256_u64..320).collect::<Vec<_>>()
            );
            assert!(report.truncated);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn ownership_report_merges_bitmap_set_bits_and_dangling_owners_across_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("ownership-bit-scan-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 512).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let allocated = volume.allocate_blocks(&mut device, 11).unwrap();
                volume.free_blocks(&mut device, &[allocated[4]]).unwrap();

                let report = volume
                    .allocation_ownership_report(
                        &mut device,
                        &[allocated[0], allocated[2], allocated[4], allocated[7]],
                    )
                    .unwrap();

                assert_eq!(
                    report.orphaned_allocations,
                    vec![
                        allocated[1],
                        allocated[3],
                        allocated[5],
                        allocated[6],
                        allocated[8],
                        allocated[9],
                        allocated[10],
                    ],
                    "orphaned allocated blocks must be ordered for block_size={block_size}"
                );
                assert_eq!(
                    report.orphaned_allocation_count, 7,
                    "block_size={block_size}"
                );
                assert_eq!(
                    report.dangling_references,
                    vec![allocated[4]],
                    "a reference to a freed block must be reported for block_size={block_size}"
                );
                assert_eq!(
                    report.dangling_reference_count, 1,
                    "block_size={block_size}"
                );
                assert!(!report.is_consistent());
            }
            fs::remove_file(path).unwrap();
        }
    }
}
