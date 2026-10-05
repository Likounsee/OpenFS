# P3-C Final Release Validation

## Scope

P3-C was reopened after an independent audit of the extent-tree, backup-superblock, physical-extent, Linux adapter, and path-concurrency boundaries.

Validation followed:

**OBSERVE → REPRODUCE → CLASSIFY → CORRECT → TARGETED TEST → FULL REGRESSION → CI**

No production change was made solely to increase coverage. Existing tests and fault injection remain enabled.

## Production corrections

### 1. Backup superblock + extent-tree

**PASS — FIXED.**

Extent-tree read/write paths previously depended on primary-superblock geometry instead of the selected and validated superblock from mount. This was incorrect after primary-superblock corruption with a valid backup.

The extent-tree APIs now receive the selected superblock explicitly. File and fsck paths pass that same validated copy through; the extent layer does not perform a hidden primary-superblock reread or fallback.

A real mount regression now:
- creates a fragmented file large enough to require the extent tree;
- verifies the tree exists with the primary selected;
- corrupts only the primary superblock;
- mounts using the valid backup;
- reads through the backup-selected superblock;
- writes through the backup-selected superblock;
- remounts;
- verifies data and inode metadata;
- runs fsck with zero errors.

### 2. Extent physical data-area bounds

**PASS — FIXED.**

Extent-tree writes now require:

data_start <= physical < data_start + data_blocks

with overflow-safe end validation. The file mutation/validation paths also enforce the selected superblock data-area range.

Regression coverage rejects:
- physical < data_start;
- physical == data_start + data_blocks;
- a journal/metadata block inside [0, block_count);
- tree extents overlapping inline extents.

Fsck validates every extent against the data-area boundary before bitmap/reference accounting.

### 3. Linux write offset overflow

**PASS — FIXED.**

Linux read and write now both reject requests unless the converted offset is representable and:

offset + bytes <= LLONG_MAX

The write-side terminal-range omission was a production bug. A dedicated Linux adapter regression covers a request whose terminal byte range exceeds LLONG_MAX and verifies rejection before the transfer loop.

## Extent-tree API migration

**PASS.**

All extent-tree declarations and call-sites were migrated to the selected-superblock API. GCC and Clang builds compile the complete repository, and the final CI test/sanitizer jobs pass.

Earlier intermediate CI failures were migration/test-fixture errors, not production validation:
- an outdated test call signature;
- a boundary fixture using sb instead of its actual s fixture variable;
- an initial backup regression that used contiguous allocation, so it never actually created more than four extents.

These were corrected without weakening assertions or production checks.

## strtok / concurrency

**DOCUMENTED LIMITATION.**

OpenFS path lookup uses the stateful C strtok tokenizer. The public core path API does not promise general concurrent/reentrant operation. OpenFS/docs/adapters.md now explicitly states that callers must serialize concurrent path operations sharing a process.

No unnecessary parser refactor was made and no release-blocking thread-safety promise was found.

## Historical regression preservation

All historical P1/P2/P3-A/P3-B/P3-C tests remain registered and enabled. No test was removed, disabled, or weakened.

The final CTest suite remains **23/23 tests**, because the new backup-superblock regression is integrated into the existing mount test rather than creating a separate CTest executable.

The final CI also validates:
- GCC;
- Clang;
- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- Windows/MSVC;
- Linux and ArchiaOS adapter tests;
- the complete crash/recovery, namespace, journal, fsck, extent-tree, CRC compatibility, boundary, and stability coverage.

## Documentation

The format documentation now states that mount-selected superblock geometry is propagated to filesystem operations and that extent-tree roots are data-area-only. The extent-tree format section was also normalized to real Markdown line breaks.

The adapters documentation records the core path concurrency limitation.

## Final validation evidence

Final post-correction CI:

- Run #1353 / Run ID 37272685827
- HEAD: 1a4b9294bacd42c262a832824f85489f7cb6f8c3
- conclusion: SUCCESS
- GCC: PASS
- Clang: PASS
- ASan/UBSan: PASS
- Windows: PASS
- CTest: 23/23 PASS

This CI completed after the extent physical-boundary regression was added and therefore validates that regression as well.

## Production bugs found during the reopened audit

**3**

1. Extent-tree paths used primary-superblock geometry instead of the mount-selected validated superblock.
2. Extent-tree physical extents were checked only against device bounds, permitting metadata/reserved blocks.
3. Linux adapter writes omitted the terminal offset + bytes <= LLONG_MAX check.

All three were corrected and regression-covered.

## Test/oracle issues found during the reopened audit

Intermediate migration/fixture issues were found and corrected:
- outdated extent-tree test signatures;
- a boundary-test fixture variable mismatch;
- a backup regression that initially allocated contiguous blocks and therefore did not exercise the extent tree.

No production behavior was weakened to make these tests pass.

## Release decision

**P3-C: CLOSED**

**RELEASE READY: YES**

The release decision is based on the completed final CI above. No known release blocker remains from the reopened audit.
