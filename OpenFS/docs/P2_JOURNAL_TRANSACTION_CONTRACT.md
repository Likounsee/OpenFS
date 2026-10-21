# P2 journal transaction and recovery contract

**Status: implementation contract for the current bounded redo journal.** It describes journal-level guarantees only; it does not claim that every persistent file/namespace operation is one atomic transaction or that physical-device power-loss durability has been qualified.

## Persistent layout and capacity

The canonical version 4 layout reserves two checkpoint blocks and a 16-block journal ring. Earlier supported Rust layouts use version-specific geometry, including a four-block ring; never apply the current geometry to an older image. Each transaction uses BEGIN, one or more UPDATE records, and COMMIT records. Updates may be chunked. The engine validates targets, duplicate targets, image sizes and complete record capacity before the first persistent transaction write.

A capacity rejection must occur before persistent bytes change. Ring capacity is a format constraint, not a best-effort limit: uncheckpointed records must never be overwritten. Any ring-size or record-encoding change requires an explicit compatibility decision.

## Journal state machine

1. **Preflight:** validate target blocks, uniqueness, image sizes, arithmetic and complete ring capacity without persistent mutation.
2. **Prepare:** write BEGIN and UPDATE records, then flush. No valid COMMIT means recovery must not apply the transaction.
3. **Commit decision:** write COMMIT and flush. If persistence may have succeeded but the call returns an I/O error, the result is indeterminate.
4. **Home application:** apply committed redo images to home blocks and flush them.
5. **Checkpoint:** persist and flush the checkpoint only after home application; do not reuse covered ring slots before the checkpoint is durable.
6. **Ready:** continue only after successful completion. If an I/O failure leaves state uncertain, the current writer must not be reused; reopen and recover.

## Error and retry rules

- Writable open must recover the journal before returning a volume handle.
- After an uncertain commit, home write or checkpoint result, do not retry mutations through the same possibly stale instance.
- A caller must not infer abort solely from an I/O error after COMMIT may have reached storage.
- Recovery failure means no writable volume may be exposed.
- Redo targets must be unique, in-range home blocks and must not overlap the superblocks, checkpoints or journal ring.
- A successful flush means only what the specific BlockDevice implementation promises.

## Interaction with persistent file operations

Persistent create, rename, remove, file-content write, range write and truncate APIs exist. Their presence does not mean every multi-step operation is represented by a single redo transaction. Descriptor replacement has a specific journal-backed bitmap ownership transition, while file data, extent-map and inventory transitions may cross multiple persistence stages.

When extending these paths, preserve the following invariants:
- Do not publish a reference to data or metadata before the referenced contents are written and flushed as required by that operation's contract.
- Do not free old storage while the currently discoverable namespace may still reference it.
- Preflight bounded journal capacity before staging a transition that depends on one journal commit point.
- If the commit result is uncertain, reopen, recover and inspect the discovered state before deciding whether a higher-level operation should be retried.
- Do not claim atomic multi-object semantics unless the complete set of affected home blocks and metadata is covered by the same recovery-visible decision.

## Verification evidence and known limits

The code contains deterministic I/O fault injection and selected journal write/tear/replay/checkpoint scenarios. This is useful software-ordering evidence, not exhaustive hardware qualification. Remaining engineering work is to close the operation-specific ordering/recovery gaps, prevent stale concurrent writers, and ensure the checker can distinguish safe recoverable states from corruption without speculative repair.

Do not add tests merely to increase their count. Add failure cases when they validate a concrete persistence invariant or protect a known regression. Keep exact-SHA Linux and Windows CI results with each substantive change.

## Change control

Any change to record encoding, checkpoint selection, transaction sequencing, ring capacity, protected target ranges or recovery interpretation must update this contract and any directly affected fixtures. Progress should be awarded for implemented guarantees, not for documentation or test volume alone.
