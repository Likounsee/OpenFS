### Reprise de session — 2026-10-23 (flush failures sur renommage persistant)

- Branche autorisée : `OpenFS` uniquement.
- CI précédente confirmée verte sur Linux et Windows pour `e4ca2a1d93ba69f22e4a83ff0a39bf2f39671360`.
- Ajout d'une matrice de régression qui injecte une erreur de flush à chaque frontière testée pendant le renommage persistant. Après chaque essai, elle rouvre l'image et vérifie l'exclusivité ancien-ou-nouveau chemin, l'intégrité du contenu et la cohérence de l'inventaire de propriété des blocs, pour cinq tailles de blocs.
- Ce test ne prouve pas la durabilité physique après coupure d'alimentation; il vérifie les garanties de récupération du backend image sous erreurs de flush simulées.
- Résultat du nouveau test et de la CI exacte-SHA en attente; ne pas augmenter P1/P2 avant validation Linux/Windows.

---

### Reprise de session — 2026-10-23 (récupération d'allocation et flush de nettoyage)

- Branche de travail : `OpenFS` uniquement; ne pas modifier `main` ni `OpenFS-old`.
- Réexamen ciblé de l'arbre Rust (16 fichiers), des manifests, du workflow CI, du contrat journal P2, du contrat de publication P1, du design de récupération, des spécifications de format/compatibilité, de l'architecture, du modèle de sécurité et des guides de développement/périphérique. Les gros fichiers ont été parcourus par sections ciblées; ce n'est pas une lecture manuelle exhaustive de chaque ligne.
- La CI du commit `4f674704742a69cbf22b34255cfdcb8a93aad266` a passé Linux, y compris les tests workspace et Clippy; Windows était encore en cours au dernier contrôle. Le run antérieur `39974902756` a échoué sur l'inventaire de symboles obsolète, corrigé ensuite dans ce commit.
- Ajout d'un test de régression sur cinq tailles de blocs : une erreur de flush après l'écriture de nettoyage du bloc candidat doit laisser le bitmap libre; après réouverture, le même bloc doit pouvoir être alloué et ses octets ne doivent pas exposer l'ancien contenu. Le test utilise le `FaultInjectingDevice` déjà présent, sans créer un injecteur parallèle.
- Ce nouveau test et sa ligne d'inventaire doivent être validés par la CI Linux/Windows du commit qui les contient. Ne pas augmenter les estimations P1/P2 avant validation complète.

---

# OpenFS roadmap — source-audited plan to a usable filesystem

### Reprise de session — 2026-10-22 (rename/remove persistant et CI exacte-SHA)

- Branche de travail : `OpenFS` uniquement; `main` et `OpenFS-old` restent intouchées.
- Relecture structurée de l'arbre Git (62 entrées), des manifests, du workflow CI, des contrats P1/P2, du modèle de récupération, du module BlockDevice et des sections ciblées de `volume.rs`, `namespace.rs` et du CLI. Les gros fichiers ont été examinés par sections ciblées, pas ligne par ligne; ce n'est pas un audit manuel exhaustif de chaque ligne.
- Le scénario CLI persistant couvre maintenant aussi `mkdir /docs/archive`, la création/écriture de `/docs/discard.txt`, le déplacement de `/docs/note.txt` vers `/docs/archive/note.txt`, puis la suppression de `/docs/discard.txt`. Après fermeture/réouverture, il vérifie le contenu à la nouvelle adresse et l'absence des deux anciens chemins, pour les cinq tailles de blocs (4, 8, 16, 32 et 64 KiB).
- CI du commit `9c1a54a9b6d8103ed590ec2d054ad5dcb8439689` : [run 39854507344](https://github.com/Likounsee/OpenFS/actions/runs/39854507344) **vert sous Linux et Windows**; inventaire des symboles reproductible, rustfmt, tests du workspace avec toutes les features et Clippy sur toutes les cibles/features sont terminés avec succès sur les deux systèmes.
- Limite encore ouverte : ce scénario valide le chemin nominal après réouverture, pas chaque frontière d'échec E/S du renommage/suppression. La prochaine tranche doit cibler l'injection de pannes, la récupération et l'intégrité des blocs avant d'augmenter les estimations.
- Estimations maintenues sans hausse : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global pondéré 60 %.


### Reprise de session — 2026-10-22 (vertical slice CLI persistant)

- Branche cible : `OpenFS` uniquement; aucune modification de `main` ni de `OpenFS-old`.
- Le workflow `39851938335` sur `f2edfbd` est vert sur Linux et Windows. Le lot suivant a révélé une hypothèse de test trop forte : après plusieurs mutations de fichier, endommager arbitrairement un checkpoint peut enlever le seul checkpoint assez récent pour correspondre au journal circulaire. Ce n'est pas équivalent au scénario déjà couvert où le checkpoint pair reste suffisant. La corruption de checkpoint reste donc couverte par son test dédié sur un volume propre.
- Relecture structurée renouvelée de l'arborescence, du workflow CI, des contrats journal/récupération, du module CLI, des codecs et des zones ciblées de `volume.rs`. Les très gros fichiers ont été examinés par sections, pas ligne par ligne; ne pas présenter cela comme un audit exhaustif manuel.
- Le test CLI vertical-slice couvre sur les cinq tailles de blocs : `init-namespace → mkdir → touch → write → append → truncate → close/reopen → check-image → audit-image`. Le contenu final est relu depuis le volume rouvert; le contrôle et l'audit sont vérifiés sans changement des octets de l'image. Le scénario de checkpoint endommagé demeure isolé dans son test spécialisé.
- Le test initial du lot a échoué précisément à cette hypothèse de checkpoint; l'assertion incorrecte est retirée, pas masquée. Le nouveau commit doit encore passer CI Linux et Windows avant validation.
- Les estimations restent P0 100 %, P1 79 %, P2 69 %, P3 5 %, global pondéré 60 %.


### Reprise de session — 2026-10-22 (couverture du streaming complet)

- Branche de travail : `OpenFS` uniquement. La branche a été résolue par l'arbre Git; ne pas modifier `main` ni `OpenFS-old`.
- Inventaire de l'arbre Git relu : 62 entrées au total, dont les manifests des quatre crates, les sources Rust, les documents d'architecture/format/sécurité/récupération, les inventaires CSV, le générateur d'inventaire et le workflow CI. Les chemins prioritaires ont été relus dans cette passe; les très gros fichiers (`volume.rs`, inventaires CSV et rapport exhaustif) ont été inspectés par sections ou métadonnées plutôt que ligne par ligne. Ce n'est donc pas une affirmation d'audit manuel exhaustif de chaque ligne.
- Relecture ciblée renouvelée : `OpenFS/Cargo.toml`, manifests des crates, `FORMAT_SPEC.md`, `P2_JOURNAL_TRANSACTION_CONTRACT.md`, `scripts/generate_symbol_inventory.py`, les deux inventaires CSV et `.github/workflows/ci.yml`. Les invariants restent : préflight de capacité avant mutation, COMMIT durable avant application des blocs de données, checkpoint après application, et récupération après résultat d'E/S incertain.
- Nouveau test dans `crates/openfs-format/src/volume.rs` : `stream_persistent_file_to_writer` est désormais testé sur un fichier multi-blocs complet avec un writer qui n'accepte que 7 octets par appel. Le test vérifie que `write_all` gère les écritures partielles, que le nombre retourné correspond à la taille persistante et que tous les octets reçus correspondent au contenu attendu. Le chemin d'erreur du writer est également vérifié pour les deux API de streaming (plage et fichier complet), et les cas EOF/flush restent couverts.
- Commits code de cette passe : `fd1d23345ef6bae1f05c79e338ce82860e739a9f` (`test(format): exercise whole-file streaming with short writes`) et `1b95446e0b9098caf04c4ce63edc475244c845b6` (`test(format): assert full-file stream write errors propagate`). Aucun test Rust n'a été exécuté dans cet environnement : ces commits ajoutent des régressions à valider, ils ne prouvent pas encore le succès des tests.
- Le workflow CI du dépôt est présent à `.github/workflows/ci.yml` (racine). Il vérifie la reproductibilité du CSV, `cargo fmt --all -- --check`, `cargo test --workspace --all-features` et `cargo clippy --workspace --all-targets --all-features -- -D warnings` sous Linux et Windows. Le statut doit être contrôlé sur le HEAD exact; des listes vides de checks/runs ne sont pas une réussite CI.
- Estimations conservées faute de validation du nouveau test : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global pondéré 60 %. Ne créditer aucun progrès de phase avant validation par tests et CI Linux/Windows.

---

### Reprise de session — 2026-10-22 (tests de bornes de lecture)

- Branche de travail : `OpenFS` uniquement. Ne pas modifier `main` ni `OpenFS-old`.
- Relecture ciblée renouvelée : `ROADMAP.md`, `FORMAT_SPEC.md`, `P2_JOURNAL_TRANSACTION_CONTRACT.md`, workflow `.github/workflows/ci.yml`, générateur `scripts/generate_symbol_inventory.py`, CSV `INVENTAIRE_SYMBOLES.csv`, inventaire fonctionnel `INVENTAIRE_FONCTIONNEL_REANALYSE.csv`, et chemins de lecture persistante dans `crates/openfs-format/src/volume.rs`. Le gros module `volume.rs` (~682 Ko) a été inspecté par sections ciblées, pas ligne par ligne; ceci n'est pas un audit exhaustif de tous les fichiers du dépôt.
- Les tests existants couvrent déjà les plages de lecture persistante, la réouverture, les remplacements courts/vides et certains scénarios d'échec. Ajout dans `volume.rs` : la lecture par plage classique avec longueur `usize::MAX` est testée aux frontières EOF; cette passe ajoute les mêmes cas pour `stream_persistent_path_range_to_writer`, en vérifiant que la longueur retournée et les octets produits sont limités au suffixe réel, et qu'un offset `u64::MAX` produit zéro octet.
- Commits code de cette passe : `eca7ede23cf5d9cf28751b57c32010983e204970` (`test(format): cover streaming range boundaries at EOF`) et `e0ea29a3cc731b27366eab0c9d545f7cc52959c7` (`test(format): cover streaming writer failures`). Le test de streaming couvre désormais la borne EOF et les erreurs du writer : un échec de `write` doit remonter comme erreur, et l'échec de `flush` reste visible même pour une plage vide/hors EOF. Les tests n'ont pas été exécutés dans l'environnement disponible; ce sont des assertions ajoutées, pas une preuve d'exécution réussie.
- Les vérifications de statut combiné et de runs Actions doivent être contrôlées sur le nouveau HEAD exact. **CI non confirmée tant qu'un résultat explicite n'est pas visible**; la génération du CSV et les commandes Rust restent à exécuter dans un runner.
- Les invariants P1/P2 prioritaires restent : ne pas publier de références avant la durabilité requise, ne pas libérer de blocs encore référencés, et rouvrir/récupérer après un résultat d'E/S incertain. Le contrat du journal ne garantit pas à lui seul l'atomicité de toutes les opérations de fichiers.
- Estimations conservées faute de validation de ce nouveau test : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global pondéré 60 %. Ne créditer aucun progrès de phase avant validation par tests et CI Linux/Windows.

Last source audit: 2026-10-22  
Authoritative repository/branch: Likounsee/OpenFS, branch OpenFS only  
Code baseline audited: f7d7bdaf5d06095fd8cd448cf255ecc6eca6b0fc  
Current code HEAD before this roadmap revision: f7d7bdaf5d06095fd8cd448cf255ecc6eca6b0fc  
Legacy comparison: Likounsee/OpenFS-old at fdb5dee16266d037c79438a5b20ebccceab3fa24  
Scope: re-audit of the filesystem source tree and inventory; follow-up integration tests for the explicit journal-recovery gate. Earlier descriptor-range and superblock-fixture work is historical; current CI is tracked per exact commit below.

### Mise à jour du 2026-10-22

- `905acdf` — `audit-image` effectue désormais `check_volume` avant son ouverture récupératrice et refuse les journaux contenant des enregistrements en attente ou des transactions récupérables. Le chemin de récupération implicite est bloqué dans ce cas; l'utilisateur doit sauvegarder l'image puis lancer explicitement `recover-image`.
- Test de régression ajouté pour les trois états du garde (enregistrements en attente, transactions récupérables, journal propre).
- CI exacte-SHA [run 39685194736](https://github.com/Likounsee/OpenFS/actions/runs/39685194736) : **Linux et Windows réussis**, formatage, tests du workspace et Clippy.
- Les estimations restent inchangées : ce garde réduit le risque de modification inattendue pendant l'audit, mais ne prouve pas à lui seul que toutes les voies d'audit sont strictement sans écriture, ni la durabilité physique après coupure.
- `f9dd943` — ajoute un test d'intégration avec injection d'une panne après le commit du journal; Linux a validé formatage, tests workspace et Clippy dans [run 39685969008](https://github.com/Likounsee/OpenFS/actions/runs/39685969008). Windows était encore en cours lors du dernier contrôle.
- Test supplémentaire en cours d'intégration : un audit sur volume propre mais sans inventaire persistant doit échouer sans modifier l'image. Il verrouille le comportement non destructif du chemin d'erreur; il ne remplace pas un test de réussite d'audit sur un inventaire valide.

## Suivi d'avancement — phases P0 à P3 (mise à jour du 2026-10-15)

**Branche de travail :** `OpenFS` uniquement. Cette feuille de route utilise quatre phases, **P0, P1, P2 et P3**. Les pourcentages sont des estimations d'ingénierie fondées sur les preuves répertoriées plus bas, pas des taux de tests réussis.

| Phase | Poids global | Avancement estimé | Périmètre / état |
| --- | ---: | ---: | --- |
| **P0 — Socle d'ingénierie et sécurité** | 10 % | **100 %** | Socle validé; rouvrir en cas de défaut concret de sécurité ou d'intégrité. |
| **P1 — Format, layout, allocateur, transactions et récupération** | 35 % | **79 %** | Fusion pondérée des anciens lots format/allocateur et journal; des blocages de propriété, remplacement de descripteur et transactions générales restent ouverts. |
| **P2 — Vérification, namespace persistant et E/S fichiers** | 30 % | **69 %** | Vérification partielle; create/write/truncate/rename/unlink durables et parcours persistant complet restent à réaliser. |
| **P3 — Périphériques sûrs, intégration OS et livraison** | 25 % | **5 %** | Découverte/formatage sûr et adaptateurs de montage absents; durcissement de livraison principalement planifié. |
| **Global pondéré** | **100 %** | **60 %** | Estimation pondérée : 59,6 % avant arrondi. Ce n'est pas un pourcentage de préparation à la production. |

Les anciens lots sont regroupés sans faire passer du travail non réalisé pour terminé :
- **P0** garde le socle d'ingénierie.
- **P1** rassemble format/layout/allocateur et protocole transactionnel/récupération.
- **P2** rassemble vérification/réparation et namespace/fichiers persistants.
- **P3** rassemble découverte et formatage sûrs, intégration OS, compatibilité et livraison.

### Mise à jour du 2026-10-20

- `9091b25` — corrige le formatage Rust du calcul des octets logiques alloués dans `stat`. La CI exacte-SHA a validé formatage, tests workspace et Clippy sous Linux avant annulation des jobs par un push plus récent; ce n'est pas une validation complète Linux/Windows.
- `b6df8cd` — ajoute les compteurs d'entrées directes, fichiers et sous-répertoires dans `stat`.
- `d868ee9` — documente les précautions des commandes d'inspection/audit et les effets possibles de récupération.
- `8d6f071` — étend `stat` des répertoires avec un parcours itératif du sous-arbre, les totaux récursifs de fichiers/répertoires et la somme des tailles logiques. Les additions sont vérifiées et le parcours évite la récursion sur la pile native.
- `d6cdc15` — documente le comportement récursif de `stat` dans le README.
- Ces améliorations de diagnostic ne prouvent pas la robustesse transactionnelle générale. La CI exacte-SHA du dernier commit reste à vérifier avant de déclarer le lot validé.

### Dernier lot de code documenté

- [`f9869e7`](https://github.com/Likounsee/OpenFS/commit/f9869e7aa7cc4ff3ef05617724300142b5e63e58) — expose les entrées du namespace dans un ordre déterministe pour permettre la validation du graphe complet.
- [`78550e4`](https://github.com/Likounsee/OpenFS/commit/78550e4b46fdaa0faa9a1fbd3b4527da7c39213d) — les chemins de lecture/écriture persistants rejettent désormais un namespace dont les entrées de répertoire diffèrent du disque, même si les enregistrements d'objets correspondent.
- [`f039c36`](https://github.com/Likounsee/OpenFS/commit/f039c36cf9db32f52eba63d15f123f9f0092334b) — ajoute une régression pour les lectures et écritures avec un nom de répertoire falsifié. Le test est présent dans la source, mais son exécution et la CI exacte-SHA ne sont pas confirmées.
- Ces corrections renforcent la validation d'intégrité du chemin d'E/S persistant. Elles ne réalisent pas la création durable de fichiers et ne justifient pas à elles seules une hausse des pourcentages P1/P2.

- `d88e757` / `346ca8c` — ajoute `replace_persistent_namespace_inventory` : sérialisation déterministe du namespace, allocation et flush des nouveaux flux de métadonnées/répertoires, puis bascule copy-on-write via le remplacement transactionnel du descripteur. Le test ajouté couvre la persistance d'une nouvelle entrée et sa reconstruction après réouverture. Cette primitive ne crée pas encore les extents ni le contenu d'un fichier. Les tests et la CI de ce SHA ne sont pas confirmés; aucun pourcentage n'est augmenté sur la seule présence du code.
- `50c5f8f` / `600d866` — ajoute `create_persistent_file` pour créer un fichier vide à partir du namespace reconstruit depuis le disque, puis publier son entrée et ses métadonnées via le chemin copy-on-write; un test vérifie sa résolution après réouverture. L'allocation d'extents et l'écriture de données restent à faire. Tests et CI de ce SHA non confirmés; pourcentages maintenus.
- `404dc80` — renforce le test de création durable : une seconde création avec un nom déjà utilisé doit échouer sans remplacer l'entrée existante. La lecture par chemin persistant existait déjà; pas de nouvelle fonctionnalité revendiquée pour elle. Tests/CI toujours non confirmés et pourcentages inchangés.
- `fa74b5b` — ajoute une régression au cycle de création : écrire dans un fichier persistant encore vide doit échouer sans modifier sa taille, y compris après réouverture. Cela verrouille le comportement actuel; l'allocation d'extents et l'écriture non vide restent absentes. CI exacte-SHA et exécution du test non confirmées; pourcentages inchangés.
- `0284b4c` / `019bb8d` / `6890aa8` — première tranche d'écriture persistante : `write_persistent_file_contents` accepte un fichier vide, alloue un bloc de données et une page d'extents, écrit les octets puis publie taille/racine par remplacement copy-on-write du namespace. Limite explicite : contenu non vide tenant dans un seul bloc; une publication en échec peut laisser des blocs orphelins, jamais libérés de façon spéculative. Le test `writes_initial_persistent_file_contents_and_reads_after_reopen` couvre la relecture après réouverture. CI de ces SHA à vérifier; la CI précédente échouait au contrôle rustfmt avant les tests, donc aucune réussite de tests revendiquée. Pourcentages maintenus tant que le pipeline n'est pas vert.
- **Validation exacte-SHA (2026-10-15, commit `863cfa2`)** — `cargo fmt --all -- --check` passe sous Linux, mais les tests Linux échouent : 224 réussis, 4 échecs. Les échecs sont dans les tests de lecture de plages, de propriété des extents et d'alias inter-fichiers; plusieurs fixtures n'ont pas d'inventaire persistant publié, ce que l'API de lecture exige désormais. Ne pas poursuivre l'extension multi-blocs avant d'avoir réconcilié ces fixtures/contrats et obtenu une CI verte sur Linux et Windows. [Run CI](https://github.com/Likounsee/OpenFS/actions/runs/38802359544).


- [`53a2875`](https://github.com/Likounsee/OpenFS/commit/53a2875748363a1fa69961937547eb281ee43545) — tests des limites de capacité des chunks redo.
- [`fffa639`](https://github.com/Likounsee/OpenFS/commit/fffa6399e12dbc1847d6cd0dd700ee49a610c434) — rejet des compteurs de séquence checkpoint incohérents.
- [`2607070`](https://github.com/Likounsee/OpenFS/commit/2607070113f65bd9d72a79def80427fd27565ddb) — conservation du scénario de récupération d'un checkpoint corrompu sur disque.
- [`5d7ff54`](https://github.com/Likounsee/OpenFS/commit/5d7ff5445475a58ff1b92137f9af3e1903fdfc90) — rejet d'un écart de séquence malgré un checksum valide.

Ces tests renforcent principalement **P1**. Les tests Rust, `cargo fmt` et `cargo clippy` n'ont pas été exécutés localement dans l'environnement de travail utilisé pour cette mise à jour. Les statuts/runs consultés pour le dernier code n'ont pas fourni de résultat exploitable : ne pas en déduire que la CI est verte.

### Prochaine action prioritaire

1. Valider par tests et CI exacte-SHA le remplacement copy-on-write du namespace, notamment les erreurs d'écriture/flush et le résultat incertain du commit.
2. Enchaîner sur **P2** : créer durablement un objet fichier, allouer/persister ses extents et ses données, puis relier `create → write → close → reopen → read` et vérifier depuis la racine persistante.
3. Aborder **P3** seulement lorsque les contrats de persistance/récupération sont suffisamment stables : découverte sûre des périphériques, intégration OS et gates de livraison.

Ne pas annoncer une phase terminée si sa porte de sortie n'est pas satisfaite.

---

## Roadmap opérationnelle révisée — alignée sur les fonctions manquantes d'OpenFS-old

**Source fonctionnelle de cette révision :** [`DIFFERENCES_OPENFS_OLD.txt`](../../DIFFERENCES_OPENFS_OLD.txt), complété par [`OLD_OPENFS_AUDIT.md`](OLD_OPENFS_AUDIT.md). Cette matrice devient le guide de portage : chaque comportement historique utile doit être réimplémenté en Rust, intégré au vrai chemin disque et testé. La présence d'un module ou d'une API seule ne valide pas une fonctionnalité.

### Ordre de priorité à suivre

L'ordre ci-dessous prime sur une lecture naïve des numéros de phases plus bas : il respecte les dépendances techniques et empêche d'empiler des fonctionnalités avancées sur un chemin de persistance incomplet.

| Priorité | Livrable | Fonctions historiques concernées | État de départ | Condition de sortie |
| --- | --- | --- | --- | --- |
| **R0 — Contrats et format** | Figer les règles du format Rust, les erreurs d'E/S, le rôle de `flush`, les versions et les limites. Conserver des fixtures Rust distinctes des images C historiques. | Superblocs, CRC, géométrie, fonctionnalités, régions réservées, périphérique bloc. | **Partiellement fait** : format Rust expérimental et politique de compatibilité documentée; les golden images complètes et la stabilité de format restent à établir. | Images de référence versionnées, tests octet-par-octet et de corruption; refus explicite des images OpenFS-old non compatibles. |
| **R1 — Transaction générale fiable** | Finaliser le contrat du journal : commit durable, capacité, erreurs à résultat incertain, replay idempotent, checkpoints et récupération avant toute exposition des objets. Étendre ensuite les transactions à plusieurs blocs. | WAL BEGIN/DATA/COMMIT, transactions, replay, checkpoints, rollback, échecs d'écriture/flush et crash cuts. | **Partiellement fait** : le journal redo existe et protège surtout des changements d'allocation; ce n'est pas encore le moteur transactionnel général du système de fichiers. | Matrice de panne write/flush/reopen/replay; état final toujours complet (ancien ou nouveau); tests exact-SHA CI; opérations refusées si l'état reste incertain. |
| **R2 — Premier chemin persistant de bout en bout** | Implémenter une table d'objets/inodes et une racine/index canoniques, puis `create/mkdir/lookup/list` persistants; ajouter `read/write/truncate/flush/close`, extents multiples, trous creux et réouverture. | Inodes, générations, bitmap d'inodes, extents, fichiers, répertoires, lecture/écriture, EOF, sparse files, seek-data/seek-hole. | **Bloquant principal** : codecs, allocations, reconstruction et lectures de plages existent, mais pas de cycle complet de mutation durable. | Formater → créer → écrire → fermer → rouvrir → relire les mêmes octets; cohérence transactionnelle entre bitmap, inode, extents, données et répertoire; échecs injectés testés. |
| **R3 — Namespace complet et cycle de vie** | Compléter `unlink/rmdir/rename`, remplacement atomique, liens, générations, compte de liens, objets ouverts mais supprimés et récupération des orphelins. Ajouter handles et verrouillage seulement avec des règles de durée de vie explicites. | Paths, directory operations, hard links, symlinks, open/unlink/close, file descriptors, byte-range locks, concurrence. | **Non intégré** : la validation/résolution du namespace est en mémoire; la mutation persistante complète n'existe pas. | Tests de collisions, répertoires non vides, identité d'objet après rename, open-unlink-close, concurrence déterministe et crash/reopen pour chaque opération multi-métadonnées. |
| **R4 — Vérification complète avant réparation** | Étendre le checker en lecture seule depuis la racine persistante à tous les objets, puis ajouter scrub et diagnostics stables. Les réparations viennent après, limitées, prévalidées, explicites et transactionnelles. | FSCK, bitmap/refcounts, allocations orphelines, extents, CRC, namespace, scrub et réparation. | **Partiellement fait** : vérifications structurelles et d'inventaire présentes; pas de FSCK complet parcourant un filesystem persistant ni de réparation générale. | Checker sans écriture, diagnostics reproductibles, corruption corpus, fuzzing; toute réparation proposée a un dry-run, un journal d'actions et un second check. Refuser les corrections ambiguës. |
| **R5 — Métadonnées et fonctionnalités avancées** | Ajouter par lots séparés les permissions/modes, UID/GID, timestamps, xattrs et ACL; puis CoW/refcounts/clone/discard; snapshots uniquement après stabilisation des références et transactions. | Access checks, ACL, xattrs, time metadata, CoW, refcounts, metadata roots, snapshots. | **Absent ou primitives non intégrées** dans le chemin persistant Rust. | Chaque famille a son format, ses limites, ses erreurs, ses tests positifs/négatifs et ses tests de crash. Ne pas annoncer les snapshots sur la seule présence de primitives CoW. |
| **R6 — Adaptateurs OS et outils sûrs** | Stabiliser l'API portable avant l'adaptateur userspace/OS. Séparer clairement le backend bloc d'un véritable montage de filesystem. Protéger la découverte et le formatage de périphériques. | Mount/unmount, Linux/Windows/ArchiaOS, handles OS, format/check/fsck tools, USB/device workflows. | **Absent** pour le montage Rust; les outils actuels sont orientés images de développement. | Tests OS normaux create/read/write/rename/unlink/flush/unmount/remount. Refus de périphériques système, montés, ambigus ou dont l'identité change; confirmation avant toute écriture destructive. |
| **R7 — Qualification et livraison** | Ajouter fuzz/property tests, stress, benchmarks reproductibles, guide de récupération et matrice de plateformes supportées. | Toute la suite historique de tests, CI GCC/Clang/sanitizers/Windows, crash-cut, performance et opérations. | **Incomplet** : CI Rust existe, mais fuzzing, benchmarks, montage OS et validation matérielle ne sont pas établis. | CI verte sur le SHA exact; corpus de corruption et crash cuts; preuves séparées pour image, OS et matériel; aucun claim de compatibilité ou de durabilité non démontré. |

### Lots de portage à réaliser en parallèle (sans contourner les dépendances)

- **Lot A — format/codec :** superblocs miroir, versions inconnues, champs réservés, géométrie/overflow, fixtures binaires et rejet explicite d'images historiques.
- **Lot B — transaction :** journal multi-blocs, validation de tous les targets avant publication, erreurs à résultat incertain, replay répété et récupération après chaque write/flush.
- **Lot C — données :** allocation d'extents, écritures partielles et multi-blocs, croissance/rétrécissement, trous remplis de zéros, libération uniquement après suppression durable des références.
- **Lot D — namespace :** stockage persistant des répertoires, règles de noms/chemins, create/mkdir/rename/unlink/rmdir et identité stable des objets.
- **Lot E — intégrité :** ownership global, orphelins, CRC/checksums, scrub lecture seule et réparations prévalidées uniquement.
- **Lot F — runtime :** handles, open-unlink-close, locks et concurrence après que les mutations de base soient transactionnelles.
- **Lot G — intégrations :** adaptateur OS, outils image/périphérique et validation matérielle, après stabilité du contrat portable.

Les lots peuvent avancer en parallèle pour les spécifications, fixtures et tests; ils ne doivent pas exposer d'écriture utilisateur avant que R0–R2 et leur contrat de récupération soient satisfaits.

### Règles de portage de la checklist historique

1. `OpenFS-old` est une référence de comportements et de tests, jamais la branche cible. Toutes les modifications d'implémentation vont sur la branche `OpenFS` en Rust.
2. Ne pas copier le format C v1.6 par défaut : le format Rust est distinct. Toute conversion doit être explicite, non destructive et validée avec des fixtures.
3. Convertir les scénarios C utiles en tests Rust, en donnant la priorité aux échecs, corruptions, rollbacks, crash cuts et doubles pannes — pas uniquement aux chemins heureux.
4. Une fonction ne passe à « terminée » qu'après preuve séparée de son implémentation, de son intégration, de ses tests exécutés et du CI exact-SHA pertinent. Les tests d'image ne prouvent ni montage OS ni durabilité physique.
5. Les limites connues d'OpenFS-old restent des limites : son adaptateur WinFsp est essentiellement en lecture seule, son extent tree est peu profond, sa réparation FSCK est limitée et son dernier job Windows crash-cut a échoué par timeout. Ne pas en faire des promesses de parité.
6. **Premier objectif utilisateur réaliste :** un fichier persistant créé, écrit, fermé, relu après réouverture et vérifié par le checker. Le montage USB réel arrive après ce jalon, pas avant.

### Prochaine action prioritaire

Ne plus multiplier les validations isolées du journal tant que la prochaine étape utile n'est pas reliée au chemin de fichier persistant. Après le contrat transactionnel et les prérequis de format, construire le vertical slice **racine persistante → inode/objet → entrée de répertoire → allocation d'extents → écriture/lecture → close/reopen → checker**, en couvrant plusieurs sous-fonctionnalités à la fois et en injectant des pannes dans chaque frontière de persistance.

---

## Executive assessment

OpenFS is currently a Rust filesystem-format and storage-engine prototype, not yet a mountable general-purpose filesystem. The Rust workspace has bounded block-device I/O, an experimental canonical layout, bitmap allocation, a bounded redo journal used for allocator changes, metadata/directory codecs, extent maps, an in-memory namespace, disk-backed inventory reconstruction, a persistent inventory descriptor, file-range reads over reconstructed metadata, and a read-only volume checker. It does not yet provide durable file or namespace mutation APIs, a mount/VFS adapter, safe USB discovery, or a repair utility.

The historical C repository is materially more feature-complete: it contains persistent inode/file/directory/path operations, transactions, links, orphan lifecycle, CoW/refcounts, FSCK/scrub, access metadata, handles, locks/runtime lifecycle, tools and platform adapters. These are source-level capabilities, not a reason to copy the C implementation wholesale. The latest historical CI run inspected for this audit, run 38167327262 on legacy HEAD fdb5dee, passed GCC, Clang and ThreadSanitizer but failed the Windows test job because openfs-crash-cut timed out (50/51 tests passed). Legacy functionality must therefore be revalidated before any behavior is used as a parity target.

The first product milestone remains a safely formatted, disposable USB drive that can be used for ordinary file operations through a supported OS. That milestone is blocked by the absence of persistent namespace writes and an OS mount adapter. Formatting an image and reading caller-located metadata do not constitute a usable filesystem.

## Evidence and confidence rules

Use these evidence labels consistently:

- **Implemented**: source code exists and has a reachable API or command.
- **Integrated**: the feature is wired into its intended end-to-end path, not just a standalone helper.
- **Test added**: a regression exists in the repository.
- **CI validated**: the exact commit has a successful relevant CI run.
- **Unverified**: code/tests exist but no passing run for the exact revision was confirmed, or the behavior needs a different environment.
- **Absent**: no implementation was found in the audited Rust tree.
- **Blocked**: a required design or dependency prevents safe completion.

A test being present is not proof it ran; a green CI run is not proof of physical-device durability or of untested crash cuts. Do not mark an item validated solely because it compiles, has a type/structure, or has a README entry. This audit used GitHub source trees, source files, documentation, workflow definitions, commits and Actions job results; it did not execute a local build or physical hardware test.

## Progress model

The phase percentages estimate progress toward the explicit acceptance gates below, not lines of code or feature count. Each phase has a fixed weight. The initial estimates are provisional until the per-task scoring ledger below is applied; the ledger is the source of truth for subsequent updates. Global progress is the sum of each phase's weight multiplied by its earned phase fraction, divided by the sum of all phase weights, then rounded to the nearest whole percentage point. Do not average phase percentages equally.

**Units and rounding:** phase weights are percentage points of the project and sum to 100. Inside each phase, task weights are normalized points and sum to 100. A task earns between zero and its full task weight; its earned points are based on the strongest evidence actually available for its acceptance criteria, not on the number of files or tests. Phase progress = earned task points / 100 × 100%. Calculate with full precision and round only the displayed phase and global results to the nearest whole percent (0.5 rounds upward). A displayed integer is not a claim of measurement precision; retain the earned-point ledger to make changes reproducible.

**Evidence credit rule:** for each task, score each stated acceptance criterion once, then sum its weighted criterion contributions. A criterion with no implementation earns 0%; code present but not reachable from the intended path earns at most 25%; integrated behavior without a relevant regression test earns at most 50%; a regression test added but not confirmed executed earns at most 60%; a test confirmed executed locally or in a run earns at most 75%; exact-SHA relevant CI success earns at most 90%; 100% is reserved for all task acceptance criteria being met with evidence appropriate to the claim. These are ceilings, not automatic awards. Do not award the same criterion again under another task. For a cross-cutting test, credit it to the task whose behavior it exercises, not again to a generic testing task. Record test-added, test-executed, CI-validated, and physical-device-validated as separate evidence fields.

**Scoring reproducibility limitation:** the task ledger below records weights and current earned points, but some earned values are still expert estimates justified by narrative evidence rather than sums from published subcriteria. Therefore, the phase values and the 40% global figure are **provisional engineering estimates**, not a fully mechanical measurement. Do not describe the score as fully reproducible until every non-zero partial task is split into named, weighted acceptance criteria with an evidence link/status per criterion. Until that decomposition is complete, preserve the current figures for continuity, disclose this limitation, and change a score only when the cited evidence changes. Never invent criterion weights retroactively merely to make an existing estimate appear exact.

| Phase | Weight | Estimate | What the estimate means |
| --- | ---: | ---: | --- |
| P0 Engineering baseline | 10% | 100% | Baseline checks and development guardrails exist; this is not product readiness |
| P1 Format/layout/allocator + transactions/recovery | 35% | 63% | Combined weighted estimate from format/allocator and transaction phases; descriptor/ownership and general transaction work remain incomplete |
| P2 Verification/repair + durable namespace/file I/O | 30% | 26% | Combined weighted estimate from verification and persistent namespace/file I/O; durable mutation and whole-volume traversal remain incomplete |
| P3 Safe device discovery + OS integration + release | 25% | 3% | Combined weighted estimate from device safety, OS integration and release hardening |
| **Overall** | **100%** | **40%** | Exact weighted estimate 40.15%; rounded display 40%, not percentage of a production-ready filesystem |

Status meanings:
- **Not implemented** — no usable implementation found.
- **Partially implemented** — useful subset exists, but core behavior or integration is missing.
- **Implemented, not validated** — implementation/tests exist but the relevant exact revision has no confirmed passing validation.
- **Validated** — stated acceptance criteria passed in the cited automated or manual environment.
- **Blocked** — dependent design, safety condition or prerequisite remains open.
- **Replaced / deferred** — not part of the current implementation path; rationale is recorded.

When reporting a task, state implementation status separately from test status and exact CI evidence. A CI pass applies only to the SHA shown by that run.

## Repository and CI baseline checked for this revision

- Code/source baseline audited for this roadmap: commit [44efa42408258247fdcb99890799f19b8e9c482f](https://github.com/Likounsee/OpenFS/commit/44efa42408258247fdcb99890799f19b8e9c482f).
- Earlier roadmap-only commit: [7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618](https://github.com/Likounsee/OpenFS/commit/7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618). It changes documentation, not filesystem implementation.
- CI run [38379103899](https://github.com/Likounsee/OpenFS/actions/runs/38379103899) completed successfully for exact SHA `44efa42408258247fdcb99890799f19b8e9c482f`; Linux and Windows jobs both passed rustfmt check, `cargo test --workspace` and Clippy with warnings denied.
- CI run [38379956462](https://github.com/Likounsee/OpenFS/actions/runs/38379956462) completed successfully for exact roadmap-only SHA `7e0f50eabe49ac4aeb1e763d9f7fe1181a1bf618`. This validates the configured automated checks on that revision; it does not constitute new implementation evidence.
- Latest roadmap-only commit before this revision: [4b0ee02b974ceecc10379378b72d20e168d4d6bf](https://github.com/Likounsee/OpenFS/commit/4b0ee02b974ceecc10379378b72d20e168d4d6bf); its exact-SHA CI run [38423229047](https://github.com/Likounsee/OpenFS/actions/runs/38423229047) passed Linux and Windows. This is documentation validation only, not new implementation evidence.
- Earlier run 38375379163 on `ce36ced79a50676553e7f9bb7d0b1bb3f82fce7c` and run 38375058616 on `1a7d2ea32cd21a4c6674914b4945a2ea878aa258` are historical evidence only; neither is the latest run for the current branch.
- Legacy HEAD inspected: `fdb5dee16266d037c79438a5b20ebccceab3fa24`. Its latest Actions run found, 38167327262, failed on Windows because `openfs-crash-cut` timed out; GCC, Clang and ThreadSanitizer jobs passed. The previous legacy run 38166246104 succeeded on an earlier commit, not current legacy HEAD.
- Rust CI covers formatting, workspace tests and Clippy on Linux and Windows. The latest ownership-validation batch is exact SHA `c7b93f1ee77869529d869ce4d3ae15ef6dae52aa`, validated by run [38437650554](https://github.com/Likounsee/OpenFS/actions/runs/38437650554) with both platform jobs successful. It bounds extent-allocation bitmap caching to one page while scanning sorted physical ranges, replaces per-range linear root-alias checks with binary search, and adds `namespace_extent_map_root_checks_scale_across_all_supported_block_sizes` for 32 file roots at each supported block size. No dedicated fuzzing, sanitizers, benchmarks, power-loss hardware testing or mounted-filesystem integration was evidenced in the inspected workflows.
- Exact-SHA CI run [38453937847](https://github.com/Likounsee/OpenFS/actions/runs/38453937847) passed for `536a744d46e0c0b12616f92e0f07bbcbe69343a7` on both Linux and Windows (formatting, workspace tests, Clippy). The `refuses_to_publish_when_existing_allocations_have_no_owner` regression now exercises all five supported block sizes (4, 8, 16, 32 and 64 KiB) and confirms a rejected inventory publication leaves the orphan allocated, consumes no descriptor block, and remains undiscoverable.
- Exact-SHA CI run [38455410660](https://github.com/Likounsee/OpenFS/actions/runs/38455410660) passed Linux and Windows formatting, workspace tests, and Clippy for `7db08563db31540bb62f0d1f7eb2ab245857e017`. It validates the five-size staging-flush failure regression after closing and reopening the image, including undiscoverability and successful retry.
- Exact-SHA CI run [38456414882](https://github.com/Likounsee/OpenFS/actions/runs/38456414882) passed formatting, workspace tests and Clippy on Linux and Windows for `a68968118edee7e854bdda0233ed88843278c970`. This includes the journal commit-record flush-failure recovery regression: after the commit record has been written but its flush reports an error, reopening replays the committed redo transaction. The image backend exposes writes before the injected flush error; this is an uncertain-outcome recovery test, not physical power-loss proof.
- Exact-SHA CI run [38457627241](https://github.com/Likounsee/OpenFS/actions/runs/38457627241) passed formatting, workspace tests and Clippy on Linux and Windows for `38dc8b8e75d0b468d1ec4892ab3d9c468db605ad`. `protected_recovery_validates_all_targets_before_replaying_any_update` constructs a durable transaction with one valid home target followed by a protected control-block target and confirms recovery rejects the transaction without partially applying the first update or modifying the protected block.
- Exact-SHA CI run [38458063369](https://github.com/Likounsee/OpenFS/actions/runs/38458063369) passed formatting, workspace tests and Clippy on Linux and Windows for `996b4566f60f196d618c14f17d0de1b728b8299d`. `malformed_nonterminal_record_before_later_valid_records_fails_closed` damages the BEGIN record of a durable but uncheckpointed transaction and confirms recovery refuses the malformed interior record rather than treating it like a safely ignorable terminal append.
- Exact-SHA run [38459392863](https://github.com/Likounsee/OpenFS/actions/runs/38459392863) passed formatting, workspace tests and Clippy on Linux and Windows for `97ebd67d4470dc72000430b41d9686940c72c3a2`. `counter_overflow_is_rejected_before_any_persistent_write` covers transaction-ID and checkpoint-generation overflow; `recovery_counter_overflow_is_rejected_before_replaying_home_blocks` confirms recovery rejects exhausted checkpoint generation before replay.
- Exact-SHA run [38460415085](https://github.com/Likounsee/OpenFS/actions/runs/38460415085) passed formatting, workspace tests and Clippy on Linux and Windows for `3c8efe9f8eae716412634e8b91ba2831d259ab85`. `counter_overflow_is_rejected_before_any_persistent_write` now also covers record-sequence overflow before the first persistent write; `recovery_sequence_overflow_is_rejected_before_replaying_home_blocks` confirms sequence exhaustion is rejected before home-block replay and leaves home bytes unchanged. The preceding implementation commit `5d19382d525b20c0283262b4f98b83de4982ec75` was corrected for rustfmt before this passing run; its first CI attempt failed only the formatting check.
- Exact-SHA run [38460922848](https://github.com/Likounsee/OpenFS/actions/runs/38460922848) passed formatting, `cargo test --workspace` and Clippy on Linux and Windows for `87947011d4ef2cf51b2ef025d0bc042fc116d946`. `every_commit_io_failure_recovers_to_a_complete_home_image` injects failures at all ten I/O operations for a one-block 4 KiB transaction and checks that reopen yields the old image before a written COMMIT, or the fully committed image once the COMMIT write is present. This software image test does not model a physical device that loses acknowledged writes or lies about flush.
- Descriptor publication and journal recovery now have separate injected staging-write, staging-flush, journal-commit-flush, uncommitted-transaction, torn-record, replay-failure, protected-target and counter-overflow regression coverage. The crash-cut matrix is still not exhaustive across every device/flush boundary, malformed-ring arrangement and real storage failure mode; a normal CI pass is not physical durability evidence.
- No physical USB device was accessed and no local commands were run. All CI claims here refer to remote GitHub Actions evidence and the exact SHA stated above.

## Source inventory and actual Rust capabilities

Workspace modules are openfs-core, openfs-block, openfs-format and openfs-tool. Rust forbids unsafe code at workspace lint level. The core crate provides checked block-range arithmetic and shared errors. The block crate defines BlockDevice read/write/flush, image and raw-file backends, and deterministic operation/write-tear injection. The format crate owns the layout, superblock, bitmap allocator, journal/checkpoints, metadata/directory codecs, extent maps, inventory descriptor, namespace reconstruction and verification. The tool crate provides development image commands, including a read-only check command.

Capabilities found in source:
- Volume: format/open/check, canonical geometry, primary/mirror superblocks, allocation/free, allocation/free-space queries, persistent inventory discovery/publication, namespace reconstruction from supplied or discovered inventory, extent-map loading, file range/content/path reads.
- Journal: bounded redo ring, checkpoint pair, sequence checks, protected home-target checks, replay and retry paths; current use is allocator metadata, not arbitrary multi-object filesystem transactions.
- Metadata: fixed-width checksummed metadata and directory records; checksummed extent-map pages; namespace graph and path APIs in memory.
- Verifier: persistent inventory/ownership consistency report for caller-located or descriptor-discovered records, including orphan/dangling diagnostics.
- CLI: image-focused development tooling; not a mount service or safe destructive raw-device workflow.

Critical capabilities still incomplete in the Rust workspace:
- A general atomic transaction spanning namespace records, extent maps, allocation bitmap and file-data publication. Persistent create/write/truncate/rename/remove and directory mutation APIs exist, but their multi-stage crash behavior is not one universal transaction.
- A canonical, bounded, scalable metadata region/index; descriptor discovery still includes a data-region scan.
- Automatic safe recovery/repair of orphaned allocation after interrupted higher-level file/namespace operations.
- Persistent inode generation/link-count lifecycle, symlinks/hard links, permissions/ACL/xattrs, timestamps and open-unlinked inode handling.
- Mount/unmount, VFS integration, file handles, locking/concurrency control and OS adapters.
- Full FSCK with repair, a standalone scrub command, fuzz targets and performance/load benchmarks.
- USB identity discovery, partition/system-volume refusal, exclusive device ownership, and a user-confirmed formatter.
- Stable format release policy, versioned golden images and a legacy image converter.

## Comparison with OpenFS-old: preserve behavior, not the implementation

The legacy source tree contains a C core with format, bitmap/allocator, inode allocation, inode, extent, file, directory, path, link, journal, transaction, mount, orphan, FSCK and repair, scrub, CoW, metadata CoW/root, xattr, ACL, time, file descriptors, file locks and runtime lifecycle modules. It has 50+ registered CTest cases in the current CMake configuration and adapter/tool code for Linux, Windows and ArchiaOS. The legacy test inventory includes file/directory/path operations, concurrency, crash cuts, journal failure paths, FSCK repair failures, orphan handling, checksums, links and transaction ordering. The current legacy Windows crash-cut timeout is a real validation issue, so do not equate test count with a fully green release.

### Reimplement or adapt from legacy (in dependency order)

1. **Format contract and error boundaries** — compare the C v1.6 superblock/feature flags, primary+backup geometry, reserved regions, journal and metadata layout with the Rust draft. Rust intentionally defines a new incompatible format; reject legacy images clearly. Do not silently import the C v1.6 layout or call it compatible.
2. **Transactional allocator/inode lifecycle** — reuse the legacy tests for allocation rollback, inode bitmap/generation reuse, exact ownership, overflow and disk-full cases as test-design references. Reimplement against Rust types and the new format.
3. **File and extent semantics** — adapt tests and invariants for fragmented extents, sparse holes, EOF, partial blocks, truncate growth/shrink, zero-fill, overflow, freeing blocks only after references are removed, and failed I/O rollback. Rust has persistent file writes, range writes, sparse extension, and truncation APIs; continue testing fragmented extents, sparse holes, EOF, partial blocks, overflow, disk-full behavior, and crash recovery.
4. **Directory/path semantics** — adapt tests for nested paths, parent references, name limits, trailing slash, file-as-intermediate, rename collision, moving directories, non-empty directory removal and case sensitivity. The namespace layer is in-memory, but persistent path APIs publish copy-on-write inventories. Keep distinguishing that persistence from a single atomic transaction across all affected data and metadata.
5. **Transaction/crash matrix** — use legacy transaction, replay-idempotence, crash-cut, double-failure and namespace-crash tests as scenarios, not as proof the Rust journal already supports the same semantics. Specify every on-disk mutation and commit point before porting.
6. **Checker/repair/scrub** — first implement a non-mutating whole-volume checker; only then design explicit, previewable repairs. Adapt FSCK diagnostics, exact block ownership/refcount reconciliation, bitmap tails, root/parent/link invariants and scrub read-error coverage.
7. **Lifecycle/concurrency** — later adapt open handles, open-but-unlinked objects, orphan recovery, runtime admission/drain, lock ordering, file-range locks and concurrent namespace/file operations. These depend on durable mutation and a defined mount lifecycle.
8. **Advanced optional features** — CoW/refcounts, clone/discard, metadata CoW/root, snapshots, ACLs, xattrs and links should follow durable baseline operations, and each must be independently specified and tested. Do not make these prerequisites for the first simple USB file workflow unless the format design requires them.
9. **OS glue/tools** — port concepts from Linux/Windows/ArchiaOS adapters and format/fsck tools only after the Rust core API is stable. Keep OS-specific code out of openfs-format.

### Legacy hazards and compatibility rules

- Legacy documentation has format-version drift (v1.3/v1.5 documents versus source declaring v1.6); treat source plus tests as evidence and resolve discrepancies before any parity claim.
- Legacy C has a much broader feature surface, but C rollback, locking and lifecycle paths are complex and require fresh validation. The current Windows timeout is not to be ignored.
- Rust format is a separate draft. No migration/conversion tool is present; future conversion must be explicit and tested against versioned golden fixtures.
- NTFS is not an on-disk compatibility target. “NTFS-inspired” or feature comparison must not be described as NTFS mount compatibility.

## P0 — Engineering baseline and safe development contract
**Status: Validated as a baseline only — 100%. Weight: 10%.**

Implemented and validated:
- Rust workspace and four-crate dependency direction.
- Bounds-checked block I/O and shared errors.
- Image/raw-file device abstractions and I/O fault injection.
- Explicit superblock encoding/decoding, CRC and reserved-byte checks.
- Linux/Windows CI for formatting, workspace tests and Clippy.
- Image creation refuses overwrite unless the explicitly destructive API is selected.

Remaining rule: reopen this phase if a concrete integrity or safety defect is found. P0 completion is not production readiness.

**Exit criteria:** baseline CI green on both OS jobs for the exact SHA; no known untriaged baseline blocker; contributors know how to distinguish image tests from physical-device guarantees.

## P1 — Canonical format, layout, allocator, transactions and crash recovery
**Status: Partially implemented; validation advancing.** This subsection is part of P1's combined 35% weight.

- **Current P1 work (2026-10-14):** [descriptor publication and ownership contract](P1_DESCRIPTOR_PUBLICATION_CONTRACT.md) documents descriptor states, the bitmap/journal commit point, ownership invariants, uncertain-I/O handling and required crash-cut evidence. Commit [`63b84cc`](https://github.com/Likounsee/OpenFS/commit/63b84cc695c5626e0f8a49896c9de636bb8d12d8). Regression `refused_descriptor_publication_preserves_exact_image_bytes_across_block_sizes` was added in [`70acfe1`](https://github.com/Likounsee/OpenFS/commit/70acfe1963eb78b3be3f4ccc8d90d44b40074b34); it checks exact image-byte preservation after rejected publication at all five block sizes. At this roadmap edit, execution and exact-SHA CI for that test are **not yet confirmed**, so it earns no validated progress credit.
- **Format compatibility policy (2026-10-14):** [FORMAT_COMPATIBILITY_POLICY.md](FORMAT_COMPATIBILITY_POLICY.md) now specifies immutable released versions, fail-closed handling of unknown versions/features, deterministic encoding, explicit conversion, recovery compatibility and golden-fixture acceptance criteria. Commit [`6abf422`](https://github.com/Likounsee/OpenFS/commit/6abf422d8cee32a5b16661a44084fe3ba5cc3d10). This is a written policy only; binary golden fixtures, compatibility tests and conversion remain unimplemented and receive no completion credit.
- **Descriptor format regression (2026-10-14):** `descriptor_has_stable_golden_bytes_and_checksum` now pins the 4 KiB descriptor header, CRC32C (`0xd4b32501`), zero padding and decode result. Commit [`2d1f9fd`](https://github.com/Likounsee/OpenFS/commit/2d1f9fd2a6c2ea5cf93ac7ec927cfc1df8a89e1f). This adds a stable descriptor-byte fixture in test source, not a complete golden volume image; no execution or exact-SHA CI result is confirmed yet.
- **Cross-list alias rejection regression (2026-10-14):** the fixture uses record counts consistent with both one-block lists and expects the descriptor codec's duplicate-physical-block rejection before any disk writes. Commit [`4e2d25f`](https://github.com/Likounsee/OpenFS/commit/4e2d25fda337b8bd91e424f85497b7f45ab306a9); exact-SHA CI run [38528841110](https://github.com/Likounsee/OpenFS/actions/runs/38528841110) passed on Linux and Windows. The test compares the complete image before/after and verifies that no descriptor becomes discoverable.
- **Invalid-inventory publication regression (2026-10-14):** commit [`746b3bd`](https://github.com/Likounsee/OpenFS/commit/746b3bd55034d885deeabab0761b5f3a385ac334) checks that an inventory whose referenced metadata block has no valid metadata record is rejected without changing a single image byte, consuming a free block, or making a descriptor discoverable, at all five supported block sizes (4, 8, 16, 32 and 64 KiB). Exact-SHA CI run [38529074047](https://github.com/Likounsee/OpenFS/actions/runs/38529074047) passed on Linux and Windows.
- **Descriptor codec block-size validation (2026-10-14):** decoder and encoder reject unsupported sizes outside the 4–64 KiB power-of-two contract. The decoder guard passed exact-SHA Linux and Windows CI in run [38530234139](https://github.com/Likounsee/OpenFS/actions/runs/38530234139) for commit [`e5d01b2`](https://github.com/Likounsee/OpenFS/commit/e5d01b2c354eade94b816e0b954bb5ec61438574); the follow-up encoder-rejection assertion is covered by exact-SHA run [38530564695](https://github.com/Likounsee/OpenFS/actions/runs/38530564695) for [`5b31109`](https://github.com/Likounsee/OpenFS/commit/5b31109e7d6ca4013a362055dc19ba6be2514dd0), with both platform jobs successful.
- **Sparse and duplicate descriptor discovery regressions (2026-10-14):** the sparse-volume test places a descriptor on the final data block and checks discovery across all five block sizes; the duplicate-descriptor test verifies fail-closed behavior when two allocated descriptors are present. Commits [`c946b6a`](https://github.com/Likounsee/OpenFS/commit/c946b6aa420d0f5ee2e2e9e243ab8169696c03e6), [`6fcf649`](https://github.com/Likounsee/OpenFS/commit/6fcf6494433517df49963b10f92a17dd499c75bc) and [`d81cd1e`](https://github.com/Likounsee/OpenFS/commit/d81cd1e16f3467d4b110432ed2e0bd58de72f40d). Exact-SHA CI run [38529717836](https://github.com/Likounsee/OpenFS/actions/runs/38529717836) passed formatting, workspace tests and Clippy on Linux and Windows.

Implemented:
- Canonical layout calculation with checked geometry and supported block sizes from 4 KiB through 64 KiB.
- Primary and backup superblocks; bitmap, checkpoint copies and four-block redo ring; reserved/padding bitmap validation.
- Allocation/free, exhaustion, double-free rejection, reuse after reopen, free-space and free-extent queries, contiguous allocation/free helpers and fault injection.
- Journaled bitmap allocation changes and layout/bitmap corruption tests.
- Persistent metadata/directory codecs, extent-map page codec and ownership verifier foundations.
- Persistent inventory descriptor publication/discovery, bounded batched reads and fail-closed handling for malformed, duplicated or unallocated references.
- Descriptor publication stages the encoded descriptor in a free data block and flushes it before journaling the allocation bitmap. The bitmap commit is the visibility point: discovery scans allocated blocks, so an uncommitted staged descriptor remains invisible after reopen. Tests fail each journal/home/checkpoint write position and each flush boundary across all five supported block sizes, requiring recovery to either the old state or the complete descriptor. Torn-prefix writes to the staging block are tested at five prefix lengths per block size; the descriptor remains invisible, and reuse clears stale descriptor magic.
- Allocator recovery regression: commits [`1eaf85ed7fa001249666c57f7947d1b58f0d3d13`](https://github.com/Likounsee/OpenFS/commit/1eaf85ed7fa001249666c57f7947d1b58f0d3d13) and [`32984d4e12dd46339fa48adc7d1f893eab93aa7c`](https://github.com/Likounsee/OpenFS/commit/32984d4e12dd46339fa48adc7d1f893eab93aa7c) cover stale descriptor staging through single, batch, contiguous and best-fit allocation, plus a simulated scrub-flush failure that must leave the bitmap allocation free and allow a safe retry. CI run [38448265235](https://github.com/Likounsee/OpenFS/actions/runs/38448265235) passed Linux and Windows for the fail-closed regression test.
- **Persistent descriptor and inventory lifetime:** earlier commits [`09c169de84876e6fb71f4aff901f1860e32700de`](https://github.com/Likounsee/OpenFS/commit/09c169de84876e6fb71f4aff901f1860e32700de), [`780fd760a1ff226197a7729da395029582b46077`](https://github.com/Likounsee/OpenFS/commit/780fd760a1ff226197a7729da395029582b46077), [`ba9e006db0364b2f497925bba76b1bb7c334a319`](https://github.com/Likounsee/OpenFS/commit/ba9e006db0364b2f497925bba76b1bb7c334a319) and [`c8ee192bf14f62e943c360f595ba8a169619786c`](https://github.com/Likounsee/OpenFS/commit/c8ee192bf14f62e943c360f595ba8a169619786c) protect the descriptor block itself. New guard [`e12a0b8`](https://github.com/Likounsee/OpenFS/commit/e12a0b86b492bc4b7e19988baa3e060fd4fca384) also refuses to free any metadata/directory block referenced by the descriptor through single, batch and contiguous APIs; regression `allocator_free_apis_preserve_persistent_inventory_blocks` covers all three APIs and all five block sizes. The old corruption test now clears the on-disk bitmap directly to retain hostile-image coverage. Exact-SHA CI run [38537199290](https://github.com/Likounsee/OpenFS/actions/runs/38537199290) passed formatting, workspace tests and Clippy on Linux and Windows for [`9c7f7ee`](https://github.com/Likounsee/OpenFS/commit/9c7f7eea2081cba225739ab97aa703a52d8c8ec5). Follow-up cache optimization [`f45d133`](https://github.com/Likounsee/OpenFS/commit/f45d1334b93604b11182d2f22bc565b63797b003) avoids rescanning the volume on every free call; its exact-SHA CI is pending and earns no separate credit yet.

Open blockers:
- The initial descriptor publication now has an explicit visibility point and tested old-or-complete recovery for journal write failures, flush failures, and torn-prefix staging writes. This does not prove physical-device power-loss behavior and does not solve general multi-object transactions.
- Descriptor replacement is implemented with one journal COMMIT covering the new descriptor allocation, old descriptor release and obsolete inventory-block release across affected bitmap pages. Regression tests cover write-failure recovery and cross-bitmap-page transitions; the latest exact-SHA run for this test batch is still pending Windows completion. The canonical locator still scans the data region, and general namespace mutation remains unsupported.
- Discovery scans the whole data region and is not suitable as a final scalable metadata locator.
- Ownership checking depends on the current inventory representation; no full durable namespace mutation path uses it yet.
- Golden image fixtures and an explicit stable format release contract remain prerequisites before format freeze.

**Tasks and acceptance criteria**
1. **P1.1 Format invariants — Implemented; validation ongoing.** Keep golden fixtures for each supported block size; malformed geometry, reserved bytes, version, backup mismatch, overflow and truncated image must fail closed without writes.
2. **P1.2 Allocator invariants — Implemented; CI validated for recorded batches.** Every successful allocate/free must persist across reopen; no reserved/double-free/out-of-range allocation; exhaustion must not advance the checkpoint or corrupt prior allocations.
3. **P1.3 Ownership contract — Partially implemented; allocator free protection validated.** Define the owner of each metadata block, extent root and data block. Reject aliases, dangling references, allocated orphans and freeing blocks still referenced by the persistent inventory. Extent-root/data ownership transitions remain incomplete.
4. **P1.4 Descriptor lifecycle — Replacement implemented; broader lifecycle remains incomplete.** `replace_persistent_inventory_descriptor` stages and flushes the replacement while free, preflights bounded journal capacity, then atomically switches bitmap ownership through one journal COMMIT. Tests cover write-failure recovery, cross-bitmap-page transitions, and idempotent repeated publication/replacement. Remaining work: scalable canonical locator/index, golden volume fixtures and general persistent namespace transactions. Do not treat this as format-freeze or physical-device durability evidence.
5. **P1.5 Metadata locator scalability — Not implemented.** Replace full-data-region scan with a canonical locator/index before scale claims; test descriptor discovery when metadata is sparse and near the end of large images.
6. **P1.6 Format compatibility — Partially documented.** Add golden images and explicit version-change rules before writing durable namespace mutations.

**Exit gate:** deterministic format/open/allocate/free/reopen and corruption tests pass on Linux/Windows; descriptor lifecycle and ownership invariants have a safe recovery story; no unresolved known allocator/format corruption blocker. P1 must not be called complete while descriptor publication can leave unrecoverable ownership anomalies.

### P1 continued — Transaction protocol and crash recovery

- **Exact-SHA CI validation (2026-10-19):** commit [`0729c8d`](https://github.com/Likounsee/OpenFS/commit/0729c8d2ba9334f48ed4ed6c114521c0fe8f36a0) passes formatting, workspace tests and Clippy on both Linux and Windows in [workflow `39291340964`](https://github.com/Likounsee/OpenFS/actions/runs/39291340964). This fixes a regression-test expectation so a path write's expected file image includes the earlier successful partial write. It validates the existing bounded write/reopen path; it does **not** close the general transaction/crash-cut matrix, so P1 progress remains 63%.
**Status: Partially implemented.** This subsection is part of P1's combined 35% weight.

- **Transaction/recovery contract (2026-10-14):** [P2_JOURNAL_TRANSACTION_CONTRACT.md](P2_JOURNAL_TRANSACTION_CONTRACT.md) now records the current bounded-ring state machine, commit uncertainty, recovery/retry rules, capacity preflight and explicit limits. Commit [`1a0e603`](https://github.com/Likounsee/OpenFS/commit/1a0e603136da5a03ef221e97caea0038b312b826). This is a normative documentation step, not proof of complete crash-cut coverage and not a reason to raise P1 progress by itself.
- **Capacity preflight retry regression (2026-10-14):** commit [`34ed75e`](https://github.com/Likounsee/OpenFS/commit/34ed75e0ebf77331a2f12ae0d0704f6983fb6899) extends `rejects_journal_targets_and_ring_exhaustion`: after an oversized transaction is rejected without changing persisted bytes, a valid transaction is committed using the same journal instance. Exact-SHA CI is pending; no validation credit is awarded until both OS jobs pass.
- **Writer poisoning after uncertain I/O (2026-10-14):** commit [`9a1456b`](https://github.com/Likounsee/OpenFS/commit/9a1456b413c637fc13f0108bf3b2d67a2fede29d) extends the per-I/O-failure recovery test: after each injected commit I/O error, another commit through the same live journal must fail with the explicit recovery-required error before the test reopens and checks recovered home bytes. Exact-SHA CI is pending; no validation credit is awarded yet.
- **Writer resumes after recovery (2026-10-14):** commit [`07448b8`](https://github.com/Likounsee/OpenFS/commit/07448b8730cb8a4f6bf2bbc4db94846b6cacee7f) extends every injected commit-I/O failure case to reopen the journal, verify the recovered home image/checkpoint, commit a fresh transaction and verify the new data. Exact-SHA CI is pending; no validation credit until Linux and Windows pass.

Implemented:
- Bounded single-writer redo journal, checkpoint copies, sequence and framing checks, protected home-target checks and recovery of committed allocator changes.
- Tests for commit/reopen, failed home writes, ignored incomplete commit, torn terminal COMMIT, torn home writes, torn checkpoint writes across several byte prefixes, retryable recovery and repeated journal use after recovery.
- Commit preflights transaction ID and checkpoint generation before its first write; recovery preflights sequence, transaction ID and checkpoint generation before replay. Tests cover commit-side transaction-ID, checkpoint-generation and record-sequence overflow, plus recovery-side generation and record-sequence overflow with home-byte preservation.
- Exact-SHA Rust CI run [38460415085](https://github.com/Likounsee/OpenFS/actions/runs/38460415085) passed formatting, workspace tests and Clippy on Linux and Windows for `3c8efe9f8eae716412634e8b91ba2831d259ab85`.
- Exact-SHA Rust CI run [38460922848](https://github.com/Likounsee/OpenFS/actions/runs/38460922848) passed formatting, workspace tests and Clippy on Linux and Windows for `87947011d4ef2cf51b2ef025d0bc042fc116d946`; it verifies all ten injected commit I/O failure points for a single 4 KiB home-block update and checks old-or-committed home bytes after reopen.

Absent/incomplete:
- General transactions spanning metadata records, directory entries, extent-map blocks and data blocks.
- A formal public transaction API and a defined indeterminate-result/write-poisoning contract for every device error.
- Full crash-cut matrix across all block sizes, all journal writes/flushes/checkpoint transitions and all namespace mutations.
- Transaction capacity beyond the four-block ring. Do not solve this by silently allowing oversized transactions or overwriting uncheckpointed records.
- Proof of physical-device durability; host flush semantics are only software ordering evidence.

**Tasks and acceptance criteria**
1. **P1.7 Normative state machine — Documented; implementation validation ongoing.** The contract records BEGIN/redo/COMMIT/home writes/flush/checkpoint states, indeterminate I/O, preflight rejection, and recovery-before-retry rules. Exhaustive fault evidence and API-wide enforcement remain incomplete.
2. **P1.8 Fault-injection harness — Partially implemented.** Existing tests inject selected write/flush failures and torn prefixes, reopen after each cut, and compare home state. New regression explicitly requires a live writer to fail closed after each injected commit I/O error; broad block-size/partial-write cross-product remains incomplete.
3. **P1.9 Recovery correctness — Partially implemented.** Repeat replay and inject failures during replay/checkpoint. Committed transactions must be replayed completely and idempotently; uncommitted transactions must not be published.
4. **P1.10 Capacity design — Partially implemented.** Current four-slot ring capacity is preflighted before writes and oversize rejection preserves image bytes; a follow-up regression verifies a valid commit still works after rejection. A larger/versioned journal and general transaction-capacity design remain unimplemented.
5. **P1.11 Namespace transaction — Not implemented.** Atomic create/rename/delete must update namespace records, directory indexes, extent ownership and bitmap in a defined order. Crash outcomes must be exactly old or new state, not mixed state.
6. **P1.12 Durability contract — Partially documented.** Distinguish process crash, OS crash, power loss and hardware that lies about flush; hardware guarantees require dedicated devices and test plans.

**Exit gate:** persisted-byte crash-cut matrix is green on Linux and Windows; each mutation recovers to one permitted complete state; replay is idempotent; journal exhaustion and uncertain I/O fail closed. Only then expand the journal to general filesystem transactions.

## P2 — Read-only verifier, corruption corpus and repair design
**Status: Partially implemented.** This subsection is part of P1's combined 30% weight.

Implemented:
- check_volume validates superblocks, canonical layout, bitmap reserved/padding invariants, allocation totals and journal/checkpoint state without replay or modifying the image.
- CLI `check-image` byte-preservation regressions: valid image, pending redo, one damaged checkpoint with a valid peer at all five supported block sizes, and both checkpoint copies damaged. A single damaged copy is tolerated without an on-disk repair; two invalid copies are rejected.
- CLI `audit-image` regression at all five supported block sizes: when one checkpoint copy is damaged but its peer is valid, a failed audit caused by a missing inventory must preserve every image byte and must not rewrite the damaged checkpoint.
- Persistent inventory verifier composes namespace reconstruction, extent maps and allocation ownership, with orphan/dangling reports when inventories are available.

Incomplete:
- Full whole-volume traversal from a canonical metadata root/locator is not yet possible for every image.
- No standalone FSCK repair command; no explicit repair preview/plan/apply protocol.
- No fuzz target, long-running malformed-input corpus or sanitizer/fuzz CI.
- Current checker cannot prove data content checksums because the Rust format has no general persistent data checksum table.
- Read-only check and writable open are distinct: check must never replay or repair silently.

**Tasks and acceptance criteria**
1. **P2.1 Read-only structural check — Partially validated.** Corrupt each superblock/checkpoint/bitmap/journal field and verify deterministic diagnostics, no writes and no panic.
2. **P2.2 Full object traversal — Blocked by P2 metadata locator.** Validate every object, parent/reachability edge, directory record, extent map, allocated block and owner count from disk.
3. **P2.3 Diagnostics contract — Not implemented.** Stable machine-readable codes, block/object identifiers, deterministic ordering and severity; avoid opaque error-only output for recoverable ownership anomalies.
4. **P2.4 Fuzzing — Not implemented.** Add bounded fuzz targets for superblock, journal/checkpoint, metadata/directory codecs, extent-map and descriptor decoders. Keep discovered failures as regression fixtures.
5. **P2.5 Repair planning — Not implemented.** Read-only scan first; explicit dry-run report, narrow repair set, backup/rollback policy and second verification pass before any repair write.
6. **P2.6 Data integrity — Not implemented.** Decide whether the new format needs data checksums, then specify their on-disk coverage and transaction semantics before enabling them.

**Exit gate:** checker covers every reachable persistent object, reports known corruption, never mutates, and malformed input is fuzzed within a documented budget. Repair remains opt-in and separate from checking.

### P2 continued — Durable namespace, files and directories
**Status: Partially implemented, read-only only — 21%. Weight: 20%.**

Implemented:
- Fixed-width checksummed metadata records and directory entries.
- Checksummed extent-map pages, sparse logical mapping, extent coalescing, bounds/alias validation and physical range removal in memory.
- In-memory namespace create/lookup/list/path/rename/remove validation.
- Disk-backed inventory readers, descriptor-based discovery, namespace reconstruction, extent-map loading, and file range/content/path reads including fragmented extents, sparse zero-fill and EOF handling.
- A bounded `write_file_range` primitive now stages complete home-block images and commits all touched data blocks in one redo-journal transaction. It validates the persisted namespace/extent ownership graph, preserves bytes outside partial-block writes, rejects sparse holes and writes past EOF, and preflights journal capacity before writing data. After an I/O error during commit, the volume must be reopened for recovery before further mutations.
- Tests cover persistent namespace discovery after reopen, writes spanning two physical extents across all supported block sizes, and refusal to free blocks still owned by file extents.
- The complete persistent mutation lifecycle is still absent: no durable create/truncate/rename/unlink API and no allocation or extension for writes. Multi-block data writes now use journaled redo when they fit the bounded ring; injected-failure coverage and the transactionally consistent metadata lifecycle are still required.

Required design:
- Choose the persistent metadata root/index and allocation/ownership model.
- Specify directory storage and growth, file lengths, object IDs/generations, data initialization, sparse semantics, name normalization/case policy and error mapping.
- Decide the supported first-release semantics before implementing optional POSIX/Windows features.
- Keep data blocks and metadata publication transactionally consistent; do not release an old block before the new metadata no longer references it.

**Tasks and acceptance criteria**
1. **P2.8 Canonical metadata locator/root — Not implemented.** Versioned root points to bounded metadata and directory structures; validate all pointers before use.
2. **P2.9 Durable read-only mount model — Partially implemented.** Open from only on-disk roots without caller-supplied block lists; remount must reconstruct the same object graph.
3. **P2.10 Durable create/mkdir — Not implemented.** Allocate IDs and blocks, publish parent entry and metadata atomically; failure/reopen must show either absent or complete object.
4. **P2.11 Durable write/truncate — Journaled bounded-write primitive only.** `write_file_range` stages all touched data blocks and submits them as one redo transaction; ranges that exceed journal capacity are rejected before data writes. Still required: allocation, extension/truncation, metadata/data atomicity as one lifecycle, and injected-I/O-failure tests proving reopen/recovery. Test empty/large/fragmented/sparse files, partial blocks, grow/shrink and no-space; after recovery, the transaction must be fully applied or absent.
5. **P2.12 Durable rename/unlink/rmdir — Not implemented.** Test collisions, moving directories, non-empty directories, same-name cases, stale references, link counts when supported and crash cuts.
6. **P2.13 Directory growth and large inventories — Not implemented.** Test capacity boundaries, pagination, bounded memory and no duplicate/lost entries.
7. **P2.14 Semantics — Partially implemented in memory only.** Path resolution, rename/remove validation and case-sensitivity behavior have in-memory coverage; durable on-disk semantics and the full first-release contract remain undefined. Specify path normalization, absolute/relative paths, trailing separators, name encoding, timestamps, permissions and file type behavior.
8. **P2.15 Read API plus journaled bounded-write primitive.** Range/content/path reads and `write_file_range` exist, but there is no complete durable mutation API or handle lifecycle. Distinguish EOF, no-space, corruption, unsupported operation and I/O failure; after a commit-phase I/O error, require closing/reopening the volume so journal recovery completes before more writes.
9. **P2.16 Persistent end-to-end tests — Not implemented.** Create nested trees, write known bytes, close/reopen, read, rename, delete, verify ownership, and inject failures after every durable step.

**Exit gate:** image-backed create/read/write/truncate/rename/delete and directory operations survive reopen and injected crashes; checker reports no leaks, aliases or dangling references. No OS mount should precede this gate.

## P3 — Safe device discovery, OS integration and release
**Status: Bare groundwork only.** This subsection is part of P2's combined 25% weight.

The raw-file device backend is real, but it does not establish that a path is a USB device or safely selected. Opening is non-destructive; later writes can overwrite partition tables and user data. There is no end-user raw-device formatter or safe discovery flow.

Tasks:
1. **P3.1 Read-only discovery — Not implemented.** Per-OS identity, serial/device path, removable status, exact capacity, sector geometry, partition table and mounted/system-volume status.
2. **P3.2 Exclusive ownership — Not implemented.** Acquire OS-supported exclusive access; reject mounted, busy, boot/system, ambiguous, unsupported or changed devices.
3. **P3.3 Dry-run plan — Not implemented.** Show exact stable identity, capacity, partitions and every destructive action before any write.
4. **P3.4 Explicit confirmation — Not implemented.** Revalidate identity/geometry immediately before format; no default target and no path-name heuristics.
5. **P3.5 Mocked refusal tests — Not implemented.** Prove no writes occur for every unsafe/ambiguous condition.
6. **P3.6 Hardware validation — Blocked.** Only after P1–P2 gates: dedicated expendable USB, disconnect/reconnect, format/open/reopen, read/write, flush, removal during I/O and checker pass. Never test on valuable or system disks.

**Exit gate:** all unsafe target cases are refused, target identity is revalidated, and the dedicated hardware test record is reproducible. Image-only tests do not satisfy this gate.

### P3 continued — OS integration and normal file APIs
**Status: Not implemented.** This subsection is part of P2's combined 25% weight.

No Rust mount/VFS or OS filesystem adapter was found in the current workspace. Legacy adapters are references only.

Tasks:
1. **P3.7 Choose Windows route — Blocked by P2** Document WinFsp or other chosen integration, privilege/signing/installer requirements, error mapping and failure modes.
2. **P3.8 Adapter contract — Not implemented.** Map file operations, handles, flush, unmount and device removal to stable core APIs without OS assumptions in the format crate.
3. **P3.9 Safe lifecycle — Not implemented.** Prevent unmount races, reject operations after shutdown starts, drain in-flight operations and report dirty/failed state.
4. **P3.10 Windows integration — Not implemented.** Integration tests through normal OS APIs, not direct library calls alone.
5. **P3.11 Linux adapter — Not implemented.** Separate mount/userspace adapter, permissions/error semantics and unmount tests.
6. **P3.12 ArchiaOS VFS/block integration — Not implemented.** Port only after the portable contract stabilizes.
7. **P3.13 ARM64 — Deferred.** Build/test where target and runner support are available; do not claim support from architecture-neutral source alone.

**Exit gate:** users can create/edit/read/rename/delete files via normal OS APIs, unmount cleanly, remount and verify bytes on each claimed platform.

### P3 continued — Release, compatibility and long-term reliability
**Status: Planning only.** This subsection is part of P2's combined 25% weight.

Tasks:
1. **P3.14 Format version policy — Partially documented.** Define feature negotiation, incompatibility rejection, golden fixtures, migration rules and no implicit conversion. Rust draft is not compatible with legacy C v1.x.
2. **P3.15 Upgrade/conversion — Not implemented / optional.** If conversion is added, use a separate read-only source and new target image, validate both, and never overwrite the only copy.
3. **P3.16 Stress and resource limits — Not implemented.** Large-volume, fragmentation, many-file, near-full, repeated mount/unmount and sustained I/O tests.
4. **P3.17 Fuzz/sanitizer matrix — Not implemented for Rust.** Add fuzzing, Miri where appropriate, sanitizer-equivalent checks and minimized regression cases.
5. **P3.18 Benchmarks — Not implemented as a release gate.** Establish correctness-preserving baselines for sequential/random I/O, metadata operations, fragmented reads and checker throughput; avoid premature tuning.
6. **P3.19 Security review — Not implemented.** Hostile image inputs, resource exhaustion, symlink/path traversal once supported, concurrent external modification and raw-device privilege boundaries.
7. **P3.20 Release operations — Not implemented.** Reproducible builds, signed artifacts where appropriate, support matrix, known limitations, recovery guide and rollback/recovery instructions.

**Exit gate:** all earlier exit gates pass, no known data-loss blocker remains, compatibility and recovery behavior are documented, and the supported OS/device matrix is validated.

## Cross-cutting reliability requirements

These requirements apply to every phase and must not be deferred to release:
- Check all disk-derived lengths, offsets, counts and arithmetic for overflow and bounds before I/O or allocation.
- Reject malformed, truncated, unsupported-version, checksum-invalid and ambiguous state without guessing or silently repairing.
- Preserve the distinction between absent/uncommitted and committed/recoverable operations.
- Test no-space and every I/O error path, including failed rollback and failed flush; if the state is uncertain, stop further writes until recovery.
- Check ownership across bitmap, metadata records, directory entries, extent-map roots and data extents; no alias, dangling pointer or silent leak.
- Every mutation needs a defined commit point and persisted-byte crash tests before it is considered durable.
- Keep checker read-only and repair explicit, previewable and independently validated.
- Do not claim multi-writer concurrency until locking, serialization and race tests exist.
- Define data-checksum coverage or explicitly document that regular file contents are not checksummed by the current Rust format.
- Distinguish process termination tests from OS crash, power-loss and physical-device tests.
- Never claim NTFS compatibility or legacy OpenFS image compatibility without an implemented and tested conversion/driver path.

## Immediate execution order

1. **P1 — Finish format/ownership blockers:** preserve descriptor publication invariants, define safe descriptor replacement, close allocator ownership gaps and keep exact-image preservation regressions.
2. **P1 — Prove transaction recovery:** expand write/flush/checkpoint/replay fault cuts, including repeated recovery and the supported block sizes; record exact-SHA Linux/Windows CI evidence.
3. **P2 — Persistent file vertical slice:** define the canonical metadata root/index, then implement create → write → close → reopen → read with ownership verification and crash-cut tests.
4. **P2 — Whole-volume verification:** traverse objects from the persistent root, add deterministic diagnostics and bounded malformed-input/fuzz coverage; design repair only after the checker is trustworthy.
5. **P3 — Safe device workflow:** implement read-only device identity/discovery, refusal of mounted/system/ambiguous targets, dry-run and explicit confirmation before destructive formatting.
6. **P3 — OS integration and release gates:** choose the Windows adapter only after the durable core is stable, then add OS API tests, lifecycle safety, stress/fuzz/benchmark gates and support/recovery documentation.

Do not bypass phase dependencies to chase a later feature. Physical-device testing must use dedicated expendable media and never a valuable or system disk.

---

## Required format for future progress reports

Every development update must report:
- Current branch HEAD and exact code/documentation commit SHA.
- Phase percentages and the weighted overall estimate using the formula above.
- Implementation status versus integration status.
- Tests added versus tests executed versus tests passed.
- Exact GitHub Actions run URL and tested SHA for any CI claim.
- Remaining blockers, including any failed or pending CI job.
- Whether the evidence is image-backed, host-device-backed, or physical-hardware-backed.

Never mark a phase complete while its exit gate is false. Never report the latest successful CI for a different SHA as validation of a new change. Do not modify the main branch or OpenFS-old as part of this roadmap.


## Consolidated four-phase progress ledger (source of truth for percentages)

The former eight-phase estimates are consolidated into four phases. The combined phase scores are weighted by their previous global phase weights; the task details remain in the phase descriptions above and in the evidence register below.

| Phase | Global weight | Calculation from former task groups | Earned phase score |
| --- | ---: | --- | ---: |
| P0 — Engineering baseline | 10% | Existing P0 ledger: 100/100 | 100% |
| P1 — Format/layout/allocator + transactions/recovery | 35% | (15 × 93 + 20 × 40) / 35 | 63% |
| P2 — Verifier/repair + durable namespace/file I/O | 30% | (10 × 35 + 20 × 21) / 30 | 26% |
| P3 — Safe device discovery + OS integration + release | 25% | (10 × 5 + 10 × 0 + 5 × 3) / 25 | 3% |
| **Global** | **100%** | **(10×100 + 15×93 + 20×40 + 10×35 + 20×21 + 10×5 + 10×0 + 5×3) / 100 = 40.15%** | **40%** |

The combined scores are rounded only for display. The global result retains the prior weighted estimate. Some partial task awards remain expert estimates rather than a complete criterion-by-criterion calculation, so 40% remains provisional. A documentation-only change does not change implementation scores. The former phase IDs have been consolidated as follows: former P0→P0; former P1/P2→P1; former P3/P4→P2; former P5/P6/P7→P3. Contract document filenames such as `P2_JOURNAL_TRANSACTION_CONTRACT.md` are historical paths and are intentionally unchanged.

---

## Evidence register for implemented work

These references are intended to make important claims quickly auditable without repeating every link in every task. Paths are relative to the repository root.

| Claim | Source/API and representative tests | Change/CI evidence | Limits |
| --- | --- | --- | --- |
| Format and geometry | `OpenFS/crates/openfs-format/src/lib.rs` (`Superblock::encode/decode`); `volume.rs` (`VolumeLayout::calculate`, `format_volume`, `open_volume`, `check_volume`) | Rust CI run [38379103899](https://github.com/Likounsee/OpenFS/actions/runs/38379103899), exact tested SHA `44efa42408258247fdcb99890799f19b8e9c482f` | CI proves the configured automated suite only; not physical-device durability. |
| Allocation, descriptor discovery and failed-publication behavior | `volume.rs`: `allocate_block`, `free_block`, `publish_persistent_inventory`, `discover_persistent_inventory`; `allocator_extents.rs`: `clear_stale_descriptor_magic`, `allocate_blocks`, `allocate_contiguous_blocks`; regression `every_allocation_path_scrubs_stale_descriptor_staging` covers all five block sizes for batch and contiguous allocation. Existing tests include `descriptor_publication_is_atomic_across_every_journal_write_failure`, `descriptor_publication_flush_failures_recover_old_or_complete_state`, `torn_descriptor_staging_writes_remain_invisible_and_safe_to_reuse`, `discovery_fails_closed_on_corrupt_allocated_descriptor`, `discovery_fails_closed_for_corrupt_descriptors_at_every_block_size`; `inventory_descriptor.rs`: `descriptor_codec_round_trips_every_supported_block_size`, `descriptor_decoder_fails_closed_for_header_and_shape_corruption`, `rejects_overlapping_or_mis_sized_inventory_lists_at_every_block_size` | Descriptor publication/corruption regression batch [339945c](https://github.com/Likounsee/OpenFS/commit/339945c907bc0ae5f17aa502436e88934d7cf842), followed by ownership-scan optimization and multi-root regression [c7b93f1](https://github.com/Likounsee/OpenFS/commit/c7b93f1ee77869529d869ce4d3ae15ef6dae52aa) and allocator stale-descriptor fix [9fd6d85](https://github.com/Likounsee/OpenFS/commit/9fd6d857235723f9474109d73666be89994962f6); exact-SHA CI run [38439682402](https://github.com/Likounsee/OpenFS/actions/runs/38439682402) passed Linux and Windows (formatting, workspace tests, Clippy) on `9fd6d857235723f9474109d73666be89994962f6`. The new regression test passed on Linux and Windows; extent allocation validation retains one bitmap page while scanning sorted ranges and root alias checks use binary search. Earlier descriptor-inventory validation retains its separate eight-page bounded cache. | Covers software-level descriptor corruption and selected write/flush cuts across supported block sizes; does not prove physical-device power-loss behavior, provide automatic orphan repair, safe descriptor replacement, or remove the full-data-region discovery scan. |
| Journal/checkpoint recovery | `OpenFS/crates/openfs-format/src/journal_engine.rs`: `PersistentJournal::commit`, `open_journal_protected`, `validate_checkpoint_geometry`; tests `replays_durable_commit_if_home_write_fails`, `ignores_uncommitted_transaction_after_commit_write_failure`, `torn_terminal_commit_record_is_ignored_and_home_data_is_preserved`, `torn_home_write_is_repaired_from_durable_redo_record`, `torn_checkpoint_write_recovers_from_durable_redo_for_multiple_prefixes`, `recovery_io_failure_can_be_retried_without_losing_committed_redo`, `every_commit_io_failure_recovers_to_a_consistent_home_block`, `open_rejects_persisted_checkpoint_sequence_gap`, `protected_recovery_validates_all_targets_before_replaying_any_update`, `malformed_nonterminal_record_before_later_valid_records_fails_closed`, `counter_overflow_is_rejected_before_any_persistent_write`, and `recovery_counter_overflow_is_rejected_before_replaying_home_blocks` | Historical checkpoint validation/torn-checkpoint/reuse commits remain listed above; exact-SHA CI run [38456414882](https://github.com/Likounsee/OpenFS/actions/runs/38456414882) validates journal commit-flush recovery on Linux and Windows; run [38457627241](https://github.com/Likounsee/OpenFS/actions/runs/38457627241) validates all-target prevalidation before replay; run [38458063369](https://github.com/Likounsee/OpenFS/actions/runs/38458063369) validates malformed interior record rejection; run [38459392863](https://github.com/Likounsee/OpenFS/actions/runs/38459392863) validates counter-overflow preflight on Linux and Windows. | Selected injected I/O cuts and image-backed recovery tests only; not a general namespace transaction protocol and not physical power-loss evidence. |
| In-memory namespace and path invariants | `OpenFS/crates/openfs-format/src/namespace.rs`: `Namespace::from_records`, `resolve_path`, `insert`, `rename`, `remove`, `validate`; tests for duplicate names, rename/move, removal, generation overflow, traversal and index consistency | Code CI run 38379103899 passed the full workspace at exact code SHA 44efa42408258247fdcb99890799f19b8e9c482f; latest roadmap-only CI run 38423229047 passed at SHA 4b0ee02b974ceecc10379378b72d20e168d4d6bf | In-memory correctness does not prove durable namespace mutations. |
| Read paths and read-only checker | `volume.rs`: `read_file_range`, `read_file_contents`, `read_path_range`, `check_volume`; CLI `OpenFS/crates/openfs-tool/src/main.rs`: `check_image`; test `checks_current_image_without_modifying_it` | Code CI run 38379103899 passed on exact SHA 44efa42408258247fdcb99890799f19b8e9c482f; latest roadmap-only CI run 38423229047 passed on exact SHA 4b0ee02b974ceecc10379378b72d20e168d4d6bf | Does not provide whole-volume repair, durable write/truncate or mount integration. |
| Cross-structure ownership verifier | `OpenFS/crates/openfs-format/src/verifier.rs`: `verify_discovered_persistent_inventory` composes descriptor, inventory, extent-root, file-data and bitmap ownership; regression `discovered_verification_rejects_file_data_aliasing_descriptor_at_every_block_size` creates a real on-image alias and requires fail-closed `Error::Corrupt` at 4096, 8192, 16384, 32768 and 65536-byte blocks. | Test commit [51abae3](https://github.com/Likounsee/OpenFS/commit/51abae381e8dbb108dfb84e9eb3c5a29cf90bb26); exact-SHA CI run [38450124058](https://github.com/Likounsee/OpenFS/actions/runs/38450124058) passed formatting, workspace tests and Clippy on Linux and Windows. | Software image regression only; no physical-device power-loss or end-to-end durable namespace mutation evidence. |\n| Cross-platform CI contract | `.github/workflows/ci.yml`: formatting, `cargo test --workspace`, Clippy with warnings denied on Linux and Windows | Latest code SHA `c7b93f1ee77869529d869ce4d3ae15ef6dae52aa`: [run 38437650554](https://github.com/Likounsee/OpenFS/actions/runs/38437650554), both jobs success. | CI validates only the configured automated checks. No fuzzing, sanitizer, benchmark, mounted OS integration or physical hardware job. |

Evidence vocabulary is mandatory: **Implemented** means code/API exists; **Integrated** means connected to the intended end-to-end path; **Test added** means a matching test exists in source; **Test executed** means a recorded runner actually ran it; **CI validated** means the relevant job succeeded on the exact cited SHA; **Non vérifié** means evidence does not cover the claim; **Bloqué** means a safety prerequisite or unresolved defect prevents progression. For historical commits whose exact run cannot be retrieved, treat the run reference above as historical recorded evidence and do not extrapolate its result to later SHAs.

## Dependency matrix (P1–P3)

| Blocked phase/task | Required dependency | Technical reason | Objective unblock condition and proof | Phases unblocked |
| --- | --- | --- | --- | --- |
| P1.3–P1.4 ownership and descriptor lifecycle | P1.7 transaction state machine plus explicit descriptor/root ownership invariants | Allocation can commit before descriptor publication; failed publication can leave a detectable allocated orphan, and replacement cannot safely choose between old/new descriptors without a commit point. | Written state machine and on-disk ownership contract; injected failure at each write/flush boundary; reopen yields one unambiguous committed descriptor and no unexplained allocation. | P2 durable root and mutations; P2 full traversal. |
| P1.8–P1.11 transaction/recovery | P1.7 error/commit contract and P1 protected layout | A journal with a bounded ring and selected recovery tests cannot safely provide multi-object atomicity until capacity, replay and uncertain-I/O behavior are specified. | Crash-cut matrix for every journal/flush/checkpoint/replay boundary; repeat replay; compare persisted bytes and require old-or-new state only; prove writes are refused after indeterminate recovery. | P2 mutations; later P3 adapter. |
| P2.8–P2.9 canonical root and reconstruction | P1 descriptor lifecycle plus P1 commit protocol | Reads currently depend on inventory/descriptor structures and cannot establish a durable, scalable root locator under mutation. | Versioned root/index discoverable without caller-supplied lists; reopen reconstructs identical graph; malformed pointers fail closed; large/sparse image test. | P2 whole-volume verifier; P2 durable mutations; P3 API adapter planning. |
| P2.10–P2.13 durable file/namespace operations | P2.8 root, P1 atomic transaction semantics, P1 ownership/allocation contract | Create/write/truncate/rename/delete must update object records, directory entries, extent maps and allocation ownership atomically. | For each operation, injected failure at each persistent step; reopen is old or new complete state; checker finds no alias, dangling pointer or leak. | P2 full traversal; P3 OS integration. |
| P2.2–P2.5 full verification/repair | P2.8 root and defined ownership/reference model; checker remains read-only | Without a canonical root, a full traversal cannot find all objects. Repair before reliable diagnostics and ownership reconstruction risks making corruption worse. | Read-only scan covers every reachable object; deterministic diagnostics and byte-for-byte image preservation; repairs, if later added, require explicit preview/apply and post-check. | P3 safety confidence; P3 release gate. |
| P3.1–P3.5 safe discovery/format | P2 durable API for useful use; OS-specific identity/partition APIs and destructive safety policy | A raw path is not proof of a removable USB identity; writing can overwrite system/partition data. | Mocked tests refuse mounted/system/ambiguous/changed devices; revalidate stable identity and geometry immediately before confirmation and first write. | Physical test stage and P3 operational release, not core image development. |
| P3.7–P3.12 OS integration | P2 durable APIs and error semantics; P1 flush/uncertain-I/O contract; P2 diagnostics for failure handling | OS callbacks need stable semantics for file operations, flush, shutdown, errors and failed recovery. | Normal OS file API integration tests for create/read/write/rename/delete, clean unmount/remount and byte verification on the claimed OS. | P3 release qualification for that OS. |
| P3.14–P3.20 compatibility and release | P1 format/version contract, P1 recovery evidence, P2 checker, P2 functional API, P3/P3 claimed platform validation | A release cannot promise compatibility, data safety or platform support from source presence or image-only unit tests. | Versioned golden images; compatibility/rejection tests; stress/fuzz results; recovery guide; supported-platform matrix; no unresolved data-loss blocker. | Public/production release. |

The dependencies are gates, not a requirement to stop all parallel work: tests, fuzz harness design, diagnostics design, OS-adapter research and mocked discovery can proceed in parallel as long as they do not expose unsafe mutation or claim unsupported behavior.

## First usable filesystem milestones (P0–P3)

These milestones are acceptance gates, not assertions that the milestone has already been reached.

| Milestone | Status at this audit | Acceptance criteria | Required evidence |
| --- | --- | --- | --- |
| **A — Format and recovery reliability** | **Partially achieved; not complete** | Format/reopen images; allocate/free/reopen without leaks; reject corrupt layouts; recover deterministically at every covered interruption. | Image-backed regression suite, exact-SHA Linux/Windows CI, exhaustive persisted-byte crash-cut matrix. |
| **B — First persistent file** | **Not achieved; blocked** | Create file, write known bytes, close/reopen, read identical bytes; ownership checker finds no leaks/aliases. | End-to-end image test with injected failure at each commit boundary and reopen verification. |
| **C — Complete namespace operations** | **Not achieved; blocked** | mkdir, empty/fragmented files, rename, truncate and delete; no-space and interruption behavior is coherent. | Persistent operation tests, crash matrix, checker pass after every scenario. |
| **D — Verification and reliability** | **Partially achieved; incomplete** | Traverse all persistent objects, stable diagnostics, malformed-image corpus, bounded fuzzing; repairs explicit and never silent. | Read-only image preservation, deterministic reports, fuzz corpus/regression fixtures, separate repair dry-run/apply tests if repair is implemented. |
| **E — OS integration and normal use** | **Not achieved; blocked** | Stable persistent API and first OS adapter; ordinary OS file APIs; clean unmount/remount with bytes preserved. | Integration test against real OS filesystem APIs on every claimed platform. |
| **F — Physical device and publication** | **Not achieved; blocked** | Safe removable-device identity, refuse system/mounted/ambiguous targets, explicit confirmation, dedicated disposable USB test, support/compatibility matrix. | Mocked refusal tests plus recorded hardware test on dedicated media; never infer physical durability from image tests. |

## Final consistency checks for roadmap maintenance

Before changing any percentage or status in a future update:
1. Confirm branch and exact HEAD; inspect the task's current source, tests and workflow.
2. Update the earned points only for acceptance criteria whose evidence changed; keep the task weight fixed unless the scope changes and the reason is documented.
3. Recompute every phase as earned points / 100 and the global value using phase weights; never edit the displayed total by intuition.
4. Verify every cited CI run's `head_sha` matches the claim. Label code-validation runs separately from documentation-only runs; the latter do not create implementation evidence.
5. Verify dependency gates and milestone status against the actual implementation, not just task completion percentages.
6. Review the diff and ensure this roadmap-only mission did not change filesystem source, tests, scripts or workflow code.


## Scoring ledger: limitation and next scoring pass

The task-level ledger is the current source for the published **provisional** percentages, but it is not yet a fully mechanical score: non-zero partial task awards remain expert estimates based on the evidence summaries in their rows. Do not call the 40% value fully reproducible. Do not invent criterion weights retroactively merely to preserve it.

Before claiming a mechanically reproducible score, replace each non-zero partial task row with named acceptance criteria and fixed subweights. For every criterion record: stable ID; weight within the task; expected behavior; source/API path; regression test name; whether the test was added, executed and passed; exact CI run and tested SHA where applicable; earned credit; and a one-line justification. Subweights must sum exactly to the parent task weight, and earned criterion points must sum to the parent earned points. Each acceptance criterion may be credited once only. Keep absent or unverified behavior at zero; recalculate phase and global percentages from the criterion ledger and disclose any change from 40%.

Until that work is done, retain the existing values only as provisional estimates and use the exit gates—not percentages alone—to decide whether a phase can advance. This revision fixes status/evidence inconsistencies but does not claim that the score has become mechanically reproducible.


### Mise à jour du 2026-10-21 — durcissement des mutations persistantes

- `4f18d6a`, `b882f08` et `a71b20b` renforcent la préparation des listes de blocs, la construction groupée des extents et les calculs de capacité avant remplacement, suppression et troncature.
- La CI exacte-SHA de `a71b20b` a terminé avec succès sous Linux et Windows (formatage, tests du workspace et Clippy) : [run 39610130554](https://github.com/Likounsee/OpenFS/actions/runs/39610130554).
- Le lot suivant retire les appels redondants à `try_reserve(1)` dans la collecte des blocs d'une suppression groupée : la capacité exacte est déjà réservée avant la boucle. Cela évite un contrôle d'allocation par bloc sans changer l'ordre de validation ni le moment de libération.
- Les limites principales restent la propriété exclusive du volume entre plusieurs ouvreurs/processus, l'atomicité générale des mutations multi-étapes et l'intégration OS. Le journal protège des mises à jour de bitmap, pas chaque opération fichier comme transaction globale.


### Mise à jour du 2026-10-22 — crash-cut du COMMIT terminal

- `torn_terminal_commit_record_is_ignored_and_home_data_is_preserved` couvre les préfixes de 1, 20 et 32 octets : le COMMIT reste incomplet, la réouverture doit garder l'ancien checkpoint et le bloc home ancien.
- `reported_torn_terminal_write_can_still_commit_when_record_is_complete` couvre le préfixe de 63 octets : l'octet non réécrit peut déjà correspondre au checksum attendu. Même si l'appel d'écriture renvoie une erreur, un COMMIT complet et durable doit être récupéré comme validé; le résultat de l'appel est donc incertain, pas nécessairement annulé.
- Le run `39690006441` du commit `63aa735` a échoué sur Linux et Windows car le test initial supposait à tort que toute erreur d'écriture signifiait que le COMMIT était invalide. Le correctif sépare désormais les cas incomplets du cas où les octets persistants forment un enregistrement valide.
- Le commit `72cc59b8144736dfa773d0b1ed938811f4593677` reste confirmé vert sur Linux et Windows pour rustfmt, les tests workspace et Clippy : [run 39688617770](https://github.com/Likounsee/OpenFS/actions/runs/39688617770). Les commits correctifs `450738c` et `525789b` attendent leur validation CI exacte-SHA.
- Ces tests simulent des écritures déchirées à la frontière du backend logiciel; ils ne prouvent pas la durabilité sur matériel réel ni le comportement de contrôleurs qui réordonnent ou mettent en cache les écritures.


### Mise à jour du 2026-10-22 — CI verte et couverture check-image multi-tailles

- La CI exacte-SHA du commit `d3a706fdd460e8ae6faad15ea66e553eedc07aca` est verte sous Linux et Windows : formatage, tests du workspace et Clippy réussis ([run 39694672671](https://github.com/Likounsee/OpenFS/actions/runs/39694672671)). Cela valide notamment `allocation_bitmap_diagnostics_are_read_only_across_supported_block_sizes`.
- Le test CLI `checks_current_image_without_modifying_it` est étendu aux cinq tailles de blocs prises en charge. Il compare l'image octet par octet avant/après `check-image` et vérifie le rejet d'une taille non prise en charge sans créer de fichier.
- Le nouveau test doit être validé par une CI exacte-SHA avant que cette couverture supplémentaire soit comptabilisée comme validée.


### Mise à jour du 2026-10-22 — vérificateur d'inventaire strictement en lecture seule

- Ajout de `persistent_inventory_verification_performs_no_writes_or_flushes` : le test prépare une image valide puis enveloppe le périphérique dans un compteur d'écritures et de flushes.
- Le test vérifie les cinq tailles de blocs prises en charge (4, 8, 16, 32 et 64 KiB), exige un rapport cohérent et zéro écriture/zéro flush pendant la vérification.
- La fixture est créée avant l'activation des compteurs afin de mesurer uniquement l'API du vérificateur. Cela démontre le comportement logiciel du chemin testé, pas la durabilité matérielle ni l'exhaustivité de tous les chemins de corruption.
- La validation CI exacte-SHA de ce nouveau test reste à obtenir après publication du commit.


### Mise à jour du 2026-10-22 — validation exacte-SHA et couverture des deux API du vérificateur

- CI exacte-SHA du commit `2b65a61e535c19a56edeb6f760508f606a4a7819` terminée avec succès sur Linux **et Windows** : formatage, tests du workspace et Clippy ([run 39704510031](https://github.com/Likounsee/OpenFS/actions/runs/39704510031)).
- Le test de vérification en lecture seule est renforcé pour couvrir les deux points d'entrée : inventaire fourni par l'appelant et inventaire découvert via son descripteur persistant. La fixture est publiée avant le comptage; les deux vérifications doivent ensuite produire des rapports cohérents sans écriture ni flush, sur les cinq tailles de blocs.
- La CI exacte-SHA du commit `2c9d77fc56e020e955f8721b0e0e97bfcb227fcc` a échoué sur les deux OS uniquement à cause du formatage de l'assertion `caller_report`; les tests et Clippy ont été ignorés après cet échec. Le formatage est corrigé ici, et le test de descripteur invalide est renforcé pour compter les écritures/flushes sur cinq tailles de blocs. La CI de ce nouveau commit reste à obtenir. Cette preuve logicielle ne constitue pas une garantie de durabilité matérielle ni une validation exhaustive de toutes les corruptions.
- Limite d'inventaire : `INVENTAIRE_SYMBOLES.csv` et `INVENTAIRE_FICHIERS.csv` sont des index historiques dont le HEAD source et/ou les tailles doivent être recalculés; ne pas les présenter comme des relevés synchronisés avec le HEAD courant. `INVENTAIRE_EXHAUSTIF.csv` (1,6 Mo) n'a pas été récupéré intégralement par le connecteur, donc aucune fusion n'est revendiquée.


### Mise à jour — audit en lecture seule des descripteurs invalides

- Le test `discovered_verification_rejects_semantically_invalid_descriptor_without_writes` fabrique un descripteur dont le CRC est valide mais dont l'identifiant racine est nul, puis vérifie le refus sans écriture ni flush.
- Le scénario est exécuté sur les cinq tailles de blocs supportées. Les compteurs ne sont activés qu'après la création de l'image et l'écriture du descripteur invalide.
- Le formatage ayant fait échouer la CI du commit précédent est corrigé; aucune réussite de CI n'est revendiquée pour ce nouveau commit avant la fin du run exact-SHA.


### Mise à jour — réconciliation de l'inventaire de fichiers

- `INVENTAIRE_FICHIERS.csv` est régénéré à partir des métadonnées Git de l'arbre non tronqué du snapshot `c8b8cacf02ff` (46 fichiers). Les tailles et SHA de blob sont des métadonnées Git; elles ne prétendent pas démontrer que chaque ligne de chaque source a été auditée.
- Les commentaires de module dans `metadata.rs` et `directory_entry.rs` ont été corrigés pour ne plus laisser entendre que les opérations d'espace de noms persistantes n'existent pas; les codecs restent distincts de l'orchestration de volume et de la récupération transactionnelle.


### Clarification du contrat du vérificateur

- `verify_persistent_inventory` ne recherche pas le descripteur : il ne connaît que les propriétaires fournis par l'appelant. Si un descripteur persistant est présent mais non déclaré dans ses entrées, son bloc doit être rapporté comme orphelin par ce rapport.
- `verify_discovered_persistent_inventory` découvre le descripteur et l'inclut explicitement dans le graphe des propriétaires; avec la même image, ce rapport doit être cohérent.
- Le test multi-tailles vérifie désormais cette différence attendue ainsi que l'absence d'écritures/flushes pour les deux chemins.


### Mise à jour — inventaire de symboles reproductible

- Ajout de `OpenFS/scripts/generate_symbol_inventory.py`, un générateur Python standard-library-only pour reconstruire l'index des déclarations Rust.
- `INVENTAIRE_SYMBOLES.csv` a été recalculé sur le snapshot source `31adce9a3974` : 1 003 déclarations repérées dans les 16 fichiers Rust.
- Le générateur est lexical et volontairement conservateur : `Indexed` confirme uniquement la présence d'une déclaration à une ligne donnée, pas son comportement, sa sûreté ou sa couverture de tests.
- Le gros `INVENTAIRE_EXHAUSTIF.csv` reste inchangé tant que son contenu complet n'est pas récupérable pour comparaison.
- Les estimations P0/P1/P2/P3 restent inchangées; une mise à jour de documentation ne suffit pas à augmenter l'avancement fonctionnel.


### CI — inventaire de symboles

La CI Linux et Windows exécute désormais `scripts/generate_symbol_inventory.py`, puis `git diff --exit-code -- docs/INVENTAIRE_SYMBOLES.csv`. Cela vérifie que l'index lexical est reproductible sur le checkout et empêche une modification de source Rust de laisser silencieusement l'index périmé. Ce contrôle ne remplace ni l'analyse sémantique ni les tests Rust.

### Mise à jour du 2026-10-22 — CI exacte-SHA de l'inventaire lexical

- Le run exact-SHA [39802397765](https://github.com/Likounsee/OpenFS/actions/runs/39802397765), sur le commit `b7cf17f8eea15b1ed8c1997d19d8cf6a575f3d45`, est **vert sous Linux et Windows** : génération reproductible de l'index, rustfmt, tests du workspace avec toutes les fonctionnalités et Clippy sur toutes les cibles.
- Cause du blocage précédent : l'index encodait un SHA obtenu via `git log`. Le checkout CI est superficiel, donc son historique disponible ne permet pas de reproduire la même révision source que le CSV suivi. Le générateur n'utilise plus l'historique Git pour construire les notes; les 1 003 déclarations des 16 fichiers Rust restent indexées.
- Cette correction valide la reproductibilité de l'index et la CI du commit cité. Elle ne constitue pas une validation sémantique de chaque symbole ni une preuve de préparation à la production.
- Les estimations P0/P1/P2/P3 et l'avancement global restent inchangés.


### Mise à jour du 2026-10-22 — audit non réparateur avec checkpoint dégradé

- Le test audit_image_accepts_valid_persistent_inventory_at_every_block_size vérifie maintenant, pour les blocs de 4, 8, 16, 32 et 64 Kio, qu'un inventaire valide est accepté sans modifier l'image.
- Le même scénario corrompt ensuite un seul checkpoint alors que son pair reste valide, puis exige que audit-image réussisse et conserve octet pour octet l'image endommagée. L'audit ne doit pas se transformer en réparation implicite.
- L'inventaire fonctionnel OFS-F-026 est synchronisé avec cette couverture. La vérification Linux/Windows du nouveau commit reste requise; aucune hausse de pourcentage n'est accordée avant validation.
- Les estimations restent P0 100 %, P1 79 %, P2 69 %, P3 5 %, global 60 %.


### Mise à jour du 2026-10-22 — correction Clippy du workflow CLI persistant

- Le run exact-SHA [39853416440](https://github.com/Likounsee/OpenFS/actions/runs/39853416440) a confirmé que les tests Rust passent sous Linux (324 tests de `openfs-format` et 13 tests CLI), mais Clippy Linux bloque sur un `mut` inutile dans le test de réouverture du fichier persistant.
- Le qualificatif mutable a été retiré : le volume rouvert sert uniquement à lire et vérifier le contenu après réouverture. Aucun comportement de production n'est modifié.
- Le job Windows était encore en cours au moment de l'inspection; il ne doit pas être présenté comme vert avant résultat final. Le prochain run doit valider rustfmt, tests et Clippy sur les deux plateformes.
- Estimations inchangées tant que la CI complète n'est pas verte : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global 60 %.


### Mise à jour du 2026-10-22 — vertical slice CLI élargi aux mutations namespace

- Le scénario de test CLI persistant traverse maintenant, sur les cinq tailles de blocs supportées, la création des répertoires/fichiers, l'écriture, l'append, la troncature, le déplacement du fichier vers un sous-répertoire, la suppression d'un second fichier, puis la fermeture/réouverture.
- Après réouverture, le contenu du fichier déplacé est relu et les anciens chemins du fichier déplacé et du fichier supprimé doivent rester absents. `check-image` et `audit-image` vérifient ensuite l'image; l'audit doit toujours laisser chaque octet inchangé.
- Ce test renforce la couverture d'intégration du namespace persistant, mais ne remplace pas les tests d'injection de panne entre les phases de publication/récupération.
- Estimations conservées : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global 60 %; l'avancement ne sera réévalué qu'après validation CI exacte-SHA.


### Correction de formatage — test de déplacement/suppression

- La CI du commit `2707115b621aa855cb729bfd57062be7401218d0` a échoué sur `cargo fmt --all -- --check`, sous Linux et Windows, à cause d'une seule expression de lecture de chemin absente du formatage rustfmt canonique.
- Le test a été ajusté à la sortie attendue par rustfmt. Pas de changement de comportement fonctionnel.
- La suite doit repartir sur le nouveau SHA avant de conclure que le scénario de déplacement/suppression est validé.

### Reprise — récupération après erreur de publication (2026-10-23)

- Le CI du commit précédent a révélé un cas concret : une erreur pendant l'écriture du journal pouvait laisser l'instance de journal empoisonnée. Le code tentait alors de redécouvrir l'inventaire et de libérer les blocs temporaires avant d'avoir rejoué une éventuelle transaction COMMIT durable.
- Correction : après l'échec de publication, réouvrir/récupérer le journal sur le même périphérique avant de redécouvrir le descripteur. Si le nouveau descripteur a été validé, le chemin retourne succès; si l'ancien est toujours actif, les blocs temporaires peuvent être récupérés après la décision du journal. Si la récupération échoue, l'opération s'arrête en erreur au lieu de poursuivre des écritures.
- Validation en attente sur CI Linux et Windows; aucune hausse des estimations avant les deux résultats verts.

### Allocation: issue I/O indéterminée pendant la réservation (2026-10-23)

- Le test de récupération du renommage a isolé un cas plus précoce que la publication de l'inventaire : l'allocation d'un bloc pouvait renvoyer une erreur après un COMMIT durable, alors que le bitmap ne serait mis à jour qu'au rejeu. Le bloc restait alors alloué mais non référencé.
- Correction : si le COMMIT d'allocation retourne une erreur, réouvrir/récupérer le journal avant de relire le bitmap. Si le bit est alloué après récupération, retourner le bloc comme allocation réussie; sinon conserver l'erreur d'origine. Cela empêche le staging d'inventaire de perdre la propriété d'un bloc dont l'allocation a été validée.
- CI exact-SHA requis; ne pas considérer la correction comme validée avant tests Linux et Windows.


### Matrice de récupération de suppression persistante — 2026-10-23

- Le test `persistent_path_remove_recovers_old_or_removed_namespace_after_write_and_flush_failures` couvre les erreurs injectées d'écriture et de flush aux positions 0 à 8, sur les tailles de blocs 4, 8, 16, 32 et 64 Kio.
- Après chaque panne, l'image est rouverte; le fichier doit soit rester lisible avec son contenu intact, soit être absent, et l'inventaire découvert doit conserver une propriété cohérente.
- Le test augmente la couverture des frontières d'E/S simulées, sans prouver à lui seul la durabilité sur matériel réel. CI exacte-SHA Linux et Windows requise avant validation; estimations conservées : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global 60 %.


### Matrice de récupération de suppression récursive — 2026-10-23

- Ajout du test `persistent_tree_remove_recovers_complete_subtree_or_keeps_it_after_write_and_flush_failures` : suppression d'un dossier avec fichier direct, sous-dossier et fichier imbriqué, sous injections d'écriture/flush.
- La matrice couvre trois tailles de blocs représentatives (4, 16 et 64 Kio) et sept positions d'injection par type d'opération. Après réouverture, le sous-arbre doit être intégralement présent avec ses contenus ou intégralement absent; un fichier extérieur doit toujours rester intact et l'inventaire doit être cohérent.
- Cette couverture par image fichier et fautes simulées ne remplace pas les tests sur matériel réel. Validation CI exacte-SHA Linux et Windows en attente; estimations inchangées : P0 100 %, P1 79 %, P2 69 %, P3 5 %, global 60 %.
