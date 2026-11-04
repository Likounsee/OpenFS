# OpenFS

**Système de fichiers indépendant en développement, principalement implémenté en Rust.**

> **Statut au 4 novembre 2026 : expérimental, non prêt pour la production.** Le moteur Rust et les outils image ont des opérations persistantes, un namespace, un journal et des chemins de récupération. Le montage Windows de développement utilise Dokany. Le pilote WDM natif en C compile en CI et valide plusieurs structures, mais **ne monte pas encore un volume utilisable**. Le montage Linux n'est pas implémenté. Les essais physiques USB sont en pause : ne formatez aucun disque pour tester cette branche.

[CI de la branche OpenFS](https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml)

## État CI exact

HEAD examiné : `4127059d7a4b48d45ba32942dd986cb318609096`. [Run 41728407756](https://github.com/Likounsee/OpenFS/actions/runs/41728407756).

| Contrôle | Résultat |
|---|---|
| Formatage Rust Linux + Windows | Réussi |
| Tests workspace Linux + Windows | Réussi |
| Crash sweep release Linux | Réussi |
| Tests C natifs | Réussi |
| Compilation/lien WDK x64 | Réussi |
| Clippy strict Linux + Windows | **Échec** : `unused_mut` dans `crash_sweep.rs:551` |

La CI globale n'est donc pas verte. Corriger `let mut reopened` (retirer `mut`) puis relancer les contrôles.

## Fonctionnalités présentes

- Workspace Rust : `openfs-core`, `openfs-block`, `openfs-format`, `openfs-partition`, `openfs-tool`.
- Format versionné : superblocs, layout, bitmap, métadonnées, entrées de répertoire, extents et inventaire persistant.
- Namespace et opérations fichiers persistantes : création, lecture/écriture, import/export, troncature, renommage et suppression via API/outils.
- Journal/checkpoints, récupération des queues incomplètes et réclamation vérifiée des blocs orphelins à l'ouverture writable.
- Tests d'injection d'erreurs et balayage de coupures simulées sur les événements write/flush.
- Adaptateur Windows Dokany expérimental pour images ; chemin USB brut présent dans le code mais non validé sur matériel.
- Pilote WDM natif C : bootstrap, sonde de volume et validateurs, sans dispatch complet des fichiers.

## Construire et tester

Depuis `OpenFS/` :

```sh
cargo build --workspace
cargo fmt --all -- --check
cargo test --workspace --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

Le dernier Clippy échoue sur `crates/openfs-format/src/crash_sweep.rs:551`. Le crash sweep release est lancé sous Linux par CI.

## Créer une image de développement

```sh
cargo run -p openfs-tool -- create-image ./openfs.img 1024
cargo run -p openfs-tool -- inspect-image ./openfs.img
cargo run -p openfs-tool -- check-image ./openfs.img 4096
cargo run -p openfs-tool -- mkdir ./openfs.img 4096 /docs
cargo run -p openfs-tool -- touch ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- import ./openfs.img 4096 ./README.md /docs/hello.txt
cargo run -p openfs-tool -- cat ./openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- audit-image ./openfs.img 4096
```

`create-image` initialise le namespace racine. `check-image` est non mutatif ; `audit-image` et `recover-image` utilisent le chemin writable et peuvent rejouer le journal/réclamer des blocs. Sauvegarder les images avant récupération.

## Intégration système

- **Windows/Dokany :** montage expérimental, notamment pour les images ; pas production.
- **Windows WDM natif :** compile en CI mais ne monte pas encore de volume. Il manque snapshot namespace dans le VCB, root FCB/DCB, lookup, énumération et dispatchs read/write.
- **Linux :** aucun adaptateur de montage implémenté.
- **USB brut :** le formatage vise le disque physique entier et est destructif. Essais matériels en pause ; ne lancez pas les scripts USB.

## Limites

Les crash sweeps utilisent des périphériques simulés et ne prouvent pas que les contrôleurs/disques réels respectent flush/FUA. La réclamation d'orphelins est une récupération après interruption, pas une atomicité universelle de toutes les mutations. Fuzzing étendu, tests matériels et benchmarks restent à faire. OpenFS n'est pas compatible NTFS/ReFS et ne doit pas stocker de données importantes.

## Documentation

- [État courant](OpenFS/docs/CURRENT_STATUS.md)
- [Architecture](OpenFS/docs/ARCHITECTURE.md)
- [Format disque](OpenFS/docs/FORMAT_SPEC.md)
- [Récupération](OpenFS/docs/RECOVERY_DESIGN.md)
- [Contrat journal P2](OpenFS/docs/P2_JOURNAL_TRANSACTION_CONTRACT.md)
- [Sécurité](OpenFS/docs/SECURITY_MODEL.md)
- [Compatibilité](OpenFS/docs/COMPATIBILITY.md)
- [Développement](OpenFS/docs/DEVELOPMENT.md)
- [Roadmap](OpenFS/docs/ROADMAP.md)
- [Pilote WDM](OpenFS/windows-driver/README.md)

## Licence

Les crates déclarent actuellement `MIT OR Apache-2.0`. Vérifier les fichiers de licence et manifests avant redistribution.
