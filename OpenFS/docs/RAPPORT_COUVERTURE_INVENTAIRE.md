# OpenFS — rapport de couverture de l'inventaire

Date du relevé : 2026-10-21. Branche ciblée : `OpenFS`. Commit source examiné : `73c50c9f7977c8b34146729bb5e4e3a5777c9fc1`.

## État réel

- Fichiers suivis (blobs) : 43.
- Fichiers Rust : 16.
- Fichiers Rust dont les déclarations ont été indexées : 16/16 (100.0 %).
- Déclarations de fonctions/méthodes détectées par extraction ligne à ligne : 683.
- Déclarations de struct/enum/trait/type détectées : 56.
- Fichiers dont le contenu a été analysé fonctionnellement de bout en bout dans ce lot : 0/43. L'indexation des déclarations ne vaut pas validation comportementale.
- Inventaire fonctionnel CSV enrichi : 1 183 entrées conservées, 24 colonnes (8 colonnes historiques + 16 champs de traçabilité). Les identifiants `OFS-00001` à `OFS-01183` ont été attribués dans l'ordre des lignes conservées.

## Classification du CSV enrichi

- Complete : 310
- Partial : 846
- Not implemented : 27

Ces compteurs sont issus d'une normalisation automatique des statuts historiques, pas d'une validation complète du comportement de chaque fonctionnalité. Les entrées `FONCTION` avec « corps détecté » restent `Partial` tant que le comportement de bout en bout n'est pas vérifié. Les entrées roadmap et méta-inventaire doivent être considérées comme pistes de couverture, pas comme preuve de fonctionnalités runtime.

## Principaux domaines à auditer ensuite

1. Allocation bitmap/extents, propriété et libération des blocs — `allocator_extents.rs`, puis appels dans `volume.rs`.
2. Journal, commit durable, checkpoints, rejeu et récupération après échec d'E/S — `journal.rs`, `journal_engine.rs`, `checkpoint.rs`.
3. Format persistant, métadonnées, namespace et alias de blocs — `metadata.rs`, `directory_entry.rs`, `namespace.rs`, `extent_map.rs`.
4. Parcours de volume, opérations fichier, publication d'inventaire et vérification — `volume.rs`, `verifier.rs`, `inventory_descriptor.rs`.
5. CLI et interface utilisateur — `openfs-tool/src/main.rs`.
6. Contrats d'intégration OS / montage natif : vérifier s'ils sont implémentés dans ce dépôt ou explicitement hors périmètre.

## Limites de cette passe

Les déclarations de symboles ont été extraites des 16 fichiers Rust. Les corps complets, les appels croisés et les chemins d'erreur n'ont pas tous été audités individuellement. Les fichiers non Rust sont seulement inventoriés, sauf le workflow CI et le CSV principal. Aucun test local n'a été exécuté via ce connecteur. Ne pas annoncer l'audit comme exhaustif.
