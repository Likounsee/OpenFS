//! Validated extent maps for sparse-file block addressing.
//!
//! This module defines a checksummed, block-sized extent-map page and logical-to-physical
//! lookup. It deliberately does not allocate blocks or perform file I/O.

use crate::crc32c;
use openfs_core::{Error, Result};

const MAGIC: &[u8; 8] = b"OFSEXT\0\0";
const VERSION: u16 = 1;
const HEADER_SIZE: usize = 16;
const EXTENT_SIZE: usize = 24;
const CHECKSUM_SIZE: usize = 4;

/// One contiguous run mapping logical file blocks to physical volume blocks.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Extent {
    pub logical_start: u64,
    pub physical_start: u64,
    pub block_count: u64,
}

impl Extent {
    fn logical_end(self) -> Result<u64> {
        self.logical_start
            .checked_add(self.block_count)
            .ok_or(Error::ArithmeticOverflow)
    }

    fn physical_end(self) -> Result<u64> {
        self.physical_start
            .checked_add(self.block_count)
            .ok_or(Error::ArithmeticOverflow)
    }
}

/// Ordered extent map for one file. Gaps in logical space represent sparse holes.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ExtentMap {
    extents: Vec<Extent>,
}

impl ExtentMap {
    pub fn new(extents: Vec<Extent>) -> Result<Self> {
        let map = Self { extents };
        map.validate_ordering()?;
        Ok(map)
    }

    pub fn extents(&self) -> &[Extent] {
        &self.extents
    }

    pub fn is_empty(&self) -> bool {
        self.extents.is_empty()
    }

    /// Number of logical blocks explicitly backed by physical storage.
    pub fn allocated_blocks(&self) -> Result<u64> {
        self.extents.iter().try_fold(0_u64, |total, extent| {
            total
                .checked_add(extent.block_count)
                .ok_or(Error::ArithmeticOverflow)
        })
    }

    /// Map a logical block to its physical block, returning None for a sparse hole or EOF.
    pub fn physical_block(&self, logical_block: u64) -> Option<u64> {
        let index = self
            .extents
            .partition_point(|extent| extent.logical_start <= logical_block);
        let extent = self.extents.get(index.checked_sub(1)?)?;
        let offset = logical_block.checked_sub(extent.logical_start)?;
        if offset >= extent.block_count {
            return None;
        }
        extent.physical_start.checked_add(offset)
    }

    /// Append a run, merging it with the previous run when both address spaces are adjacent.
    pub fn push_coalescing(&mut self, extent: Extent) -> Result<()> {
        validate_extent(extent)?;
        if let Some(last) = self.extents.last_mut() {
            if last.logical_end()? == extent.logical_start
                && last.physical_end()? == extent.physical_start
            {
                last.block_count = last
                    .block_count
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)?;
                return Ok(());
            }
            if extent.logical_start < last.logical_end()? {
                return Err(Error::InvalidInput("extent overlaps prior logical blocks"));
            }
        }
        self.extents.push(extent);
        self.validate_ordering()
    }

    /// Validate logical ordering and ensure physical extents are inside the data region.
    /// data_end is exclusive. Physical extents must not alias one another.
    pub fn validate(&self, logical_block_count: u64, data_start: u64, data_end: u64) -> Result<()> {
        if data_start >= data_end {
            return Err(Error::InvalidInput("data region is empty or inverted"));
        }
        self.validate_ordering()?;
        for extent in &self.extents {
            if extent.logical_end()? > logical_block_count {
                return Err(Error::Corrupt("extent exceeds logical file length"));
            }
            let physical_end = extent.physical_end()?;
            if extent.physical_start < data_start || physical_end > data_end {
                return Err(Error::Corrupt("extent lies outside the data region"));
            }
        }
        let mut physical: Vec<(u64, u64)> = self
            .extents
            .iter()
            .map(|extent| Ok((extent.physical_start, extent.physical_end()?)))
            .collect::<Result<_>>()?;
        physical.sort_unstable();
        if physical.windows(2).any(|pair| pair[0].1 > pair[1].0) {
            return Err(Error::Corrupt("physical extents overlap"));
        }
        Ok(())
    }

    /// Encode this map into one complete filesystem block with zeroed padding and CRC32C.
    pub fn encode_block(&self, block_size: usize) -> Result<Vec<u8>> {
        self.validate_ordering()?;
        if block_size < HEADER_SIZE + CHECKSUM_SIZE
            || (block_size - HEADER_SIZE - CHECKSUM_SIZE) / EXTENT_SIZE < self.extents.len()
        {
            return Err(Error::InvalidInput("extent map does not fit in one block"));
        }
        let mut bytes = vec![0_u8; block_size];
        bytes[0..8].copy_from_slice(MAGIC);
        bytes[8..10].copy_from_slice(&VERSION.to_le_bytes());
        bytes[10..12].copy_from_slice(&(HEADER_SIZE as u16).to_le_bytes());
        let count = u32::try_from(self.extents.len()).map_err(|_| Error::ArithmeticOverflow)?;
        bytes[12..16].copy_from_slice(&count.to_le_bytes());
        for (index, extent) in self.extents.iter().enumerate() {
            let offset = HEADER_SIZE + index * EXTENT_SIZE;
            bytes[offset..offset + 8].copy_from_slice(&extent.logical_start.to_le_bytes());
            bytes[offset + 8..offset + 16].copy_from_slice(&extent.physical_start.to_le_bytes());
            bytes[offset + 16..offset + 24].copy_from_slice(&extent.block_count.to_le_bytes());
        }
        let checksum_offset = block_size - CHECKSUM_SIZE;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());
        Ok(bytes)
    }

    /// Decode a complete block and reject checksum errors, invalid extents, and nonzero padding.
    pub fn decode_block(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < HEADER_SIZE + CHECKSUM_SIZE {
            return Err(Error::InvalidInput("extent-map block is too short"));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad extent-map magic"));
        }
        let version = u16::from_le_bytes([bytes[8], bytes[9]]);
        if version != VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if u16::from_le_bytes([bytes[10], bytes[11]]) as usize != HEADER_SIZE {
            return Err(Error::Corrupt("unexpected extent-map header size"));
        }
        let checksum_offset = bytes.len() - CHECKSUM_SIZE;
        let stored = u32::from_le_bytes(
            bytes[checksum_offset..]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid extent-map checksum"))?,
        );
        if stored != crc32c(&bytes[..checksum_offset]) {
            return Err(Error::Corrupt("extent-map CRC32C mismatch"));
        }
        let count = u32::from_le_bytes(
            bytes[12..16]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid extent count"))?,
        ) as usize;
        let capacity = (checksum_offset - HEADER_SIZE) / EXTENT_SIZE;
        if count > capacity {
            return Err(Error::Corrupt("extent count exceeds block capacity"));
        }
        let used_end = HEADER_SIZE + count * EXTENT_SIZE;
        if bytes[used_end..checksum_offset]
            .iter()
            .any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("extent-map padding must be zero"));
        }
        let mut extents = Vec::with_capacity(count);
        for index in 0..count {
            let offset = HEADER_SIZE + index * EXTENT_SIZE;
            extents.push(Extent {
                logical_start: read_u64(bytes, offset)?,
                physical_start: read_u64(bytes, offset + 8)?,
                block_count: read_u64(bytes, offset + 16)?,
            });
        }
        Self::new(extents).map_err(|error| match error {
            Error::InvalidInput(message) => Error::Corrupt(message),
            other => other,
        })
    }

    fn validate_ordering(&self) -> Result<()> {
        let mut previous_end = 0_u64;
        for (index, extent) in self.extents.iter().copied().enumerate() {
            validate_extent(extent)?;
            if index > 0 && extent.logical_start < previous_end {
                return Err(Error::InvalidInput(
                    "logical extents overlap or are unordered",
                ));
            }
            previous_end = extent.logical_end()?;
        }
        Ok(())
    }
}

fn validate_extent(extent: Extent) -> Result<()> {
    if extent.block_count == 0 {
        return Err(Error::InvalidInput("extent block count must be nonzero"));
    }
    extent.logical_end()?;
    extent.physical_end()?;
    Ok(())
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .map_err(|_| Error::Corrupt("truncated extent record"))?,
    ))
}

#[cfg(test)]
mod tests {
    use super::{Extent, ExtentMap};
    use openfs_core::Error;

    fn sample() -> ExtentMap {
        ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 3,
            },
            Extent {
                logical_start: 5,
                physical_start: 200,
                block_count: 2,
            },
        ])
        .unwrap()
    }

    #[test]
    fn extent_map_round_trips_and_maps_sparse_blocks() {
        let map = sample();
        let bytes = map.encode_block(4096).unwrap();
        assert_eq!(ExtentMap::decode_block(&bytes).unwrap(), map);
        assert_eq!(map.physical_block(0), Some(100));
        assert_eq!(map.physical_block(2), Some(102));
        assert_eq!(map.physical_block(3), None);
        assert_eq!(map.physical_block(5), Some(200));
        assert_eq!(map.physical_block(7), None);
        assert_eq!(map.allocated_blocks().unwrap(), 5);
    }

    #[test]
    fn coalesces_adjacent_runs_and_rejects_logical_overlap() {
        let mut map = ExtentMap::default();
        map.push_coalescing(Extent {
            logical_start: 0,
            physical_start: 50,
            block_count: 2,
        })
        .unwrap();
        map.push_coalescing(Extent {
            logical_start: 2,
            physical_start: 52,
            block_count: 3,
        })
        .unwrap();
        assert_eq!(map.extents().len(), 1);
        assert_eq!(map.extents()[0].block_count, 5);
        assert!(matches!(
            map.push_coalescing(Extent {
                logical_start: 4,
                physical_start: 80,
                block_count: 1
            }),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn rejects_bad_crc_padding_order_and_out_of_region_extents() {
        let mut bytes = sample().encode_block(4096).unwrap();
        bytes[200] = 1;
        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode_block(4096).unwrap();
        bytes[100] = 1;
        let checksum_offset = bytes.len() - 4;
        let checksum = crate::crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::Corrupt(_))
        ));

        let overlap = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 4,
            },
            Extent {
                logical_start: 4,
                physical_start: 102,
                block_count: 2,
            },
        ])
        .unwrap();
        assert!(matches!(
            overlap.validate(8, 10, 300),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            sample().validate(6, 100, 201),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn rejects_overflow_zero_length_and_capacity_errors() {
        assert!(matches!(
            ExtentMap::new(vec![Extent {
                logical_start: u64::MAX,
                physical_start: 1,
                block_count: 1
            }]),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(matches!(
            ExtentMap::new(vec![Extent {
                logical_start: 0,
                physical_start: 1,
                block_count: 0
            }]),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            sample().encode_block(32),
            Err(Error::InvalidInput(_))
        ));
    }
}
