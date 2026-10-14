# OpenFS on-disk format compatibility policy

Status: policy for format version 2. This document defines compatibility expectations; it does not claim that a golden-image fixture suite or migration implementation already exists.

## Compatibility rules

1. **A released format version is immutable.** A change to field meaning, offsets, checksums, allocation interpretation, journal semantics, or recovery rules requires a new version or a documented feature-bit negotiation rule.
2. **Readers fail closed.** Unknown major format versions and unknown mandatory feature bits must return an explicit unsupported-version/feature error. They must never be interpreted as the current layout.
3. **Reserved bytes remain zero.** Readers reject non-zero reserved bytes unless a future version explicitly assigns those bytes meaning.
4. **Encoding is deterministic.** Given the same logical input and version, encoding must produce identical bytes, including zero padding and checksums.
5. **Compatibility is directional and explicit.** A newer reader may read an older version only through a version-specific decoder and tests. An older reader must not be expected to read a newer version.
6. **No implicit conversion.** Opening an older image must not rewrite it. Conversion, when implemented, must be an explicit operation that writes a separate destination image or uses a separately reviewed crash-safe protocol.
7. **Recovery semantics are part of the format contract.** Journal record kinds, sequence rules, checkpoint selection, protected targets and replay ordering are compatibility-critical, not implementation details.

## Golden-image fixture requirements

Fixtures must be immutable test inputs with a documented origin, format version, block size, expected logical result, and SHA-256 digest. Do not regenerate expected fixtures automatically during ordinary tests.

Required fixture classes:

- One minimal valid image per supported block size: 4096, 8192, 16384, 32768 and 65536 bytes.
- A valid image containing the initial persistent inventory descriptor.
- A valid image with a committed redo transaction requiring replay.
- Valid older-version images, if backward compatibility is claimed.
- Invalid images for bad superblock/descriptor checksum, unsupported version, non-zero reserved bytes, invalid region overlap, malformed journal sequence and protected-target violation.

## Test acceptance criteria

- Decode every valid fixture and assert exact expected geometry, inventory and recovery result.
- Re-encode supported logical structures and compare bytes with the expected fixture, not merely round-trip decode/encode.
- Reject each malformed fixture with a stable error category; no invalid fixture may be silently repaired during open.
- Open/check operations must preserve source-image bytes when documented as read-only.
- Keep fixture digests and expected behavior in a manifest reviewed alongside format changes.
- Run formatting, workspace tests and Clippy on Linux and Windows for the exact source SHA.

## Version-change checklist

Before changing on-disk layout or semantics, the change author must document:

- Which bytes or recovery rules change and why.
- Whether the change is backward- and forward-compatible.
- How unknown versions and feature bits fail closed.
- Which fixtures and compatibility tests are added.
- Whether conversion is required and how it avoids modifying the source on failure.
- Which exact Linux and Windows CI runs validate the implementation.

## Current limitations

The existence of this policy is not evidence that golden binary fixtures, cross-version decoding, or conversion exist. Those remain separate implementation tasks and must not be counted as complete until the fixture suite is checked into the repository and executed.
