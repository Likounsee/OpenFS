//! Fixed-width directory-entry codec for the initial OpenFS format draft.
//!
//! This is a validated record encoding only. It does not provide directory indexing,
//! persistent namespace mutation, or transaction/crash-recovery guarantees.

use crate::crc32c;
use crate::metadata::ObjectKind;
use openfs_core::{Error, Result};

pub const DIRECTORY_ENTRY_SIZE: usize = 128;
pub const DIRECTORY_NAME_MAX_BYTES: usize = 64;
const MAGIC: &[u8; 8] = b"OFSDENT\0";
const VERSION: u16 = 1;
const CHECKSUM_OFFSET: usize = 124;
const NAME_OFFSET: usize = 44;
const NAME_END: usize = NAME_OFFSET + DIRECTORY_NAME_MAX_BYTES;

/// One directory name referring to an object. Names are case-sensitive in this draft.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DirectoryEntry {
    pub parent_id: u128,
    pub object_id: u128,
    pub kind: ObjectKind,
    pub name: String,
}

/// Decode a packed inventory of fixed-width directory entries.
///
/// The byte slice must contain complete records only. Every entry is individually
/// checked for checksum, UTF-8, reserved-byte, ID, and name validity. This does not
/// infer the on-disk location or ordering of a directory index.
pub fn decode_directory_entries(bytes: &[u8]) -> Result<Vec<DirectoryEntry>> {
    if bytes.len() % DIRECTORY_ENTRY_SIZE != 0 {
        return Err(Error::InvalidInput(
            "directory inventory is not record-aligned",
        ));
    }
    bytes
        .chunks_exact(DIRECTORY_ENTRY_SIZE)
        .map(DirectoryEntry::decode)
        .collect()
}

impl DirectoryEntry {
    pub fn encode(&self) -> Result<[u8; DIRECTORY_ENTRY_SIZE]> {
        validate_name(&self.name).map_err(Error::InvalidInput)?;
        if self.parent_id == 0 || self.object_id == 0 {
            return Err(Error::InvalidInput("directory entry IDs must be nonzero"));
        }
        if self.parent_id == self.object_id {
            return Err(Error::InvalidInput(
                "directory cannot directly contain itself",
            ));
        }

        let name = self.name.as_bytes();
        let mut bytes = [0_u8; DIRECTORY_ENTRY_SIZE];
        bytes[0..8].copy_from_slice(MAGIC);
        bytes[8..10].copy_from_slice(&VERSION.to_le_bytes());
        bytes[10] = self.kind as u8;
        bytes[11] = name.len() as u8;
        bytes[12..28].copy_from_slice(&self.parent_id.to_le_bytes());
        bytes[28..44].copy_from_slice(&self.object_id.to_le_bytes());
        bytes[NAME_OFFSET..NAME_OFFSET + name.len()].copy_from_slice(name);
        let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
        bytes[CHECKSUM_OFFSET..].copy_from_slice(&checksum.to_le_bytes());
        Ok(bytes)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != DIRECTORY_ENTRY_SIZE {
            return Err(Error::InvalidInput("directory entry has an invalid length"));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad directory-entry magic"));
        }
        let version = u16::from_le_bytes([bytes[8], bytes[9]]);
        if version != VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if bytes[108..CHECKSUM_OFFSET].iter().any(|byte| *byte != 0) {
            return Err(Error::Corrupt(
                "directory-entry reserved bytes must be zero",
            ));
        }
        let stored = u32::from_le_bytes(
            bytes[CHECKSUM_OFFSET..]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid directory-entry checksum"))?,
        );
        if stored != crc32c(&bytes[..CHECKSUM_OFFSET]) {
            return Err(Error::Corrupt("directory-entry CRC32C mismatch"));
        }
        let name_len = usize::from(bytes[11]);
        if name_len == 0 || name_len > DIRECTORY_NAME_MAX_BYTES {
            return Err(Error::Corrupt("invalid directory-entry name length"));
        }
        if bytes[NAME_OFFSET + name_len..NAME_END]
            .iter()
            .any(|byte| *byte != 0)
        {
            return Err(Error::Corrupt("directory-entry name padding must be zero"));
        }
        let name = std::str::from_utf8(&bytes[NAME_OFFSET..NAME_OFFSET + name_len])
            .map_err(|_| Error::Corrupt("directory-entry name is not UTF-8"))?
            .to_owned();
        validate_name(&name).map_err(Error::Corrupt)?;
        let parent_id = u128::from_le_bytes(
            bytes[12..28]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid parent ID"))?,
        );
        let object_id = u128::from_le_bytes(
            bytes[28..44]
                .try_into()
                .map_err(|_| Error::Corrupt("invalid object ID"))?,
        );
        if parent_id == 0 || object_id == 0 || parent_id == object_id {
            return Err(Error::Corrupt("invalid directory-entry object IDs"));
        }
        let kind = ObjectKind::try_from(bytes[10])?;
        Ok(Self {
            parent_id,
            object_id,
            kind,
            name,
        })
    }
}

fn validate_name(name: &str) -> std::result::Result<(), &'static str> {
    let bytes = name.as_bytes();
    if bytes.is_empty() || bytes.len() > DIRECTORY_NAME_MAX_BYTES {
        return Err("directory name must contain 1 to 64 UTF-8 bytes");
    }
    if name == "." || name == ".." {
        return Err("dot components are reserved");
    }
    if name
        .chars()
        .any(|ch| ch == '/' || ch == '\\' || ch == '\0' || ch.is_control())
    {
        return Err("directory name contains a forbidden character");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{DirectoryEntry, DIRECTORY_ENTRY_SIZE};
    use crate::metadata::ObjectKind;
    use openfs_core::Error;

    fn sample() -> DirectoryEntry {
        DirectoryEntry {
            parent_id: 1,
            object_id: 42,
            kind: ObjectKind::File,
            name: "notes-é.txt".to_owned(),
        }
    }

    #[test]
    fn decodes_packed_directory_inventory_and_rejects_partial_records() {
        let first = sample();
        let mut second = sample();
        second.object_id = 43;
        second.name = "second".to_owned();
        let mut packed = first.encode().unwrap().to_vec();
        packed.extend_from_slice(&second.encode().unwrap());
        assert_eq!(
            super::decode_directory_entries(&packed).unwrap(),
            vec![first, second]
        );
        packed.push(0);
        assert!(matches!(
            super::decode_directory_entries(&packed),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn entry_round_trips_with_utf8_name() {
        let entry = sample();
        let bytes = entry.encode().unwrap();
        assert_eq!(bytes.len(), DIRECTORY_ENTRY_SIZE);
        assert_eq!(&bytes[..8], b"OFSDENT\0");
        assert_eq!(DirectoryEntry::decode(&bytes).unwrap(), entry);
    }

    #[test]
    fn rejects_invalid_names_and_ids() {
        for name in ["", ".", "..", "a/b", "a\\b", "bad\0name", "line\nfeed"] {
            let mut entry = sample();
            entry.name = name.to_owned();
            assert!(
                matches!(entry.encode(), Err(Error::InvalidInput(_))),
                "{name:?}"
            );
        }
        let mut entry = sample();
        entry.parent_id = 0;
        assert!(matches!(entry.encode(), Err(Error::InvalidInput(_))));
        let mut entry = sample();
        entry.object_id = entry.parent_id;
        assert!(matches!(entry.encode(), Err(Error::InvalidInput(_))));
        let mut entry = sample();
        entry.name = "é".repeat(33);
        assert!(matches!(entry.encode(), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn utf8_name_limit_is_measured_in_bytes_and_round_trips_at_boundary() {
        let mut entry = sample();
        entry.name = "é".repeat(32);
        assert_eq!(entry.name.len(), 64);
        assert_eq!(
            DirectoryEntry::decode(&entry.encode().unwrap()).unwrap(),
            entry
        );

        let mut too_long = sample();
        too_long.name = "é".repeat(33);
        assert!(matches!(
            too_long.encode(),
            Err(Error::InvalidInput(
                "directory name must contain 1 to 64 UTF-8 bytes"
            ))
        ));
    }

    #[test]
    fn decoder_rejects_invalid_ids_and_unknown_kind_after_checksum_validation() {
        let original = sample().encode().unwrap();

        let mut self_parent = original;
        self_parent[12..28].copy_from_slice(&42_u128.to_le_bytes());
        let crc = crate::crc32c(&self_parent[..124]);
        self_parent[124..128].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            DirectoryEntry::decode(&self_parent),
            Err(Error::Corrupt("invalid directory-entry object IDs"))
        ));

        let mut unknown_kind = sample().encode().unwrap();
        unknown_kind[10] = 99;
        let crc = crate::crc32c(&unknown_kind[..124]);
        unknown_kind[124..128].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            DirectoryEntry::decode(&unknown_kind),
            Err(Error::Corrupt("unknown metadata object kind"))
        ));
    }

    #[test]
    fn empty_packed_inventory_is_valid_and_partial_record_is_not() {
        assert!(super::decode_directory_entries(&[]).unwrap().is_empty());
        let entry = sample().encode().unwrap();
        assert!(matches!(
            super::decode_directory_entries(&entry[..DIRECTORY_ENTRY_SIZE - 1]),
            Err(Error::InvalidInput(
                "directory inventory is not record-aligned"
            ))
        ));
    }

    #[test]
    fn detects_checksum_padding_and_utf8_corruption() {
        let mut bytes = sample().encode().unwrap();
        bytes[10] = 99;
        let crc = crate::crc32c(&bytes[..124]);
        bytes[124..128].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            DirectoryEntry::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[110] = 1;
        let crc = crate::crc32c(&bytes[..124]);
        bytes[124..128].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            DirectoryEntry::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[44] = 0xff;
        let crc = crate::crc32c(&bytes[..124]);
        bytes[124..128].copy_from_slice(&crc.to_le_bytes());
        assert!(matches!(
            DirectoryEntry::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn name_validation_rejects_unicode_control_and_path_separators() {
        for name in [
            "tab\tname",
            "carriage\rreturn",
            "slash/name",
            "backslash\\name",
            "nul\0byte",
            "control\u{0001}byte",
        ] {
            let mut entry = sample();
            entry.name = name.to_owned();
            assert!(
                matches!(entry.encode(), Err(Error::InvalidInput(_))),
                "{name:?}"
            );
        }
    }

    #[test]
    fn decoder_rejects_zero_name_length_and_lengths_over_capacity() {
        for name_len in [0_u8, 65, 255] {
            let mut bytes = sample().encode().unwrap();
            bytes[11] = name_len;
            let crc = crate::crc32c(&bytes[..124]);
            bytes[124..128].copy_from_slice(&crc.to_le_bytes());
            assert!(
                matches!(
                    DirectoryEntry::decode(&bytes),
                    Err(Error::Corrupt("invalid directory-entry name length"))
                ),
                "name length {name_len}"
            );
        }
    }

    #[test]
    fn packed_inventory_stops_at_first_corrupt_record_without_partial_result() {
        let first = sample().encode().unwrap();
        let mut invalid = sample();
        invalid.object_id = 43;
        invalid.name = "other".to_owned();
        let mut invalid = invalid.encode().unwrap();
        invalid[44] ^= 1;
        let mut packed = first.to_vec();
        packed.extend_from_slice(&invalid);
        assert!(matches!(
            super::decode_directory_entries(&packed),
            Err(Error::Corrupt("directory-entry CRC32C mismatch"))
        ));
    }

    #[test]
    fn maximum_ascii_name_and_maximum_identifier_round_trip() {
        let mut entry = sample();
        entry.parent_id = u128::MAX;
        entry.object_id = u128::MAX - 1;
        entry.name = "x".repeat(64);
        assert_eq!(
            DirectoryEntry::decode(&entry.encode().unwrap()).unwrap(),
            entry
        );
    }
}
