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

impl OpenedVolumeV2 {
    /// Return total, allocated, and free data-block counts from the current bitmap.
    pub fn allocation_stats<D: BlockDevice>(&self, device: &mut D) -> Result<AllocationStats> {
        let free = self.free_data_block_count(device)?;
        let total = self.volume.layout.data_blocks();
        let allocated = total.checked_sub(free).ok_or(Error::Corrupt(
            "free-space count exceeds the usable data region",
        ))?;
        Ok(AllocationStats {
            total_data_blocks: total,
            allocated_data_blocks: allocated,
            free_data_blocks: free,
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

    /// Verify that the allocation bitmap exactly matches the caller's discovered ownership map.
    ///
    /// owned_data_blocks must contain every data block referenced by persistent metadata,
    /// with each block listed once. This detects both orphaned allocations (bitmap set but no
    /// owner) and dangling ownership references (owner points to a free block). It does not
    /// discover metadata itself; the persistent namespace scanner must supply that inventory.
    pub fn validate_allocation_ownership<D: BlockDevice>(
        &self,
        device: &mut D,
        owned_data_blocks: &[u64],
    ) -> Result<()> {
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
                if allocated != referenced {
                    return Err(Error::Corrupt(
                        "allocation bitmap and persistent block ownership disagree",
                    ));
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
        Ok(())
    }
    /// Check reserved and padding bits in the allocation bitmap without repairing it.
    ///
    /// The scan is byte-oriented: it verifies only bits outside the usable data region,
    /// instead of iterating over every individual block. The running time is therefore
    /// proportional to bitmap bytes, including for large volumes.
    pub fn validate_allocation_bitmap<D: BlockDevice>(&self, device: &mut D) -> Result<()> {
        validate_device(self, device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
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

            // Every bit outside [data_start, data_end) must be set. Build the
            // required mask byte-by-byte, so valid data bits are not constrained.
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
                let required_reserved_mask = !data_mask;
                if byte & required_reserved_mask != required_reserved_mask {
                    return Err(Error::Corrupt(
                        "v2 bitmap marks a reserved or padding block free",
                    ));
                }
            }
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
