# P1 descriptor publication and ownership contract

Status: contract for initial publication and descriptor replacement in the current Rust image-backed implementation. This does not authorize persistent namespace/file mutation or claim physical-device power-loss durability.

## Scope and non-goals

The current Rust format has one persistent inventory descriptor at a time. Discovery scans allocated data blocks and must find either no descriptor or exactly one valid descriptor. Initial publication and replacement are implemented. Replacement stages and flushes the new descriptor while its block is free, then uses one journal COMMIT to switch allocation ownership across every affected bitmap page: the new descriptor becomes allocated, the old descriptor is freed, and obsolete old inventory blocks are released. Journal-capacity preflight happens before staging. If the requested transition does not fit the bounded journal, it is rejected without staging. This contract describes image-backed journal recovery under the BlockDevice model; it is not proof of physical-device power-loss durability, honest flush semantics, or atomicity on hardware that loses acknowledged writes.

## Persistent states

The descriptor candidate has four relevant states:

1. **Absent** — no allocated data block contains a discoverable descriptor. There may be arbitrary bytes in free blocks, but those bytes are not metadata owners.
2. **Staged, not published** — a candidate free data block has been written with the encoded descriptor and a flush attempted, but its allocation bit has not been committed. Discovery MUST ignore it because discovery considers allocated data blocks only.
3. **Published** — the candidate block's allocation bit is committed through the redo journal, and the descriptor bytes were written and flushed before that commit was attempted. Discovery may return it only if decoding and all descriptor/inventory/ownership checks succeed.
4. **Corrupt or ambiguous** — an allocated candidate has invalid descriptor bytes, multiple allocated descriptors are found, or references/ownership are inconsistent. Discovery and verification MUST fail closed; they MUST NOT choose a winner, overwrite one descriptor, or silently free blocks.

For initial publication, the supported transition is Absent -> Staged -> Published, or Absent -> Staged -> Absent when the bitmap allocation transaction did not commit. For replacement, recovery must expose the complete old descriptor and ownership state if the transaction did not commit, or the complete new descriptor and ownership state if it did; mixed bitmap-page states are forbidden. Replacing a descriptor with byte-identical inventory is idempotent and repairs the locator hint without allocating another descriptor. A failed/uncertain I/O result is not itself evidence of which durable state exists: discard the current writer/journal state, reopen, run recovery, and rediscover before any further mutation.

## Ownership invariants

Before staging a descriptor:

- Every metadata and directory block named by the descriptor must be inside the data region and allocated in the bitmap.
- The metadata and directory lists must not overlap.
- The namespace and cross-structure ownership verifier must report no unresolved ownership anomalies.
- The candidate descriptor block must be free and must not be in either inventory list.

After the allocation transaction commits:

- The descriptor block is allocated and owned by the persistent descriptor, not by a metadata record, directory, extent map, or file payload.
- Every inventory block has one unambiguous owner under the current inventory model.
- A generic free API MUST reject freeing the descriptor block.
- If a transaction did not commit, the staged block remains logically free. Before reusing a free block whose bytes start with descriptor magic, allocation MUST clear and flush those bytes before publishing another owner. If clearing or flushing fails, allocation MUST abort without setting the allocation bit.

No cleanup path may guess that an allocated orphan is safe to free. Orphan repair is not implemented and is outside this contract.

## Commit point and error handling

Initial publication ordering is:

1. Validate the inventory and its ownership.
2. Select a free candidate block and verify it is not already claimed.
3. Write the full encoded descriptor to the candidate.
4. Flush the device.
5. Commit the bitmap allocation through the journal.
6. Return the descriptor block only if the journal commit reports success.

Replacement ordering is:

1. Discover and validate the old descriptor and validate the proposed replacement inventory.
2. Reject unrelated orphans, dangling references, diagnostic truncation, and invalid ownership transitions.
3. Select a free descriptor block and assemble all bitmap-page redo images for allocating the new descriptor, freeing the old descriptor, and releasing old inventory blocks no longer referenced.
4. Preflight the number of journal records required for all bitmap images; reject an oversized transaction before staging bytes.
5. Write and flush the new descriptor while its allocation bit remains clear.
6. Clear cached discovery state and commit all bitmap images in one journal transaction. This is the only replacement visibility point.
7. Persist the locator hint and update in-memory discovery state only after commit succeeds. On uncertain commit/hint errors, reopen and rediscover; retrying an identical replacement must not create a second descriptor.

The journal's durable COMMIT record is the transaction decision point; home-bitmap application and checkpointing may follow. If an error occurs after COMMIT may have been written, the caller MUST treat the result as indeterminate, discard the current journal/writer instance, reopen and recover, then rediscover. The current API does not expose a general-purpose poisoned-volume type, so callers must not retry mutations on the same failed instance. A failed open/recovery means no writable state may be exposed.

If the staging write or staging flush fails before journal commit begins, publication returns an error and does not intentionally set the allocation bit. A later open must still ignore unallocated staging bytes. The implementation must not claim physical durability beyond the BlockDevice contract.

## Required failure-injection matrix

Run every case against each supported block size: 4096, 8192, 16384, 32768, and 65536 bytes. Capture persisted image bytes, close/reopen the image, allow journal recovery, then inspect discovery, bitmap ownership, and verification.

| Injected boundary | Required post-reopen result |
| --- | --- |
| Descriptor staging write fails before any bytes | No descriptor discovered; candidate remains free |
| Descriptor staging write tears at each tested prefix | No descriptor discovered; candidate remains free |
| Descriptor staging flush fails | No descriptor discovered unless bitmap commit was subsequently started (it must not be); candidate remains free |
| Journal BEGIN/UPDATE/COMMIT write fails before durable COMMIT | No descriptor discovered; candidate remains free |
| Journal COMMIT write may have persisted but reports failure | Reopen determines outcome by recovery: descriptor absent if transaction is uncommitted, or one complete valid descriptor if COMMIT is durable |
| Journal commit flush reports failure | Same old-or-complete rule; never expose partial bitmap state after recovery |
| Home bitmap write or its flush fails after COMMIT | Recovery must finish the committed bitmap update; one complete descriptor is discoverable |
| Checkpoint write/flush fails after home update | Reopen/recovery is repeatable; one complete descriptor is discoverable and no second descriptor is created |
| Reopen/recovery fails | No writable volume may be returned; caller must not continue mutations |
| Retry after an uncommitted staging attempt | Stale magic is cleared and flushed before another allocation owns the candidate |

Tests must also assert that unrelated image bytes are unchanged, inventory blocks remain allocated, the descriptor block does not alias the inventory, and repeated discovery/recovery produces the same outcome. The five block sizes and any boundary counts must be stated in test names or assertion messages.

## Exit criteria for this P1 slice

- [ ] The code's failure behavior matches this contract, or this document is amended to reflect a justified safer implementation.
- [ ] Each persistent write/flush boundary has a named regression test, including staging-write prefixes and journal recovery.
- [ ] Tests assert old-or-complete state, byte preservation, unique discovery and ownership consistency after reopen.
- [ ] Linux and Windows formatting, workspace tests and Clippy pass for the exact implementation SHA.
- [x] Descriptor replacement uses one recovery-visible journal commit point for all affected bitmap pages; regression coverage includes injected write failures and replacements spanning bitmap pages. Exact-SHA CI for the current revision is tracked separately.

## Known limitations

- Discovery scans allocated data blocks; this is not a scalable canonical locator.
- The current inventory is not a transactional namespace root and does not support durable file/directory mutations.
- Image-backed fault injection does not model all physical storage caches, torn sectors, lost acknowledged writes, or dishonest flush implementations.
