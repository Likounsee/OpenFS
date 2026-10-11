# Audit of historical OpenFS-old

**Scope:** read-only inspection of Likounsee/OpenFS-old branch OpenFS, HEAD fdb5dee16266d037c79438a5b20ebccceab3fa24. This is a source-level audit, not a fresh execution of its tests. Files cited below were inspected in the repository; conclusions are separated from recommendations.

## Observed architecture

The historical implementation is C11 and has a substantial source tree under OpenFS/src, public headers under OpenFS/include/openfs, tests under OpenFS/tests, command-line tools under OpenFS/tools, adapters for Linux, Windows/WinFsp and ArchiaOS, and a CMake build. Its architecture documentation describes format, block I/O, metadata, transactions, filesystem operations, tools and OS adapters.

Observed components include:
- format.c: explicit little-endian field helpers, layout calculations, bounds checks and CRC32C superblock validation.
- journal.c and transaction.c: journal record parsing, transaction staging, replay and checkpoint operations.
- mount.c: mount lifecycle and replay integration.
- fsck.c and fsck_repair.c: integrity checks and repair-related paths.
- Tests for boundaries, format, allocation, files, directories, links, journal failures, replay idempotence, transaction ordering, concurrency and crash cuts.
- Windows integration code using a FUSE-compatible/WinFsp-related interface. This is evidence of an adapter attempt, not proof of complete Windows semantic compatibility.

## Findings and migration decisions

| Area | Observed evidence | Decision for Rust | Reason / follow-up |
| --- | --- | --- | --- |
| Explicit format encoding | format.c uses little-endian byte helpers, fixed offsets and CRC32C. | **Keep the concept; reimplement** | Make every field and reserved byte policy normative; never serialize native structs. Add golden byte fixtures and version compatibility tests. |
| Checked arithmetic | Format, transaction and fsck code include overflow/range checks. | **Keep and centralize** | Rust types help, but untrusted values still require checked arithmetic and allocation caps. |
| Block-device abstraction | C block-device interface carries block size/count and read/write/flush callbacks. | **Keep, simplify** | Rust trait and error propagation provide clearer contracts. Specify concurrency and flush semantics. |
| Write-ahead journal | Journal and transaction modules implement commit/replay/checkpoint behavior. | **Re-design before porting** | Write a state-machine specification first; validate durability order and every failure boundary with crash tests. |
| Replay/checkpoint failure matrix | Tests include partial writes, flush failures, checkpoint rollback and process-death cut points; latest history extended transaction recovery coverage. | **Keep test strategy** | Port scenarios into deterministic Rust fault-injection tests, with persisted bytes verified after reopen. |
| Allocation and metadata | Separate bitmap, inode, extent, directory and metadata modules exist. | **Re-design incrementally** | Define format invariants and ownership first; don't port large C interfaces wholesale. |
| FSCK and repair | Checker and repair modules/tests exist, including boundary and repair-failure cases. | **Keep read-only checker first** | Repair must be opt-in, explain every proposed change, and never overwrite corruption implicitly. |
| Concurrency and locking | Lock, file-lock, namespace-concurrency and file-concurrency modules/tests exist. | **Measure and redesign** | Define lock order, lifecycle, and sharing semantics before introducing concurrency into the Rust engine. |
| OS adapters | Linux, Windows/WinFsp and ArchiaOS adapter sources and tests are present. | **Defer** | Stabilize core API and file semantics first; adapters must map errors and lifecycle correctly. |
| Broad feature set | Headers/source/tests indicate ACL, xattrs, links, extents, checksums, orphan handling and snapshots/COW concepts. | **Stage features later** | Each feature adds format and recovery complexity; prioritize correctness and compatibility first. |

## Risks and caveats

- The legacy code is a large C implementation with many cross-module invariants; copying it directly would also copy its API and maintenance burden.
- A source-level inspection does not prove all documented behavior, test coverage, or physical durability guarantees.
- Existing crash tests are a strong starting point, but only passing execution on supported environments provides current evidence. This audit did not rerun the old C suite.
- WinFsp-related source does not establish complete NTFS compatibility or full Windows filesystem semantics.
- The legacy format document describes version 1.3 and features that should not be assumed to match the new Rust format draft.

## Recommended sequence

1. Stabilize checked block I/O and explicit error taxonomy.
2. Finalize the byte-level format specification and add golden fixtures before a formatter writes real volumes.
3. Build bounded parsing and a read-only checker before mutation-heavy features.
4. Specify transaction durability states; then implement journal/recovery with fault injection and crash-cut tests.
5. Add allocation and namespace operations only with end-to-end reopen tests.
6. Add OS adapters and performance optimization only after core semantics are stable and measurable.
