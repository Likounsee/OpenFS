//! Checksummed on-disk locator for persistent namespace inventories.
//!
//! The descriptor is stored in one allocated data block. The superblock remains unchanged;
//! discovery can scan allocated data blocks for this self-identifying, checksummed record.

use crate::{crc32c, ObjectId};
use openfs_block::BlockDevice;
use openfs_core::{check_block_range, Error, Result};

fn try_zeroed_vec(length: usize, allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(length)
        .map_err(|_| Error::AllocationFailed(allocation_error))?;
    bytes.resize(length, 0);
    Ok(bytes)
}

fn try_copy_bytes(input: &[u8], allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(input.len())
        .map_err(|_| Error::AllocationFailed(allocation_error))?;
    bytes.extend_from_slice(input);
    Ok(bytes)
}

fn validate_descriptor_io<D: BlockDevice>(
    device: &D,
    start_block: u64,
    end_block: u64,
    block_size: u32,
) -> Result<usize> {
    if !(4096..=65_536).contains(&block_size) || !block_size.is_power_of_two() {
        return Err(Error::InvalidInput(
            "unsupported inventory descriptor block size",
        ));
    }
    if device.block_size() != block_size {
        return Err(Error::InvalidInput(
            "descriptor block size disagrees with device",
        ));
    }
    if start_block >= end_block {
        return Err(Error::InvalidInput(
            "descriptor guard range must be non-empty",
        ));
    }
    check_block_range(start_block, end_block - start_block, device.block_count())?;
    usize::try_from(block_size).map_err(|_| Error::ArithmeticOverflow)
}

/// Refuse to release the block that currently stores the persistent inventory locator.
///
/// Descriptor replacement/deletion has no transactional protocol yet. Allowing generic
/// allocator APIs to free this block would make the persistent namespace undiscoverable.
pub(crate) fn reject_freeing_inventory_descriptor<D: BlockDevice>(
    device: &mut D,
    block: u64,
    block_size: u32,
) -> Result<()> {
    let block_size_usize = validate_descriptor_io(
        device,
        block,
        block.checked_add(1).ok_or(Error::ArithmeticOverflow)?,
        block_size,
    )?;
    let mut contents = try_zeroed_vec(
        block_size_usize,
        "unable to allocate descriptor guard buffer",
    )?;
    device.read_blocks(block, &mut contents)?;
    if contents.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
        return Err(Error::InvalidInput(
            "cannot free a persistent inventory descriptor block",
        ));
    }
    Ok(())
}

/// Refuse to release a contiguous range containing a persistent inventory descriptor.
///
/// Read in bounded batches instead of issuing one device read per block. A maximum of
/// 64 blocks keeps temporary memory bounded to 4 MiB at the largest supported block size.
pub(crate) fn reject_freeing_inventory_descriptor_range<D: BlockDevice>(
    device: &mut D,
    start_block: u64,
    end_block: u64,
    block_size: u32,
) -> Result<()> {
    const BATCH_BLOCKS: u64 = 64;
    let block_size_usize = validate_descriptor_io(device, start_block, end_block, block_size)?;
    let mut next_block = start_block;
    while next_block < end_block {
        let count = (end_block - next_block).min(BATCH_BLOCKS);
        let byte_count = usize::try_from(count)
            .map_err(|_| Error::ArithmeticOverflow)?
            .checked_mul(block_size_usize)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut contents =
            try_zeroed_vec(byte_count, "unable to allocate descriptor range buffer")?;
        device.read_blocks(next_block, &mut contents)?;
        for chunk in contents.chunks_exact(block_size_usize) {
            if chunk.starts_with(INVENTORY_DESCRIPTOR_MAGIC) {
                return Err(Error::InvalidInput(
                    "cannot free a persistent inventory descriptor block",
                ));
            }
        }
        next_block = next_block
            .checked_add(count)
            .ok_or(Error::ArithmeticOverflow)?;
    }
    Ok(())
}

pub const INVENTORY_DESCRIPTOR_MAGIC: &[u8; 8] = b"OFSMINV\0";
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
        let mut bytes = try_zeroed_vec(
            block_size,
            "unable to allocate encoded inventory descriptor",
        )?;
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
        for block in self
            .metadata_blocks
            .iter()
            .chain(self.directory_blocks.iter())
        {
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
        if !(4096..=65_536).contains(&bytes.len()) || !bytes.len().is_power_of_two() {
            return Err(Error::InvalidInput(
                "inventory descriptor has an invalid block size",
            ));
        }
        if &bytes[..8] != INVENTORY_DESCRIPTOR_MAGIC {
            return Err(Error::Corrupt("bad inventory descriptor magic"));
        }
        let version = u16::from_le_bytes([bytes[8], bytes[9]]);
        if version != VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if u16::from_le_bytes([bytes[10], bytes[11]]) as usize != HEADER_SIZE {
            return Err(Error::Corrupt(
                "unexpected inventory descriptor header size",
            ));
        }
        if bytes[36..40].iter().any(|byte| *byte != 0)
            || bytes[44..48].iter().any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt(
                "inventory descriptor reserved bytes must be zero",
            ));
        }
        let stored = u32::from_le_bytes(
            bytes[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4]
                .try_into()
                .unwrap(),
        );
        let mut checksum_input =
            try_copy_bytes(bytes, "unable to allocate descriptor checksum buffer")?;
        checksum_input[CHECKSUM_OFFSET..CHECKSUM_OFFSET + 4].fill(0);
        if stored != crc32c(&checksum_input) {
            return Err(Error::Corrupt("inventory descriptor CRC32C mismatch"));
        }
        let root_id = ObjectId::new(u128::from_le_bytes(bytes[12..28].try_into().unwrap()))
            .map_err(|_| Error::Corrupt("inventory descriptor root ID is zero"))?;
        let metadata_record_count = u32::from_le_bytes(bytes[28..32].try_into().unwrap()) as usize;
        let directory_entry_count = u32::from_le_bytes(bytes[32..36].try_into().unwrap()) as usize;
        let metadata_blocks_len = bytes_needed(
            metadata_record_count,
            crate::METADATA_RECORD_SIZE,
            bytes.len(),
        )
        .map_err(|_| Error::Corrupt("inventory descriptor metadata count overflows"))?;
        let directory_blocks_len = bytes_needed(
            directory_entry_count,
            crate::DIRECTORY_ENTRY_SIZE,
            bytes.len(),
        )
        .map_err(|_| Error::Corrupt("inventory descriptor directory count overflows"))?;
        let required_blocks = metadata_blocks_len
            .checked_add(directory_blocks_len)
            .ok_or(Error::Corrupt("inventory descriptor block count overflows"))?;
        let used_end = HEADER_SIZE
            .checked_add(
                required_blocks
                    .checked_mul(8)
                    .ok_or(Error::Corrupt("inventory descriptor block list overflows"))?,
            )
            .ok_or(Error::Corrupt("inventory descriptor block list overflows"))?;
        if used_end > bytes.len() {
            return Err(Error::Corrupt(
                "inventory descriptor block list exceeds descriptor size",
            ));
        }
        if bytes[used_end..].iter().any(|byte| *byte != 0) {
            return Err(Error::Corrupt("inventory descriptor padding must be zero"));
        }
        let mut cursor = HEADER_SIZE;
        let mut metadata_blocks = Vec::new();
        metadata_blocks
            .try_reserve_exact(metadata_blocks_len)
            .map_err(|_| {
                Error::AllocationFailed("unable to allocate descriptor metadata block list")
            })?;
        for _ in 0..metadata_blocks_len {
            metadata_blocks.push(u64::from_le_bytes(
                bytes[cursor..cursor + 8].try_into().unwrap(),
            ));
            cursor += 8;
        }
        let mut directory_blocks = Vec::new();
        directory_blocks
            .try_reserve_exact(directory_blocks_len)
            .map_err(|_| {
                Error::AllocationFailed("unable to allocate descriptor directory block list")
            })?;
        for _ in 0..directory_blocks_len {
            directory_blocks.push(u64::from_le_bytes(
                bytes[cursor..cursor + 8].try_into().unwrap(),
            ));
            cursor += 8;
        }
        let descriptor = Self {
            root_id,
            metadata_blocks,
            metadata_record_count,
            directory_blocks,
            directory_entry_count,
        };
        validate_shape(&descriptor, bytes.len())
            .map_err(|_| Error::Corrupt("invalid inventory descriptor shape"))?;
        Ok(descriptor)
    }
}

fn bytes_needed(record_count: usize, record_size: usize, block_size: usize) -> Result<usize> {
    let byte_count = record_count
        .checked_mul(record_size)
        .ok_or(Error::ArithmeticOverflow)?;
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
        return Err(Error::InvalidInput(
            "unsupported inventory descriptor block size",
        ));
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
        return Err(Error::InvalidInput(
            "inventory descriptor block counts do not match record counts",
        ));
    }
    let total_blocks = descriptor
        .metadata_blocks
        .len()
        .checked_add(descriptor.directory_blocks.len())
        .ok_or(Error::ArithmeticOverflow)?;
    let used_end = HEADER_SIZE
        .checked_add(
            total_blocks
                .checked_mul(8)
                .ok_or(Error::ArithmeticOverflow)?,
        )
        .ok_or(Error::ArithmeticOverflow)?;
    if used_end > block_size {
        return Err(Error::InvalidInput(
            "inventory block lists do not fit in one descriptor block",
        ));
    }
    let mut blocks = std::collections::BTreeSet::new();
    if descriptor
        .metadata_blocks
        .iter()
        .chain(descriptor.directory_blocks.iter())
        .any(|block| !blocks.insert(*block))
    {
        return Err(Error::InvalidInput(
            "inventory descriptor contains duplicate physical blocks",
        ));
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
        assert_eq!(
            PersistentInventoryDescriptor::decode(&bytes).unwrap(),
            expected
        );
        let mut corrupt = bytes.clone();
        corrupt[48] ^= 1;
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&corrupt),
            Err(Error::Corrupt(_))
        ));
        let mut corrupt = bytes;
        corrupt[4000] = 1;
        let checksum = crate::crc32c(&{
            let mut input = corrupt.clone();
            input[40..44].fill(0);
            input
        });
        corrupt[40..44].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&corrupt),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn descriptor_decoder_rejects_every_single_byte_mutation_of_golden_image() {
        let descriptor = PersistentInventoryDescriptor {
            root_id: ObjectId(1),
            metadata_blocks: vec![],
            metadata_record_count: 0,
            directory_blocks: vec![],
            directory_entry_count: 0,
        };
        let valid = descriptor.encode(4096).unwrap();

        // Exercise every byte, including magic, version, counts, checksum, and padding.
        // This is a deterministic corruption sweep, not a probabilistic fuzz test.
        for offset in 0..valid.len() {
            let mut corrupt = valid.clone();
            corrupt[offset] ^= 0x01;
            assert!(
                PersistentInventoryDescriptor::decode(&corrupt).is_err(),
                "single-byte corruption at offset {offset} was accepted"
            );
        }
    }

    #[test]
    fn descriptor_codec_round_trips_every_supported_block_size() {
        let expected = PersistentInventoryDescriptor {
            root_id: ObjectId(1),
            metadata_blocks: vec![20],
            metadata_record_count: 1,
            directory_blocks: vec![21],
            directory_entry_count: 1,
        };
        for block_size in [4096_usize, 8192, 16_384, 32_768, 65_536] {
            let encoded = expected.encode(block_size).unwrap();
            assert_eq!(encoded.len(), block_size);
            assert_eq!(
                PersistentInventoryDescriptor::decode(&encoded).unwrap(),
                expected,
                "block_size={block_size}"
            );
            assert_eq!(&encoded[..8], crate::INVENTORY_DESCRIPTOR_MAGIC);
            assert_eq!(u16::from_le_bytes([encoded[8], encoded[9]]), 1);
            assert_eq!(u16::from_le_bytes([encoded[10], encoded[11]]), 48);
        }
    }

    #[test]
    fn descriptor_codec_rejects_unsupported_block_sizes() {
        let descriptor = sample();
        for block_size in [64_usize, 128, 256, 512, 1024, 2048, 131_072] {
            assert!(
                matches!(
                    descriptor.encode(block_size),
                    Err(Error::InvalidInput(
                        "unsupported inventory descriptor block size"
                    ))
                ),
                "encoder accepted unsupported descriptor block size {block_size}"
            );

            let mut bytes = vec![0_u8; block_size];
            if block_size >= 48 && block_size.is_power_of_two() {
                bytes[..8].copy_from_slice(crate::INVENTORY_DESCRIPTOR_MAGIC);
            }
            assert!(
                matches!(
                    PersistentInventoryDescriptor::decode(&bytes),
                    Err(Error::InvalidInput(
                        "inventory descriptor has an invalid block size"
                    ))
                ),
                "unsupported descriptor block size {block_size} was accepted"
            );
        }
    }

    #[test]
    fn descriptor_decoder_fails_closed_for_header_and_shape_corruption() {
        fn refresh_checksum(bytes: &mut [u8]) {
            bytes[40..44].fill(0);
            let checksum = crate::crc32c(bytes);
            bytes[40..44].copy_from_slice(&checksum.to_le_bytes());
        }

        let valid = sample().encode(4096).unwrap();

        let mut unsupported = valid.clone();
        unsupported[8..10].copy_from_slice(&2_u16.to_le_bytes());
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&unsupported),
            Err(Error::UnsupportedVersion(2))
        ));

        let mut bad_header = valid.clone();
        bad_header[10..12].copy_from_slice(&47_u16.to_le_bytes());
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&bad_header),
            Err(Error::Corrupt(
                "unexpected inventory descriptor header size"
            ))
        ));

        for offset in [36_usize, 44] {
            let mut reserved = valid.clone();
            reserved[offset] = 1;
            refresh_checksum(&mut reserved);
            assert!(matches!(
                PersistentInventoryDescriptor::decode(&reserved),
                Err(Error::Corrupt(
                    "inventory descriptor reserved bytes must be zero"
                ))
            ));
        }

        let mut zero_root = valid.clone();
        zero_root[12..28].fill(0);
        refresh_checksum(&mut zero_root);
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&zero_root),
            Err(Error::Corrupt("inventory descriptor root ID is zero"))
        ));

        let mut duplicate = valid.clone();
        duplicate[56..64].copy_from_slice(&20_u64.to_le_bytes());
        refresh_checksum(&mut duplicate);
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&duplicate),
            Err(Error::Corrupt("invalid inventory descriptor shape"))
        ));

        let mut impossible_count = valid;
        impossible_count[28..32].copy_from_slice(&u32::MAX.to_le_bytes());
        refresh_checksum(&mut impossible_count);
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&impossible_count),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn rejects_overlapping_or_mis_sized_inventory_lists_at_every_block_size() {
        for block_size in [4096_usize, 8192, 16_384, 32_768, 65_536] {
            let valid = PersistentInventoryDescriptor {
                root_id: ObjectId(1),
                metadata_blocks: vec![20],
                metadata_record_count: 1,
                directory_blocks: vec![21],
                directory_entry_count: 1,
            };
            assert!(valid.encode(block_size).is_ok());

            let mut overlapping = valid.clone();
            overlapping.directory_blocks[0] = overlapping.metadata_blocks[0];
            assert!(
                matches!(overlapping.encode(block_size), Err(Error::InvalidInput(_))),
                "overlapping inventory lists were accepted at block size {block_size}"
            );

            let mut mis_sized = valid;
            mis_sized.metadata_blocks.clear();
            assert!(
                matches!(mis_sized.encode(block_size), Err(Error::InvalidInput(_))),
                "mis-sized inventory list was accepted at block size {block_size}"
            );
        }
    }

    #[test]
    fn descriptor_has_stable_golden_bytes_and_checksum() {
        let descriptor = PersistentInventoryDescriptor {
            root_id: ObjectId(1),
            metadata_blocks: vec![],
            metadata_record_count: 0,
            directory_blocks: vec![],
            directory_entry_count: 0,
        };
        let encoded = descriptor.encode(4096).unwrap();

        assert_eq!(
            &encoded[..48],
            &[
                79, 70, 83, 77, 73, 78, 86, 0, 1, 0, 48, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 37, 179, 212, 0, 0, 0, 0
            ]
        );
        assert_eq!(
            u32::from_le_bytes(encoded[40..44].try_into().unwrap()),
            0xd4b3_2501
        );
        assert!(encoded[48..].iter().all(|byte| *byte == 0));
        assert_eq!(
            PersistentInventoryDescriptor::decode(&encoded).unwrap(),
            descriptor
        );
    }
    #[test]
    fn descriptor_decoder_rejects_duplicate_blocks_and_zero_root_with_valid_checksum() {
        fn refresh_checksum(bytes: &mut [u8]) {
            bytes[40..44].fill(0);
            let checksum = crate::crc32c(bytes);
            bytes[40..44].copy_from_slice(&checksum.to_le_bytes());
        }

        let valid = sample().encode(4096).unwrap();

        // The first directory block starts after the one metadata block.
        let mut duplicate = valid.clone();
        duplicate[56..64].copy_from_slice(&20_u64.to_le_bytes());
        refresh_checksum(&mut duplicate);
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&duplicate),
            Err(Error::Corrupt("invalid inventory descriptor shape"))
        ));

        let mut zero_root = valid;
        zero_root[12..28].fill(0);
        refresh_checksum(&mut zero_root);
        assert!(matches!(
            PersistentInventoryDescriptor::decode(&zero_root),
            Err(Error::Corrupt("inventory descriptor root ID is zero"))
        ));
    }
}
