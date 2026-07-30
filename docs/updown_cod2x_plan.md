# Up/Down (stance) — audit complet cod2x + plan de portage CoD1

> Ecrit 2026-07-24 apres 2 regressions (diag_fold discontinu, anim_clamp clobber esi).
> Regle de ce plan : **on mesure avant de patcher**. Aucun patch en aval sans donnees.

---

## 1. Ce que fait cod2x, EN TOTALITE (verifie ligne par ligne dans `cod2x/src/shared/animation.cpp`)

Le fix up/down de cod2x = **4 composants solidaires**, pas un reglage.

### C1 — Supprimer les ANIMATIONS de transition de posture (le coeur)
`animation_changeFix()` NOP 4 sites qui declenchent les anims scriptees de playeranim.script :
```
patch_nop(0x00517e60 / 0x080e4cab, 5)   // ANIM_ET_PRONE_TO_STAND
patch_nop(0x00517f89 / 0x080e4e4f, 5)   // ANIM_ET_CROUCH_TO_STAND
patch_nop(0x00517db1 / 0x080e4bde, 5)   // ANIM_ET_STAND_TO_CROUCH
patch_nop(0x00517d16 / 0x080e4b65, 5)   // ANIM_ET_PRONE_TO_CROUCH
```
=> plus d'animation scriptee de transition (celle qui porte le rebond vertical).
Le modele **BLEND** simplement de la pose debout vers la pose accroupie.
**C'est ca qui tue l'up-down spam.**

### C2 — Imposer le TEMPS de blend par type de transition
`BG_SetNewAnimation` (substitue via `patch_call` @0x004f9dd1/0x080da4cd) ecrit `lf->animationTime` :
| transition | valeur |
|---|---|
| 1 Stand->Crouch | `PLAYER_CROUCH_TIME` = **200 ms** |
| 2 Crouch->Prone | `PLAYER_PRONE_TIME * 0.58` = **232 ms** (1re pers. tombe plus vite) |
| 3 Prone->Crouch | `PLAYER_PRONE_TIME` = **400 ms** |
| 4 Crouch->Stand | `PLAYER_CROUCH_TIME` = **200 ms** |
Sinon plancher generique : `transitionMin` = 120 (si l'anim bouge) / 170 (idle->idle) / 250 (idle->moving).

### C3 — Empecher le moteur d'ecraser cette valeur
4 NOP sur les ecritures moteur de `lf->animationTime` :
`0x004f9991`(7o) `0x004f98ff`(3o) `0x004f9a07`(3o) `0x004f997e`(7o).

### C4 — Compenser la POSE pendant la transition (prone uniquement)
Dans `BG_Player_DoControllersInternal`, avec `stanceTransitionFraction` (0->1) et une courbe cosinus :
```c
// case 2 Crouch->Prone
curve1 = 0.5*(1-cos(2*PI*fraction1));   // fraction1 = f/(1-0.4)
curve2 = 0.5*(1-cos(2*PI*fraction2));   // fraction2 = (f-0.4)/(1-0.4)
tag_origin_offset[0]   += curve1 * -5;  // recule pour centrer la tete en 1re pers
tag_origin_offset[2]   += curve2 * 12;  // rebond de hauteur au contact du sol
tag_origin_angles[0]   += curve2 * -10; // rotation corps entier
tag_back_low_angles[0] += curve2 * +10; // contre-rotation du dos
// case 3 Prone->Crouch : tag_origin_offset[0] += curve1 * -5
```
**Stand<->Crouch (cas 1 et 4) : AUCUNE compensation de pose.** Uniquement C1+C2.

### Etat persistant necessaire
`animationPlayerData[64*2]` (serveur + client separes, index = clientNum + 64 si client) :
`stanceTransitionType`, `stanceTransitionTime`, `stanceTransitionTimeEnd`.
Rempli dans C2, consomme dans C4.

### Hors scope up/down mais dans le meme fichier
`PM_SetMovementDir` velocity-based (`if(backwards) +180; clamp ±90`) — c'est le fix du
**movementDir**, pas de l'up/down. (C'est lui qui rendait `animation_adjustRotation` sur.)

---

## 2. Realite CoD1 (verifie dans les binaires + les assets)

| Element cod2x | Equivalent CoD1 | Statut |
|---|---|---|
| Anims de transition stand<->crouch | **N'EXISTENT PAS** | C1 **INAPPLICABLE** |
| `mp/playeranim.script` | **PRESENT** (pak4.pk3, 24 Ko) | meme systeme |
| movetypes | `walkcr, walkcrbk, runcr, runcrbk, idlecr...` | pose par set d'anims |
| `crouch_to_prone`/`prone_to_crouch` | **en COMMENTAIRE uniquement** | pas d'anim reelle |
| blend de pose | imm32 `add ecx,0x190` @cgame+0x397f | **deja patche** (stance_fix 400->150) |
| viewheight | `bg_viewheight_crouched` (client+serveur) | present |
| levier bonus | **`cg_noplayeranims`** (cvar cgame) | a explorer |

### Consequence majeure
**CoD1 n'a pas d'animation scriptee de transition stand<->crouch.** Le composant C1 — le coeur
du fix cod2x — **n'a rien a NOP**. Un portage 1:1 est donc impossible ; la cause de l'up/down
CoD1 est forcement ailleurs (blend de pose, viewheight, ou desync serveur/client de l'etat crouch).

**=> Il faut IDENTIFIER la cause CoD1 avant de coder.** C'est exactement l'erreur que j'ai faite
deux fois aujourd'hui (patcher en aval une cause supposee).

---

## 3. Plan

### Phase 0 — DIAGNOSTIC (obligatoire, aucun patch)
Objectif : savoir **ce qui bouge** exactement, et **ou**.

0.1 **Cadrer le symptome avec les testeurs** (3 questions) :
  - C'est la **vue 1re personne** (la camera du joueur qui spamme) ou le **modele 3e personne**
    (ce que voient les autres) ? -> determine viewheight vs bones.
  - Le spam rend-il le joueur **difficile a toucher** (probleme competitif/desync) ou est-ce
    seulement **moche** (cosmetique) ? -> determine la priorite et si le serveur est concerne.
  - Reproductible en **vanilla** (mss32.dll retire) ? -> notre mod est-il en cause, oui/non.

0.2 **Instrumenter** (log client, throttle, zero effet gameplay) :
  - `ps.viewHeightCurrent` / cible / vitesse de lerp par frame pendant un cycle crouch
  - `tag_origin_offset[2]` (hauteur du modele) + `tag_origin_angles[0]` (pitch corps)
  - l'index d'animation des jambes (`legs.animation`) pendant crouch+S
  - le timing : quand l'etat crouch change vs quand la pose suit

0.3 **Livrable** : une trace horodatee d'un cycle "crouch spam + W/S" -> on voit si l'amplitude
vient du viewheight, du blend de pose, ou d'un changement d'anim.

### Phase 1 — FIX cible (dependra de la Phase 0)
Trois scenarios probables, prepares a l'avance :

**S-A : c'est le BLEND de pose** (le plus probable vu C1 absent)
  -> equivalent CoD1 de C2 : imposer un temps de blend **par type de transition**
     (au lieu de notre unique 400->150 global), + envisager le plancher `transitionMin`.
  -> travail : RE de la fonction CoD1 equivalente a `BG_SetNewAnimation` (chercher les
     ecritures de `animationTime` autour de cgame+0x397f, deja localise).

**S-B : c'est le VIEWHEIGHT** (1re personne)
  -> notre `viewheight_lerp_speed` (80) est deja un levier ; il faudra le lier au **meme**
     timing que le modele pour que vue et modele soient synchrones (l'esprit de C2).

**S-C : c'est un DESYNC serveur/client de l'etat crouch** (exploit)
  -> le fix est **serveur** (cod1plus.so) : borner la frequence de changement de posture,
     ou forcer la hitbox a suivre la pose reelle. Notre `perbone_hit` pose deja le squelette
     -> a verifier s'il suit le bob ou non.

**Ce qu'on NE fait PAS** (lecons du jour) :
  - aucun repli/clamp d'angle en aval (`diag_fold` = discontinu a ±90 -> claque)
  - aucun patch transformant un chemin no-return en chemin returnant sans auditer les
    registres callee-saved (`anim_clamp` client = clobber esi -> camera cassee)

### Phase 2 — VALIDATION
- knob hot-reload + valeur vanilla exacte pour revert instantane sans rebuild
- test A/B par les testeurs : crouch spam, crouch+S, crouch+lean+S, stand<->crouch repete
- non-regression : le fix piquer avance+strafe+lean, et la camera

---

## 4. Ce qui est deja en place cote CoD1 (base de depart)
- `stance_fix.cpp` : blend modele 400 -> 150 ms (imm32 @cgame+0x397f), hot-reload
- `viewheight_fix.cpp` : lerp vue (`viewheight_lerp_speed` = 80)
- `lean_fix.cpp` : lissage de controleurs (`ctrl_smooth_*`, inspire du controllerMovement* cod2x)
- `perbone_hit.c` (serveur) : hitbox par os, pose le squelette reel

Ces 4 modules sont les points d'accroche du futur fix — rien a creer de zero.

---

## 5. MISE A JOUR apres reponses testeurs (2026-07-24)

**Reponses:** "ca rend dur a toucher" + "reproductible en VANILLA".
=> ce n'est **PAS notre mod**, c'est le comportement CoD1 de base, et c'est un probleme de
**HITBOX** (serveur-autoritatif), pas cosmetique.

### 5.1 Mecanique de posture CoD1 (desassemblee, faits nouveaux)

**`PM_GetEffectiveStance` @game.so+0x1e9a5** — la posture est DERIVEE de la hauteur de vue cible:
```
[pm+0xcc] = viewHeightTarget courant
si == [pm+0x340] -> stance 2 (CROUCH)
si == [pm+0x33c] -> stance 1 (PRONE)
sinon            -> stance 0 (STAND)
```

**`PM_GetViewHeightLerpTime` @game.so+0x223a6** — le timing du lerp de vue:
```
target == [pm+0x33c] (prone)  -> 0x190 = 400 ms
target == [pm+0x340] (crouch) -> arg3 ? 0xc8 = 200 ms : 400 ms
sinon (stand)                 -> 0xc8 = 200 ms
```

> **CoD1 utilise DEJA 400 ms (prone) et 200 ms (crouch/stand) — les valeurs EXACTES de cod2x**
> (`PLAYER_PRONE_TIME` 400 / `PLAYER_CROUCH_TIME` 200). **Le composant C2 est donc DEJA
> NATIF en CoD1.** Le timing de la vue 1re personne n'est pas le bug.

Cvars presentes: `bg_viewheight_standing/crouched/prone`, `g_bounds_width`,
**`g_bounds_height_standing`** — et **AUCUN `g_bounds_height_crouched/prone`**.
(Acces via GOT, module PIC: pas de xref absolue exploitable statiquement.)

### 5.2 Hypothese de travail (a CONFIRMER par mesure, pas a patcher a l'aveugle)

La vue et la pose du modele **lerpent** (200/400 ms). La question decisive:
**la BOITE de collision suit-elle ce lerp, ou saute-t-elle instantanement ?**

- Si la boite **saute** pendant que le modele est encore entre deux -> on tire sur ce qu'on
  voit et la boite n'y est plus = **exactement "dur a toucher"**, et le spam maximise l'ecart.
- Notre `perbone_hit` ne peut PAS rattraper ca: c'est une phase FINE qui ne tourne
  **que si la boite (phase LARGE) a deja touche**. Boite ratee = perbone jamais appele.

### 5.3 Fix candidat (colle a l'architecture existante)

**Rendre la phase LARGE genereuse pendant les transitions** (boite jamais plus petite que ce
que le visuel peut etre pendant le lerp) et **laisser `perbone_hit` trancher au bone pres**.
C'est litteralement le design deja documente dans `perbone_hit.c`:
> "the wide box catches every legit shot; the per-bone pass rejects the over-hang"

Avantages: 100% serveur (la ou est l'autorite), aucun changement client, aucun impact sur le
ressenti (perbone decide toujours du hit reel), et ca ne touche pas au gameplay de la posture.

### 5.4 PROCHAINE ETAPE = MESURE (pas de patch)

Instrumenter `perbone_hit.c` (mode dump, deja existant) pour logger a CHAQUE tir sur un joueur:
- la stance de la victime (via eFlags CROUCH/PRONE)
- `r.mins[2]` / `r.maxs[2]` (hauteur reelle de la boite a cet instant)
- `ps.viewHeightCurrent` vs `viewHeightTarget` (ou en est le lerp)
- si la phase large a touche, et si perbone a trouve un os

=> Une seule session de spam crouch devant un testeur donne la reponse:
**boite qui saute (hypothese confirmee) vs boite qui lerp (chercher ailleurs)**.

---

## 6. GAP ANALYSIS cod2x vs nous (2026-07-24) — "qu'est-ce qu'ils font qu'on ne fait pas ?"

Le lissage de controleurs EST porte (`apply_ctrl_smooth` dans lean_fix.cpp) et fidele sur le
PRINCIPE: au changement d'etat -> fige la pose -> lerp temporel des 24 floats -> puis clamp
de vitesse en regime etabli (equivalent BG_LerpAngles 0.36 deg/ms, BG_LerpOffset 0.1 u/ms).
cod2x remplace ainsi le lerp vanilla a vitesse limitee, dont le defaut est ecrit dans leur
propre commentaire: *"the rotation is sum of body_low+body_mid+body_up ... the weapon moving
side to side when player movement changes ... tag_origin was lerping too slow compared to the
sum of the body angles"*. Les 8 controleurs convergent independamment -> desynchro visible.

### Ecart #1 (LE PLUS IMPORTANT) : d'ou vient "l'etat de mouvement"
| | source de l'etat |
|---|---|
| **cod2x** | l'ANIMATION REELLEMENT JOUEE: `BG_GetConditionValue(ci, ANIM_COND_MOVETYPE)` teste contre RUN/WALK/RUNCR/WALKCR/WALKPRONE (fw) et RUNBK/WALKBK/RUNCRBK/WALKCRBK/WALKPRONEBK (bw), + `animations[es->legsAnim].flags & 0x10 / 0x20` (strafe gauche/droite) |
| **nous** | HEURISTIQUE sur les angles de SORTIE: `buf[19]` (yaw) bucketise a 45 deg, `buf[8]` (roll de lean) bucketise, + le flag crouch |

**Consequence concrete: IDLE <-> MOVING est INVISIBLE pour nous.** Immobile et courant en avant
donnent tous deux movementYaw ~0 -> meme bucket -> **aucune transition declenchee** -> la pose
SNAP au lieu d'etre lerpee. Or **W+S fait precisement osciller entre moving et idle** (c'est le
symptome rapporte par les testeurs). En plus, notre bucketisation peut declencher des
transitions PARASITES quand le yaw traverse une frontiere de bucket.

**CoD1 a l'equivalent exact et il est deja RE'd:** `es->animMovetype` @ **es+0xe0** (4 bits =
idle + 8 directions discretes; cf. note movementDir/pmove dans MEMORY.md). Notre pointeur
"entity" EST un entityState_s (eFlags @+0x8 = layout Q3 standard, confirme). Donc remplacer
l'heuristique par une lecture de es+0xe0 (+ crouch/prone/lean) donnerait un etat **semantique**
equivalent a cod2x, incluant idle<->moving.

### Ecart #2 : duree de transition par TYPE
cod2x: **250** ms (stand/crouch), **400** (prone), **150** (lateral gauche/droite),
**100** (prone backwards fire), **300** (fallback si 0). Nous: un seul `ctrl_smooth_time` = 250.

### Ecart #3 : PRONE totalement exclu chez nous
`apply_ctrl_smooth` fait `if (eflags & PRONE) return;`. cod2x a toute une section prone
(400 ms, mise a l'echelle du lean, compensations tir/reload en arriere).

### Ecart #4 : compensations de pose prone<->crouch (= C4 de la section 1)
Les courbes cosinus (rebond au contact du sol, recul pour centrer la tete). Non portees.

### Ecart #5 : echelles (climbup/climbdown, tag_neck) — cosmetique, faible priorite.

### HONNETETE sur le lien avec le bug testeur
Ces 5 ecarts sont **client-side visuels**. Le symptome rapporte est "dur a toucher" + "reproductible
en vanilla" = un probleme de HITBOX (serveur). Donc **combler ces ecarts ne reglera pas
mecaniquement le bug** SI "dur a toucher" veut dire desync de hitbox.
MAIS si "dur a toucher" veut dire *"le modele bouge de facon erratique, je n'arrive pas a le
suivre"*, alors l'ecart #1 est EXACTEMENT le fix (c'est le probleme que cod2x decrit lui-meme).
=> **Les deux lectures restent ouvertes; la Phase 0 (mesure) tranche.** Ecart #1 vaut d'etre
comble dans tous les cas (qualite visuelle + parite cod2x), avec un log de verification de
es+0xe0 AVANT de s'y fier.
