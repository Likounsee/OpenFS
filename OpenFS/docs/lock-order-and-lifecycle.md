# OpenFS lock-order and lifecycle audit

This document records the lock-order contract enforced by `src/lock.c` and the
runtime lifetime rules enforced by `src/runtime.c`. It is a review aid, not a
claim that every filesystem call path has been exhaustively model-checked.

## Ranked mutex order

Nested ranked locks must be acquired in nondecreasing rank order. Acquiring a
lower-ranked lock while a higher-ranked lock is on the thread-local stack is
rejected with `OPENFS_LOCK_DEADLOCK`. Unlocking must follow the per-thread
last-acquired-first-released order.

| Rank | Value | Intended domain |
|---|---:|---|
| MOUNT | 5 | Mount state |
| DIRECTORY | 10 | Namespace/directory state |
| HANDLE | 15 | Open file description state |
| INODE | 20 | Inode state |
| REGISTRY | 25 | Runtime registries (handles and file locks) |
| ALLOCATION | 30 | Block/inode allocation |
| TRANSACTION | 35 | Transaction serialization |
| JOURNAL | 40 | WAL/journal state |
| CHECKSUM | 45 | Checksum state |
| LIFECYCLE | 50 | Runtime admission and lifecycle |

Equal ranks are allowed by the rank checker. Consequently, this ordering is a
deadlock-prevention guardrail rather than a complete proof for two distinct
locks in the same domain; callers must still avoid opposite-order acquisition.

## Runtime lifetime contract

- Operations that access runtime-owned state must successfully enter the
  runtime and leave it on every exit path.
- Shutdown marks the runtime as destroying and closes admission before it
  drains active users. It must not destroy lifecycle or registry locks while
  an active operation can still use them.
- The global runtime registry guard protects the lookup/removal boundary.
  Callers probing a possibly stale runtime must use `openfs_runtime_enter()`,
  which checks registry membership before touching the runtime lifecycle lock.
- `openfs_runtime_is_accepting()` is only safe while the caller already owns
  an active runtime pin; it must not be used as a stale-pointer probe.
- A failed unused-shutdown with open handles must reopen admission rather than
  strand a mounted runtime in a permanently closed state.
- Windows SRWLOCK writer recursion is tracked per thread. Shared acquisition
  and writer metadata must not inspect another thread's writer state before
  acquiring the native lock.

## Validation currently exercised

The CI matrix on the branch runs GCC and Clang Debug/Release, ASan/UBSan on
Linux, Windows Debug/Release, and a focused ThreadSanitizer suite. Lifecycle
tests cover repeated runtime registration/removal and concurrent shutdown.
The deterministic concurrency suite covers a gated in-flight read versus
unmount (100 cycles), write versus unmount (32 cycles), rename versus unmount
(32 cycles), close versus unmount (32 cycles), block allocation versus unmount
(32 cycles), and CoW refcount reads versus unmount (32 cycles). The gated
scenarios check that admission closes and teardown waits for the active
operation, then remount and run FSCK with zero reported errors. Runtime
lifecycle tests also cover concurrent shutdown/admission and repeated
registration/removal.

The complete CI run for source SHA
`0d3a5d0872ea8555109250f6fcdc8af6b22098b1` passed GCC, Clang, Windows, and
ThreadSanitizer: [run 38093073548](https://github.com/Likounsee/OpenFS/actions/runs/38093073548).
This closes the measurable P0-001 acceptance scope for the covered paths.
It is not a formal proof or exhaustive model checking of every possible
interleaving or every public API; newly discovered concurrent paths must be
added to the deterministic suite and reviewed separately.
