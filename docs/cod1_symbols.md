# CoD1 — Adresses identifiees par RE

Reverse engineering des DLLs CoD1 pour porter les fonctionnalites CoD2x.

> Pour le lean fix complet (cible de port cod2x principale), voir
> [`lean_fix_reference.md`](./lean_fix_reference.md) : RVAs, layouts,
> conventions de signe, knobs. Ce fichier-ci reste une reference generale
> multi-DLL (gamex86 + cgame).

---

## `gamex86.dll` (base `0x20000000`)

### Exports
| Adresse | Nom |
|---------|-----|
| `0x20042730` | `dllEntry` |
| `0x2001cd70` | `vmMain` (dispatcher principal, switch 25 cases) |

### Fonctions identifiées
| Adresse | Nom | Source |
|---------|-----|--------|
| `0x20012430` | `ClientThink_real` | Référence `pmove_msec`, appelle Pmove |
| `0x20008580` | `Pmove` | Wrapper qui loop sur `PmoveSingle` (max 66ms par tick) |
| `0x20007fd0` | `PmoveSingle` | Tick unique — appelle PM_GroundTrace, PM_UpdateViewHeight, etc. |
| `0x20004f30` | `PM_GroundTrace` | Trace au sol (strings `"i:kickoff"`, `"i:steep"`, `"i:Land"`) |
| `0x20005a80` | `PM_UpdateViewHeight` | Lerp smooth du view height — utilise offsets `+0xbc`/`+0xc0` |
| `0x200051c0` | `PM_CheckStance` / clearance | Traces verticales pour valider stand-up (flag à `+0x101`) |
| `0x20073f80` | Post-Pmove (cleanup/anim) | Appelée à la fin de `Pmove` |
| `0x20017ff0` | `ClientCommand` | Parse "give", "noclip", "kill", "dropweapon"... |
| `0x2001ed30` | `ClientBegin` (wrapper) | Setup viewmodel + tail call vers `fcn.2001fb10` |

### Système de view height (déjà présent dans CoD1)

CoD1 a un système de lerp pour le view height. Offsets dans la struct serveur joueur (depuis `[edi]` qui est `pmove->ps`) :

| Offset | Rôle |
|--------|------|
| `+0xbc` | viewheight target |
| `+0xc0` | viewheight current (lerpé) |
| `+0xc4` | état du lerp (0 = stable, !=0 = en transition) |
| `+0xc8` | viewheight de départ du lerp |
| `+0xcc` | type/direction du lerp |
| `+0xd0` | valeur initiale stockée |
| `+0x320` | duck transition time (?) |
| `+0x324` | standing height (60 par défaut, lu depuis `bg_viewheight_standing`) |
| `+0x328` | crouched height (40) |
| `+0x32c` | prone height (11) |
| `+0x330` | default/last viewheight |

### Vitesse du lerp viewheight

Adresse : `0x2009a1b0` (data section, .rdata)
Valeur actuelle : `180.0f` (= `0x43340000` little-endian = `00 00 34 43`)
Utilisation : `fmul dword [0x2009a1b0]` dans `PM_UpdateViewHeight` à `0x2000557b`

Formule du lerp :
```
ps->viewheight_current += pml.frametime * 180.0f
```

### Analyse du bug "body visible during crouch/standup"

Avec une vitesse de 180 unités/sec et un delta de 20 (stand 60 → crouch 40) :

| Phase | Durée |
|-------|-------|
| Viewheight lerp (stand → crouch) | 20 / 180 = 111 ms |
| Animation transition (`BG_SetNewAnimation` constante `0xc8`) | 200 ms |
| Fenêtre de désync | ~89 ms |

Pendant ces ~89 ms, la caméra est déjà arrivée à hauteur basse mais le modèle 3D du corps continue son animation de crouch → le corps devient visible depuis la caméra qui regarde vers le bas.

### Fix proposé

Aligner la vitesse du lerp sur la durée de l'animation : 100.0f unités/sec
- 100.0f en hex little-endian : `00 00 c8 42` (= `0x42c80000`)
- Nouveau timing : 20 / 100 = 200 ms, synchro avec l'anim

### Approche d'implémentation (hook DLL)

Suivre le modèle CoD2x : injection via `mss32.dll` shim, puis patch mémoire au runtime.

Pas un patch direct du binaire : on hook depuis notre DLL chargée par le jeu.

```cpp
// Dans le DLL cod1reloaded chargé par CoD1.exe
void patch_viewheight_lerp_speed() {
    HMODULE game = GetModuleHandle("gamex86.dll");
    if (!game) return;

    // Calculer l'adresse runtime (la DLL peut être relocalisée)
    uintptr_t base = (uintptr_t)game;
    uintptr_t target = base + (0x2009a1b0 - 0x20000000);

    // Patcher la valeur float (180.0f → 100.0f)
    DWORD oldProtect;
    VirtualProtect((void*)target, 4, PAGE_READWRITE, &oldProtect);
    *(float*)target = 100.0f;
    VirtualProtect((void*)target, 4, oldProtect, &oldProtect);
}
```

À appeler après le chargement de `gamex86.dll`, dans une fonction d'initialisation du shim.

### À investiguer ensuite

- [ ] Vérifier si `0x2009a1b0` est aussi présent dans `cgamex86.dll` (lerp côté client pour la prediction)
- [ ] Identifier la constante `0xc8` (200ms) dans `BG_SetNewAnimation` côté client pour ajustement si besoin
- [ ] Trouver les valeurs `bg_prone2duck_time` et `bg_duck2prone_time` pour transitions prone→crouch
- [ ] Tester l'impact en jeu et ajuster la valeur cible (100.0f est un point de départ)

### Struct `pmove_t` (offsets identifiés)
- `+0x00` : `ps*` (playerState pointer)
- `+0x04` : `cmd_serverTime`
- `+0x08`/`+0x09` : cmd buttons/wbuttons
- `+0x18`-`+0x1a` : autres champs cmd
- `+0x108` : `pmove_msec` (passé par `ClientThink`)
- `+0x10c` : `pmove_msec` cap

### Constantes Pmove
- `0x3e8` (1000) — frame time max
- `0x42` (66) — pmove_msec cap (66ms par sub-tick)
- `0x32` (50) — clamping threshold

### vmMain — mapping des cases
```
case 0  → fcn.2001e110  G_InitGame (4 args)
case 1  → fcn.2001e4d0  G_ShutdownGame
case 2  → fcn.20014800  ClientConnect
case 3  → fcn.200148d0  ClientBegin?
case 4  → fcn.20014ca0  ClientUserInfoChanged?
case 5  → fcn.20017ff0  ClientCommand ✓
case 6  → fcn.200129c0  ClientDisconnect?
case 7  → fcn.2001ed30  → fcn.2001fb10 (ClientThink wrapper)
case 8  → fcn.2001ee00  G_RunFrame? (2 args)
case 9  → fcn.2002c1b0
```

### Dvars view height (table à `0x200a54ec`, entrées de 24 bytes)
| dvar | name string | dvar_t* |
|------|------------|---------|
| `bg_viewheight_standing` | `0x20095978` | `0x201278a0` |
| `bg_viewheight_crouched` | `0x2009595c` | `0x20126cc0` |
| `bg_viewheight_prone` | `0x20095944` | `0x20127660` |

Structure d'une entrée de table de dvars (24 bytes) :
```c
struct dvar_table_entry {
    char*    name;          // +0x00
    char*    default_val;   // +0x04
    uint32_t flags;         // +0x08  (0x200 pour ces dvars)
    uint32_t unknown1;      // +0x0C  (0)
    uint32_t unknown2;      // +0x10  (0)
    void*    dvar_ptr;      // +0x14
};
```

### Autres dvars liés au mouvement
- `bg_ladder_yawcap` (`0x20095900`)
- `bg_prone2duck_time` (`0x20095914`)
- `bg_duck2prone_time` (`0x2009592c`)

### Offsets dans la structure joueur (depuis `[ebx + 0x104]`)
- `+0x73e0` : `pm_type` (== 2 quand alive normal)
- `+0x73e4` : commandTime / serverTime
- `+0x7444` : flag PMF (probablement DUCKED)
- `+0x7448` : flag PMF (probablement PRONE)
- `+0x744c` : autre flag PMF

### Constantes confirmées
- `0xc8` (200) = `PLAYER_CROUCH_TIME`
- `0x190` (400) = `PLAYER_PRONE_TIME`

---

## `cgame_mp_x86.dll` (base `0x30000000`)

### Fonctions identifiees
| Adresse | Nom | Source / verifie |
|---------|-----|--------|
| `0x300051c0` | `CG_Player_DoControllers` | Loop sur les 6 bones, syscalls `0xaf`/`0xa0`/`0xa1` |
| `0x30004960` | `BG_Player_DoControllersInternal` | Calcule les angles des 6 bones - `pdf` complet |
| `0x30034180` | `CG_OffsetFirstPersonView` (STAND) | Camera path local player stand - `pdf` complet |
| `0x30038300` | `CG_OffsetFirstPersonView` (CROUCH/PRONE ?) | 2e reader du leanf global - a `pdf` |
| `0x30040df0` | `BG_AddLeanToPosition` | Applique l'offset lateral leanf - `pdf` complet, 5 call sites |
| `0x3003db10` | `GetLeanFraction` (scale lerpLean) | Appelle depuis DoControllersInternal a `0x300049c5` |
| `0x3003da20` | `LerpAngle` | Appele 5+ fois dans DoControllersInternal |
| `0x30003870` | `BG_SetNewAnimation` | Contient `0xc8`/`0x190` (crouch/prone times) |
| `0x30028680` | Render attach @ `Bip01 Head` | Lit position du bone tete pour effets/UI |
| `0x3001e6e0` | DoControllers dispatcher | Check entity type + clientInfo lookup |
| `0x3001e7e0` | DoControllers caller wrapper | Syscalls `0xa3`/`0xa9`/`0xab`/`0x98` + dispatch |
| `0x3001c670` | Inconnue (utilise `tag_origin`) | A investiguer |
| `0x30005590` | Inconnue (utilise `torso`) | A investiguer |

### Globals data (cgame, base 0x30000000)
| Adresse | Nom | Type | Notes |
|---|---|---|---|
| `0x3020af00` | `playerstate.commandtime` | int32 | base du playerstate local |
| `0x3020af14` | `playerstate.origin` | vec3 | |
| `0x3020af28/2c` | `origin.x/y` (alias) | float | verifie via diag mvmt dump |
| `0x3020af54` | `leanf` | float | leanf signe ±0.5 (normalise = `*2`). Read-only |
| `0x3020af94` | `eFlags` | uint32 | bits 0x40, 0x4000, 0x8000 |
| `0x3020d348/4c/50` | view origin | vec3 | camera position finale |
| `0x3020d380/84/88` | view angles | vec3 | [pitch, yaw, roll] |

### Constantes float identifiees (cgame .rdata)
| Adresse | Valeur | Role |
|---|---|---|
| `0x3006b58c` | 1.0f | unit |
| `0x3006b594` | 0.0f | compare zero |
| `0x3006b608` | pi/180 (DEG2RAD) | angle conversion |
| `0x3006b648` | -1.0f | sign flip |

### Layout de la struct des controllers (sortie de `fcn.30004960`)

Corrige 2026-05-28 : ordre verifie via `pdf @ fcn.30004960` aux writes
`0x30004f8f-0x3000502b`. Le doc original avait l'ordre inverse - bug
historique vu dans le commit lean_fix.h:35-44.

Le pointeur `edi` passe en sortie contient 8 vec3 consecutifs (96 bytes) :

| Offset | Champ | Notes |
|--------|-------|-------|
| `+0x00` | `back_low_angles[3]` | cible lean fix (pitch, yaw, roll) |
| `+0x0c` | `back_mid_angles[3]` | cible lean fix |
| `+0x18` | `back_up_angles[3]` | cible lean fix |
| `+0x24` | `neck_angles[3]` | neck[2] hardcode a 0 par engine |
| `+0x30` | `head_angles[3]` | |
| `+0x3c` | `pelvis_angles[3]` | pelvis[1] et pelvis[2] hardcodes a 0 |
| `+0x48` | `tag_origin_angles[3]` | root orientation (pitch, yaw, roll) |
| `+0x54` | `tag_origin_offset[3]` | root position (x, y, z) - cible body_shift |

Quand stocke dans le `clientInfo_t` (1200 bytes / 0x4b0), ces angles se trouvent a partir de `+0x3fc`.

Voir `docs/lean_fix_reference.md` pour le doc maitre du port cod2x.

### Offsets du `clientInfo_t` CoD1 (1200 bytes, indexé à `data.3018dc8c`)
| Offset | Champ supposé |
|--------|---------------|
| `+0x008` | `eFlags` (test `0xc0` haut = CROUCH/PRONE flags) |
| `+0x090` | clientNum ou similaire |
| `+0x380` | angle (yaw?) |
| `+0x3b0` | angle |
| `+0x3b8` | `lerpLean` (utilisé avec `GetLeanFraction` = `fcn.3003db10`) |
| `+0x3e4` | pitch (avec masque `0x7fffffff` = `fabs`) |
| `+0x3e8` / `+0x3ec` | angles head/torso |
| `+0x474` | flags (test `0x30000`) |
| `+0x3fc` → `+0x444` | angles des 6 bones (output de DoControllersInternal) |
| `+0x444+` | `tag_origin_offset` |

### Plan d'implementation du lean fix

Implemente dans `src/lean_fix.cpp` (hook installe a `cgame+0x51d8`).
Pour le detail complet voir `docs/lean_fix_reference.md`.

Notes critiques :
- Bone offsets aux angles : `back_low=+0x00`, `back_mid=+0x0c`, `back_up=+0x18`
  (pas l'ordre inverse comme indique historiquement)
- `eFlags & 0x40` = bit "is_leaning" fiable (gate recommande)
- `eFlags & 0xC000` (test ah,0xc0) = (CROUCH 0x4000 | PRONE 0x8000)
- En STAND, l'engine zero le buffer entier - additions absolues uniquement
- `BG_AddLeanToPosition` @ `cgame+0x40df0` est appele par 5 call sites
  patches par `lean_amplify` (immediates 16.0 et 20.0)

### Tags exposés via `tag_*`
- `tag_origin` — racine
- `tag_weapon`, `tag_aim`, `tag_aim_animated` — arme
- `tag_weapon_right`, `tag_weapon_left` — dual wield
- `tag_brass` — éjection douilles
- `tag_flash`, `tag_flash_2`, `tag_flash_11`, `tag_flash_22` — muzzle flash
- `tag_player` — fixation tourelle

### Bones du squelette manipulables par le systeme de controllers

Decouvert via la table de pointeurs a `0x30079670` (cgame_mp_x86.dll) qui sert au loop de `CG_Player_DoControllers` (`fcn.300051c0`). Les 6 bones controllables :

| Adresse string | Nom CoD1 | Equivalent CoD2 |
|----------------|----------|-----------------|
| `0x3006b2bc` | `pelvis` | `tag_pelvis` |
| `0x3006b2c4` | `head` | (interne) |
| `0x3006b2cc` | `neck` | `tag_neck` |
| `0x3006b2d4` | `back_up` | `tag_back_up` |
| `0x3006b2dc` | `back_mid` | `tag_back_mid` |
| `0x3006b2e8` | `back_low` | `tag_back_low` |

(NB : `back_low` est a `0x3006b2e8`, pas `0x3006b2ec` comme indique historiquement -
verifie via `iz` sur cgame_mp_x86.dll.)

Les mêmes 6 bones existent côté CoD1, juste avec un naming différent (pas de préfixe `tag_`, underscores au lieu de camelCase), donc la philosophie du fix d'animation CoD2x (`BG_Player_DoControllersInternal`) est portable.

Le lean fix CoD2x (3 corrections dans `cod2x/src/shared/animation.cpp:330-365`) peut être adapté en remplaçant `tag_back_*_angles` par `back_*_angles` côté struct controllers CoD1.

---

## TODO

- [ ] Identifier l'adresse exacte de `PM_CheckDuck` à l'intérieur de `Pmove` (`0x20008580`)
- [ ] Identifier l'adresse exacte de `PM_ViewHeightAdjust`
- [ ] Trouver l'offset de `viewheight` dans la struct playerState
- [ ] Vérifier la présence d'un équivalent `viewHeightCurrent` / `viewHeightLerp*`
- [ ] Identifier `BG_Player_DoControllersInternal` dans `cgame_mp_x86.dll`
