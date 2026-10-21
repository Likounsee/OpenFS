# OpenFS on-disk format

**Status: experimental development format.** The active Rust implementation uses canonical format version 4. This is not a production compatibility promise. Do not use OpenFS for important data or format physical devices.

OpenFS is its own filesystem; it is not NTFS and does not implicitly read historical OpenFS C images. Unknown or unsupported format versions must fail closed.

## Superblock

The primary superblock is in block 0 and the backup is in the final block. The record is encoded field-by-field in little-endian order, with a CRC32C and reserved-byte validation. The format identifier is at offset 8; the header size is 48 bytes. The persistent inventory locator hint begins at offset 48 in formats that support it. The remaining reserved bytes and the reserved tail must follow the rules for the decoded version.

Supported filesystem block sizes are powers of two from 4096 through 65536 bytes. Geometry arithmetic is checked for overflow. Primary and backup superblocks must decode consistently and match the device geometry.

## Canonical layout — version 4

For block size `B` and volume length `N` blocks:

- Block 0: primary superblock.
- Bitmap begins at block 1 and covers the volume.
- Immediately after the bitmap: checkpoint A, checkpoint B, then a 16-block redo journal ring.
- The data region begins after the bitmap and all 18 journal/checkpoint blocks.
- Block `N - 1`: backup superblock.

Let `bits_per_bitmap_block = B * 8`, `bitmap_blocks = ceil(N / bits_per_bitmap_block)`, `checkpoint_a = 1 + bitmap_blocks`, `checkpoint_b = checkpoint_a + 1`, `ring_start = checkpoint_b + 1`, `data_start = ring_start + 16`, and `backup_superblock = N - 1`. The volume must have a usable data region after all reserved blocks.

The implementation retains version-specific layout handling for earlier Rust format versions, including their four-block journal rings. Do not reinterpret an old image using version 4 geometry, and do not change an on-disk layout without an explicit version/compatibility decision.

Bitmap bits are least-significant-bit first within each byte; set means allocated. Superblocks, bitmap blocks, checkpoints, journal blocks and out-of-volume padding bits are reserved/allocated. Only blocks in the data interval may be allocated or freed by the volume allocator.

## Journal and recovery

The bounded redo journal validates record framing, sequence/checkpoint state, target ranges and protected volume-control blocks. Writable volume open runs journal recovery before returning a writable handle. Read-only checking validates journal state without replaying or modifying the image.

The journal is a single-writer mechanism, not a general multi-object transaction manager. It journals allocator bitmap updates and provides a recovery-visible commit point for descriptor ownership transitions. Persistent file and namespace APIs exist, but operations that span file data, extent maps, inventory publication and reclamation must not be assumed to be one atomic transaction unless their specific contract guarantees it. After an uncertain I/O result, reopen and recover before any further mutation.

## Records, ownership and verification

Metadata records, directory entries, extent maps, checkpoints, journal records and inventory descriptors use explicit codecs; checksums are used where specified. Validation rejects malformed lengths, invalid fields, duplicate ownership, out-of-range pointers and references to free blocks. The checker reports cross-layer consistency issues but is not a transactional repair tool.

## Remaining production gates

OpenFS still needs robust exclusive-writer enforcement, comprehensive end-to-end mutation ordering, scalable discovery/verification for large images, safe physical-device discovery and exclusion, and OS integration. Image-backed CI does not establish physical-media durability.
