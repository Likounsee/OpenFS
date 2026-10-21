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

    /// Clone an extent map without relying on infallible vector growth.
    ///
    /// File mutation paths use this before touching persistent state so allocation
    /// pressure is reported as an ordinary filesystem error rather than aborting midway.
    pub fn try_clone(&self) -> Result<Self> {
        let mut extents = Vec::new();
        extents
            .try_reserve_exact(self.extents.len())
            .map_err(|_| Error::AllocationFailed("unable to clone extent map"))?;
        extents.extend_from_slice(&self.extents);
        Ok(Self { extents })
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
        let physical_end = extent.physical_end()?;
        // Physical addresses are not necessarily ordered with logical addresses, so
        // check ownership overlap before either appending or coalescing. This keeps
        // the fast path equivalent to full-map validation without cloning/sorting.
        for existing in &self.extents {
            let existing_end = existing.physical_end()?;
            if extent.physical_start < existing_end && existing.physical_start < physical_end {
                return Err(Error::InvalidInput("physical extents overlap"));
            }
        }

        if let Some(last) = self.extents.last_mut() {
            let logical_end = last.logical_end()?;
            let last_physical_end = last.physical_end()?;
            if logical_end == extent.logical_start && last_physical_end == extent.physical_start {
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
        self.extents
            .try_reserve(1)
            .map_err(|_| Error::AllocationFailed("unable to reserve extent-map append"))?;
        self.extents.push(extent);
        Ok(())
    }

    /// Append a validated batch of logical extents with one physical-overlap scan.
    ///
    /// Reader-driven imports often produce many extents on fragmented volumes. Calling
    /// `push_coalescing` for each run rescans all earlier extents each time, making import
    /// bookkeeping quadratic. This batch path validates all incoming and existing physical
    /// ranges together, then appends/coalesces only after the full batch is known to be valid.
    pub(crate) fn push_coalescing_batch(&mut self, incoming: Vec<Extent>) -> Result<()> {
        if incoming.is_empty() {
            return Ok(());
        }
        for extent in &incoming {
            validate_extent(*extent)?;
        }

        if let Some(last) = self.extents.last() {
            let logical_end = last.logical_end()?;
            if incoming[0].logical_start < logical_end {
                return Err(Error::InvalidInput("extent overlaps prior logical blocks"));
            }
        }
        for pair in incoming.windows(2) {
            if pair[0].logical_end()? > pair[1].logical_start {
                return Err(Error::InvalidInput("batch extents overlap logical blocks"));
            }
        }

        let range_count = self
            .extents
            .len()
            .checked_add(incoming.len())
            .ok_or(Error::ArithmeticOverflow)?;
        let mut physical_ranges = Vec::new();
        physical_ranges
            .try_reserve_exact(range_count)
            .map_err(|_| Error::AllocationFailed("unable to reserve physical extent validation"))?;
        for extent in self.extents.iter().chain(incoming.iter()) {
            physical_ranges.push((extent.physical_start, extent.physical_end()?));
        }
        physical_ranges.sort_unstable();
        if physical_ranges.windows(2).any(|pair| pair[0].1 > pair[1].0) {
            return Err(Error::InvalidInput("physical extents overlap"));
        }

        self.extents
            .try_reserve(incoming.len())
            .map_err(|_| Error::AllocationFailed("unable to reserve extent-map batch"))?;
        for extent in incoming {
            if let Some(last) = self.extents.last_mut() {
                if last.logical_end()? == extent.logical_start
                    && last.physical_end()? == extent.physical_start
                {
                    last.block_count = last
                        .block_count
                        .checked_add(extent.block_count)
                        .ok_or(Error::ArithmeticOverflow)?;
                    continue;
                }
            }
            self.extents.push(extent);
        }
        Ok(())
    }

    /// Insert an extent at any logical position, merging adjacent logical and physical runs.
    ///
    /// This supports rebuilding a file's map when newly allocated blocks are not physically
    /// adjacent to its existing data. Overlapping logical ranges are rejected, and the map
    /// remains unchanged if validation or arithmetic fails.
    pub fn insert_coalescing(&mut self, extent: Extent) -> Result<()> {
        validate_extent(extent)?;
        let physical_end = extent.physical_end()?;
        // The current map is already validated. Check only the new run against existing
        // physical ownership so we can keep validation linear and avoid sorting every
        // physical range again after insertion.
        for existing in &self.extents {
            let existing_end = existing.physical_end()?;
            if extent.physical_start < existing_end && existing.physical_start < physical_end {
                return Err(Error::InvalidInput("physical extents overlap"));
            }
        }

        let position = self
            .extents
            .partition_point(|item| item.logical_start < extent.logical_start);

        // Merge directly from the existing sorted map plus the insertion point.
        // Avoid building a cloned candidate vector and then a second merged vector.
        let capacity = self
            .extents
            .len()
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut merged: Vec<Extent> = Vec::new();
        merged
            .try_reserve_exact(capacity)
            .map_err(|_| Error::AllocationFailed("unable to reserve extent insertion"))?;
        for current in self
            .extents
            .iter()
            .copied()
            .take(position)
            .chain(std::iter::once(extent))
            .chain(self.extents.iter().copied().skip(position))
        {
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

        // Logical order and physical non-overlap are now established without a
        // second full-map sort: existing runs were valid and the inserted run was
        // checked against every existing physical range above.
        self.extents = merged;
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
        let block_count =
            usize::try_from(self.allocated_blocks()?).map_err(|_| Error::ArithmeticOverflow)?;
        let mut blocks: Vec<u64> = Vec::new();
        blocks
            .try_reserve_exact(block_count)
            .map_err(|_| Error::AllocationFailed("unable to reserve physical block inventory"))?;
        blocks.extend(self.physical_blocks());
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

    /// Return logical sparse holes inside a file's logical block range.
    ///
    /// The returned pairs are (start, block_count), ordered by logical offset. Extents
    /// outside the requested file length are rejected rather than silently clipped, so
    /// callers can use the result when validating persisted metadata before mutating data.
    /// The output is proportional to the number of holes, not the file's logical size.
    pub fn logical_hole_ranges(&self, logical_block_count: u64) -> Result<Vec<(u64, u64)>> {
        self.validate_ordering()?;
        let mut holes = Vec::new();
        holes
            .try_reserve_exact(self.extents.len().saturating_add(1))
            .map_err(|_| Error::AllocationFailed("unable to reserve sparse-hole ranges"))?;

        let mut cursor = 0_u64;
        for extent in &self.extents {
            let end = extent.logical_end()?;
            if end > logical_block_count {
                return Err(Error::Corrupt("extent exceeds logical file block count"));
            }
            if extent.logical_start > cursor {
                holes.push((cursor, extent.logical_start - cursor));
            }
            cursor = end;
        }
        if cursor < logical_block_count {
            holes.push((cursor, logical_block_count - cursor));
        }
        Ok(holes)
    }

    /// Return whether every logical block in the requested file range is backed by data.
    ///
    /// Empty files are fully allocated by definition. Invalid extents beyond the requested
    /// logical length are reported as corruption instead of being mistaken for a dense map.
    pub fn is_fully_allocated(&self, logical_block_count: u64) -> Result<bool> {
        self.validate_ordering()?;
        let mut cursor = 0_u64;
        let mut fully_allocated = true;
        for extent in &self.extents {
            let end = extent.logical_end()?;
            if end > logical_block_count {
                return Err(Error::Corrupt("extent exceeds logical file block count"));
            }
            if extent.logical_start != cursor {
                fully_allocated = false;
            }
            cursor = end;
        }
        Ok(fully_allocated && cursor == logical_block_count)
    }

    /// Check whether a logical block range is fully backed by extents.
    ///
    /// The requested range must fit inside the file logical block count. Sparse holes
    /// return false; malformed extents are reported as corruption even if a hole was
    /// encountered earlier in the map.
    /// Count physically allocated logical blocks intersecting a file range.
    ///
    /// Sparse holes are excluded from the count. The requested half-open range must fit
    /// inside the file's logical length, and every extent is validated so corruption beyond
    /// the requested range cannot be hidden by an early result.
    pub fn allocated_blocks_in_range(
        &self,
        logical_start: u64,
        block_count: u64,
        logical_block_count: u64,
    ) -> Result<u64> {
        let logical_end = logical_start
            .checked_add(block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        if logical_end > logical_block_count {
            return Err(Error::InvalidInput(
                "logical range exceeds file block count",
            ));
        }
        self.validate_ordering()?;

        let mut allocated = 0_u64;
        for extent in &self.extents {
            let extent_end = extent.logical_end()?;
            if extent_end > logical_block_count {
                return Err(Error::Corrupt("extent exceeds logical file block count"));
            }
            let overlap_start = logical_start.max(extent.logical_start);
            let overlap_end = logical_end.min(extent_end);
            if overlap_start < overlap_end {
                allocated = allocated
                    .checked_add(overlap_end - overlap_start)
                    .ok_or(Error::ArithmeticOverflow)?;
            }
        }
        Ok(allocated)
    }

    /// Check whether every logical block in a requested range is backed by an extent.
    pub fn logical_range_is_allocated(
        &self,
        logical_start: u64,
        block_count: u64,
        logical_block_count: u64,
    ) -> Result<bool> {
        Ok(
            self.allocated_blocks_in_range(logical_start, block_count, logical_block_count)?
                == block_count,
        )
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
        let mut ranges: Vec<(u64, u64)> = Vec::new();
        ranges
            .try_reserve_exact(self.extents.len())
            .map_err(|_| Error::AllocationFailed("unable to reserve physical extent ranges"))?;
        for extent in &self.extents {
            ranges.push((extent.physical_start, extent.physical_end()?));
        }
        ranges.sort_unstable();

        // Compact the sorted ranges in place. Keeping the output in the same buffer
        // avoids a second allocation on every ownership/fragmentation inspection.
        let mut write = 0_usize;
        for read in 0..ranges.len() {
            let (start, end) = ranges[read];
            if write > 0 && ranges[write - 1].1 == start {
                ranges[write - 1].1 = end;
            } else {
                ranges[write] = (start, end);
                write += 1;
            }
        }
        ranges.truncate(write);
        Ok(ranges)
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

        // A disjoint range cannot release any physical blocks. Detect it with
        // the sorted logical index before allocating rebuilt/removed extent lists.
        let insertion = self
            .extents
            .partition_point(|extent| extent.logical_start < logical_start);
        let overlaps_previous =
            insertion > 0 && self.extents[insertion - 1].logical_end()? > logical_start;
        let overlaps_next = self
            .extents
            .get(insertion)
            .is_some_and(|extent| extent.logical_start < logical_end);
        if !overlaps_previous && !overlaps_next {
            return Ok(Vec::new());
        }

        let capacity = self
            .extents
            .len()
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut kept = Vec::new();
        kept.try_reserve_exact(capacity)
            .map_err(|_| Error::AllocationFailed("unable to reserve retained extent ranges"))?;
        let mut removed = Vec::new();
        removed
            .try_reserve_exact(self.extents.len())
            .map_err(|_| Error::AllocationFailed("unable to reserve removed extent ranges"))?;
        // Logical extents are sorted and non-overlapping. Begin at the only extent
        // that might cross the start boundary instead of scanning the untouched prefix.
        let first_candidate = insertion.saturating_sub(1);
        kept.extend_from_slice(&self.extents[..first_candidate]);

        // Slice before iterating: skipping the prefix on the full iterator would
        // still walk untouched extents and erase the benefit of the binary search.
        for (offset, extent) in self.extents[first_candidate..].iter().copied().enumerate() {
            let index = first_candidate + offset;
            if extent.logical_start >= logical_end {
                // The remainder lies wholly after the removed range; preserve it as-is.
                kept.extend_from_slice(&self.extents[index..]);
                break;
            }
            let extent_end = extent.logical_end()?;
            let overlap_start = extent.logical_start.max(logical_start);
            let overlap_end = extent_end.min(logical_end);
            if overlap_start >= overlap_end {
                kept.push(extent);
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
        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(block_size)
            .map_err(|_| Error::AllocationFailed("unable to allocate encoded extent-map block"))?;
        bytes.resize(block_size, 0);
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
        let mut extents = Vec::new();
        extents
            .try_reserve_exact(count)
            .map_err(|_| Error::AllocationFailed("unable to reserve decoded extent records"))?;
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
        // Zero or one extent cannot physically overlap itself. Avoid a temporary
        // allocation for the common empty/small-file case.
        if self.extents.len() <= 1 {
            if let Some(extent) = self.extents.first().copied() {
                validate_extent(extent)?;
            }
            return Ok(());
        }

        let mut physical = Vec::new();
        physical
            .try_reserve_exact(self.extents.len())
            .map_err(|_| Error::AllocationFailed("unable to reserve extent validation ranges"))?;
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
    use super::{crc32c, Extent, ExtentMap};
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
    fn batch_extent_append_coalesces_runs_like_individual_appends() {
        let initial = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 100,
            block_count: 2,
        }])
        .unwrap();
        let incoming = vec![
            Extent {
                logical_start: 2,
                physical_start: 102,
                block_count: 2,
            },
            Extent {
                logical_start: 7,
                physical_start: 300,
                block_count: 1,
            },
            Extent {
                logical_start: 8,
                physical_start: 301,
                block_count: 2,
            },
        ];

        let mut batch = initial.clone();
        batch.push_coalescing_batch(incoming.clone()).unwrap();

        let mut individual = initial;
        for extent in incoming {
            individual.push_coalescing(extent).unwrap();
        }

        assert_eq!(batch, individual);
        assert_eq!(
            batch.extents(),
            &[
                Extent {
                    logical_start: 0,
                    physical_start: 100,
                    block_count: 4,
                },
                Extent {
                    logical_start: 7,
                    physical_start: 300,
                    block_count: 3,
                },
            ]
        );
    }

    #[test]
    fn batch_extent_append_rejects_logical_overlaps_atomically() {
        let mut map = sample();
        let before = map.clone();

        assert!(matches!(
            map.push_coalescing_batch(vec![Extent {
                logical_start: 6,
                physical_start: 300,
                block_count: 2,
            }]),
            Err(Error::InvalidInput("extent overlaps prior logical blocks"))
        ));
        assert_eq!(map, before);

        assert!(matches!(
            map.push_coalescing_batch(vec![
                Extent {
                    logical_start: 8,
                    physical_start: 300,
                    block_count: 3,
                },
                Extent {
                    logical_start: 10,
                    physical_start: 400,
                    block_count: 1,
                },
            ]),
            Err(Error::InvalidInput("batch extents overlap logical blocks"))
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn batch_extent_append_rejects_physical_aliases_without_mutating_map() {
        let mut map = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 100,
            block_count: 2,
        }])
        .unwrap();
        let before = map.clone();

        assert!(matches!(
            map.push_coalescing_batch(vec![
                Extent {
                    logical_start: 4,
                    physical_start: 200,
                    block_count: 2,
                },
                Extent {
                    logical_start: 8,
                    physical_start: 101,
                    block_count: 1,
                },
            ]),
            Err(Error::InvalidInput("physical extents overlap"))
        ));
        assert_eq!(map, before);

        assert!(matches!(
            map.remove_range(3, 0),
            Err(Error::InvalidInput("removed logical range must be nonzero"))
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn range_removal_matches_blockwise_reference_for_sparse_maps() {
        for logical_start in 0..10 {
            for block_count in 1..=5 {
                let mut map = sample();
                let original = map.clone();
                let logical_end = logical_start + block_count;
                let expected_removed: Vec<(u64, u64)> = (logical_start..logical_end)
                    .filter_map(|logical| {
                        original
                            .physical_block(logical)
                            .map(|physical| (logical, physical))
                    })
                    .collect();

                let removed = map.remove_range(logical_start, block_count).unwrap();
                let actual_removed: Vec<(u64, u64)> = removed
                    .iter()
                    .flat_map(|extent| {
                        (0..extent.block_count).map(move |offset| {
                            (
                                extent.logical_start + offset,
                                extent.physical_start + offset,
                            )
                        })
                    })
                    .collect();
                assert_eq!(
                    actual_removed, expected_removed,
                    "removed runs differ for start={logical_start}, count={block_count}"
                );

                for logical in 0..10 {
                    if (logical_start..logical_end).contains(&logical) {
                        assert_eq!(map.physical_block(logical), None);
                    } else {
                        assert_eq!(
                            map.physical_block(logical),
                            original.physical_block(logical)
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn removing_disjoint_sparse_ranges_is_a_noop() {
        let mut map = sample();
        let before = map.clone();

        assert!(map.remove_range(3, 2).unwrap().is_empty());
        assert_eq!(map, before);
        assert!(map.remove_range(8, 2).unwrap().is_empty());
        assert_eq!(map, before);
        assert!(map.remove_range(u64::MAX - 1, 1).unwrap().is_empty());
        assert_eq!(map, before);
    }

    #[test]
    fn middle_insert_coalesces_both_neighbors_and_rejects_overlap_atomically() {
        let mut map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 2,
            },
            Extent {
                logical_start: 4,
                physical_start: 104,
                block_count: 2,
            },
        ])
        .unwrap();

        map.insert_coalescing(Extent {
            logical_start: 2,
            physical_start: 102,
            block_count: 2,
        })
        .unwrap();
        assert_eq!(
            map.extents(),
            &[Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 6,
            }]
        );

        let before = map.clone();
        assert!(matches!(
            map.insert_coalescing(Extent {
                logical_start: 6,
                physical_start: 103,
                block_count: 1,
            }),
            Err(Error::InvalidInput("physical extents overlap"))
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn truncation_preserves_sparse_prefix_and_releases_only_allocated_suffix() {
        let mut map = sample();
        let removed = map.truncate(6).unwrap();
        assert_eq!(
            removed,
            vec![Extent {
                logical_start: 6,
                physical_start: 201,
                block_count: 1,
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

        let before = map.clone();
        assert!(map.truncate(6).unwrap().is_empty());
        assert_eq!(map, before);
    }

    #[test]
    fn allocated_range_queries_handle_empty_ranges_and_reject_overflow() {
        let map = sample();
        assert_eq!(map.allocated_blocks_in_range(3, 0, 7).unwrap(), 0);
        assert!(map.logical_range_is_allocated(3, 0, 7).unwrap());
        assert!(matches!(
            map.allocated_blocks_in_range(u64::MAX, 1, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(matches!(
            map.allocated_blocks_in_range(7, 1, 7),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn truncate_to_zero_releases_all_allocated_runs_but_not_sparse_holes() {
        let mut map = sample();
        assert_eq!(
            map.truncate(0).unwrap(),
            vec![
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
            ]
        );
        assert!(map.is_empty());
        assert_eq!(map.logical_block_count().unwrap(), 0);
        assert!(map.truncate(0).unwrap().is_empty());
    }

    #[test]
    fn failed_middle_insert_preserves_map_for_logical_and_arithmetic_errors() {
        let mut map = sample();
        let before = map.clone();

        assert!(matches!(
            map.insert_coalescing(Extent {
                logical_start: 1,
                physical_start: 300,
                block_count: 1,
            }),
            Err(Error::InvalidInput(
                "extent overlaps existing logical blocks"
            ))
        ));
        assert_eq!(map, before);

        assert!(matches!(
            map.insert_coalescing(Extent {
                logical_start: u64::MAX,
                physical_start: 400,
                block_count: 1,
            }),
            Err(Error::ArithmeticOverflow)
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn range_removal_overflow_is_rejected_without_mutation() {
        let mut map = sample();
        let before = map.clone();
        assert!(matches!(
            map.remove_range(u64::MAX - 1, 4),
            Err(Error::ArithmeticOverflow)
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn validated_physical_ranges_coalesce_adjacent_runs_without_losing_gaps() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 200,
                block_count: 2,
            },
            Extent {
                logical_start: 2,
                physical_start: 202,
                block_count: 3,
            },
            Extent {
                logical_start: 8,
                physical_start: 100,
                block_count: 1,
            },
            Extent {
                logical_start: 9,
                physical_start: 110,
                block_count: 1,
            },
        ])
        .unwrap();

        assert_eq!(
            map.validated_physical_ranges(10, 80, 300).unwrap(),
            vec![(100, 101), (110, 111), (200, 205)]
        );
        assert_eq!(
            ExtentMap::default()
                .validated_physical_ranges(0, 80, 300)
                .unwrap(),
            Vec::<(u64, u64)>::new()
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
    fn sparse_hole_ranges_cover_prefix_middle_suffix_and_empty_files() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 2,
                physical_start: 20,
                block_count: 2,
            },
            Extent {
                logical_start: 7,
                physical_start: 40,
                block_count: 1,
            },
        ])
        .unwrap();

        assert_eq!(map.logical_hole_ranges(8).unwrap(), vec![(0, 2), (4, 3)]);
        assert_eq!(
            ExtentMap::default().logical_hole_ranges(0).unwrap(),
            Vec::<(u64, u64)>::new()
        );
        assert_eq!(
            ExtentMap::default().logical_hole_ranges(4).unwrap(),
            vec![(0, 4)]
        );
        assert!(matches!(map.logical_hole_ranges(7), Err(Error::Corrupt(_))));
    }

    #[test]
    fn logical_range_allocation_checks_holes_bounds_and_overflow() {
        let map = sample();
        assert!(map.logical_range_is_allocated(0, 3, 7).unwrap());
        assert!(!map.logical_range_is_allocated(2, 4, 7).unwrap());
        assert!(map.logical_range_is_allocated(5, 2, 7).unwrap());
        assert!(map.logical_range_is_allocated(7, 0, 7).unwrap());
        assert!(matches!(
            map.logical_range_is_allocated(6, 2, 7),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            map.logical_range_is_allocated(u64::MAX, 1, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
    }

    #[test]
    fn allocated_block_count_in_range_handles_sparse_boundaries() {
        let map = sample();
        assert_eq!(map.allocated_blocks_in_range(0, 7, 7).unwrap(), 5);
        assert_eq!(map.allocated_blocks_in_range(1, 2, 7).unwrap(), 2);
        assert_eq!(map.allocated_blocks_in_range(2, 4, 7).unwrap(), 2);
        assert_eq!(map.allocated_blocks_in_range(3, 2, 7).unwrap(), 0);
        assert_eq!(map.allocated_blocks_in_range(7, 0, 7).unwrap(), 0);
        assert!(matches!(
            map.allocated_blocks_in_range(6, 2, 7),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            map.allocated_blocks_in_range(u64::MAX, 1, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
    }

    #[test]
    fn allocated_block_count_in_range_does_not_hide_later_corruption() {
        let mut map = sample();
        map.extents.push(Extent {
            logical_start: 10,
            physical_start: 300,
            block_count: 1,
        });
        assert!(matches!(
            map.allocated_blocks_in_range(0, 1, 7),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn logical_range_allocation_does_not_hide_corruption_after_an_earlier_hole() {
        let mut map = sample();
        map.extents.push(Extent {
            logical_start: 10,
            physical_start: 300,
            block_count: 1,
        });
        assert!(matches!(
            map.logical_range_is_allocated(3, 1, 7),
            Err(Error::Corrupt(_))
        ));
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
    fn range_removal_crosses_sparse_holes_and_preserves_unaffected_extents() {
        let mut map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 100,
                block_count: 3,
            },
            Extent {
                logical_start: 5,
                physical_start: 200,
                block_count: 3,
            },
            Extent {
                logical_start: 10,
                physical_start: 300,
                block_count: 2,
            },
        ])
        .unwrap();

        let removed = map.remove_range(2, 9).unwrap();
        assert_eq!(
            removed,
            vec![
                Extent {
                    logical_start: 2,
                    physical_start: 102,
                    block_count: 1,
                },
                Extent {
                    logical_start: 5,
                    physical_start: 200,
                    block_count: 3,
                },
                Extent {
                    logical_start: 10,
                    physical_start: 300,
                    block_count: 1,
                },
            ]
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
                    logical_start: 11,
                    physical_start: 301,
                    block_count: 1,
                },
            ]
        );

        let before = map.clone();
        assert!(map.remove_range(20, 1).unwrap().is_empty());
        assert_eq!(map, before);
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
    fn physical_bounds_reject_reserved_blocks_and_volume_overflow() {
        let map = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 9,
            block_count: 2,
        }])
        .unwrap();
        assert!(matches!(
            map.validate_physical_bounds(10, 100),
            Err(Error::Corrupt("extent points outside the data region"))
        ));
        assert!(matches!(
            map.validate_physical_bounds(0, 10),
            Err(Error::Corrupt("extent points outside the data region"))
        ));
        assert!(matches!(
            map.validate_physical_bounds(10, 10),
            Err(Error::InvalidInput("data region bounds are invalid"))
        ));
    }

    #[test]
    fn file_size_validation_allows_sparse_holes_but_rejects_extents_past_eof() {
        let sparse = ExtentMap::new(vec![
            Extent {
                logical_start: 2,
                physical_start: 20,
                block_count: 1,
            },
            Extent {
                logical_start: 8,
                physical_start: 40,
                block_count: 2,
            },
        ])
        .unwrap();
        assert!(sparse.validate_file_size(10 * 4096 - 1, 4096).is_ok());
        assert!(matches!(
            sparse.validate_file_size(9 * 4096, 4096),
            Err(Error::Corrupt("extent exceeds file byte length"))
        ));
        assert!(matches!(
            sparse.validate_file_size(1, 0),
            Err(Error::InvalidInput("block size must be nonzero"))
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

    #[test]
    fn batch_append_empty_input_is_a_noop() {
        let mut map = sample();
        let before = map.clone();

        map.push_coalescing_batch(Vec::new()).unwrap();

        assert_eq!(map, before);
    }

    #[test]
    fn batch_append_rejects_invalid_extents_atomically() {
        let invalid_batches = [
            vec![Extent {
                logical_start: 9,
                physical_start: 500,
                block_count: 0,
            }],
            vec![Extent {
                logical_start: u64::MAX,
                physical_start: 500,
                block_count: 1,
            }],
            vec![Extent {
                logical_start: 9,
                physical_start: u64::MAX,
                block_count: 1,
            }],
        ];

        for incoming in invalid_batches {
            let mut map = sample();
            let before = map.clone();

            assert!(map.push_coalescing_batch(incoming).is_err());
            assert_eq!(map, before);
        }
    }

    #[test]
    fn batch_append_keeps_logically_adjacent_runs_separate_when_physical_runs_are_not() {
        let mut map = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 100,
            block_count: 2,
        }])
        .unwrap();

        map.push_coalescing_batch(vec![Extent {
            logical_start: 2,
            physical_start: 500,
            block_count: 3,
        }])
        .unwrap();

        assert_eq!(
            map.extents(),
            &[
                Extent {
                    logical_start: 0,
                    physical_start: 100,
                    block_count: 2,
                },
                Extent {
                    logical_start: 2,
                    physical_start: 500,
                    block_count: 3,
                },
            ]
        );
    }

    #[test]
    fn decode_rejects_truncated_and_checksum_corrupted_blocks() {
        let bytes = sample().encode_block(4096).unwrap();

        assert!(matches!(
            ExtentMap::decode_block(&bytes[..19]),
            Err(Error::InvalidInput("extent-map block is too short"))
        ));

        let mut corrupted = bytes;
        corrupted[20] ^= 0x80;
        assert!(matches!(
            ExtentMap::decode_block(&corrupted),
            Err(Error::Corrupt("extent-map CRC32C mismatch"))
        ));
    }

    #[test]
    fn decode_rejects_nonzero_padding_even_with_valid_checksum() {
        let mut bytes = sample().encode_block(4096).unwrap();
        let padding_offset = 16 + sample().extents().len() * 24;
        bytes[padding_offset] = 1;
        let checksum_offset = bytes.len() - 4;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());

        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::Corrupt("extent-map padding must be zero"))
        ));
    }

    #[test]
    fn decode_rejects_extent_count_beyond_block_capacity() {
        let mut bytes = sample().encode_block(80).unwrap();
        bytes[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
        let checksum_offset = bytes.len() - 4;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());

        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::Corrupt("extent count exceeds block capacity"))
        ));
    }

    #[test]
    fn decode_rejects_unsupported_version_before_parsing_records() {
        let mut bytes = sample().encode_block(4096).unwrap();
        bytes[8..10].copy_from_slice(&u16::MAX.to_le_bytes());

        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::UnsupportedVersion(u16::MAX))
        ));
    }

    #[test]
    fn decode_rejects_invalid_magic_and_header_size() {
        let original = sample().encode_block(4096).unwrap();

        let mut bad_magic = original.clone();
        bad_magic[0] ^= 1;
        assert!(matches!(
            ExtentMap::decode_block(&bad_magic),
            Err(Error::Corrupt("bad extent-map magic"))
        ));

        let mut bad_header_size = original;
        bad_header_size[10..12].copy_from_slice(&0_u16.to_le_bytes());
        assert!(matches!(
            ExtentMap::decode_block(&bad_header_size),
            Err(Error::Corrupt("unexpected extent-map header size"))
        ));
    }

    #[test]
    fn decode_rejects_zero_length_extent_with_valid_checksum() {
        let mut bytes = sample().encode_block(4096).unwrap();
        bytes[16 + 16..16 + 24].copy_from_slice(&0_u64.to_le_bytes());
        let checksum_offset = bytes.len() - 4;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());

        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::Corrupt("extent block count must be nonzero"))
        ));
    }

    #[test]
    fn empty_extent_map_round_trips_in_minimum_sized_block() {
        let map = ExtentMap::default();
        let bytes = map.encode_block(20).unwrap();

        assert_eq!(ExtentMap::decode_block(&bytes).unwrap(), map);
    }
    #[test]
    fn decode_rejects_logical_overlap_and_physical_aliasing() {
        let original = sample().encode_block(4096).unwrap();
        let checksum_offset = original.len() - 4;

        let mut logical_overlap = original.clone();
        logical_overlap[40..48].copy_from_slice(&2_u64.to_le_bytes());
        let checksum = crc32c(&logical_overlap[..checksum_offset]);
        logical_overlap[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            ExtentMap::decode_block(&logical_overlap),
            Err(Error::Corrupt("logical extents overlap or are unordered"))
        ));

        let mut physical_alias = original;
        physical_alias[40 + 8..40 + 16].copy_from_slice(&101_u64.to_le_bytes());
        let checksum = crc32c(&physical_alias[..checksum_offset]);
        physical_alias[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            ExtentMap::decode_block(&physical_alias),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn decode_rejects_extent_arithmetic_overflow() {
        let mut bytes = sample().encode_block(4096).unwrap();
        bytes[16 + 16..16 + 24].copy_from_slice(&u64::MAX.to_le_bytes());
        let checksum_offset = bytes.len() - 4;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_le_bytes());

        assert!(matches!(
            ExtentMap::decode_block(&bytes),
            Err(Error::ArithmeticOverflow)
        ));
    }

    #[test]
    fn logical_holes_and_allocation_queries_cover_sparse_boundaries() {
        let map = sample();

        assert_eq!(map.logical_hole_ranges(7).unwrap(), vec![(3, 2)]);
        assert_eq!(map.logical_hole_ranges(9).unwrap(), vec![(3, 2), (7, 2)]);
        assert!(!map.is_fully_allocated(7).unwrap());
        assert!(map.is_fully_allocated(3).is_err());
        assert!(map.logical_range_is_allocated(1, 2, 7).unwrap());
        assert!(!map.logical_range_is_allocated(3, 2, 7).unwrap());
        assert_eq!(map.allocated_blocks_in_range(1, 5, 7).unwrap(), 4);
        assert!(map.logical_hole_ranges(6).is_err());
    }

    #[test]
    fn validated_physical_inventory_and_ranges_are_sorted_and_coalesced() {
        let map = ExtentMap::new(vec![
            Extent {
                logical_start: 0,
                physical_start: 200,
                block_count: 2,
            },
            Extent {
                logical_start: 2,
                physical_start: 100,
                block_count: 2,
            },
            Extent {
                logical_start: 4,
                physical_start: 102,
                block_count: 1,
            },
        ])
        .unwrap();

        assert_eq!(
            map.validated_physical_block_inventory(5, 10, 300).unwrap(),
            vec![100, 101, 102, 200, 201]
        );
        assert_eq!(
            map.validated_physical_ranges(5, 10, 300).unwrap(),
            vec![(100, 103), (200, 202)]
        );
        assert!(map.validated_physical_ranges(4, 10, 300).is_err());
        assert!(map.validated_physical_block_inventory(5, 100, 300).is_err());
    }

    #[test]
    fn byte_capacity_helpers_report_overflow_instead_of_wrapping() {
        let map = ExtentMap::new(vec![Extent {
            logical_start: 0,
            physical_start: 0,
            block_count: u64::MAX,
        }])
        .unwrap();

        assert!(matches!(
            map.allocated_byte_count(4096),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(matches!(
            map.logical_byte_capacity(4096),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(matches!(
            map.allocated_byte_count(0),
            Err(Error::InvalidInput("block size must be nonzero"))
        ));
    }

    #[test]
    fn failed_coalescing_append_keeps_existing_map_unchanged() {
        let mut map = sample();
        let before = map.clone();

        assert!(matches!(
            map.push_coalescing(Extent {
                logical_start: 7,
                physical_start: 102,
                block_count: 1,
            }),
            Err(Error::InvalidInput("physical extents overlap"))
        ));
        assert_eq!(map, before);
    }

    #[test]
    fn logical_to_physical_lookup_handles_sparse_holes_and_eof() {
        let map = sample();

        assert_eq!(map.physical_block(0), Some(100));
        assert_eq!(map.physical_block(2), Some(102));
        assert_eq!(map.physical_block(3), None);
        assert_eq!(map.physical_block(4), None);
        assert_eq!(map.physical_block(5), Some(200));
        assert_eq!(map.physical_block(6), Some(201));
        assert_eq!(map.physical_block(7), None);
        assert_eq!(map.physical_block(u64::MAX), None);
    }
}
