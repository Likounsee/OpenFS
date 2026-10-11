# Roadmap and acceptance status

Percentages are estimates against phase acceptance criteria, not feature counts. A phase is not complete because its documentation exists.

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
- [ ] CI passed on a commit containing all of the above.
- [ ] Independent review of on-disk field coverage and CRC/reserved-byte policy.
- [x] Deterministic one-shot read/write/flush failure injection and retry tests at the device-operation boundary.
- [ ] Partial/torn-write injection and end-to-end volume tests.
- [ ] Complete audit of the legacy repository and its design decisions.

Phase percentages must be updated after CI evidence and code reviews; the presence of a checklist item is not itself proof of runtime success.
