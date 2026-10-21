# Security model

## Trust boundaries

Image contents, superblock bytes, caller-supplied offsets and lengths, directory/metadata records and host I/O results are untrusted. The parser, allocator and checker must reject malformed inputs without unchecked indexing or arithmetic.

## Current controls

- Checked block-range validation and checked geometry arithmetic.
- Image size and block size validation before use.
- Image creation refuses to overwrite an existing path unless the explicit destructive truncate API is used.
- Superblock decode checks record length, magic, version, header size, checksum, reserved bytes and field bounds.
- Metadata, directory, extent-map, descriptor, journal and checkpoint codecs validate their own structural constraints.
- Workspace lint forbids unsafe Rust.
- I/O errors propagate through a shared explicit error type.
- Journal recovery runs during writable volume open; uncertain persistent outcomes require recovery before further mutation.
- Inventory verification reconciles persistent references and allocation ownership.

## Current limitations

ImageDevice and RawDevice acquire an exclusive OS file lock, preventing cooperating OpenFS processes from simultaneously holding stale volume state. This is advisory on some platforms and does not sandbox hostile external modification by software that ignores the lock. Persistent file/namespace operations can span multiple persistence stages; their presence does not imply universal atomicity. Some disk-sized collections still need review for fallible allocation and large-volume scaling.

The raw-device backend does not identify physical-device type, discover trusted geometry, coordinate with mounted filesystems or prove power-loss durability. There is no production OS mount adapter or automatic repair path. Never silently repair ambiguous ownership or overwrite evidence after recovery failure.

Future changes should prioritize testing lock behavior across supported operating systems, protecting against external writers where feasible, checked arithmetic, fallible allocations derived from disk input, strict distinction between corruption and I/O errors, and conservative behavior when a persistent result is uncertain.
