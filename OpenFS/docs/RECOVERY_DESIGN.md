# Recovery and journal design

**Status: experimental journal engine integrated into the canonical Rust volume layout.** Writable volume open validates and recovers the bounded journal before returning. The journal protects allocator bitmap transitions and is used by descriptor ownership publication. This is not proof that every multi-step file mutation is atomic, and it is not physical-device power-loss qualification.

## Format compatibility

The Rust implementation defines its own format, independent of historical OpenFS C format v1.6. The active canonical layout is version 4 and reserves two checkpoint blocks plus a 16-block redo ring. Earlier supported Rust format versions use their own version-specific layouts, including a four-block ring. Readers must reject unknown versions rather than guess or reinterpret geometry. No stable production compatibility promise is made.

## Commit and recovery contract

1. Validate the volume geometry, both superblocks, journal region and checkpoint metadata before exposing writable operations.
2. Preflight the entire redo transaction: target bounds, duplicate targets, protected regions, image lengths, counters and ring capacity. A failed preflight must not change persistent bytes.
3. Write BEGIN and UPDATE records, then flush. Write COMMIT and flush only after the prepared records are durable.
4. After durable COMMIT, apply redo images to home blocks and flush them before advancing the checkpoint.
5. Persist and flush the checkpoint before reusing ring slots covered by it.
6. On reopen, validate journal sequence and transaction framing, assemble each transaction before applying it, and replay committed redo idempotently. A committed but malformed/incomplete transaction must fail closed.
7. If the writer receives an I/O error after persistence may have begun, treat the result as uncertain. Reopen through the normal recovery path before any further write.

The journal engine is single-writer in its intended usage. A non-clonable in-memory journal handle does not prevent another process or independently opened handle from writing the same volume. Exclusive-writer enforcement remains a separate requirement.

## Persistent namespace and file mutations

Persistent namespace and file APIs are implemented in `volume.rs`. Do not describe them as wholly absent. Their crash-consistency contract must nevertheless be assessed operation by operation: data writes, extent-map updates, namespace/inventory publication and reclamation can span multiple persistence steps and are not automatically one journal transaction.

The critical ordering rule is that old blocks cannot be reclaimed while the discoverable inventory may still reference them, and new references cannot become visible before their contents are sufficiently durable. When an I/O error makes publication uncertain, recover and rediscover before retrying.

## Corruption policy

Reject invalid record framing, sequence discontinuities, invalid transaction boundaries, duplicate/protected targets, malformed committed redo payloads and impossible checkpoint state. A terminal torn append may only be tolerated where the implementation can prove it is an incomplete terminal record and no valid committed transaction depends on it. Never silently skip corruption that may conceal an acknowledged transaction.

## Device failure boundary

Host flush calls and file-backed fault injection test software-level ordering and recovery behavior. They do not model every controller cache, sector tear, lost acknowledged write, disconnect or dishonest flush implementation. Do not use important data or physical devices for early development.

## Remaining prerequisites

- Complete the operation-specific persistence and recovery ordering for file-data, extent-map and namespace mutations.
- Enforce exclusive writers at the appropriate device/volume layer.
- Keep layout/version documentation aligned with the actual constants and version-specific decoding.
- Improve large-volume discovery/checker scaling and avoid infallible allocations sized from disk-derived collections.
- Only then advance toward verified raw-device discovery and OS mounting.
