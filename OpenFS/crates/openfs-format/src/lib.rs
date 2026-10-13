//! Explicit byte-level encoding for the initial OpenFS superblock draft.

#[cfg(test)]
use openfs_core::{Error, Result};

pub use allocator_extents::{
    AllocationBitmapReport, AllocationOwnershipReport, AllocationStats, FreeExtent,
    FreeExtentStats, OWNERSHIP_DIAGNOSTIC_SAMPLE_LIMIT,
};

/// Size of the initial fixed superblock record.
pub const SUPERBLOCK_SIZE: usize = 4096;
/// Supported format version for this draft.
#[cfg(test)]
pub const FORMAT_VERSION: u16 = 1;
#[cfg(test)]
const MAGIC: &[u8; 8] = b"OPENFS\0\0";
#[cfg(test)]
const HEADER_SIZE: u16 = 48;
#[cfg(test)]
const CHECKSUM_OFFSET: usize = 4088;

/// Validated logical superblock fields. This Rust struct is not written directly to disk.
#[derive(Debug, Clone, PartialEq, Eq)]
#[cfg(test)]
pub struct Superblock {
    pub block_size: u32,
    pub total_blocks: u64,
    pub features: u64,
    pub uuid: [u8; 16],
}

#[cfg(test)]
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

#[cfg(test)]
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

#[cfg(test)]
fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}
#[cfg(test)]
fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
#[cfg(test)]
fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
#[cfg(test)]
fn get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}
#[cfg(test)]
fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}
#[cfg(test)]
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

mod allocator_extents;
mod checkpoint;
mod directory_entry;
mod extent_map;
mod inventory_descriptor;
mod journal;
mod journal_engine;
#[cfg(test)]
mod legacy_layout;
mod metadata;
mod namespace;
mod verifier;
mod volume;
pub use checkpoint::{JournalCheckpoint, JOURNAL_CHECKPOINT_SIZE, JOURNAL_CHECKPOINT_VERSION};
pub use directory_entry::{
    decode_directory_entries, DirectoryEntry, DIRECTORY_ENTRY_SIZE, DIRECTORY_NAME_MAX_BYTES,
};
pub use extent_map::{Extent, ExtentMap};
pub use inventory_descriptor::{PersistentInventoryDescriptor, INVENTORY_DESCRIPTOR_MAGIC};
pub use journal::{
    plan_recovery, CommittedTransaction, JournalRecord, JournalRecordKind, JOURNAL_HEADER_SIZE,
    JOURNAL_MAX_PAYLOAD_SIZE, JOURNAL_VERSION,
};
pub use journal_engine::{
    format_journal, open_journal, JournalRegion, PersistentJournal, RedoUpdate,
};
#[cfg(test)]
pub use legacy_layout::{format_volume, open_volume, Volume, VolumeLayout, BITMAP_START_BLOCK};
pub use metadata::{
    decode_metadata_records, MetadataRecord, ObjectId, ObjectKind, METADATA_RECORD_SIZE,
};
pub use namespace::Namespace;
pub use verifier::PersistentInventoryReport;
pub use volume::{
    check_volume_v2, format_volume_v2, open_volume_v2, OpenedVolumeV2, VolumeCheckReport,
    VolumeLayoutV2, VolumeV2,
};
#[cfg(not(test))]
pub use volume::{
    check_volume_v2 as check_volume, format_volume_v2 as format_volume,
    open_volume_v2 as open_volume, OpenedVolumeV2 as OpenedVolume,
    VolumeLayoutV2 as VolumeLayout, VolumeV2 as Volume,
};
