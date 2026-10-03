# OpenFS v1.3 on-disk extent-tree extension

OpenFS v1.3 keeps the v1.2 inode record size and serialization offsets
unchanged. Existing v1.2 filesystems remain readable.

## Inline and tree-backed extents

Legacy inodes without `OPENFS_INODE_FLAG_EXTENT_TREE` retain the original
five inline extent records.

A tree-backed inode uses four inline extent records. The fifth 24-byte extent
slot is split by reserving its final 8 bytes (inode reserved bytes 112..119)
for a 64-bit extent-tree root block. The first four extent records remain at
the legacy offsets.

The root is a depth-0, checksummed leaf node. Its block contains:

- 8-byte magic `OEXT1\0\0\0`;
- 16-bit depth (currently zero);
- 16-bit entry count;
- 16-bit maximum entry count;
- 16-bit reserved field;
- 64-bit inode generation;
- 32-bit CRC32C over the complete block with the checksum field zeroed;
- 24-byte extent records.

With 4 KiB blocks this stores 169 overflow extents in addition to the four
inline extents. The implementation therefore supports files with many more
than the original five extent records while keeping the legacy v1.2 layout
compatible.

## Allocation and shrink ordering

When the sixth extent is required, OpenFS allocates the tree root, migrates
the fifth legacy extent plus the new overflow extents into the leaf, and then
persists the inode.

During shrink, the physical blocks being removed are resolved before the
extent-tree block is rewritten. This prevents the new tree state from hiding
the old mappings before the blocks have been identified for release.

The transaction API remains the crash-atomic path. Direct file mutation APIs
retain their documented non-transactional failure boundary.

## Corruption handling

Tree root bounds, magic, depth, entry count, capacity, extent ordering and
CRC32C are validated by the extent layer and fsck. The tree root is treated as
an allocated metadata block and is included in fsck block-reference checks.
