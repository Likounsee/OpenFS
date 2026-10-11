# Compatibility

OpenFS defines its own format; it does not aim to read or write NTFS volumes. NTFS performance and semantic parity are goals to evaluate, not current claims.

The current workspace is a portable Rust library prototype for file images only. It has no mount implementation, OS adapter, permissions/ACL model, case-folding policy, alternate streams, Windows error mapping, or crash-recovery guarantees.

Before Windows integration, document and test path normalization, case sensitivity, rename/delete semantics, sharing and locking, timestamps, permissions, reparse points, sparse files, and mapping of filesystem errors to Win32 behavior. A read-only adapter must never be described as full read/write support.
