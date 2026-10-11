# Recovery design

**Status: design only.** The Rust workspace has no transaction journal or recovery implementation.

The legacy C project contains a write-ahead journal, transaction buffering, replay logic, checkpoint handling, and failure-injection tests. These are valuable concepts, but their guarantees must be specified independently before a Rust port.

Before P2 is considered complete, the design must define:
- transaction record layout, sequence numbers, checksums, and commit marker;
- exact persistence barriers between log payload, commit record, home-block writes, and checkpoint;
- behavior for short/failed writes and flush errors at every stage;
- which post-crash states are accepted and how ambiguous/corrupt records are handled;
- idempotent replay rules and when recovery may clear journal state;
- what errors force the mounted instance to become unavailable pending remount/recovery;
- tests that crash or fail I/O before/after each persistence boundary and verify disk contents after repeated reopen.

A successful call to the host flush API is not proof that every storage controller honors durable ordering. No current Rust code claims crash consistency.
