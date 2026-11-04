# Sécurité, tests et limites

## Règle essentielle

**OpenFS est expérimental. N'y stocke pas de données importantes.** Utilise des images jetables et conserve des sauvegardes.

## Images et commandes de récupération

- check-image est le premier contrôle à utiliser : il ne modifie pas l'image.
- audit-image ouvre le chemin writable ; il peut rejouer des transactions validées avant l'audit.
- recover-image lance volontairement le chemin de récupération et peut modifier le volume.

Fais une copie de l'image avant toute opération de récupération ou d'audit si son état est incertain.

## Périphériques physiques et USB

Le code contient des chemins d'accès à des disques physiques Windows et de formatage GPT. Ces opérations peuvent effacer la table de partitions et les données du disque sélectionné.

Les essais physiques USB sont actuellement en pause. Ne lance pas les commandes de formatage ou de montage de disque pour tester cette branche. Une validation sur image ne prouve pas la compatibilité avec un contrôleur USB réel.

## Ce que les tests prouvent — et ne prouvent pas

Les tests couvrent les structures, les opérations prises en charge, les erreurs d'E/S simulées et certains scénarios de coupure/reprise. Ils ne prouvent pas :

- que tous les contrôleurs et firmwares respectent les demandes flush/FUA ;
- que toutes les opérations complexes sont atomiques face à toute panne ;
- que le pilote fonctionne sur toutes les versions et configurations Windows ;
- que le format est stable ou compatible avec NTFS/ReFS ;
- que le système est prêt pour la production.

## Avant toute contribution

- Valider toutes les données venant du disque.
- Utiliser des calculs vérifiés et refuser les plages hors limites.
- Ne jamais libérer un bloc tant que son absence de références n'est pas démontrée.
- Ajouter des tests de corruption, d'échec d'E/S et de réouverture.
- Ne déclarer un contrôle réussi que si la CI correspondante est réellement verte.
