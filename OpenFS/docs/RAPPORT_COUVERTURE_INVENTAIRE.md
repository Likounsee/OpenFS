# OpenFS — rapport de couverture de l'inventaire reanalysé

Date du relevé : 2026-10-21.
Branche : `OpenFS`.
Commit de code analysé : `1dc834990da67b7d742138a01805db6b14cf716d`.
Les commits de ce relevé actualisent les fichiers de documentation/inventaire seulement; ils ne changent pas le code Rust audité.

## Résultats vérifiés

- Arbre Git au commit de code analysé : 46 fichiers suivis; le dépôt compte maintenant 47 fichiers suivis après l'ajout du nouvel inventaire fonctionnel.
- Sources Rust récupérées et parcourues : 16/16.
- Déclarations indexées par extraction ligne à ligne : 989.
- Répartition : 779 fonctions/méthodes, 74 constantes, 51 blocs `impl`, 48 structures, 27 modules, 5 enums, 3 types, 1 trait et 1 static.
- Inventaire fonctionnel complémentaire : 39 capacités et lacunes, avec chemin source, statut, preuve, prochaine étape et confiance.
- L'indexation est syntaxique et ligne par ligne; elle peut manquer des déclarations multi-lignes ou générées par macro. Elle n'est pas un parseur Rust complet et ne valide pas automatiquement le comportement.

## Fichiers actualisés

- `INVENTAIRE_SYMBOLES.csv` : index de 989 déclarations détectées au commit de code indiqué.
- `INVENTAIRE_FICHIERS.csv` : métadonnées des 46 fichiers suivis au commit de code indiqué (taille et SHA Git).
- `INVENTAIRE_FONCTIONNEL_REANALYSE.csv` : 39 lignes d'analyse fonctionnelle et de lacunes.
- `RAPPORT_COUVERTURE_INVENTAIRE.md` : ce rapport.

## Limite importante du CSV historique

Le fichier `INVENTAIRE_EXHAUSTIF.csv` historique est conservé sans écrasement. Le connecteur GitHub n'a pas renvoyé son contenu, car il est trop volumineux pour une récupération complète dans cette session. Je ne pouvais donc pas fusionner les nouvelles données sans risquer de supprimer des entrées ou des colonnes. Le nouveau `INVENTAIRE_FONCTIONNEL_REANALYSE.csv` est un relevé complémentaire actualisé, pas une affirmation que toutes les lignes historiques ont été réévaluées.

## Résumé fonctionnel

### Capacités présentes, mais garanties à valider

- Abstraction de blocs bornée, image fichier et injection d'erreurs d'E/S.
- Format canonique, superblocs, bitmap, extents, métadonnées et namespace.
- Codec et moteur de journal redo persistant, checkpoints et récupération.
- Publication/remplacement du descripteur d'inventaire avec transition de propriété journalisée.
- API persistantes de création/renommage/suppression, écriture de contenu, écriture par plage et troncature.
- Vérification et audit de propriété des blocs.
- CLI orientée images disque.

### Lacunes majeures

1. La transaction du journal ne garantit pas à elle seule l'atomicité de chaque opération fichier multi-étapes.
2. La durabilité physique sur matériel réel n'est pas prouvée par les tests d'images fichier.
3. Le vérificateur est en lecture seule; aucune réparation générale de type fsck n'est établie.
4. Aucun montage natif Windows/Linux n'est établi dans le workspace actuel.
5. La compatibilité binaire NTFS/ReFS, les ACL complètes, la compression, le chiffrement et les snapshots ne sont pas établis par le code audité.
6. Le CSV historique doit être fusionné localement avec ce relevé si l'on veut conserver et réévaluer chaque ancienne entrée.

## Interprétation des statuts

- `Complete` : uniquement après preuve du comportement et tests associés.
- `Partial` : une partie de la capacité est présente, mais garanties, cas limites ou intégration restent à démontrer.
- `Not implemented` : aucune implémentation n'a été établie dans le périmètre examiné.
- L'absence de preuve dans cette passe n'est pas une preuve universelle d'absence; les statuts sont des résultats d'audit, pas une mesure de pourcentage globale.

## CI

Exécution vérifiée : `39666862823`, statut `completed`, conclusion `cancelled`. La CI ne doit être déclarée verte qu'après une conclusion positive des étapes formatage, tests workspace et Clippy sur Linux et Windows.


## Mise à jour du 2026-10-22

- CI exacte-SHA vérifiée sur le commit `2b65a61e535c19a56edeb6f760508f606a4a7819` : run `39704510031`, conclusion `success`; les jobs Linux et Windows ont terminé avec succès pour formatage, tests du workspace et Clippy.
- Les tests récents ajoutent une mesure explicite des écritures et flushes pendant la vérification d'inventaire; la couverture des deux API du vérificateur est en cours d'ajout et attend sa CI exacte-SHA.
- L'index des symboles reste à l'ancien commit `1dc834990da67b7d742138a01805db6b14cf716d`; ses numéros de ligne et son statut comportemental ne sont donc pas à jour au HEAD courant.
- Le fichier `INVENTAIRE_EXHAUSTIF.csv` fait environ 1,6 Mo. Son contenu intégral n'a pas été retourné par le connecteur dans cette session; il n'a été ni fusionné ni réécrit. L'inventaire fonctionnel complémentaire ne remplace pas ce fichier.


## Mise à jour — CI et test négatif du vérificateur

- Le run `39791177933` sur `2c9d77fc56e020e955f8721b0e0e97bfcb227fcc` a échoué sur Linux et Windows à l'étape `cargo fmt --check`; les tests workspace et Clippy ont donc été ignorés. La différence indiquée par les logs était uniquement la mise en forme d'un `assert!`.
- Le formatage est corrigé. En complément, le test de descripteur sémantiquement invalide à CRC valide exige désormais zéro écriture et zéro flush sur cinq tailles de blocs. Cette révision doit encore passer sa propre CI.


## Réconciliation de l'inventaire des fichiers — snapshot c8b8cacf02ff

- `INVENTAIRE_FICHIERS.csv` a été régénéré depuis l'arbre Git récursif non tronqué : 46 fichiers suivis, avec chemin, catégorie, taille et blob SHA tirés de Git.
- L'entrée historique `DIFFERENCES_OPENFS_OLD.txt` a été retirée de cet index courant car elle n'existe pas dans l'arbre Git de ce snapshot.
- L'index décrit les métadonnées du snapshot source `c8b8cacf02ff`; la ligne du fichier d'inventaire lui-même est explicitement marquée comme auto-référentielle, car un fichier ne peut pas contenir son propre SHA final.
- Cette réconciliation ne remplace pas la relecture sémantique des sources, ne reconstruit pas `INVENTAIRE_SYMBOLES.csv` et ne modifie pas `INVENTAIRE_EXHAUSTIF.csv`.


- Contrat corrigé dans le test : le rapport caller-located doit signaler un descripteur non déclaré comme allocation orpheline; le rapport découvert doit inclure ce bloc et être cohérent. Le compteur d'écritures/flushes reste à zéro pour les deux.


## Relecture complémentaire des modules et compatibilité

- Les modules `metadata.rs` et `directory_entry.rs` décrivent désormais précisément leurs responsabilités de codec, sans nier l'existence des opérations persistantes dans les couches supérieures.
- Le commentaire d'en-tête de `namespace.rs` précise que le modèle reste en mémoire, mais qu'il est utilisé par les opérations de volume persistantes pour valider les changements.
- `COMPATIBILITY.md` distingue désormais la récupération journalisée déjà présente pour certains chemins des garanties universelles encore absentes pour toutes les opérations de fichiers et d'espace de noms.
- `INVENTAIRE_SYMBOLES.csv` demeure volontairement signalé comme index historique : ses lignes doivent être régénérées par une extraction de symboles reproductible, plutôt que de simplement remplacer le SHA source sans recalculer les numéros de ligne.


## Mise à jour — index de symboles reproductible (snapshot 31adce9a3974)

- Les 16 fichiers Rust du workspace ont été relus pour reconstruire l'index lexical à partir de leurs lignes de déclaration; le nouvel index contient 1 003 entrées.
- Le script `OpenFS/scripts/generate_symbol_inventory.py` régénère `INVENTAIRE_SYMBOLES.csv` depuis le checkout courant, sans dépendance Python externe.
- L'index distingue la détection syntaxique du comportement : le statut `Indexed` signifie seulement qu'une déclaration et son numéro de ligne ont été détectés. Ce n'est ni une validation sémantique ni une couverture de tests.
- `INVENTAIRE_EXHAUSTIF.csv` est conservé inchangé; son contenu intégral n'a pas été relu dans cette passe, car sa récupération complète n'est pas disponible via le connecteur.
- La CI exacte-SHA du snapshot précédent est verte : run 39792518837. La CI du commit contenant cette mise à jour devra être vérifiée séparément.


## Contrôle de reproductibilité en CI

La CI Linux et Windows régénère l'index lexical avec `scripts/generate_symbol_inventory.py` et échoue si `docs/INVENTAIRE_SYMBOLES.csv` diffère du fichier suivi. Cela vérifie la reproductibilité de l'inventaire, pas le comportement sémantique des symboles.


## Mise à jour du 2026-10-23 — suppression persistante

- Un test de récupération de suppression couvre désormais les erreurs d'écriture et de flush injectées aux positions 0 à 8, sur cinq tailles de blocs (4 à 64 Kio).
- Après réouverture, il vérifie l'ancien contenu si le chemin existe et la cohérence de propriété de l'inventaire découvert. Les erreurs de lecture inattendues ne sont pas acceptées comme preuve de suppression.
- Cette couverture repose sur une image fichier et un périphérique de test à fautes injectées; elle ne simule pas toutes les propriétés des caches et flushes d'un périphérique physique. La validation exacte-SHA reste requise.
- La matrice de suppression récursive vérifie aussi que chaque panne configurée a été déclenchée, pour éviter les faux positifs liés à des indices d'injection jamais atteints.


## Mise à jour du 2026-10-23 — suppression récursive après panne

- Une matrice complémentaire couvre la suppression d'un sous-arbre contenant plusieurs niveaux et fichiers, avec injections d'erreurs d'écriture et de flush à différentes positions et tailles de blocs.
- Après réouverture, le test exige un état complet ancien ou nouveau, la conservation d'un fichier extérieur et un inventaire d'ownership cohérent.
- Le test étend la couverture simulée des frontières de suppression; il ne garantit pas les caractéristiques physiques des caches ou des commandes flush d'un périphérique réel.


## Mise à jour du 2026-10-23 — suppression récursive réussie après prévalidation

- Le test existant de refus des cibles invalides vérifie désormais aussi le chemin de succès : suppression d'un répertoire contenant un fichier et un sous-répertoire vide.
- Après suppression, le test exige que ni le fichier ni le répertoire vide ne soient résolvables et que l'inventaire persistant découvert reste cohérent.
- L'extension réutilise un test existant afin de préserver l'index des déclarations historiques; elle ne régénère ni ne supprime d'entrées d'inventaire.
- La validation reste à confirmer par la CI sur le commit exact.
