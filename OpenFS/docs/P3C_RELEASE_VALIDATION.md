# P3-C Final Release Validation

## Scope

P3-C is the final validation phase for OpenFS v1.3. Validation follows:

**OBSERVE → REPRODUCE → CLASSIFY → CORRECT → TEST → FULL REGRESSION → CI**

No production change is made solely to increase coverage.

## CRC

**PASS — compatible.**

The v1.3 reader accepts both:
- current full-record CRC32C with the CRC field zeroed;
- historical v1.3 CRC32C over `block_size - 4` bytes, with the reserved final four bytes required to remain zero.

New writers always use the current full-record CRC. The format version remains v1.3.

The compatibility regression reconstructs the historical writer representation and validates:
- old journal record opening;
- committed replay;
- mount;
- remount after checkpoint;
- new-format writer/readability;
- payload CRC corruption rejection;
- legacy reserved-tail corruption rejection.

## Format audit

**PASS.**

Validated:
- primary and backup superblocks;
- version and feature flags;
- geometry and overflow checks;
- inode serialization/checksum/generation;
- allocation bitmaps;
- inline and tree-backed extents;
- directory records and CRC;
- symlink representation;
- journal record structure and CRC;
- explicit little-endian encoding/decoding.

Primary/backup selection is validated by mount tests. Unknown major versions and unsupported minor/feature combinations are rejected.

## Corruption / recovery

**PASS.**

Existing and P3-B coverage exercises minimal corruption of:
- superblocks and backup selection;
- journal records/CRC/sequence/transaction state;
- inodes;
- extents and extent-tree metadata;
- directory references and generations;
- allocation bitmaps;
- fsck geometry and consistency invariants.

Corruption is rejected rather than silently accepted; committed journal state is replayed where recovery is explicitly part of the format lifecycle.

## Stability

**PASS.**

`openfs-stability` performs 24 repeated create/write/read/truncate/rename/link/symlink/unlink/mkdir cycles, with periodic mount/remount and fsck validation. The cycle completed without corruption or persistent allocation failure.

## Memory / resources

**PASS.**

GCC and Clang sanitizer validation passed on the complete 23-test suite. The stability test is included in sanitizer coverage.

## API / errors

**PASS.**

Public invalid-argument, journal-full, corruption, I/O-failure, transaction-failure and recovery-required paths are covered by the existing journal, transaction, mount, fsck, P3-A/P3-B and stability tests. Durable-state-sensitive failures preserve the documented recovery-required behavior.

## Portability

**PASS.**

The complete suite builds/tests with GCC and Clang and passes the Windows job. Serialization uses explicit little-endian encoding, while size/offset arithmetic is guarded at the filesystem/device boundaries.

## Documentation

**PASS.**

README and format documentation now agree with the v1.3 CRC compatibility behavior, P3-B status, release-validation scope, journal behavior and current limitations.

## Release decision

At the pre-documentation validation point:
- CTest: 23/23 PASS
- GCC: SUCCESS
- Clang: SUCCESS
- ASan: SUCCESS
- UBSan: SUCCESS
- Windows: SUCCESS

A final CI run is required after these documentation/test-cleanup commits before P3-C is formally closed.
