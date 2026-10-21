# OpenFS on-disk format compatibility policy

**Status: development policy.** The active canonical Rust format is version 4. Earlier Rust layouts have version-specific decoding, including versions 2 and 3; historical OpenFS C images are not implicitly compatible. This policy does not claim a production stability guarantee or that a complete golden-image fixture set exists.

## Compatibility rules

1. A released format version is immutable. Changes to field meaning, offsets, checksums, allocation interpretation, journal semantics or recovery rules require a new version or a documented feature-bit rule.
2. Readers fail closed on unknown versions and unsupported mandatory features.
3. Reserved bytes must be interpreted according to the decoded version; never repurpose them without an explicit versioned rule.
4. Encoding is deterministic, including zero padding and checksums.
5. Compatibility is directional and explicit. An older image may only be read through a version-specific decoder and layout.
6. Opening an older image must not implicitly rewrite it. Conversion must be an explicit operation with a separate destination or a separately reviewed crash-safe protocol.
7. Journal record formats, checkpoint selection, sequence rules, protected targets and replay ordering are compatibility-critical.

## Layout facts that must stay synchronized

- Canonical format version 4 reserves two checkpoint blocks and a 16-block redo ring.
- Earlier supported Rust formats use their version-specific layout, including a four-block ring.
- The bitmap, reserved regions and data start must be calculated from the decoded format version, not from the current default layout.
- A journal-size or layout change requires a compatibility review and updated format documentation before implementation is considered complete.

## Golden-image fixtures

Fixtures should be immutable, identify their source format/version and block size, and include expected decoded behavior and a digest. Useful classes include valid current images, valid older-version images where compatibility is claimed, committed redo requiring replay, and malformed superblock/descriptor/journal images. Do not regenerate expected bytes automatically during ordinary checks.

## Change checklist

Before changing disk layout or recovery semantics, document affected bytes and rules, compatibility direction, rejection behavior for unknown versions, required fixtures, conversion implications and exact-SHA Linux/Windows CI results. Keep normative documentation synchronized with the constants and decoding logic.

The existence of this policy is not evidence that every block size, version transition or physical-device failure mode has been qualified.
