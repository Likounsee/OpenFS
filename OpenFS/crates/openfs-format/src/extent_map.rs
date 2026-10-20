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

    /// Number of bytes backed by physical storage, excluding sparse holes.
    ///
    /// The multiplication is checked so a malformed or extremely large map cannot wrap
    /// a byte count. A zero block size is rejected instead of silently reporting zero.
    pub fn allocated_byte_count(&self, block_size: u32) -> Result<u64> {
        if block_size == 0 {
            return Err(Error::InvalidInput("block size must be nonzero"));
        }
        self.allocated_blocks()?
            .checked_mul(u64::from(block_size))
            .ok_or(Error::ArithmeticOverflow)
    }

    /// Logical byte length implied by the final extent, including sparse gaps.
    ///
    /// This reports the extent map's covered length, not necessarily the file's recorded
    /// byte length (the final allocated block may be only partially used).
    pub fn logical_byte_capacity(&self, block_size: u32) -> Result<u64> {
        if block_size == 0 {
            return Err(Error::InvalidInput("block size must be nonzero"));
        }
        self.logical_block_count()?
            .checked_mul(u64::from(block_size))
            .ok_or(Error::ArithmeticOverflow)
    }

    /// Return the logical block count covered by the map, including trailing holes.
    ///
    /// An empty map covers zero blocks. Sparse gaps before the final extent count toward
    /// the logical length even though they consume no physical storage.
    pub fn logical_block_count(&self) -> Result<u64> {
        match self.extents.last().copied() {
            Some(extent) => extent.logical_end(),
            None => Ok(0),
        }
    }

    /// Validate that no allocated logical block lies beyond a file's byte length.
    ///
    /// A partially used final block is valid, and sparse holes are allowed anywhere,
    /// including at the end of a file. This validates the upper bound only; it does not
    /// require every byte in the file to be allocated.
    pub fn validate_file_size(&self, file_size: u64, block_size: u32) -> Result<()> {
        if block_size == 0 {
            return Err(Error::InvalidInput("block size must be nonzero"));
        }
        let block_size = u64::from(block_size);
        let required_blocks = file_size / block_size + u64::from(file_size % block_size != 0);
        for extent in &self.extents {
            if extent.logical_end()? > required_blocks {
                return Err(Error::Corrupt("extent exceeds file byte length"));
            }
        }
        if file_size == 0 && !self.extents.is_empty() {
            return Err(Error::Corrupt("empty file must not own data extents"));
        }
        Ok(())
    }

    /// Ensure every physical run is inside the volume's allocatable data region.
    ///
    /// The data start is inclusive and total_blocks is exclusive. This prevents extents
    /// from aliasing superblocks, journal/checkpoint blocks, or addresses past EOF.
    pub fn validate_physical_bounds(&self, data_start_block: u64, total_blocks: u64) -> Result<()> {
        if data_start_block >= total_blocks {
            return Err(Error::InvalidInput("data region bounds are invalid"));
        }
        self.validate_ordering()?;
        for extent in &self.extents {
            let end = extent.physical_end()?;
            if extent.physical_start < data_start_block || end > total_blocks {
                return Err(Error::Corrupt("extent points outside the data region"));
            }
        }
        Ok(())
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
    /// Failed insertions leave the original map unchanged.
    pub fn push_coalescing(&mut self, extent: Extent) -> Result<()> {
        validate_extent(extent)?;
        if let Some(last) = self.extents.last_mut() {
            let logical_end = last.logical_end()?;
            let physical_end = last.physical_end()?;
            if logical_end == extent.logical_start && physical_end == extent.physical_start {
                // Appending is already ordered, so merge in place instead of cloning
                // and revalidating the entire extent vector on every appended run.
                last.block_count = last
                    .block_count
                    .checked_add(extent.block_count)
                    .ok_or(Error::ArithmeticOverflow)?;
                return Ok(());
            }
            if extent.logical_start < logical_end {
                return Err(Error::InvalidInput("extent overlaps prior logical blocks"));
            }
        }
        self.extents.push(extent);
        Ok(())
    }

    /// Insert an extent at any logical position, merging adjacent logical and physical runs.
    ///
    /// This supports rebuilding a file's map when newly allocated blocks are not physically
    /// adjacent to its existing data. Overlapping logical ranges are rejected, and the map
    /// remains unchanged if validation or arithmetic fails.
    pub fn insert_coalescing(&mut self, extent: Extent) -> Result<()> {
        validate_extent(extent)?;
        let mut candidate = self.extents.clone();
        let position = candidate.partition_point(|item| item.logical_start < extent.logical_start);
        candidate.insert(position, extent);

        let mut merged: Vec<Extent> = Vec::with_capacity(candidate.len());
        for current in candidate {
            if let Some(previous) = merged.last_mut() {
                let previous_logical_end = previous.logical_end()?;
                let previous_physical_end = previous.physical_end()?;
                if current.logical_start < previous_logical_end {
                    return Err(Error::InvalidInput(
                        "extent overlaps existing logical blocks",
                    ));
                }
                if current.logical_start == previous_logical_end
                    && current.physical_start == previous_physical_end
                {
                    previous.block_count = previous
                        .block_count
                        .checked_add(current.block_count)
                        .ok_or(Error::ArithmeticOverflow)?;
                    continue;
                }
            }
            merged.push(current);
        }

        let validated = Self::new(merged)?;
        self.extents = validated.extents;
        Ok(())
    }

    /// Iterate every physical data block represented by this map in logical extent order.
    /// Sparse holes are omitted. The caller must still validate the map against its volume.
    pub fn physical_blocks(&self) -> impl Iterator<Item = u64> + '_ {
        self.extents
            .iter()
            .flat_map(|extent| extent.physical_start..extent.physical_start + extent.block_count)
    }

    /// Return a validated, ascending inventory of physical data blocks owned by this map.
    ///
    /// Unlike `physical_blocks`, this checks logical length, volume bounds, and physical
    /// overlap before producing the inventory. The result can be combined with inventories
    /// from other objects and passed to the allocator's ownership consistency validator.
    pub fn validated_physical_block_inventory(
        &self,
        logical_block_count: u64,
        data_start: u64,
        data_end: u64,
    ) -> Result<Vec<u64>> {
        self.validate(logical_block_count, data_start, data_end)?;
        let mut blocks: Vec<u64> = self.physical_blocks().collect();
        blocks.sort_unstable();
        Ok(blocks)
    }

    /// Return the validated number of physical blocks owned by this map.
    ///
    /// Unlike `allocated_blocks`, this rejects malformed logical bounds, physical bounds,
    /// and physical aliases before reporting a count suitable for ownership accounting.
    pub fn validated_physical_block_count(
        &self,
        logical_block_count: u64,
        data_start: u64,
        data_end: u64,
    ) -> Result<u64> {
        self.validate(logical_block_count, data_start, data_end)?;
        self.allocated_blocks()
    }

    /// Return validated physical ranges as sorted, coalesced (start, end-exclusive) pairs.
    ///
    /// Adjacent physical extents are merged, while gaps remain distinct. This preserves an
    /// extent-level representation instead of expanding every run into one entry per block,
    /// reducing the work needed by bitmap consistency scans of highly fragmented files.
    pub fn validated_physical_ranges(
        &self,
        logical_block_count: u64,
        data_start: u64,
        data_end: u64,
    ) -> Result<Vec<(u64, u64)>> {
        self.validate(logical_block_count, data_start, data_end)?;
        let mut ranges: Vec<(u64, u64)> = self
            .extents
            .iter()
            .map(|extent| Ok((extent.physical_start, extent.physical_end()?)))
            .collect::<Result<_>>()?;
        ranges.sort_unstable();

        let mut coalesced: Vec<(u64, u64)> = Vec::with_capacity(ranges.len());
        for (start, end) in ranges {
            if let Some(last) = coalesced.last_mut() {
                if last.1 == start {
                    last.1 = end;
                    continue;
                }
            }
            coalesced.push((start, end));
        }
        Ok(coalesced)
    }

    /// Remove a logical block range and return the physical runs that became unreferenced.
    ///
    /// The operation is atomic in memory: validation or arithmetic errors leave the map
    /// unchanged. The caller is responsible for journaling metadata changes and freeing
    /// returned physical runs in the same persistent transaction.
    pub fn remove_range(&mut self, logical_start: u64, block_count: u64) -> Result<Vec<Extent>> {
        if block_count == 0 {
            return Err(Error::InvalidInput("removed logical range must be nonzero"));
        }
        let logical_end = logical_start
            .checked_add(block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut kept = Vec::with_capacity(self.extents.len() + 1);
        let mut removed = Vec::new();
        for extent in &self.extents {
            let extent_end = extent.logical_end()?;
            let overlap_start = extent.logical_start.max(logical_start);
            let overlap_end = extent_end.min(logical_end);
            if overlap_start >= overlap_end {
                kept.push(*extent);
                continue;
            }
            if extent.logical_start < overlap_start {
                kept.push(Extent {
                    logical_start: extent.logical_start,
                    physical_start: extent.physical_start,
                    block_count: overlap_start - extent.logical_start,
                });
            }
            removed.push(Extent {
                logical_start: overlap_start,
                physical_start: extent
                    .physical_start
                    .checked_add(overlap_start - extent.logical_start)
                    .ok_or(Error::ArithmeticOverflow)?,
                block_count: overlap_end - overlap_start,
            });
            if overlap_end < extent_end {
                kept.push(Extent {
                    logical_start: overlap_end,
                    physical_start: extent
                        .physical_start
                        .checked_add(overlap_end - extent.logical_start)
                        .ok_or(Error::ArithmeticOverflow)?,
                    block_count: extent_end - overlap_end,
                });
            }
        }
        let validated = Self::new(kept)?;
        self.extents = validated.extents;
        Ok(removed)
    }

    /// Truncate the logical file to the given block count, returning blocks to release.
    /// Extents before the new end are preserved, including sparse holes.
    pub fn truncate(&mut self, logical_block_count: u64) -> Result<Vec<Extent>> {
        // Extents are validated in ascending, non-overlapping logical order, so
        // the final extent also has the greatest logical end. Avoid allocating a
        // temporary Vec and rechecking every extent just to find that boundary.
        let logical_end = match self.extents.last() {
            Some(extent) => extent.logical_end()?,
            None => 0,
        };
        if logical_block_count >= logical_end {
            return Ok(Vec::new());
        }
        self.remove_range(logical_block_count, logical_end - logical_block_count)
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
        // validate_ordering() already checks physical overlap. Keep this method
        // focused on volume-specific logical and physical bounds rather than
        // allocating and sorting a second copy of the same physical ranges.
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
        let mut physical = Vec::with_capacity(self.extents.len());
        for (index, extent) in self.extents.iter().copied().enumerate() {
            validate_extent(extent)?;
            if index > 0 && extent.logical_start < previous_end {
                return Err(Error::InvalidInput(
                    "logical extents overlap or are unordered",
                ));
            }
            previous_end = extent.logical_end()?;
            physical.push((extent.physical_start, extent.physical_end()?));
        }
        physical.sort_unstable();
        if physical.windows(2).any(|pair| pair[0].1 > pair[1].0) {
            return Err(Error::InvalidInput("physical extents overlap"));
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
    fn reports_allocated_bytes_separately_from_sparse_logical_capacity() {
        let map = sample();
        assert_eq!(map.allocated_byte_count(4096).unwrap(), 5 * 4096);
        assert_eq!(map.logical_byte_capacity(4096).unwrap(), 7 * 4096);
        assert!(matches!(
            map.allocated_byte_count(0),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            map.logical_byte_capacity(0),
            Err(Error::InvalidInput(_))
        ));

        let huge = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 1,
            block_count: u64::MAX,
        }]);
        assert!(matches!(huge, Err(Error::ArithmeticOverflow)));
    }

    #[test]
    fn validates_byte_length_against_sparse_and_partial_final_blocks() {
        let map = sample();
        assert!(map.validate_file_size(7 * 4096 - 1, 4096).is_ok());
        assert!(map.validate_file_size(7 * 4096, 4096).is_ok());
        assert!(matches!(
            map.validate_file_size(6 * 4096 - 1, 4096),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validate_file_size(0, 4096),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validate_file_size(1, 0),
            Err(Error::InvalidInput(_))
        ));
        assert!(ExtentMap::default().validate_file_size(0, 4096).is_ok());
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
    fn failed_extent_insertions_leave_map_unchanged_and_physical_iteration_skips_holes() {
        let mut map = sample();
        let before = map.clone();
        assert!(matches!(
            map.push_coalescing(Extent {
                logical_start: 7,
                physical_start: 201,
                block_count: 1,
            }),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(map, before);
        assert_eq!(
            map.physical_blocks().collect::<Vec<_>>(),
            vec![100, 101, 102, 200, 201]
        );
    }

    #[test]
    fn validated_physical_inventory_is_sorted_and_rejects_invalid_ownership() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 200,
                block_count: 2,
            },
            Extent {
                logical_start: 3,
                physical_start: 100,
                block_count: 2,
            },
        ])
        .unwrap();
        assert_eq!(
            map.validated_physical_block_inventory(5, 80, 300).unwrap(),
            vec![100, 101, 200, 201]
        );
        assert!(matches!(
            map.validated_physical_block_inventory(4, 80, 300),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validated_physical_block_inventory(5, 150, 300),
            Err(Error::Corrupt(_))
        ));

        assert!(matches!(
            ExtentMap::new(vec![
                Extent {
                    logical_start: 0,
                    physical_start: 100,
                    block_count: 3,
                },
                Extent {
                    logical_start: 4,
                    physical_start: 102,
                    block_count: 2,
                },
            ]),
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

        assert!(matches!(
            ExtentMap::new(vec![
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
            ]),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            sample().validate(6, 100, 201),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn logical_length_includes_sparse_holes_and_physical_bounds_exclude_metadata() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 2,
                physical_start: 20,
                block_count: 3,
            },
            Extent {
                logical_start: 9,
                physical_start: 30,
                block_count: 2,
            },
        ])
        .unwrap();
        assert_eq!(map.allocated_blocks().unwrap(), 5);
        assert_eq!(map.logical_block_count().unwrap(), 11);
        assert!(map.validate_physical_bounds(16, 40).is_ok());
        assert!(matches!(
            map.validate_physical_bounds(21, 40),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validate_physical_bounds(16, 31),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validate_physical_bounds(40, 40),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ExtentMap::default().logical_block_count().unwrap(), 0);
    }

    #[test]
    fn validated_physical_ranges_coalesce_adjacent_runs_and_keep_gaps() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 200,
                block_count: 3,
            },
            Extent {
                logical_start: 4,
                physical_start: 100,
                block_count: 2,
            },
            Extent {
                logical_start: 8,
                physical_start: 102,
                block_count: 4,
            },
        ])
        .unwrap();
        assert_eq!(
            map.validated_physical_ranges(12, 80, 300).unwrap(),
            vec![(100, 106), (200, 203)]
        );
        assert_eq!(map.validated_physical_block_count(12, 80, 300).unwrap(), 9);
        assert!(matches!(
            map.validated_physical_ranges(5, 80, 300),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validated_physical_ranges(12, 201, 300),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            map.validated_physical_block_count(12, 201, 300),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn range_removal_splits_extents_and_returns_releasable_runs() {
        let mut map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 8,
            },
            Extent {
                logical_start: 12,
                physical_start: 300,
                block_count: 3,
            },
        ])
        .unwrap();
        let removed = map.remove_range(2, 4).unwrap();
        assert_eq!(
            removed,
            vec![Extent {
                logical_start: 2,
                physical_start: 102,
                block_count: 4,
            }]
        );
        assert_eq!(
            map.extents(),
            &[
                Extent {
                    logical_start: 0,
                    physical_start: 100,
                    block_count: 2,
                },
                Extent {
                    logical_start: 6,
                    physical_start: 106,
                    block_count: 2,
                },
                Extent {
                    logical_start: 12,
                    physical_start: 300,
                    block_count: 3,
                },
            ]
        );
        assert!(matches!(
            map.remove_range(u64::MAX, 2),
            Err(Error::ArithmeticOverflow)
        ));
        assert_eq!(map.extents().len(), 3);
    }

    #[test]
    fn truncate_preserves_prefix_and_returns_tail_extents() {
        let mut map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 3,
            },
            Extent {
                logical_start: 5,
                physical_start: 200,
                block_count: 4,
            },
        ])
        .unwrap();
        let removed = map.truncate(6).unwrap();
        assert_eq!(
            removed,
            vec![Extent {
                logical_start: 6,
                physical_start: 201,
                block_count: 3,
            }]
        );
        assert_eq!(
            map.extents(),
            &[
                Extent {
                    logical_start: 0,
                    physical_start: 100,
                    block_count: 3,
                },
                Extent {
                    logical_start: 5,
                    physical_start: 200,
                    block_count: 1,
                },
            ]
        );
        assert!(map.truncate(64).unwrap().is_empty());
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
