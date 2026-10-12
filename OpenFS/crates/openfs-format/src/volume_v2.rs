//! Version 2 volume layout with a reserved persistent journal.
//!
//! Version 1 remains unchanged. V2 reserves two checkpoint blocks and a four-block
//! redo ring before the data region, and journals allocator bitmap changes.

use crate::{
    crc32c, format_journal, plan_recovery, JournalCheckpoint, JournalRecord, JournalRegion,
    PersistentJournal, RedoUpdate, JOURNAL_CHECKPOINT_SIZE, SUPERBLOCK_SIZE,
};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

const MAGIC: &[u8; 8] = b"OPENFS\0\0";
const VERSION: u16 = 2;
const HEADER_SIZE: u16 = 48;
const CHECKSUM_OFFSET: usize = 4088;
const JOURNAL_RING_BLOCKS: u64 = 4;
const JOURNAL_RESERVED_BLOCKS: u64 = 2 + JOURNAL_RING_BLOCKS;

/// On-disk regions for format v2. The journal occupies six consecutive blocks.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VolumeLayoutV2 {
    pub total_blocks: u64,
    pub block_size: u32,
    pub bitmap_start: u64,
    pub bitmap_blocks: u64,
    pub journal: JournalRegion,
    pub data_start: u64,
    pub backup_superblock: u64,
}

impl VolumeLayoutV2 {
    pub fn calculate(block_size: u32, total_blocks: u64) -> Result<Self> {
        if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
            return Err(Error::InvalidInput("unsupported filesystem block size"));
        }
        if total_blocks < 12 {
            return Err(Error::InvalidInput(
                "volume is too small for v2 reserved regions",
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
                "volume has no data region after v2 reserved blocks",
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

/// Validated v2 volume geometry and superblock fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VolumeV2 {
    pub block_size: u32,
    pub total_blocks: u64,
    pub features: u64,
    pub uuid: [u8; 16],
    pub layout: VolumeLayoutV2,
}

/// Writable v2 volume. Opening this value runs journal recovery before returning it.
#[derive(Debug)]
pub struct OpenedVolumeV2 {
    pub volume: VolumeV2,
    pub journal: PersistentJournal,
}

impl OpenedVolumeV2 {
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
                    *byte |= 1_u8 << bit;
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

    fn validate_device<D: BlockDevice>(&self, device: &D) -> Result<()> {
        if device.block_size() != self.volume.block_size
            || device.block_count() != self.volume.total_blocks
        {
            return Err(Error::InvalidInput(
                "device geometry does not match opened v2 volume",
            ));
        }
        let canonical =
            VolumeLayoutV2::calculate(self.volume.block_size, self.volume.total_blocks)?;
        if self.volume.layout != canonical {
            return Err(Error::InvalidInput(
                "v2 volume layout was modified after validation",
            ));
        }
        Ok(())
    }
}

/// Result of a non-mutating V2 volume check.
///
/// This verifies the primary/backup superblocks, canonical geometry, and reserved/padding
/// bitmap invariants. It deliberately does not replay the journal or claim namespace
/// ownership integrity; use `open_volume_v2` when recovery is intended.
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

/// Inspect static V2 volume metadata without writing to the device.
///
/// Unlike `open_volume_v2`, this function does not recover or checkpoint the journal.
/// It is suitable for a read-only first-pass checker, not a full crash-recovery verdict.
pub fn check_volume_v2<D: BlockDevice>(device: &mut D) -> Result<VolumeCheckReport> {
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
        return Err(Error::Corrupt("primary and backup v2 superblocks disagree"));
    }
    let (block_size, total_blocks, _features, _uuid) = decode_superblock_v2(&primary)?;
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "v2 superblock geometry does not match device geometry",
        ));
    }
    let layout = VolumeLayoutV2::calculate(block_size, total_blocks)?;
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
    fn read_copy<D: BlockDevice>(device: &mut D, block_number: u64) -> Result<JournalCheckpoint> {
        let mut block = vec![0_u8; device.block_size() as usize];
        device.read_blocks(block_number, &mut block)?;
        if block[JOURNAL_CHECKPOINT_SIZE..]
            .iter()
            .any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("checkpoint block padding must be zero"));
        }
        JournalCheckpoint::decode(&block[..JOURNAL_CHECKPOINT_SIZE])
    }

    let first = read_copy(device, region.checkpoint_a);
    let second = read_copy(device, region.checkpoint_b);
    let (checkpoint, _) = match (first, second) {
        (Ok(a), Ok(b)) if a.generation == b.generation && a != b => {
            return Err(Error::Corrupt(
                "checkpoint copies conflict at the same generation",
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
            Ok(record) => newer.push((slot, record)),
            Err(_) => malformed.push(slot),
        }
    }
    newer.sort_by_key(|(_, record)| record.sequence);
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

/// Format a device as v2. Destructive; call only on a disposable image or explicitly
/// selected device. Physical-device formatting is not exposed by the host utility.
pub fn format_volume_v2<D: BlockDevice>(
    device: &mut D,
    uuid: [u8; 16],
    features: u64,
) -> Result<OpenedVolumeV2> {
    let layout = VolumeLayoutV2::calculate(device.block_size(), device.block_count())?;
    let encoded = encode_superblock_v2(layout.block_size, layout.total_blocks, features, uuid)?;
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
    open_volume_v2(device)
}

/// Validate a v2 volume and recover committed journal records before returning writable state.
pub fn open_volume_v2<D: BlockDevice>(device: &mut D) -> Result<OpenedVolumeV2> {
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
        return Err(Error::Corrupt("primary and backup v2 superblocks disagree"));
    }
    let (block_size, total_blocks, features, uuid) = decode_superblock_v2(&primary)?;
    if block_size != device.block_size() || total_blocks != device.block_count() {
        return Err(Error::Corrupt(
            "v2 superblock geometry does not match device geometry",
        ));
    }
    let layout = VolumeLayoutV2::calculate(block_size, total_blocks)?;
    validate_bitmap(device, &layout)?;
    let journal = crate::journal_engine::open_journal_protected(
        device,
        layout.journal,
        &[0, layout.backup_superblock],
    )?;
    // Recovery may replay a bitmap home-block image. Validate reserved bits again
    // after replay before exposing writable volume state.
    validate_bitmap(device, &layout)?;
    Ok(OpenedVolumeV2 {
        volume: VolumeV2 {
            block_size,
            total_blocks,
            features,
            uuid,
            layout,
        },
        journal,
    })
}

fn encode_superblock_v2(
    block_size: u32,
    total_blocks: u64,
    features: u64,
    uuid: [u8; 16],
) -> Result<[u8; SUPERBLOCK_SIZE]> {
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() || total_blocks < 2 {
        return Err(Error::InvalidInput("invalid v2 superblock geometry"));
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

fn decode_superblock_v2(bytes: &[u8; SUPERBLOCK_SIZE]) -> Result<(u32, u64, u64, [u8; 16])> {
    if &bytes[..8] != MAGIC {
        return Err(Error::Corrupt("bad v2 superblock magic"));
    }
    let version = u16::from_le_bytes([bytes[8], bytes[9]]);
    if version != VERSION {
        return Err(Error::UnsupportedVersion(version));
    }
    if u16::from_le_bytes([bytes[10], bytes[11]]) != HEADER_SIZE {
        return Err(Error::Corrupt("unexpected v2 superblock header size"));
    }
    let stored = u32::from_le_bytes(
        bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
            .try_into()
            .unwrap(),
    );
    if stored != crc32c(&bytes[..CHECKSUM_OFFSET]) {
        return Err(Error::Corrupt("v2 superblock CRC32C mismatch"));
    }
    if bytes[48..CHECKSUM_OFFSET].iter().any(|b| *b != 0) || bytes[4092..].iter().any(|b| *b != 0) {
        return Err(Error::Corrupt("reserved v2 superblock bytes must be zero"));
    }
    let block_size = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    let total_blocks = u64::from_le_bytes(bytes[16..24].try_into().unwrap());
    let features = u64::from_le_bytes(bytes[24..32].try_into().unwrap());
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() || total_blocks < 2 {
        return Err(Error::Corrupt("invalid v2 superblock geometry"));
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

fn validate_bitmap<D: BlockDevice>(device: &mut D, layout: &VolumeLayoutV2) -> Result<()> {
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
                        "v2 bitmap marks a reserved or padding block free",
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
        check_volume_v2, decode_superblock_v2, encode_superblock_v2, format_volume_v2,
        open_volume_v2, VolumeLayoutV2,
    };
    use crate::{open_volume, Extent, ExtentMap};
    use openfs_block::{BlockDevice, ImageDevice, RawDevice};
    use openfs_core::Error;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn image_path(label: &str) -> std::path::PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!(
            "openfs-v2-{label}-{}-{nonce}.img",
            std::process::id()
        ))
    }

    #[test]
    fn v2_superblock_has_stable_golden_bytes_and_checksum() {
        let uuid = *b"openfs-v2-test!!";
        let features = 0x0102_0304_0506_0708;
        let encoded = encode_superblock_v2(4096, 256, features, uuid).unwrap();

        assert_eq!(
            &encoded[..48],
            &[
                79, 80, 69, 78, 70, 83, 0, 0, 2, 0, 48, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 8,
                7, 6, 5, 4, 3, 2, 1, 111, 112, 101, 110, 102, 115, 45, 118, 50, 45, 116, 101, 115,
                116, 33, 33
            ]
        );
        assert_eq!(
            u32::from_le_bytes(encoded[4088..4092].try_into().unwrap()),
            0x0936_7ef8
        );
        assert!(encoded[48..4088].iter().all(|byte| *byte == 0));
        assert!(encoded[4092..].iter().all(|byte| *byte == 0));
        assert_eq!(
            decode_superblock_v2(&encoded).unwrap(),
            (4096, 256, features, uuid)
        );
    }

    #[test]
    fn v2_format_open_allocate_free_roundtrips_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("block-size-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 32).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
                let first = volume.allocate_block(&mut device).unwrap();
                let second = volume.allocate_block(&mut device).unwrap();
                assert_eq!(second, first + 1);
                volume.free_block(&mut device, first).unwrap();
                device.flush().unwrap();
                drop(volume);

                let mut volume = open_volume_v2(&mut device).unwrap();
                assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
                assert_eq!(volume.allocate_block(&mut device).unwrap(), second + 1);
                device.flush().unwrap();
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_journals_allocator_updates_across_reopen_and_keeps_v1_separate() {
        let path = image_path("roundtrip");
        let first;
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            first = volume.allocate_block(&mut device).unwrap();
            assert!(first >= volume.volume.layout.data_start);
            assert!(volume.free_block(&mut device, 0).is_err());
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            assert!(matches!(
                open_volume(&mut device),
                Err(Error::UnsupportedVersion(2))
            ));
            let mut volume = open_volume_v2(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            assert_ne!(first, second);
            volume.free_block(&mut device, first).unwrap();
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_layout_bitmap_boundaries_keep_regions_disjoint() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let bits_per_bitmap_block = u64::from(block_size) * 8;
            for total_blocks in [
                12,
                bits_per_bitmap_block - 1,
                bits_per_bitmap_block,
                bits_per_bitmap_block + 1,
                bits_per_bitmap_block * 2 + 17,
            ] {
                let layout = VolumeLayoutV2::calculate(block_size, total_blocks).unwrap();
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
    fn v2_allocator_rejects_mutated_public_layout_before_io() {
        for mutation in 0..10 {
            let path = image_path(&format!("mutated-layout-{mutation}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                        "v2 volume layout was modified after validation"
                    ))
                ));
                assert!(matches!(
                    volume.free_block(&mut device, 8),
                    Err(Error::InvalidInput(
                        "v2 volume layout was modified after validation"
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

                let reopened = open_volume_v2(&mut device).unwrap();
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
    fn v2_allocator_rejects_device_geometry_mismatch_without_mutation() {
        let volume_path = image_path("geometry-volume");
        let other_path = image_path("geometry-other");
        {
            let mut original = ImageDevice::create(&volume_path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut original, *b"openfs-v2-test!!", 0).unwrap();
            let mut wrong_count = ImageDevice::create(&other_path, 4096, 257).unwrap();
            assert!(matches!(
                volume.allocate_block(&mut wrong_count),
                Err(Error::InvalidInput(
                    "device geometry does not match opened v2 volume"
                ))
            ));
            assert!(matches!(
                volume.free_block(&mut wrong_count, volume.volume.layout.data_start),
                Err(Error::InvalidInput(
                    "device geometry does not match opened v2 volume"
                ))
            ));

            let wrong_size_path = image_path("geometry-wrong-size");
            {
                let mut wrong_size = ImageDevice::create(&wrong_size_path, 8192, 256).unwrap();
                assert!(matches!(
                    volume.allocate_block(&mut wrong_size),
                    Err(Error::InvalidInput(
                        "device geometry does not match opened v2 volume"
                    ))
                ));
                assert!(matches!(
                    volume.free_block(&mut wrong_size, volume.volume.layout.data_start),
                    Err(Error::InvalidInput(
                        "device geometry does not match opened v2 volume"
                    ))
                ));
            }
            fs::remove_file(wrong_size_path).unwrap();

            let reopened = open_volume_v2(&mut original).unwrap();
            assert_eq!(
                reopened.volume.layout.data_start,
                volume.volume.layout.data_start
            );
        }
        fs::remove_file(volume_path).unwrap();
        fs::remove_file(other_path).unwrap();
    }

    #[test]
    fn v2_allocator_exhaustion_and_reuse_preserve_data_region_bounds() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("allocator-exhaustion-{block_size}"));
            {
                // The minimum valid V2 geometry has exactly three data blocks.
                let mut device = ImageDevice::create(&path, block_size, 12).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                let mut reopened = open_volume_v2(&mut device).unwrap();
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
    fn v2_allocator_advances_to_next_bitmap_segment() {
        let bits_per_bitmap_block = 4096_u64 * 8;
        let path = image_path("allocator-bitmap-boundary");
        {
            let mut device = ImageDevice::create(&path, 4096, bits_per_bitmap_block + 12).unwrap();
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

            let mut volume = open_volume_v2(&mut device).unwrap();
            let allocated = volume.allocate_block(&mut device).unwrap();
            assert_eq!(allocated, bits_per_bitmap_block);
            volume.free_block(&mut device, allocated).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), allocated);
        }
        {
            let mut device = ImageDevice::open(&path, 4096).unwrap();
            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                bits_per_bitmap_block + 1
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_allocator_rejects_double_free_and_out_of_range_blocks() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("invalid-free-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                let mut reopened = open_volume_v2(&mut device).unwrap();
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

    #[test]
    fn v2_bitmap_read_failures_do_not_mutate_allocator_state() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("bitmap-read-failure-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                let mut volume = open_volume_v2(&mut device).unwrap();
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

                let mut volume = open_volume_v2(&mut device).unwrap();
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
    fn v2_allocation_keeps_bitmap_unchanged_when_commit_fails_before_durable_record() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..4 {
                let path = image_path(&format!("allocation-precommit-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                    let mut recovered = open_volume_v2(&mut device).unwrap();
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
    fn v2_allocation_recovers_after_durable_commit_and_home_write_failure() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in [4, 5] {
                let path = image_path(&format!("allocation-recovery-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    drop(open_volume_v2(&mut device).unwrap());
                    let mut recovered = open_volume_v2(&mut device).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
                    recovered.free_block(&mut device, first).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn v2_rejects_primary_or_backup_superblock_mismatch() {
        for (label, block) in [("primary", 0_u64), ("backup", 255_u64)] {
            let path = image_path(&format!("superblock-mismatch-{label}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();

                let mut bytes = vec![0_u8; device.block_size() as usize];
                device.read_blocks(block, &mut bytes).unwrap();
                bytes[32] ^= 0x01;
                device.write_blocks(block, &bytes).unwrap();
                device.flush().unwrap();

                assert!(matches!(
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt("primary and backup v2 superblocks disagree"))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_rejects_matching_superblocks_with_bad_checksum() {
        let path = image_path("superblock-bad-checksum");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();

            for block in [0_u64, 255] {
                let mut bytes = vec![0_u8; device.block_size() as usize];
                device.read_blocks(block, &mut bytes).unwrap();
                bytes[40] ^= 0x01;
                device.write_blocks(block, &bytes).unwrap();
            }
            device.flush().unwrap();

            assert!(matches!(
                open_volume_v2(&mut device),
                Err(Error::Corrupt("v2 superblock CRC32C mismatch"))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_rejects_nonzero_reserved_superblock_bytes_with_valid_checksum() {
        for (label, offset) in [
            ("superblock-reserved-header-byte", 48_usize),
            ("superblock-reserved-tail-byte", 4092_usize),
        ] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();

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
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt("reserved v2 superblock bytes must be zero"))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_rejects_invalid_superblock_headers_and_geometry() {
        let cases: [(&str, usize, &[u8], &'static str); 2] = [
            ("bad-magic", 0, b"BADMAGIC", "bad v2 superblock magic"),
            (
                "bad-header-size",
                10,
                &[0x31, 0x00],
                "unexpected v2 superblock header size",
            ),
        ];

        for (label, offset, replacement, expected) in cases {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt(message)) if message == expected
                ));
            }
            fs::remove_file(path).unwrap();
        }

        let path = image_path("unsupported-version");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                open_volume_v2(&mut device),
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
                format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt("invalid v2 superblock geometry"))
                ));
            }
            fs::remove_file(path).unwrap();
        }

        // A checksum-valid superblock must still reject byte-size overflow
        // before comparing its claimed geometry with the backing device.
        let path = image_path("overflowing-byte-geometry");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                open_volume_v2(&mut device),
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
                format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt(
                        "v2 superblock geometry does not match device geometry"
                    ))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_rejects_reserved_and_padding_bitmap_corruption_on_open() {
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
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                            open_volume_v2(&mut device),
                            Err(Error::Corrupt(
                                "v2 bitmap marks a reserved or padding block free"
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
    fn v2_rejects_backup_and_padding_corruption_in_later_bitmap_segment() {
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
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                        open_volume_v2(&mut device),
                        Err(Error::Corrupt(
                            "v2 bitmap marks a reserved or padding block free"
                        ))
                    ),
                    "accepted {label} corruption in second bitmap segment"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_rejects_each_reserved_block_marked_free() {
        let reserved_blocks = (0..256_u64)
            .filter(|block| {
                VolumeLayoutV2::calculate(4096, 256)
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
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                            open_volume_v2(&mut device),
                            Err(Error::Corrupt(
                                "v2 bitmap marks a reserved or padding block free"
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
    fn v2_allocator_reuses_freed_blocks_in_order_after_reopen() {
        let path = image_path("allocator-reuse-order");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

            let mut volume = open_volume_v2(&mut device).unwrap();
            for block in allocated.iter().step_by(2) {
                assert_eq!(volume.allocate_block(&mut device).unwrap(), *block);
            }
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                first + allocated.len() as u64
            );
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(
                volume.allocate_block(&mut device).unwrap(),
                first + allocated.len() as u64 + 1
            );
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_rejects_each_reserved_bitmap_corruption_replayed_from_journal() {
        // Exercise every reserved bit through durable journal replay, not just
        // the primary superblock bit. A minimum-size volume keeps this matrix cheap.
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let layout = VolumeLayoutV2::calculate(block_size, 12).unwrap();
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
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                            open_volume_v2(&mut device),
                            Err(Error::Corrupt(
                                "v2 bitmap marks a reserved or padding block free"
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
    fn v2_rejects_later_segment_reserved_and_padding_corruption_replayed_from_journal() {
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
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                        open_volume_v2(&mut device),
                        Err(Error::Corrupt(
                            "v2 bitmap marks a reserved or padding block free"
                        ))
                    ),
                    "replay must reject {label} corruption in the second bitmap segment"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_free_keeps_allocation_when_commit_fails_before_durable_record() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in 0..4 {
                let path = image_path(&format!("free-precommit-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume =
                        format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                    let mut recovered = open_volume_v2(&mut device).unwrap();
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
    fn v2_free_recovers_after_durable_commit_and_home_write_failure() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for fail_at in [4, 5] {
                let path = image_path(&format!("free-recovery-{block_size}-{fail_at}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                    let mut volume =
                        format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                    drop(open_volume_v2(&mut device).unwrap());
                    let mut recovered = open_volume_v2(&mut device).unwrap();
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
                    assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
                }
                fs::remove_file(path).unwrap();
            }
        }
    }
    #[test]
    fn v2_format_allocate_and_reopen_through_raw_device_backend() {
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
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            first = volume.allocate_block(&mut device).unwrap();
            device.flush().unwrap();
        }
        {
            let mut device = RawDevice::open(&path, 4096, 256).unwrap();
            let mut volume = open_volume_v2(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            assert_ne!(first, second);
            volume.free_block(&mut device, first).unwrap();
            device.flush().unwrap();
        }
        {
            let mut device = RawDevice::open(&path, 4096, 256).unwrap();
            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_minimum_volume_exhausts_and_reuses_only_data_blocks() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("minimum-volume-exhaustion-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 12).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_allocator_handles_last_data_block_after_bitmap_segment_boundary() {
        for block_size in [4096_u32, 8192] {
            let total_blocks = 65_537_u64;
            let path = image_path(&format!("last-data-block-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, total_blocks).unwrap();
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                let mut volume = open_volume_v2(&mut device).unwrap();
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

                let mut reopened = open_volume_v2(&mut device).unwrap();
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
    fn v2_later_bitmap_segment_read_failures_preserve_allocator_state() {
        for block_size in [4096_u32] {
            let path = image_path(&format!("later-bitmap-read-failure-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 65_537).unwrap();
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                let mut volume = open_volume_v2(&mut device).unwrap();
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

                let mut recovered = open_volume_v2(&mut device).unwrap();
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

                let mut recovered = open_volume_v2(&mut device).unwrap();
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
    fn v2_allocator_queries_report_free_space_and_block_state() {
        let path = image_path("allocator-state-queries");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_contiguous_allocator_is_atomic_and_persistent() {
        let path = image_path("contiguous-allocator");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

            let mut reopened = open_volume_v2(&mut device).unwrap();
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
    fn v2_contiguous_allocator_rejects_unrepresentable_extents_without_mutation() {
        let path = image_path("contiguous-allocator-limits");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_batch_allocator_and_free_are_atomic_and_persistent() {
        let path = image_path("batch-allocator");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

            let mut reopened = open_volume_v2(&mut device).unwrap();
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
    fn v2_bitmap_validator_rejects_padding_corruption_in_second_segment() {
        // 4096-byte bitmap blocks cover 32768 volume blocks each. Keep the
        // image sparse so this regression exercises the second segment without
        // requiring a large physical test fixture.
        let path = image_path("bitmap-validator-second-segment-padding");
        {
            let mut device = ImageDevice::create(&path, 4096, 32_770).unwrap();
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_public_bitmap_validator_rejects_reserved_and_padding_corruption_for_all_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for (label, corrupt_block) in [("reserved", 0_u64), ("padding", 300_u64)] {
                let path = image_path(&format!("bitmap-validator-{block_size}-{label}"));
                {
                    let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                    let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_batch_allocator_roundtrips_all_supported_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("batch-roundtrip-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                let mut reopened = open_volume_v2(&mut device).unwrap();
                assert_eq!(reopened.allocation_stats(&mut device).unwrap(), initial);
                let allocated_again = reopened.allocate_blocks(&mut device, 4).unwrap();
                assert_eq!(allocated_again, blocks);
                reopened.free_blocks(&mut device, &allocated_again).unwrap();
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_read_only_checker_reports_allocator_state_and_rejects_mirror_mismatch() {
        let path = image_path("read-only-checker");
        {
            let mut device = ImageDevice::create(&path, 4096, 512).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            device.flush().unwrap();
            let bytes_before_check = fs::read(&path).unwrap();
            let initial = check_volume_v2(&mut device).unwrap();
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
            let after_allocate = check_volume_v2(&mut device).unwrap();
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
            assert!(matches!(
                check_volume_v2(&mut device),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_read_only_checker_validates_journal_copies_for_all_block_sizes() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("checker-journal-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 257).unwrap();
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                let report = check_volume_v2(&mut device).unwrap();
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
                    matches!(check_volume_v2(&mut device), Err(Error::Corrupt(_))),
                    "checker accepted two invalid checkpoint copies at block size {block_size}"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_layout_regions_are_disjoint_across_supported_geometries() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            for total_blocks in [12_u64, 13, 255, 256, 4097, 65_537, 1_000_003] {
                let layout = VolumeLayoutV2::calculate(block_size, total_blocks).unwrap();
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
    fn v2_allocator_persists_mixed_allocate_free_sequence_across_reopens() {
        let path = image_path("mixed-allocation-reopen-sequence");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            let first = volume.allocate_block(&mut device).unwrap();
            let second = volume.allocate_block(&mut device).unwrap();
            let third = volume.allocate_block(&mut device).unwrap();
            assert_eq!(second, first + 1);
            assert_eq!(third, second + 1);

            volume.free_block(&mut device, second).unwrap();
            volume.free_block(&mut device, first).unwrap();
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), first);
            assert_eq!(volume.allocate_block(&mut device).unwrap(), second);
            assert_eq!(volume.allocate_block(&mut device).unwrap(), third + 1);
            device.flush().unwrap();
            drop(volume);

            let mut volume = open_volume_v2(&mut device).unwrap();
            assert_eq!(volume.allocate_block(&mut device).unwrap(), third + 2);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_allocated_block_inventory_tracks_persistent_bitmap_state() {
        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            let path = image_path(&format!("allocated-block-inventory-{block_size}"));
            {
                let mut device = ImageDevice::create(&path, block_size, 256).unwrap();
                let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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

                let volume = open_volume_v2(&mut device).unwrap();
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
    fn v2_extent_map_ownership_bridge_checks_bitmap_and_cross_file_aliases() {
        let path = image_path("extent-map-ownership-bridge");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
                .validate_extent_map_ownership(
                    &mut device,
                    &[(&first_map, 2), (&second_map, 2)],
                )
                .unwrap();

            // Every allocated block must have exactly one owner.
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
            assert!(matches!(
                volume.validate_extent_map_ownership(&mut device, &[(&dangling, 1)]),
                Err(Error::Corrupt(_))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_allocation_ownership_validator_detects_orphans_and_dangling_references() {
        let path = image_path("ownership-inventory");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
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
    fn v2_layout_rejects_too_small_volumes() {
        assert!(VolumeLayoutV2::calculate(4096, 11).is_err());
    }

    #[test]
    fn v2_layout_pins_minimum_geometry_and_rejects_invalid_or_overflowing_inputs() {
        let minimum = VolumeLayoutV2::calculate(4096, 12).unwrap();
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
                    VolumeLayoutV2::calculate(block_size, 256),
                    Err(Error::InvalidInput(_))
                ),
                "unexpectedly accepted v2 block size {block_size}"
            );
        }

        for block_size in [4096_u32, 8192, 16_384, 32_768, 65_536] {
            assert!(matches!(
                VolumeLayoutV2::calculate(block_size, u64::MAX),
                Err(Error::ArithmeticOverflow)
            ));
            assert!(matches!(
                VolumeLayoutV2::calculate(block_size, u64::MAX / u64::from(block_size) + 1),
                Err(Error::ArithmeticOverflow)
            ));
            assert!(matches!(
                VolumeLayoutV2::calculate(block_size, 12),
                Ok(layout) if layout.data_blocks() > 0
            ));
        }
    }
}
