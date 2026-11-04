# Intégration Windows et pilote natif

OpenFS a deux chemins Windows distincts. Il est important de ne pas les confondre.

## 1. Montage expérimental avec Dokany

Le programme Rust openfs-tool utilise Dokany pour exposer une image OpenFS comme lecteur Windows. C'est actuellement le chemin de développement pour tester les opérations depuis l'Explorateur et les applications Windows.

Il nécessite Windows et Dokany. Il n'est pas considéré comme prêt pour des données importantes ni pour un usage de production.

## 2. Pilote WDM natif en C

Le code est dans OpenFS/windows-driver/. Il s'agit d'un véritable pilote de système de fichiers noyau, pas d'un minifiltre.

### Ce qui existe

- Enregistrement du système de fichiers auprès du gestionnaire d'E/S Windows.
- Réception des demandes de détection/montage.
- Lecture en lecture seule des superblocs principal et secondaire.
- Validation de la géométrie, des révisions, des CRC et de plusieurs structures.
- État de volume privé, protection de durée de vie et bases de cache FCB/DCB.
- Tests C exécutables sur l'hôte pour plusieurs codecs et structures.

### Ce qui manque encore

Le pilote ne publie pas de volume monté : une sonde réussie se termine encore par STATUS_NOT_IMPLEMENTED. Il reste notamment à :

1. terminer la construction du VCB et du répertoire racine ;
2. connecter la résolution de chemins au cache FCB/DCB ;
3. publier le VPB uniquement lorsque le volume est complètement initialisé ;
4. gérer proprement démontage, vérification et erreurs ;
5. implémenter ouverture, informations, énumération et lecture ;
6. ajouter écriture, suppression, renommage, partage et synchronisation ;
7. intégrer les exigences Cache Manager, Memory Manager, sécurité et concurrence.

La présence d'une fonction ou d'une structure de préparation ne signifie pas que le parcours Windows correspondant fonctionne.

## Compiler le pilote

Prérequis : Visual Studio avec les outils C++, Windows SDK et WDK compatibles, outils x64.

Depuis une invite développeur WDK :

```powershell
msbuild .\OpenFSDriver.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

La compilation produit un pilote de développement non signé. Elle ne prouve ni qu'il peut être installé en sécurité, ni qu'il peut monter un volume. Une distribution nécessite un package et une signature adaptés, ainsi que des validations sur Windows réel.

## Règle de développement

Ne jamais retourner un succès de montage avant que le VCB, le VPB, le namespace racine, les dispatchs et le démontage soient cohérents. Un volume reconnu n'est pas un volume monté.
