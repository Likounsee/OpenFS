//! Initial volume layout, formatter, and read-only open-time validation.
//!
//! Formatting is destructive and is not crash-atomic. Do not expose it as a
//! safe recovery mechanism until the journal/recovery layer is implemented.

use crate::{Superblock, SUPERBLOCK_SIZE};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

/// First block containing the allocation bitmap. Block zero stores the primary superblock.
pub const BITMAP_START_BLOCK: u64 = 1;

/// Derived v1 volume layout. The last block stores the backup superblock.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VolumeLayout {
    pub total_blocks: u64,
    pub block_size: u32,
    pub bitmap_start: u64,
    pub bitmap_blocks: u64,
    pub data_start: u64,
    pub backup_superblock: u64,
}

impl VolumeLayout {
    /// Derive all regions using checked arithmetic; reject volumes with no data blocks.
    pub fn calculate(block_size: u32, total_blocks: u64) -> Result<Self> {
        if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
            return Err(Error::InvalidInput("unsupported filesystem block size"));
        }
        if total_blocks < 4 {
            return Err(Error::InvalidInput(
                "volume is too small for v1 reserved regions",
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
        let data_start = BITMAP_START_BLOCK
            .checked_add(bitmap_blocks)
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
            bitmap_start: BITMAP_START_BLOCK,
            bitmap_blocks,
            data_start,
            backup_superblock,
        })
    }
    pub fn data_blocks(&self) -> u64 {
        self.backup_superblock - self.data_start
    }

    /// Allocate the first free data block and persist its bitmap bit.
    ///
    /// The caller must serialize allocator operations. Without the journal, an I/O
    /// error during write/flush can leave the outcome uncertain; reopen and validate
    /// the volume before retrying such an operation.
    pub fn allocate_block<D: BlockDevice>(&self, device: &mut D) -> Result<u64> {
        self.validate_device_geometry(device)?;
        let block_size = usize::try_from(self.layout.block_size)
            .map_err(|_| Error::ArithmeticOverflow)?;
        let bits_per_bitmap_block = u64::from(self.layout.block_size) * 8;
        let mut bitmap = vec![0_u8; block_size];

        for block in self.layout.data_start..self.layout.backup_superblock {
            let bitmap_index = block / bits_per_bitmap_block;
            let local = block % bits_per_bitmap_block;
            device.read_blocks(self.layout.bitmap_start + bitmap_index, &mut bitmap)?;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let mask = 1_u8 << (local % 8);
            if bitmap[byte] & mask == 0 {
                bitmap[byte] |= mask;
                device.write_blocks(self.layout.bitmap_start + bitmap_index, &bitmap)?;
                device.flush()?;
                return Ok(block);
            }
        }
        Err(Error::InvalidInput("volume has no free data blocks"))
    }

    /// Release a data block after verifying it is allocated and not reserved.
    ///
    /// This low-level primitive does not know whether metadata still references the
    /// block. The caller must first ensure it is no longer reachable and serialize
    /// allocator operations. Journaling/recovery is not implemented yet.
    pub fn free_block<D: BlockDevice>(&self, device: &mut D, block: u64) -> Result<()> {
        self.validate_device_geometry(device)?;
        if block < self.layout.data_start || block >= self.layout.backup_superblock {
            return Err(Error::InvalidInput("cannot free a reserved or out-of-range block"));
        }
        let block_size = usize::try_from(self.layout.block_size)
            .map_err(|_| Error::ArithmeticOverflow)?;
        let bits_per_bitmap_block = u64::from(self.layout.block_size) * 8;
        let bitmap_index = block / bits_per_bitmap_block;
        let local = block % bits_per_bitmap_block;
        let mut bitmap = vec![0_u8; block_size];
        device.read_blocks(self.layout.bitmap_start + bitmap_index, &mut bitmap)?;
        let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
        let mask = 1_u8 << (local % 8);
        if bitmap[byte] & mask == 0 {
            return Err(Error::InvalidInput("data block is already free"));
        }
        bitmap[byte] &= !mask;
        device.write_blocks(self.layout.bitmap_start + bitmap_index, &bitmap)?;
        device.flush()
    }

    fn validate_device_geometry<D: BlockDevice>(&self, device: &D) -> Result<()> {
        if device.block_size() != self.layout.block_size
            || device.block_count() != self.layout.total_blocks
        {
            return Err(Error::InvalidInput("device geometry does not match opened volume"));
        }
        Ok(())
    }
}

/// A validated, read-only view of the initial on-disk volume format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Volume {
    pub superblock: Superblock,
    pub layout: VolumeLayout,
}

/// Format an existing block device with mirrored superblocks and an initialized bitmap.
///
/// This overwrites blocks in the supplied device. It writes the backup superblock and
/// bitmap first, flushes, then writes the primary superblock last. This ordering helps
/// reject many incomplete formats, but is not an atomic transaction or crash guarantee.
pub fn format_volume<D: BlockDevice>(
    device: &mut D,
    uuid: [u8; 16],
    features: u64,
) -> Result<Volume> {
    let layout = VolumeLayout::calculate(device.block_size(), device.block_count())?;
    let superblock = Superblock {
        block_size: layout.block_size,
        total_blocks: layout.total_blocks,
        features,
        uuid,
    };
    let encoded = superblock.encode()?;
    let bytes_per_bitmap_block =
        usize::try_from(layout.block_size).map_err(|_| Error::ArithmeticOverflow)?;
    let bits_per_bitmap_block = u64::from(layout.block_size) * 8;
    let bitmap_end = layout.bitmap_start + layout.bitmap_blocks;
    // Write backup first; the primary is published only after the bitmap is persisted.
    write_superblock_block(device, layout.backup_superblock, &encoded)?;
    for bitmap_index in 0..layout.bitmap_blocks {
        let first_bit = bitmap_index
            .checked_mul(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = vec![0_u8; bytes_per_bitmap_block];
        set_bitmap_bit(&mut bitmap, first_bit, 0);
        for reserved in layout.bitmap_start..bitmap_end {
            set_bitmap_bit(&mut bitmap, first_bit, reserved);
        }
        set_bitmap_bit(&mut bitmap, first_bit, layout.backup_superblock);
        let bitmap_capacity = first_bit
            .checked_add(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        for bit in layout.total_blocks.max(first_bit)..bitmap_capacity {
            set_bitmap_bit(&mut bitmap, first_bit, bit);
        }
        device.write_blocks(layout.bitmap_start + bitmap_index, &bitmap)?;
    }
    device.flush()?;
    write_superblock_block(device, 0, &encoded)?;
    device.flush()?;
    Ok(Volume { superblock, layout })
}

/// Open and validate an existing volume without modifying it.
pub fn open_volume<D: BlockDevice>(device: &mut D) -> Result<Volume> {
    if device.block_size() < SUPERBLOCK_SIZE as u32 {
        return Err(Error::InvalidInput(
            "device block is smaller than the superblock record",
        ));
    }
    let last = device
        .block_count()
        .checked_sub(1)
        .ok_or(Error::InvalidInput("empty device"))?;
    let primary = read_superblock_block(device, 0)?;
    let backup = read_superblock_block(device, last)?;
    if primary != backup {
        return Err(Error::Corrupt("primary and backup superblocks disagree"));
    }
    if primary.block_size != device.block_size() || primary.total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "superblock geometry does not match device geometry",
        ));
    }
    let layout = VolumeLayout::calculate(primary.block_size, primary.total_blocks)?;
    validate_reserved_bitmap_bits(device, &layout)?;
    Ok(Volume {
        superblock: primary,
        layout,
    })
}

fn write_superblock_block<D: BlockDevice>(
    device: &mut D,
    block: u64,
    encoded: &[u8; SUPERBLOCK_SIZE],
) -> Result<()> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    if block_size < SUPERBLOCK_SIZE {
        return Err(Error::InvalidInput(
            "device block is smaller than superblock",
        ));
    }
    let mut buffer = vec![0_u8; block_size];
    buffer[..SUPERBLOCK_SIZE].copy_from_slice(encoded);
    device.write_blocks(block, &buffer)
}

fn read_superblock_block<D: BlockDevice>(device: &mut D, block: u64) -> Result<Superblock> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    if block_size < SUPERBLOCK_SIZE {
        return Err(Error::InvalidInput(
            "device block is smaller than superblock",
        ));
    }
    let mut buffer = vec![0_u8; block_size];
    device.read_blocks(block, &mut buffer)?;
    if buffer[SUPERBLOCK_SIZE..].iter().any(|byte| *byte != 0) {
        return Err(Error::Corrupt(
            "nonzero bytes after superblock record in reserved block",
        ));
    }
    Superblock::decode(&buffer[..SUPERBLOCK_SIZE])
}

fn set_bitmap_bit(bitmap: &mut [u8], first_bit: u64, bit: u64) {
    let capacity = (bitmap.len() as u64) * 8;
    let Some(end) = first_bit.checked_add(capacity) else {
        return;
    };
    if bit < first_bit || bit >= end {
        return;
    }
    let local = bit - first_bit;
    let byte = (local / 8) as usize;
    bitmap[byte] |= 1_u8 << (local % 8);
}

fn validate_reserved_bitmap_bits<D: BlockDevice>(
    device: &mut D,
    layout: &VolumeLayout,
) -> Result<()> {
    let block_size = usize::try_from(layout.block_size).map_err(|_| Error::ArithmeticOverflow)?;
    let bits_per_bitmap_block = u64::from(layout.block_size) * 8;
    let bitmap_end = layout.bitmap_start + layout.bitmap_blocks;
    for bitmap_index in 0..layout.bitmap_blocks {
        let first_bit = bitmap_index
            .checked_mul(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let end_bit = first_bit
            .checked_add(bits_per_bitmap_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut bitmap = vec![0_u8; block_size];
        device.read_blocks(layout.bitmap_start + bitmap_index, &mut bitmap)?;
        for reserved in core::iter::once(0)
            .chain(layout.bitmap_start..bitmap_end)
            .chain(core::iter::once(layout.backup_superblock))
        {
            if reserved >= first_bit && reserved < end_bit {
                let local = reserved - first_bit;
                let byte = (local / 8) as usize;
                if bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                    return Err(Error::Corrupt("bitmap marks a reserved block as free"));
                }
            }
        }
        let valid_end = layout.total_blocks.min(end_bit);
        for bit in valid_end..end_bit {
            let local = bit - first_bit;
            let byte = (local / 8) as usize;
            if bitmap[byte] & (1_u8 << (local % 8)) == 0 {
                return Err(Error::Corrupt(
                    "bitmap padding bits must be marked allocated",
                ));
            }
        }
    }
    Ok(())
}
