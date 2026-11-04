# Architecture

OpenFS est un système de fichiers indépendant. Il possède son propre format sur disque et ses propres règles de gestion des fichiers. Il ne s'appuie pas sur NTFS ou ReFS pour stocker ses données.

## Vue d'ensemble

```text
Applications et outils
        |
        v
Adaptateur du système d'exploitation
  Windows : Dokany aujourd'hui
  Windows : pilote WDM natif en développement
  Linux   : adaptateur à créer
        |
        v
API de volume et namespace
        |
        v
Format, métadonnées, extents et journal
        |
        v
Couche d'accès aux blocs
        |
        v
Image fichier ou périphérique
```

Le schéma représente les responsabilités visées. Tous les chemins ne sont pas encore reliés : en particulier, le pilote WDM ne monte pas encore de volume.

## Les projets Rust

Le workspace se trouve dans OpenFS/ et contient cinq crates :

| Crate | Responsabilité |
|---|---|
| openfs-core | Erreurs communes, validations et utilitaires sûrs |
| openfs-block | Accès aux blocs, images et périphériques |
| openfs-format | Structures disque, allocations, namespace, journal et opérations de volume |
| openfs-partition | Lecture, validation et écriture des tables GPT |
| openfs-tool | Programme en ligne de commande et intégration Windows/Dokany |

### Pourquoi séparer ces couches ?

- Le format ne doit pas dépendre de l'interface graphique ou du système de fichiers hôte.
- La couche bloc doit empêcher les lectures et écritures hors limites.
- Les outils doivent réutiliser les mêmes règles que la bibliothèque.
- Les adaptateurs Windows et Linux doivent traduire les demandes du système sans redéfinir le format disque.

## Le namespace

Le namespace représente les dossiers et les fichiers. Chaque objet possède un identifiant et des métadonnées. Les entrées de répertoire relient un nom à un objet et à son dossier parent.

Le modèle actuel utilise des noms sensibles à la casse et un seul parent par objet. Il ne fournit donc pas encore toutes les sémantiques d'un système de fichiers Windows ou Linux.

## Données et allocations

Les fichiers sont décrits par des métadonnées et des extents. Un extent indique où se trouve une plage de blocs du fichier sur le volume. La bitmap suit les blocs utilisés ou libres. L'inventaire persistant permet de retrouver et vérifier les objets et leurs allocations.

## Journal et récupération

Le journal enregistre les mises à jour persistantes nécessaires aux opérations prises en charge. Les checkpoints permettent de suivre la progression du journal. À la réouverture writable, OpenFS peut récupérer les transactions validées et réclamer certains blocs orphelins après vérification de leur propriété.

Cela améliore la récupération après interruption, mais ne garantit pas que toutes les opérations complexes sont atomiques dans tous les scénarios de panne. Les essais simulés ne remplacent pas les tests de coupure sur du vrai matériel.

## Code natif Windows

Le dossier OpenFS/windows-driver/ contient un pilote WDM en C. Il ne réutilise pas directement le moteur Rust : le noyau Windows impose des contraintes de mémoire, d'IRP, de synchronisation et de dépendances différentes.

Les codecs natifs et les tests C doivent rester conformes au format Rust. Le pilote doit être considéré comme un chantier séparé tant que son cycle de montage et ses dispatchs de fichiers ne sont pas terminés.
