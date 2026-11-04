<p align="center">
  <img src="docs/assets/openfs-logo.png" alt="Logo OpenFS" width="520">
</p>

<p align="center">
  <strong>Un système de fichiers indépendant, développé en Rust et C.</strong>
  <br>
  Conçu pour posséder son propre format disque et viser Windows, Linux et, à terme, ArchiaOS.
</p>

<p align="center">
  <a href="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml"><img src="https://github.com/Likounsee/OpenFS/actions/workflows/ci.yml/badge.svg?branch=OpenFS" alt="OpenFS CI"></a>
  <img src="https://img.shields.io/badge/statut-exp%C3%A9rimental-yellow.svg" alt="Expérimental">
</p>

# OpenFS

OpenFS est un système de fichiers indépendant. Il définit sa propre organisation des données sur disque : métadonnées, dossiers, fichiers, blocs, allocations et journal. Il ne lit pas et n'écrit pas les volumes NTFS ou ReFS.

> **Statut : expérimental.** Le moteur Rust et les outils d'image permettent déjà de créer et manipuler des volumes de développement. Sous Windows, le montage d'image passe par Dokany. Le pilote WDM natif en C est en cours de développement et **ne monte pas encore de volume utilisable**. Le montage Linux n'est pas implémenté. N'utilise pas OpenFS pour des données importantes.

## Ce qui fonctionne déjà

- Un workspace Rust organisé en bibliothèques indépendantes.
- Un format disque versionné avec superblocs, métadonnées, entrées de répertoire, extents et bitmap d'allocation.
- Un namespace persistant et des opérations sur les fichiers : création, lecture, écriture, import/export, renommage, suppression et troncature.
- Un journal et des mécanismes de récupération pour les opérations prises en charge.
- Des outils pour créer, inspecter, vérifier et auditer des images.
- Une intégration Windows expérimentale avec Dokany.
- Des validateurs et codecs C pour préparer le pilote WDM natif.

## Ce qui n'est pas encore terminé

- Le pilote Windows natif ne monte pas encore de volume et ne traite pas les opérations de fichiers.
- Aucun adaptateur de montage Linux n'est disponible.
- Les garanties de récupération ne couvrent pas encore toutes les mutations et toutes les pannes possibles.
- Les tests sur matériel USB sont en pause.
- OpenFS n'est pas prêt pour la production et n'est pas compatible NTFS/ReFS.

## Compiler

Prérequis : Git et Rust stable.

Depuis la racine du dépôt :

```sh
cd OpenFS
cargo build --workspace
```

Pour vérifier le code :

```sh
cargo fmt --all -- --check
cargo test --workspace --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

Consulte les [exécutions CI](https://github.com/Likounsee/OpenFS/actions) pour connaître le résultat réel de la dernière branche.

## Essayer OpenFS sans disque physique

La méthode recommandée est de travailler sur une image :

```sh
cargo run -p openfs-tool -- create-image ./test-openfs.img 128
cargo run -p openfs-tool -- mkdir ./test-openfs.img 4096 /docs
cargo run -p openfs-tool -- touch ./test-openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- write ./test-openfs.img 4096 /docs/hello.txt "Bonjour OpenFS"
cargo run -p openfs-tool -- cat ./test-openfs.img 4096 /docs/hello.txt
cargo run -p openfs-tool -- tree ./test-openfs.img 4096 /
cargo run -p openfs-tool -- check-image ./test-openfs.img 4096
```

La création initialise le volume et son dossier racine. Les images sont idéales pour les essais : elles évitent de modifier un disque réel.

## Organisation du dépôt

```text
.
├── README.md
├── docs/                    Documentation et ressources du projet
│   └── assets/
│       └── openfs-logo.png
├── .github/workflows/       Intégration continue
└── OpenFS/                  Workspace Rust
    ├── crates/
    │   ├── openfs-core/
    │   ├── openfs-block/
    │   ├── openfs-format/
    │   ├── openfs-partition/
    │   └── openfs-tool/
    └── windows-driver/      Pilote Windows natif en C
```

## Documentation

- [Prise en main](docs/GETTING_STARTED.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Format disque](docs/DISK_FORMAT.md)
- [Pilote Windows](docs/WINDOWS_DRIVER.md)
- [Sécurité et limites](docs/SAFETY.md)
- [Feuille de route](docs/ROADMAP.md)
- [Index de la documentation](docs/README.md)

## Licence

Les crates déclarent actuellement MIT OR Apache-2.0. Consulte les fichiers de licence et les manifests avant toute redistribution.
