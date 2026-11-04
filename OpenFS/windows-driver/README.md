# OpenFS 1.0 — pilote Windows natif

> **État au 2026-11-04 — pilote WDM x64 compilable, montage en cours d'intégration, pas encore validé sur matériel réel.** Le commit `59bf12e` passe les quatre jobs CI, dont la compilation WDK x64. Le code du chemin `IRP_MN_MOUNT_VOLUME` crée maintenant un VDO/VCB, charge le snapshot du namespace, réserve puis publie le VPB. Les dispatchs en lecture seule couvrent notamment ouverture, lecture, informations, énumération et plusieurs FSCTL. Cela ne constitue pas encore une validation fonctionnelle sur Windows réel.

## Ce qui est réellement présent

- Enregistrement du système de fichiers via `IoRegisterFileSystem`.
- Détection du volume et lecture seule des superblocs principal et secondaire.
- Validation des révisions de format prises en charge, CRC32C, géométrie, taille exacte et bitmap d'allocation.
- Découverte et décodage de l'inventaire persistant et du namespace.
- Création du VDO/VCB, attachement à la pile du périphérique inférieur, initialisation du DCB racine et cache FCB/DCB.
- Réservation et publication du VPB après initialisation du namespace.
- Ouverture en lecture seule, lecture de fichiers, informations de fichiers/volumes et contrôle d'accès de partage.
- Contrôle de répertoires et requêtes FSCTL de bitmap, pointeurs de récupération et plages allouées.
- Verrouillage du volume et refus du démontage tant que la destruction différée du VDO/VPB n'est pas sûre.
- Tests hôte C pour les décodeurs et structures ; compilation WDK x64 en CI.

## Limites avant un test sur le PC principal

- Le pilote est un build de développement non signé ; le dépôt ne fournit pas encore de package signé installable sur un Windows standard.
- L'artefact CI actuel contient le binaire `.sys` et l'INF, mais pas un catalogue signé.
- Aucun test de montage sur matériel réel n'a encore été effectué.
- Le démontage sûr n'est pas achevé ; `FSCTL_DISMOUNT_VOLUME` retourne volontairement `STATUS_DEVICE_BUSY`.
- Le pilote est en lecture seule : pas de formatage, création, suppression, renommage ni écriture.
- Les exigences avancées de Cache Manager, Memory Manager, sécurité, concurrence et validation HLK restent à compléter.

**Ne pas désactiver Secure Boot ni activer le mode test sur le PC principal pour charger ce build.** La première validation d'installation doit se faire dans une machine virtuelle Windows de test, avec un instantané récupérable. Le passage à un test USB sur l'hôte attendra un package et une procédure de signature sûrs.

## Compilation

Prérequis : Visual Studio avec outils C++, Windows SDK et WDK compatibles, outils x64.

```powershell
msbuild .\OpenFSDriver.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

La CI compile le pilote avec le WDK x64 et exécute les tests hôte. Une compilation réussie ne prouve ni le chargement du pilote, ni le montage, ni la lecture correcte d'un volume réel.

## Sources principales

- `OpenFSDriver.c` : DriverEntry, dispatchs IRP et transaction de montage.
- `OpenFSVolume.c/.h` : VDO/VCB, FCB/DCB, VPB, cycle de vie et lecture de fichiers.
- `OpenFSDeviceIo.c/.h` : lecture bloc et sonde de volume.
- `OpenFSSuperblock.c/.h` : validation des superblocs.
- `OpenFSNamespace.c/.h` : décodage et résolution du namespace.
- `OpenFSExtentMap.c/.h` : décodage et résolution des extents.

Voir aussi [le plan de tests USB Windows](../docs/WINDOWS_DRIVER_USB_TEST_PLAN.md).
