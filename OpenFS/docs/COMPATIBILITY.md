> **État courant — 2026-11-04 :** OpenFS est expérimental. Le montage Windows de développement passe par Dokany ; le pilote WDM natif compile en CI mais ne monte pas encore de volume. Le montage Linux n'est pas disponible. Le chemin USB brut existe mais n'a pas de validation matérielle ; les essais physiques sont en pause. Les simulations de coupure ne prouvent pas la durabilité sur matériel réel. CI HEAD 4127059d : tests workspace/formatage Linux+Windows, crash sweep release Linux, tests C et build WDK réussis ; Clippy échoue sur les deux plateformes pour `unused_mut` à `crash_sweep.rs:551`.

---

# Compatibility

OpenFS defines its own format; it does not aim to read or write NTFS volumes. NTFS performance and semantic parity are goals to evaluate, not current claims.

The current workspace is a portable Rust library prototype with image-backed persistence and journal recovery for selected allocator/descriptor transitions. It has no mount implementation, OS adapter, permissions/ACL model, case-folding policy, alternate streams, Windows error mapping, or universal crash-recovery guarantees for every file/namespace operation.

Before Windows integration, document and test path normalization, case sensitivity, rename/delete semantics, sharing and locking, timestamps, permissions, reparse points, sparse files, and mapping of filesystem errors to Win32 behavior. A read-only adapter must never be described as full read/write support.
