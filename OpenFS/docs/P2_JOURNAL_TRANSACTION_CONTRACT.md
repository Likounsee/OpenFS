# P2 journal transaction and recovery contract

Status: normative description of the current bounded redo journal. This contract does not claim general namespace atomicity or physical-device power-loss durability.

## Persistent layout and capacity

The current format reserves two checkpoint blocks and a fixed four-block journal ring. A transaction is encoded as one BEGIN record, one or more UPDATE records (a block-sized redo image may require multiple chunks), and one COMMIT record. Every record consumes one ring slot. Before the first persistent write, the writer validates target blocks, rejects duplicate/protected targets, validates each image size, computes the complete record count, and rejects transactions that do not fit in the ring.

For the current four-slot ring, a transaction needs at least two slots for BEGIN and COMMIT, leaving at most two UPDATE-record slots in total. This is not equivalent to a promise that two home blocks always fit: update chunking and device block size determine the exact record count. A capacity rejection must occur before any device write and must leave persisted bytes unchanged. Enlarging the ring or changing the on-disk layout requires an explicit format-compatibility decision and golden fixtures; do not silently change the constant.

## Transaction states

1. **Preflight** — no persistent mutation has started. Invalid targets, duplicate targets, malformed images, counter overflow, and capacity overflow are rejected without writes.
2. **Prepared records** — BEGIN and all UPDATE records are written into ring slots, then flushed. No COMMIT record exists yet, so recovery must not apply the transaction.
3. **Commit decision** — the COMMIT record is written and flushed. Once this may have succeeded, an I/O error is an *indeterminate result*, not proof of abort.
4. **Home application** — committed redo images are written to their home blocks and flushed. Recovery must be able to repeat this application idempotently.
5. **Checkpoint publication** — a newer checkpoint is persisted after home application. Failure here must not undo a committed transaction; reopen/recovery must safely finish or recognize it.
6. **Ready** — the journal may accept a new transaction only after the previous operation completed, or after a successful reopen/recovery.

## Error and retry rules

- Any I/O error after persistent transaction writes begin marks the live journal as requiring recovery. Further commits through that instance must fail closed.
- Do not retry an uncertain transaction on the same journal instance. Drop it, reopen the volume, let recovery run, then inspect the durable state before deciding whether to retry the higher-level operation.
- A caller must not report success until the journal commit operation returns success. If it returns an error, the caller must treat the operation as indeterminate whenever COMMIT may have reached storage.
- Recovery failure means no writable volume may be exposed.
- Redo targets must be unique, in-range home blocks and must not overlap the superblocks, checkpoints, or journal ring.
- A successful flush only means what the BlockDevice implementation promises. Image-backed tests do not establish physical power-loss guarantees or protection against hardware that acknowledges but loses writes.

## Required test evidence

For each journal write, flush, home-write, checkpoint-write and recovery boundary:

- Inject failure and, where supported, partial writes.
- Preserve the exact persisted image before reopening.
- Reopen through the normal recovery path; repeat recovery to prove idempotence.
- Verify every home block is either the complete pre-transaction image or the complete committed image; never accept a mixed multi-target state.
- Verify an oversized transaction is rejected before any persisted byte changes.
- Verify the recovered journal can accept a subsequent transaction.
- Record Linux and Windows results against the exact commit SHA.

## Known gaps

- The existing test suite covers selected write/tear/replay/checkpoint boundaries, not an exhaustive cross-product of every boundary, block size and failure prefix.
- The ring is intentionally bounded; no larger/versioned journal geometry has been implemented.
- Durable create/write/rename/delete operations and atomic namespace transactions are not implemented.
- Image-backed tests are not physical-device durability qualification.

## Change control

Any change to record encoding, checkpoint selection, transaction sequencing, ring capacity, protected target ranges, or recovery interpretation must update this contract and the corresponding golden/corruption tests in the same change. Do not increase P2 progress merely because this document exists; award implementation progress only when acceptance criteria are implemented and evidenced.
