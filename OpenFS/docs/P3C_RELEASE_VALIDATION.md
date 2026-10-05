# P3-C Final Release Validation

## Scope

P3-C was temporarily reopened after an independent audit found a real production defect in extent-tree root validation. The reopened validation followed:

**OBSERVE → REPRODUCE → CORRECT → TARGETED TEST → FULL REGRESSION → CI**

No production change was made solely to increase coverage. Existing tests and fault injection remain enabled.

## Extent-tree root validation

**PASS — corrected and regression-covered.**

The defect was that the direct extent-tree read/write APIs accepted any non-zero root below device->block_count, allowing a root in metadata/reserved space.

The production fix now validates the root against the filesystem data area before tree I/O:

data_start <= root < data_start + data_blocks

The same invariant is also enforced on the file mutation path before an existing tree root can be reused. Allocation/free paths already use the superblock-aware allocator, and fsck/normal inode validation already reject roots outside the data area.

A dedicated regression constructs a tree inode whose root points at the journal/metadata area, populates that block with a structurally valid extent-tree leaf, and verifies that:
- direct tree read is rejected as corruption;
- direct tree write is rejected as corruption;
- normal file read/write validation rejects the inode.

The first CI attempt exposed a test-fixture oracle/setup error: the fixture initialized extent_count before populating inline extents, causing the existing overlap/record validation to reject the fixture. This was a **test/oracle bug**, corrected by populating the inline extents first and setting the final extent count afterward. No production weakening was made.

## Journal DATA header

**PASS — corrected.**

OPENFS_JOURNAL_BLOCK_DATA_HEADER is now 24U, matching the on-disk OJBD1 DATA payload header. Journal block-write construction uses the constant rather than a duplicated literal, and the P3-B journal-boundary test asserts the layout constant.

## CRC

**PASS — compatible.**

The v1.3 reader accepts both:
- current full-record CRC32C with the CRC field zeroed;
- historical v1.3 CRC32C over block_size - 4 bytes, with the reserved final four bytes required to remain zero.

New writers always use the current full-record CRC. The format version remains v1.3.

The compatibility regression validates historical replay, mount/remount, new-format readability, CRC corruption rejection, and legacy reserved-tail corruption rejection.

## Format audit

**PASS.**

Validated:
- primary and backup superblocks;
- version and feature flags;
- geometry and overflow checks;
- inode serialization/checksum/generation;
- allocation bitmaps;
- inline and tree-backed extents;
- extent-tree root data-area bounds;
- directory records and CRC;
- symlink representation;
- journal record structure, 24-byte DATA payload header and CRC;
- explicit little-endian encoding/decoding.

Primary/backup selection is validated by mount tests. Unknown major versions and unsupported minor/feature combinations are rejected.

## Corruption / recovery

**PASS.**

Existing and P3-B coverage exercises corruption of:
- superblocks and backup selection;
- journal records/CRC/sequence/transaction state;
- inodes;
- extents and extent-tree metadata;
- directory references and generations;
- allocation bitmaps;
- fsck geometry and consistency invariants.

The newly added root regression demonstrates that the normal extent-tree APIs reject a metadata-area root even when the target block itself is a valid-looking extent-tree leaf.

## Stability

**PASS.**

openfs-stability performs 24 repeated create/write/read/truncate/rename/link/symlink/unlink/mkdir cycles, with periodic mount/remount and fsck validation.

## Memory / resources

**PASS.**

GCC and Clang sanitizer validation passed on the complete 23-test suite.

## API / errors

**PASS.**

Public invalid-argument, journal-full, corruption, I/O-failure, transaction-failure and recovery-required paths are covered by the existing journal, transaction, mount, fsck, P3-A/P3-B, stability and extent-tree root regression tests.

## Portability

**PASS.**

The complete suite builds/tests with GCC and Clang and passes the Windows job. Serialization uses explicit little-endian encoding, while size/offset arithmetic is guarded at filesystem/device boundaries.

## Documentation

**PASS.**

README and format documentation now agree on:
- P3-C CLOSED;
- Windows CI validation;
- extent-tree root data-area bounds;
- the 24-byte journal DATA header;
- v1.3 CRC compatibility;
- the current release-validation scope and limitations.

## Final validation evidence

Final code-validation CI before this documentation-only update:

- **Run #1329 / Run ID 37264582582**
- HEAD: a7ec9308c4ce50471efa9242e5c5c214cd059fa8
- conclusion: **SUCCESS**
- GCC: **PASS**
- Clang: **PASS**
- ASan/UBSan: **PASS**
- Windows: **PASS**
- CTest: **23/23 PASS**

The validated suite explicitly passed:
- crash-cut;
- namespace crash;
- journal fine-crash;
- P3-A double-failure;
- P3-B boundary;
- P3-B fsck boundary;
- P3-B journal boundary;
- CRC v1.3 legacy;
- stability;
- extent-tree root regression through openfs-file.

## Production bugs

One new production bug was found and fixed during the reopened P3-C audit:

1. Extent-tree root validation accepted metadata/reserved blocks as tree roots.

Previously identified P3-B production defects remain fixed and covered by regression tests.

## Test / oracle bugs

One new test/oracle bug was found and fixed during this reopened audit:

1. The new extent-tree root regression initialized extent_count before populating inline extents, triggering the existing inline-record validation. The fixture was corrected without changing production behavior.

## Release decision

**P3-C: REOPENED — pending final audit CI**

**RELEASE READY: NO**

No known release blocker remains from this audit.


## Final P3-C reopening audit

The independent audit found and reproduced a production issue in the extent-tree API design: the extent layer previously re-read the primary superblock to validate the tree root. This was incompatible with a valid mount that had selected the backup superblock after primary corruption.

The correction changes extent-tree read/write APIs to receive the already validated superblock selected by the caller. File and fsck extent-tree paths now pass that selected superblock through. Root validation remains data-area-only and retains overflow checks.

The audit also demonstrated a second production issue: extent-tree write validated physical extents only against device block_count, so a metadata/reserved block could be accepted as an extent. The write path now enforces the selected superblock data-area range.

The Linux adapter audit found a real write-side off_t boundary omission: writes checked the converted starting offset but not offset + bytes against LLONG_MAX. The write path now applies the same terminal-range check already used by reads, with a dedicated adapter regression.

The repository contains strtok() in path lookup. The public API does not promise general concurrent/reentrant path calls, so this is classified as a documented limitation rather than a release-blocking production bug. The limitation is now explicit in adapters.md; no unnecessary parser refactor was made.

A dedicated mount regression now creates a real extent-tree-backed file, corrupts only the primary superblock, mounts through the valid backup, reads and writes through the selected superblock, remounts, and runs fsck while checking inode size, block count, tree root and data.

Final release status remains **NO** until the complete post-audit CI run is finished and reviewed.
