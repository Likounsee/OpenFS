# Recovery and journal design

**Status: experimental low-level journal engine implemented; not yet integrated into the v1 volume layout or allocator.** The record codec, bounded block-framed ring, dual checkpoints, serialized commit path, redo replay, and basic I/O-failure tests exist. Volume formatting still does not reserve this region, and the filesystem must not claim end-to-end crash consistency.

## Compatibility decision

The Rust implementation defines a **new OpenFS on-disk format**, independent of historical OpenFS C format v1.6. Rust must reject legacy images rather than guess at their layout. A future converter may be added, but compatibility with legacy v1.x is not a requirement for the first Rust format. The current formatter and records remain experimental; no stable on-disk compatibility promise exists yet.

This choice does not mean the current draft is frozen. Before persistent namespace data is written, the superblock/layout specification must reserve and validate the journal location and capacity, and the implementation must include versioned golden image fixtures. Do not hide journal geometry in bytes currently specified as reserved-zero.

## Current record codec (draft, version 1)

Each record is encoded explicitly in little-endian form. The fixed 40-byte header is followed by a bounded payload.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic: `OFSJNL\0\0` |
| 8 | 2 | Journal record version (1) |
| 10 | 2 | Header size (40) |
| 12 | 2 | Record kind: Begin=1, Update=2, Commit=3, Abort=4 |
| 14 | 2 | Reserved flags; must be zero |
| 16 | 8 | Monotonically assigned record sequence; zero is invalid |
| 24 | 8 | Transaction identifier; zero is invalid |
| 32 | 4 | Payload length |
| 36 | 4 | CRC32C |
| 40 | variable | Payload; at most 1 MiB |

CRC32C covers bytes 0–35 and the payload, excluding the checksum field. Begin, Commit, and Abort records must have empty payloads. Update payloads are opaque redo data at this stage. The decoder requires exactly one complete record: truncated data, trailing bytes, unknown kinds, unsupported versions, non-zero reserved flags, invalid identifiers and checksum mismatches are rejected.

This codec is not itself a durable log protocol. The future writer must either extend the record protocol with explicit checkpoint framing or define an independently checksummed, versioned journal header/checkpoint area. It must not treat a successful decode of isolated records as proof of a valid transaction stream.

## Transaction and recovery contract to implement

The initial implementation should use a **single serialized writer**. Parallel transactions and lock-free journal reservation are explicitly out of scope until the single-writer protocol is tested. `PersistentJournal` is intentionally non-`Clone` so an opened writer state cannot be duplicated in memory; callers must also ensure only one journal handle is opened for a given region at a time.

### Commit path

1. Validate the complete mutation in memory before publishing any home-block change. Construct redo updates for every affected block, including allocation metadata and metadata checksums.
2. Append a BEGIN record and all UPDATE records for one transaction, with strictly increasing sequence numbers. Transaction IDs must not be reused while any record for that transaction can remain in the journal.
3. Flush the journal. If any write or flush fails, report the transaction as not committed; do not write home blocks. Because persistence may be uncertain after an I/O error, stop accepting mutations until recovery/reopen has resolved the on-disk state.
4. Append COMMIT and flush again. **The operation may be acknowledged as committed only after this flush succeeds.** If the write/flush returns an error, the caller must treat the outcome as indeterminate and the mounted instance must require recovery; never claim rollback merely because the API returned an error.
5. Apply the transaction's redo images to home blocks, then flush the device.
6. Persist a checkpoint that advances only over transactions whose home-block writes and flush completed, then flush the checkpoint metadata before reusing their journal space.

The log format must support detecting a full journal before starting a transaction. The first implementation must fail safely with a no-space error; it must not overwrite uncheckpointed records.

### Recovery path

1. On open, validate the superblock copies, journal geometry, journal header/checkpoint metadata, and all relevant bounds before exposing namespace operations.
2. Scan records in sequence order from the persisted checkpoint. Reject duplicate/decreasing sequences, invalid transaction boundaries, illegal record-kind ordering, transaction-ID mismatches, impossible lengths, and committed transactions whose redo set cannot be validated completely.
3. Assemble and validate each transaction fully before applying any of its redo records. A transaction without a valid COMMIT is uncommitted and must not be replayed. A valid COMMIT whose transaction is incomplete or corrupt is a hard recovery error, not something to silently skip.
4. Replay committed transactions in commit/sequence order. Redo application must be idempotent: repeating recovery after a crash during replay converges to the same bytes.
5. Flush home blocks before advancing the checkpoint. Persist and flush the checkpoint before making those journal slots reusable.
6. Do not expose the mounted filesystem until recovery and all required validation have succeeded. If recovery cannot determine a safe state, fail the mount and preserve the evidence for diagnostics rather than performing speculative repair.

### Tail and corruption policy

A future block-framed journal must distinguish a torn/incomplete final append from corruption in previously persisted records. Only a provably incomplete terminal append may be discarded, and only when no valid durable COMMIT depends on it. CRC mismatch, sequence gaps, or malformed records within the committed/checkpointed region must fail closed. Do not implement “stop at first bad record” as a general recovery policy; that could silently lose an acknowledged transaction.

### Device failures and uncertain persistence

- Any failed journal write or flush before COMMIT prevents acknowledgement; recovery decides whether a valid COMMIT reached storage.
- A failed COMMIT flush makes the result indeterminate. The instance becomes unavailable for further writes until remount/recovery.
- A failed home write or home flush after a durable COMMIT leaves a committed transaction to replay.
- A failed checkpoint write/flush must not permit journal space reuse.
- The block-device flush contract is a host synchronization request, not a guarantee against every controller or physical-device failure. Tests establish software ordering and recovery behavior, not hardware guarantees.

## Required test matrix

Use deterministic failure injection and reopen a fresh device handle after each case. Verify actual persisted bytes, not just return codes.

- Fail every journal write and each flush in turn; retry/reopen and verify the transaction is either fully absent or fully replayed, never partially visible.
- Tear the final BEGIN, UPDATE, and COMMIT append at several byte/block boundaries.
- Crash after durable COMMIT; after each home-block write; after home flush; during checkpoint write; and after checkpoint flush.
- Replay the same image repeatedly, including a crash during replay, and assert identical final home blocks.
- Inject malformed ordering, duplicate/gapped sequences, reused transaction IDs, invalid lengths, bad checksums, missing BEGIN, duplicate COMMIT, ABORT after COMMIT, and corruption inside versus after the last valid append.
- Verify journal exhaustion never overwrites uncheckpointed data.
- Verify every indeterminate I/O outcome blocks further mutations until recovery.
- Once persistent namespace operations exist, repeat these cuts for create, unlink, rename, and allocation/free transitions, comparing object identities as well as path names.

## Remaining integration prerequisites

1. Extend the experimental volume-format contract with a versioned, bounded journal region and checkpoint metadata; do not repurpose reserved-zero superblock bytes silently. Bump the on-disk format version before reserving blocks that v1 treated as data.
2. Add golden image fixtures for the revised layout and explicitly reject older draft images unless a migration is implemented.
3. Integrate the low-level engine's reserved region into `VolumeLayout`, format/open validation, and the allocation bitmap so the allocator cannot return journal blocks.
4. Expand crash-cut tests to cover torn checkpoint writes, each home-block write in a multi-block transaction, replay interrupted during application, and repeated recovery against reopened file-backed images.
5. Only then integrate persistent metadata and namespace mutations. The engine currently uses a bounded epoch ring that reuses slots only after a durable checkpoint; it is not a mount implementation.

No current Rust code claims crash consistency.
