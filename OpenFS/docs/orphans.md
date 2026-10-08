# Orphan inode lifecycle

OpenFS keeps an inode alive after its final directory link is removed when an open file handle still references it.

The inode is marked with OPENFS_INODE_FLAG_ORPHAN and its link count becomes zero. Its data and inode allocation remain valid while the open-file description exists.

When the last reference to that open-file description is closed, the runtime handle registry releases the inode and OpenFS reclaims its data blocks and inode allocation.

The orphan flag is persistent. If the machine crashes before the final close, the next mount scans allocated inodes and reclaims persistent orphans before the filesystem is exposed to callers. This prevents open-unlinked files from leaking their blocks after a crash.

Unmount refuses to destroy a mounted runtime while file handles are still alive. This prevents runtime lock/handle state from becoming dangling.

This is intentionally a small first orphan implementation. A future transactional orphan protocol will move the directory-entry removal, orphan publication, and final reclamation onto the filesystem journal so crash recovery does not depend on a multi-step direct-write window.
