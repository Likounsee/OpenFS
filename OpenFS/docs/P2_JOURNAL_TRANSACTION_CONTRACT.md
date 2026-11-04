> **État courant — 2026-11-04 :** la récupération traite désormais les queues de journal interrompues : les enregistrements non validés après une lacune sont ignorés/nettoyés ; un COMMIT après une lacune reste une corruption et entraîne un échec fermé. Les erreurs d'E/S restent distinctes des corruptions. Après reconstruction d'ownership, les blocs alloués mais non référencés sont réclamés via le parcours contrôlé ; la réclamation est refusée si des références pending subsistent. La CI inclut un crash sweep release et des coupures pendant la réclamation, simulées en mémoire avec plusieurs politiques de perte de cache. Aucun de ces tests ne certifie les flushes d'un support physique réel.

---

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

## Torn-prepare tolerance (power cut inside the unflushed window)

BEGIN and UPDATE records are written before the first flush, so a power cut may persist them in any order or only in part. Recovery therefore treats valid post-checkpoint records that follow an empty, stale or malformed ring slot as an **unacknowledged torn prepare** and ignores and clears them, **provided none of them is a COMMIT**. COMMIT is only written after every earlier record of its transaction is durable, so a COMMIT that follows a gap cannot be produced by a crash and still fails closed (`Corrupt`). Records in the contiguous prefix are interpreted exactly as before.

## Orphan reclamation after a crash

Operations may allocate storage before the namespace that references it is published, and reclaim retired storage only after publication is confirmed. A power cut between those steps leaves allocated-but-unreferenced blocks. `OpenedVolume::reclaim_orphaned_blocks` frees them after journal recovery: it requires a published namespace, refuses to run while any dangling reference exists, and frees through the ownership-checked batch reclamation used by file removal. `VolumeSession::open` (the mount lifecycle) and `recover-image` run it; `audit-image` does not, because an audit reports instead of repairing.

## Power-cut sweep

`crash_sweep.rs` models a volatile write cache (a write is durable only after a successful flush) and injects a power cut at **every** write/flush event of each mutation (create file/directory, content write, range write, append, sparse extension, streamed import, truncate shrink/zero/grow, rename, replace, remove, tree remove). The surviving image is built under four cache-loss scenarios (nothing, everything, even pending blocks, second half of pending blocks). After each cut the volume must reopen, show exactly the old or the new state, have zero orphans and zero dangling references, keep acknowledged operations durable, and remain writable. The full matrix runs with `cargo test --release -p openfs-format crash_sweep`; debug builds run a smoke subset.

## Error and retry rules

- Writable open must recover the journal before returning a volume handle.
- After an uncertain commit, home write or checkpoint result, do not retry mutations through the same possibly stale instance.
- Before every commit, compare the persisted checkpoint epoch and scan for uncheckpointed or malformed records. A stale handle must fail closed and be reopened; this is a stale-handle guard, not a cross-process lock, so callers must still enforce one writer at a time.
- A caller must not infer abort solely from an I/O error after COMMIT may have reached storage.
- Recovery failure means no writable volume may be exposed.
- Redo targets must be unique, in-range home blocks and must not overlap the superblocks, checkpoints or journal ring.
- A successful flush means only what the specific BlockDevice implementation promises.

## Interaction with persistent file operations

Persistent create, rename, remove, file-content write, range write and truncate APIs exist. Their presence does not mean every multi-step operation is represented by a single redo transaction. Descriptor replacement has a specific journal-backed bitmap ownership transition, while file data, extent-map and inventory transitions may cross multiple persistence stages.

When extending these paths, preserve the following invariants:
- For replacement, truncation, and removal, derive the retired file-block allowlist from validated old ownership and prove those blocks are absent from the proposed namespace. During descriptor publication, allow only these known retired blocks and obsolete inventory pages to appear as orphans; unknown orphans and dangling references must still fail closed. Do not free retired file blocks in the descriptor transaction itself: reclaim them only after publication is confirmed, so an uncertain outcome cannot free storage still referenced by the active inventory.
- Sparse growth must zero the retained physical block's bytes beyond the old EOF before publishing a larger size; a prior truncate can have committed its shorter length while a tail-scrub write failed. If the old EOF is in a hole, sparse reads already produce zeroes and no data block needs clearing.
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
