# Roadmap and acceptance status

Percentages are estimates against phase acceptance criteria, not feature counts. A phase is not complete because its documentation exists.

## Progress snapshot — 2026-10-12

| Phase | Progress | Basis for current estimate |
| --- | ---: | --- |
| P0 | 70% | Workspace, CI, bounded image I/O, shared errors, superblock parser, mirrored volume formatter, bitmap initialization and end-to-end image reopen/corruption tests exist; independent format review and crash/recovery validation remain. |
| P1 | 15% | Experimental volume layout/formatter, mirrored superblocks, reserved-block bitmap initialization, open-time validation, first-fit allocation/free operations, metadata and an in-memory namespace are implemented; persistent file/directory operations remain. |
| P2 | 5% | Versioned, checksummed journal record codec and corruption/length tests exist; no durable log writer, transaction engine, replay or crash recovery yet. |
| P3 | 0% | No volume checker, repair tool or fuzz target. |
| P4 | 0% | No filesystem benchmarks or comparative measurements. |
| P5 | 0% | No advanced filesystem features implemented. |
| P6 | 0% | No Windows mount adapter. |
| P7 | 0% | No stable-release hardening or compatibility gates. |

The global estimate is **11%**, using explicit phase weights of P0 10%, P1 20%, P2 20%, P3 15%, P4 10%, P5 10%, P6 5%, and P7 10%. With P0 at 70%, P1 at 15%, P2 at 5%, and the remaining phases at 0%, the weighted estimate is 11%. These weights intentionally emphasize the filesystem engine, transaction recovery and integrity work that does not yet exist.

| Phase | Goal | Initial status | Remaining acceptance work |
| --- | --- | --- | --- |
| P0 | Foundation, safety, CI, image I/O, docs | In progress | Independent review of format fields/CRC policy; extend I/O fault injection; finish legacy audit and review format draft |
| P1 | Filesystem format and essential operations | In progress | Persist namespace metadata, implement file/directory operations and integrate crash-safe persistence |
| P2 | Transaction journal and crash recovery | In progress | Specify protocol, implement WAL/replay/checkpoint, run reproducible crash-cut and failed-I/O matrix |
| P3 | Integrity/checker/fuzzing | Not started | Read-only checker, corruption detection, bounded parser fuzzing, explicit safe repair workflows |
| P4 | Performance | Not started | Reproducible workloads and same-machine comparisons, profiling and evidence-driven changes |
| P5 | Advanced features | Not started | Prioritize features only after core format and semantics stabilize |
| P6 | Windows integration | Not started | Select adapter, map semantics and errors, test mount lifecycle and failures |
| P7 | Hardening/stable release | Not started | Security review, long fuzz/stress runs, compatibility and migration policy, release gates |

## P0 acceptance gates

- [x] Root README and project-local technical README.
- [x] Rust workspace and stable toolchain declaration.
- [x] Root-level CI definition for fmt, tests, and Clippy on Linux and Windows.
- [x] Shared error type and overflow-safe block-range check.
- [x] Image-file backend with explicit bounds and host flush contract.
- [x] Initial explicit superblock encoder/decoder and malformed-input tests.
- [x] CI passed for the workspace code on Linux and Windows (commit b99a60b668d2981664c20007b2e6e9bf0e7b3b1c; [workflow run](https://github.com/Likounsee/OpenFS/actions/runs/38175567933)).
- [ ] Independent review of on-disk field coverage and CRC/reserved-byte policy. Expanded tests now verify malformed field values even when CRC32C is recomputed.
- [x] Deterministic one-shot read/write/flush failure injection and retry tests at the device-operation boundary.
- [x] Mixed-state torn-write simulation and verification of persisted prefix/suffix contents.
- [x] End-to-end experimental volume format/reopen and corruption tests (not crash recovery).
- [ ] Complete audit of the legacy repository and its design decisions.

Phase percentages must be updated after CI evidence and code reviews; the presence of a checklist item is not itself proof of runtime success.
