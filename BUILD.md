# Build cod1reloaded

## Pré-requis

- **MinGW-w64 32-bit** (i686, pas x86_64)
  - Sur MSYS2 : `pacman -S mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-make`
  - Ou télécharger depuis [winlibs.com](https://winlibs.com/) la version i686-posix-dwarf
- **CMake ≥ 3.16**

## Compilation

Depuis le dossier racine du repo, dans un **shell MSYS2 MINGW32** :

```bash
cmake -S . -B build -G "MinGW Makefiles" \
    -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Le DLL est produit dans `build/cod1reloaded.dll`.
Un proxy test `build/mss32.dll` est aussi produit pour un chargement automatique
par le jeu.

### Build rapide sous PowerShell

Un helper est aussi disponible pour les setups Windows ou le toolchain n'est pas
dans le `PATH` :

```powershell
.\build_cod1.ps1
```

Si PowerShell bloque l'execution des scripts, utilise :

```powershell
powershell -ExecutionPolicy Bypass -File .\build_cod1.ps1
```

Le script cherche automatiquement un MinGW 32-bit dans :

- `.\tools\mingw\bin`
- `.\cod2x\tools\mingw\bin`
- `C:\msys64\mingw32\bin`
- `C:\mingw32\bin`

Options utiles :

```powershell
.\build_cod1.ps1 -Clean
.\build_cod1.ps1 -Configuration Debug
```

## Test du proxy `mss32.dll`

Prototype actuel :

- `build\mss32.dll` = proxy custom charge par le jeu
- `mss32_original.dll` = copie renommee de la DLL audio d'origine

Procedure de test dans le dossier du jeu :

1. Faire une sauvegarde de `mss32.dll`
2. Renommer la DLL d'origine en `mss32_original.dll`
3. Copier `build\mss32.dll` dans le dossier du jeu
4. Lancer `CoDMP.exe` normalement

Si le chargement du proxy fonctionne, notre code est execute automatiquement par
le jeu et continue ensuite a forwarder les exports vers `mss32_original.dll`.

## Test en jeu

Pour cette version initiale, le DLL doit être **injecté manuellement** dans `CoDMP.exe` après le lancement du jeu. Le proxy `mss32.dll` (chargement automatique) sera ajouté plus tard.

### Injection avec Process Hacker

1. Lancer CoD1 Multiplayer
2. Ouvrir Process Hacker, trouver `CoDMP.exe`
3. Clic droit > Miscellaneous > Inject DLL
4. Sélectionner `build/cod1reloaded.dll`

### Injection en ligne de commande

Avec un outil comme [Injector](https://github.com/nefarius/Injector) :

```cmd
injector.exe --process-name CoDMP.exe --inject build\cod1reloaded.dll
```

### Vérification

Le DLL polle pendant 60s à la recherche de `gamex86.dll`. Une fois trouvée :

- Vérification de cohérence : la valeur actuelle doit être `180.0f`. Si non, une popup d'erreur s'affiche et le patch est annulé (signe que les adresses ont changé — autre version de CoD1).
- Si OK : la valeur est remplacée par `100.0f` et un message debug est écrit (visible avec [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)).

Pour tester l'effet en jeu : crouch/standup rapide et observer si le corps "pop" toujours pendant la transition.

## Désinstallation

Fermer le jeu. Le DLL ne touche rien d'autre que la mémoire volatile.

## Limitations actuelles

- Pas de chargement automatique — injection manuelle requise
- Ciblé uniquement sur CoD1 Multiplayer (CoDMP.exe), version "1.5" présumée
- Une seule modification appliquée (viewheight lerp speed)
- Pas de désactivation à chaud
