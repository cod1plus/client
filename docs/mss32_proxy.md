# Proxy `mss32.dll` pour CoD1

## Objectif

Le but du proxy `mss32.dll` est de faire charger automatiquement notre code par
`CoDMP.exe`, sans injection manuelle.

Le jeu charge deja `mss32.dll` depuis son dossier racine. On exploite donc ce
chargement natif pour :

- lancer notre logique de patch CoD1 au demarrage
- continuer a fournir les fonctions audio attendues par le jeu
- rendre l'installation plus "plug&play" pour les joueurs

## Principe retenu

Le systeme repose sur 2 DLL :

- `mss32.dll` : notre proxy custom, charge automatiquement par le jeu
- `mss32_original.dll` : la vraie DLL audio d'origine, renommee

Flux de chargement :

1. `CoDMP.exe` demarre
2. Windows charge `mss32.dll` dans le dossier du jeu
3. Notre code s'execute
4. Les exports de `mss32.dll` sont rediriges vers `mss32_original.dll`
5. En parallele, notre logique attend `gamex86.dll` puis applique le patch

## Ce qu'on a verifie

On a compare la DLL `mss32.dll` de CoD1 avec la base `cod2x` deja presente dans
le repo.

Constats utiles :

- la `mss32.dll` CoD1 exporte `364` entrees
- la base `cod2x` expose elle aussi `364` exports dans son `mss32.def`
- pour un prototype, on peut donc reutiliser la liste d'exports `cod2x` pour
  generer un proxy CoD1 sans re-faire toute la couche audio

## Fichiers ajoutes ou modifies

### Build principal

- `CMakeLists.txt`
  - ajout d'une nouvelle cible `mss32`
  - reutilisation des memes sources que `cod1reloaded.dll`
  - sortie du binaire proxy dans `build/mss32.dll`

- `build_cod1.ps1`
  - detection de `cmake.exe`, `g++.exe` et `mingw32-make.exe`
  - ajout du bon `PATH` MinGW pour que `cmake` fonctionne proprement sous
    PowerShell
  - build des 2 artefacts :
    - `build/cod1reloaded.dll`
    - `build/mss32.dll`

### Proxy `mss32`

- `src/mss32_proxy.def`
  - fichier `.def` de proxy
  - chaque export est forwarde vers `mss32_original.<nom_export>`
  - exemple :
    - `_AIL_startup@0=mss32_original._AIL_startup@0`

### Logique de patch reutilisee

- `src/main.cpp`
  - garde la logique de watcher
  - attend `gamex86.dll`
  - applique le patch quand la DLL du jeu est disponible

- `src/patches.cpp`
  - patch de la constante `viewheight lerp speed`
  - verification de coherence avant ecriture

- `src/patches.h`
  - constantes du patch CoD1

## Correction faite pendant le build

Le premier build MinGW a revele un probleme dans `src/main.cpp` :

- la lambda passee a `CreateThread` n'avait pas la bonne convention d'appel
  attendue par MinGW

Correction appliquee :

- remplacement de la lambda par une vraie fonction `WINAPI`

Cette correction etait necessaire pour produire un proxy `mss32.dll` buildable
en 32-bit avec MinGW.

## Comment le proxy est genere

Pour aller vite sur le prototype, la liste d'exports a ete derivee depuis
`cod2x/src/mss32/mss32.def`.

Strategie retenue :

1. reprendre la liste des 364 exports connus
2. generer un `.def` minimal qui forwarde vers `mss32_original.dll`
3. lier une DLL `mss32.dll` qui exporte ces forwarders
4. y embarquer aussi notre code de patch CoD1

Resultat :

- `build/mss32.dll` compile
- les exports sont bien presents
- `objdump` confirme que les exports sont des `Forwarder RVA` vers
  `mss32_original.*`

## Build du proxy

Commande recommande :

```powershell
powershell -ExecutionPolicy Bypass -File .\build_cod1.ps1 -Clean
```

Resultat attendu :

- `build/cod1reloaded.dll`
- `build/mss32.dll`

## Procedure de test dans le dossier du jeu

1. Faire une sauvegarde de la DLL audio d'origine `mss32.dll`
2. Renommer l'originale en `mss32_original.dll`
3. Copier `build\mss32.dll` dans le dossier de `CoDMP.exe`
4. Lancer `CoDMP.exe` normalement
5. Charger une map ou rejoindre un serveur pour forcer le chargement de
   `gamex86.dll`

Si tout se passe bien :

- le jeu demarre via notre `mss32.dll`
- le son continue de fonctionner via `mss32_original.dll`
- notre patch CoD1 s'applique automatiquement

## Verification du patch

Au runtime, la logique actuelle fait :

- attente de `gamex86.dll`
- lecture de la constante cible
- verification que la valeur vaut bien `180.0f`
- remplacement par `100.0f`

En cas de mismatch :

- une popup d'erreur s'affiche
- le patch est annule

En cas de succes :

- un message debug est ecrit via `OutputDebugStringA`

## Ce qui a deja ete valide

- build 32-bit local fonctionnel avec MSYS2 / MinGW
- `cod1reloaded.dll` compile
- `mss32.dll` compile
- le proxy contient bien des forwarders vers `mss32_original.dll`
- le jeu a deja ete observe en demarrage avec le proxy

## Ce qu'il reste a valider proprement

- verification ingame que le son reste 100% stable
- verification que tous les imports utilises par CoD1 passent bien avec ce
  forwarding
- verification visuelle du fix `crouch / standup`
- eventuelle gestion de cas limites si certaines installations importent
  differemment `mss32.dll`

## Risques connus

- si `mss32_original.dll` manque ou est mauvaise, le jeu peut perdre le son ou
  planter
- si CoD1 attend un comportement plus specifique sur certains exports, il
  faudra peut-etre raffiner le proxy
- le prototype repose sur l'hypothese que la table d'exports CoD1 est
  suffisamment compatible avec celle de `cod2x`

## Etat actuel

Etat du proxy :

- suffisant pour un test reel
- pas encore considere comme release finale "joueurs"
- bonne base pour une version plug&play plus propre
