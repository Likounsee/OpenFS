//! Atomic contiguous allocation helpers for v2 volumes.
//!
//! The current on-disk journal can atomically update one bitmap block per transaction.
//! Extents are therefore deliberately constrained to a single bitmap segment.

use crate::{OpenedVolumeV2, RedoUpdate, VolumeLayoutV2};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

impl OpenedVolumeV2 {
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
            let end = layout.backup_superblock.min(base.saturating_add(bits_per_block));
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
            return Err(Error::InvalidInput("contiguous extent length must be nonzero"));
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

    /// Check reserved and padding bits in the allocation bitmap without repairing it.
    pub fn validate_allocation_bitmap<D: BlockDevice>(&self, device: &mut D) -> Result<()> {
        validate_device(self, device)?;
        let layout = self.volume.layout;
        let bits_per_block = u64::from(layout.block_size) * 8;
        for index in 0..layout.bitmap_blocks {
            let base = index.checked_mul(bits_per_block).ok_or(Error::ArithmeticOverflow)?;
            let mut bitmap = vec![0_u8; layout.block_size as usize];
            device.read_blocks(layout.bitmap_start + index, &mut bitmap)?;
            for local in 0..bits_per_block {
                let block = base.checked_add(local).ok_or(Error::ArithmeticOverflow)?;
                let reserved = block == 0
                    || (block >= layout.bitmap_start
                        && block < layout.bitmap_start + layout.bitmap_blocks)
                    || block == layout.journal.checkpoint_a
                    || block == layout.journal.checkpoint_b
                    || (block >= layout.journal.ring_start
                        && block - layout.journal.ring_start < layout.journal.ring_blocks)
                    || block == layout.backup_superblock
                    || block >= layout.total_blocks;
                if reserved {
                    let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
                    if bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                        return Err(Error::Corrupt(
                            "v2 bitmap marks a reserved or padding block free",
                        ));
                    }
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
    let canonical = VolumeLayoutV2::calculate(volume.volume.block_size, volume.volume.total_blocks)?;
    if volume.volume.layout != canonical {
        return Err(Error::InvalidInput(
            "v2 volume layout was modified after validation",
        ));
    }
    Ok(())
}
