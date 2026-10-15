//! Persistent single-writer write-ahead journal over an explicitly reserved block region.
//!
//! The caller must reserve this region in the volume layout. This engine is not yet
//! wired into the legacy formatter or allocator; open_journal must run before mutations.

use crate::{
    plan_recovery, JournalCheckpoint, JournalRecord, JournalRecordKind, JOURNAL_HEADER_SIZE,
};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

const UPDATE_CHUNK_HEADER_SIZE: usize = 12;
const MIN_RING_BLOCKS: u64 = 4;

/// Explicit location of checkpoint copies and the bounded one-block-per-record ring.
/// After a durable checkpoint, a new epoch starts at ring slot zero. Old slots may
/// retain records whose sequence is already covered by the checkpoint.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct JournalRegion {
    pub checkpoint_a: u64,
    pub checkpoint_b: u64,
    pub ring_start: u64,
    pub ring_blocks: u64,
}

impl JournalRegion {
    fn validate<D: BlockDevice>(&self, device: &D) -> Result<()> {
        let total = device.block_count();
        if self.ring_blocks < MIN_RING_BLOCKS {
            return Err(Error::InvalidInput(
                "journal ring must contain at least four blocks",
            ));
        }
        if self.checkpoint_a >= total
            || self.checkpoint_b >= total
            || self.checkpoint_a == self.checkpoint_b
        {
            return Err(Error::InvalidInput(
                "journal checkpoint blocks are invalid or overlap",
            ));
        }
        if self.ring_start >= total || self.ring_blocks > total - self.ring_start {
            return Err(Error::InvalidInput("journal ring lies outside the device"));
        }
        let ring_end = self
            .ring_start
            .checked_add(self.ring_blocks)
            .ok_or(Error::ArithmeticOverflow)?;
        if (self.checkpoint_a >= self.ring_start && self.checkpoint_a < ring_end)
            || (self.checkpoint_b >= self.ring_start && self.checkpoint_b < ring_end)
        {
            return Err(Error::InvalidInput(
                "checkpoint blocks must not overlap the journal ring",
            ));
        }
        let block_size =
            usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
        // Checkpoint records are 72 bytes even when the journal-record header is
        // smaller. Reject undersized devices here, before checkpoint padding slices
        // or redo chunk arithmetic can panic.
        if block_size < crate::JOURNAL_CHECKPOINT_SIZE
            || block_size <= JOURNAL_HEADER_SIZE + UPDATE_CHUNK_HEADER_SIZE
        {
            return Err(Error::InvalidInput(
                "device block is too small for journal checkpoints and records",
            ));
        }
        Ok(())
    }

    fn contains_journal_block(&self, block: u64) -> bool {
        block == self.checkpoint_a
            || block == self.checkpoint_b
            || (block >= self.ring_start && block - self.ring_start < self.ring_blocks)
    }
}

/// A full home-block redo image. The target must be outside the journal region.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RedoUpdate {
    pub target_block: u64,
    pub contents: Vec<u8>,
}

/// Open, recovered, single-writer journal state. Any I/O error during commit poisons
/// this instance; drop it and reopen before attempting another mutation.
/// Deliberately not `Clone`: duplicated writer state could reuse transaction IDs and ring slots.
#[derive(Debug)]
pub struct PersistentJournal {
    region: JournalRegion,
    checkpoint: JournalCheckpoint,
    active_checkpoint_copy: u8,
    recovery_required: bool,
    protected_blocks: Vec<u64>,
}

/// Initialize a reserved journal region. Destructive; only call while formatting.
pub fn format_journal<D: BlockDevice>(device: &mut D, region: JournalRegion) -> Result<()> {
    region.validate(device)?;
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    let zero = vec![0_u8; block_size];
    for slot in 0..region.ring_blocks {
        device.write_blocks(region.ring_start + slot, &zero)?;
    }
    device.flush()?;
    let first = JournalCheckpoint {
        generation: 1,
        checkpoint_sequence: 0,
        next_sequence: 1,
        next_transaction_id: 1,
        head_block: 0,
        tail_block: 0,
    };
    let second = JournalCheckpoint {
        generation: 2,
        ..first
    };
    write_checkpoint_block(device, region.checkpoint_a, &first)?;
    write_checkpoint_block(device, region.checkpoint_b, &second)?;
    device.flush()
}

/// Open and recover before exposing writable journal state. A malformed terminal append
/// is ignored only when no later valid post-checkpoint record follows it.
pub fn open_journal<D: BlockDevice>(
    device: &mut D,
    region: JournalRegion,
) -> Result<PersistentJournal> {
    open_journal_protected(device, region, &[])
}

/// Open a journal while prohibiting replay into volume-control blocks.
pub(crate) fn open_journal_protected<D: BlockDevice>(
    device: &mut D,
    region: JournalRegion,
    protected_blocks: &[u64],
) -> Result<PersistentJournal> {
    region.validate(device)?;
    if protected_blocks
        .iter()
        .any(|block| *block >= device.block_count())
    {
        return Err(Error::InvalidInput(
            "protected journal target lies outside the device",
        ));
    }
    if protected_blocks
        .iter()
        .any(|block| region.contains_journal_block(*block))
    {
        return Err(Error::InvalidInput(
            "journal metadata overlaps a protected volume-control block",
        ));
    }
    let (mut checkpoint, active_copy) = read_checkpoint_pair(device, region)?;
    let scanned = scan_uncheckpointed_records(
        device,
        region,
        checkpoint.checkpoint_sequence,
        checkpoint.next_transaction_id,
    )?;

    if !scanned.records.is_empty() {
        let plan = plan_recovery(&scanned.records, checkpoint.checkpoint_sequence)?;
        // Preflight every counter before replay: a counter overflow must never leave
        // home blocks modified while recovery cannot publish its checkpoint.
        let highest_sequence = scanned.records.last().unwrap().sequence;
        let next_sequence = highest_sequence
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let highest_transaction = scanned
            .records
            .iter()
            .map(|record| record.transaction_id)
            .max()
            .unwrap();
        let next_transaction_id = highest_transaction
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let next_generation = checkpoint
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let block_size =
            usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;

        // Validate all committed redo images before applying the first home write.
        let mut validated = Vec::with_capacity(plan.len());
        for transaction in &plan {
            let updates = decode_redo_updates(&transaction.updates, block_size)?;
            for update in &updates {
                validate_home_target(device, region, update.target_block, update.contents.len())?;
                validate_protected_target(protected_blocks, update.target_block)?;
            }
            validated.push(updates);
        }
        for updates in &validated {
            for update in updates {
                device.write_blocks(update.target_block, &update.contents)?;
            }
        }
        if validated.iter().any(|updates| !updates.is_empty()) {
            device.flush()?;
        }

        checkpoint.checkpoint_sequence = highest_sequence;
        checkpoint.next_sequence = next_sequence;
        checkpoint.next_transaction_id = next_transaction_id;
        checkpoint.head_block = 0;
        checkpoint.tail_block = 0;
        checkpoint.generation = next_generation;
        let next_copy = 1 - active_copy;
        let target = checkpoint_block_number(region, next_copy);
        write_checkpoint_block(device, target, &checkpoint)?;
        device.flush()?;
        return Ok(PersistentJournal {
            region,
            checkpoint,
            active_checkpoint_copy: next_copy,
            recovery_required: false,
            protected_blocks: protected_blocks.to_vec(),
        });
    }

    Ok(PersistentJournal {
        region,
        checkpoint,
        active_checkpoint_copy: active_copy,
        recovery_required: false,
        protected_blocks: protected_blocks.to_vec(),
    })
}

impl PersistentJournal {
    pub fn checkpoint(&self) -> JournalCheckpoint {
        self.checkpoint
    }

    /// Return the exact number of ring records needed for this many full-block redo images.
    ///
    /// Callers that stage data before committing ownership changes can preflight capacity
    /// before their first write, avoiding durable-but-unpublishable staging when a bounded
    /// journal cannot fit the complete atomic transition.
    pub(crate) fn required_record_count<D: BlockDevice>(
        &self,
        device: &D,
        update_count: usize,
    ) -> Result<usize> {
        self.region.validate(device)?;
        let block_size =
            usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
        let chunk_capacity = redo_chunk_size(block_size)?;
        let chunks_per_update = block_size
            .checked_add(chunk_capacity - 1)
            .ok_or(Error::ArithmeticOverflow)?
            / chunk_capacity;
        let required_records = 2_usize
            .checked_add(
                update_count
                    .checked_mul(chunks_per_update)
                    .ok_or(Error::ArithmeticOverflow)?,
            )
            .ok_or(Error::ArithmeticOverflow)?;
        if u64::try_from(required_records).map_err(|_| Error::ArithmeticOverflow)?
            > self.region.ring_blocks
        {
            return Err(Error::InvalidInput(
                "transaction does not fit in the bounded journal ring",
            ));
        }
        Ok(required_records)
    }

    /// Append, durably commit, apply, flush home blocks, then persist the checkpoint.
    pub fn commit<D: BlockDevice>(
        &mut self,
        device: &mut D,
        updates: &[RedoUpdate],
    ) -> Result<u64> {
        if self.recovery_required {
            return Err(Error::InvalidInput(
                "journal requires recovery after a previous I/O error",
            ));
        }
        if updates.is_empty() {
            return Err(Error::InvalidInput(
                "journal transaction must contain redo updates",
            ));
        }
        self.region.validate(device)?;
        let block_size =
            usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;

        // Reject transactions that cannot fit before walking every target or
        // allocating the duplicate-detection set. The journal has a fixed ring, so
        // an oversized caller-provided slice must not trigger unbounded extra work.
        let required_records = self.required_record_count(device, updates.len())?;

        // A contiguous vector avoids one tree-node allocation per target. Sort
        // only after all targets pass validation, then detect duplicates in-place.
        let mut targets = Vec::with_capacity(updates.len());
        for update in updates {
            if update.contents.len() != block_size {
                return Err(Error::InvalidInput(
                    "redo image must be exactly one device block",
                ));
            }
            validate_home_target(device, self.region, update.target_block, block_size)?;
            validate_protected_target(&self.protected_blocks, update.target_block)?;
            targets.push(update.target_block);
        }
        targets.sort_unstable();
        if targets.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::InvalidInput(
                "transaction contains duplicate redo targets",
            ));
        }

        let transaction_id = self.checkpoint.next_transaction_id;
        let mut sequence = self.checkpoint.next_sequence;
        // Encode records as they are built instead of retaining a second full copy
        // of every UPDATE payload in a JournalRecord vector. The complete encoded
        // transaction is still prepared before the first persistent write, so a
        // validation/encoding error cannot leave a partial transaction on disk.
        let mut encoded = Vec::with_capacity(required_records);
        encoded.push(
            JournalRecord {
                kind: JournalRecordKind::Begin,
                sequence,
                transaction_id,
                payload: Vec::new(),
            }
            .encode_block(block_size)?,
        );
        sequence = sequence.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        for update in updates {
            for payload in encode_redo_chunks(update, block_size)? {
                encoded.push(
                    JournalRecord {
                        kind: JournalRecordKind::Update,
                        sequence,
                        transaction_id,
                        payload,
                    }
                    .encode_block(block_size)?,
                );
                sequence = sequence.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            }
        }
        let commit_sequence = sequence;
        encoded.push(
            JournalRecord {
                kind: JournalRecordKind::Commit,
                sequence,
                transaction_id,
                payload: Vec::new(),
            }
            .encode_block(block_size)?,
        );
        sequence = sequence.checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        if encoded.len() != required_records {
            return Err(Error::Corrupt(
                "journal record count preflight disagrees with encoding",
            ));
        }

        // Preflight counters before the first persistent write. Overflow after a
        // durable COMMIT would otherwise strand recovery before checkpoint publication.
        let next_transaction_id = transaction_id
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        let next_generation = self
            .checkpoint
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;

        // From here on persistence may be uncertain after any error.
        self.recovery_required = true;
        for (slot, block) in encoded.iter().take(encoded.len() - 1).enumerate() {
            device.write_blocks(self.region.ring_start + slot as u64, block)?;
        }
        device.flush()?;
        let commit_slot = encoded.len() - 1;
        device.write_blocks(
            self.region.ring_start + commit_slot as u64,
            &encoded[commit_slot],
        )?;
        device.flush()?;

        for update in updates {
            device.write_blocks(update.target_block, &update.contents)?;
        }
        device.flush()?;

        let mut checkpoint = self.checkpoint;
        checkpoint.generation = next_generation;
        checkpoint.checkpoint_sequence = commit_sequence;
        checkpoint.next_sequence = sequence;
        checkpoint.next_transaction_id = next_transaction_id;
        checkpoint.head_block = 0;
        checkpoint.tail_block = 0;
        let next_copy = 1 - self.active_checkpoint_copy;
        write_checkpoint_block(
            device,
            checkpoint_block_number(self.region, next_copy),
            &checkpoint,
        )?;
        device.flush()?;

        self.checkpoint = checkpoint;
        self.active_checkpoint_copy = next_copy;
        self.recovery_required = false;
        Ok(transaction_id)
    }
}

struct ScannedRecords {
    records: Vec<JournalRecord>,
}

fn scan_uncheckpointed_records<D: BlockDevice>(
    device: &mut D,
    region: JournalRegion,
    checkpoint_sequence: u64,
    next_transaction_id: u64,
) -> Result<ScannedRecords> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    let mut newer = Vec::<(u64, JournalRecord)>::new();
    // Only the earliest malformed slot matters: it must not precede a later
    // valid record. Tracking one index avoids a second allocation for bad slots.
    let mut first_malformed_slot = None;
    // Reuse one scratch block for the bounded ring scan. Decoding owns its payload,
    // so the next read can safely overwrite this buffer without cloning the block.
    let mut block = vec![0_u8; block_size];
    for slot in 0..region.ring_blocks {
        device.read_blocks(region.ring_start + slot, &mut block)?;
        if block.iter().all(|byte| *byte == 0) {
            continue;
        }
        match JournalRecord::decode_block(&block) {
            Ok(record) if record.sequence <= checkpoint_sequence => {}
            Ok(record) => {
                if record.transaction_id < next_transaction_id {
                    return Err(Error::Corrupt(
                        "post-checkpoint journal record reuses a checkpointed transaction ID",
                    ));
                }
                newer.push((slot, record));
            }
            Err(_) => {
                first_malformed_slot =
                    Some(first_malformed_slot.map_or(slot, |first: u64| first.min(slot)));
            }
        }
    }
    // Sequence numbers are validated for strict continuity by recovery planning;
    // an unstable in-place sort avoids the scratch allocation of stable sorting.
    newer.sort_unstable_by_key(|(_, record)| record.sequence);
    // The checkpoint stores the *exact* next transaction ID, not merely a lower
    // bound. With one serialized writer, each new BEGIN must consume that ID in
    // order; accepting gaps would hide journal corruption and desynchronize IDs.
    let mut expected_transaction_id = next_transaction_id;
    for (_, record) in &newer {
        if record.kind == JournalRecordKind::Begin {
            if record.transaction_id != expected_transaction_id {
                return Err(Error::Corrupt(
                    "journal transaction IDs are not contiguous after checkpoint",
                ));
            }
            expected_transaction_id = expected_transaction_id
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow)?;
        }
    }
    if let Some((last_slot, _)) = newer.iter().max_by_key(|(slot, _)| *slot) {
        if first_malformed_slot.is_some_and(|slot| slot < *last_slot) {
            return Err(Error::Corrupt(
                "malformed journal block appears before a later valid record",
            ));
        }
    }
    for (index, (slot, _)) in newer.iter().enumerate() {
        if *slot != index as u64 {
            return Err(Error::Corrupt(
                "post-checkpoint journal records are not contiguous from ring start",
            ));
        }
    }
    Ok(ScannedRecords {
        records: newer.into_iter().map(|(_, record)| record).collect(),
    })
}

fn read_checkpoint_pair<D: BlockDevice>(
    device: &mut D,
    region: JournalRegion,
) -> Result<(JournalCheckpoint, u8)> {
    // A checksum-valid checkpoint can still be semantically unusable (for example,
    // inconsistent sequence counters or unsupported ring positions). Validate each copy
    // independently so a bad newer copy does not mask a recoverable older one.
    let first = read_checkpoint_block(device, region.checkpoint_a).and_then(|checkpoint| {
        validate_checkpoint_geometry(&checkpoint, region)?;
        Ok(checkpoint)
    });
    let second = read_checkpoint_block(device, region.checkpoint_b).and_then(|checkpoint| {
        validate_checkpoint_geometry(&checkpoint, region)?;
        Ok(checkpoint)
    });
    match (first, second) {
        // Either checkpoint copy can be unavailable independently. Preserve an I/O
        // error only when no readable copy exists; otherwise the valid copy is enough
        // to recover the journal state.
        (Err(error @ Error::Io(_)), Err(_)) => Err(error),
        (Err(_), Err(error @ Error::Io(_))) => Err(error),
        (Ok(a), Ok(b)) if a.generation == b.generation && a != b => Err(Error::Corrupt(
            "checkpoint copies conflict at the same generation",
        )),
        (Ok(a), Ok(b))
            if a.generation > b.generation && a.generation.checked_sub(b.generation) != Some(1) =>
        {
            Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ))
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation && b.generation.checked_sub(a.generation) != Some(1) =>
        {
            Err(Error::Corrupt(
                "journal checkpoint generations are not consecutive",
            ))
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(a.generation == 2
                    && b.generation == 1
                    && a.checkpoint_sequence == 0
                    && a.next_transaction_id == 1
                    && b.next_transaction_id == 1) =>
        {
            Err(Error::Corrupt(
                "checkpoint generation advanced without a durable journal record",
            ))
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && a.checkpoint_sequence == b.checkpoint_sequence
                && !(b.generation == 2
                    && a.generation == 1
                    && a.checkpoint_sequence == 0
                    && a.next_transaction_id == 1
                    && b.next_transaction_id == 1) =>
        {
            Err(Error::Corrupt(
                "checkpoint generation advanced without a durable journal record",
            ))
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && a.checkpoint_sequence > b.checkpoint_sequence
                && a.next_transaction_id == b.next_transaction_id =>
        {
            Err(Error::Corrupt(
                "journal sequence advanced without a transaction ID",
            ))
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && b.checkpoint_sequence > a.checkpoint_sequence
                && b.next_transaction_id == a.next_transaction_id =>
        {
            Err(Error::Corrupt(
                "journal sequence advanced without a transaction ID",
            ))
        }
        (Ok(a), Ok(b))
            if a.generation > b.generation
                && (a.checkpoint_sequence < b.checkpoint_sequence
                    || a.next_transaction_id < b.next_transaction_id) =>
        {
            Err(Error::Corrupt(
                "newer journal checkpoint regresses durable counters",
            ))
        }
        (Ok(a), Ok(b))
            if b.generation > a.generation
                && (b.checkpoint_sequence < a.checkpoint_sequence
                    || b.next_transaction_id < a.next_transaction_id) =>
        {
            Err(Error::Corrupt(
                "newer journal checkpoint regresses durable counters",
            ))
        }
        (Ok(a), Ok(b)) if a.generation >= b.generation => Ok((a, 0)),
        (Ok(_), Ok(b)) => Ok((b, 1)),
        (Ok(a), Err(_)) => Ok((a, 0)),
        (Err(_), Ok(b)) => Ok((b, 1)),
        (Err(error), Err(_)) => Err(error),
    }
}

fn validate_checkpoint_geometry(
    checkpoint: &JournalCheckpoint,
    region: JournalRegion,
) -> Result<()> {
    if checkpoint.checkpoint_sequence.checked_add(1) != Some(checkpoint.next_sequence) {
        return Err(Error::Corrupt(
            "journal checkpoint sequence counters disagree",
        ));
    }
    if checkpoint.head_block >= region.ring_blocks || checkpoint.tail_block >= region.ring_blocks {
        return Err(Error::Corrupt("checkpoint ring position is out of bounds"));
    }
    if checkpoint.head_block != 0 || checkpoint.tail_block != 0 {
        return Err(Error::Corrupt(
            "unsupported non-zero checkpoint ring position",
        ));
    }
    Ok(())
}

fn checkpoint_block_number(region: JournalRegion, copy: u8) -> u64 {
    if copy == 0 {
        region.checkpoint_a
    } else {
        region.checkpoint_b
    }
}

fn read_checkpoint_block<D: BlockDevice>(
    device: &mut D,
    block_number: u64,
) -> Result<JournalCheckpoint> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    let mut block = vec![0_u8; block_size];
    device.read_blocks(block_number, &mut block)?;
    if block[crate::JOURNAL_CHECKPOINT_SIZE..]
        .iter()
        .any(|byte| *byte != 0)
    {
        return Err(Error::Corrupt("checkpoint block padding must be zero"));
    }
    JournalCheckpoint::decode(&block[..crate::JOURNAL_CHECKPOINT_SIZE])
}

fn write_checkpoint_block<D: BlockDevice>(
    device: &mut D,
    block_number: u64,
    checkpoint: &JournalCheckpoint,
) -> Result<()> {
    let block_size = usize::try_from(device.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
    let encoded = checkpoint.encode()?;
    let mut block = vec![0_u8; block_size];
    block[..encoded.len()].copy_from_slice(&encoded);
    device.write_blocks(block_number, &block)
}

fn validate_home_target<D: BlockDevice>(
    device: &D,
    region: JournalRegion,
    target: u64,
    contents_len: usize,
) -> Result<()> {
    if target >= device.block_count() {
        return Err(Error::InvalidInput("redo target lies outside the device"));
    }
    if region.contains_journal_block(target) {
        return Err(Error::InvalidInput(
            "redo target overlaps the journal region",
        ));
    }
    if contents_len != device.block_size() as usize {
        return Err(Error::InvalidInput("redo image has incorrect block size"));
    }
    Ok(())
}

fn validate_protected_target(protected_blocks: &[u64], target: u64) -> Result<()> {
    if protected_blocks.contains(&target) {
        return Err(Error::InvalidInput(
            "redo target overlaps a protected volume-control block",
        ));
    }
    Ok(())
}

fn redo_chunk_size(block_size: usize) -> Result<usize> {
    let chunk_size = block_size
        .checked_sub(JOURNAL_HEADER_SIZE + UPDATE_CHUNK_HEADER_SIZE)
        .ok_or(Error::InvalidInput(
            "device block is too small for redo chunks",
        ))?;
    if chunk_size == 0 {
        return Err(Error::InvalidInput(
            "device block is too small for redo chunks",
        ));
    }
    Ok(chunk_size)
}

fn encode_redo_chunks(update: &RedoUpdate, block_size: usize) -> Result<Vec<Vec<u8>>> {
    let chunk_size = redo_chunk_size(block_size)?;
    let count = update
        .contents
        .len()
        .checked_add(chunk_size - 1)
        .ok_or(Error::ArithmeticOverflow)?
        / chunk_size;
    let count_u16 =
        u16::try_from(count).map_err(|_| Error::InvalidInput("too many redo chunks"))?;
    let mut chunks = Vec::with_capacity(count);
    for index in 0..count {
        let start = index * chunk_size;
        let end = update.contents.len().min(start + chunk_size);
        let mut payload = vec![0; UPDATE_CHUNK_HEADER_SIZE + end - start];
        payload[..8].copy_from_slice(&update.target_block.to_le_bytes());
        payload[8..10].copy_from_slice(&(index as u16).to_le_bytes());
        payload[10..12].copy_from_slice(&count_u16.to_le_bytes());
        payload[12..].copy_from_slice(&update.contents[start..end]);
        chunks.push(payload);
    }
    Ok(chunks)
}

fn decode_redo_updates(payloads: &[Vec<u8>], block_size: usize) -> Result<Vec<RedoUpdate>> {
    let chunk_size = redo_chunk_size(block_size)?;
    let expected_count = block_size
        .checked_add(chunk_size - 1)
        .ok_or(Error::ArithmeticOverflow)?
        / chunk_size;
    let expected_count_u16 =
        u16::try_from(expected_count).map_err(|_| Error::ArithmeticOverflow)?;
    // Keep decoded images in one contiguous collection. The target list would
    // duplicate one u64 per image; sorting the images themselves is sufficient
    // because a committed transaction may not target the same home block twice.
    // Each complete image consumes exactly expected_count chunks. Reserving
    // the maximum possible image count avoids repeated growth during recovery.
    let mut updates = Vec::with_capacity(payloads.len() / expected_count);
    let mut target_active = None;
    let mut active_count = 0_u16;
    let mut current = Vec::with_capacity(block_size);
    for (payload_index, payload) in payloads.iter().enumerate() {
        if payload.len() < UPDATE_CHUNK_HEADER_SIZE {
            return Err(Error::Corrupt("redo chunk header is truncated"));
        }
        let target = u64::from_le_bytes(payload[..8].try_into().unwrap());
        let index = u16::from_le_bytes([payload[8], payload[9]]);
        let count = u16::from_le_bytes([payload[10], payload[11]]);
        if count != expected_count_u16 || index >= count {
            return Err(Error::Corrupt("redo chunk index or count is invalid"));
        }
        let expected_len = if usize::from(index) + 1 < expected_count {
            chunk_size
        } else {
            block_size - chunk_size * (expected_count - 1)
        };
        if payload.len() != UPDATE_CHUNK_HEADER_SIZE + expected_len {
            return Err(Error::Corrupt("redo chunk has invalid data length"));
        }
        if index == 0 {
            if target_active.is_some() {
                return Err(Error::Corrupt("redo image ended before all chunks arrived"));
            }
            target_active = Some(target);
            active_count = count;
            current.clear();
        } else if target_active != Some(target)
            || active_count != count
            || usize::from(index) != current.len() / chunk_size
        {
            return Err(Error::Corrupt(
                "redo chunks are out of order or target-mismatched",
            ));
        }
        current.extend_from_slice(&payload[UPDATE_CHUNK_HEADER_SIZE..]);
        if index + 1 == count {
            if current.len() != block_size {
                return Err(Error::Corrupt(
                    "redo image has incorrect reconstructed size",
                ));
            }
            // Transfer the reconstructed image into the result instead of cloning
            // every byte. The final image needs no replacement scratch allocation.
            let contents = if payload_index + 1 == payloads.len() {
                std::mem::take(&mut current)
            } else {
                std::mem::replace(&mut current, Vec::with_capacity(block_size))
            };
            updates.push(RedoUpdate {
                target_block: target,
                contents,
            });
            target_active = None;
            active_count = 0;
        }
    }
    if target_active.is_some() {
        return Err(Error::Corrupt("redo image is missing one or more chunks"));
    }
    updates.sort_unstable_by_key(|update| update.target_block);
    if updates
        .windows(2)
        .any(|pair| pair[0].target_block == pair[1].target_block)
    {
        return Err(Error::Corrupt(
            "redo transaction contains duplicate targets",
        ));
    }
    Ok(updates)
}

#[cfg(test)]
mod tests {
    use super::{
        decode_redo_updates, encode_redo_chunks, format_journal, open_journal,
        open_journal_protected, JournalCheckpoint, JournalRegion, RedoUpdate,
    };
    use openfs_block::BlockDevice;
    use openfs_core::{Error, Result};
    use std::sync::{Arc, Mutex};

    const BS: u32 = 4096;
    const COUNT: u64 = 32;
    const REGION: JournalRegion = JournalRegion {
        checkpoint_a: 1,
        checkpoint_b: 2,
        ring_start: 3,
        ring_blocks: 8,
    };
    const HOME: u64 = 20;

    struct SmallBlockDevice {
        inner: MemoryDevice,
        block_size: u32,
    }
    impl BlockDevice for SmallBlockDevice {
        fn block_size(&self) -> u32 {
            self.block_size
        }
        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }
        fn read_blocks(&mut self, first: u64, output: &mut [u8]) -> Result<()> {
            self.inner.read_blocks(first, output)
        }
        fn write_blocks(&mut self, first: u64, input: &[u8]) -> Result<()> {
            self.inner.write_blocks(first, input)
        }
        fn flush(&mut self) -> Result<()> {
            self.inner.flush()
        }
    }

    #[derive(Clone)]
    struct MemoryDevice {
        bytes: Arc<Mutex<Vec<u8>>>,
    }
    impl MemoryDevice {
        fn new() -> Self {
            Self {
                bytes: Arc::new(Mutex::new(vec![0; BS as usize * COUNT as usize])),
            }
        }
    }
    impl BlockDevice for MemoryDevice {
        fn block_size(&self) -> u32 {
            BS
        }
        fn block_count(&self) -> u64 {
            COUNT
        }
        fn read_blocks(&mut self, first: u64, output: &mut [u8]) -> Result<()> {
            let start = first as usize * BS as usize;
            output.copy_from_slice(&self.bytes.lock().unwrap()[start..start + output.len()]);
            Ok(())
        }
        fn write_blocks(&mut self, first: u64, input: &[u8]) -> Result<()> {
            let start = first as usize * BS as usize;
            self.bytes.lock().unwrap()[start..start + input.len()].copy_from_slice(input);
            Ok(())
        }
        fn flush(&mut self) -> Result<()> {
            Ok(())
        }
    }

    struct FailAt<D> {
        inner: D,
        operation: usize,
        fail_at: usize,
    }
    impl<D> FailAt<D> {
        fn new(inner: D, fail_at: usize) -> Self {
            Self {
                inner,
                operation: 0,
                fail_at,
            }
        }
        fn into_inner(self) -> D {
            self.inner
        }
        fn check(&mut self) -> Result<()> {
            let current = self.operation;
            self.operation += 1;
            if current == self.fail_at {
                return Err(Error::Io(std::io::Error::other(
                    "injected journal I/O failure",
                )));
            }
            Ok(())
        }
    }
    impl<D: BlockDevice> BlockDevice for FailAt<D> {
        fn block_size(&self) -> u32 {
            self.inner.block_size()
        }
        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }
        fn read_blocks(&mut self, first: u64, output: &mut [u8]) -> Result<()> {
            self.check()?;
            self.inner.read_blocks(first, output)
        }
        fn write_blocks(&mut self, first: u64, input: &[u8]) -> Result<()> {
            self.check()?;
            self.inner.write_blocks(first, input)
        }
        fn flush(&mut self) -> Result<()> {
            self.check()?;
            self.inner.flush()
        }
    }

    /// Persists only a prefix of one selected block write, then reports an I/O error.
    /// This models a torn block at the software boundary, not physical flash behavior.
    struct TearWriteAt<D> {
        inner: D,
        operation: usize,
        tear_at: usize,
        prefix_bytes: usize,
    }
    impl<D> TearWriteAt<D> {
        fn new(inner: D, tear_at: usize, prefix_bytes: usize) -> Self {
            Self {
                inner,
                operation: 0,
                tear_at,
                prefix_bytes,
            }
        }
        fn into_inner(self) -> D {
            self.inner
        }
    }
    impl<D: BlockDevice> BlockDevice for TearWriteAt<D> {
        fn block_size(&self) -> u32 {
            self.inner.block_size()
        }
        fn block_count(&self) -> u64 {
            self.inner.block_count()
        }
        fn read_blocks(&mut self, first: u64, output: &mut [u8]) -> Result<()> {
            self.inner.read_blocks(first, output)
        }
        fn write_blocks(&mut self, first: u64, input: &[u8]) -> Result<()> {
            let current = self.operation;
            self.operation += 1;
            if current == self.tear_at {
                if input.is_empty() || self.prefix_bytes >= input.len() {
                    return Err(Error::InvalidInput("invalid test torn-write prefix"));
                }
                let mut mixed = vec![0; input.len()];
                self.inner.read_blocks(first, &mut mixed)?;
                mixed[..self.prefix_bytes].copy_from_slice(&input[..self.prefix_bytes]);
                self.inner.write_blocks(first, &mixed)?;
                return Err(Error::Io(std::io::Error::other(
                    "injected torn journal write",
                )));
            }
            self.inner.write_blocks(first, input)
        }
        fn flush(&mut self) -> Result<()> {
            self.inner.flush()
        }
    }

    fn update(byte: u8) -> RedoUpdate {
        RedoUpdate {
            target_block: HOME,
            contents: vec![byte; BS as usize],
        }
    }
    fn read_home(device: &mut MemoryDevice) -> Vec<u8> {
        let mut bytes = vec![0; BS as usize];
        device.read_blocks(HOME, &mut bytes).unwrap();
        bytes
    }
    fn read_block(device: &mut MemoryDevice, block: u64) -> Vec<u8> {
        let mut bytes = vec![0; BS as usize];
        device.read_blocks(block, &mut bytes).unwrap();
        bytes
    }
    fn formatted() -> MemoryDevice {
        let mut device = MemoryDevice::new();
        format_journal(&mut device, REGION).unwrap();
        device.write_blocks(HOME, &vec![0x11; BS as usize]).unwrap();
        device.flush().unwrap();
        device
    }

    #[test]
    fn open_rejects_post_checkpoint_record_after_empty_ring_slot() {
        let mut device = formatted();
        let record = crate::JournalRecord {
            kind: crate::JournalRecordKind::Begin,
            sequence: 1,
            transaction_id: 1,
            payload: Vec::new(),
        };
        device
            .write_blocks(
                REGION.ring_start + 1,
                &record.encode_block(BS as usize).unwrap(),
            )
            .unwrap();
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(
                "post-checkpoint journal records are not contiguous from ring start"
            ))
        ));
        assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
    }

    #[test]
    fn open_rejects_duplicate_post_checkpoint_record_sequences() {
        let mut device = formatted();
        let begin = crate::JournalRecord {
            kind: crate::JournalRecordKind::Begin,
            sequence: 1,
            transaction_id: 1,
            payload: Vec::new(),
        };
        let update = crate::JournalRecord {
            kind: crate::JournalRecordKind::Update,
            sequence: 1,
            transaction_id: 1,
            payload: vec![0xA5],
        };
        for (slot, record) in [begin, update].iter().enumerate() {
            device
                .write_blocks(
                    REGION.ring_start + slot as u64,
                    &record.encode_block(BS as usize).unwrap(),
                )
                .unwrap();
        }
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(
                "journal sequence is not contiguous after checkpoint"
            ))
        ));
        assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
    }

    #[test]
    fn open_rejects_post_checkpoint_transaction_id_reuse() {
        use crate::{JournalRecord, JournalRecordKind};

        let mut device = MemoryDevice::new();
        format_journal(&mut device, REGION).unwrap();

        let checkpoint = JournalCheckpoint {
            generation: 3,
            checkpoint_sequence: 12,
            next_sequence: 13,
            next_transaction_id: 5,
            head_block: 0,
            tail_block: 0,
        };
        let encoded = checkpoint.encode().unwrap();
        let mut checkpoint_block = vec![0_u8; BS as usize];
        checkpoint_block[..encoded.len()].copy_from_slice(&encoded);
        device
            .write_blocks(REGION.checkpoint_a, &checkpoint_block)
            .unwrap();
        let older = JournalCheckpoint {
            generation: 2,
            checkpoint_sequence: 9,
            next_sequence: 10,
            next_transaction_id: 4,
            head_block: 0,
            tail_block: 0,
        };
        let older_encoded = older.encode().unwrap();
        let mut older_block = vec![0_u8; BS as usize];
        older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
        device
            .write_blocks(REGION.checkpoint_b, &older_block)
            .unwrap();

        let record = JournalRecord {
            kind: JournalRecordKind::Begin,
            sequence: 13,
            transaction_id: 1,
            payload: Vec::new(),
        };
        device
            .write_blocks(
                REGION.ring_start,
                &record.encode_block(BS as usize).unwrap(),
            )
            .unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn open_rejects_skipped_transaction_id_after_checkpoint() {
        let mut device = MemoryDevice::new();
        format_journal(&mut device, REGION).unwrap();

        let checkpoint = JournalCheckpoint {
            generation: 3,
            checkpoint_sequence: 12,
            next_sequence: 13,
            next_transaction_id: 5,
            head_block: 0,
            tail_block: 0,
        };
        let encoded = checkpoint.encode().unwrap();
        let mut checkpoint_block = vec![0_u8; BS as usize];
        checkpoint_block[..encoded.len()].copy_from_slice(&encoded);
        device
            .write_blocks(REGION.checkpoint_a, &checkpoint_block)
            .unwrap();
        let older = JournalCheckpoint {
            generation: 2,
            checkpoint_sequence: 9,
            next_sequence: 10,
            next_transaction_id: 4,
            head_block: 0,
            tail_block: 0,
        };
        let older_encoded = older.encode().unwrap();
        let mut older_block = vec![0_u8; BS as usize];
        older_block[..older_encoded.len()].copy_from_slice(&older_encoded);
        device
            .write_blocks(REGION.checkpoint_b, &older_block)
            .unwrap();

        let record = crate::JournalRecord {
            kind: crate::JournalRecordKind::Begin,
            sequence: 13,
            transaction_id: 6,
            payload: Vec::new(),
        };
        device
            .write_blocks(
                REGION.ring_start,
                &record.encode_block(BS as usize).unwrap(),
            )
            .unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn open_rejects_newer_checkpoint_with_regressed_counters() {
        let mut device = formatted();
        let newer = JournalCheckpoint {
            generation: 3,
            checkpoint_sequence: 4,
            next_sequence: 5,
            next_transaction_id: 2,
            head_block: 0,
            tail_block: 0,
        };
        let older = JournalCheckpoint {
            generation: 2,
            checkpoint_sequence: 8,
            next_sequence: 9,
            next_transaction_id: 3,
            head_block: 0,
            tail_block: 0,
        };
        super::write_checkpoint_block(&mut device, REGION.checkpoint_a, &newer).unwrap();
        super::write_checkpoint_block(&mut device, REGION.checkpoint_b, &older).unwrap();
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(
                "newer journal checkpoint regresses durable counters"
            ))
        ));
        assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
    }

    #[test]
    fn open_rejects_checkpoint_generation_gaps() {
        let mut device = MemoryDevice::new();
        format_journal(&mut device, REGION).unwrap();
        for (block_number, checkpoint) in [
            (
                REGION.checkpoint_a,
                JournalCheckpoint {
                    generation: 4,
                    checkpoint_sequence: 3,
                    next_sequence: 4,
                    next_transaction_id: 2,
                    head_block: 0,
                    tail_block: 0,
                },
            ),
            (
                REGION.checkpoint_b,
                JournalCheckpoint {
                    generation: 2,
                    checkpoint_sequence: 0,
                    next_sequence: 1,
                    next_transaction_id: 1,
                    head_block: 0,
                    tail_block: 0,
                },
            ),
        ] {
            let encoded = checkpoint.encode().unwrap();
            let mut block = vec![0_u8; BS as usize];
            block[..encoded.len()].copy_from_slice(&encoded);
            device.write_blocks(block_number, &block).unwrap();
        }
        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(_))
        ));
    }

    #[test]
    fn open_uses_valid_checkpoint_copy_when_other_read_fails() {
        for fail_at in [0, 1] {
            let mut device = formatted();
            let mut failing = FailAt::new(device.clone(), fail_at);
            let journal = open_journal(&mut failing, REGION)
                .expect("one readable checkpoint copy should be sufficient");
            assert_eq!(journal.checkpoint().checkpoint_sequence, 0);
            // Opening with no pending redo must not modify the home block.
            device = failing.into_inner();
            assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
        }
    }

    #[test]
    fn open_falls_back_when_newer_checkpoint_has_invalid_geometry() {
        for invalid_copy in [0_u8, 1_u8] {
            let mut device = formatted();
            let invalid = JournalCheckpoint {
                generation: 10,
                checkpoint_sequence: 0,
                next_sequence: 1,
                next_transaction_id: 1,
                // Valid checkpoint encoding, but invalid for this journal engine's
                // currently supported ring layout.
                head_block: 1,
                tail_block: 0,
            };
            let encoded = invalid.encode().unwrap();
            let block_number = if invalid_copy == 0 {
                REGION.checkpoint_a
            } else {
                REGION.checkpoint_b
            };
            let mut block = vec![0; BS as usize];
            block[..encoded.len()].copy_from_slice(&encoded);
            device.write_blocks(block_number, &block).unwrap();
            device.flush().unwrap();

            let journal = open_journal(&mut device, REGION)
                .expect("the other coherent checkpoint copy should remain usable");
            let expected_generation = if invalid_copy == 0 { 2 } else { 1 };
            assert_eq!(journal.checkpoint().generation, expected_generation);
            assert_eq!(journal.checkpoint().checkpoint_sequence, 0);
            assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
        }
    }

    #[test]
    fn recovery_replays_durable_commit_when_newer_checkpoint_geometry_is_invalid() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();

        // A single-block redo image uses BEGIN + two UPDATE chunks + COMMIT.
        // Fail the first home write, after COMMIT is durable but before checkpointing.
        let mut failing = FailAt::new(device.clone(), 6);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut recovered = failing.into_inner();

        // Simulate a torn/semantically invalid newest checkpoint. The older copy
        // must remain sufficient to find and replay the committed ring records.
        let invalid = JournalCheckpoint {
            generation: 10,
            checkpoint_sequence: 0,
            next_sequence: 1,
            next_transaction_id: 1,
            head_block: 1,
            tail_block: 0,
        };
        super::write_checkpoint_block(&mut recovered, REGION.checkpoint_b, &invalid).unwrap();
        recovered.flush().unwrap();

        let reopened = open_journal(&mut recovered, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 4);
        assert_eq!(read_home(&mut recovered), vec![0xA5; BS as usize]);

        // The repaired checkpoint must be valid and recovery idempotent.
        let reopened_again = open_journal(&mut recovered, REGION).unwrap();
        assert_eq!(reopened_again.checkpoint().checkpoint_sequence, 4);
        assert_eq!(read_home(&mut recovered), vec![0xA5; BS as usize]);
    }

    #[test]
    fn format_rejects_blocks_too_small_for_checkpoint_before_writing() {
        for block_size in [52, 64, 71] {
            let inner = MemoryDevice::new();
            let before = inner.bytes.lock().unwrap().clone();
            let mut device = SmallBlockDevice { inner, block_size };
            assert!(matches!(
                format_journal(&mut device, REGION),
                Err(Error::InvalidInput(_))
            ));
            assert_eq!(*device.inner.bytes.lock().unwrap(), before);
        }
    }

    #[test]
    fn counter_overflow_is_rejected_before_any_persistent_write() {
        for overflow in ["transaction id", "checkpoint generation", "record sequence"] {
            let mut device = formatted();
            let mut journal = open_journal(&mut device, REGION).unwrap();
            match overflow {
                "transaction id" => journal.checkpoint.next_transaction_id = u64::MAX,
                "checkpoint generation" => journal.checkpoint.generation = u64::MAX,
                // BEGIN and UPDATE can be constructed, but the COMMIT would require
                // advancing the sequence beyond u64::MAX.
                "record sequence" => journal.checkpoint.next_sequence = u64::MAX - 2,
                _ => unreachable!("test cases are fixed"),
            }

            // The first attempted I/O fails. ArithmeticOverflow must win, proving
            // overflow was detected before the journal touched persistent storage.
            let mut failing = FailAt::new(device.clone(), 0);
            assert!(matches!(
                journal.commit(&mut failing, &[update(0xA5)]),
                Err(Error::ArithmeticOverflow)
            ));
            let mut persisted = failing.into_inner();
            assert_eq!(read_home(&mut persisted), vec![0x11; BS as usize]);
            let reopened = open_journal(&mut persisted, REGION).unwrap();
            assert_eq!(reopened.checkpoint().checkpoint_sequence, 0);
            assert_eq!(read_home(&mut persisted), vec![0x11; BS as usize]);
        }
    }

    #[test]
    fn recovery_rejects_persisted_duplicate_targets_before_any_home_write() {
        use crate::{JournalRecord, JournalRecordKind};

        let mut device = formatted();
        let first = update(0xA5);
        let second = update(0x5A);
        let chunks = encode_redo_chunks(&first, BS as usize)
            .unwrap()
            .into_iter()
            .chain(encode_redo_chunks(&second, BS as usize).unwrap())
            .collect::<Vec<_>>();
        let mut records = Vec::new();
        records.push(JournalRecord {
            kind: JournalRecordKind::Begin,
            sequence: 1,
            transaction_id: 1,
            payload: Vec::new(),
        });
        for (index, payload) in chunks.into_iter().enumerate() {
            records.push(JournalRecord {
                kind: JournalRecordKind::Update,
                sequence: index as u64 + 2,
                transaction_id: 1,
                payload,
            });
        }
        records.push(JournalRecord {
            kind: JournalRecordKind::Commit,
            sequence: records.len() as u64 + 1,
            transaction_id: 1,
            payload: Vec::new(),
        });
        for (slot, record) in records.iter().enumerate() {
            device
                .write_blocks(
                    REGION.ring_start + slot as u64,
                    &record.encode_block(BS as usize).unwrap(),
                )
                .unwrap();
        }
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(
                "redo transaction contains duplicate targets"
            ))
        ));
        assert_eq!(
            read_home(&mut device),
            vec![0x11; BS as usize],
            "corrupt duplicate-target transaction must not replay either image"
        );
    }

    #[test]
    fn decoded_redo_rejects_duplicate_targets_from_persisted_records() {
        let first = encode_redo_chunks(&update(0xA5), BS as usize).unwrap();
        let second = encode_redo_chunks(&update(0x5A), BS as usize).unwrap();
        let payloads: Vec<Vec<u8>> = first.into_iter().chain(second).collect();

        assert!(matches!(
            decode_redo_updates(&payloads, BS as usize),
            Err(Error::Corrupt(
                "redo transaction contains duplicate targets"
            ))
        ));
    }

    #[test]
    fn decoded_redo_sorts_images_without_a_duplicate_target_buffer() {
        let mut first = update(0xA5);
        first.target_block = HOME + 1;
        let second = update(0x5A);
        let payloads: Vec<Vec<u8>> = encode_redo_chunks(&first, BS as usize)
            .unwrap()
            .into_iter()
            .chain(encode_redo_chunks(&second, BS as usize).unwrap())
            .collect();

        let decoded = decode_redo_updates(&payloads, BS as usize).unwrap();
        assert_eq!(
            decoded
                .iter()
                .map(|update| update.target_block)
                .collect::<Vec<_>>(),
            vec![HOME, HOME + 1],
            "recovery images should be ordered in-place before duplicate validation"
        );
        assert_eq!(decoded[0].contents, vec![0x5A; BS as usize]);
        assert_eq!(decoded[1].contents, vec![0xA5; BS as usize]);
    }

    #[test]
    fn redo_chunk_helpers_reject_insufficient_payload_capacity_without_panicking() {
        let minimum = super::JOURNAL_HEADER_SIZE + super::UPDATE_CHUNK_HEADER_SIZE;
        let image = update(0xA5);

        for block_size in [minimum - 1, minimum] {
            assert!(matches!(
                encode_redo_chunks(&image, block_size),
                Err(Error::InvalidInput(
                    "device block is too small for redo chunks"
                ))
            ));
            assert!(matches!(
                decode_redo_updates(&[], block_size),
                Err(Error::InvalidInput(
                    "device block is too small for redo chunks"
                ))
            ));
        }
    }

    #[test]
    fn redo_chunk_round_trip_covers_small_block_capacity_boundaries() {
        // The chunk payload capacity is block_size - journal header - chunk header.
        // Exercise the smallest legal capacity and sizes immediately around it,
        // as well as a normal filesystem block, to catch off-by-one final chunks.
        for block_size in [53_usize, 54, 63, 64, 71, 72, 4096] {
            let contents = (0..block_size)
                .map(|index| (index.wrapping_mul(37) & 0xff) as u8)
                .collect::<Vec<_>>();
            let update = RedoUpdate {
                target_block: 7,
                contents: contents.clone(),
            };
            let chunks = encode_redo_chunks(&update, block_size).unwrap();
            let decoded = decode_redo_updates(&chunks, block_size).unwrap();
            assert_eq!(decoded.len(), 1, "block size {block_size}");
            assert_eq!(decoded[0].target_block, 7, "block size {block_size}");
            assert_eq!(
                decoded[0].contents, contents,
                "redo image changed across chunk boundary for block size {block_size}"
            );
        }
    }

    #[test]
    fn decoded_redo_rejects_malformed_chunk_framing() {
        let valid = encode_redo_chunks(&update(0xA5), BS as usize).unwrap();

        let mut cases = Vec::new();

        let mut truncated_header = valid.clone();
        truncated_header[0].truncate(super::UPDATE_CHUNK_HEADER_SIZE - 1);
        cases.push(("truncated header", truncated_header));

        let mut invalid_count = valid.clone();
        invalid_count[0][10..12].copy_from_slice(&1_u16.to_le_bytes());
        cases.push(("invalid chunk count", invalid_count));

        let mut invalid_index = valid.clone();
        invalid_index[0][8..10].copy_from_slice(&2_u16.to_le_bytes());
        cases.push(("out-of-range chunk index", invalid_index));

        let mut mismatched_target = valid.clone();
        mismatched_target[1][..8].copy_from_slice(&(HOME + 1).to_le_bytes());
        cases.push(("target changes mid-image", mismatched_target));

        let mut invalid_length = valid.clone();
        invalid_length[1].pop();
        cases.push(("truncated data", invalid_length));

        cases.push(("missing final chunk", valid[..1].to_vec()));

        let mut out_of_order = valid.clone();
        out_of_order.reverse();
        cases.push(("out-of-order chunks", out_of_order));

        for (case, payloads) in cases {
            assert!(
                matches!(
                    decode_redo_updates(&payloads, BS as usize),
                    Err(Error::Corrupt(_))
                ),
                "malformed redo framing must fail closed: {case}"
            );
        }
    }

    #[test]
    fn duplicate_redo_targets_are_rejected_before_any_persistent_write() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let mut failing = FailAt::new(device.clone(), 0);

        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5), update(0x5A)]),
            Err(Error::InvalidInput(
                "transaction contains duplicate redo targets"
            ))
        ));

        let mut persisted = failing.into_inner();
        assert_eq!(read_home(&mut persisted), vec![0x11; BS as usize]);
        let reopened = open_journal(&mut persisted, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 0);
        assert_eq!(reopened.checkpoint().next_transaction_id, 1);
        assert_eq!(read_home(&mut persisted), vec![0x11; BS as usize]);
    }

    #[test]
    fn recovery_sequence_overflow_is_rejected_before_replaying_home_blocks() {
        let mut device = formatted();
        let checkpoint = crate::JournalCheckpoint {
            generation: 2,
            checkpoint_sequence: u64::MAX - 2,
            next_sequence: u64::MAX - 1,
            next_transaction_id: 1,
            head_block: 0,
            tail_block: 0,
        };
        // Both copies describe the same durable state; otherwise the pair
        // consistency check would correctly reject the fixture before recovery.
        super::write_checkpoint_block(&mut device, REGION.checkpoint_a, &checkpoint).unwrap();
        super::write_checkpoint_block(&mut device, REGION.checkpoint_b, &checkpoint).unwrap();

        // The stream begins at MAX-1. Processing the next record at MAX would
        // require incrementing the expected sequence and must fail before replay.
        let records = [
            crate::JournalRecord {
                kind: crate::JournalRecordKind::Begin,
                sequence: u64::MAX - 1,
                transaction_id: 1,
                payload: Vec::new(),
            },
            crate::JournalRecord {
                kind: crate::JournalRecordKind::Update,
                sequence: u64::MAX,
                transaction_id: 1,
                payload: vec![0xA5],
            },
        ];
        for (slot, record) in records.iter().enumerate() {
            device
                .write_blocks(
                    REGION.ring_start + slot as u64,
                    &record.encode_block(BS as usize).unwrap(),
                )
                .unwrap();
        }
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::ArithmeticOverflow)
        ));
        assert_eq!(
            read_home(&mut device),
            vec![0x11; BS as usize],
            "recovery changed home data before rejecting sequence overflow"
        );
    }

    #[test]
    fn recovery_counter_overflow_is_rejected_before_replaying_home_blocks() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();

        // Leave a durable COMMIT but fail the first home write.
        let mut failing = FailAt::new(device.clone(), 6);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut recovered = failing.into_inner();
        assert_eq!(read_home(&mut recovered), vec![0x11; BS as usize]);

        let exhausted = crate::JournalCheckpoint {
            generation: u64::MAX,
            checkpoint_sequence: 0,
            next_sequence: 1,
            next_transaction_id: 1,
            head_block: 0,
            tail_block: 0,
        };
        super::write_checkpoint_block(&mut recovered, REGION.checkpoint_a, &exhausted).unwrap();
        super::write_checkpoint_block(&mut recovered, REGION.checkpoint_b, &exhausted).unwrap();
        recovered.flush().unwrap();

        assert!(matches!(
            open_journal(&mut recovered, REGION),
            Err(Error::ArithmeticOverflow)
        ));
        assert_eq!(
            read_home(&mut recovered),
            vec![0x11; BS as usize],
            "recovery modified home data before detecting checkpoint generation overflow"
        );
    }

    #[test]
    fn commit_and_reopen_preserve_home_data_and_checkpoint() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        assert_eq!(journal.commit(&mut device, &[update(0xA5)]).unwrap(), 1);
        assert_eq!(read_home(&mut device), vec![0xA5; BS as usize]);
        let reopened = open_journal(&mut device, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 4);
        assert_eq!(read_home(&mut device), vec![0xA5; BS as usize]);
    }

    #[test]
    fn replays_durable_commit_if_home_write_fails() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let mut failing = FailAt::new(device.clone(), 6);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut device = failing.into_inner();
        assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
        let reopened = open_journal(&mut device, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 4);
        assert_eq!(read_home(&mut device), vec![0xA5; BS as usize]);
    }

    #[test]
    fn ignores_uncommitted_transaction_after_commit_write_failure() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let mut failing = FailAt::new(device.clone(), 4);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut device = failing.into_inner();
        let reopened = open_journal(&mut device, REGION).unwrap();
        // The scanner may advance past the valid BEGIN/UPDATE prefix, but must
        // not replay the transaction without its terminal COMMIT.
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 3);
        assert_eq!(read_home(&mut device), vec![0x11; BS as usize]);
    }

    #[test]
    fn malformed_nonterminal_record_before_later_valid_records_fails_closed() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();

        // Fail the first home write after COMMIT is durable. This leaves a committed,
        // uncheckpointed transaction while preserving the old home image.
        let mut failing = FailAt::new(device.clone(), 6);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut recovered = failing.into_inner();
        assert_eq!(read_home(&mut recovered), vec![0x11; BS as usize]);

        // Damage BEGIN while later valid UPDATE/COMMIT records remain. Unlike a malformed
        // terminal append, this is not a safely ignorable tail and must fail closed.
        recovered
            .write_blocks(REGION.ring_start, &vec![0xEE; BS as usize])
            .unwrap();
        recovered.flush().unwrap();
        assert!(matches!(
            open_journal(&mut recovered, REGION),
            Err(Error::Corrupt(
                "malformed journal block appears before a later valid record"
            ))
        ));
        // Recovery must validate the ring before replay; malformed history cannot
        // trigger a partial home-block update.
        assert_eq!(read_home(&mut recovered), vec![0x11; BS as usize]);
    }

    #[test]
    fn torn_terminal_commit_record_is_ignored_and_home_data_is_preserved() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        // One block redo is encoded as multiple chunks; the commit record follows them.
        // Tear inside the fixed header, before the checksum field is fully written.
        // A 64-byte prefix would include the entire empty COMMIT record and is not torn.
        let mut failing = TearWriteAt::new(device.clone(), 3, 20);
        assert!(matches!(
            journal.commit(&mut failing, &[update(0xA5)]),
            Err(Error::Io(_))
        ));
        let mut recovered = failing.into_inner();
        let reopened = open_journal(&mut recovered, REGION)
            .expect("a malformed terminal commit must not make the old state unusable");
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 3);
        assert_eq!(read_home(&mut recovered), vec![0x11; BS as usize]);
    }

    #[test]
    fn torn_home_write_is_repaired_from_durable_redo_record() {
        // Exercise tears at the first byte, within the header-sized prefix, halfway
        // through the block, and at the final byte before the write would be complete.
        for prefix_bytes in [1, 20, BS as usize / 2, BS as usize - 1] {
            let mut device = formatted();
            let mut journal = open_journal(&mut device, REGION).unwrap();
            // Begin + redo chunks + commit are writes 0..=3; the home block is write 4.
            let mut failing = TearWriteAt::new(device.clone(), 4, prefix_bytes);
            assert!(matches!(
                journal.commit(&mut failing, &[update(0xA5)]),
                Err(Error::Io(_))
            ));
            let mut recovered = failing.into_inner();
            let reopened = open_journal(&mut recovered, REGION)
                .expect("durable redo should repair a torn home-block write");
            assert_eq!(
                reopened.checkpoint().checkpoint_sequence,
                4,
                "unexpected checkpoint after a {prefix_bytes}-byte home-write tear"
            );
            assert_eq!(
                read_home(&mut recovered),
                vec![0xA5; BS as usize],
                "redo did not repair a {prefix_bytes}-byte home-write tear"
            );
        }
    }

    #[test]
    fn torn_checkpoint_write_recovers_from_durable_redo_for_multiple_prefixes() {
        // The checkpoint is written only after the commit record and home block are
        // durable. A torn checkpoint must therefore fall back to the older valid copy,
        // replay the still-uncheckpointed transaction, and publish a fresh checkpoint.
        // Tear the checkpoint block at several byte boundaries, including its final byte.
        for prefix_bytes in [1, 20, BS as usize / 2, BS as usize - 1] {
            let mut device = formatted();
            let mut journal = open_journal(&mut device, REGION).unwrap();
            // Writes 0..=3 are journal records, write 4 is the home block, and write 5
            // is the checkpoint copy. Flushes do not count as writes in TearWriteAt.
            let mut failing = TearWriteAt::new(device.clone(), 5, prefix_bytes);
            assert!(matches!(
                journal.commit(&mut failing, &[update(0xA5)]),
                Err(Error::Io(_))
            ));

            let mut recovered = failing.into_inner();
            let reopened = open_journal(&mut recovered, REGION).unwrap_or_else(|error| {
                panic!("recovery failed after a {prefix_bytes}-byte checkpoint tear: {error:?}")
            });
            assert_eq!(
                reopened.checkpoint().checkpoint_sequence,
                4,
                "checkpoint sequence after a {prefix_bytes}-byte tear"
            );
            assert_eq!(
                read_home(&mut recovered),
                vec![0xA5; BS as usize],
                "home image after a {prefix_bytes}-byte checkpoint tear"
            );

            // Recovery itself must be idempotent once the replacement checkpoint is durable.
            let reopened_again = open_journal(&mut recovered, REGION).unwrap();
            assert_eq!(reopened_again.checkpoint().checkpoint_sequence, 4);
            assert_eq!(read_home(&mut recovered), vec![0xA5; BS as usize]);

            // The recovered ring must also be reusable by a subsequent transaction.
            let mut next_journal = open_journal(&mut recovered, REGION).unwrap();
            next_journal
                .commit(&mut recovered, &[update(0x5A)])
                .unwrap();
            let final_journal = open_journal(&mut recovered, REGION).unwrap();
            assert_eq!(final_journal.checkpoint().checkpoint_sequence, 8);
            assert_eq!(read_home(&mut recovered), vec![0x5A; BS as usize]);
        }
    }

    #[test]
    fn transaction_capacity_preflight_rejects_oversized_redo_without_mutation() {
        let mut device = MemoryDevice::new();
        format_journal(&mut device, REGION).unwrap();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let before = device.bytes.lock().unwrap().clone();

        // A 4096-byte redo image needs two ring records; BEGIN and COMMIT
        // consume the other two records per transaction.
        assert_eq!(journal.required_record_count(&device, 3).unwrap(), 8);
        assert!(matches!(
            journal.required_record_count(&device, 4),
            Err(Error::InvalidInput(
                "transaction does not fit in the bounded journal ring"
            ))
        ));

        // Capacity is checked before duplicate-target validation, which would
        // otherwise allocate and walk an arbitrarily large target set.
        let oversized = vec![update(0x33); 4];
        assert!(matches!(
            journal.commit(&mut device, &oversized),
            Err(Error::InvalidInput(
                "transaction does not fit in the bounded journal ring"
            ))
        ));
        assert_eq!(
            *device.bytes.lock().unwrap(),
            before,
            "capacity preflight must be read-only"
        );
    }

    #[test]
    fn rejects_journal_targets_and_ring_exhaustion() {
        let mut device = formatted();
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let bad = RedoUpdate {
            target_block: REGION.ring_start,
            contents: vec![0; BS as usize],
        };
        assert!(matches!(
            journal.commit(&mut device, &[bad]),
            Err(Error::InvalidInput(_))
        ));
        let many = (0..4)
            .map(|index| {
                let mut item = update(index as u8 + 1);
                item.target_block = HOME + index;
                item
            })
            .collect::<Vec<_>>();
        let before = device.bytes.lock().unwrap().clone();
        assert!(matches!(
            journal.commit(&mut device, &many),
            Err(Error::InvalidInput(
                "transaction does not fit in the bounded journal ring"
            ))
        ));
        assert_eq!(
            *device.bytes.lock().unwrap(),
            before,
            "oversized transaction must be rejected before any persistent write"
        );

        // A preflight rejection is not an uncertain I/O result and must not poison
        // the live journal. Prove the same instance can commit a valid transaction.
        journal.commit(&mut device, &[update(0xA5)]).unwrap();
        assert_eq!(read_home(&mut device), vec![0xA5; BS as usize]);
        assert_eq!(journal.checkpoint().checkpoint_sequence, 4);
    }

    #[test]
    fn multi_target_transaction_recovers_to_all_old_or_all_new_images() {
        // A two-home-block transaction is the minimum useful primitive for
        // publishing related filesystem metadata. Fail each I/O boundary in
        // turn, then reopen and ensure recovery never exposes a mixed pair.
        for fail_at in 0..24 {
            let mut device = formatted();
            device
                .write_blocks(HOME + 1, &vec![0x22; BS as usize])
                .unwrap();
            device.flush().unwrap();
            let mut journal = open_journal(&mut device, REGION).unwrap();

            let updates = [
                update(0xA5),
                RedoUpdate {
                    target_block: HOME + 1,
                    contents: vec![0x5A; BS as usize],
                },
            ];
            let mut failing = FailAt::new(device, fail_at);
            let _ = journal.commit(&mut failing, &updates);
            let mut recovered = failing.into_inner();

            // A failed commit may have reached the durable commit point or
            // may not have; reopening must settle on one complete outcome.
            let _reopened = open_journal(&mut recovered, REGION).unwrap_or_else(|error| {
                panic!("recovery failed after injected I/O {fail_at}: {error:?}")
            });
            let first = read_block(&mut recovered, HOME);
            let second = read_block(&mut recovered, HOME + 1);
            let old_pair = first == vec![0x11; BS as usize] && second == vec![0x22; BS as usize];
            let new_pair = first == vec![0xA5; BS as usize] && second == vec![0x5A; BS as usize];
            assert!(
                old_pair || new_pair,
                "transaction exposed a mixed state after injected I/O {fail_at}"
            );

            // A second reopen must preserve the same settled state.
            let settled = (first, second);
            let _reopened_again = open_journal(&mut recovered, REGION).unwrap();
            assert_eq!(read_block(&mut recovered, HOME), settled.0);
            assert_eq!(read_block(&mut recovered, HOME + 1), settled.1);
        }
    }

    #[test]
    fn open_rejects_persisted_checkpoint_sequence_gap() {
        let mut device = formatted();
        let checkpoint = open_journal(&mut device, REGION).unwrap().checkpoint();
        let mut encoded = checkpoint.encode().unwrap();
        // Forge a checksum-valid but semantically inconsistent on-disk record:
        // the public codec now rejects this state before it can be emitted.
        encoded[32..40].copy_from_slice(&(checkpoint.next_sequence + 1).to_le_bytes());
        let checksum = super::crc32c(&encoded[..64]);
        encoded[64..68].copy_from_slice(&checksum.to_le_bytes());
        let mut block = vec![0_u8; BS as usize];
        block[..encoded.len()].copy_from_slice(&encoded);
        device.write_blocks(REGION.checkpoint_a, &block).unwrap();
        device.write_blocks(REGION.checkpoint_b, &block).unwrap();
        device.flush().unwrap();

        assert!(matches!(
            open_journal(&mut device, REGION),
            Err(Error::Corrupt(
                "journal checkpoint sequence counters disagree"
            ))
        ));
    }

    #[test]
    fn protected_open_rejects_journal_region_overlapping_volume_control_blocks() {
        let mut device = formatted();

        for protected in [
            REGION.checkpoint_a,
            REGION.checkpoint_b,
            REGION.ring_start,
            REGION.ring_start + REGION.ring_blocks - 1,
        ] {
            assert!(
                matches!(
                    super::open_journal_protected(&mut device, REGION, &[protected]),
                    Err(Error::InvalidInput(
                        "journal metadata overlaps a protected volume-control block"
                    ))
                ),
                "accepted protected block inside journal region: {protected}"
            );
        }

        // The rejection is performed before reading or modifying journal state.
        let reopened = open_journal(&mut device, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 0);
    }

    #[test]
    fn protected_open_rejects_volume_control_targets_before_writing() {
        let mut device = formatted();
        let original = {
            let mut bytes = vec![0; BS as usize];
            device.read_blocks(0, &mut bytes).unwrap();
            bytes
        };
        let mut journal = super::open_journal_protected(&mut device, REGION, &[0]).unwrap();
        let bad = RedoUpdate {
            target_block: 0,
            contents: vec![0xCC; BS as usize],
        };
        assert!(matches!(
            journal.commit(&mut device, &[bad]),
            Err(Error::InvalidInput(_))
        ));
        let mut after = vec![0; BS as usize];
        device.read_blocks(0, &mut after).unwrap();
        assert_eq!(after, original);
    }

    #[test]
    fn protected_recovery_validates_all_targets_before_replaying_any_update() {
        let mut device = formatted();
        let original_control = {
            let mut bytes = vec![0; BS as usize];
            device.read_blocks(0, &mut bytes).unwrap();
            bytes
        };

        // The first redo target is valid, but the second targets protected block zero.
        // Recovery must validate the whole committed transaction before writing HOME.
        let updates = [
            update(0xA5),
            RedoUpdate {
                target_block: 0,
                contents: vec![0xCC; BS as usize],
            },
        ];
        let mut journal = open_journal(&mut device, REGION).unwrap();
        // Two full-block redo images occupy four UPDATE records: BEGIN and UPDATE
        // writes are 0..=4, flush is 5, COMMIT write/flush are 6/7, HOME is 8.
        let mut failing = FailAt::new(device.clone(), 8);
        assert!(matches!(
            journal.commit(&mut failing, &updates),
            Err(Error::Io(_))
        ));
        let mut recovered = failing.into_inner();

        assert!(matches!(
            super::open_journal_protected(&mut recovered, REGION, &[0]),
            Err(Error::InvalidInput(
                "redo target overlaps a protected volume-control block"
            ))
        ));
        assert_eq!(
            read_home(&mut recovered),
            vec![0x11; BS as usize],
            "recovery partially applied a transaction before validating all targets"
        );
        let mut control_after = vec![0; BS as usize];
        recovered.read_blocks(0, &mut control_after).unwrap();
        assert_eq!(control_after, original_control);
    }

    #[test]
    fn protected_recovery_rejects_durable_redo_to_volume_control_block() {
        let mut device = formatted();
        let original = {
            let mut bytes = vec![0; BS as usize];
            device.read_blocks(0, &mut bytes).unwrap();
            bytes
        };
        let bad = RedoUpdate {
            target_block: 0,
            contents: vec![0xCC; BS as usize],
        };

        // Operation 6 is the home-block write, after the commit record and its
        // flush have succeeded. The durable redo must not overwrite block zero
        // when the volume is reopened with its control blocks protected.
        let mut journal = open_journal(&mut device, REGION).unwrap();
        let mut failing = FailAt::new(device.clone(), 6);
        assert!(matches!(
            journal.commit(&mut failing, &[bad]),
            Err(Error::Io(_))
        ));
        let mut recovered_device = failing.into_inner();
        let result = super::open_journal_protected(&mut recovered_device, REGION, &[0]);
        assert!(matches!(result, Err(Error::InvalidInput(_))));

        let mut after = vec![0; BS as usize];
        recovered_device.read_blocks(0, &mut after).unwrap();
        assert_eq!(after, original);
    }

    #[test]
    fn recovery_io_failure_can_be_retried_without_losing_committed_redo() {
        // Cover failures during replay, its flush, checkpoint write, and
        // checkpoint flush. The transaction must remain consistent each time.
        for fail_at in 6..10 {
            let mut device = formatted();
            let mut journal = open_journal(&mut device, REGION).unwrap();

            // Leave a durable commit whose first home-block write fails.
            let mut failing_commit = FailAt::new(device.clone(), 6);
            assert!(matches!(
                journal.commit(&mut failing_commit, &[update(0xA5)]),
                Err(Error::Io(_))
            ));
            let device = failing_commit.into_inner();

            // Recovery reads two checkpoints and four ring slots before the
            // home write (operation 6), then flushes and checkpoints.
            let mut failing_recovery = FailAt::new(device, fail_at);
            assert!(matches!(
                open_journal(&mut failing_recovery, REGION),
                Err(Error::Io(_))
            ));
            let mut device = failing_recovery.into_inner();

            let reopened = open_journal(&mut device, REGION).unwrap_or_else(|error| {
                panic!("retry failed after recovery I/O {fail_at}: {error:?}")
            });
            assert_eq!(reopened.checkpoint().checkpoint_sequence, 4);
            assert_eq!(read_home(&mut device), vec![0xA5; BS as usize]);
        }
    }

    #[test]
    fn every_multi_block_commit_io_failure_recovers_all_home_blocks_together() {
        // Each full-block redo image is split into two UPDATE records. The two-image
        // transaction therefore uses BEGIN + four UPDATEs + COMMIT in this test's
        // eight-slot ring. There are thirteen fallible I/O operations: five pre-commit
        // writes, pre-commit flush, commit write/flush, two home writes, home flush,
        // checkpoint write/flush. Once COMMIT is written, recovery must replay both
        // images, even if only one home block reached disk.
        for fail_at in 0..13 {
            let mut device = formatted();
            device
                .write_blocks(HOME + 1, &vec![0x22; BS as usize])
                .unwrap();
            let mut journal = open_journal(&mut device, REGION).unwrap();
            let updates = [
                update(0xA5),
                RedoUpdate {
                    target_block: HOME + 1,
                    contents: vec![0xB6; BS as usize],
                },
            ];
            let mut failing = FailAt::new(device.clone(), fail_at);
            assert!(
                matches!(journal.commit(&mut failing, &updates), Err(Error::Io(_))),
                "operation {fail_at} should return the injected I/O error"
            );

            let mut recovered_device = failing.into_inner();
            let mut reopened =
                open_journal(&mut recovered_device, REGION).unwrap_or_else(|error| {
                    panic!("multi-block recovery failed at operation {fail_at}: {error:?}")
                });
            let committed = fail_at >= 7;
            let expected_first = if committed { 0xA5 } else { 0x11 };
            let expected_second = if committed { 0xB6 } else { 0x22 };
            assert_eq!(
                read_block(&mut recovered_device, HOME),
                vec![expected_first; BS as usize],
                "first home block differs after operation {fail_at}"
            );
            assert_eq!(
                read_block(&mut recovered_device, HOME + 1),
                vec![expected_second; BS as usize],
                "second home block differs after operation {fail_at}"
            );
            // An abandoned BEGIN/UPDATE prefix may advance the checkpoint sequence
            // while being retired; the safety invariant here is the paired home state.
            let _checkpoint = reopened.checkpoint();

            // Recovery must leave the journal able to commit another multi-target
            // transaction, not just repair the interrupted one.
            let follow_up = [
                update(0xC3),
                RedoUpdate {
                    target_block: HOME + 1,
                    contents: vec![0xD4; BS as usize],
                },
            ];
            reopened
                .commit(&mut recovered_device, &follow_up)
                .unwrap_or_else(|error| {
                    panic!(
                        "post-recovery multi-block commit failed at operation {fail_at}: {error:?}"
                    )
                });
            assert_eq!(
                read_block(&mut recovered_device, HOME),
                vec![0xC3; BS as usize]
            );
            assert_eq!(
                read_block(&mut recovered_device, HOME + 1),
                vec![0xD4; BS as usize]
            );
        }
    }

    #[test]
    fn every_commit_io_failure_recovers_to_a_consistent_home_block() {
        // One full-block redo image produces ten I/O operations:
        // 3 pre-commit records, flush, commit record, flush, home write,
        // flush, checkpoint write, flush. Before the commit record is durable,
        // recovery must keep the old home block; after it may replay the new one.
        for fail_at in 0..10 {
            let mut device = formatted();
            let mut journal = open_journal(&mut device, REGION).unwrap();
            let mut failing = FailAt::new(device.clone(), fail_at);
            assert!(
                matches!(
                    journal.commit(&mut failing, &[update(0xA5)]),
                    Err(Error::Io(_))
                ),
                "operation {fail_at} should return the injected I/O error"
            );
            assert!(
                matches!(
                    journal.commit(&mut device, &[update(0xBC)]),
                    Err(Error::InvalidInput(
                        "journal requires recovery after a previous I/O error"
                    ))
                ),
                "operation {fail_at} must poison the live writer until recovery"
            );
            let mut recovered_device = failing.into_inner();
            let mut reopened =
                open_journal(&mut recovered_device, REGION).unwrap_or_else(|error| {
                    panic!("recovery failed at operation {fail_at}: {error:?}")
                });
            let expected_byte = if fail_at <= 4 { 0x11 } else { 0xA5 };
            assert_eq!(
                read_home(&mut recovered_device),
                vec![expected_byte; BS as usize],
                "unexpected home image after failure at operation {fail_at}"
            );
            let expected_sequence = match fail_at {
                0 => 0,
                1 => 1,
                2 => 2,
                3 | 4 => 3,
                _ => 4,
            };
            assert_eq!(
                reopened.checkpoint().checkpoint_sequence,
                expected_sequence,
                "unexpected checkpoint after failure at operation {fail_at}"
            );

            // Successful recovery must restore a usable writer, not merely expose
            // a readable home image. A fresh commit must persist and remain readable.
            reopened
                .commit(&mut recovered_device, &[update(0xC3)])
                .unwrap_or_else(|error| {
                    panic!("post-recovery commit failed at operation {fail_at}: {error:?}")
                });
            assert_eq!(
                read_home(&mut recovered_device),
                vec![0xC3; BS as usize],
                "writer did not resume after recovery at operation {fail_at}"
            );
        }
    }

    #[test]
    fn preflight_rejections_leave_journal_and_image_unchanged() {
        let mut device = formatted();
        let mut journal = open_journal_protected(&mut device, REGION, &[HOME + 1]).unwrap();

        let duplicate_target = [update(0x22), update(0x33)];
        let wrong_sized_image = [RedoUpdate {
            target_block: HOME,
            contents: vec![0x44; BS as usize - 1],
        }];
        let journal_target = [RedoUpdate {
            target_block: REGION.ring_start,
            contents: vec![0x55; BS as usize],
        }];
        let protected_target = [RedoUpdate {
            target_block: HOME + 1,
            contents: vec![0x66; BS as usize],
        }];

        for (label, invalid_updates) in [
            ("duplicate targets", duplicate_target.as_slice()),
            ("wrong-sized redo image", wrong_sized_image.as_slice()),
            ("journal metadata target", journal_target.as_slice()),
            ("protected volume target", protected_target.as_slice()),
        ] {
            let before = device.bytes.lock().unwrap().clone();
            assert!(
                matches!(
                    journal.commit(&mut device, invalid_updates),
                    Err(Error::InvalidInput(_))
                ),
                "{label} must be rejected during preflight"
            );
            assert_eq!(
                *device.bytes.lock().unwrap(),
                before,
                "{label} must not modify any persistent byte"
            );
        }

        // Validation errors occur before the writer is poisoned. A valid operation
        // should work immediately without reopening or running recovery.
        journal.commit(&mut device, &[update(0xA6)]).unwrap();
        assert_eq!(read_home(&mut device), vec![0xA6; BS as usize]);
        let reopened = open_journal(&mut device, REGION).unwrap();
        assert_eq!(reopened.checkpoint().checkpoint_sequence, 4);
        assert_eq!(read_home(&mut device), vec![0xA6; BS as usize]);
    }
}
