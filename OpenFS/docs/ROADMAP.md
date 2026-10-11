# Roadmap and acceptance status

Percentages are estimates against phase acceptance criteria, not feature counts. A phase is not complete because its documentation exists.

## Progress snapshot — 2026-10-11

| Phase | Progress | Basis for current estimate |
| --- | ---: | --- |
| P0 | 60% | Workspace, CI, bounded image I/O, shared errors, initial superblock parser, tests and first legacy audit exist; torn-write simulation, independent format review and broader end-to-end validation remain. |
| P1 | 0% | No formatter, allocator, metadata namespace or file/directory operations. |
| P2 | 0% | No Rust journal or crash-recovery implementation. |
| P3 | 0% | No volume checker, repair tool or fuzz target. |
| P4 | 0% | No filesystem benchmarks or comparative measurements. |
| P5 | 0% | No advanced filesystem features implemented. |
| P6 | 0% | No Windows mount adapter. |
| P7 | 0% | No stable-release hardening or compatibility gates. |

The global estimate is **6%**, using explicit phase weights of P0 10%, P1 20%, P2 20%, P3 15%, P4 10%, P5 10%, P6 5%, and P7 10%. The weighted estimate is 5.5%, rounded to the nearest whole percent. These weights intentionally emphasize the filesystem engine, transaction recovery and integrity work that does not yet exist.

| Phase | Goal | Initial status | Remaining acceptance work |
| --- | --- | --- | --- |
| P0 | Foundation, safety, CI, image I/O, docs | In progress | Pass CI on current commit; verify platform builds; extend I/O fault injection and malformed-input coverage; finish legacy audit and review format draft |
| P1 | Filesystem format and essential operations | Not started | Complete volume layout, formatter, allocation, metadata, files/directories, persistence integration tests |
| P2 | Transaction journal and crash recovery | Not started | Specify protocol, implement WAL/replay/checkpoint, run reproducible crash-cut and failed-I/O matrix |
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
- [x] CI passed for the workspace code on Linux and Windows (commit 0a00995f1f57b6e1bfc44986e8dfcd0dd3520cfd; [workflow run](https://github.com/Likounsee/OpenFS/actions/runs/38172350309)).
- [ ] Independent review of on-disk field coverage and CRC/reserved-byte policy.
- [x] Deterministic one-shot read/write/flush failure injection and retry tests at the device-operation boundary.
- [x] Mixed-state torn-write simulation and verification of persisted prefix/suffix contents.
- [ ] End-to-end volume tests and recovery validation.
- [ ] Complete audit of the legacy repository and its design decisions.

Phase percentages must be updated after CI evidence and code reviews; the presence of a checklist item is not itself proof of runtime success.
