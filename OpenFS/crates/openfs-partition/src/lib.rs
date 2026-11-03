//! GPT partition-table support and bounded filesystem partition views.
//!
//! GPT LBAs use the device's logical sector size. A PartitionDevice exposes one
//! partition using an OpenFS block size, translating filesystem blocks to sectors.

use std::collections::HashSet;

use openfs_block::BlockDevice;
use openfs_core::{check_block_range, try_zeroed_bytes, Error, Result};

const GPT_HEADER_SIZE: usize = 92;
const GPT_ENTRY_COUNT: u32 = 128;
const GPT_ENTRY_SIZE: u32 = 128;
const GPT_MAX_ENTRY_BYTES: u64 = 16 * 1024 * 1024;
const GPT_NAME_OFFSET: usize = 56;
const GPT_NAME_BYTES: usize = 72;
const MIB: u64 = 1024 * 1024;

/// OpenFS partition type GUID, encoded in GPT on-disk byte order.
/// Canonical GUID: 4F50454E-4653-4F50-454E-465300000001.
pub const OPENFS_PARTITION_TYPE_GUID: [u8; 16] = [
    0x4e, 0x45, 0x50, 0x4f, 0x53, 0x46, 0x50, 0x4f,
    0x45, 0x4e, 0x46, 0x53, 0x00, 0x00, 0x00, 0x01,
];

/// One validated GPT partition entry. LBA values are inclusive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GptPartition {
    /// One-based GPT entry number.
    pub entry_index: u32,
    pub type_guid: [u8; 16],
    pub unique_guid: [u8; 16],
    pub first_lba: u64,
    pub last_lba: u64,
    pub attributes: u64,
    pub name: String,
}

impl GptPartition {
    pub fn is_openfs(&self) -> bool {
        self.type_guid == OPENFS_PARTITION_TYPE_GUID
    }

    pub fn sector_count(&self) -> Result<u64> {
        self.last_lba
            .checked_sub(self.first_lba)
            .and_then(|value| value.checked_add(1))
            .ok_or(Error::ArithmeticOverflow)
    }
}

/// A GPT layout whose primary and backup copies have both passed validation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GptLayout {
    pub sector_size: u32,
    pub block_count: u64,
    pub disk_guid: [u8; 16],
    pub first_usable_lba: u64,
    pub last_usable_lba: u64,
    pub entry_count: u32,
    pub entry_size: u32,
    pub partitions: Vec<GptPartition>,
}

struct GptCopy {
    current_lba: u64,
    backup_lba: u64,
    first_usable_lba: u64,
    last_usable_lba: u64,
    disk_guid: [u8; 16],
    entry_count: u32,
    entry_size: u32,
    entries_crc: u32,
    entries: Vec<u8>,
    partitions: Vec<GptPartition>,
}

/// Plan a single OpenFS partition aligned to at least 1 MiB.
///
/// This function is read-only. Call write_gpt only after the filesystem has
/// been initialized successfully inside the planned partition.
pub fn plan_openfs_gpt<D: BlockDevice>(
    device: &D,
    filesystem_block_size: u32,
    disk_guid: [u8; 16],
    partition_guid: [u8; 16],
) -> Result<GptLayout> {
    let sector_size = device.block_size();
    validate_sector_size(sector_size)?;
    validate_filesystem_block_size(filesystem_block_size, sector_size)?;
    if device.block_count() < 4096 {
        return Err(Error::InvalidInput("disk is too small for a GPT OpenFS partition"));
    }
    if disk_guid.iter().all(|byte| *byte == 0)
        || partition_guid.iter().all(|byte| *byte == 0)
        || disk_guid == partition_guid
    {
        return Err(Error::InvalidInput("GPT disk and partition GUIDs must be non-zero and distinct"));
    }

    let entries_bytes = u64::from(GPT_ENTRY_COUNT) * u64::from(GPT_ENTRY_SIZE);
    let entries_blocks = div_ceil(entries_bytes, u64::from(sector_size))?;
    let backup_header_lba = device
        .block_count()
        .checked_sub(1)
        .ok_or(Error::ArithmeticOverflow)?;
    let backup_entries_lba = backup_header_lba
        .checked_sub(entries_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    let first_usable_lba = 2_u64
        .checked_add(entries_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    let last_usable_lba = backup_entries_lba
        .checked_sub(1)
        .ok_or(Error::ArithmeticOverflow)?;

    // 1 MiB is divisible by every supported OpenFS block size.
    let alignment_sectors = MIB / u64::from(sector_size);
    let first_lba = align_up(first_usable_lba, alignment_sectors)?;
    let end_exclusive = align_down(
        last_usable_lba
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?,
        alignment_sectors,
    );
    if end_exclusive <= first_lba {
        return Err(Error::InvalidInput("disk has no usable space for an OpenFS partition"));
    }
    let last_lba = end_exclusive - 1;
    let partition = GptPartition {
        entry_index: 1,
        type_guid: OPENFS_PARTITION_TYPE_GUID,
        unique_guid: partition_guid,
        first_lba,
        last_lba,
        attributes: 0,
        name: "OpenFS".to_owned(),
    };
    let partition_bytes = partition
        .sector_count()?
        .checked_mul(u64::from(sector_size))
        .ok_or(Error::ArithmeticOverflow)?;
    if partition_bytes < u64::from(filesystem_block_size) * 64
        || partition_bytes % u64::from(filesystem_block_size) != 0
    {
        return Err(Error::InvalidInput("planned OpenFS partition is too small or misaligned"));
    }

    Ok(GptLayout {
        sector_size,
        block_count: device.block_count(),
        disk_guid,
        first_usable_lba,
        last_usable_lba,
        entry_count: GPT_ENTRY_COUNT,
        entry_size: GPT_ENTRY_SIZE,
        partitions: vec![partition],
    })
}

/// Write both GPT copies and a protective MBR.
///
/// The backup GPT is written and flushed first, followed by the primary GPT.
/// The protective MBR is published last. This ordering improves recoverability
/// after an interrupted write, but is not a substitute for device-level
/// power-loss guarantees.
pub fn write_gpt<D: BlockDevice>(device: &mut D, layout: &GptLayout) -> Result<()> {
    if device.block_size() != layout.sector_size || device.block_count() != layout.block_count {
        return Err(Error::InvalidInput("GPT layout no longer matches device geometry"));
    }
    validate_sector_size(layout.sector_size)?;
    if layout.block_count < 6 {
        return Err(Error::InvalidInput("device is too small for GPT"));
    }
    if layout.entry_count == 0 || layout.entry_count > 4096
        || layout.entry_size < 128 || layout.entry_size % 8 != 0
    {
        return Err(Error::InvalidInput("GPT entry geometry is invalid"));
    }
    if layout.partitions.is_empty() {
        return Err(Error::InvalidInput("GPT writer requires at least one partition"));
    }
    if layout.disk_guid.iter().all(|byte| *byte == 0) {
        return Err(Error::InvalidInput("GPT disk GUID must be non-zero"));
    }

    let entries_bytes = u64::from(layout.entry_count)
        .checked_mul(u64::from(layout.entry_size))
        .ok_or(Error::ArithmeticOverflow)?;
    if entries_bytes > GPT_MAX_ENTRY_BYTES {
        return Err(Error::InvalidInput("GPT entry array exceeds the 16 MiB safety limit"));
    }
    let entries_blocks = div_ceil(entries_bytes, u64::from(layout.sector_size))?;
    let backup_header_lba = layout
        .block_count
        .checked_sub(1)
        .ok_or(Error::ArithmeticOverflow)?;
    let backup_entries_lba = backup_header_lba
        .checked_sub(entries_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    if layout.first_usable_lba < 2 + entries_blocks
        || layout.first_usable_lba > layout.last_usable_lba
        || layout.last_usable_lba >= backup_entries_lba
        || layout.last_usable_lba >= layout.block_count
    {
        return Err(Error::InvalidInput("GPT usable-LBA range overlaps GPT metadata"));
    }

    let entries_len = usize::try_from(entries_bytes).map_err(|_| Error::ArithmeticOverflow)?;
    let mut entries = try_zeroed_bytes(entries_len, "unable to allocate GPT entry array")?;
    let mut seen_indices = HashSet::new();
    let mut seen_guids = HashSet::new();
    let mut ranges = Vec::new();
    for partition in &layout.partitions {
        if partition.entry_index == 0 || partition.entry_index > layout.entry_count
            || !seen_indices.insert(partition.entry_index)
            || partition.type_guid.iter().all(|byte| *byte == 0)
            || partition.unique_guid.iter().all(|byte| *byte == 0)
            || !seen_guids.insert(partition.unique_guid)
            || partition.first_lba > partition.last_lba
            || partition.first_lba < layout.first_usable_lba
            || partition.last_lba > layout.last_usable_lba
        {
            return Err(Error::InvalidInput("GPT partition entry is invalid"));
        }
        ranges.push((partition.first_lba, partition.last_lba));
        let offset = usize::try_from(partition.entry_index - 1)
            .map_err(|_| Error::ArithmeticOverflow)?
            .checked_mul(layout.entry_size as usize)
            .ok_or(Error::ArithmeticOverflow)?;
        let entry = &mut entries[offset..offset + layout.entry_size as usize];
        entry[0..16].copy_from_slice(&partition.type_guid);
        entry[16..32].copy_from_slice(&partition.unique_guid);
        entry[32..40].copy_from_slice(&partition.first_lba.to_le_bytes());
        entry[40..48].copy_from_slice(&partition.last_lba.to_le_bytes());
        entry[48..56].copy_from_slice(&partition.attributes.to_le_bytes());
        for (index, unit) in partition.name.encode_utf16().take(36).enumerate() {
            let name_offset = GPT_NAME_OFFSET + index * 2;
            entry[name_offset..name_offset + 2].copy_from_slice(&unit.to_le_bytes());
        }
    }
    ranges.sort_unstable();
    for pair in ranges.windows(2) {
        if pair[1].0 <= pair[0].1 {
            return Err(Error::InvalidInput("GPT partitions overlap"));
        }
    }

    let padded_len = usize::try_from(
        entries_blocks
            .checked_mul(u64::from(layout.sector_size))
            .ok_or(Error::ArithmeticOverflow)?,
    )
    .map_err(|_| Error::ArithmeticOverflow)?;
    let mut padded_entries = try_zeroed_bytes(padded_len, "unable to allocate padded GPT array")?;
    padded_entries[..entries.len()].copy_from_slice(&entries);
    let entries_crc = crc32_ieee(&entries);

    let primary_header = build_header(layout, 1, backup_header_lba, 2, entries_crc)?;
    let backup_header = build_header(layout, backup_header_lba, 1, backup_entries_lba, entries_crc)?;

    // Publish the backup copy first.
    device.write_blocks(backup_entries_lba, &padded_entries)?;
    device.write_blocks(backup_header_lba, &backup_header)?;
    device.flush()?;

    // Then publish the primary copy and protective MBR.
    device.write_blocks(2, &padded_entries)?;
    device.write_blocks(1, &primary_header)?;
    device.flush()?;

    let mut mbr = try_zeroed_bytes(
        layout.sector_size as usize,
        "unable to allocate protective MBR",
    )?;
    mbr[446 + 1..446 + 4].copy_from_slice(&[0x00, 0x02, 0x00]);
    mbr[446 + 4] = 0xee;
    mbr[446 + 5..446 + 8].copy_from_slice(&[0xff, 0xff, 0xff]);
    mbr[446 + 8..446 + 12].copy_from_slice(&1_u32.to_le_bytes());
    let protective_size = layout.block_count.saturating_sub(1).min(u64::from(u32::MAX)) as u32;
    mbr[446 + 12..446 + 16].copy_from_slice(&protective_size.to_le_bytes());
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    device.write_blocks(0, &mbr)?;
    device.flush()
}

/// Read and validate both GPT copies, including their headers, entry-array CRCs,
/// partition GUID uniqueness, bounds, and non-overlap.
pub fn read_gpt<D: BlockDevice>(device: &mut D) -> Result<GptLayout> {
    let sector_size = device.block_size();
    validate_sector_size(sector_size)?;
    let block_count = device.block_count();
    if block_count < 6 {
        return Err(Error::InvalidInput("device is too small for GPT"));
    }
    let mut mbr = try_zeroed_bytes(sector_size as usize, "unable to allocate MBR buffer")?;
    device.read_blocks(0, &mut mbr)?;
    validate_protective_mbr(&mbr, block_count)?;

    let primary = read_gpt_copy(device, 1, block_count)?;
    let backup = read_gpt_copy(device, block_count - 1, block_count)?;
    if primary.backup_lba != backup.current_lba
        || backup.backup_lba != primary.current_lba
        || primary.first_usable_lba != backup.first_usable_lba
        || primary.last_usable_lba != backup.last_usable_lba
        || primary.disk_guid != backup.disk_guid
        || primary.entry_count != backup.entry_count
        || primary.entry_size != backup.entry_size
        || primary.entries_crc != backup.entries_crc
        || primary.entries != backup.entries
    {
        return Err(Error::Corrupt("primary and backup GPT copies disagree"));
    }
    Ok(GptLayout {
        sector_size,
        block_count,
        disk_guid: primary.disk_guid,
        first_usable_lba: primary.first_usable_lba,
        last_usable_lba: primary.last_usable_lba,
        entry_count: primary.entry_count,
        entry_size: primary.entry_size,
        partitions: primary.partitions,
    })
}

/// A bounded view of a GPT partition exposing filesystem-sized blocks.
///
/// The backing device uses GPT logical sectors (commonly 512 or 4096 bytes), while
/// OpenFS can use 4 KiB through 64 KiB blocks. This adapter handles that translation.
pub struct PartitionDevice<D> {
    inner: D,
    first_lba: u64,
    sector_count: u64,
    filesystem_block_size: u32,
    filesystem_block_count: u64,
    sectors_per_filesystem_block: u64,
}

impl<D: BlockDevice> PartitionDevice<D> {
    pub fn new(
        inner: D,
        first_lba: u64,
        sector_count: u64,
        filesystem_block_size: u32,
    ) -> Result<Self> {
        let sector_size = inner.block_size();
        validate_sector_size(sector_size)?;
        validate_filesystem_block_size(filesystem_block_size, sector_size)?;
        check_block_range(first_lba, sector_count, inner.block_count())?;
        let sectors_per_filesystem_block = u64::from(filesystem_block_size / sector_size);
        if first_lba % sectors_per_filesystem_block != 0
            || sector_count == 0
            || sector_count % sectors_per_filesystem_block != 0
        {
            return Err(Error::InvalidInput("partition boundaries are not OpenFS-block aligned"));
        }
        Ok(Self {
            inner,
            first_lba,
            sector_count,
            filesystem_block_size,
            filesystem_block_count: sector_count / sectors_per_filesystem_block,
            sectors_per_filesystem_block,
        })
    }

    pub fn into_inner(self) -> D {
        self.inner
    }

    pub fn first_lba(&self) -> u64 {
        self.first_lba
    }

    pub fn sector_count(&self) -> u64 {
        self.sector_count
    }
}

impl<D: BlockDevice> BlockDevice for PartitionDevice<D> {
    fn block_size(&self) -> u32 {
        self.filesystem_block_size
    }

    fn block_count(&self) -> u64 {
        self.filesystem_block_count
    }

    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        let (first_sector, _) = self.translate_range(first_block, output.len())?;
        self.inner.read_blocks(first_sector, output)
    }

    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
        let (first_sector, _) = self.translate_range(first_block, input.len())?;
        self.inner.write_blocks(first_sector, input)
    }

    fn flush(&mut self) -> Result<()> {
        self.inner.flush()
    }
}

impl<D: BlockDevice> PartitionDevice<D> {
    fn translate_range(&self, first_block: u64, length: usize) -> Result<(u64, u64)> {
        let filesystem_block_size = self.filesystem_block_size as usize;
        if length == 0 || length % filesystem_block_size != 0 {
            return Err(Error::InvalidInput(
                "partition I/O length must be a non-zero multiple of filesystem block size",
            ));
        }
        let block_count =
            u64::try_from(length / filesystem_block_size).map_err(|_| Error::ArithmeticOverflow)?;
        check_block_range(first_block, block_count, self.filesystem_block_count)?;
        let relative_sectors = first_block
            .checked_mul(self.sectors_per_filesystem_block)
            .ok_or(Error::ArithmeticOverflow)?;
        let first_sector = self
            .first_lba
            .checked_add(relative_sectors)
            .ok_or(Error::ArithmeticOverflow)?;
        let sector_count = block_count
            .checked_mul(self.sectors_per_filesystem_block)
            .ok_or(Error::ArithmeticOverflow)?;
        if sector_count > self.sector_count.saturating_sub(relative_sectors) {
            return Err(Error::OutOfBounds {
                first_block,
                block_count,
                device_blocks: self.filesystem_block_count,
            });
        }
        Ok((first_sector, sector_count))
    }
}

fn read_gpt_copy<D: BlockDevice>(
    device: &mut D,
    header_lba: u64,
    block_count: u64,
) -> Result<GptCopy> {
    let sector_size = device.block_size();
    let mut header = try_zeroed_bytes(sector_size as usize, "unable to allocate GPT header")?;
    device.read_blocks(header_lba, &mut header)?;
    if header.get(0..8) != Some(b"EFI PART") {
        return Err(Error::Corrupt("GPT header signature is missing"));
    }
    if get_u32(&header, 8)? != 0x0001_0000 {
        return Err(Error::Corrupt("unsupported GPT revision"));
    }
    let header_size = get_u32(&header, 12)?;
    if !(GPT_HEADER_SIZE as u32..=sector_size).contains(&header_size) {
        return Err(Error::Corrupt("GPT header size is outside supported bounds"));
    }
    if get_u32(&header, 20)? != 0 {
        return Err(Error::Corrupt("GPT reserved header field is non-zero"));
    }
    let expected_header_crc = get_u32(&header, 16)?;
    let mut crc_header = header[..header_size as usize].to_vec();
    crc_header[16..20].fill(0);
    if crc32_ieee(&crc_header) != expected_header_crc {
        return Err(Error::Corrupt("GPT header CRC32 mismatch"));
    }

    let current_lba = get_u64(&header, 24)?;
    let backup_lba = get_u64(&header, 32)?;
    let first_usable_lba = get_u64(&header, 40)?;
    let last_usable_lba = get_u64(&header, 48)?;
    let disk_guid: [u8; 16] = header[56..72]
        .try_into()
        .map_err(|_| Error::Corrupt("GPT disk GUID is truncated"))?;
    let entries_lba = get_u64(&header, 72)?;
    let entry_count = get_u32(&header, 80)?;
    let entry_size = get_u32(&header, 84)?;
    let entries_crc = get_u32(&header, 88)?;

    if current_lba != header_lba
        || backup_lba >= block_count
        || backup_lba == current_lba
        || first_usable_lba > last_usable_lba
        || last_usable_lba >= block_count
        || disk_guid.iter().all(|byte| *byte == 0)
    {
        return Err(Error::Corrupt("GPT header geometry or disk GUID is invalid"));
    }
    if entry_count == 0 || entry_count > 4096 || entry_size < 128 || entry_size % 8 != 0 {
        return Err(Error::Corrupt("GPT partition-entry geometry is invalid"));
    }
    let entries_bytes = u64::from(entry_count)
        .checked_mul(u64::from(entry_size))
        .ok_or(Error::ArithmeticOverflow)?;
    if entries_bytes > GPT_MAX_ENTRY_BYTES {
        return Err(Error::Corrupt("GPT partition-entry array exceeds 16 MiB"));
    }
    let entries_blocks = div_ceil(entries_bytes, u64::from(sector_size))?;
    let entries_end = entries_lba
        .checked_add(entries_blocks)
        .ok_or(Error::ArithmeticOverflow)?;
    if header_lba == 1 {
        if backup_lba != block_count - 1
            || entries_lba < 2
            || entries_end > first_usable_lba
        {
            return Err(Error::Corrupt("primary GPT entry array is misplaced"));
        }
    } else if backup_lba != 1
        || entries_lba <= last_usable_lba
        || entries_end != header_lba
    {
        return Err(Error::Corrupt("backup GPT entry array is misplaced"));
    }

    let padded_len = usize::try_from(
        entries_blocks
            .checked_mul(u64::from(sector_size))
            .ok_or(Error::ArithmeticOverflow)?,
    )
    .map_err(|_| Error::ArithmeticOverflow)?;
    let mut padded_entries =
        try_zeroed_bytes(padded_len, "unable to allocate GPT partition array")?;
    device.read_blocks(entries_lba, &mut padded_entries)?;
    let entries_len = usize::try_from(entries_bytes).map_err(|_| Error::ArithmeticOverflow)?;
    let entries = padded_entries[..entries_len].to_vec();
    if crc32_ieee(&entries) != entries_crc {
        return Err(Error::Corrupt("GPT partition-entry array CRC32 mismatch"));
    }
    let partitions = decode_partitions(
        &entries,
        entry_count,
        entry_size,
        first_usable_lba,
        last_usable_lba,
    )?;
    Ok(GptCopy {
        current_lba,
        backup_lba,
        first_usable_lba,
        last_usable_lba,
        disk_guid,
        entry_count,
        entry_size,
        entries_crc,
        entries,
        partitions,
    })
}

fn decode_partitions(
    entries: &[u8],
    entry_count: u32,
    entry_size: u32,
    first_usable: u64,
    last_usable: u64,
) -> Result<Vec<GptPartition>> {
    let mut partitions = Vec::new();
    let mut guids = HashSet::new();
    let mut ranges = Vec::new();
    for index in 0..entry_count {
        let start = usize::try_from(u64::from(index) * u64::from(entry_size))
            .map_err(|_| Error::ArithmeticOverflow)?;
        let end = start
            .checked_add(entry_size as usize)
            .ok_or(Error::ArithmeticOverflow)?;
        let entry = entries.get(start..end).ok_or(Error::Corrupt("GPT entry is truncated"))?;
        let type_guid: [u8; 16] = entry[0..16]
            .try_into()
            .map_err(|_| Error::Corrupt("GPT type GUID is truncated"))?;
        if type_guid.iter().all(|byte| *byte == 0) {
            if entry.iter().any(|byte| *byte != 0) {
                return Err(Error::Corrupt("unused GPT entry contains non-zero bytes"));
            }
            continue;
        }
        let unique_guid: [u8; 16] = entry[16..32]
            .try_into()
            .map_err(|_| Error::Corrupt("GPT unique GUID is truncated"))?;
        if unique_guid.iter().all(|byte| *byte == 0) || !guids.insert(unique_guid) {
            return Err(Error::Corrupt("GPT partition GUID is zero or duplicated"));
        }
        let first_lba = get_u64(entry, 32)?;
        let last_lba = get_u64(entry, 40)?;
        if first_lba > last_lba || first_lba < first_usable || last_lba > last_usable {
            return Err(Error::Corrupt("GPT partition range is outside usable LBAs"));
        }
        let attributes = get_u64(entry, 48)?;
        let name_end = GPT_NAME_OFFSET + GPT_NAME_BYTES.min(entry.len().saturating_sub(GPT_NAME_OFFSET));
        let name = decode_name(&entry[GPT_NAME_OFFSET..name_end]);
        ranges.push((first_lba, last_lba));
        partitions.push(GptPartition {
            entry_index: index + 1,
            type_guid,
            unique_guid,
            first_lba,
            last_lba,
            attributes,
            name,
        });
    }
    ranges.sort_unstable();
    for pair in ranges.windows(2) {
        if pair[1].0 <= pair[0].1 {
            return Err(Error::Corrupt("GPT partitions overlap"));
        }
    }
    Ok(partitions)
}

fn validate_protective_mbr(mbr: &[u8], block_count: u64) -> Result<()> {
    if mbr.len() < 512 || mbr[510..512] != [0x55, 0xaa] {
        return Err(Error::Corrupt("protective MBR signature is missing"));
    }
    let expected_sectors = block_count.saturating_sub(1).min(u64::from(u32::MAX)) as u32;
    let mut protective_entries = 0;
    for index in 0..4 {
        let offset = 446 + index * 16;
        let entry = &mbr[offset..offset + 16];
        if entry[4] == 0 {
            if entry.iter().any(|byte| *byte != 0) {
                return Err(Error::Corrupt("empty MBR partition entry is not zero"));
            }
            continue;
        }
        if entry[4] != 0xee || entry[0] != 0
            || u32::from_le_bytes(entry[8..12].try_into().unwrap()) != 1
            || u32::from_le_bytes(entry[12..16].try_into().unwrap()) != expected_sectors
        {
            return Err(Error::Corrupt("MBR is hybrid or has invalid protective coverage"));
        }
        protective_entries += 1;
    }
    if protective_entries != 1 {
        return Err(Error::Corrupt("MBR must contain exactly one protective 0xEE entry"));
    }
    Ok(())
}

fn build_header(
    layout: &GptLayout,
    current_lba: u64,
    backup_lba: u64,
    entries_lba: u64,
    entries_crc: u32,
) -> Result<Vec<u8>> {
    let mut header = try_zeroed_bytes(
        layout.sector_size as usize,
        "unable to allocate GPT header buffer",
    )?;
    header[0..8].copy_from_slice(b"EFI PART");
    put_u32(&mut header, 8, 0x0001_0000);
    put_u32(&mut header, 12, GPT_HEADER_SIZE as u32);
    put_u64(&mut header, 24, current_lba);
    put_u64(&mut header, 32, backup_lba);
    put_u64(&mut header, 40, layout.first_usable_lba);
    put_u64(&mut header, 48, layout.last_usable_lba);
    header[56..72].copy_from_slice(&layout.disk_guid);
    put_u64(&mut header, 72, entries_lba);
    put_u32(&mut header, 80, layout.entry_count);
    put_u32(&mut header, 84, layout.entry_size);
    put_u32(&mut header, 88, entries_crc);
    let checksum = crc32_ieee(&header[..GPT_HEADER_SIZE]);
    put_u32(&mut header, 16, checksum);
    Ok(header)
}

fn validate_sector_size(sector_size: u32) -> Result<()> {
    if !matches!(sector_size, 512 | 1024 | 2048 | 4096) {
        return Err(Error::InvalidInput("GPT logical sector size must be 512, 1024, 2048 or 4096"));
    }
    Ok(())
}

fn validate_filesystem_block_size(block_size: u32, sector_size: u32) -> Result<()> {
    if !(4096..=65_536).contains(&block_size)
        || !block_size.is_power_of_two()
        || block_size % sector_size != 0
    {
        return Err(Error::InvalidInput("OpenFS block size must be 4 KiB..64 KiB and a multiple of sector size"));
    }
    Ok(())
}

fn div_ceil(value: u64, divisor: u64) -> Result<u64> {
    value
        .checked_add(divisor - 1)
        .ok_or(Error::ArithmeticOverflow)
        .map(|sum| sum / divisor)
}

fn align_up(value: u64, alignment: u64) -> Result<u64> {
    let remainder = value % alignment;
    if remainder == 0 {
        Ok(value)
    } else {
        value
            .checked_add(alignment - remainder)
            .ok_or(Error::ArithmeticOverflow)
    }
}

fn align_down(value: u64, alignment: u64) -> u64 {
    value - value % alignment
}

fn get_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        bytes
            .get(offset..offset + 4)
            .ok_or(Error::Corrupt("GPT field is truncated"))?
            .try_into()
            .map_err(|_| Error::Corrupt("GPT field is truncated"))?,
    ))
}

fn get_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        bytes
            .get(offset..offset + 8)
            .ok_or(Error::Corrupt("GPT field is truncated"))?
            .try_into()
            .map_err(|_| Error::Corrupt("GPT field is truncated"))?,
    ))
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn crc32_ieee(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    !crc
}

fn decode_name(bytes: &[u8]) -> String {
    let units = bytes
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .take_while(|unit| *unit != 0);
    char::decode_utf16(units)
        .map(|value| value.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{plan_openfs_gpt, read_gpt, write_gpt, PartitionDevice};
    use openfs_block::{BlockDevice, ImageDevice};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn image_path(label: &str) -> std::path::PathBuf {
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("openfs-gpt-{label}-{}-{stamp}.img", std::process::id()))
    }

    fn ids() -> ([u8; 16], [u8; 16]) {
        ([0x11; 16], [0x22; 16])
    }

    #[test]
    fn writes_and_reads_consistent_primary_and_backup_gpt_copies() {
        let path = image_path("roundtrip");
        let mut disk = ImageDevice::create(&path, 512, 131_072).unwrap();
        let (disk_guid, partition_guid) = ids();
        let layout = plan_openfs_gpt(&disk, 4096, disk_guid, partition_guid).unwrap();
        let partition = layout.partitions[0].clone();
        assert_eq!((partition.first_lba * 512) % (1024 * 1024), 0);
        write_gpt(&mut disk, &layout).unwrap();

        let parsed = read_gpt(&mut disk).unwrap();
        assert_eq!(parsed.disk_guid, disk_guid);
        assert_eq!(parsed.partitions, vec![partition]);
        assert_eq!(parsed.partitions[0].name, "OpenFS");
        assert!(parsed.partitions[0].is_openfs());

        drop(disk);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn writes_and_reads_gpt_with_4k_logical_sectors() {
        let path = image_path("roundtrip-4k");
        let mut disk = ImageDevice::create(&path, 4096, 16_384).unwrap();
        let (disk_guid, partition_guid) = ids();
        let layout = plan_openfs_gpt(&disk, 4096, disk_guid, partition_guid).unwrap();
        let partition = layout.partitions[0].clone();
        assert_eq!((partition.first_lba * 4096) % (1024 * 1024), 0);
        write_gpt(&mut disk, &layout).unwrap();

        let parsed = read_gpt(&mut disk).unwrap();
        assert_eq!(parsed.sector_size, 4096);
        assert_eq!(parsed.disk_guid, disk_guid);
        assert_eq!(parsed.partitions, vec![partition]);

        drop(disk);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn partition_device_maps_filesystem_blocks_and_protects_adjacent_sectors() {
        let path = image_path("partition-io");
        let disk = ImageDevice::create(&path, 512, 131_072).unwrap();
        let (disk_guid, partition_guid) = ids();
        let layout = plan_openfs_gpt(&disk, 4096, disk_guid, partition_guid).unwrap();
        let partition = layout.partitions[0].clone();
        let sector_count = partition.sector_count().unwrap();
        let mut view = PartitionDevice::new(disk, partition.first_lba, sector_count, 4096).unwrap();

        let expected = vec![0xa5; 4096];
        view.write_blocks(0, &expected).unwrap();
        let mut actual = vec![0; 4096];
        view.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, expected);
        assert!(view.read_blocks(view.block_count(), &mut actual).is_err());

        let mut disk = view.into_inner();
        let mut adjacent = vec![0xff; 512];
        disk.read_blocks(partition.first_lba - 1, &mut adjacent).unwrap();
        assert!(adjacent.iter().all(|byte| *byte == 0));
        let mut first_partition_block = vec![0; 4096];
        disk.read_blocks(partition.first_lba, &mut first_partition_block).unwrap();
        assert_eq!(first_partition_block, expected);

        drop(disk);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn rejects_a_damaged_backup_header_instead_of_trusting_primary_only() {
        let path = image_path("bad-backup");
        let mut disk = ImageDevice::create(&path, 512, 131_072).unwrap();
        let (disk_guid, partition_guid) = ids();
        let layout = plan_openfs_gpt(&disk, 4096, disk_guid, partition_guid).unwrap();
        write_gpt(&mut disk, &layout).unwrap();
        let damaged = vec![0; 512];
        disk.write_blocks(disk.block_count() - 1, &damaged).unwrap();
        assert!(read_gpt(&mut disk).is_err());

        drop(disk);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn rejects_misaligned_partition_views() {
        let path = image_path("misaligned");
        let disk = ImageDevice::create(&path, 512, 8192).unwrap();
        assert!(PartitionDevice::new(disk, 1, 4096, 4096).is_err());
        let _ = fs::remove_file(path);
    }
}
