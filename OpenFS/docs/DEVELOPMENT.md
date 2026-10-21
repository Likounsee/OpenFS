# Development guide

## Before changing code

1. Read [Architecture](ARCHITECTURE.md), [Format specification](FORMAT_SPEC.md), and the relevant acceptance gate in [Roadmap](ROADMAP.md).
2. Keep changes within the owning crate/module. Avoid parallel implementations and avoid mixing unrelated refactors with filesystem semantics changes.
3. Fetch the latest `OpenFS` branch and current file contents before editing; do not overwrite newer changes.

## Module map

- `openfs-core/src/lib.rs`: shared error model and checked ranges.
- `openfs-block/src/lib.rs`: bounded block-device API, image/raw backends, exclusive OS file locks, and fault injection.
- `openfs-format/src/volume.rs`: canonical layout, format/open/check, allocation, persistent inventory and file-read entry points.
- `openfs-format/src/journal.rs` and `journal_engine.rs`: journal codec, commit, replay and checkpointing.
- `openfs-format/src/allocator_extents.rs`: bitmap scans, free-space statistics and ownership reconciliation.
- `openfs-format/src/metadata.rs`, `directory_entry.rs`, `extent_map.rs`: persistent record codecs and extent mapping.
- `openfs-format/src/namespace.rs`: namespace graph and path operations.
- `openfs-format/src/inventory_descriptor.rs`, `verifier.rs`: inventory discovery and consistency reports.
- `openfs-tool/src/main.rs`: CLI only; keep OS-specific behavior out of the format crate.

## Required checks

```sh
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

CI runs these checks on Linux and Windows. A queued or running job is not a pass; report the exact status.

## Storage safety checklist

- Validate magic, format identifier, checksums, reserved bytes and geometry before using decoded values.
- Use checked arithmetic for all disk-derived lengths, offsets and block ranges.
- Never free a block while any metadata or extent map may still reference it.
- For journaled mutations, specify the commit point, replay behavior, checkpoint order, and every relevant crash cut before implementation.
- Test malformed records, duplicate/aliased ownership, free/reserved block references, I/O failures, reopen, and repeated recovery.
- Exclusive OpenFS device handles use OS file locks; remember that external software may ignore advisory locks.
- Do not claim physical-device durability or expose raw-device formatting based only on regular-file image tests.
- Keep format compatibility guards even though the source tree has one canonical implementation.

## Commit discipline

Use descriptive, scoped commits. Update the roadmap only when the acceptance evidence changes. State whether checks were added, executed, and passed; do not infer passing tests from code inspection.
