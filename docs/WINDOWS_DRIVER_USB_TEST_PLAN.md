# Plan de test matériel — pilote Windows OpenFS

## Support de test identifié

Support réservé par l'utilisateur :

- Modèle : SanDisk USB 3.2 Gen 1
- Capacité rapportée par Windows : 123 009 761 280 octets (~123 Go)
- Numéro de série : `03020915081126092611`
- Numéro de disque observé : `2` (ce numéro peut changer après reconnexion)
- État rapporté après suppression des partitions : `Online`, GPT, aucune partition retournée par `Get-Partition`.

**Ne jamais se fier uniquement au numéro de disque.** Avant toute commande ciblant le support, revérifier modèle, capacité et numéro de série. Les disques système observés pendant la préparation étaient le WD Blue SN5000 4 To (disque 0) et le Predator SSD GM7 1 To (disque 1) ; ne jamais leur envoyer de commande de modification.

## État de préparation

- [x] Identifier le support USB.
- [x] Confirmer l'absence de partitions.
- [x] Confirmer que le pilote compile en WDK x64 sur CI.
- [ ] Construire un package d'installation complet (INF, SYS, catalogue).
- [ ] Définir une signature de test et une VM Windows isolée.
- [ ] Charger le pilote dans la VM et vérifier l'enregistrement du système de fichiers.
- [ ] Passer un périphérique/volume non OpenFS et confirmer un refus propre.
- [ ] Créer un volume OpenFS valide avec le formateur officiel, après validation indépendante du formateur.
- [ ] Tester le montage en lecture seule, les métadonnées, l'énumération et la lecture de fichiers.
- [ ] Tester le retrait du périphérique et les erreurs d'E/S.
- [ ] Après validation VM, préparer seulement alors un essai USB sur le Windows hôte.

## Pourquoi aucun test de montage immédiat sur la clé vide

Le gestionnaire de fichiers monte des volumes/partitions, pas un disque USB simplement parce qu'il est présent. La clé ne contient actuellement aucune partition. De plus, le pilote lit le superbloc OpenFS au début du volume ciblé et vérifie le superbloc de sauvegarde, le bitmap et l'inventaire. Une clé vide ne peut donc pas fournir un volume OpenFS valide.

La création d'une partition seule ne rendrait pas le système de fichiers OpenFS : il faut un formateur qui écrive correctement toutes les structures et leurs CRC. Le pilote natif est actuellement en lecture seule et ne formate pas.

## Sécurité et signature

Le build CI actuel est un binaire de développement non signé. Ne pas exécuter `sc start`, `pnputil /add-driver` ou une installation manuelle sur le PC principal à ce stade. Ne pas désactiver Secure Boot et ne pas activer `TESTSIGNING` sur la machine principale.

La première validation doit être réalisée dans une VM Windows dédiée, avec instantané et signature de test explicitement installée dans cette VM. L'accès USB à la VM doit être configuré après vérification du package et du fonctionnement du pilote dans l'environnement isolé.

## Première collecte non destructive sur le PC hôte

Les commandes suivantes sont uniquement informatives :

```powershell
Get-Disk | Format-Table Number,FriendlyName,SerialNumber,BusType,Size,PartitionStyle,OperationalStatus
Get-Partition -DiskNumber 2
```

Remplacer le numéro 2 seulement après avoir revérifié le numéro de série et le modèle. Une erreur « aucun objet MSFT_Partition » est attendue si le disque ne possède aucune partition.

Aucune commande `Clear-Disk`, `Initialize-Disk`, `Format-Volume`, `clean` ou écriture brute n'est autorisée dans cette phase.
