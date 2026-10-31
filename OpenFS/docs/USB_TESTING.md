# OpenFS — essais sur une vraie clé USB (Windows)

## État actuel

Le dépôt possède maintenant un premier chemin de montage physique Windows, en plus du backend image :

- `format-usb` initialise un volume OpenFS sur **tout le disque physique** sélectionné. Cette opération détruit la table de partitions et toutes les données présentes sur la clé.
- `mount-usb` ouvre un disque USB déjà hors ligne, vérifie son `UniqueId` exact, son transport USB, son état boot/system et sa géométrie, puis monte le volume avec Dokany.
- `unmount` demande à Dokany de démonter le lecteur. Le processus de montage attend la fin du démontage, effectue un flush final et retourne les erreurs de fermeture différées.
- `scripts/mount-openfs-usb.ps1` automatise le passage hors ligne puis la restauration de l'état initial du disque. Il ne formate jamais.

**Limite majeure :** le chemin est implémenté mais n'a pas encore été validé sur une vraie clé USB dans cette session. La persistance sur matériel, le retrait à chaud, les coupures d'alimentation et la compatibilité réelle avec toutes les applications Windows restent à tester. Le statut CI du commit courant doit être vérifié séparément; aucune réussite n'est présumée.

## 1. Préparer le programme

Depuis la racine du dépôt, dans PowerShell :

```powershell
cd .\OpenFS
cargo build --release -p openfs-tool
```

Le binaire est normalement créé à `OpenFS\target\release\openfs.exe`.

Pour vérifier le socle logiciel, lance aussi :

```powershell
cargo fmt --all -- --check
cargo test --workspace --all-features
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

Ces commandes ne remplacent pas un test matériel.

## 2. Identifier la clé sans écrire dessus

Depuis la racine du dépôt :

```powershell
.\OpenFS\scripts\inspect-usb-disks.ps1
Get-Disk | Format-Table Number, FriendlyName, SerialNumber, UniqueId, BusType, Size, IsOffline, IsBoot, IsSystem
```

Note le numéro du disque et copie son `UniqueId` exact. Débranche les autres disques externes si possible. Ne sélectionne jamais un disque système, de démarrage ou contenant des données à conserver.

## 3. Initialiser OpenFS — opération destructive

**Cette commande efface intégralement le disque choisi.** Sauvegarde la clé, vérifie deux fois son numéro, sa capacité et son `UniqueId`. N'utilise qu'une clé dédiée et sacrifiable.

Dans un PowerShell administrateur :

```powershell
.\OpenFS\target\release\openfs.exe format-usb 3 "UNIQUE-ID-EXACT" ERASE-ALL-DATA
```

Remplace `3` et `UNIQUE-ID-EXACT` par les valeurs réellement affichées sur ton PC. Ne copie pas l'exemple tel quel. Le texte `ERASE-ALL-DATA` est une confirmation obligatoire, pas une protection contre une mauvaise sélection du disque.

## 4. Monter la clé comme un lecteur Windows

Dans un PowerShell administrateur, remplace le numéro, l'identifiant et la lettre :

```powershell
.\OpenFS\scripts\mount-openfs-usb.ps1 -Action Mount -OpenFsExe .\OpenFS\target\release\openfs.exe -DiskNumber 3 -UniqueId "UNIQUE-ID-EXACT" -MountPoint O:
```

La fenêtre reste occupée pendant que le volume est monté. Le lecteur `O:\` doit apparaître dans l'Explorateur Windows. Tu peux y créer, copier, déplacer et supprimer des fichiers comme sur un lecteur expérimental classique.

Le script vérifie de nouveau l'identité du disque, refuse les disques non-USB, boot/system ou read-only, le met hors ligne avant l'ouverture brute, puis lance `mount-usb`. Il ne lance aucun formatage.

## 5. Démonter proprement

Laisse la première fenêtre ouverte. Dans **un deuxième PowerShell administrateur**, lance :

```powershell
.\OpenFS\scripts\mount-openfs-usb.ps1 -Action Unmount -OpenFsExe .\OpenFS\target\release\openfs.exe -MountPoint O:
```

Attends que la première fenêtre se termine et confirme le retour du disque à son état initial avant de débrancher la clé. Ne retire jamais la clé pendant que le lecteur est encore monté ou pendant le flush final.

## 6. Validation matérielle à effectuer

| Essai | État |
|---|---|
| Backend image et tests logiciels | Implémentés; exécution locale/CI à confirmer pour le HEAD |
| Formatage raw USB avec identité exacte | Implémenté; destructive, non validé sur matériel ici |
| Montage Dokany depuis PhysicalDrive | Implémenté; non validé sur matériel ici |
| Création/copie de fichiers via Explorer | À tester sur une clé dédiée |
| Fermeture des handles et flush | Flush callback + flush final implémentés; comportement matériel à valider |
| Démontage puis remontage et comparaison des fichiers | À tester |
| Débranchement surprise / panne d'alimentation | Non validé; risque de perte ou corruption |
| Compatibilité complète Windows (partage, verrouillage, ACL, timestamps) | Incomplète |

## Sécurité

- N'utilise pas de clé contenant des données importantes.
- Ne formate jamais un disque sans vérifier l'identifiant exact.
- Ne retire pas la clé avant la fin du démontage.
- Un `flush` ne protège pas contre une déconnexion ou une coupure pendant une écriture.
- Le montage Dokany initial ne signifie pas que toutes les sémantiques NTFS/Windows sont prises en charge.
- En cas d'erreur I/O ou de retrait à chaud, arrête les écritures et préserve le périphérique pour diagnostic; ne relance pas immédiatement un formatage.
