//! Byte-level codec for OpenFS write-ahead journal records.
//!
//! This module validates individual records and builds a fail-closed recovery plan.
//! It does not scan persistent storage, write a WAL, apply redo, or provide crash recovery.

use openfs_core::{Error, Result};

#[cfg(test)]
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

        let mut bytes = try_zeroed_vec(total_size, "unable to allocate encoded journal record")?;
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
            return Err(Error::InvalidInput(
                "journal record is shorter than its header",
            ));
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
            return Err(Error::Corrupt(
                "journal payload exceeds the supported limit",
            ));
        }
        let expected_size = JOURNAL_HEADER_SIZE
            .checked_add(payload_len)
            .ok_or(Error::ArithmeticOverflow)?;
        if bytes.len() != expected_size {
            return Err(Error::InvalidInput(
                "journal record length does not match its header",
            ));
        }
        if get_u32(bytes, CHECKSUM_OFFSET) != record_checksum(bytes) {
            return Err(Error::Corrupt("journal record CRC32C mismatch"));
        }

        let mut payload = Vec::new();
        payload
            .try_reserve_exact(payload_len)
            .map_err(|_| Error::InvalidInput("unable to allocate decoded journal payload"))?;
        payload.extend_from_slice(&bytes[JOURNAL_HEADER_SIZE..]);
        let record = Self {
            kind: JournalRecordKind::try_from(get_u16(bytes, 12))?,
            sequence: get_u64(bytes, 16),
            transaction_id: get_u64(bytes, 24),
            payload,
        };
        validate_record(&record)?;
        Ok(record)
    }

    /// Encode a record into one fixed-size block with zero-filled trailing padding.
    pub fn encode_block(&self, block_size: usize) -> Result<Vec<u8>> {
        if block_size < JOURNAL_HEADER_SIZE {
            return Err(Error::InvalidInput(
                "journal block is smaller than the record header",
            ));
        }
        let encoded = self.encode()?;
        if encoded.len() > block_size {
            return Err(Error::InvalidInput(
                "journal record does not fit in one journal block",
            ));
        }
        let mut block = try_zeroed_vec(block_size, "unable to allocate journal block")?;
        block[..encoded.len()].copy_from_slice(&encoded);
        Ok(block)
    }

    /// Decode one block-framed record and require all bytes after it to be zero.
    pub fn decode_block(block: &[u8]) -> Result<Self> {
        if block.len() < JOURNAL_HEADER_SIZE {
            return Err(Error::InvalidInput(
                "journal block is smaller than the record header",
            ));
        }
        if &block[0..8] != MAGIC {
            return Err(Error::Corrupt("bad journal record magic"));
        }
        let payload_len = get_u32(block, 32) as usize;
        if payload_len > JOURNAL_MAX_PAYLOAD_SIZE {
            return Err(Error::Corrupt(
                "journal payload exceeds the supported limit",
            ));
        }
        let record_size = JOURNAL_HEADER_SIZE
            .checked_add(payload_len)
            .ok_or(Error::ArithmeticOverflow)?;
        if record_size > block.len() {
            return Err(Error::InvalidInput(
                "journal record does not fit in its block",
            ));
        }
        if block[record_size..].iter().any(|byte| *byte != 0) {
            return Err(Error::Corrupt("journal block padding must be zero"));
        }
        Self::decode(&block[..record_size])
    }
}

/// One fully committed transaction selected for redo during recovery planning.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CommittedTransaction {
    /// The transaction's non-zero, monotonically increasing identifier.
    pub transaction_id: u64,
    /// Redo payloads in journal order. Their interpretation belongs to the caller.
    pub updates: Vec<Vec<u8>>,
}

/// Validate an ordered stream after a durable checkpoint and select committed redo.
///
/// The first record must have sequence `checkpoint_sequence + 1`; every subsequent
/// record must be contiguous. The draft protocol permits one serialized writer, so
/// transactions cannot interleave. A final transaction without a terminal COMMIT or
/// ABORT is treated as uncommitted and omitted. Malformed ordering, sequence gaps,
/// reused/non-increasing transaction IDs, or an incomplete transaction followed by
/// more records are corruption. The caller must still scan persistent storage,
/// distinguish a torn final record, and apply returned updates idempotently.
pub fn plan_recovery(
    records: &[JournalRecord],
    checkpoint_sequence: u64,
) -> Result<Vec<CommittedTransaction>> {
    let mut expected_sequence = checkpoint_sequence
        .checked_add(1)
        .ok_or(Error::ArithmeticOverflow)?;
    let mut last_transaction_id = 0_u64;
    let mut active: Option<CommittedTransaction> = None;
    let mut completed = Vec::new();

    for (index, record) in records.iter().enumerate() {
        validate_record(record)?;
        if record.sequence != expected_sequence {
            return Err(Error::Corrupt(
                "journal sequence is not contiguous after checkpoint",
            ));
        }
        expected_sequence = expected_sequence
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;

        match record.kind {
            JournalRecordKind::Begin => {
                if active.is_some() || record.transaction_id <= last_transaction_id {
                    return Err(Error::Corrupt("invalid or interleaved journal transaction"));
                }
                last_transaction_id = record.transaction_id;
                active = Some(CommittedTransaction {
                    transaction_id: record.transaction_id,
                    updates: Vec::new(),
                });
            }
            JournalRecordKind::Update => {
                let transaction = active
                    .as_mut()
                    .ok_or(Error::Corrupt("journal UPDATE without BEGIN"))?;
                if transaction.transaction_id != record.transaction_id {
                    return Err(Error::Corrupt("journal UPDATE transaction ID mismatch"));
                }
                transaction.updates.try_reserve(1).map_err(|_| {
                    Error::InvalidInput("unable to reserve journal recovery updates")
                })?;
                let mut payload = Vec::new();
                payload
                    .try_reserve_exact(record.payload.len())
                    .map_err(|_| Error::InvalidInput("unable to copy journal recovery payload"))?;
                payload.extend_from_slice(&record.payload);
                transaction.updates.push(payload);
            }
            JournalRecordKind::Commit => {
                let transaction = active
                    .take()
                    .ok_or(Error::Corrupt("journal COMMIT without BEGIN"))?;
                if transaction.transaction_id != record.transaction_id {
                    return Err(Error::Corrupt("journal COMMIT transaction ID mismatch"));
                }
                if transaction.updates.is_empty() {
                    return Err(Error::Corrupt(
                        "committed journal transaction contains no redo updates",
                    ));
                }
                completed
                    .try_reserve(1)
                    .map_err(|_| Error::InvalidInput("unable to reserve recovered transactions"))?;
                completed.push(transaction);
            }
            JournalRecordKind::Abort => {
                let transaction = active
                    .take()
                    .ok_or(Error::Corrupt("journal ABORT without BEGIN"))?;
                if transaction.transaction_id != record.transaction_id {
                    return Err(Error::Corrupt("journal ABORT transaction ID mismatch"));
                }
            }
        }

        if let Some(transaction) = active.as_ref() {
            if index + 1 < records.len() {
                let next = &records[index + 1];
                if next.kind == JournalRecordKind::Begin
                    || next.transaction_id != transaction.transaction_id
                {
                    return Err(Error::Corrupt(
                        "incomplete journal transaction is not at stream tail",
                    ));
                }
            }
        }
    }

    // Any transaction still active at the end lacks a durable terminal record.
    Ok(completed)
}

fn try_zeroed_vec(length: usize, allocation_error: &'static str) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(length)
        .map_err(|_| Error::InvalidInput(allocation_error))?;
    bytes.resize(length, 0);
    Ok(bytes)
}

fn validate_record(record: &JournalRecord) -> Result<()> {
    if record.sequence == 0 {
        return Err(Error::InvalidInput("journal sequence must be non-zero"));
    }
    if record.transaction_id == 0 {
        return Err(Error::InvalidInput(
            "journal transaction ID must be non-zero",
        ));
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
    // Hash the header before the checksum and the payload directly. Building a
    // temporary concatenation here would allocate and copy every journal payload
    // once during both encoding and decoding.
    let mut crc = !0_u32;
    for byte in bytes[..CHECKSUM_OFFSET]
        .iter()
        .chain(bytes[JOURNAL_HEADER_SIZE..].iter())
    {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0_u32.wrapping_sub(crc & 1);
            crc = (crc >> 1) ^ (0x82F6_3B78 & mask);
        }
    }
    !crc
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
        plan_recovery, CommittedTransaction, JournalRecord, JournalRecordKind, JOURNAL_HEADER_SIZE,
        JOURNAL_MAX_PAYLOAD_SIZE, JOURNAL_VERSION,
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
    fn streaming_checksum_matches_shared_crc32c_for_varied_payload_lengths() {
        for payload_len in [0, 1, 2, 7, 31, 255, 4096, JOURNAL_MAX_PAYLOAD_SIZE] {
            let payload = (0..payload_len)
                .map(|index| (index.wrapping_mul(37) & 0xff) as u8)
                .collect::<Vec<_>>();
            let record = sample(JournalRecordKind::Update, &payload);
            let encoded = record.encode().unwrap();

            let mut reference_input = Vec::with_capacity(36 + payload.len());
            reference_input.extend_from_slice(&encoded[..36]);
            reference_input.extend_from_slice(&encoded[JOURNAL_HEADER_SIZE..]);
            assert_eq!(
                super::record_checksum(&encoded),
                super::crc32c(&reference_input),
                "CRC mismatch for payload length {payload_len}"
            );
            assert_eq!(JournalRecord::decode(&encoded).unwrap(), record);
        }
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
            assert_eq!(
                &encoded[10..12],
                &(JOURNAL_HEADER_SIZE as u16).to_le_bytes()
            );
            assert_eq!(JournalRecord::decode(&encoded).unwrap(), record);
        }
    }

    #[test]
    fn block_framing_round_trips_and_validates_padding() {
        let expected = sample(JournalRecordKind::Update, b"redo bytes");
        let block = expected.encode_block(4096).unwrap();
        assert_eq!(block.len(), 4096);
        assert_eq!(JournalRecord::decode_block(&block).unwrap(), expected);

        let mut nonzero_padding = block;
        *nonzero_padding.last_mut().unwrap() = 1;
        assert!(matches!(
            JournalRecord::decode_block(&nonzero_padding),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn block_framing_rejects_small_blocks_and_oversized_records() {
        let record = sample(JournalRecordKind::Update, b"redo");
        assert!(matches!(
            record.encode_block(JOURNAL_HEADER_SIZE - 1),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            record.encode_block(JOURNAL_HEADER_SIZE + 3),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            JournalRecord::decode_block(&[0; JOURNAL_HEADER_SIZE - 1]),
            Err(Error::InvalidInput(_))
        ));
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
        let mut bytes = sample(JournalRecordKind::Update, b"payload")
            .encode()
            .unwrap();
        bytes[40] ^= 1;
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[0] ^= 1;
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

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
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn rejects_unknown_kind_and_invalid_identifiers() {
        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[12..14].copy_from_slice(&99_u16.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[16..24].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::InvalidInput(_))
        ));

        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[24..32].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::InvalidInput(_))
        ));
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

    fn record(
        kind: JournalRecordKind,
        sequence: u64,
        transaction_id: u64,
        payload: &[u8],
    ) -> JournalRecord {
        JournalRecord {
            kind,
            sequence,
            transaction_id,
            payload: payload.to_vec(),
        }
    }

    #[test]
    fn recovery_plan_replays_only_committed_transactions_in_order() {
        let records = vec![
            record(JournalRecordKind::Begin, 11, 4, b""),
            record(JournalRecordKind::Update, 12, 4, b"redo-a"),
            record(JournalRecordKind::Update, 13, 4, b"redo-b"),
            record(JournalRecordKind::Commit, 14, 4, b""),
            record(JournalRecordKind::Begin, 15, 5, b""),
            record(JournalRecordKind::Update, 16, 5, b"redo-uncommitted"),
        ];
        let plan = plan_recovery(&records, 10).unwrap();
        assert_eq!(
            plan,
            vec![CommittedTransaction {
                transaction_id: 4,
                updates: vec![b"redo-a".to_vec(), b"redo-b".to_vec()],
            }]
        );
    }

    #[test]
    fn recovery_plan_rejects_committed_transaction_without_updates() {
        let records = vec![
            record(JournalRecordKind::Begin, 1, 1, b""),
            record(JournalRecordKind::Commit, 2, 1, b""),
        ];
        assert!(matches!(
            plan_recovery(&records, 0),
            Err(Error::Corrupt(
                "committed journal transaction contains no redo updates"
            ))
        ));
    }

    #[test]
    fn recovery_plan_discards_aborted_transactions() {
        let records = vec![
            record(JournalRecordKind::Begin, 1, 1, b""),
            record(JournalRecordKind::Update, 2, 1, b"must-not-replay"),
            record(JournalRecordKind::Abort, 3, 1, b""),
        ];
        assert!(plan_recovery(&records, 0).unwrap().is_empty());
    }

    #[test]
    fn recovery_plan_fails_closed_on_sequence_gaps_and_bad_order() {
        let gap = vec![
            record(JournalRecordKind::Begin, 1, 1, b""),
            record(JournalRecordKind::Commit, 3, 1, b""),
        ];
        assert!(matches!(plan_recovery(&gap, 0), Err(Error::Corrupt(_))));

        let update_without_begin = vec![record(JournalRecordKind::Update, 1, 1, b"x")];
        assert!(matches!(
            plan_recovery(&update_without_begin, 0),
            Err(Error::Corrupt(_))
        ));

        let mismatched = vec![
            record(JournalRecordKind::Begin, 1, 1, b""),
            record(JournalRecordKind::Commit, 2, 2, b""),
        ];
        assert!(matches!(
            plan_recovery(&mismatched, 0),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn recovery_plan_requires_incomplete_transaction_at_the_tail() {
        let records = vec![
            record(JournalRecordKind::Begin, 1, 1, b""),
            record(JournalRecordKind::Update, 2, 1, b"redo"),
            record(JournalRecordKind::Begin, 3, 2, b""),
        ];
        assert!(matches!(plan_recovery(&records, 0), Err(Error::Corrupt(_))));
    }

    #[test]
    fn rejects_nonempty_control_record_payload_after_checksum_validation() {
        let mut bytes = sample(JournalRecordKind::Begin, b"").encode().unwrap();
        bytes[32..36].copy_from_slice(&1_u32.to_le_bytes());
        bytes.push(b'x');
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalRecord::decode(&bytes),
            Err(Error::InvalidInput(_))
        ));
    }
}
