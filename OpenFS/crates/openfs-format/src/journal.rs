//! Byte-level codec for OpenFS write-ahead journal records.
//!
//! This module validates individual records only. It does not implement ordering,
//! transaction assembly, persistence barriers, replay, or crash recovery.

use openfs_core::{Error, Result};

use crate::crc32c;

/// Journal record framing version.
pub const JOURNAL_VERSION: u16 = 1;
/// Fixed-size header before the payload.
pub const JOURNAL_HEADER_SIZE: usize = 40;
/// Largest payload accepted by this experimental codec (1 MiB).
pub const JOURNAL_MAX_PAYLOAD_SIZE: usize = 1024 * 1024;
const MAGIC: &[u8; 8] = b"OFSJNL\0\0";
const CHECKSUM_OFFSET: usize = 36;

/// Record kinds currently defined by the draft journal protocol.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum JournalRecordKind {
    /// Starts a transaction. Payload must be empty.
    Begin = 1,
    /// Carries opaque redo information for a transaction.
    Update = 2,
    /// Marks the transaction as committed. Payload must be empty.
    Commit = 3,
    /// Marks a transaction as aborted. Payload must be empty.
    Abort = 4,
}

impl TryFrom<u16> for JournalRecordKind {
    type Error = Error;

    fn try_from(value: u16) -> Result<Self> {
        match value {
            1 => Ok(Self::Begin),
            2 => Ok(Self::Update),
            3 => Ok(Self::Commit),
            4 => Ok(Self::Abort),
            _ => Err(Error::Corrupt("unknown journal record kind")),
        }
    }
}

/// A decoded journal record. Sequence and transaction identifiers are non-zero.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JournalRecord {
    pub kind: JournalRecordKind,
    pub sequence: u64,
    pub transaction_id: u64,
    pub payload: Vec<u8>,
}

impl JournalRecord {
    /// Encode one complete record with explicit little-endian fields and CRC32C.
    pub fn encode(&self) -> Result<Vec<u8>> {
        validate_record(self)?;
        let total_size = JOURNAL_HEADER_SIZE
            .checked_add(self.payload.len())
            .ok_or(Error::ArithmeticOverflow)?;
        let payload_len = u32::try_from(self.payload.len())
            .map_err(|_| Error::InvalidInput("journal payload is too large"))?;

        let mut bytes = vec![0_u8; total_size];
        bytes[0..8].copy_from_slice(MAGIC);
        put_u16(&mut bytes, 8, JOURNAL_VERSION);
        put_u16(&mut bytes, 10, JOURNAL_HEADER_SIZE as u16);
        put_u16(&mut bytes, 12, self.kind as u16);
        // bytes 14..16 are reserved flags and remain zero.
        put_u64(&mut bytes, 16, self.sequence);
        put_u64(&mut bytes, 24, self.transaction_id);
        put_u32(&mut bytes, 32, payload_len);
        // bytes 36..40 hold the checksum and are zero while calculating it.
        bytes[JOURNAL_HEADER_SIZE..].copy_from_slice(&self.payload);
        let checksum = record_checksum(&bytes);
        put_u32(&mut bytes, CHECKSUM_OFFSET, checksum);
        Ok(bytes)
    }

    /// Decode exactly one complete record, rejecting truncation and trailing data.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < JOURNAL_HEADER_SIZE {
            return Err(Error::InvalidInput("journal record is shorter than its header"));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad journal record magic"));
        }
        let version = get_u16(bytes, 8);
        if version != JOURNAL_VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if get_u16(bytes, 10) as usize != JOURNAL_HEADER_SIZE {
            return Err(Error::Corrupt("unexpected journal header size"));
        }
        if get_u16(bytes, 14) != 0 {
            return Err(Error::Corrupt("journal reserved flags must be zero"));
        }

        let payload_len = get_u32(bytes, 32) as usize;
        if payload_len > JOURNAL_MAX_PAYLOAD_SIZE {
            return Err(Error::Corrupt("journal payload exceeds the supported limit"));
        }
        let expected_size = JOURNAL_HEADER_SIZE
            .checked_add(payload_len)
            .ok_or(Error::ArithmeticOverflow)?;
        if bytes.len() != expected_size {
            return Err(Error::InvalidInput("journal record length does not match its header"));
        }
        if get_u32(bytes, CHECKSUM_OFFSET) != record_checksum(bytes) {
            return Err(Error::Corrupt("journal record CRC32C mismatch"));
        }

        let record = Self {
            kind: JournalRecordKind::try_from(get_u16(bytes, 12))?,
            sequence: get_u64(bytes, 16),
            transaction_id: get_u64(bytes, 24),
            payload: bytes[JOURNAL_HEADER_SIZE..].to_vec(),
        };
        validate_record(&record)?;
        Ok(record)
    }
}

fn validate_record(record: &JournalRecord) -> Result<()> {
    if record.sequence == 0 {
        return Err(Error::InvalidInput("journal sequence must be non-zero"));
    }
    if record.transaction_id == 0 {
        return Err(Error::InvalidInput("journal transaction ID must be non-zero"));
    }
    if record.payload.len() > JOURNAL_MAX_PAYLOAD_SIZE {
        return Err(Error::InvalidInput(
            "journal payload exceeds the supported limit",
        ));
    }
    if record.kind != JournalRecordKind::Update && !record.payload.is_empty() {
        return Err(Error::InvalidInput(
            "begin, commit, and abort journal records must have empty payloads",
        ));
    }
    Ok(())
}

fn record_checksum(bytes: &[u8]) -> u32 {
    let mut crc_input = Vec::with_capacity(bytes.len().saturating_sub(4));
    crc_input.extend_from_slice(&bytes[..CHECKSUM_OFFSET]);
    crc_input.extend_from_slice(&bytes[JOURNAL_HEADER_SIZE..]);
    crc32c(&crc_input)
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

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

#[cfg(test)]
mod tests {
    use super::{
        JournalRecord, JournalRecordKind, JOURNAL_HEADER_SIZE, JOURNAL_MAX_PAYLOAD_SIZE,
        JOURNAL_VERSION,
    };
    use openfs_core::Error;

    fn sample(kind: JournalRecordKind, payload: &[u8]) -> JournalRecord {
        JournalRecord {
            kind,
            sequence: 42,
            transaction_id: 7,
            payload: payload.to_vec(),
        }
    }

    fn refresh_checksum(bytes: &mut [u8]) {
        let mut input = Vec::new();
        input.extend_from_slice(&bytes[..36]);
        input.extend_from_slice(&bytes[JOURNAL_HEADER_SIZE..]);
        let checksum = super::crc32c(&input);
        bytes[36..40].copy_from_slice(&checksum.to_le_bytes());
    }

    #[test]
    fn round_trips_all_record_kinds() {
        for (kind, payload) in [
            (JournalRecordKind::Begin, &[][..]),
            (JournalRecordKind::Update, b"redo payload".as_slice()),
            (JournalRecordKind::Commit, &[][..]),
            (JournalRecordKind::Abort, &[][..]),
        ] {
            let record = sample(kind, payload);
            let encoded = record.encode().unwrap();
            assert_eq!(&encoded[..8], b"OFSJNL\0\0");
            assert_eq!(&encoded[8..10], &JOURNAL_VERSION.to_le_bytes());
            assert_eq!(&encoded[10..12], &(JOURNAL_HEADER_SIZE as u16).to_le_bytes());
            assert_eq!(JournalRecord::decode(&encoded).unwrap(), record);
        }
    }

    #[test]
    fn rejects_truncated_and_trailing_data() {
        let encoded = sample(JournalRecordKind::Update, b"abc").encode().unwrap();
        assert!(matches!(
            JournalRecord::decode(&encoded[..JOURNAL_HEADER_SIZE - 1]),
            Err(Error::InvalidInput(_))
        ));
        let mut trailing = encoded;
        trailing.push(0);
        assert!(matches!(
            JournalRecord::decode(&trailing),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn rejects_checksum_magic_version_and_reserved_field_corruption() {
        let mut bytes = sample(JournalRecordKind::Update, b"payload").encode().unwrap();
        bytes[40] ^= 1;
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::Corrupt(_))));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[0] ^= 1;
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::Corrupt(_))));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[8..10].copy_from_slice(&(JOURNAL_VERSION + 1).to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::UnsupportedVersion(_))
        ));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[14] = 1;
        refresh_checksum(&mut bytes);
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::Corrupt(_))));
    }

    #[test]
    fn rejects_unknown_kind_and_invalid_identifiers() {
        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[12..14].copy_from_slice(&99_u16.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::Corrupt(_))));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[16..24].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::InvalidInput(_))));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[24..32].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn enforces_payload_rules_and_limit() {
        let invalid = JournalRecord {
            payload: b"not allowed".to_vec(),
            ..sample(JournalRecordKind::Commit, b"")
        };
        assert!(matches!(invalid.encode(), Err(Error::InvalidInput(_))));

        let too_large = JournalRecord {
            payload: vec![0; JOURNAL_MAX_PAYLOAD_SIZE + 1],
            ..sample(JournalRecordKind::Update, b"")
        };
        assert!(matches!(too_large.encode(), Err(Error::InvalidInput(_))));

        let valid_max = JournalRecord {
            payload: vec![0x5a; JOURNAL_MAX_PAYLOAD_SIZE],
            ..sample(JournalRecordKind::Update, b"")
        };
        let bytes = valid_max.encode().unwrap();
        assert_eq!(JournalRecord::decode(&bytes).unwrap(), valid_max);
    }

    #[test]
    fn rejects_nonempty_control_record_payload_after_checksum_validation() {
        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[32..36].copy_from_slice(&1_u32.to_le_bytes());
        bytes.push(b'x');
        refresh_checksum(&mut bytes);
        assert!(matches!(JournalRecord::decode(&bytes), Err(Error::InvalidInput(_))));
    }
}
