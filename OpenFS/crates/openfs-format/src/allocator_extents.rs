//! Atomic contiguous allocation helpers for v2 volumes.
//!
//! The current on-disk journal can atomically update one bitmap block per transaction.
//! Extents are therefore deliberately constrained to a single bitmap segment.

use crate::{OpenedVolumeV2, RedoUpdate, VolumeLayoutV2};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

/// Read-only snapshot of usable data-space capacity.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AllocationStats {
    pub total_data_blocks: u64,
    pub allocated_data_blocks: u64,
    pub free_data_blocks: u64,
}

/// Read-only fragmentation summary for free space in the V2 data region.
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

impl OpenedVolumeV2 {
    /// Return total, allocated, and free data-block counts from the current bitmap.
    pub fn allocation_stats<D: BlockDevice>(&self, device: &mut D) -> Result<AllocationStats> {
        let report = self.allocation_bitmap_report(device)?;
        if report.invalid_free_block_count != 0 {
            return Err(Error::Corrupt(
                "v2 bitmap marks a reserved or padding block free",
            ));
        }
        let total = self.volume.layout.data_blocks();
        let accounted = report
            .free_data_block_count
            .checked_add(report.allocated_data_block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        if accounted != total {
            return Err(Error::Corrupt(
                "v2 bitmap data-block counts do not match the usable data region",
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
            let mut run_start = start;
            let mut run_len = 0_u64;
            for block in start..end {
                let local = block - base;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                if bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                    if run_len == 0 {
                        run_start = block;
                    }
                    run_len += 1;
                    if run_len == count {
                        return Ok(Some(run_start));
                    }
                } else {
                    run_len = 0;
                }
            }
        }
        Ok(None)
    }

    /// Atomically allocate a contiguous extent and return its first data block.
    /// The extent must fit within one bitmap block to fit the bounded v2 journal.
    pub fn allocate_contiguous_blocks<D: BlockDevice>(
        &mut self,
        device: &mut D,
        count: u64,
    ) -> Result<u64> {
        validate_device(self, device)?;
        if count == 0 {
            return Err(Error::InvalidInput(
                "contiguous extent length must be nonzero",
            ));
        }
        let start = self
            .find_contiguous_free_blocks(device, count)?
            .ok_or(Error::InvalidInput("no contiguous free data extent"))?;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
        let bitmap_block = self.volume.layout.bitmap_start + start / bits_per_block;
        let mut bitmap = vec![0_u8; self.volume.block_size as usize];
        device.read_blocks(bitmap_block, &mut bitmap)?;
        for block in start..start + count {
            let local = block % bits_per_block;
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
                stats.largest_free_extent_blocks =
                    stats.largest_free_extent_blocks.max(run_len);
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
        let mut owned = Vec::new();
        for (map, logical_blocks) in extent_maps {
            owned.extend(map.validated_physical_block_inventory(
                *logical_blocks,
                layout.data_start,
                layout.backup_superblock,
            )?);
        }
        self.allocation_ownership_report(device, &owned)
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

        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut owner_index = 0_usize;
        let mut report = AllocationOwnershipReport::default();
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index * bits_per_block;
            let start = layout.data_start.max(base);
            let end = layout
                .backup_superblock
                .min(base.saturating_add(bits_per_block));
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
            for block in start..end {
                let local = block - base;
                let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                let allocated = bitmap[byte] & (1_u8 << (local % 8)) != 0;
                if owner_index < owned.len() && owned[owner_index] < block {
                    return Err(Error::Corrupt(
                        "ownership map contains an unscanned data block",
                    ));
                }
                let referenced = owner_index < owned.len() && owned[owner_index] == block;
                if allocated && !referenced {
                    report.record_orphan(block)?;
                } else if referenced && !allocated {
                    report.record_dangling_reference(block)?;
                }
                if referenced {
                    owner_index += 1;
                }
            }
        }
        if owner_index != owned.len() {
            return Err(Error::Corrupt(
                "ownership map contains an unscanned data block",
            ));
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
                "v2 bitmap marks a reserved or padding block free",
            ));
        }
        Ok(())
    }
}

fn validate_device<D: BlockDevice>(volume: &OpenedVolumeV2, device: &D) -> Result<()> {
    if device.block_size() != volume.volume.block_size
        || device.block_count() != volume.volume.total_blocks
    {
        return Err(Error::InvalidInput(
            "device geometry does not match opened v2 volume",
        ));
    }
    let canonical =
        VolumeLayoutV2::calculate(volume.volume.block_size, volume.volume.total_blocks)?;
    if volume.volume.layout != canonical {
        return Err(Error::InvalidInput(
            "v2 volume layout was modified after validation",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod allocation_bitmap_report_tests {
    use super::*;
    use crate::format_volume_v2;
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

    #[test]
    fn bitmap_report_lists_exact_reserved_and_padding_bits() {
        let path = image_path("exact");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = crate::format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    "v2 bitmap marks a reserved or padding block free"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn free_inventory_matches_allocation_state_across_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("free-inventory-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
                let allocated = volume.allocate_blocks(&mut device, 9).unwrap();
                volume.free_blocks(&mut device, &allocated[2..5]).unwrap();

                let allocated_inventory =
                    volume.allocated_data_block_inventory(&mut device).unwrap();
                let free_inventory = volume.free_data_block_inventory(&mut device).unwrap();
                assert_eq!(allocated_inventory, vec![allocated[0], allocated[1], allocated[5], allocated[6], allocated[7], allocated[8]]);
                assert_eq!(free_inventory.len() as u64, volume.volume.layout.data_blocks() - 6);
                let fragmentation = volume.free_extent_stats(&mut device).unwrap();
                assert_eq!(fragmentation.free_extent_count, 2, "block_size={block_size}");
                assert_eq!(
                    fragmentation.largest_free_extent_blocks,
                    volume.volume.layout.data_blocks() - 9,
                    "block_size={block_size}"
                );
                assert!(free_inventory.windows(2).all(|pair| pair[0] < pair[1]));
                assert!(free_inventory.iter().all(|block| !allocated_inventory.contains(block)));
                for block in allocated_inventory.iter().chain(free_inventory.iter()) {
                    assert!(volume.is_data_block_allocated(&mut device, *block).unwrap()
                        == allocated_inventory.contains(block));
                }
                assert_eq!(
                    volume.free_data_block_count(&mut device).unwrap(),
                    free_inventory.len() as u64
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn bitmap_report_counts_free_and_allocated_data_blocks() {
        let path = image_path("counts");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    "v2 bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.free_data_block_inventory(&mut device),
                Err(Error::Corrupt(
                    "v2 bitmap marks a reserved or padding block free"
                ))
            ));
            assert!(matches!(
                volume.free_extent_stats(&mut device),
                Err(Error::Corrupt(
                    "v2 bitmap marks a reserved or padding block free"
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
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
}
