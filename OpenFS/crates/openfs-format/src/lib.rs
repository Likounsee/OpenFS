//! Canonical on-disk format and filesystem primitives for OpenFS.

/// Size of the fixed superblock record in bytes.
pub const SUPERBLOCK_SIZE: usize = 4096;

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
mod metadata;
mod namespace;
mod verifier;
mod volume;

pub use allocator_extents::{
    AllocationBitmapReport, AllocationOwnershipReport, AllocationStats, FreeExtent,
    FreeExtentStats, OWNERSHIP_DIAGNOSTIC_SAMPLE_LIMIT,
};
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
pub use metadata::{
    decode_metadata_records, MetadataRecord, ObjectId, ObjectKind, METADATA_RECORD_SIZE,
};
pub use namespace::Namespace;
pub use verifier::PersistentInventoryReport;
pub use volume::{
    check_volume, format_volume, open_volume, OpenedVolume, Volume, VolumeCheckReport,
    VolumeLayout,
};
