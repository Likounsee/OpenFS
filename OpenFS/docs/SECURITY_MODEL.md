# Security model

## Trust boundaries

Image contents, superblock bytes, caller-supplied offsets and lengths, and host I/O results are untrusted. The image backend and format parser must reject malformed inputs without unchecked indexing or arithmetic.

## Current controls

- Checked range validation avoids computing an unchecked end block.
- Image buffer lengths must be non-zero multiples of block size.
- Image sizes and block sizes are validated before use.
- Image creation refuses to overwrite an existing path unless the caller explicitly chooses the destructive truncate API.
- Superblock decode checks record length, magic, version, header size, CRC32C, reserved bytes, and field bounds.
- Workspace lint forbids unsafe Rust.
- I/O errors propagate through a shared explicit error type.

## Current limitations

The image backend does not provide a sandbox against hostile concurrent modification of the image file by another process. It uses the host filesystem's metadata and synchronization semantics. The superblock draft is not a complete volume format, and there is no filesystem namespace or repair code yet.

Future parser, allocator, directory, journal, and checker code must check arithmetic, cap allocations derived from disk values, avoid panics on malformed input, distinguish corruption from I/O failure, and never repair or overwrite data implicitly.
