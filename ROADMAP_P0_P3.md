# OpenFS — Roadmap technique P0 à P3

- **Date de l’audit :** 2026-10-10
- **Branche cible exclusive :** `OpenFS` (aucune modification de `main`, aucune nouvelle branche)
- **Commit de référence :** `43263f22703e508b3ed9940c0b1c3c310b73a2b6`
- **CI vérifiée :** le run GitHub Actions `38078250358` sur le SHA `54b88cdc0516a65dc99e97c483fcac8402824780` est terminé avec succès : GCC, Clang et Windows, Debug/Release, tests, ASan/UBSan sur Linux. Les commits correctifs plus récents (`007786e` et `290bfb0`) ont leurs propres runs en attente/en file au moment de cette mise à jour ; ils ne sont pas déclarés validés avant leur conclusion.

Cette roadmap est fondée sur l’inspection du code à la référence ci-dessus. README et journaux de progression ne sont pas utilisés comme preuve d’implémentation. Les fonctions ou champs seuls ne prouvent pas une fonctionnalité de bout en bout.

## 1. Résultats vérifiés dans le code

| Domaine et fichiers inspectés | Constat source | État réel à retenir |
|---|---|---|
| WAL/transactions : `src/transaction.c`, `src/journal.c`, `include/openfs/transaction.h`, `include/openfs/journal.h` | Ordre commit observé : COMMIT WAL, écritures des blocs de destination, flush, checkpoint. Le replay valide séquences/propriété de transaction, revalide les blocs relus et prépare les données avant callbacks. Les échecs post-COMMIT posent un état de récupération. | Implémentation substantielle, mais pas preuve de tous les scénarios de panne, d’idempotence de tous les callbacks ou de durabilité physique. |
| Tests WAL/crash : `test_journal.c`, `test_journal_failure_paths.c`, `test_journal_replay_idempotent.c`, `test_transaction.c`, `test_crash_cut.c`, `test_journal_fine_crash.c`, `test_double_failure.c`, `test_namespace_crash.c` | Corruption, échecs d’E/S, flush, retry et publication partielle ont des tests source dédiés. | Couverture présente ; non exécutée pendant cet audit. |
| Runtime/concurrence : `src/runtime.c`, `src/lock.c`, `src/file_lock.c`, `src/fd.c`; tests lifecycle, lock, file-lock et concurrency | Admission runtime, locks et handles existent. Plusieurs tests de concurrence sont déclarés dans CMake. L’historique de code inclut une inversion de verrou sur le chemin de rename qui a été corrigée. | Base existante, audit de toutes les interleavings et TSAN manquants. |
| Namespace : `src/path.c`, `src/link.c`, `src/orphan.c` | Chemins transactionnels montés de create/mkdir/rename/clone et récupération d’orphelins présents. L’historique des patches a révélé de vrais défauts d’atomicité/ownership, corrigés avec tests, puis des régressions intermédiaires de verrouillage/erreurs. | Ne pas déclarer l’atomicité universelle vérifiée avant matrice de pannes exécutée. |
| Fichiers sparse : `src/file.c`, `include/openfs/file.h`, `tests/test_file.c` | Routines sparse et APIs `openfs_file_seek_data` / `openfs_file_seek_hole` présentes. Aucun target `test_sparse.c` dédié n’est déclaré dans le CMake inspecté. | Fonctionnalités présentes ; cas limites sparse à auditer et couvrir explicitement. |
| Allocation/CoW : `src/allocator.c`, `src/inode_alloc.c`, `src/cow.c`, `src/metadata_cow.c` | Bitmap, refcounts, validation de bloc alloué et primitives de CoW présentes. L’en-tête de metadata-CoW décrit explicitement une primitive stage 1, non suffisante pour snapshots. | Primitive partielle, pas un système de snapshots. |
| FSCK/réparation : `src/fsck.c`, `src/fsck_repair.c`, `include/openfs/fsck.h` | FSCK de détection et diagnostics ; APIs de réparation exposées pour queues de bitmaps et refcounts CoW. Tests FSCK et d’échec de réparation existent. | Réparation partielle ; ne pas supposer la réparation générale des inodes, extents, liens ou journal. |
| Scrub/checksums : `src/scrub.c`, `src/data_checksum.c`, `include/openfs/scrub.h` | Scrub read-only, vérification des checksums de données si feature activé et appel à FSCK. | Pas de réparation de données corrompues par ce chemin ; couverture des couches à compléter. |
| Racines métadonnées : `src/metadata_root.c`, `include/openfs/metadata_root.h`, `src/metadata_cow.c` | Champ `snapshot_catalog_root` et type CoW catalogue présents ; aucun module/API publique de snapshots trouvé dans l’arborescence inspectée. | Snapshots absents en tant que fonctionnalité de bout en bout. |
| ACL/xattrs/handles : `src/acl.c`, `src/xattr.c`, `src/fd.c`, `src/file_lock.c` et en-têtes publics | APIs moteur existantes. | Équivalence avec ACL Windows, security descriptors, partage Windows et ADS non démontrée. |
| Adaptateur Windows : `adapters/windows/openfs_winfsp_mount.c`, `adapters/CMakeLists.txt` | API FUSE de compatibilité WinFsp ; capability read-only ; write renvoie `-EROFS`, create/mkdir/rename/truncate absents ou non implémentés ; flush/fsync/release absents. | Intégration Windows lecture-écriture non implémentée dans cet adaptateur. |
| Build/CI : `OpenFS/CMakeLists.txt`, `.github/workflows/ci.yml` | Workflow GCC/Clang Debug, ASan/UBSan, Release et Windows Debug/Release. Le CMake force les assertions actives dans les tests Release avec `-UNDEBUG` / `/UNDEBUG`. | Matrice définie, mais aucun statut CI exploitable sur le commit audité. |

### Historique des 69 commits

Les 69 entrées récentes ont été recensées. Les diffs de commits liés au code et aux tests ont été inspectés par groupes, en particulier WAL/replay, ownership des refcounts, réparations FSCK, atomicité create/clone/rename, orphelins, xattrs et erreurs des wrappers de transaction. Ils montrent des corrections substantielles, mais aussi des régressions intermédiaires détectées par CI (inversion de verrou lors du rename, mapping d’erreur post-COMMIT). Les commits documentaires ne prouvent pas qu’une fonctionnalité existe. Cette revue de l’historique n’est pas une revue ligne par ligne exhaustive de chaque patch ; le code à la référence indiquée prime.

## 2. P0 — Fiabilité, correction et récupération

### P0-001 — Audit de verrous et cycle de vie
- **Correctif ajouté (commit `41acf4c17c962fb60257579700f49e207279bc51`) :** le chemin Windows du rwlock suit désormais les acquisitions writer récursives avec une pile TLS au lieu de lire `writer`/`write_depth` avant l’acquisition du SRWLOCK. Les tests source couvrent le writer récursif et `try_write_lock` récursif sous Windows, huit threads concurrents incrémentant un compteur protégé par rwlock (80 000 acquisitions attendues), ainsi qu’une réentrée de try-write sur un rwlock déjà détenu sous un autre verrou. La matrice complète du run `38078250358` a réussi sur le code précédant ce dernier test de réentrée ; les commits `007786e` et `290bfb0` sont en cours de validation CI.
- **Priorité :** critique. **Statut :** En cours — couverture lifecycle renforcée, audit global non terminé.
- **Problème :** runtime admission, locks par domaine et handles existent, mais toutes les interleavings et voies de sortie ne sont pas prouvées ; une inversion de verrou de rename a déjà existé.
- **Fichiers :** `src/runtime.c`, `src/lock.c`, `src/path.c`, `src/dir.c`, `src/inode.c`, `src/allocator.c`, `src/cow.c`, `src/journal.c`, `src/fd.c`, `src/file_lock.c`, `tests/test_runtime_lifecycle.c`, tests concurrency/lifecycle.
- **Attendu / changements :** cartographier chaque état partagé et chaque acquisition imbriquée ; documenter ordre global ; corriger seulement les violations démontrées ; garantir qu’un runtime/handle ne peut être détruit pendant une opération référencée.
- **Tests :** ajout de cas source qui vérifient qu’un handle ouvert fait échouer `shutdown_if_unused` sans laisser l’admission fermée, vérifient le comptage/libération du handle, et rejettent une seconde tentative de shutdown pendant que la première draine les utilisateurs actifs. Le test source couvre aussi 64 cycles successifs init/enter/leave/shutdown pour exercer insertion/retrait du registre runtime ; les diagnostics de ces cycles utilisent désormais des retours à la ligne C normaux. Restent à couvrir les barrières déterministes shutdown/unmount contre read/write/rename/close, les contentions allocation/refcount et TSAN quand disponible.
- **Acceptation mesurable :** 100 répétitions des scénarios de stress ciblés sans deadlock/corruption ; zéro race TSAN sur la suite compatible ; FSCK propre après chaque run. La version initiale du test de contention a réussi dans le run `38078250358` (GCC/Clang Linux Debug et Release, ASan/UBSan, Windows Debug et Release). Le test supplémentaire de réentrée imbriquée attend la conclusion des runs déclenchés par les commits `007786e` et `290bfb0`.
- **Dépendances :** aucune ; bloque la sortie P0.

### P0-002 — WAL, transactions et doubles défaillances
- **Priorité :** critique. **Statut :** À auditer.
- **Problème :** le code implémente COMMIT → home writes → flush → checkpoint, mais la garantie globale dépend des écritures partielles, flush, callbacks idempotents et wrappers appelants.
- **Fichiers :** `src/transaction.c`, `src/journal.c`, `src/path.c`, `src/allocator.c`, `src/inode_alloc.c`, `src/cow.c`, `src/xattr.c`, `src/orphan.c` et tests transaction/journal/crash.
- **Attendu / changements :** spécifier chaque état pré/post-COMMIT ; injecter panne sur chaque write/flush/checkpoint ; conserver les erreurs d’origine ; retirer les IDs actifs sans annuler un COMMIT durable.
- **Tests :** matrice automatique de coupures, remount, replay répété et FSCK ; échec d’écriture primaire + échec de restauration.
- **Acceptation :** chaque point de coupure aboutit à l’ancien état ou au nouvel état complet, jamais à un état mixte ; retry idempotent prouvé pour chaque callback de publication.
- **Dépendances :** P0-001, P0-008.

### P0-003 — Ownership des blocs et refcounts
- **Priorité :** critique. **Statut :** À faire.
- **Problème :** bitmap, inode, extents, xattrs et refcounts doivent évoluer ensemble ; les précontrôles FSCK existants couvrent certains désaccords, pas une preuve exhaustive de propriété.
- **Fichiers :** `src/allocator.c`, `src/inode_alloc.c`, `src/cow.c`, `src/file.c`, `src/xattr.c`, `src/orphan.c`, `src/fsck.c`, `src/fsck_repair.c`.
- **Attendu / changements :** établir modèle d’ownership et invariants ; détecter blocs perdus, doubles références, blocs libres mais référencés et extents invalides ; interdire réparation ambiguë.
- **Tests :** fixtures bitmap/inode/extent/xattr/refcount contradictoires, avant et après crash/replay.
- **Acceptation :** chaque fixture est diagnostiquée ; aucune réparation ne libère un bloc d’ownership incertain ; compteurs exactement cohérents après réparation.
- **Dépendances :** P0-002, P0-004.

### P0-004 — FSCK et réparations conservatrices
- **Priorité :** critique. **Statut :** À faire.
- **Problème :** réparations publiques inspectées limitées aux queues de bitmaps et refcounts CoW ; les garanties pour liens, générations, extents, orphelins et journal ne sont pas prouvées.
- **Fichiers :** `src/fsck.c`, `src/fsck_repair.c`, `include/openfs/fsck.h`, `src/orphan.c`, tests FSCK et repair-failures.
- **Attendu / changements :** inventaire de chaque invariant ; mode check strictement read-only ; préflight avant toute réparation ; documenter les cas automatiques versus manuels.
- **Tests :** corruption par invariant, panne à chaque écriture, panne au préflight, deuxième réparation puis FSCK.
- **Acceptation :** aucune écriture en mode read-only ou si le préflight échoue ; réparation idempotente ; deuxième FSCK propre pour chaque cas déclaré réparable.
- **Dépendances :** P0-003, P0-006.

### P0-005 — Checksum et scrub
- **Priorité :** élevée. **Statut :** À auditer.
- **Problème :** checksums de données et scrub read-only existent ; attribution détaillée surtout au bloc et aucune réparation automatique de contenu corrompu n’est fournie par scrub.
- **Fichiers :** `src/data_checksum.c`, `src/scrub.c`, `src/metadata_cow.c`, `src/format.c`, `src/fsck.c`, tests scrub/checksum.
- **Attendu / changements :** définir la couverture de chaque checksum ; séparer corruption, E/S et récupération requise ; étendre l’attribution d’objet uniquement si fiable.
- **Tests :** corruption superblock, WAL, metadata-CoW, xattr et data, seules et combinées à crash ; vérification d’absence de mutation par scrub.
- **Acceptation :** bloc/type d’erreur exacts pour chaque fixture ; aucune réparation sans source fiable.
- **Dépendances :** P0-002, P0-004.

### P0-006 — Bornes, entiers et parseurs malformés
- **Priorité :** élevée. **Statut :** À faire.
- **Problème :** les modules manipulent plages, tailles, offsets et allocations ; tests de frontières présents mais corpus fuzz continu non confirmé.
- **Fichiers :** `src/format.c`, `src/inode.c`, `src/extent.c`, `src/dir.c`, `src/file.c`, `src/journal.c`, `src/acl.c`, `src/xattr.c`, tests boundary/fsck-boundary.
- **Attendu / changements :** auditer toutes les additions/multiplications/conversions ; borner tailles et profondeurs ; créer harnesses fuzz des parseurs disque.
- **Tests :** corpus valides/malformés, ASan/UBSan, seeds persistants pour chaque bug.
- **Acceptation :** aucun crash/UB sur le budget fuzz CI défini ; chaque défaut corrigé ajoute une seed ; limites documentées.
- **Dépendances :** aucune.

### P0-007 — Validation CI au SHA exact
- **Priorité :** élevée. **Statut :** À faire.
- **État de validation :** le run `38078250358` a réussi sur le SHA `54b88cdc0516a65dc99e97c483fcac8402824780` pour GCC/Clang, Debug/Release, ASan/UBSan et Windows Debug/Release. La validation doit être répétée sur le HEAD courant, qui contient un correctif additionnel de verrouillage et son test.
- **Fichiers :** `OpenFS/CMakeLists.txt`, `.github/workflows/ci.yml`, tests du moteur.
- **Attendu / changements :** exécuter la matrice sur le HEAD ; conserver assertions Release et avertissements ; traiter les échecs sans supprimer/neutraliser les tests.
- **Tests :** GCC Debug/Release, Clang Debug/Release, ASan/UBSan, Windows Debug/Release.
- **Acceptation :** tous les jobs requis verts sur le SHA exact, logs disponibles, aucune erreur masquée.
- **Dépendances :** corrections P0.

### P0-008 — Coupures de courant de bout en bout
- **Priorité :** élevée. **Statut :** À faire.
- **Problème :** crash-cut, namespace-crash et double-failure tests existent, mais il faut prouver que chaque point de persistance de chaque mutation multi-structure est couvert.
- **Fichiers :** tests crash/namespace/journal/double-failure, `src/path.c`, `src/file.c`, `src/xattr.c`, `src/orphan.c`.
- **Attendu / changements :** périphérique simulé commun et injection avant/après chaque write/flush pour create/mkdir/rename/unlink/link/clone/write/truncate/xattr/orphan reclaim.
- **Tests :** remount/replay/FSCK après chaque coupure ; vérifier contenu, namespace, handles, bitmaps et refcounts.
- **Acceptation :** zéro publication partielle ou fuite/double libération inexpliquée ; chaque scénario répétable avec seed fixe.
- **Dépendances :** P0-002, P0-003.

## 3. P1 — Moteur utilisable et intégration Windows

### P1-001 — Adapter Windows en lecture-écriture
- **Priorité :** critique après P0. **Statut :** À faire.
- **Problème :** l’adaptateur inspecté annonce read-only ; write retourne `-EROFS`, create/mkdir/rename/truncate sont absents ou incomplets et flush/fsync/release ne sont pas implémentés.
- **Fichiers :** `adapters/windows/openfs_winfsp_mount.c`, `adapters/CMakeLists.txt`, `src/fd.c`, `src/path.c`, `src/file.c`.
- **Attendu / changements :** callbacks create/open/read/write/truncate/rename/unlink/readdir/stat/flush/fsync/release, traduction correcte des erreurs et règles WinFsp/share modes.
- **Tests :** API Windows réelle sur volume jetable, handles concurrents, rename sur fichier ouvert, flush, crash/remount.
- **Acceptation :** matrice create/read/write/truncate/rename/delete/stat/readdir/flush passe ; erreurs d’accès correctes ; récupération validée.
- **Dépendances :** P0 complet, P1-002.

### P1-002 — Handles, partage, suppression différée
- **Priorité :** élevée. **Statut :** À auditer.
- **Problème :** handles et locks de plage existent ; sémantique Windows de partage, unlink et handles ouverts pas démontrée.
- **Fichiers :** `src/fd.c`, `src/file_lock.c`, `src/path.c`, `src/orphan.c`, en-têtes fd, tests fd/path/orphan/lock.
- **Attendu / changements :** définir access/share/delete semantics ; protéger références de handles et générations d’inodes lors de dup/close/rename/unlink.
- **Tests :** close concurrent, rename/unlink ouvert, réutilisation inode, dup, verrous qui se chevauchent.
- **Acceptation :** aucun handle ne pointe vers un inode réutilisé ; résultats conformes à la matrice documentée.
- **Dépendances :** P0-001, P0-003, P1-001.

### P1-003 — Permissions et ACL Windows
- **Priorité :** élevée. **Statut :** À faire.
- **Problème :** ACL et vérification d’accès moteur existent, mais mapping SID/security descriptors/inheritance Windows non confirmé.
- **Fichiers :** `src/acl.c`, `src/path.c`, `src/fd.c`, `include/openfs/acl.h`, adaptateur Windows.
- **Attendu / changements :** définir conversion UID/GID/SID et appliquer le contrôle à chaque callback, fail-closed.
- **Tests :** matrice owner/group/other, ACL héritée, accès refusé, create/rename dans répertoires restreints.
- **Acceptation :** tests Windows positifs et négatifs ; aucune mutation sans autorisation ; limitations documentées.
- **Dépendances :** P0-001, P1-001.

### P1-004 — Durabilité API et compatibilité format
- **Priorité :** élevée. **Statut :** À auditer.
- **Problème :** API write directe et transactionnelle distinctes ; garanties de flush et politique complète de migration/support des versions à formaliser.
- **Fichiers :** `include/openfs/file.h`, `include/openfs/transaction.h`, `src/format.c`, `src/mount.c`, tests format/stability.
- **Attendu / changements :** spécifier write/flush/commit, versions/features, champs réservés et refus des features inconnues.
- **Tests :** images historiques, flags inconnus, tailles de bloc, interruptions de migration.
- **Acceptation :** fixtures versionnées pour chaque format supporté ; aucune migration destructive sans procédure de sauvegarde/validation.
- **Dépendances :** P0-002, P0-006.

## 4. P2 — Snapshots et fonctions avancées

### P2-001 — Snapshots persistants de bout en bout
- **Priorité :** élevée, après P0. **Statut :** À faire.
- **Problème :** `snapshot_catalog_root` et type CoW de catalogue existent, mais aucune API/catalogue snapshot complet n’a été trouvé ; metadata-CoW est explicitement stage 1.
- **Fichiers :** `src/metadata_root.c`, `src/metadata_cow.c`, `src/cow.c`, en-têtes et nouveau catalogue snapshot.
- **Attendu / changements :** IDs persistants, vues read-only, create/delete atomiques, références exactes, suppression reprenable, FSCK.
- **Tests :** snapshot + write/truncate/rename/link/clone, panne create/delete, remount, corruption et scrub.
- **Acceptation :** lecture snapshot immuable ; aucune fuite/double libération ; toutes coupures donnent catalogue/refcounts cohérents.
- **Dépendances :** P0 complet, P1-004.

### P2-002 — Flux nommés / ADS
- **Priorité :** moyenne. **Statut :** À auditer.
- **Problème :** xattrs ne démontrent pas l’équivalence aux Alternate Data Streams NTFS.
- **Fichiers :** `src/xattr.c`, `include/openfs/xattr.h`, adaptateur Windows, format.
- **Attendu / changements :** décider stockage et limites ; définir API Windows, nommage, accès, persistance, suppression et FSCK.
- **Tests :** streams multiples/vides, rename/delete, ACL, crash/corruption.
- **Acceptation :** tests Windows ADS passent ; références invalides détectées par FSCK.
- **Dépendances :** P0-002, P0-004, P1-001.

### P2-003 — Quotas, compression, chiffrement, reparse points
- **Priorité :** moyenne ; évaluer séparément. **Statut :** À auditer.
- **Problème :** aucune implémentation de bout en bout n’a été confirmée pour ces familles ; chacune a des implications de format, sécurité et récupération.
- **Fichiers :** nouveaux modules, `src/format.c`, `src/file.c`, `src/fsck.c`, adaptateur Windows.
- **Attendu / changements :** étude coût/risque par fonctionnalité ; spécifier quota concurrent, codec, clés/effacement et filtrage de cibles reparse avant codage.
- **Tests :** dépassement concurrent, corruption codec, clé absente, boucle reparse, crash/recovery.
- **Acceptation :** ne pas annoncer une fonctionnalité avant API, persistance, récupération, FSCK/scrub et tests Windows.
- **Dépendances :** P0 complet, P1-001/003/004.

### P2-004 — Journal de changements type USN
- **Priorité :** moyenne. **Statut :** À faire.
- **Problème :** aucune API/persistance type USN confirmée.
- **Fichiers :** nouveaux modules, `src/path.c`, `src/transaction.c`, adaptateur Windows, format.
- **Attendu / changements :** identifiants monotones, raisons, rétention, wrap/reset et publication atomique avec commit.
- **Tests :** événement perdu/dupliqué, overflow, crash entre commit et événement, reset/remount.
- **Acceptation :** ordre et garantie événement/changement documentés et vérifiés après reprise.
- **Dépendances :** P0-002, P1-001/004.

## 5. P3 — Performances et maturité

### P3-001 — Benchmarks reproductibles
- **Priorité :** P3. **Statut :** À faire.
- **Problème :** aucune comparaison reproductible OpenFS/NTFS n’a été confirmée.
- **Fichiers :** nouveau dossier benchmark et scripts.
- **Attendu / changements :** mesurer séquentiel/aléatoire, petits fichiers, gros répertoires, charge concurrente et coût mount/FSCK.
- **Tests :** mêmes workloads, matériel/OS/cache documentés, runs répétés.
- **Acceptation :** données brutes archivées, médiane et dispersion publiées ; pas d’optimisation sans avant/après.
- **Dépendances :** P0 complet, P1-001.

### P3-002 — Optimisations guidées par profilage
- **Priorité :** P3. **Statut :** À faire.
- **Problème :** pas de baseline fiable pour choisir cache, read-ahead, allocations ou structures de gros répertoires.
- **Fichiers :** `src/file.c`, `src/extent.c`, `src/dir.c`, `src/allocator.c`, runtime.
- **Attendu / changements :** optimiser une cause mesurée à la fois, sans changer garanties de persistance.
- **Tests :** benchmark avant/après, stress et suite P0.
- **Acceptation :** gain reproductible sans régression de correction ni hausse inexpliquée de latence de queue.
- **Dépendances :** P3-001, P0 complet.

### P3-003 — Fuzzing continu, upgrade et distribution
- **Priorité :** P3. **Statut :** À faire.
- **Problème :** corpus fuzz continu et procédure complète d’upgrade/release non confirmés.
- **Fichiers :** parsers format/journal/inode/extent/ACL/xattr, CI, documentation release.
- **Attendu / changements :** fuzz périodique, seeds minimisées, politique versions, backup/upgrade/rollback et artefacts reproductibles.
- **Tests :** corpus connu, migration de chaque version supportée, vérification artefacts.
- **Acceptation :** zéro crash connu du corpus ; toutes versions déclarées testées ; checklist release reproductible.
- **Dépendances :** P0-006, P1-004, P3-001.

### P3-004 — Décision d’intégration Linux native
- **Priorité :** P3 facultative. **Statut :** À auditer.
- **Problème :** un adaptateur Linux userspace existe, mais cela ne prouve pas une intégration noyau.
- **Fichiers :** `adapters/linux`, `adapters/CMakeLists.txt`, documentation d’architecture.
- **Attendu / changements :** décision explicite sur FUSE/userspace versus noyau, selon les objectifs du projet.
- **Tests :** tests d’intégration de l’adaptateur et opérations documentées.
- **Acceptation :** décision d’architecture et surface de support publiées.
- **Dépendances :** P0 complet.

## 6. Matrice fonctionnelle face à NTFS

« Présente mais insuffisamment testée » signifie que du code/tests source existent, mais que l’exécution complète sur le commit audité n’est pas confirmée.

| Domaine | Classement OpenFS | Base factuelle / écart |
|---|---|---|
| Fiabilité/récupération | Présente mais insuffisamment testée | WAL, replay, recovery gate et tests de pannes présents ; CI du SHA inconnue. |
| Journalisation | Présente mais insuffisamment testée | BEGIN/DATA/COMMIT et checkpoint présents ; toutes doubles défaillances et durabilité physique non prouvées. |
| Fichiers/répertoires | Présente mais insuffisamment testée | API moteur et tests présents ; sémantique Windows complète non validée. |
| Allocation/fragmentation | Présente mais insuffisamment testée | bitmap, extents, CoW/refcounts présents ; aucun benchmark comparatif vérifié. |
| ACL moteur | Présente mais insuffisamment testée | ACL/access checks existent ; mapping Windows/SID non démontré. |
| ACL Windows natives | Partielle | Conversion security descriptor non confirmée. |
| Flux alternatifs ADS | Absente / non vérifiée | xattrs ne prouvent pas l’API ADS. |
| Snapshots | Partielle | champ root et primitive CoW de catalogue, sans API/catalogue complet trouvé. |
| Quotas | Absente / non vérifiée | aucune implémentation de bout en bout confirmée. |
| Compression | Absente / non vérifiée | aucune implémentation de bout en bout confirmée. |
| Chiffrement | Absente / non vérifiée | aucune gestion de clés/intégration complète confirmée. |
| Journal des changements USN | Absente / non vérifiée | aucun journal durable correspondant confirmé. |
| Points de réanalyse | Absente / non vérifiée | aucune sémantique Windows confirmée. |
| Intégration Windows | Partielle | adaptateur WinFsp/FUSE explicitement read-only, callbacks mutation/flush incomplets. |
| FSCK | Présente mais insuffisamment testée | détection/diagnostics présents ; couverture de toutes corruptions non prouvée. |
| Réparation | Partielle | API exposées limitées aux tails de bitmap et refcounts CoW. |
| Scrub/intégrité | Partielle | scrub read-only et checksum data ; pas de réparation de contenu corrompu. |
| Performances mesurées | Non vérifiée | benchmarks reproductibles face à NTFS non confirmés. |
| Compatibilité format | Partielle | format propriétaire/versionné et tests présents ; politique de migration à formaliser. |

Le format disque OpenFS est propriétaire et distinct du format NTFS : cette comparaison fonctionnelle ne signifie pas que les volumes sont interchangeables. Aucun pourcentage de parité n’est avancé.

## 7. Ordre de livraison

1. P0-001 et P0-006 : invariants de concurrence et validation des entrées.
2. P0-002, P0-003, P0-008 : atomicité et récupération.
3. P0-004 et P0-005 : FSCK/scrub conservateurs et diagnostiques.
4. P0-007 : exécution de toute la matrice sur le SHA final.
5. P1-002 et P1-004 : figer sémantiques moteur et format.
6. P1-001 et P1-003 : intégration Windows réelle.
7. P2-001 seulement après fondations stables ; évaluer les autres features séparément.
8. P3 après sorties P0/P1 démontrées.

## 8. Validation et limites de cet audit

- **Code récupéré et inspecté :** transactions, journal, runtime, fichiers, namespace, allocation, allocation inode, FSCK/réparation, scrub, CoW/metadata-CoW/root, orphelins, liens, ACL, xattrs, fd, file locks, adaptateur Windows.
- **Build/CMake/CI inspectés :** `OpenFS/CMakeLists.txt`, `.github/workflows/ci.yml`, `adapters/CMakeLists.txt`.
- **Tests source inspectés :** WAL/transactions, crash-cut, namespace-crash, double-failure, FSCK/repair, scrub/checksum, handles/orphelins/liens et tests de concurrence déclarés par CMake.
- **Build/test local lancé par cet assistant :** aucun ; la validation de compilation et des tests est réalisée via GitHub Actions.
- **CI confirmée :** run `38078250358` réussi sur `54b88cdc0516a65dc99e97c483fcac8402824780` (GCC, Clang, Linux Debug/Release, ASan/UBSan, Windows Debug/Release). Les runs du HEAD plus récent restent à vérifier.
- **Historique :** 69 commits recensés ; patches de code pertinents inspectés par groupes, mais pas revue ligne par ligne exhaustive de tous les diffs.
- **Branche de ce document :** `OpenFS` uniquement ; aucune modification de `main` et aucune nouvelle branche.
