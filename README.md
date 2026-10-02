# OpenFS

OpenFS (Open File System) is a portable filesystem designed to work on different
operating systems through small OS-specific adapters.

ArchiaOS is the first planned integration, but the filesystem core itself does
not depend on ArchiaOS, Linux, Windows, BSD, or any CPU architecture.

## Where we are

We are building the real filesystem core step by step. It is **not yet ready to
be used as a normal everyday filesystem**.

Already implemented:
- portable block-device API;
- versioned on-disk format;
- primary and backup superblocks;
- CRC32C checks for metadata;
- strong geometry and overflow checks;
- automatic initial inode bitmap;
- inode table;
- checksummed inodes;
- root directory inode creation;
- bitmap and inode regression tests;
- CMake build and automated CI with sanitizers.

Still to build:
- real block allocation and freeing;
- file data storage and extents;
- directories and path lookup;
- create, read, write, truncate, unlink and rename;
- hard links and symbolic links;
- permissions and timestamps;
- journal and crash recovery;
- mount/unmount;
- fsck and recovery tools;
- Linux, Windows and ArchiaOS adapters.

## Project structure

Everything belonging to the filesystem is inside `OpenFS/`:

- `OpenFS/include/` — public API;
- `OpenFS/src/` — filesystem implementation;
- `OpenFS/tests/` — automated tests;
- `OpenFS/docs/` — technical documentation.

The GitHub Actions configuration stays in `.github/` because GitHub requires
workflows to be stored there.

## Architecture

The planned structure is:

```text
Operating System
      |
      v
OpenFS adapter
      |
      v
OpenFS core
      |
      v
Block device API
      |
      v
Disk / SSD / image / virtual disk
```

The core must never call an OS-specific API directly.

## Development rule

We first define how data is stored on disk, then implement the code, then add
tests for normal cases and corrupted data. A bug found later should become a
regression test whenever possible.

See `OpenFS/docs/architecture.md` and `OpenFS/docs/format.md`.
