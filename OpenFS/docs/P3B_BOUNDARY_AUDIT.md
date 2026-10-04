# P3-B Boundary Audit

Status: P3-B remains in progress until this audit is reviewed against the final CI.

## B9 arithmetic

Audited the requested paths: journal, transaction, format, inode/inode allocation, file, extent, directory, path, link, fsck and mount, plus their boundary-related headers.

The relevant calculations were classified against their reachable bounds and operand types. Existing hardening covers journal slot and sequence arithmetic; journal record-size multiplication and offsets; inode-table multiplication/addressing; bitmap size/bit addressing; extent physical-end overflow; file offset + length and block-count calculations; directory entry offset multiplication/addition; fsck reference-buffer sizing and range calculations; and transaction buffer growth.

No new production arithmetic BUG was demonstrated during this audit. Calculations that rely on a validated superblock are treated under that contract; malformed-superblock cases are handled by fsck/format validation.

Result: PASS.

## B10 fsck boundary matrix

`tests/test_p3b_fsck_boundary.c` exercises one-corruption images for block-count geometry; inode-table/inode-count geometry; inode number zero and out-of-range; generation; link count; extent offset, zero length, overlap and extent count; directory inode reference; directory generation reference; parent inode; journal start and length bounds; journal record size; journal txid and sequence; inode bitmap inconsistency; block bitmap inconsistency; and bitmap bits outside the device range.

Each case starts from a freshly formatted image and expects fsck to reject the corruption without requiring errors > 0 for early validation failures.

A real fsck production bug was found: a directory-referenced inode whose allocation bitmap bit was cleared was previously skipped by the final inode scan. fsck now increments the corruption count for that condition.

Result: PASS.

## B11 journal physical boundaries

`tests/test_p3b_journal_boundary.c` covers next_record = 0, 1, journal_blocks - 1 and journal_blocks; empty journal replay/checkpoint/remount/fsck; one-slot-remaining state; DATA in the final physical slot; exact-full transaction through the real journal writer; full journal with COMMIT already published; refusal of further BEGIN/DATA until checkpoint; replay and mount recovery of committed data; checkpoint/remount cleanup and fsck; and exact physical state assertions for next_record, transaction/sequence and commit state.

BEGIN on a physically final slot is not reachable through the journal lifecycle after a prior COMMIT without checkpoint; the test treats that as an intentional lifecycle restriction rather than inventing an invalid state.

Result: PASS.

## CRC v1.3 compatibility

Repository history provides an exact historical writer/reader pair.

At commit 0352b12a15f06c0411e3b5bea91594929658026b, format version was 1.3 and the journal writer calculated CRC over block_size - 4 bytes, excluding the CRC field.

Commit a409818cdd1ca55f1d7d708b311ea403c5b3bab0 changed the writer and reader to calculate CRC over the full journal block while the CRC field is zeroed. The format version remained 1.3.

`tests/test_crc_v13_legacy.c` reconstructs a v1.3 journal record using the exact historical writer span and demonstrates that the current reader rejects it.

Therefore compatibility is explicitly NO for legacy v1.3 journal records produced by the pre-change writer.

This is a reconstructed historical on-disk fixture based on the exact historical writer, not a byte dump captured from an old release binary. The repository does not contain a shipped historical disk image. The architectural remedy is a legacy CRC reader/migration path if backward readability is required; the format version must not be silently changed to hide the incompatibility.


## Regression

The final validation run must include the complete CTest suite plus GCC/Clang sanitizer jobs and Windows. No P3-B closure should be declared until that final run is successful.
