# Extended attributes and ACLs

OpenFS stores extended attributes in a checksummed metadata block referenced by unused inode metadata space. The reference is runtime-independent and is validated by fsck.

The current xattr API supports set/get/remove, create/replace semantics, listing names, checksummed metadata validation, and system attributes used by ACLs.

POSIX-style ACLs are represented by the system.posix_acl_access and system.posix_acl_default attributes.

ACL entries support owner, named user, owning group, named group, mask, and other classes. Access checks use the ACL when present and fall back to the inode mode bits otherwise.

A directory default ACL is inherited by newly created children. The inherited access ACL is restricted according to the requested mode. New directories also receive the unmodified default ACL for further inheritance.

The metadata is protected by the inode runtime lock and its block is included in fsck block-reference accounting.

Crash-atomic publication of xattr/ACL updates through the filesystem journal is a remaining hardening task. The current implementation therefore does not claim the same transaction-level durability guarantee as operations already implemented through openfs_transaction_t.
