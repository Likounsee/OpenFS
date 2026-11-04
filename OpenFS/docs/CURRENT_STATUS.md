# OpenFS — état courant

Date : **2026-11-04** · Branche : `OpenFS` · HEAD examiné : `4127059d7a4b48d45ba32942dd986cb318609096`.

[CI exacte-SHA : run 41728407756](https://github.com/Likounsee/OpenFS/actions/runs/41728407756)

## Validation

| Contrôle | Résultat |
|---|---|
| rustfmt Linux/Windows | OK |
| Tests workspace Linux/Windows | OK |
| Crash sweep release Linux (4K/16K, réclamation interrompue incluse) | OK |
| Tests C natifs | OK |
| Build/link WDK x64 | OK |
| Clippy strict Linux/Windows | **Échec** : `unused_mut` ligne 551 de `crash_sweep.rs` |

## État fonctionnel

Le workspace Rust couvre format versionné, block I/O, allocation/extents, namespace persistant, opérations de fichiers, journal/checkpoints, outils et récupération. Le chemin de récupération gère les queues de journal incomplètes, refuse un COMMIT après lacune, distingue les erreurs d'E/S et de corruption, et réclame les blocs orphelins via un parcours d'ownership vérifié.

Les crash sweeps simulent des pertes de cache et des coupures à chaque écriture/flush, y compris pendant la réclamation d'orphelins. Ils ne prouvent pas le comportement des contrôleurs et caches physiques ; les mutations multi-étapes ne sont pas toutes atomiques globalement.

## Intégrations OS

- **Windows Dokany :** adaptateur expérimental de montage d'images.
- **Windows WDM natif :** build CI et tests C réussis, mais pas de montage fonctionnel. Snapshot namespace/VCB, root FCB/DCB, lookup, énumération et dispatch read/write manquants.
- **Linux :** montage non implémenté.
- **USB brut :** code présent, validation matérielle absente ; essais physiques en pause.

## Priorités

1. Corriger le `unused_mut` et obtenir une CI verte.
2. Développer le montage WDM natif : snapshot namespace, root FCB/DCB, lookup, énumération, lecture.
3. Renforcer fuzzing, corruption, erreurs d'E/S et coût du scan d'inventaire.
4. Reprendre les tests USB physiques seulement lors d'une phase dédiée.

## Estimations indicatives

| Domaine | Avancement |
|---|---:|
| Format/volume | 85 % |
| Allocation/extents | 75 % |
| Namespace/fichiers Rust | 80 % |
| Journal/checkpoints | 80 % |
| Résistance aux crashs simulés | 80 % |
| Outils Rust | 60 % |
| Pilote WDM natif | 26 % |
| Produit complet | Pas de pourcentage global fiable |

Ces estimations ne mesurent ni la qualité de production ni la couverture exhaustive.
