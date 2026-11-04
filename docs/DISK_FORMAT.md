# Format disque

Cette page donne une vue compréhensible du format. Elle ne remplace pas une spécification binaire exhaustive.

## Principes

- OpenFS possède son propre format ; un volume OpenFS n'est pas un volume NTFS ou ReFS.
- Les structures disque sont encodées explicitement en little-endian. Les structures Rust en mémoire ne sont pas écrites telles quelles.
- Les données lues depuis un volume sont considérées comme non fiables et doivent être validées.
- Les tailles, offsets et calculs de blocs doivent être vérifiés contre les débordements et les limites du volume.
- Les structures importantes utilisent des sommes de contrôle CRC32C.

## Principales structures

| Structure | Utilité |
|---|---|
| Superbloc principal et copie de secours | Identifie le volume, sa révision de format et sa géométrie |
| Layout | Décrit les régions réservées et les régions de données |
| Bitmap d'allocation | Indique quels blocs sont occupés |
| Métadonnées | Décrivent les objets, leur type, leur parent et leur taille |
| Entrées de répertoire | Associent un nom à un objet |
| Extent maps | Localisent les blocs de données d'un fichier |
| Inventaire persistant | Permet de découvrir et valider les objets et leurs allocations |
| Journal et checkpoints | Aident à récupérer les mises à jour après une interruption |

## Révisions

La révision de format inscrite dans le superbloc est indépendante de la version commerciale du projet. Le code Rust actuel écrit la révision 4 et conserve des chemins de lecture pour des révisions antérieures prises en charge. Le pilote natif doit appliquer exactement les mêmes règles de compatibilité.

Ne change pas une structure disque sans :
1. définir sa nouvelle représentation et sa version ;
2. prévoir le comportement à la lecture des anciennes versions ;
3. ajouter des tests de compatibilité et des fixtures stables ;
4. mettre à jour la documentation et les codecs natifs concernés.

## Vérification

Utilise d'abord check-image pour contrôler les structures principales sans modifier l'image. audit-image va plus loin dans les relations entre les métadonnées, le namespace et les blocs alloués, mais suit le chemin writable et peut déclencher une récupération.

Le format évolue encore. Ne considère pas les images de développement comme un format stable garanti pour les versions futures.
