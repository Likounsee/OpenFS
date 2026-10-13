//! Versioned on-disk locator for persistent V2 namespace inventories.
//!
//! The descriptor is stored in one allocated data block. V2 superblocks remain unchanged;
//! discovery can scan allocated data blocks for this self-identifying, checksummed record.

use crate::{crc32c, ObjectId};
use openfs_core::{Error, Result};

pub const INVENTORY_DESCRIPTOR_MAGIC: &[u8; 8] = b"OFSMINV\\0";
const VERSION: u16 = 1;
const HEADER_SIZE: usize = 48;
const CHECKSUM_OFFSET: usize = 40;

/// Physical locations and record counts needed to reconstruct a persistent namespace.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PersistentInventoryDescriptor {
    pub root_id: ObjectId,
    pub metadata_blocks: Vec<u64>,
    pub metadata_record_count: usize,
    pub directory_blocks: Vec<u64>,
    pub directory_entry_count: usize,
}

impl PersistentInventoryDescriptor {
    /// Encode the descriptor into exactly one filesystem block.
    pub fn encode(&self, block_size: usize) -> Result<Vec<u8>> {
        validate_shape(self, block_size)?;
        ObjectId::new(self.root_id.0)?;
        let mut bytes = vec![0_u8; block_size];
        bytes[..8].copy_from_slice(INVENTORY_DESCRIPTOR_MAGIC);
        bytes[8..10].copy_from_slice(&VERSION.to_le_bytes());
        bytes[10..12].copy_from_slice(&(HEADER_SIZE as u16).to_le_bytes());
        bytes[12..28].copy_from_slice(&self.root_id.0.to_le_bytes());
        bytes[28..32].copy_from_slice(
            &u32::try_from(self.metadata_record_count)
                .map_err(|_| Error::InvalidInput("metadata record count exceeds descriptor limit"))?
                .to_le_bytes(),
        );
        bytes[32..36].copy_from_slice(
            &u32::try_from(self.directory_entry_count)
                .map_err(|_| Error::InvalidInput("directory entry count exceeds descriptor limit"))?
                .to_le_bytes(),
        );
        let mut cursor = HEADER_SIZE;
        for block in self.metadata_blocks.iter().chain(self.directory_blocks.iter()) {
            bytes[cursor..cursor + 8].copy_from_slice(&block.to_le_bytes());
            cursor += 8;
        }
        let checksum = crc32c(&bytes);
        bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].copy_from_slice(&checksum.to_le_bytes());
        Ok(bytes)
    }

    /// Decode one block, rejecting malformed lengths, unsupported versions, bad checksums,
    /// nonzero reserved/padding bytes, and duplicate or overlapping physical block lists.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < HEADER_SIZE || bytes.len() > 65_536 || !bytes.len().is_power_of_two() {
            return Err(Error::InvalidInput("inventory descriptor has an invalid block size"));
        }
        if &bytes[..8] != INVENTORY_DESCRIPTOR_MAGIC {
            return Err(Error::Corrupt("bad inventory descriptor magic"));
        }
        let version = u16::from_le_bytes([bytes[8], bytes[9]]);
        if version != VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if u16::from_le_bytes([bytes[10], bytes[11]]) as usize != HEADER_SIZE {
            return Err(Error::Corrupt("unexpected inventory descriptor header size"));
        }
        if bytes[36..40].iter().any(|byte| *byte != 0)
            || bytes[44..48].iter().any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("inventory descriptor reserved bytes must be zero"));
        }
        let stored = u32::from_le_bytes(bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].try_into().unwrap());
        let mut checksum_input = bytes.to_vec();
        checksum_input[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].fill(0);
        if stored != crc32c(&checksum_input) {
            return Err(Error::Corrupt("inventory descriptor CRC32C mismatch"));
        }
        let root_id = ObjectId::new(u128::from_le_bytes(bytes[12..28].try_into().unwrap()))
            .map_err(|_| Error::Corrupt("inventory descriptor root ID is zero"))?;
        let metadata_record_count = u32::from_le_bytes(bytes[28..32].try_into().unwrap()) as usize;
        let directory_entry_count = u32::from_le_bytes(bytes[32..36].try_into().unwrap()) as usize;
        let metadata_blocks_len = bytes_needed(metadata_record_count, crate::METADATA_RECORD_SIZE, bytes.len())?;
        let directory_blocks_len = bytes_needed(directory_entry_count, crate::DIRECTORY_ENTRY_SIZE, bytes.len())?;
        let required_blocks = metadata_blocks_len
            .checked_add(directory_blocks_len)
            .ok_or(Error::ArithmeticOverflow)?;
        let used_end = HEADER_SIZE
            .checked_add(required_blocks.checked_mul(8).ok_or(Error::ArithmeticOverflow)?)
            .ok_or(Error::ArithmeticOverflow)?;
        if used_end > bytes.len() {
            return Err(Error::Corrupt("inventory descriptor block list exceeds descriptor size"));
        }
        if bytes[used_end..].iter().any(|byte| *byte != 0) {
            return Err(Error::Corrupt("inventory descriptor padding must be zero"));
        }
        let mut cursor = HEADER_SIZE;
        let mut metadata_blocks = Vec::with_capacity(metadata_blocks_len);
        for _ in 0..metadata_blocks_len {
            metadata_blocks.push(u64::from_le_bytes(bytes[cursor..cursor + 8].try_into().unwrap()));
            cursor += 8;
        }
        let mut directory_blocks = Vec::with_capacity(directory_blocks_len);
        for _ in 0..directory_blocks_len {
            directory_blocks.push(u64::from_le_bytes(bytes[cursor..cursor + 8].try_into().unwrap()));
            cursor += 8;
        }
        let descriptor = Self {
            root_id,
            metadata_blocks,
            metadata_record_count,
            directory_blocks,
            directory_entry_count,
        };
        validate_shape(&descriptor, bytes.len()).map_err(|_| Error::Corrupt("invalid inventory descriptor shape"))?;
        Ok(descriptor)
    }
}

fn bytes_needed(record_count: usize, record_size: usize, block_size: usize) -> Result<usize> {
    let byte_count = record_count.checked_mul(record_size).ok_or(Error::ArithmeticOverflow)?;
    if byte_count == 0 {
        return Ok(0);
    }
    Ok(byte_count
        .checked_add(block_size - 1)
        .ok_or(Error::ArithmeticOverflow)?
        / block_size)
}

fn validate_shape(descriptor: &PersistentInventoryDescriptor, block_size: usize) -> Result<()> {
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
        return Err(Error::InvalidInput("unsupported inventory descriptor block size"));
    }
    let metadata_needed = bytes_needed(
        descriptor.metadata_record_count,
        crate::METADATA_RECORD_SIZE,
        block_size,
    )?;
    let directory_needed = bytes_needed(
        descriptor.directory_entry_count,
        crate::DIRECTORY_ENTRY_SIZE,
        block_size,
    )?;
    if descriptor.metadata_blocks.len() != metadata_needed
        || descriptor.directory_blocks.len() != directory_needed
    {
        return Err(Error::InvalidInput("inventory descriptor block counts do not match record counts"));
    }
    let total_blocks = descriptor.metadata_blocks.len()
        .checked_add(descriptor.directory_blocks.len())
        .ok_or(Error::ArithmeticOverflow)?;
    let used_end = HEADER_SIZE
        .checked_add(total_blocks.checked_mul(8).ok_or(Error::ArithmeticOverflow)?)
        .ok_or(Error::ArithmeticOverflow)?;
    if used_end > block_size {
        return Err(Error::InvalidInput("inventory block lists do not fit in one descriptor block"));
    }
    let mut blocks = std::collections::BTreeSet::new();
    if descriptor.metadata_blocks.iter().chain(descriptor.directory_blocks.iter()).any(|block| !blocks.insert(*block)) {
        return Err(Error::InvalidInput("inventory descriptor contains duplicate physical blocks"));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::PersistentInventoryDescriptor;
    use crate::ObjectId;
    use openfs_core::Error;

    fn sample() -> PersistentInventoryDescriptor {
        PersistentInventoryDescriptor {
            root_id: ObjectId(1),
            metadata_blocks: vec![20],
            metadata_record_count: 40,
            directory_blocks: vec![21, 22],
            directory_entry_count: 40,
        }
    }

    #[test]
    fn descriptor_round_trips_and_rejects_checksum_and_padding_corruption() {
        let expected = sample();
        let bytes = expected.encode(4096).unwrap();
        assert_eq!(PersistentInventoryDescriptor::decode(&bytes).unwrap(), expected);
        let mut corrupt = bytes.clone();
        corrupt[48] ^= 1;
        assert!(matches!(PersistentInventoryDescriptor::decode(&corrupt), Err(Error::Corrupt(_))));
        let mut corrupt = bytes;
        corrupt[4000] = 1;
        let checksum = crate::crc32c(&{
            let mut input = corrupt.clone();
            input[40..44].fill(0);
            input
        });
        corrupt[40..44].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(PersistentInventoryDescriptor::decode(&corrupt), Err(Error::Corrupt(_))));
    }

    #[test]
    fn rejects_overlapping_or_mis_sized_inventory_lists() {
        let mut value = sample();
        value.directory_blocks[0] = value.metadata_blocks[0];
        assert!(matches!(value.encode(4096), Err(Error::InvalidInput(_))));
        let mut value = sample();
        value.metadata_blocks.clear();
        assert!(matches!(value.encode(4096), Err(Error::InvalidInput(_))));
    }
}
