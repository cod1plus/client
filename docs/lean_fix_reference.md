# Lean fix CoD1 — Reference pour port cod2x

Notes RE pour le lean fix : reproduire en CoD1 la fix anti-wallpeek de cod2x.

Sources verifiees :
- Cutter/rizin sur `cgame_mp_x86.dll` (base `0x30000000`)
- Source cod2x (`cod2x/src/shared/animation.cpp`)
- Code existant `src/lean_fix.cpp`, `src/lean_amplify.cpp`, `src/swing_fix.cpp`

---

## 1. Objectif fonctionnel

En vanilla CoD1 multi, le wallpeek exploit :
- Camera lean sort du cover → joueur voit l'ennemi
- Modele 3D / hitbox reste derriere le cover → ennemi ne tire pas
- Asymetrie LEFT/RIGHT (bug "rolls forward / head clips out of view" specifique au LEFT)

Cod2x a fix dans CoD2. On porte vers CoD1 en :
1. Decalant le body model dans la direction du lean (`body_shift`)
2. Forcant les bones du dos a pencher en avant en crouch+lean (`headclip_fix`)
3. Posant la pose "aimwalk" en lean+walk (`aimwalk_fix`)
4. Ajoutant du roll aux bones pour une courbure spinale visible (`lean_roll_amount`)
5. Amplifiant le shift camera/body (`lean_amplify`)
6. Snap legs/torso instant (`swing_fix`, port `BG_PlayerAngles`)

---

## 2. Architecture du lean dans CoD1

### Deux chemins paralleles, decouples

```
       playerstate.leanf @ 0x3020af54
       playerstate.eFlags (bit 0x40 = lean active)
       playerstate.viewangles
                       |
        +--------------+--------------+
        |                             |
   ANIM PATH                     CAMERA PATH
   (bones squelette)             (view origin)
        |                             |
   CG_Player_DoControllers        CG_OffsetFirstPersonView
   @ cgame+0x51c0                 @ cgame+0x34180 (STAND only)
        |                         @ cgame+0x38300 (CROUCH/PRONE ?)
   BG_Player_DoControllersInternal     |
   @ cgame+0x4960                  AddLeanToPosition
        |                          @ cgame+0x40df0
   controllers[8 vec3] buffer           |
        |                         applique offset lateral
   render bones                   au view_origin
```

Le decouplage est la cause du wallpeek vanilla : le camera path bouge sans que les bones du body suivent. Notre fix re-couple les deux.

---

## 3. Adresses & symboles (cgame_mp_x86.dll, base 0x30000000)

### Fonctions (par RVA)

| RVA | Nom probable | Role | Verifie |
|---|---|---|---|
| `0x4960` | `BG_Player_DoControllersInternal` | Calcule 6 vec3 d'angles + tag_origin pour les bones du squelette | `pdf` OK |
| `0x51c0` | `CG_Player_DoControllers` | Wrapper qui loop sur les bones, appelle DoControllersInternal | `pdf` partiel |
| `0x51d8` | (call site dans CG_Player_DoControllers) | `call BG_Player_DoControllersInternal` - point de hook | confirme |
| `0x34180` | `CG_OffsetFirstPersonView` (STAND only) | Calcule view origin + appelle AddLeanToPosition | `pdf` OK |
| `0x38300` | Idem (CROUCH/PRONE ?) | 2e reader de leanf global | TODO `pdf` |
| `0x40df0` | `BG_AddLeanToPosition` | Applique l'offset lateral au view origin | `pdf` OK |
| `0x3db10` | `GetLeanFraction` / scale lerpLean | Transforme lerpLean en un fLeanFrac normalise | TODO `pdf` |
| `0x3da20` | `LerpAngle` | Interp un angle entre 2 valeurs | TODO `pdf` |

### Globals (data section)

| Adresse | Nom | Type | Notes |
|---|---|---|---|
| `0x3020af54` | `cg.predictedPlayerState.leanf` | float | leanf signe ±0.5 (normalise [-1,1] = `*2`). 2 readers seulement, read-only depuis cgame |
| `0x3020af00` | `playerstate.commandtime` | int32 | base du playerstate |
| `0x3020af14` | `playerstate.origin` | vec3 | passe a fcn.30012f00 (sweep ?) |
| `0x3020af28` | `playerstate.origin.x` (alias) | float | confirme via diag mvmt dump |
| `0x3020af2c` | `playerstate.origin.y` (alias) | float | idem |
| `0x3020af30` | `playerstate.something` | float | cap viewheight ? |
| `0x3020af94` | `playerstate.eFlags` | uint32 | bit 0x40 = lean, 0x4000 = CROUCH, 0x8000 = PRONE |
| `0x3020afe4` | `playerstate.viewangles.roll` ? | float | utilise dans CalcView |
| `0x3020d348/4c/50` | view origin [x,y,z] | vec3 | position camera apres tout offset |
| `0x3020d380/84/88` | view angles [pitch,yaw,roll] | vec3 | orientation camera |
| `0x3020d29c` | timing accumulator | int32 | -450ms correction |

### Constantes float (.rdata)

| Adresse | Valeur | Role |
|---|---|---|
| `0x3006b58c` | 1.0f | unit float (utilise dans `1.0 - x`) |
| `0x3006b588` | 1.0f ? | autre unit |
| `0x3006b594` | 0.0f | compare zero |
| `0x3006b608` | pi/180 (DEG2RAD) | `0x3C8EFA35` ≈ 0.01745329 |
| `0x3006b648` | -1.0f | sign flip |
| `0x3006b620` | constante de scale | a verifier |
| `0x3006b5e8` | diviseur du fsincos | a verifier |
| `0x3006b814/18` | facteurs de scale dans DoControllers | a verifier |
| `0x3006b6f8` | facteur ROLL ? | bone_low_roll multiplier |
| `0x3006b6fc` | offset viewheight cap | utilise dans CalcView |
| `0x3006b8d8/dc` | seuils de blend timing | morphing dans CalcView |

---

## 4. Layout structures

### Buffer `controllers[]` — sortie de `BG_Player_DoControllersInternal`

Confirme via les writes a 0x30004f8f-0x3000502b.

```c
struct controllers_buffer {  // 96 bytes (8 * vec3)
    float back_low[3];          // +0x00  (pitch, yaw, roll)
    float back_mid[3];          // +0x0c
    float back_up[3];           // +0x18
    float neck[3];              // +0x24  — neck[2] HARDCODE a 0 par l'engine
    float head[3];              // +0x30
    float pelvis[3];            // +0x3c  — pelvis[1] et pelvis[2] HARDCODES a 0
    float tag_origin_angles[3]; // +0x48  (pitch, yaw, roll - root orientation)
    float tag_origin_offset[3]; // +0x54  (x, y, z - root position)
};
```

### `clientInfo_t` (esi, 1200 bytes / 0x4b0)

| Offset | Champ | Source |
|---|---|---|
| `+0x380` | float (anim lerp ?) | passe en var_58h, copie en sortie |
| `+0x3b0` | float (ebp) | `fcn.30004988` |
| `+0x3b8` | `lerpLean` | confirme par xref leanf call et gate eFlags 0x40 |
| `+0x3e4` | float (avec masque `0x7fffffff` = abs) | facteur de blend `1.0 - |val|` |
| `+0x3e8` | float | arg de `fcn.3003da20` (LerpAngle) |
| `+0x3ec` | float | idem `fcn.3003da20` |
| `+0x474` | uint32, bit `0x30000` | flag "lean disabled" (skip si set) |

### Entity (ebx)

| Offset | Champ |
|---|---|
| `+0x08` | `eFlags` (uint32) |
| `+0x90` | float / int (copie en var_78h) |
| `+0xe4` | float (movement direction ?) |
| `+0xe8` | float (forward ?) |
| `+0xec` | float (right ?) |

### `eFlags` bits (entity+8 ET playerstate+0x94)

| Bit | Valeur | Sens |
|---|---|---|
| `0x40` | bit 6 | is_leaning (lean key active) |
| `0x4000` | bit 14 (ah=0x40) | CROUCH |
| `0x8000` | bit 15 (ah=0x80) | PRONE |

Test `test ah, 0xc0` = (CROUCH | PRONE).

---

## 5. Fonction `BG_Player_DoControllersInternal` (cgame+0x4960)

### Signature reconstituee

```c
// Convention custom :
//   eax = controllers buffer (out)
//   ecx = clientInfo*
//   ebx = entity*
void BG_Player_DoControllersInternal(/* eax/ecx/ebx */);
```

### Comportement

```c
void BG_Player_DoControllersInternal(controllers_t* out,
                                     clientInfo_t* ci,
                                     entity_t* ent)
{
    uint32_t eFlags = ent->eFlags;

    // 1. STAND : zero le buffer entier et return
    if (!(eFlags & 0xC000)) {        // ni CROUCH ni PRONE
        memset(out, 0, 96);          // 24 dwords = 8 vec3
        return;
    }

    // 2. Init avec valeurs par defaut
    float var_58h = ci[0x380];
    int   var_78h = ent[0x90];
    float ebp     = ci[0x3b0];
    uint  flag    = ci[0x474];

    // 3. Si flag.bit_0x30000 ou pas de lean → skip leanf transform
    if (!(flag & 0x30000) && (eFlags & 0x40)) {
        float lerpLean = ci[0x3b8];
        float scaled = GetLeanFraction(lerpLean);     // fcn.3003db10
        if (scaled >= 0) scaled *= K_right;            // 0x3006b77c
        else             scaled *= K_left;             // 0x3006b590
        // ... store in var_8ch
    }

    // 4. Lerp tous les angles
    // (multiples appels a fcn.3003da20 = LerpAngle)

    // 5. Calcul "rotation 2D" interne (fsincos a 0x30004b25)
    //    sur var_94h = (lerpLean_scaled * 0x3006b620) / 0x3006b5e8
    //    Probablement EQUIVALENT du animation_adjust_rotation cod2x

    // 6. Branchement selon data.301e49ac (game state ?)
    //    cas 4, cas 6, sinon : 3 paths differents pour pelvis[0]

    // 7. Write final dans buffer (ordre : back_low → tag_origin_offset)
    out->back_low[0]   = var_50h;
    out->back_low[1]   = var_4ch;
    out->back_low[2]   = var_48h;
    out->back_mid[0]   = var_44h;
    out->back_mid[1]   = ebp;
    out->back_mid[2]   = esi;
    // ... etc
    out->neck[2]       = 0;                            // HARDCODE
    out->pelvis[1]     = 0;                            // HARDCODE
    out->pelvis[2]     = 0;                            // HARDCODE
}
```

### Implications pour le patcher

1. En STAND, tout le buffer sort a 0 → nos additions sont en absolu, pas en relatif.
2. Pelvis yaw/roll et neck roll sont ecrases a 0 → patcher leur scale est no-op. A virer du config.
3. L'engine fait deja une rotation 2D interne (fsincos a 0x4b25) sur back_low. Notre `animation_adjust_rotation` cod2x port peut etre en doublon. A confirmer en lisant les constantes.
4. `eFlags & 0x40` = gate fiable "joueur en lean". Plus fiable que les heuristiques `sum_back_yaw` actuelles.

---

## 6. Fonction `BG_AddLeanToPosition` (cgame+0x40df0)

### Signature reconstituee

```c
// Convention : edx = origin (fastcall arg 1), reste sur stack
void BG_AddLeanToPosition(
    vec3_t* origin,     // edx
    float view_yaw,     // [esp+4]  (degres)
    float leanf,        // [esp+8]
    float lean_width,   // [esp+0xc]  (16.0 en STAND, ~12.5 en CROUCH ?)
    float lean_forward  // [esp+0x10] (20.0 en STAND, ~2.5 en CROUCH ?)
);
```

### Comportement

```c
void BG_AddLeanToPosition(vec3_t* origin, float yaw, float leanf, float w, float f)
{
    if (leanf == 0.0f) return;     // early exit

    // Damping factor base sur un autre arg (probablement view_pitch reduit dans this caller a 0)
    float damping = 1.0f - fabsf(SOMETHING);

    // Calculs intermediaires
    float lateral = damping * leanf * w;     // ≈ leanf * 16.0 quand damping=1
    float forward = leanf * f;               // ≈ leanf * 20.0

    // 3 fsincos consecutifs : sin/cos de yaw, sin/cos de 0 (pitch ignore en STAND),
    // sin/cos de yaw a nouveau (sur un autre arg ?)

    // Vec (lateral, forward) tourne autour de Z par yaw, ajoute a origin
    origin[0] += rotated_x;
    origin[1] += rotated_y;
    origin[2] += rotated_z;
}
```

### Convention de signe

`leanf` est utilise sans flip de signe. Donc :
- Sign convention identique a cod2x (a confirmer via test in-game : voir code `lean_fix.cpp:582` pour le diag log existant)
- Hypothese : `leanf < 0` = LEFT (convention cod2x classique)

### Implications

1. 5 call sites identifies (voir `lean_amplify.h:37-43`) :
   - `0xbaea/0xbaf5` — site 1 (probablement camera 3rd person ?)
   - `0x1479d/0x147b3` — site 2
   - `0x34473/0x34478` — site 3 = inside `fcn.30034180` STAND camera (verifie)
   - `0x38776/0x3878c` — site 4 = probablement inside `fcn.30038300` CROUCH camera
   - `0x3abc9/0x3abdd` — site 5
2. Seuls les sites 3 et 4 lisent le leanf global `0x3020af54` (= local player). Les autres recoivent leanf en parametre = clients distants (3rd person rendering).
3. lean_amplify patche les immediates 16.0 → 16.0\*factor et 20.0 → 20.0\*factor aux 5 sites. Idempotent, sanity check sur opcode 0x68.
4. Pas de patch des constantes CROUCH (12.5/2.5 inconnues actuellement) - lean_amplify ne touche que les sites a 16.0/20.0.

---

## 7. Fonction `CG_OffsetFirstPersonView` STAND (cgame+0x34180)

### Comportement

```c
void CG_OffsetFirstPersonView_Stand(void)
{
    // Tres tot : gate sur cg.snap.ps.pm_type ([data.301e5f24] + 0x10)
    if (gamestate == 5) return;   // dead/spectator probablement

    // Accumule view bob dans data.3020d380/84/88 (view angles)
    // Accumule view origin diff dans data.3020d348/4c/50

    // Gate CROUCH/PRONE
    if (eFlags & 0xC000) return;  // CROUCH ou PRONE → autre path (fcn.30038300)

    // Compute right vector (Quake : forward/right/up depuis view angles)
    // 3x fsincos sur pitch, yaw, roll

    // Apply right vector * scalar_from_fcn.30012f80 to origin

    // Timing/bob corrections

    // ★ THE ACTUAL LEAN CALL
    AddLeanToPosition(
        &view_origin[0x3020d348],
        view_yaw[0x3020d384],
        leanf[0x3020af54],
        16.0f,    // lean_width STAND
        20.0f     // lean_forward STAND
    );

    // Final cap on origin Z
}
```

### Implications

- Cette fonction ne tourne qu'en STAND. La fonction `fcn.30038300` (a inspecter) gere CROUCH/PRONE.
- Le camera path est isole du bone path. Modifier les bones ne change pas la camera, et vice-versa. C'est pour ca qu'on a besoin de `body_shift` et `lean_amplify` separement.

---

## 8. Code existant cod1reloaded (etat actuel)

### `lean_fix.cpp` — hook DoControllers

- Hook trampoline a `cgame+0x51d8` (patch du call rel32, opcode E8 garde)
- Wraps `fcn.30004960`
- Apres call original, modifie le buffer `controllers[]` :
  - Damping scales par bone (pitch/yaw/roll, gauche/droite overrides)
  - Yaw offsets additifs (`left_*_yaw_offset`, `right_*_yaw_offset`)
  - v14 lean roll : ajoute roll aux back bones pour courbure visible
  - v15 aimwalk : forward pitch quand lean + walk (detecte via origin delta)
  - Headclip fix : pitch bend forward en crouch+lean (cod2x port)
  - Body shift : decale `tag_origin_offset[1]` pour exposer le body
  - Aimwalk fix v2.1 : tilts `tag_origin_angles` quand lean+walk diagonal-left
  - Diagonal rotation fix : applique `animation_adjust_rotation` a back_low (cod2x animation.cpp:100-115 port)
- Tous les knobs sont dans `cod1reloaded.ini`, hot-tunable
- Diag logger via `logger::logf` capped a `diag_log_count`

### `swing_fix.cpp` — port `BG_PlayerAngles`

- Patche les immediates dans `BG_PlayerAngles` pour snap instant legs/torso
- Independant du lean fix

### `lean_amplify.cpp` — patche immediates 16.0/20.0

- 5 sites identifies, factor par defaut 1.5x
- Sanity check via opcode 0x68
- Bornes safety [0.5, 2.5]

### `widescreen_fix.cpp` — Hor+ FOV

- Independant des autres fixes

### Pipeline d'init (`patches.cpp` → `apply_to_cgame`)

1. `widescreen_apply_to_cgame`
2. `apply_swing_fix`
3. `apply_lean_amplify`
4. `install_lean_fix` (sauf si `lean_fix_enable=false`)

---

## 9. Map cod2x → cod1 (ce qu'on a porte, ce qui manque)

### Source cod2x (`cod2x/src/shared/animation.cpp`)

| cod2x lignes | Fonction | Statut port CoD1 |
|---|---|---|
| `100-115` | `animation_adjust_rotation` (rotate back_low (pitch,roll) par yaw_diff) | porte dans `lean_fix.cpp:35-48` |
| `184-200` | Body sideways shift (tag_origin_offset[1] += ...) | porte dans `lean_fix.cpp:888-923` |
| `332-348` | Aimwalk tilt (tag_origin_angles += pitch/roll) | porte dans `lean_fix.cpp:700-852` (avec smoothing v2.1) |
| `351-357` | Headclip fix (crouch+lean → back bones pitch forward) | porte dans `lean_fix.cpp:854-869` |
| `359-364` | Diagonal rotation fix (call `animation_adjust_rotation` sur back_low) | porte dans `lean_fix.cpp:955-962` |

### Knobs vanilla cod2x vs cod1reloaded defaults

| cod2x default | cod1reloaded default | Notes |
|---|---|---|
| body_shift stand_left = 5.0 | 30.0 | Bumpe car camera offset CoD1 ~14 units |
| body_shift stand_right = 2.5 | 0.0 | User a desactive RIGHT |
| body_shift crouch_left = 12.5 | 28.0 | Idem |
| aimwalk_stand_pitch = 7.2 | 7.2 | Match exact |
| aimwalk_stand_roll = 7.2 | 7.2 | Match exact |
| aimwalk_crouch_pitch = 3.8 | 3.8 | Match exact |
| aimwalk_crouch_roll = 3.8 | 3.8 | Match exact |
| headclip back_low_pitch = 40 | 40.0 | Match exact |
| headclip back_low_yaw = 30 | 30.0 | Match exact |
| headclip back_mid_pitch = -20 | -20.0 | Match exact |
| headclip back_up_pitch = -20 | -20.0 | Match exact |
| roll back_low = N/A | 8.0 | v14 nouveau (cod2x n'a pas ce knob) |
| roll back_mid = N/A | 13.0 | v14 |
| roll back_up = N/A | 18.0 | v14 |
| lean_amplify factor = N/A | 1.5x | Nouveau cod1 (patche les immediates) |

### Knobs specifiques CoD1 (anti-bug)

Ces knobs n'existent pas dans cod2x — adresses des bugs decouverts in-game :

- `left_*_pitch_scale` : overrides asymetriques (LEFT lean roll forward bug)
- `left_*_yaw_offset` / `right_*_yaw_offset` : visible peek shift
- `aimwalk_smooth_rate` : smoothing avec QPC sub-ms (engine snap 0→50 deg en 1 frame)
- `lean_aimwalk_v15_*` : forward bend quand lean+walk detecte via origin delta
- `crouch_*_offset` / `crouch_origin_z_offset` : crouch posture (no-op default)

---

## 10. Convention de signe leanf (a confirmer in-game)

### Hypothese de travail

`leanf < 0` = LEFT lean (convention cod2x).

### Indices supportant l'hypothese

1. `BG_AddLeanToPosition` utilise `leanf` directement sans flip → mecanique identique a cod2x
2. Le code `lean_fix.cpp:582` traite `real_leanf < -0.05f` comme `is_leaning_left` (deja teste en jeu, suppose valide)

### Comment confirmer (deja en place dans le code)

Code dans `lean_fix.cpp:586-596` log `real_leanf` et la direction detectee. Player presse LEFT, regarde le log :
- Si `real_leanf < 0` quand lean LEFT visuel → hypothese OK
- Si `real_leanf > 0` quand lean LEFT visuel → inverser tous les signs dans le code

---

## 11. Constantes a re-checker dans Cutter (priorise par impact)

| Adresse | Question | Commande |
|---|---|---|
| `0x3006b6f8` | Multiplier ROLL back_low ? (utilise a 0x30004bb2) | `pxw 4 @ 0x3006b6f8` |
| `0x3006b6c0` | Multiplier ROLL back_mid ? (a 0x30004ca9) | `pxw 4 @ 0x3006b6c0` |
| `0x3006b694` | Multiplier PITCH back_up ? (a 0x30004c75) | `pxw 4 @ 0x3006b694` |
| `0x3006b818` | Facteur de scale en DoControllers (0x30004a89) | `pxw 4 @ 0x3006b818` |
| `0x3006b814` | Idem | `pxw 4 @ 0x3006b814` |
| `0x3006b620` | Numerateur du fsincos angle | `pxw 4 @ 0x3006b620` |
| `0x3006b5e8` | Denominateur du fsincos angle | `pxw 4 @ 0x3006b5e8` |
| `0x3006b77c` | K_right pour leanf scaling | `pxw 4 @ 0x3006b77c` |
| `0x3006b590` | K_left pour leanf scaling | `pxw 4 @ 0x3006b590` |

Ces constantes determinent l'amplitude des bones angles. Si on veut amplifier/reduire sans patcher le code post-hoc, on peut patcher ces flots directement (plus propre).

---

## 12. Fonctions a `pdf` si besoin

| Fonction | Pourquoi | Priorite |
|---|---|---|
| `fcn.30038300` | CROUCH/PRONE variant de `OffsetFirstPersonView`. Donnera les constantes lean_width/forward pour crouch | haute si on veut amplifier separement crouch |
| `fcn.3003db10` | `GetLeanFraction` - transforme lerpLean. Donnera la mapping exacte | Moyenne |
| `fcn.3003da20` | `LerpAngle` - utilise partout dans DoControllersInternal | Basse (standard Q3 lerp) |
| `fcn.300051c0` | `CG_Player_DoControllers` wrapper. Voir comment il loop sur les 6 bones | Basse (deja sufficient pour hook) |

---

## 13. Build & deploy

### Build

```powershell
.\build_cod1.ps1
# OU
.\build_cod1.ps1 -Clean    # rebuild from scratch
```

Output : `build\cod1reloaded.dll` + `build\mss32.dll` (proxy shim)

### Deploy

```
copy build\cod1reloaded.dll  C:\Path\To\CoD1\
copy build\mss32.dll          C:\Path\To\CoD1\
copy cod1reloaded.ini         C:\Path\To\CoD1\
```

Le jeu charge `mss32.dll` au boot → shim charge `cod1reloaded.dll` → init des patches.

### Hot-tune

Pas besoin de rebuild pour changer les knobs : edit `cod1reloaded.ini`, restart le jeu. Les valeurs `-1` sur les `left_*` overrides = "use base value".

---

## 14. TODO (ordre de priorite)

### P0 — Confirmation avant nouvelle iteration

- [ ] Fix le build error initial (`build.log` montre `lean_fix.cpp.obj Error 1` mais sans le message) — lancer build et capturer la vraie erreur
- [ ] Confirmer sign convention leanf via diag log existant

### P1 — Quick wins

- [ ] Virer du config les knobs morts : `pelvis_yaw_scale`, `pelvis_roll_scale`, `neck_roll_scale`, `left_pelvis_yaw_scale`, `left_pelvis_roll_scale`, `left_neck_roll_scale` (engine ecrit 0 dessus)
- [ ] Utiliser `eFlags & 0x40` comme gate `is_leaning` au lieu de `sum_back_yaw` heuristic (plus fiable, identifiable via diag)
- [ ] `pxw` les constantes prioritaires de la section 11 pour avoir leur valeur, choisir patcher-runtime ou pas

### P2 — Features manquantes vs cod2x

- [ ] `pdf @ fcn.30038300` : trouver les constantes CROUCH lean_width/forward
- [ ] Etendre `lean_amplify` pour patcher AUSSI les sites CROUCH (si differents de 16/20)
- [ ] Considerer un hook RUNTIME sur `BG_AddLeanToPosition` au lieu de patch d'immediates (multiplier les args 16/20 a la volee depuis le .ini = pas de byte patching, plus propre)
- [ ] Reproduire le `animation_adjust_rotation` cod2x sur back_mid et back_up aussi (cod2x ne le fait que sur back_low ; verifier si on a besoin)

### P3 — Cleanup code

- [ ] Le code lean_fix.cpp a beaucoup de cycles v1-v15 dans les commentaires. Refactor : passer en revue chaque branche pour confirmer qu'elle est encore active, virer les v deprecates
- [ ] Le mvmt diagnostic logger (lignes 353-451) est tres bruyant. Capper a 30 dumps OK, mais le rendre plus parlant (decoder les champs identifies)
- [ ] Le `g_diag_logged` counter est utilise par 5+ sub-loggers, chacun avec son propre counter. Consolider en une seule structure

### P4 — Long terme

- [ ] Hook `BG_AddLeanToPosition` lui-meme pour avoir un knob runtime exact (pas patch d'immediates)
- [ ] Logger HUD overlay pour voir les valeurs leanf/eFlags en temps reel pendant le tuning
- [ ] Comparer 1:1 avec cod2x : tous les knobs, meme defaults

---

## 15. Reference rapide pour develop session

### "Je veux modifier le X"

| Symptome | Knob a tuner | Fichier |
|---|---|---|
| Head sort plus loin du cover | `body_shift_left_stand` ou `lean_amplify factor` | `cod1reloaded.ini` |
| Body penche trop / pas assez en lateral | `lean_back_*_roll_amount` | `.ini` (section [lean_fix]) |
| Body se penche en avant en lean+walk | `lean_aimwalk_v15_*` | `.ini` |
| LEFT lean diff de RIGHT | `left_*_*_scale` overrides | `.ini` |
| Camera lag derriere body | `swing_fix` (cf src/swing_fix.cpp) | `.ini` [swing_fix] |
| Crouch wallpeek | `headclip_*` | `.ini` |

### "Je veux ajouter un nouveau patch"

1. Identifier RVA dans Cutter (`axt`, `pdf`)
2. Comprendre l'asm autour du site
3. Decider : patch d'immediate OU hook trampoline
4. Ajouter nouveau fichier `src/foo_fix.cpp/h`
5. Hook dans `patches.cpp::apply_to_cgame`
6. Expose les knobs dans `cod1reloaded.ini`
7. Rebuild & test

### "Quelque chose plante"

1. `build\build.log` : erreurs compile
2. Le DLL log dans `cod1reloaded.log` (`logger::logf` → fichier)
3. MessageBox apparaissent pour erreurs de hook (opcode mismatch etc.)

---

## 16. Lessons learned (pieges historiques)

1. L'engine zero le buffer en STAND : nos additions doivent etre absolues, pas relatives a une valeur engine (qui est 0). En CROUCH, les valeurs engine sont non-nulles, donc nos `*= scale` ont du sens.

2. Bit 0x40 dans eFlags = is_leaning : signal fiable. Eviter les heuristiques sur `sum_back_yaw` qui sont sensibles au strafe.

3. `lerpLean` (ci+0x3b8) n'est pas une lean fraction propre : c'est un angle 360-wrap. Convertir signe [-180,180] avant test. Pour une vraie lean fraction normalisee, lire le global `0x3020af54` (= `leanf`).

4. Pelvis yaw/roll et neck roll sont ecrits a 0 : pas la peine de les patcher.

5. `QueryPerformanceCounter` obligatoire pour le smoothing : `GetTickCount` a 16ms res = snap entre frames a 60fps.

6. Sign convention asymetrique LEFT/RIGHT : la rig d'anim CoD1 n'est pas un miroir parfait. Toujours tester les deux cotes separement.

7. lean_amplify factor > 2.5 = collision broken : la prediction client utilise les memes constantes que le rendering, trop d'amplification peut faire traverser des murs.

8. STAND camera et CROUCH camera sont 2 fonctions differentes : `fcn.30034180` STAND, `fcn.30038300` CROUCH. Si on veut tuner les deux, patcher les sites des deux.

---

*Last updated : analyse Cutter session 2026-05-28. Tout RVA verifie sur cgame_mp_x86.dll preload base 0x30000000.*
