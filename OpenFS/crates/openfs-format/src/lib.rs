//! Explicit byte-level encoding for the initial OpenFS superblock draft.

use openfs_core::{Error, Result};

pub use allocator_extents::{AllocationOwnershipReport, AllocationStats};

/// Size of the initial fixed superblock record.
pub const SUPERBLOCK_SIZE: usize = 4096;
/// Supported format version for this draft.
pub const FORMAT_VERSION: u16 = 1;
const MAGIC: &[u8; 8] = b"OPENFS\0\0";
const HEADER_SIZE: u16 = 48;
const CHECKSUM_OFFSET: usize = 4088;

/// Validated logical superblock fields. This Rust struct is not written directly to disk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Superblock {
    pub block_size: u32,
    pub total_blocks: u64,
    pub features: u64,
    pub uuid: [u8; 16],
}

impl Superblock {
    /// Validate fields and encode a zero-padded, little-endian 4096-byte record.
    pub fn encode(&self) -> Result<[u8; SUPERBLOCK_SIZE]> {
        validate_fields(self.block_size, self.total_blocks)?;
        let mut bytes = [0_u8; SUPERBLOCK_SIZE];
        bytes[0..8].copy_from_slice(MAGIC);
        put_u16(&mut bytes, 8, FORMAT_VERSION);
        put_u16(&mut bytes, 10, HEADER_SIZE);
        put_u32(&mut bytes, 12, self.block_size);
        put_u64(&mut bytes, 16, self.total_blocks);
        put_u64(&mut bytes, 24, self.features);
        bytes[32..48].copy_from_slice(&self.uuid);
        let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
        put_u32(&mut bytes, CHECKSUM_OFFSET, checksum);
        Ok(bytes)
    }

    /// Decode and validate a record without trusting its contents.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != SUPERBLOCK_SIZE {
            return Err(Error::InvalidInput("superblock must be exactly 4096 bytes"));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad superblock magic"));
        }
        let version = get_u16(bytes, 8);
        if version != FORMAT_VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if get_u16(bytes, 10) != HEADER_SIZE {
            return Err(Error::Corrupt("unexpected superblock header size"));
        }
        let stored_checksum = get_u32(bytes, CHECKSUM_OFFSET);
        if stored_checksum != crc32c(&bytes[..CHECKSUM_OFFSET]) {
            return Err(Error::Corrupt("superblock CRC32C mismatch"));
        }
        if bytes[48..CHECKSUM_OFFSET].iter().any(|byte| *byte != 0)
            || bytes[4092..].iter().any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("reserved superblock bytes must be zero"));
        }
        let block_size = get_u32(bytes, 12);
        let total_blocks = get_u64(bytes, 16);
        validate_fields(block_size, total_blocks)?;
        let mut uuid = [0_u8; 16];
        uuid.copy_from_slice(&bytes[32..48]);
        Ok(Self {
            block_size,
            total_blocks,
            features: get_u64(bytes, 24),
            uuid,
        })
    }
}

fn validate_fields(block_size: u32, total_blocks: u64) -> Result<()> {
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
        return Err(Error::InvalidInput(
            "filesystem block size must be a power of two from 4096 through 65536",
        ));
    }
    if total_blocks < 2 {
        return Err(Error::InvalidInput(
            "volume must contain at least two blocks",
        ));
    }
    u64::from(block_size)
        .checked_mul(total_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    Ok(())
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}
fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
fn get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}
fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}
fn get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
        bytes[offset + 4],
        bytes[offset + 5],
        bytes[offset + 6],
        bytes[offset + 7],
    ])
}

/// Reflected Castagnoli CRC-32C, implemented without a third-party dependency.
pub(crate) fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0x82F6_3B78 & mask);
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::{Superblock, FORMAT_VERSION, SUPERBLOCK_SIZE};
    use openfs_core::Error;

    fn sample() -> Superblock {
        Superblock {
            block_size: 4096,
            total_blocks: 1024,
            features: 3,
            uuid: *b"openfs-test-uuid",
        }
    }

    #[test]
    fn explicit_encoding_round_trips() {
        let expected = sample();
        let bytes = expected.encode().unwrap();
        assert_eq!(&bytes[0..8], b"OPENFS\0\0");
        assert_eq!(&bytes[8..10], &[1, 0]);
        assert_eq!(&bytes[10..12], &[48, 0]);
        assert_eq!(&bytes[12..16], &[0, 16, 0, 0]);
        assert_eq!(&bytes[16..24], &1024_u64.to_le_bytes());
        assert_eq!(&bytes[24..32], &3_u64.to_le_bytes());
        assert_eq!(&bytes[32..48], b"openfs-test-uuid");
        assert!(bytes[48..4088].iter().all(|byte| *byte == 0));
        assert!(bytes[4092..].iter().all(|byte| *byte == 0));
        assert_eq!(Superblock::decode(&bytes).unwrap(), expected);
    }

    #[test]
    fn rejects_wrong_size_bad_magic_and_checksum_corruption() {
        assert!(matches!(
            Superblock::decode(&[0; 16]),
            Err(Error::InvalidInput(_))
        ));
        let mut bytes = sample().encode().unwrap();
        bytes[0] ^= 1;
        assert!(matches!(Superblock::decode(&bytes), Err(Error::Corrupt(_))));
        let mut bytes = sample().encode().unwrap();
        bytes[80] = 1;
        assert!(matches!(Superblock::decode(&bytes), Err(Error::Corrupt(_))));
    }

    #[test]
    fn rejects_unknown_version_and_nonzero_reserved_bytes() {
        let mut bytes = sample().encode().unwrap();
        bytes[8..10].copy_from_slice(&(FORMAT_VERSION + 1).to_le_bytes());
        let checksum = super::crc32c(&bytes[..4088]);
        bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            Superblock::decode(&bytes),
            Err(Error::UnsupportedVersion(_))
        ));
        let mut bytes = sample().encode().unwrap();
        bytes[10..12].copy_from_slice(&49_u16.to_le_bytes());
        let checksum = super::crc32c(&bytes[..4088]);
        bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(Superblock::decode(&bytes), Err(Error::Corrupt(_))));

        let mut bytes = sample().encode().unwrap();
        bytes[100] = 9;
        let checksum = super::crc32c(&bytes[..4088]);
        bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(Superblock::decode(&bytes), Err(Error::Corrupt(_))));
        let mut bytes = sample().encode().unwrap();
        bytes[4095] = 1;
        let checksum = super::crc32c(&bytes[..4088]);
        bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(Superblock::decode(&bytes), Err(Error::Corrupt(_))));
    }

    #[test]
    fn crc32c_matches_the_standard_check_value() {
        assert_eq!(super::crc32c(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn rejects_volume_size_overflow() {
        let mut value = sample();
        value.total_blocks = u64::MAX;
        assert!(matches!(value.encode(), Err(Error::ArithmeticOverflow)));
    }

    #[test]
    fn decoder_rejects_invalid_fields_even_with_a_valid_checksum() {
        fn refresh_checksum(bytes: &mut [u8; SUPERBLOCK_SIZE]) {
            let checksum = super::crc32c(&bytes[..4088]);
            bytes[4088..4092].copy_from_slice(&checksum.to_le_bytes());
        }

        let mut invalid_block_size = sample().encode().unwrap();
        invalid_block_size[12..16].copy_from_slice(&2048_u32.to_le_bytes());
        refresh_checksum(&mut invalid_block_size);
        assert!(matches!(
            Superblock::decode(&invalid_block_size),
            Err(Error::InvalidInput(_))
        ));

        let mut too_small = sample().encode().unwrap();
        too_small[16..24].copy_from_slice(&1_u64.to_le_bytes());
        refresh_checksum(&mut too_small);
        assert!(matches!(
            Superblock::decode(&too_small),
            Err(Error::InvalidInput(_))
        ));

        let mut overflowing = sample().encode().unwrap();
        overflowing[16..24].copy_from_slice(&u64::MAX.to_le_bytes());
        refresh_checksum(&mut overflowing);
        assert!(matches!(
            Superblock::decode(&overflowing),
            Err(Error::ArithmeticOverflow)
        ));
    }

    #[test]
    fn detects_corruption_in_every_defined_field() {
        for offset in [8_usize, 10, 12, 16, 24, 32] {
            let mut bytes = sample().encode().unwrap();
            bytes[offset] ^= 0x40;
            assert!(
                matches!(
                    Superblock::decode(&bytes),
                    Err(Error::Corrupt(_))
                        | Err(Error::UnsupportedVersion(_))
                        | Err(Error::InvalidInput(_))
                        | Err(Error::ArithmeticOverflow)
                ),
                "mutation at byte {offset} was not rejected"
            );
        }
    }

    #[test]
    fn rejects_invalid_parameters_and_trailing_bytes() {
        let mut value = sample();
        value.block_size = 1000;
        assert!(matches!(value.encode(), Err(Error::InvalidInput(_))));
        let mut value = sample();
        value.total_blocks = 1;
        assert!(matches!(value.encode(), Err(Error::InvalidInput(_))));
        let bytes = sample().encode().unwrap();
        let mut extra = bytes.to_vec();
        extra.push(0);
        assert_eq!(extra.len(), SUPERBLOCK_SIZE + 1);
        assert!(matches!(
            Superblock::decode(&extra),
            Err(Error::InvalidInput(_))
        ));
    }
}

mod allocator_extents;
mod checkpoint;
mod directory_entry;
mod extent_map;
mod journal;
mod journal_engine;
mod metadata;
mod namespace;
mod volume;
mod volume_v2;
pub use checkpoint::{JournalCheckpoint, JOURNAL_CHECKPOINT_SIZE, JOURNAL_CHECKPOINT_VERSION};
pub use directory_entry::{DirectoryEntry, DIRECTORY_ENTRY_SIZE, DIRECTORY_NAME_MAX_BYTES};
pub use extent_map::{Extent, ExtentMap};
pub use journal::{
    plan_recovery, CommittedTransaction, JournalRecord, JournalRecordKind, JOURNAL_HEADER_SIZE,
    JOURNAL_MAX_PAYLOAD_SIZE, JOURNAL_VERSION,
};
pub use journal_engine::{
    format_journal, open_journal, JournalRegion, PersistentJournal, RedoUpdate,
};
pub use metadata::{MetadataRecord, ObjectId, ObjectKind, METADATA_RECORD_SIZE};
pub use namespace::Namespace;
pub use volume::{format_volume, open_volume, Volume, VolumeLayout, BITMAP_START_BLOCK};
pub use volume_v2::{
    check_volume_v2, format_volume_v2, open_volume_v2, OpenedVolumeV2, VolumeCheckReport,
    VolumeLayoutV2, VolumeV2,
};
