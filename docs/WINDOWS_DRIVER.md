# Pilote Windows natif OpenFS

## État actuel

Le dépôt contient deux voies Windows distinctes :

1. **Adaptateur Rust/Dokany** : voie expérimentale user-mode pour exposer une image OpenFS sous Windows.
2. **Pilote WDM natif en C** : véritable système de fichiers noyau, situé dans `OpenFS/windows-driver/`.

Le pilote natif n'est plus au stade d'une simple sonde : le dispatch `IRP_MN_MOUNT_VOLUME` appelle `OpenFsProbeVolume`, crée le périphérique de volume privé, attache la pile, installe le snapshot du namespace, réserve le VPB puis publie le volume. Le code fournit également des dispatchs en lecture seule pour l'ouverture, la lecture, les informations, les répertoires et plusieurs FSCTL.

**État de validation :** compilation WDK x64 et tests C/Rust CI réussis sur le commit `59bf12e`. Aucun test de montage sur matériel réel n'a encore été effectué.

## Capacités présentes dans le code

- Enregistrement auprès du gestionnaire d'E/S avec `IoRegisterFileSystem`.
- Sonde en lecture seule des superblocs primaire et secondaire.
- Validation des révisions internes, CRC32C, géométrie, taille exacte, bitmap d'allocation et inventaire.
- Snapshot du namespace, DCB racine et cache FCB/DCB.
- Création du VDO/VCB, attachement au périphérique inférieur et publication du VPB après initialisation.
- Ouverture de fichiers et répertoires en lecture seule, partage et durée de vie des FCB.
- Lecture des données via les extents.
- Informations fichiers/volumes, énumération de répertoires et FSCTL bitmap, retrieval pointers et allocated ranges.
- Verrouillage du volume.

## Limites importantes

- Le build de développement n'est pas signé pour une installation standard sur Windows 11.
- L'artefact CI est actuellement constitué du `.sys` et de l'INF ; il ne fournit pas encore un package signé prêt à installer.
- Aucun essai sur un vrai périphérique USB n'a validé le montage ni la lecture.
- Le démontage complet n'est pas disponible : `FSCTL_DISMOUNT_VOLUME` retourne `STATUS_DEVICE_BUSY` tant que la destruction différée et la désinscription du VPB ne sont pas sûres.
- Le pilote est volontairement en lecture seule. Aucun formatage ni aucune écriture n'est pris en charge.
- L'intégration complète Cache Manager / Memory Manager, les aspects de sécurité, la concurrence, la récupération après panne et la validation HLK restent à faire.

## Compilation

Prérequis : Visual Studio avec outils C++, Windows SDK et WDK compatibles, outils x64.

```powershell
msbuild .\OpenFSDriver.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

La compilation ne prouve pas le chargement du pilote ni le bon fonctionnement du montage. Ne pas désactiver Secure Boot ou activer le mode test sur le PC principal pour installer le build non signé. La première validation de chargement doit être faite dans une VM Windows de test avec instantané.

## Plan de validation USB

Voir [WINDOWS_DRIVER_USB_TEST_PLAN.md](WINDOWS_DRIVER_USB_TEST_PLAN.md). La clé de test doit être identifiée par son modèle, sa capacité et son numéro de série à chaque étape. Les disques système ne doivent jamais être ciblés. La création d'un volume OpenFS attendra un formateur validé ; aucune commande de formatage brut ne fait partie des tests actuels.
