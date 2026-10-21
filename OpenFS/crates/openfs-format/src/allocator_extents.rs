//! Atomic contiguous allocation helpers for volumes.
//!
//! The current on-disk journal can atomically update one bitmap block per transaction.
//! Extents are therefore deliberately constrained to a single bitmap segment.

use crate::{OpenedVolume, RedoUpdate, VolumeLayout, INVENTORY_DESCRIPTOR_MAGIC};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

fn try_zeroed_buffer(len: usize, allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut buffer = Vec::new();
    buffer
        .try_reserve_exact(len)
        .map_err(|_| Error::AllocationFailed(allocation_error))?;
    buffer.resize(len, 0);
    Ok(buffer)
}

fn try_copy_blocks(blocks: &[u64], allocation_error: &'static str) -> Result<Vec<u64>> {
    let mut copy = Vec::new();
    copy.try_reserve_exact(blocks.len())
        .map_err(|_| Error::AllocationFailed(allocation_error))?;
    copy.extend_from_slice(blocks);
    Ok(copy)
}

/// Scrub descriptor signatures from one bounded contiguous batch.
///
/// Only blocks bearing the stale signature are written back; clean blocks are read
/// but never rewritten. The caller flushes after all batches have been processed.
fn scrub_descriptor_batch<D: BlockDevice>(
    device: &mut D,
    start_block: u64,
    block_size: usize,
    contents: &mut [u8],
) -> Result<bool> {
    let block_count = contents.len() / block_size;
    let mut changed = false;
    let mut cursor = 0_usize;
    while cursor < block_count {
        let byte_start = cursor
            .checked_mul(block_size)
            .ok_or(Error::ArithmeticOverflow)?;
        let chunk = &contents[byte_start..byte_start + block_size];
        if !chunk.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
            cursor += 1;
            continue;
        }

        // Scrub and write one contiguous run immediately. This avoids building a
        // per-batch vector of stale block indices, even for heavily fragmented data.
        let first = cursor;
        while cursor < block_count {
            let byte_offset = cursor
                .checked_mul(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            let end = byte_offset
                .checked_add(block_size)
                .ok_or(Error::ArithmeticOverflow)?;
            if !contents[byte_offset..end].starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
                break;
            }
            contents[byte_offset..end].fill(0);
            cursor += 1;
        }
        let byte_end = cursor
            .checked_mul(block_size)
            .ok_or(Error::ArithmeticOverflow)?;
        let target = start_block
            .checked_add(u64::try_from(first).map_err(|_| Error::ArithmeticOverflow)?)
            .ok_or(Error::ArithmeticOverflow)?;
        device.write_blocks(target, &contents[byte_start..byte_end])?;
        changed = true;
    }
    Ok(changed)
}

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
    const BATCH_BLOCKS: usize = 64;
    let block_bytes = usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
    if block_bytes == 0 {
        return Err(Error::InvalidInput("block size must be non-zero"));
    }
    let mut changed = false;
    let max_batch_bytes = BATCH_BLOCKS
        .checked_mul(block_bytes)
        .ok_or(Error::ArithmeticOverflow)?;
    let mut contents = Vec::new();
    contents.try_reserve_exact(max_batch_bytes).map_err(|_| {
        Error::AllocationFailed("allocation failed while scrubbing descriptor blocks")
    })?;
    contents.resize(max_batch_bytes, 0);
    let mut cursor = 0_usize;
    while cursor < blocks.len() {
        let run_start = cursor;
        let mut run_end = cursor + 1;
        while run_end < blocks.len()
            && run_end - run_start < BATCH_BLOCKS
            && blocks[run_end - 1].checked_add(1) == Some(blocks[run_end])
        {
            run_end += 1;
        }
        let byte_count = (run_end - run_start)
            .checked_mul(block_bytes)
            .ok_or(Error::ArithmeticOverflow)?;
        let batch = &mut contents[..byte_count];
        device.read_blocks(blocks[run_start], batch)?;
        changed |= scrub_descriptor_batch(device, blocks[run_start], block_bytes, batch)?;
        cursor = run_end;
    }
    if changed {
        device.flush()?;
    }
    Ok(())
}

/// Clear stale descriptor signatures from a contiguous range without allocating a
/// per-block index vector. Reads and writes remain bounded to at most 64 blocks.
fn clear_stale_descriptor_magic_range<D: BlockDevice>(
    device: &mut D,
    block_size: u32,
    start_block: u64,
    end_block: u64,
) -> Result<()> {
    const BATCH_BLOCKS: u64 = 64;
    let block_bytes = usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)?;
    if block_bytes == 0 {
        return Err(Error::InvalidInput("block size must be non-zero"));
    }
    let mut next_block = start_block;
    let mut changed = false;
    // Reuse one bounded buffer for every batch instead of allocating up to 64
    // block buffers again and again while scanning a large free-space range.
    let max_batch_bytes = usize::try_from(BATCH_BLOCKS)
        .map_err(|_| Error::ArithmeticOverflow)?
        .checked_mul(block_bytes)
        .ok_or(Error::ArithmeticOverflow)?;
    let mut contents = Vec::new();
    contents.try_reserve_exact(max_batch_bytes).map_err(|_| {
        Error::AllocationFailed("allocation failed while scrubbing descriptor blocks")
    })?;
    contents.resize(max_batch_bytes, 0);
    while next_block < end_block {
        let count = (end_block - next_block).min(BATCH_BLOCKS);
        let byte_count = usize::try_from(count)
            .map_err(|_| Error::ArithmeticOverflow)?
            .checked_mul(block_bytes)
            .ok_or(Error::ArithmeticOverflow)?;
        let batch = &mut contents[..byte_count];
        device.read_blocks(next_block, batch)?;
        changed |= scrub_descriptor_batch(device, next_block, block_bytes, batch)?;
        next_block = next_block
            .checked_add(count)
            .ok_or(Error::ArithmeticOverflow)?;
    }
    if changed {
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
        let requested_count = usize::try_from(count).map_err(|_| Error::ArithmeticOverflow)?;
        let layout = self.volume.layout;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_block;
        // Reuse one bitmap image and one selection buffer across segment scans.
        // Highly fragmented volumes can require checking many segments before finding
        // a large enough run; reallocating the candidate vector each time is avoidable.
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        let mut selected = Vec::new();
        selected.try_reserve_exact(requested_count).map_err(|_| {
            Error::AllocationFailed("allocation failed while selecting free extents")
        })?;
        for index in first_bitmap..=last_bitmap {
            selected.clear();
            let base = index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            if end - start < count {
                continue;
            }
            let bitmap_block = layout
                .bitmap_start
                .checked_add(index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            // Search the bitmap by byte and enumerate only free bits. The previous
            // block-by-block loop performed divisions and modulo operations for every
            // candidate, even when a full bitmap page contained no free blocks.
            let local_start = start - base;
            let local_end = end - base;
            let first_byte =
                usize::try_from(local_start / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(local_end.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            // Slice directly to the relevant bytes so scanning a later bitmap
            // segment does not walk the unused prefix through Iterator::skip().
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
                let mut available = !byte & valid_mask;
                while available != 0 {
                    let bit = u64::from(available.trailing_zeros());
                    let local = u64::try_from(byte_index)
                        .map_err(|_| Error::ArithmeticOverflow)?
                        .checked_mul(8)
                        .and_then(|value| value.checked_add(bit))
                        .ok_or(Error::ArithmeticOverflow)?;
                    selected.push(base.checked_add(local).ok_or(Error::ArithmeticOverflow)?);
                    if selected.len() == requested_count {
                        break;
                    }
                    available &= !(1_u8 << bit);
                }
                if selected.len() == requested_count {
                    break;
                }
            }
            if selected.len() != requested_count {
                continue;
            }
            // Refresh the bitmap before touching candidate contents. A second opener may
            // have allocated one of these blocks since the initial free-space scan.
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
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
            // Selected blocks are emitted in ascending order. Set consecutive runs
            // with byte-range masks instead of recomputing byte/bit positions for every
            // block, which is especially useful for large sequential allocations.
            let mut run_start = selected[0];
            let mut previous = selected[0];
            for &block in selected.iter().skip(1) {
                if previous.checked_add(1) == Some(block) {
                    previous = block;
                    continue;
                }
                set_bitmap_range(&mut bitmap, run_start - base, previous - base + 1, true)?;
                run_start = block;
                previous = block;
            }
            set_bitmap_range(
                &mut bitmap,
                run_start - base,
                previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - base,
                true,
            )?;
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
        if blocks.is_empty() {
            return Err(Error::InvalidInput("batch free list must not be empty"));
        }
        // Reclamation already supplies strictly increasing block numbers. Avoid a
        // second allocation for that common path; only normalize caller-owned unsorted
        // input when it actually needs sorting.
        if blocks.windows(2).all(|pair| pair[0] < pair[1]) {
            return self.free_blocks_sorted(device, blocks);
        }
        let mut sorted = try_copy_blocks(blocks, "allocation failed while sorting blocks to free")?;
        sorted.sort_unstable();
        if sorted.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::InvalidInput("batch free list contains duplicates"));
        }
        self.free_blocks_sorted(device, &sorted)
    }

    /// Free an already sorted, duplicate-free list without making another proportional
    /// allocation. Used by reclamation after it has validated and sorted the full list.
    pub(crate) fn free_blocks_sorted<D: BlockDevice>(
        &mut self,
        device: &mut D,
        sorted: &[u64],
    ) -> Result<()> {
        validate_device(self, device)?;
        if sorted.is_empty() {
            return Err(Error::InvalidInput("batch free list must not be empty"));
        }
        if sorted.windows(2).any(|pair| pair[0] >= pair[1]) {
            return Err(Error::InvalidInput(
                "sorted batch free list must be strictly increasing",
            ));
        }
        let layout = self.volume.layout;
        let bits_per_block = u64::from(self.volume.block_size) * 8;
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
        let bitmap_block = layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let segment_start = layout.data_start.max(base);
        let segment_end = layout.backup_superblock.min(
            base.checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        validate_bitmap_reserved_bits(&bitmap, base, segment_start, segment_end)?;
        // Validate contiguous runs a byte at a time. Sparse entries still
        // require run discovery, but long file extents no longer perform one
        // division, modulo and bit-mask check per block.
        let mut run_start = sorted[0];
        let mut previous = sorted[0];
        for &block in sorted.iter().skip(1) {
            if previous.checked_add(1) == Some(block) {
                previous = block;
                continue;
            }
            if !bitmap_range_matches(&bitmap, run_start - base, previous - base + 1, true)? {
                return Err(Error::InvalidInput(
                    "batch free contains an already-free block",
                ));
            }
            run_start = block;
            previous = block;
        }
        if !bitmap_range_matches(
            &bitmap,
            run_start - base,
            previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - base,
            true,
        )? {
            return Err(Error::InvalidInput(
                "batch free contains an already-free block",
            ));
        }
        self.ensure_inventory_blocks_releasable(device, sorted)?;
        // The sorted list lets us check descriptor protection by contiguous runs,
        // avoiding one device read per block even for sparse batches.
        let mut run_start = sorted[0];
        let mut previous = sorted[0];
        for &block in sorted.iter().skip(1) {
            if previous.checked_add(1) == Some(block) {
                previous = block;
                continue;
            }
            let run_end = previous.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
                device,
                run_start,
                run_end,
                self.volume.block_size,
            )?;
            run_start = block;
            previous = block;
        }
        let run_end = previous.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
            device,
            run_start,
            run_end,
            self.volume.block_size,
        )?;
        // Clear contiguous runs byte-wise; sparse blocks still cost O(n), but
        // long extents avoid repeating division, modulo and mask construction per block.
        let mut run_start = sorted[0];
        let mut previous = sorted[0];
        for &block in sorted.iter().skip(1) {
            if previous.checked_add(1) == Some(block) {
                previous = block;
                continue;
            }
            set_bitmap_range(&mut bitmap, run_start - base, previous - base + 1, false)?;
            run_start = block;
            previous = block;
        }
        set_bitmap_range(
            &mut bitmap,
            run_start - base,
            previous.checked_add(1).ok_or(Error::ArithmeticOverflow)? - base,
            false,
        )?;
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
        let bitmap_block = layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let segment_start = layout.data_start.max(base);
        let segment_end = layout.backup_superblock.min(
            base.checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        validate_bitmap_reserved_bits(&bitmap, base, segment_start, segment_end)?;
        // Validate the contiguous range byte-wise. A large free should not perform
        // one bitmap lookup and mask operation per data block when one byte represents
        // eight allocation states.
        let local_start = start_block - base;
        let local_end = end_block - base;
        if !bitmap_range_matches(&bitmap, local_start, local_end, true)? {
            return Err(Error::InvalidInput(
                "contiguous free extent contains an already-free block",
            ));
        }
        self.ensure_inventory_range_releasable(device, start_block, end_block)?;
        crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
            device,
            start_block,
            end_block,
            self.volume.block_size,
        )?;
        // All requested bits were verified allocated above; clear them with the
        // same edge masks, preserving neighboring allocations in partial bytes.
        set_bitmap_range(&mut bitmap, local_start, local_end, false)?;
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
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_block;
        // Reuse the same block-sized buffer while inspecting each bitmap segment.
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for index in first_bitmap..=last_bitmap {
            let bitmap_block = layout
                .bitmap_start
                .checked_add(index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let base = index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
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
            // Ignore bitmap bytes outside this segment's usable data range.
            // Enumerating the slice avoids repeatedly walking the whole prefix.
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                // A fully free byte is an eight-block run. Consume it in one step
                // rather than enumerating each bit on the common contiguous-space path.
                if free_bits == u8::MAX {
                    let block = base
                        .checked_add(byte_start)
                        .ok_or(Error::ArithmeticOverflow)?;
                    if previous_free.and_then(|previous: u64| previous.checked_add(1))
                        == Some(block)
                    {
                        run_len = run_len.checked_add(8).ok_or(Error::ArithmeticOverflow)?;
                    } else {
                        run_start = block;
                        run_len = 8;
                    }
                    if run_len >= count {
                        return Ok(Some(run_start));
                    }
                    previous_free = Some(block.checked_add(7).ok_or(Error::ArithmeticOverflow)?);
                    continue;
                }
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
        let bitmap_index = start / bits_per_block;
        let bitmap_block = self
            .volume
            .layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let segment_start = self.volume.layout.data_start.max(base);
        let segment_end = self.volume.layout.backup_superblock.min(
            base.checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        validate_bitmap_reserved_bits(&bitmap, base, segment_start, segment_end)?;
        // Recheck the reservation after the free-run scan. This also protects
        // against stale allocator views before any block contents are touched.
        if !bitmap_range_matches(&bitmap, start - base, end - base, false)? {
            return Err(Error::Corrupt(
                "contiguous extent changed during allocation",
            ));
        }
        // Validate the entire reservation before touching block contents. If the bitmap
        // changed since the free-run scan, fail without clearing a block that is now owned.
        clear_stale_descriptor_magic_range(device, self.volume.block_size, start, end)?;
        // Mark the already-validated range with the same masks in one pass per
        // bitmap byte, avoiding per-block division and modulo operations.
        set_bitmap_range(&mut bitmap, start - base, end - base, true)?;
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
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut free = Vec::new();
        // Reuse one bitmap page while walking the inventory to avoid allocating a
        // fresh block-sized buffer for every segment.
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            // Inspect only bytes intersecting usable data in this segment.
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
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut extents = Vec::new();
        // The same scratch page is reused for every segment; inventory is read-only
        // and does not need to retain bitmap images after each scan.
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut current: Option<FreeExtent> = None;
            // Start at the first relevant bitmap byte. Enumerating the whole page
            // with take/skip repeatedly revisits bytes before the data region on the
            // first segment, which is wasteful for large block sizes and many scans.
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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;
                // Consume contiguous free-bit runs a byte at a time. Walking every
                // free block individually makes inventory scans scale with free capacity,
                // even when the entire bitmap is one large extent.
                while free_bits != 0 {
                    let first_bit = free_bits.trailing_zeros();
                    let run_width = (free_bits >> first_bit).trailing_ones();
                    let block = base
                        .checked_add(byte_start)
                        .and_then(|value| value.checked_add(u64::from(first_bit)))
                        .ok_or(Error::ArithmeticOverflow)?;
                    match current {
                        Some(mut extent)
                            if extent.start_block.checked_add(extent.block_count)
                                == Some(block) =>
                        {
                            extent.block_count = extent
                                .block_count
                                .checked_add(u64::from(run_width))
                                .ok_or(Error::ArithmeticOverflow)?;
                            current = Some(extent);
                        }
                        Some(extent) => {
                            extents.push(extent);
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: u64::from(run_width),
                            });
                        }
                        None => {
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: u64::from(run_width),
                            });
                        }
                    }
                    let run_mask = ((u16::from(u8::MAX) >> (8 - run_width)) << first_bit) as u8;
                    free_bits &= !run_mask;
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

        // Keep only the current run and the best candidate. Building the full extent
        // inventory first can consume substantial memory on a heavily fragmented volume.
        let layout = self.volume.layout;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_data_block = layout
            .backup_superblock
            .checked_sub(1)
            .ok_or(Error::Corrupt("volume has no usable data blocks"))?;
        let last_bitmap = last_data_block / bits_per_block;
        let mut best: Option<FreeExtent> = None;
        // Keep memory bounded to one bitmap block even when scanning many segments.
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;

        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_local = start - base;
            let end_local = end - base;
            let first_byte =
                usize::try_from(first_local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte =
                usize::try_from(end_local.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
                    .map_err(|_| Error::ArithmeticOverflow)?;
            let mut current: Option<FreeExtent> = None;

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
                let mut free_bits = !*bitmap_byte & lower_mask & upper_mask;

                while free_bits != 0 {
                    let first_bit = free_bits.trailing_zeros();
                    let run_width = (free_bits >> first_bit).trailing_ones();
                    let block = base
                        .checked_add(byte_start)
                        .and_then(|value| value.checked_add(u64::from(first_bit)))
                        .ok_or(Error::ArithmeticOverflow)?;
                    match current {
                        Some(mut extent)
                            if extent.start_block.checked_add(extent.block_count)
                                == Some(block) =>
                        {
                            extent.block_count = extent
                                .block_count
                                .checked_add(u64::from(run_width))
                                .ok_or(Error::ArithmeticOverflow)?;
                            current = Some(extent);
                        }
                        Some(extent) => {
                            if extent.block_count >= count
                                && best.map_or(true, |candidate| {
                                    (extent.block_count, extent.start_block)
                                        < (candidate.block_count, candidate.start_block)
                                })
                            {
                                best = Some(extent);
                            }
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: u64::from(run_width),
                            });
                        }
                        None => {
                            current = Some(FreeExtent {
                                start_block: block,
                                block_count: u64::from(run_width),
                            });
                        }
                    }
                    let run_mask = ((u16::from(u8::MAX) >> (8 - run_width)) << first_bit) as u8;
                    free_bits &= !run_mask;
                }
            }

            if let Some(extent) = current {
                if extent.block_count >= count
                    && best.map_or(true, |candidate| {
                        (extent.block_count, extent.start_block)
                            < (candidate.block_count, candidate.start_block)
                    })
                {
                    best = Some(extent);
                }
            }
        }

        Ok(best)
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
        let bitmap_index = extent.start_block / bits_per_block;
        let bitmap_block = self
            .volume
            .layout
            .bitmap_start
            .checked_add(bitmap_index)
            .ok_or(Error::ArithmeticOverflow)?;
        let base = bitmap_index
            .checked_mul(bits_per_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = try_zeroed_buffer(
            self.volume.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        device.read_blocks(bitmap_block, &mut bitmap)?;
        let segment_start = self.volume.layout.data_start.max(base);
        let segment_end = self.volume.layout.backup_superblock.min(
            base.checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?,
        );
        validate_bitmap_reserved_bits(&bitmap, base, segment_start, segment_end)?;
        for block in extent.start_block..end {
            let local = block - base;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            if bitmap[byte] & (1_u8 << (local % 8)) != 0 {
                return Err(Error::Corrupt("best-fit extent changed during allocation"));
            }
        }
        // Match every other allocation path: interrupted descriptor staging must not
        // become discoverable when a stale free block is allocated through best-fit.
        clear_stale_descriptor_magic_range(
            device,
            self.volume.block_size,
            extent.start_block,
            end,
        )?;
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
    /// cannot currently update multiple bitmap home blocks atomically. Reserved and padding
    /// bits are validated during the same bitmap pass, avoiding a separate full-volume scan.
    pub fn free_extent_stats<D: BlockDevice>(&self, device: &mut D) -> Result<FreeExtentStats> {
        validate_device(self, device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let mut stats = FreeExtentStats::default();
        // Fragmentation diagnostics may scan the entire volume; allocate the
        // bitmap buffer once instead of once per segment.
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;

        for bitmap_index in 0..layout.bitmap_blocks {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let bitmap_end = base
                .checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(bitmap_end);
            // Bitmap pages wholly before the first data block contain metadata only.
            // Skip them instead of passing an empty/reversed range to the data scanner.
            if start >= end {
                continue;
            }
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;

            // Validate reserved/padding bits separately in bulk, then account for
            // free runs only across bytes that overlap the usable data region.
            crate::allocator_extents::validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_data_bit = start - base;
            let end_data_bit = end - base;
            let first_data_byte =
                usize::try_from(first_data_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_data_byte = usize::try_from(
                end_data_bit
                    .checked_add(7)
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;
            let first_local = start - base;
            let end_local = end - base;
            let mut run_len = 0_u64;

            for (byte_offset, byte) in bitmap[first_data_byte..end_data_byte].iter().enumerate() {
                let byte_index = first_data_byte
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
                let mut free_bits = !*byte & lower_mask & upper_mask;

                while free_bits != 0 {
                    let first_bit = free_bits.trailing_zeros();
                    let width = (free_bits >> first_bit).trailing_ones();
                    if first_bit == 0 && run_len != 0 {
                        run_len = run_len
                            .checked_add(u64::from(width))
                            .ok_or(Error::ArithmeticOverflow)?;
                    } else {
                        if run_len != 0 {
                            stats.free_extent_count = stats
                                .free_extent_count
                                .checked_add(1)
                                .ok_or(Error::ArithmeticOverflow)?;
                            stats.largest_free_extent_blocks =
                                stats.largest_free_extent_blocks.max(run_len);
                        }
                        run_len = u64::from(width);
                    }

                    let after_run = first_bit + width;
                    let run_mask = ((u16::from(u8::MAX) >> (8 - width)) << first_bit) as u8;
                    free_bits &= !run_mask;
                    if u64::from(after_run) < valid_bits {
                        stats.free_extent_count = stats
                            .free_extent_count
                            .checked_add(1)
                            .ok_or(Error::ArithmeticOverflow)?;
                        stats.largest_free_extent_blocks =
                            stats.largest_free_extent_blocks.max(run_len);
                        run_len = 0;
                    }
                }

                // A free suffix may continue into the next byte. Any allocated suffix
                // terminates the current run immediately.
                let last_valid_bit_is_free =
                    valid_bits != 0 && (*byte & (1_u8 << (valid_bits - 1))) == 0;
                if !last_valid_bit_is_free && run_len != 0 {
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
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        let first_bitmap = layout.data_start / bits_per_block;
        let last_bitmap = (layout.backup_superblock - 1) / bits_per_block;
        let mut allocated = Vec::new();
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
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
        let mut coalesced_ranges: Vec<(u64, u64)> = Vec::new();
        coalesced_ranges
            .try_reserve_exact(owned_ranges.len())
            .map_err(|_| {
                Error::AllocationFailed("allocation failed while coalescing owned ranges")
            })?;
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
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;

        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_byte =
                usize::try_from((start - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(
                end.checked_sub(base)
                    .and_then(|value| value.checked_add(7))
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
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
        let layout = self.volume.layout;
        let mut owned = try_copy_blocks(
            owned_data_blocks,
            "allocation failed while building ownership inventory",
        )?;
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
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for bitmap_index in first_bitmap..=last_bitmap {
            let base = bitmap_index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let start = layout.data_start.max(base);
            let end = layout.backup_superblock.min(
                base.checked_add(bits_per_block)
                    .ok_or(Error::ArithmeticOverflow)?,
            );
            let bitmap_block = layout
                .bitmap_start
                .checked_add(bitmap_index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
            validate_bitmap_reserved_bits(&bitmap, base, start, end)?;
            let first_byte =
                usize::try_from((start - base) / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let end_byte = usize::try_from(
                end.checked_sub(base)
                    .and_then(|value| value.checked_add(7))
                    .ok_or(Error::ArithmeticOverflow)?
                    / 8,
            )
            .map_err(|_| Error::ArithmeticOverflow)?;

            for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
                let byte_index = first_byte
                    .checked_add(byte_offset)
                    .ok_or(Error::ArithmeticOverflow)?;
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
        // Reuse the read buffer across all bitmap blocks so the bounded diagnostic
        // remains allocation-light even on large volumes.
        let mut bitmap = try_zeroed_buffer(
            layout.block_size as usize,
            "allocation failed while preparing allocator bitmap",
        )?;
        for index in 0..layout.bitmap_blocks {
            let base = index
                .checked_mul(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let end = base
                .checked_add(bits_per_block)
                .ok_or(Error::ArithmeticOverflow)?;
            let data_start = layout.data_start.max(base);
            let data_end = layout.backup_superblock.min(end);
            let bitmap_block = layout
                .bitmap_start
                .checked_add(index)
                .ok_or(Error::ArithmeticOverflow)?;
            device.read_blocks(bitmap_block, &mut bitmap)?;
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

/// Resolve the exact byte span and edge masks for a half-open bitmap range.
fn bitmap_range_span(bitmap_len: usize, start_bit: u64, end_bit: u64) -> Result<(usize, usize)> {
    if start_bit > end_bit {
        return Err(Error::InvalidInput("bitmap range start exceeds end"));
    }
    let first_byte = usize::try_from(start_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
    let end_byte = usize::try_from(end_bit.checked_add(7).ok_or(Error::ArithmeticOverflow)? / 8)
        .map_err(|_| Error::ArithmeticOverflow)?;
    if end_byte > bitmap_len {
        return Err(Error::InvalidInput("bitmap range exceeds bitmap size"));
    }
    Ok((first_byte, end_byte))
}

fn bitmap_range_mask(
    byte_index: usize,
    first_byte: usize,
    end_byte: usize,
    start_bit: u64,
    end_bit: u64,
) -> u8 {
    let mut mask = u8::MAX;
    if byte_index == first_byte {
        mask &= u8::MAX << (start_bit % 8);
    }
    if byte_index + 1 == end_byte && end_bit % 8 != 0 {
        mask &= (1_u8 << (end_bit % 8)) - 1;
    }
    mask
}

/// Check whether every bit in a half-open bitmap range has the requested state.
pub(crate) fn bitmap_range_matches(
    bitmap: &[u8],
    start_bit: u64,
    end_bit: u64,
    allocated: bool,
) -> Result<bool> {
    let (first_byte, end_byte) = bitmap_range_span(bitmap.len(), start_bit, end_bit)?;
    for (byte_offset, byte) in bitmap[first_byte..end_byte].iter().enumerate() {
        let byte_index = first_byte
            .checked_add(byte_offset)
            .ok_or(Error::ArithmeticOverflow)?;
        let mask = bitmap_range_mask(byte_index, first_byte, end_byte, start_bit, end_bit);
        let bits = *byte & mask;
        if (allocated && bits != mask) || (!allocated && bits != 0) {
            return Ok(false);
        }
    }
    Ok(true)
}

/// Set a half-open bitmap range while preserving bits outside its exact boundaries.
fn set_bitmap_range(
    bitmap: &mut [u8],
    start_bit: u64,
    end_bit: u64,
    allocated: bool,
) -> Result<()> {
    let (first_byte, end_byte) = bitmap_range_span(bitmap.len(), start_bit, end_bit)?;
    for (byte_offset, byte) in bitmap[first_byte..end_byte].iter_mut().enumerate() {
        let byte_index = first_byte
            .checked_add(byte_offset)
            .ok_or(Error::ArithmeticOverflow)?;
        let mask = bitmap_range_mask(byte_index, first_byte, end_byte, start_bit, end_bit);
        if allocated {
            *byte |= mask;
        } else {
            *byte &= !mask;
        }
    }
    Ok(())
}

/// Reject free bits that refer to metadata-reserved blocks or bitmap padding.
pub(crate) fn validate_bitmap_reserved_bits(
    bitmap: &[u8],
    segment_base: u64,
    data_start: u64,
    data_end: u64,
) -> Result<()> {
    if data_start < segment_base || data_end <= data_start {
        return Err(Error::Corrupt("allocation bitmap scan range is invalid"));
    }
    let bitmap_bits = u64::try_from(bitmap.len())
        .map_err(|_| Error::ArithmeticOverflow)?
        .checked_mul(8)
        .ok_or(Error::ArithmeticOverflow)?;
    let segment_end = segment_base
        .checked_add(bitmap_bits)
        .ok_or(Error::ArithmeticOverflow)?;
    if data_end > segment_end {
        return Err(Error::Corrupt(
            "allocation bitmap scan range exceeds its bitmap segment",
        ));
    }

    let first_data_bit = data_start - segment_base;
    let end_data_bit = data_end - segment_base;
    let first_data_byte =
        usize::try_from(first_data_bit / 8).map_err(|_| Error::ArithmeticOverflow)?;
    let end_data_byte = usize::try_from(
        end_data_bit
            .checked_add(7)
            .ok_or(Error::ArithmeticOverflow)?
            / 8,
    )
    .map_err(|_| Error::ArithmeticOverflow)?;

    // Every bit outside the data region is reserved or padding and must remain set.
    // Validate complete prefix/suffix bytes in bulk; only the two boundary bytes
    // need masks. This avoids calculating a mask for every byte of a large bitmap.
    if bitmap[..first_data_byte]
        .iter()
        .any(|byte| *byte != u8::MAX)
        || bitmap[end_data_byte..].iter().any(|byte| *byte != u8::MAX)
    {
        return Err(Error::Corrupt(
            "allocation bitmap marks a reserved or padding block free",
        ));
    }

    let first_bit_offset = first_data_bit % 8;
    if first_bit_offset != 0 {
        let reserved_mask = (1_u8 << first_bit_offset) - 1;
        if bitmap[first_data_byte] & reserved_mask != reserved_mask {
            return Err(Error::Corrupt(
                "allocation bitmap marks a reserved or padding block free",
            ));
        }
    }

    let end_bit_offset = end_data_bit % 8;
    if end_bit_offset != 0 {
        let data_mask = (1_u8 << end_bit_offset) - 1;
        let reserved_mask = !data_mask;
        if bitmap[end_data_byte - 1] & reserved_mask != reserved_mask {
            return Err(Error::Corrupt(
                "allocation bitmap marks a reserved or padding block free",
            ));
        }
    }
    Ok(())
}

fn validate_device<D: BlockDevice>(volume: &OpenedVolume, device: &D) -> Result<()> {
    if device.block_size() != volume.volume.block_size
        || device.block_count() != volume.volume.total_blocks
    {
        return Err(Error::InvalidInput(
            "device geometry does not match opened volume",
        ));
    }
    let canonical = VolumeLayout::calculate_for_version(
        volume.volume.block_size,
        volume.volume.total_blocks,
        volume.format_version,
    )?;
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
    use crate::{format_volume, Extent, ExtentMap};
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

    struct CountRangeReads<D> {
        inner: D,
        multi_block_reads: usize,
    }

    impl<D: BlockDevice> BlockDevice for CountRangeReads<D> {
        fn block_size(&self) -> u32 {
            self.inner.block_size()
        }
        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }
        fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
            if output.len() > self.block_size() as usize {
                self.multi_block_reads += 1;
            }
            self.inner.read_blocks(first_block, output)
        }
        fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
            self.inner.write_blocks(first_block, input)
        }
        fn flush(&mut self) -> Result<()> {
            self.inner.flush()
        }
    }

    #[test]
    fn batch_free_uses_one_range_read_for_adjacent_blocks() {
        let path = image_path("batch-free-range-read-count");
        {
            let mut image = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume(&mut image, *b"openfs-test-0001", 0).unwrap();
            let mut blocks = Vec::new();
            for _ in 0..8 {
                blocks.push(volume.allocate_block(&mut image).unwrap());
            }
            assert!(blocks.windows(2).all(|pair| pair[1] == pair[0] + 1));
            // Warm descriptor discovery before counting reads for the free operation itself.
            assert!(volume
                .discover_persistent_inventory(&mut image)
                .unwrap()
                .is_none());

            let mut device = CountRangeReads {
                inner: image,
                multi_block_reads: 0,
            };
            volume.free_blocks(&mut device, &blocks).unwrap();
            // One batched scan discovers the absent descriptor; the second is the
            // single coalesced scan performed by free_blocks. Per-block guard reads
            // would leave this count at one and add eight single-block data reads.
            assert_eq!(device.multi_block_reads, 2);
            for block in blocks {
                assert!(!volume.is_data_block_allocated(&mut device, block).unwrap());
            }
        }
        fs::remove_file(path).unwrap();
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

                assert!(
                    matches!(
                        crate::inventory_descriptor::reject_freeing_inventory_descriptor_range(
                            &mut device,
                            start,
                            descriptor_block + 1,
                            block_size,
                        ),
                        Err(Error::InvalidInput(
                            "cannot free a persistent inventory descriptor block"
                        ))
                    ),
                    "descriptor in second batch was missed at block size {block_size}"
                );
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

    #[test]
    fn ownership_report_bounds_both_diagnostic_samples_without_losing_totals() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("ownership-report-bounded-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 512).unwrap();
                let mut volume = format_volume(&mut device, *b"openfs-test-0001", 0).unwrap();
                let allocated = volume.allocate_blocks(&mut device, 100).unwrap();

                // More than the 64-entry diagnostic cap in each direction:
                // 80 owners point to blocks that have been freed, while 20
                // still-allocated blocks have no owner.
                volume.free_blocks(&mut device, &allocated[..80]).unwrap();
                let map = ExtentMap::new(vec![Extent {
                    logical_start: 0,
                    physical_start: allocated[0],
                    block_count: 80,
                }])
                .unwrap();
                let report = volume
                    .extent_map_ownership_report(&mut device, &[(&map, 80)])
                    .unwrap();

                assert_eq!(
                    report.orphaned_allocation_count, 20,
                    "block_size={block_size}"
                );
                assert_eq!(
                    report.orphaned_allocations.len(),
                    20,
                    "block_size={block_size}"
                );
                assert!(!report.orphaned_allocations_truncated);
                assert_eq!(
                    report.dangling_reference_count, 80,
                    "block_size={block_size}"
                );
                assert_eq!(
                    report.dangling_references,
                    allocated[..64],
                    "dangling sample must stay deterministic and capped for block_size={block_size}"
                );
                assert!(report.dangling_references_truncated);
                assert!(!report.is_consistent());
            }
            fs::remove_file(path).unwrap();
        }
    }
}
