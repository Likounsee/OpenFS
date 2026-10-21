//! Byte-level codec for the persisted journal checkpoint record.
//!
//! This is only a validated checkpoint value. It does not read or write a block,
//! reserve journal space, or provide transaction recovery by itself.

use openfs_core::{Error, Result};

use crate::crc32c;

/// Encoded checkpoint record size. The containing block must be zero-padded.
pub const JOURNAL_CHECKPOINT_SIZE: usize = 72;
/// Checkpoint format version.
pub const JOURNAL_CHECKPOINT_VERSION: u16 = 1;
const MAGIC: &[u8; 8] = b"OFSCKP\0\0";
const CHECKSUM_OFFSET: usize = 64;

/// Durable journal position metadata. Ring positions are block offsets relative to
/// the journal data region; their bounds are validated by the journal layout owner.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct JournalCheckpoint {
    /// Increases on every persisted checkpoint update.
    pub generation: u64,
    /// Highest record sequence known to be fully applied to home blocks.
    pub checkpoint_sequence: u64,
    /// Sequence to assign to the next record.
    pub next_sequence: u64,
    /// Identifier to assign to the next transaction.
    pub next_transaction_id: u64,
    /// Ring write position, in blocks relative to the journal data region.
    pub head_block: u64,
    /// Oldest retained ring position, in blocks relative to the journal data region.
    pub tail_block: u64,
}

impl JournalCheckpoint {
    /// Encode the 72-byte little-endian checkpoint record with CRC32C.
    pub fn encode(&self) -> Result<[u8; JOURNAL_CHECKPOINT_SIZE]> {
        self.validate()?;
        let mut bytes = [0_u8; JOURNAL_CHECKPOINT_SIZE];
        bytes[0..8].copy_from_slice(MAGIC);
        put_u16(&mut bytes, 8, JOURNAL_CHECKPOINT_VERSION);
        put_u16(&mut bytes, 10, JOURNAL_CHECKPOINT_SIZE as u16);
        // bytes 12..16 are reserved flags and remain zero.
        put_u64(&mut bytes, 16, self.generation);
        put_u64(&mut bytes, 24, self.checkpoint_sequence);
        put_u64(&mut bytes, 32, self.next_sequence);
        put_u64(&mut bytes, 40, self.next_transaction_id);
        put_u64(&mut bytes, 48, self.head_block);
        put_u64(&mut bytes, 56, self.tail_block);
        let checksum = crc32c(&bytes[..CHECKSUM_OFFSET]);
        put_u32(&mut bytes, CHECKSUM_OFFSET, checksum);
        // bytes 68..72 are reserved tail and remain zero.
        Ok(bytes)
    }

    /// Decode one checkpoint record. The caller must separately validate zero padding
    /// in the rest of the containing block and validate ring positions against geometry.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        if bytes.len() != JOURNAL_CHECKPOINT_SIZE {
            return Err(Error::InvalidInput(
                "journal checkpoint must be exactly 72 bytes",
            ));
        }
        if &bytes[0..8] != MAGIC {
            return Err(Error::Corrupt("bad journal checkpoint magic"));
        }
        let version = get_u16(bytes, 8);
        if version != JOURNAL_CHECKPOINT_VERSION {
            return Err(Error::UnsupportedVersion(version));
        }
        if get_u16(bytes, 10) as usize != JOURNAL_CHECKPOINT_SIZE {
            return Err(Error::Corrupt("unexpected journal checkpoint size"));
        }
        if get_u32(bytes, 12) != 0 || get_u32(bytes, 68) != 0 {
            return Err(Error::Corrupt(
                "journal checkpoint reserved bytes must be zero",
            ));
        }
        if get_u32(bytes, CHECKSUM_OFFSET) != crc32c(&bytes[..CHECKSUM_OFFSET]) {
            return Err(Error::Corrupt("journal checkpoint CRC32C mismatch"));
        }
        let checkpoint = Self {
            generation: get_u64(bytes, 16),
            checkpoint_sequence: get_u64(bytes, 24),
            next_sequence: get_u64(bytes, 32),
            next_transaction_id: get_u64(bytes, 40),
            head_block: get_u64(bytes, 48),
            tail_block: get_u64(bytes, 56),
        };
        // Encoding rejects caller-supplied invalid state; decoding persisted bytes
        // must report semantic corruption, not an invalid API argument.
        if checkpoint.checkpoint_sequence.checked_add(1) != Some(checkpoint.next_sequence) {
            return Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree",
            ));
        }
        checkpoint
            .validate()
            .map_err(|_| Error::Corrupt("invalid journal checkpoint state"))?;
        Ok(checkpoint)
    }

    fn validate(&self) -> Result<()> {
        if self.generation == 0 {
            return Err(Error::InvalidInput(
                "journal checkpoint generation must be non-zero",
            ));
        }
        if self.checkpoint_sequence.checked_add(1) != Some(self.next_sequence) {
            return Err(Error::InvalidInput(
                "next journal sequence must immediately follow checkpoint sequence",
            ));
        }
        if self.next_transaction_id == 0 {
            return Err(Error::InvalidInput(
                "next journal transaction ID must be non-zero",
            ));
        }
        // Every consumed transaction ID requires at least one BEGIN record.
        // A checkpoint cannot therefore claim more consumed IDs than the number
        // of journal records covered by its durable sequence.
        let maximum_next_transaction_id = self
            .checkpoint_sequence
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        if self.next_transaction_id > maximum_next_transaction_id {
            return Err(Error::InvalidInput(
                "next transaction ID exceeds the checkpointed journal history",
            ));
        }
        Ok(())
    }
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
    use super::{JournalCheckpoint, JOURNAL_CHECKPOINT_SIZE, JOURNAL_CHECKPOINT_VERSION};
    use openfs_core::Error;

    fn sample() -> JournalCheckpoint {
        JournalCheckpoint {
            generation: 3,
            checkpoint_sequence: 8,
            next_sequence: 9,
            next_transaction_id: 5,
            head_block: 12,
            tail_block: 4,
        }
    }

    fn refresh_checksum(bytes: &mut [u8; JOURNAL_CHECKPOINT_SIZE]) {
        let checksum = super::crc32c(&bytes[..64]);
        bytes[64..68].copy_from_slice(&checksum.to_le_bytes());
    }

    #[test]
    fn checkpoint_round_trip_uses_explicit_little_endian_fields() {
        let expected = sample();
        let bytes = expected.encode().unwrap();
        assert_eq!(&bytes[..8], b"OFSCKP\0\0");
        assert_eq!(&bytes[8..10], &JOURNAL_CHECKPOINT_VERSION.to_le_bytes());
        assert_eq!(&bytes[10..12], &72_u16.to_le_bytes());
        assert_eq!(&bytes[16..24], &3_u64.to_le_bytes());
        assert_eq!(&bytes[24..32], &8_u64.to_le_bytes());
        assert_eq!(&bytes[32..40], &9_u64.to_le_bytes());
        assert_eq!(&bytes[40..48], &5_u64.to_le_bytes());
        assert_eq!(&bytes[48..56], &12_u64.to_le_bytes());
        assert_eq!(&bytes[56..64], &4_u64.to_le_bytes());
        assert_eq!(JournalCheckpoint::decode(&bytes).unwrap(), expected);
    }

    #[test]
    fn rejects_bad_magic_version_checksum_and_reserved_bytes() {
        let mut bytes = sample().encode().unwrap();
        bytes[0] ^= 1;
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[8..10].copy_from_slice(&(JOURNAL_CHECKPOINT_VERSION + 1).to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::UnsupportedVersion(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[12] = 1;
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[68] = 1;
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(_))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[24] ^= 1;
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn rejects_transaction_id_ahead_of_checkpointed_history() {
        let mut bytes = sample().encode().unwrap();
        // Sequence eight can cover at most eight BEGIN records, so the next ID
        // cannot exceed nine even if every covered record began a new transaction.
        bytes[40..48].copy_from_slice(&10_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt("invalid journal checkpoint state"))
        ));

        let impossible = JournalCheckpoint {
            generation: 1,
            checkpoint_sequence: 0,
            next_sequence: 1,
            next_transaction_id: 2,
            head_block: 0,
            tail_block: 0,
        };
        assert!(matches!(impossible.encode(), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn rejects_sequence_gaps_even_when_next_sequence_is_greater() {
        let mut checkpoint = sample();
        checkpoint.next_sequence = checkpoint.checkpoint_sequence + 2;
        assert!(matches!(checkpoint.encode(), Err(Error::InvalidInput(_))));

        let mut checkpoint = sample();
        checkpoint.checkpoint_sequence = u64::MAX;
        checkpoint.next_sequence = u64::MAX;
        assert!(matches!(checkpoint.encode(), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn decoder_rejects_checksum_valid_sequence_gap() {
        let mut bytes = sample().encode().unwrap();
        bytes[32..40].copy_from_slice(&10_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree"
            ))
        ));
    }

    #[test]
    fn rejects_invalid_sequences_and_identifiers_even_with_valid_checksum() {
        let mut bytes = sample().encode().unwrap();
        bytes[32..40].copy_from_slice(&8_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree"
            ))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[40..48].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt("invalid journal checkpoint state"))
        ));

        let mut bytes = sample().encode().unwrap();
        bytes[16..24].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt("invalid journal checkpoint state"))
        ));

        assert!(matches!(
            JournalCheckpoint::decode(&[0; JOURNAL_CHECKPOINT_SIZE - 1]),
            Err(Error::InvalidInput(_))
        ));
    }
    #[test]
    fn checkpoint_encoding_rejects_generation_and_transaction_counter_boundaries() {
        let mut checkpoint = sample();
        checkpoint.generation = 0;
        assert!(matches!(checkpoint.encode(), Err(Error::InvalidInput(_))));

        let mut checkpoint = sample();
        checkpoint.next_transaction_id = checkpoint.checkpoint_sequence + 2;
        assert!(matches!(checkpoint.encode(), Err(Error::InvalidInput(_))));

        let mut checkpoint = sample();
        checkpoint.checkpoint_sequence = u64::MAX;
        checkpoint.next_sequence = 0;
        assert!(matches!(checkpoint.encode(), Err(Error::InvalidInput(_))));
    }

    #[test]
    fn checkpoint_decoder_rejects_counter_overflow_with_valid_checksum() {
        let mut bytes = sample().encode().unwrap();
        bytes[24..32].copy_from_slice(&u64::MAX.to_le_bytes());
        bytes[32..40].copy_from_slice(&0_u64.to_le_bytes());
        refresh_checksum(&mut bytes);

        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree"
            ))
        ));
    }
    #[test]
    fn checkpoint_decoder_rejects_transaction_counter_beyond_history_with_valid_checksum() {
        let mut bytes = sample().encode().unwrap();
        // The checkpoint sequence is eight, so next_transaction_id may be at most nine.
        bytes[40..48].copy_from_slice(&10_u64.to_le_bytes());
        refresh_checksum(&mut bytes);
        assert!(matches!(
            JournalCheckpoint::decode(&bytes),
            Err(Error::Corrupt("invalid journal checkpoint state"))
        ));
    }

}
