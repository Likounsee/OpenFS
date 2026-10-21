//! Fixed-width metadata records for the initial on-disk format draft.
//!
//! This module defines record encoding only. It does not yet persist a namespace,
//! implement directory indexing, or provide transaction/crash-recovery guarantees.

use crate::crc32c;
use openfs_core::{Error, Result};

pub const METADATA_RECORD_SIZE: usize = 80;
const MAGIC: &[u8; 8] = b"OFSMETA\0";
const VERSION: u16 = 1;
const CHECKSUM_OFFSET: usize = 76;
const NO_EXTENT_ROOT: u64 = u64::MAX;

/// Stable 128-bit object identifier. Zero is reserved for the root's parent sentinel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ObjectId(pub u128);

impl ObjectId {
    pub fn new(value: u128) -> Result<Self> {
        if value == 0 {
            return Err(Error::InvalidInput("object ID zero is reserved"));
        }
        Ok(Self(value))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ObjectKind {
    File = 1,
    Directory = 2,
}

impl TryFrom<u8> for ObjectKind {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::File),
            2 => Ok(Self::Directory),
            _ => Err(Error::Corrupt("unknown metadata object kind")),
        }
    }
}

/// Logical metadata fields encoded explicitly; never write the Rust struct layout directly.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MetadataRecord {
    pub id: ObjectId,
    /// Zero denotes the root object's parent sentinel; other IDs must be nonzero.
    pub parent_id: u128,
    pub kind: ObjectKind,
    pub flags: u8,
    pub size: u64,
    pub extent_root_block: Option<u64>,
    pub generation: u32,
}

/// Decode a packed inventory of fixed-width metadata records.
///
/// The input must end exactly on a record boundary. This helper validates each
/// record's magic, version, reserved bytes, checksum, and semantic fields; it does
/// not assume where the inventory is stored on a volume.
pub fn decode_metadata_records(bytes: &[u8]) -> Result<Vec<MetadataRecord>> {
    if bytes.len() % METADATA_RECORD_SIZE != 0 {
        return Err(Error::InvalidInput(
            "metadata inventory is not record-aligned",
        ));
    }
    bytes
        .chunks_exact(METADATA_RECORD_SIZE)
        .map(MetadataRecord::decode)
        .collect()
}

impl MetadataRecord {
    pub fn encode(&self) -> Result<[u8; METADATA_RECORD_SIZE]> {
        ObjectId::new(self.id.0)?;
        if self.parent_id == self.id.0 {
            return Err(Error::InvalidInput("an object cannot be its own parent"));
        }
        if self.kind == ObjectKind::Directory
            && (self.size != 0 || self.extent_root_block.is_some())
        {
            return Err(Error::InvalidInput(
                "directory records cannot own file extents",
            ));
        }
        if self.extent_root_block.is_some() && self.size == 0 {
            return Err(Error::InvalidInput(
                "empty objects cannot have an extent root",
            ));
        }
        if self.extent_root_block == Some(NO_EXTENT_ROOT) {
            return Err(Error::InvalidInput(
                "extent root uses the reserved no-extent sentinel",
            ));
        }
        let mut bytes = [0_u8; METADATA_RECORD_SIZE];
        bytes[0..8].copy_from_slice(MAGIC);
        bytes[8..10].copy_from_slice(&VERSION.to_le_bytes());
        bytes[10] = self.kind as u8;
        bytes[11] = self.flags;
        bytes[12..28].copy_from_slice(&self.id.0.to_le_bytes());
        bytes[28..44].copy_from_slice(&self.parent_id.to_le_bytes());
        bytes[44..52].copy_from_slice(&self.size.to_le_bytes());
        bytes[52..60].copy_from_slice(
            &self
                .extent_root_block
                .unwrap_or(NO_EXTENT_ROOT)
                .to_le_bytes(),
        );
        bytes[60..64].copy_from_slice(&self.generation.to_le_bytes());
        let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
        bytes[CHECKSUM_OFFSET..].copy_from_slice(&checksum.to_le_bytes());
        Ok(bytes)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != METADATA_RECORD_SIZE {
            return Err(Error::InvalidInput("metadata record has an invalid length"));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad metadata record magic"));
        }
        let version = u16::from_le_bytes([bytes[8], bytes[9]]);
        if version != VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if bytes[64..76].iter().any(|byte| *byte != 0) {
            return Err(Error::Corrupt("metadata reserved bytes must be zero"));
        }
        let stored = u32::from_le_bytes(
            bytes[CHECKSUM_OFFSET..]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid metadata checksum"))?,
        );
        if stored != crc32c(&bytes[..CHECKSUM_OFFSET]) {
            return Err(Error::Corrupt("metadata CRC32C mismatch"));
        }
        let id = ObjectId::new(u128::from_le_bytes(
            bytes[12..28]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid object ID"))?,
        ))?;
        let parent_id = u128::from_le_bytes(
            bytes[28..44]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid parent ID"))?,
        );
        if parent_id == id.0 {
            return Err(Error::Corrupt("metadata object is its own parent"));
        }
        let kind = ObjectKind::try_from(bytes[10])?;
        let size = u64::from_le_bytes(
            bytes[44..52]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid object size"))?,
        );
        let extent = u64::from_le_bytes(
            bytes[52..60]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid extent root"))?,
        );
        let record = Self {
            id,
            parent_id,
            kind,
            flags: bytes[11],
            size,
            extent_root_block: (extent != NO_EXTENT_ROOT).then_some(extent),
            generation: u32::from_le_bytes(
                bytes[60..64]
                    .try_into()
                    .map_err(|_| Error::Corrupt("invalid generation"))?,
            ),
        };
        if record.kind == ObjectKind::Directory
            && (record.size != 0 || record.extent_root_block.is_some())
        {
            return Err(Error::Corrupt("directory record owns file extents"));
        }
        if record.extent_root_block.is_some() && record.size == 0 {
            return Err(Error::Corrupt("empty object has an extent root"));
        }
        Ok(record)
    }
}

#[cfg(test)]
mod tests {
    use super::{MetadataRecord, ObjectId, ObjectKind, METADATA_RECORD_SIZE};
    use openfs_core::Error;

    fn file() -> MetadataRecord {
        MetadataRecord {
            id: ObjectId(42),
            parent_id: 1,
            kind: ObjectKind::File,
            flags: 0,
            size: 4096,
            extent_root_block: Some(17),
            generation: 3,
        }
    }

    #[test]
    fn decodes_packed_metadata_inventory_and_rejects_partial_records() {
        let first = file();
        let mut second = file();
        second.id = ObjectId(43);
        second.parent_id = 1;
        let mut packed = first.encode().unwrap().to_vec();
        packed.extend_from_slice(&second.encode().unwrap());
        assert_eq!(
            super::decode_metadata_records(&packed).unwrap(),
            vec![first, second]
        );
        packed.push(0);
        assert!(matches!(
            super::decode_metadata_records(&packed),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn metadata_record_round_trips_explicitly() {
        let record = file();
        let bytes = record.encode().unwrap();
        assert_eq!(bytes.len(), METADATA_RECORD_SIZE);
        assert_eq!(&bytes[..8], b"OFSMETA\0");
        assert_eq!(MetadataRecord::decode(&bytes).unwrap(), record);
    }

    #[test]
    fn rejects_bad_checksum_reserved_bytes_and_unknown_kind() {
        let mut bytes = file().encode().unwrap();
        bytes[11] ^= 1;
        assert!(matches!(
            MetadataRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
        let mut bytes = file().encode().unwrap();
        bytes[64] = 1;
        assert!(matches!(
            MetadataRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
        let mut bytes = file().encode().unwrap();
        bytes[10] = 99;
        let crc = crate::crc32c(&bytes[..76]);
        bytes[76..80].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            MetadataRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn rejects_invalid_identity_and_directory_extents() {
        let mut record = file();
        record.id = ObjectId(0);
        assert!(matches!(record.encode(), Err(Error::InvalidInput(_))));
        let mut record = file();
        record.parent_id = record.id.0;
        assert!(matches!(record.encode(), Err(Error::InvalidInput(_))));
        let mut record = file();
        record.kind = ObjectKind::Directory;
        assert!(matches!(record.encode(), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn metadata_round_trips_root_sentinel_and_numeric_boundaries() {
        let mut record = file();
        record.id = ObjectId(u128::MAX);
        record.parent_id = 0;
        record.flags = u8::MAX;
        record.generation = u32::MAX;
        record.extent_root_block = Some(u64::MAX - 1);
        assert_eq!(
            MetadataRecord::decode(&record.encode().unwrap()).unwrap(),
            record
        );

        let mut empty = file();
        empty.size = 0;
        empty.extent_root_block = None;
        assert_eq!(
            MetadataRecord::decode(&empty.encode().unwrap()).unwrap(),
            empty
        );
    }

    #[test]
    fn rejects_reserved_extent_root_sentinel() {
        let mut record = file();
        record.extent_root_block = Some(u64::MAX);
        assert!(matches!(
            record.encode(),
            Err(Error::InvalidInput(
                "extent root uses the reserved no-extent sentinel"
            ))
        ));
    }

    #[test]
    fn decoder_rejects_semantically_invalid_records_with_valid_checksum() {
        let mut bytes = file().encode().unwrap();
        bytes[28..44].copy_from_slice(&42_u128.to_le_bytes());
        let checksum = crate::crc32c(&bytes[..76]);
        bytes[76..80].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            MetadataRecord::decode(&bytes),
            Err(Error::Corrupt("metadata object is its own parent"))
        ));

        let mut bytes = file().encode().unwrap();
        bytes[10] = ObjectKind::Directory as u8;
        let checksum = crate::crc32c(&bytes[..76]);
        bytes[76..80].copy_from_slice(&checksum.to_le_bytes());
        assert!(matches!(
            MetadataRecord::decode(&bytes),
            Err(Error::Corrupt("directory record owns file extents"))
        ));
    }
}
