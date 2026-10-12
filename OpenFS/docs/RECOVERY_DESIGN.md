# Recovery and journal design

**Status: experimental record codec implemented; transaction execution and recovery are not implemented.**

The legacy C project contains a write-ahead journal, transaction buffering, replay logic, checkpoint handling, and failure-injection tests. These are useful concepts, but their guarantees must be specified independently before a Rust port.

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

This is a **record codec only**, not a durable log protocol. It does not validate ordering or assemble transactions; it does not write or flush journal data, perform replay, checkpoint, or guarantee crash consistency. The on-disk layout remains experimental and may change before a stable format is declared.

## Required protocol work

Before P2 is considered complete, define and implement:
- transaction record ordering, sequence allocation, transaction boundaries, and how a commit becomes durable;
- exact persistence barriers between log payload, commit record, home-block writes, and checkpoint;
- behavior for short/failed writes and flush errors at every stage;
- which post-crash states are accepted and how ambiguous/corrupt records are handled;
- idempotent replay rules and when recovery may clear journal state;
- what errors force the mounted instance to become unavailable pending remount/recovery;
- tests that crash or fail I/O before/after each persistence boundary and verify disk contents after repeated reopen.

A successful call to the host flush API is not proof that every storage controller honors durable ordering. No current Rust code claims crash consistency.
