//! Version 2 volume layout with a reserved persistent journal.
//!
//! Version 1 remains unchanged. V2 reserves two checkpoint blocks and a four-block
//! redo ring before the data region, and journals allocator bitmap changes.

use crate::{
    crc32c, format_journal, JournalRegion, PersistentJournal, RedoUpdate, SUPERBLOCK_SIZE,
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
        for block in self.volume.layout.data_start..self.volume.layout.backup_superblock {
            let bitmap_index = block / bits_per_block;
            let local = block % bits_per_block;
            let bitmap_block = self.volume.layout.bitmap_start + bitmap_index;
            let mut bitmap = vec![0_u8; block_size];
            device.read_blocks(bitmap_block, &mut bitmap)?;
            let byte = usize::try_from(local / 8).map_err(|_| Error::ArithmeticOverflow)?;
            let mask = 1_u8 << (local % 8);
            if bitmap[byte] & mask == 0 {
                bitmap[byte] |= mask;
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
        for block in first..first + bits_per_block {
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
    use super::{format_volume_v2, open_volume_v2, VolumeLayoutV2};
    use crate::open_volume;
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
        for block_size in [4096_u32, 8192, 65_536] {
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
        let path = image_path("mutated-layout");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            volume.volume.layout.data_start = 0;
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
            let reopened = open_volume_v2(&mut device).unwrap();
            assert_eq!(reopened.volume.layout.data_start, 8);
        }
        fs::remove_file(path).unwrap();
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
        let path = image_path("allocator-exhaustion");
        {
            // The minimum valid V2 geometry has exactly three data blocks.
            let mut device = ImageDevice::create(&path, 4096, 12).unwrap();
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
            assert!(volume.allocate_block(&mut device).is_err());
            let reopened = open_volume_v2(&mut device).unwrap();
            assert_eq!(reopened.volume.layout.data_start, 8);
            assert_eq!(reopened.volume.layout.backup_superblock, 11);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_allocator_rejects_double_free_and_out_of_range_blocks() {
        let path = image_path("invalid-free");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            let allocated = volume.allocate_block(&mut device).unwrap();

            volume.free_block(&mut device, allocated).unwrap();
            assert!(matches!(
                volume.free_block(&mut device, allocated),
                Err(Error::InvalidInput("data block is already free"))
            ));
            assert!(matches!(
                volume.free_block(&mut device, volume.volume.layout.backup_superblock),
                Err(Error::InvalidInput(
                    "cannot free a reserved or out-of-range block",
                ))
            ));
            assert!(matches!(
                volume.free_block(&mut device, u64::MAX),
                Err(Error::InvalidInput(
                    "cannot free a reserved or out-of-range block",
                ))
            ));

            // Invalid free attempts must not damage the journal or bitmap.
            let reopened = open_volume_v2(&mut device).unwrap();
            assert_eq!(reopened.journal.checkpoint().checkpoint_sequence, 8);
        }
        fs::remove_file(path).unwrap();
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
    fn v2_allocation_keeps_bitmap_unchanged_when_commit_fails_before_durable_record() {
        for fail_at in 0..4 {
            let path = image_path(&format!("allocation-precommit-{fail_at}"));
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
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
                    "expected injected failure at journal write {fail_at}"
                );
                device = failing.inner;
                drop(volume);

                let mut recovered = open_volume_v2(&mut device).unwrap();
                assert_eq!(
                    recovered.allocate_block(&mut device).unwrap(),
                    first,
                    "pre-commit failure at write {fail_at} must not consume the block"
                );
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_allocation_recovers_after_durable_commit_and_home_write_failure() {
        let path = image_path("allocation-recovery");
        let first;
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            first = volume.volume.layout.data_start;
            let mut failing = FailWriteAt {
                inner: device,
                write_index: 0,
                fail_at: 4,
            };
            let mut volume = volume;
            assert!(matches!(
                volume.allocate_block(&mut failing),
                Err(Error::Io(_))
            ));
            device = failing.inner;
            drop(volume);

            let mut recovered = open_volume_v2(&mut device).unwrap();
            assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
            recovered.free_block(&mut device, first).unwrap();
            assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_rejects_reserved_and_padding_bitmap_corruption_on_open() {
        for (label, bit) in [("reserved-bit", 0_u64), ("padding-bit", 256_u64)] {
            let path = image_path(label);
            {
                let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
                let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
                let bitmap_block = volume.volume.layout.bitmap_start + bit / (4096 * 8);
                let local = bit % (4096 * 8);
                let mut bitmap = vec![0_u8; device.block_size() as usize];
                device.read_blocks(bitmap_block, &mut bitmap).unwrap();
                bitmap[(local / 8) as usize] &= !(1_u8 << (local % 8));
                device.write_blocks(bitmap_block, &bitmap).unwrap();
                device.flush().unwrap();

                assert!(matches!(
                    open_volume_v2(&mut device),
                    Err(Error::Corrupt(
                        "v2 bitmap marks a reserved or padding block free"
                    ))
                ));
            }
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn v2_rejects_reserved_bitmap_corruption_replayed_from_journal() {
        let path = image_path("reserved-bitmap-replay");
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            let bitmap_block = volume.volume.layout.bitmap_start;
            let mut bitmap = vec![0_u8; device.block_size() as usize];
            device.read_blocks(bitmap_block, &mut bitmap).unwrap();
            bitmap[0] &= !1; // Mark primary-superblock block zero as free.

            let mut failing = FailWriteAt {
                inner: device,
                write_index: 0,
                fail_at: 4, // Fail after the commit record is durable, at the home write.
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
            device = failing.inner;
            drop(volume);

            assert!(matches!(
                open_volume_v2(&mut device),
                Err(Error::Corrupt(
                    "v2 bitmap marks a reserved or padding block free"
                ))
            ));
        }
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn v2_free_recovers_after_durable_commit_and_home_write_failure() {
        let path = image_path("free-recovery");
        let first;
        {
            let mut device = ImageDevice::create(&path, 4096, 256).unwrap();
            let mut volume = format_volume_v2(&mut device, *b"openfs-v2-test!!", 0).unwrap();
            first = volume.allocate_block(&mut device).unwrap();

            let mut failing = FailWriteAt {
                inner: device,
                write_index: 0,
                fail_at: 4,
            };
            assert!(matches!(
                volume.free_block(&mut failing, first),
                Err(Error::Io(_))
            ));
            let mut device = failing.inner;
            drop(volume);

            let mut recovered = open_volume_v2(&mut device).unwrap();
            assert_eq!(recovered.allocate_block(&mut device).unwrap(), first);
            assert_eq!(recovered.allocate_block(&mut device).unwrap(), first + 1);
        }
        fs::remove_file(path).unwrap();
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

        assert!(matches!(
            VolumeLayoutV2::calculate(4096, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(matches!(
            VolumeLayoutV2::calculate(4096, 12),
            Ok(layout) if layout.data_blocks() > 0
        ));
    }
}
