# Vision des versions OpenFS

**Statut :** orientation produit et objectifs à long terme  
**Langage actuel :** Rust  
**Branche de référence pour le développement actuel :** `OpenFS`

Ce document fixe la distinction entre **OpenFS Basic** et **OpenFS**, la version complète. Il décrit une cible de développement : une fonctionnalité mentionnée ici n'est pas nécessairement déjà implémentée.

## 1. Vision générale

OpenFS sera développé en deux niveaux :

1. **OpenFS Basic** : un système de fichiers généraliste, léger, fiable et pratique, avec pour objectif à terme d'être comparable à **exFAT** dans son domaine d'utilisation.
2. **OpenFS** : la version complète du système de fichiers, qui fera évoluer cette base avec des mécanismes avancés et des objectifs comparables, selon les usages, à ceux de systèmes modernes comme **NTFS, ReFS ou Btrfs**.

OpenFS Basic n'est pas un prototype jetable ni une simple démonstration. Il doit pouvoir devenir une solution utilisable à part entière.

## 2. OpenFS Basic

### Objectif

Construire une alternative crédible à exFAT pour le stockage généraliste et les supports courants : SSD, HDD, clés USB et autres volumes amovibles.

La comparaison avec exFAT est un **objectif de niveau de fonctionnalités, de fiabilité et d'utilisation**. Elle ne signifie pas que le format disque OpenFS sera compatible avec exFAT.

### Fonctionnalités cibles

- Formatage, détection et ouverture de volumes.
- Création, lecture, écriture, modification et suppression de fichiers.
- Création, listage, renommage et suppression de répertoires.
- Allocation et libération correctes des blocs, sans fuite ni double allocation.
- Prise en charge des fichiers volumineux et des écritures multi-blocs.
- Noms Unicode et gestion correcte des noms longs.
- Métadonnées, attributs et dates de fichiers.
- Persistance des données après fermeture et réouverture du volume.
- Gestion rigoureuse des erreurs d'entrée/sortie et des volumes invalides.
- Outils de vérification et, lorsque cela est possible, de réparation.
- Gestion des volumes et partitions.
- Bonnes performances sur SSD, HDD et supports amovibles.
- Tests de remplissage, de corruption, de limites et de coupure/arrêt brutal.

### Principes de conception

- Garder le format et les structures sur disque cohérents et vérifiables.
- Privilégier la fiabilité des opérations et la préservation des données.
- Documenter les limites de récupération en cas d'arrêt brutal.
- Ne pas promettre d'atomicité ou de récupération transactionnelle que l'implémentation ne garantit pas.
- Garder Basic suffisamment simple pour rester léger et maintenable.

Basic peut commencer sans journalisation. L'absence de journal ne doit toutefois jamais être présentée comme une garantie de résistance aux coupures : la cohérence et les possibilités de récupération doivent être testées et documentées explicitement.

## 3. OpenFS — version complète

### Objectif

Faire évoluer la base OpenFS vers un système de fichiers complet, moderne et adapté à des usages plus exigeants.

### Fonctionnalités envisagées

Les éléments suivants constituent des pistes de conception pour la version complète. Ils devront être étudiés, spécifiés et testés avant d'être annoncés comme disponibles :

- Journalisation et récupération avancées.
- Snapshots.
- Copy-on-write (CoW).
- Déduplication.
- Compression avancée.
- Chiffrement.
- Gestion avancée des métadonnées et des volumes.
- Protection renforcée contre la corruption et mécanismes d'intégrité.
- Optimisations spécifiques aux SSD et aux HDD.
- Autres fonctions avancées qui seront retenues au cours du développement.

L'ambition est de faire d'OpenFS une solution moderne pouvant se mesurer à des systèmes comme NTFS, ReFS et Btrfs sur les fonctionnalités pertinentes. Cela ne signifie pas que toutes leurs fonctionnalités seront nécessairement reproduites à l'identique.

## 4. Socle technique commun et évolution

OpenFS Basic doit être conçu comme la fondation de la version complète.

Principes directeurs :

- Partager autant que raisonnablement possible le code et les composants communs.
- Éviter de maintenir deux systèmes de fichiers indépendants.
- Prévoir une architecture extensible sans alourdir inutilement Basic.
- Concevoir les structures sur disque en tenant compte de leur évolution.
- Définir explicitement les règles de compatibilité et de migration entre versions avant de les rendre contractuelles.
- Ne jamais sacrifier la fiabilité de Basic pour intégrer prématurément une fonction avancée.

La compatibilité entre les formats Basic et complet reste une décision d'architecture à spécifier ; elle ne doit pas être supposée automatiquement.

## 5. Ordre de développement

### Étape 1 — Terminer OpenFS Basic

Priorité actuelle : achever et stabiliser les fonctions essentielles, notamment le formatage, l'ouverture, les fichiers, les répertoires, l'allocation, la persistance et les outils de base.

### Étape 2 — Durcir et valider Basic

Multiplier les tests sur les gros fichiers, les volumes remplis, les erreurs d'E/S, les métadonnées invalides, la corruption et les arrêts inattendus. Mesurer les performances et documenter précisément les limites.

### Étape 3 — Concevoir la version complète

Définir les besoins, les structures disque, les garanties de cohérence, les stratégies de récupération et les règles de compatibilité avant d'ajouter les fonctions avancées.

### Étape 4 — Développer et stabiliser OpenFS complet

Intégrer les fonctions avancées progressivement, avec des tests spécifiques, puis valider la fiabilité, les performances et les scénarios de récupération.

## 6. Règle directrice

**OpenFS Basic vise le niveau d'un système de fichiers généraliste comparable à exFAT. OpenFS, la version complète, vise un système de fichiers moderne doté de fonctionnalités avancées et capable de rivaliser avec les solutions établies.**

Le développement actuel se concentre sur **OpenFS Basic**. Les fonctions prévues pour la version complète ne doivent pas détourner les efforts de cette priorité.

---

*Ce document définit la vision et les objectifs du projet. L'état réel d'implémentation doit toujours être vérifié dans le code, les tests et la documentation technique correspondante.*
