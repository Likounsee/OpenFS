# OpenFS — préparation des tests sur périphérique USB (Windows)

## État actuel — lire avant d'essayer

Le CLI OpenFS documenté est principalement orienté images de fichiers. La crate `openfs-block` contient déjà un `RawDevice` capable d'effectuer des E/S par blocs sur un chemin explicitement fourni, mais ce type ouvre le chemin en lecture **et** écriture et exige une géométrie fournie par l'appelant. Il ne découvre pas lui-même la taille/les secteurs d'un disque Windows, ne démonte pas les volumes et ne rend pas sûre une opération destructive.

**À ce stade, le script ci-dessous ne teste pas OpenFS sur la clé et n'écrit rien.** Il sert à identifier la clé sans ambiguïté avant de développer un chemin raw-device contrôlé. N'utilise pas directement `RawDevice` sur un disque de données : une mauvaise géométrie ou une écriture peut endommager la table de partitions et les systèmes de fichiers existants.

## 1. Compiler et vérifier le socle image

Depuis la racine du dépôt :

```powershell
cd OpenFS
cargo build --workspace
cargo fmt --all -- --check
cargo test --workspace --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

Ces commandes valident le workspace hôte et les images simulées; elles ne valident pas les E/S physiques USB.

## 2. Identifier la clé USB sans écriture

1. Sauvegarde les données de la clé et débranche les autres disques externes si possible.
2. Ouvre PowerShell. Pour afficher les disques, lance depuis la racine du dépôt :

```powershell
.\OpenFS\scripts\inspect-usb-disks.ps1
```

3. Identifie la clé par **BusType USB, modèle, numéro de série et capacité**. Ne te fie jamais au seul numéro de disque : il peut changer après reconnexion.
4. Inspecte un candidat, par exemple le disque 3 :

```powershell
.\OpenFS\scripts\inspect-usb-disks.ps1 -DiskNumber 3
```

Le script est strictement consultatif. Il n'initialise, ne partitionne, ne formate et n'écrit sur aucun disque. Si `IsBoot` ou `IsSystem` vaut `True`, ne sélectionne pas ce disque pour les tests.

Alternative native pour comparer les informations affichées :

```powershell
Get-Disk | Format-Table Number, FriendlyName, SerialNumber, BusType, Size, PartitionStyle, IsBoot, IsSystem
Get-Volume | Format-Table DriveLetter, FileSystemLabel, FileSystem, Size, SizeRemaining
```

## 3. État des étapes de validation

| Étape | État |
|---|---|
| Inventaire des disques Windows en lecture seule | Script ajouté; exécution sur ton PC à faire |
| Backend bloc d'image | Déjà présent |
| Backend raw-device générique | Présent dans `openfs-block`; ouvre en lecture/écriture et nécessite la géométrie explicite |
| Découverte fiable de géométrie Windows et garde-fou contre le disque système | Pas encore implémentés dans le workflow CLI |
| Test de lecture seule des secteurs physiques via OpenFS | Pas encore exposé par le CLI |
| Écriture de motif, relecture et checksum sur clé dédiée | Pas encore exposées/validées |
| Initialisation du format OpenFS sur la clé | Ne pas tenter pour le moment |
| Persistance validée après débranchement/redémarrage physique | Non testée |

## 4. Conditions préalables à tout test d'écriture

Ne lance aucune commande destructive sur la base de cette page. Le prochain lot doit ajouter un mode d'ouverture réellement read-only, une découverte de géométrie vérifiée, une confirmation explicite liant le numéro de série et la capacité, des limites de plage contrôlées, puis un test d'écriture uniquement activé par un drapeau explicite sur une clé dédiée. La détection USB ne prouve ni l'accès aux secteurs ni la compatibilité du format.

Pour les premiers essais, conserve la clé sauvegardée et déconnecte les volumes qui pourraient utiliser le périphérique. Les verrous consultatifs OpenFS ne peuvent pas garantir que Windows ou un autre logiciel n'accède pas au même disque.
