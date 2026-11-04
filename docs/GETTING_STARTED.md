# Prise en main

## 1. Ce qu'il faut installer

- Git.
- Rust stable (le workspace déclare Rust 1.75 comme version minimale).
- Windows, Linux ou macOS pour compiler et utiliser les outils d'image. Le montage Dokany et les opérations sur disques physiques sont spécifiques à Windows.

## 2. Récupérer et compiler

Depuis la racine du dépôt :

```sh
cd OpenFS
cargo build --workspace
```

Pour lancer les vérifications :

```sh
cargo fmt --all -- --check
cargo test --workspace --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

La CI de la branche [OpenFS](https://github.com/Likounsee/OpenFS/actions) exécute aussi les contrôles Windows, les tests C et la compilation WDK. Consulte son résultat récent : ne considère pas une commande comme validée simplement parce qu'elle figure ici.

## 3. Créer une image de test

Toujours depuis le dossier OpenFS/ :

```sh
cargo run -p openfs-tool -- create-image ./test-openfs.img 128
cargo run -p openfs-tool -- inspect-image ./test-openfs.img
cargo run -p openfs-tool -- check-image ./test-openfs.img 4096
```

La taille est exprimée en Mio (MiB). La taille de bloc utilisée par défaut pour la création est de 4096 octets. La commande initialise le volume et son namespace racine.

Une image est un simple fichier : elle permet de développer et de tester sans toucher à un disque physique.

## 4. Ajouter des fichiers

```sh
cargo run -p openfs-tool -- mkdir ./test-openfs.img 4096 /documents
cargo run -p openfs-tool -- touch ./test-openfs.img 4096 /documents/bonjour.txt
cargo run -p openfs-tool -- write ./test-openfs.img 4096 /documents/bonjour.txt "Bonjour OpenFS"
cargo run -p openfs-tool -- cat ./test-openfs.img 4096 /documents/bonjour.txt
cargo run -p openfs-tool -- ls ./test-openfs.img 4096 /
cargo run -p openfs-tool -- tree ./test-openfs.img 4096 /
```

Pour copier un fichier depuis l'ordinateur vers l'image, utilise import. Pour l'extraire, utilise export :

```sh
cargo run -p openfs-tool -- import ./test-openfs.img 4096 ./photo.png /documents/photo.png
cargo run -p openfs-tool -- export ./test-openfs.img 4096 /documents/photo.png ./photo-exportee.png
```

## 5. Vérifier une image

- inspect-image affiche les informations générales du volume.
- check-image effectue un contrôle non mutatif des structures principales.
- audit-image vérifie aussi les relations entre namespace et allocations ; il ouvre le chemin writable et peut rejouer des transactions validées.
- recover-image ouvre volontairement le chemin de récupération et peut modifier l'image.

Avant audit-image ou recover-image, conserve une copie de l'image. Si tu ne sais pas pourquoi une image présente un état inhabituel, commence par check-image.

## 6. Montage Windows

Le montage d'une image Windows utilise actuellement Dokany. Il faut installer Dokany, puis lancer :

```powershell
cargo run -p openfs-tool -- create-image .\test-openfs.img 128
cargo run -p openfs-tool -- mount .\test-openfs.img O: 4096
```

Le processus reste actif pendant le montage. Pour démonter depuis un autre terminal :

```powershell
cargo run -p openfs-tool -- unmount O:
```

Ce chemin est expérimental. Le pilote WDM natif décrit dans [Pilote Windows](WINDOWS_DRIVER.md) n'est pas encore capable de monter un volume utilisable.

## Commandes utiles

| Commande | Rôle |
|---|---|
| create-image | Crée et initialise une image OpenFS |
| create-gpt-image | Crée une image avec une table GPT |
| inspect-image | Affiche les informations du volume |
| check-image | Vérifie les structures principales sans les réparer |
| audit-image | Contrôle le namespace et les allocations |
| recover-image | Lance le chemin de récupération |
| ls, tree, find | Parcourt le namespace |
| mkdir, mkdir-p, touch | Crée des dossiers et fichiers |
| cat, read-range | Lit un fichier ou une plage |
| write, write-range, append | Écrit dans un fichier |
| import, export, copy | Transfère ou copie des fichiers |
| truncate, rm, mv | Modifie la taille, supprime ou déplace |

Pour les arguments exacts, lance cargo run -p openfs-tool -- help.
