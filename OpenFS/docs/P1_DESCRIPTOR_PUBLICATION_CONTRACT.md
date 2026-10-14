# P1 descriptor publication and ownership contract

Status: design contract for the existing initial publication path. This document does not authorize descriptor replacement or persistent namespace mutation.

## Scope and non-goals

The current Rust format has one persistent inventory descriptor. Discovery scans allocated data blocks and must find either no descriptor or exactly one valid descriptor. Initial publication is supported; replacing an existing descriptor is not. This contract describes image-backed journal recovery under the BlockDevice model. It is not proof of physical-device power-loss durability, honest flush semantics, or atomicity on hardware that loses acknowledged writes.

## Persistent states

The descriptor candidate has four relevant states:

1. **Absent** — no allocated data block contains a discoverable descriptor. There may be arbitrary bytes in free blocks, but those bytes are not metadata owners.
2. **Staged, not published** — a candidate free data block has been written with the encoded descriptor and a flush attempted, but its allocation bit has not been committed. Discovery MUST ignore it because discovery considers allocated data blocks only.
3. **Published** — the candidate block's allocation bit is committed through the redo journal, and the descriptor bytes were written and flushed before that commit was attempted. Discovery may return it only if decoding and all descriptor/inventory/ownership checks succeed.
4. **Corrupt or ambiguous** — an allocated candidate has invalid descriptor bytes, multiple allocated descriptors are found, or references/ownership are inconsistent. Discovery and verification MUST fail closed; they MUST NOT choose a winner, overwrite one descriptor, or silently free blocks.

The only supported transition is Absent -> Staged -> Published, or Absent -> Staged -> Absent when the bitmap allocation transaction did not commit. A failed/uncertain I/O result is not itself evidence of which durable state exists: drop the poisoned writer/journal state, reopen, run recovery, and rediscover before any further mutation.

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

Publication ordering is:

1. Validate the existing inventory and its ownership.
2. Select a free candidate block and verify it is not already claimed.
3. Write the full encoded descriptor to the candidate.
4. Flush the device.
5. Commit the bitmap allocation through the journal.
6. Return the descriptor block only if the journal commit reports success.

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
- [ ] Safe descriptor replacement remains explicitly unsupported until a versioned replacement protocol has a single recovery-visible commit point and ownership-transfer tests.

## Known limitations

- Discovery scans allocated data blocks; this is not a scalable canonical locator.
- The current inventory is not a transactional namespace root and does not support durable file/directory mutations.
- Image-backed fault injection does not model all physical storage caches, torn sectors, lost acknowledged writes, or dishonest flush implementations.
