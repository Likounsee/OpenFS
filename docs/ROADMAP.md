# Feuille de route

Cette feuille de route décrit les grands chantiers. Elle ne donne pas de pourcentage artificiel : un composant compilable n'est pas forcément une fonctionnalité utilisable.

## 1. Stabiliser le moteur

- [x] Workspace Rust séparé en crates.
- [x] Format versionné et validation des structures principales.
- [x] Couche d'accès aux blocs et images de développement.
- [x] Bitmap, extents, métadonnées et namespace persistant.
- [x] Journal, checkpoints et chemins de récupération pour les opérations prises en charge.
- [x] Outils CLI pour créer, inspecter, vérifier et manipuler des images.
- [ ] Étendre les garanties transactionnelles à toutes les mutations complexes.
- [ ] Renforcer fuzzing, tests de corruption et compatibilité entre révisions.
- [ ] Définir et publier une spécification disque complète et stable.

## 2. Terminer le pilote Windows natif

- [x] Enregistrement du FSD et réception des demandes de montage.
- [x] Détection en lecture seule et validation des superblocs.
- [x] Premiers validateurs et codecs C avec tests hôte.
- [ ] Conserver le namespace validé et initialiser le VCB/DCB racine.
- [ ] Terminer publication VPB, rollback et démontage.
- [ ] Implémenter create/open, informations et énumération.
- [ ] Implémenter lecture, puis écriture et opérations de namespace.
- [ ] Ajouter cache, sécurité, partage, annulation et concurrence.
- [ ] Tests sur Windows réel, HLK et signature de production.

## 3. Intégration des systèmes

- [x] Adaptateur Windows Dokany expérimental pour les images.
- [ ] Pilote WDM natif montable et utilisable.
- [ ] Adaptateur de montage Linux.
- [ ] Intégration éventuelle avec ArchiaOS.

## 4. Périphériques et fiabilité

- [x] Abstractions de périphériques et support GPT dans le code.
- [ ] Reprendre les tests USB physiques avec un protocole sûr et du matériel jetable.
- [ ] Valider les comportements de flush, retrait, panne et redémarrage sur matériel réel.
- [ ] Benchmarks et tests longue durée.

## Critère de maturité

OpenFS ne sera pas prêt pour un usage quotidien tant que le format, la récupération, les pilotes, la sécurité, les performances et les tests sur matériel réel ne seront pas validés ensemble.
