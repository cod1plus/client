# Plan d'action — Lag Compensation (antilag) pour CoD1 par hook du module de jeu serveur

> Cible : serveur dédié CoD1 1.5 (`CoDMP.exe +dedicated 2` chargeant `game_mp_x86.dll`, et/ou `cod_lnxded`).
> Statut : plan d'implémentation appuyé sur RE confirmée du DLL Windows + decomp de l'algo CoD2 (référence 1:1) + math id Tech 3 (Unlagged).
> Convention : adresses Windows = VMA (RVA = VMA − `0x20000000`). Confiance notée [CONFIRMÉ] / [CANDIDAT À VÉRIFIER] / [À TROUVER].
> Supersede l'étude `lag_compensation_cod1.md` (qui ne visait que Linux, offsets non trouvés).

---

## 0. Acquis validés in-game (session 2026-06-23) — socle du hook tir

Validé empiriquement sur `game_mp_x86.dll` (listen server vanilla, fresh folder) :

- Hook universel sans crash : on hooke l'entrée de `Bullet_Fire_Extended` (`0x2003f9c0`) par détour
  stolen-bytes (5o `E9 rel32`, prologue volé = `sub esp,0x50` `83 ec 50` + `cmp [esp+0x68],0xc` `83 7c 24 68 0c`,
  reprise à `+8`). Raison : le call-site racine `0x2002a06d` (dans `FireWeaponBullet`) ne capte que la
  MG montée ; les armes à main atteignent `Bullet_Fire` par un appelant indirect → seul le détour
  d'entrée attrape toutes les armes.
- Le tireur = arg1 (`entry_esp+0x04`), fiable pour toutes les armes (idx 0 = l'hôte observé). `ebx`
  à l'entrée n'est pas le tireur (valait `0x26` pour une arme à main, l'entité MG par hasard pour la MG)
  → ne jamais déréférencer ebx (cause du crash initial).
- arg6 = profondeur de récursion (`entry_esp+0x18`, = `[esp+0x68]` après le `sub`). `0` = appel racine
  = le vrai tir. → filtre exact pour ne rewind/restore qu'à la racine (la pénétration ré-appelle avec depth+1).
- arg2 = inflictor (= attacker pour arme à main) ; arg3/arg4/arg7 = pointeurs pile (`&start`, `&dir`, …).
- Le trace = syscall `0x2e` à `0x2003fa3e` via `call DWORD PTR ds:0x2006d684` (dispatch trap ;
  dernier `push` avant le call = n° de syscall). Le trace teste les entités linked → décaler une hitbox =
  écrire `r.currentOrigin (+0x138)` puis relink.
- Handler d'observation anti-crash en place : log `ebx + args pile` annotés de leur index gentity
  (`(p-g_entities)/0x31c`, borné [0,1024), sans déréf) → c'est lui qui a révélé arg1=tireur.

Prochaines inconnues (en cours de RE, workflow `antilag-re-spec`) : n° syscall `LinkEntity` (candidat `0x36`),
chemin `attacker→commandTime` (candidat `player+0x73e4`, alive `+0x73e0==2`), point de capture du ring.

---

## 1. Objectif

CoD1 (2003) n'a aucune lag compensation : le serveur teste les tirs contre la position actuelle des cibles, pas celle qu'elles avaient à l'écran du tireur (corrigée du ping). Il faut viser devant l'ennemi proportionnellement à son ping.

But : injecter, par hook du module de jeu serveur, le mécanisme `g_antilag` standard — capturer un historique de positions par frame, puis au moment du tir rembobiner la position des autres joueurs au `serverTime` que le tireur voyait, lancer le trace de la balle, restaurer.

Aucun ring buffer ni `SV_ArchiveSnapshot` n'existe en CoD1 — on les recrée dans le hook. CoD2 fournit l'algorithme de référence ; CoD1 fournit les points d'ancrage binaires.

---

## 2. Architecture cible

### 2.1 Deux cibles, mêmes algos, offsets différents

| | Windows (cible primaire de dev) | Linux (cible de prod compétitif) |
|---|---|---|
| Binaire serveur | `CoDMP.exe +dedicated 2` → charge `Main/game_mp_x86.dll` | `cod_lnxded` (game serveur lié statiquement ou `.so`) |
| Injection | DLL proxy déjà en place : `mss32.dll` (cf. `cod1reloaded/mss32.dll`, `docs/mss32_proxy.md`) | `LD_PRELOAD` d'un `.so`, ou patch ELF |
| Hook technique | détour x86 (trampoline 5 octets `E9 rel32`) sur les VMA ci-dessous | mêmes algos, offsets à re-dériver (layout `cod_lnxded` ≠ DLL Windows) |
| Mémoire historique | bloc alloué par notre code (ring buffer maison) | idem |
| Offsets struct | [CONFIRMÉS] sur `game_mp_x86.dll` (section 4) | [À TROUVER] — le DLL Windows ne sert que d'indice |

Stratégie recommandée : développer et valider d'abord sur Windows dédié (offsets confirmés, debug facile via le proxy `mss32.dll`), puis re-dériver le petit jeu d'offsets pour Linux.

### 2.2 Composants à écrire (dans le DLL injecté)

1. `History` — ring buffer maison par client : `{ vec3 origin; vec3 mins; vec3 maxs; int leveltime; }[NUM_CLIENT_HISTORY]` + `historyHead`, + un `saved` par client pour le restore. Dimensionner par temps, pas par nb de frames (le lookup se fait par `level.time` → insensible au tick rate). Prod = sv_fps 40 → 25 ms/frame (local listen server observé : ~16 ms). Pour couvrir ~1,5 s : 1500/25 = 60 entrées @ 40fps. On prend NUM_CLIENT_HISTORY = 128 (marge, bon à tout tick rate + gros ping). `no-rewind-if-recent` = `1000/sv_fps` (25 ms @ 40fps), dérivé du `dt` observé.
2. `Hook_CaptureFrame` — appelé à chaque frame serveur, écrit `g_entities[i].r.currentOrigin` (+mins/maxs) + `level.time` dans le ring.
3. `Hook_BulletFire` — autour du trace balle : calcule le temps de rembobinage, `Rewind` tous les autres clients vivants, laisse le trace s'exécuter, `Restore`.
4. `Rewind` / `Restore` — déplacent `r.currentOrigin` (+ bbox) et forcent un relink collision (équivalent `SV_UnlinkEntity`/`SV_LinkEntity` — en CoD1 ce relink se fait via syscall `0x36` `trap_LinkEntity`).
5. Cvar `g_antilag` — l'enregistrer nous-mêmes (il n'existe pas en CoD1) via le syscall Cvar, pour pouvoir l'activer/désactiver à chaud.

---

## 3. Algorithme + formule de timing concrète

### 3.1 Chaîne de données (du usercmd au rewind)

Le tireur envoie un `usercmd_t` portant `cmd->serverTime` = le serverTime que son écran affichait quand il a tiré. Cette valeur encode déjà le RTT + le délai d'interpolation client — on ne soustrait jamais le ping à la main.

En CoD1 cette valeur est lisible :
- dans `pmove_t` à `+0x04` = `cmd.serverTime` [CONFIRMÉ via cod1_symbols]
- recopiée dans la struct joueur serveur à `player+0x73e4` = commandTime/serverTime [CONFIRMÉ] (test alive `player+0x73e0 == 2`, comme le `state==2` de CoD2).

### 3.2 Formule de reconciliation (le cœur)

Au tir hitscan, choix du temps de rembobinage `rewindTime` :

```
si (g_antilag activé) :
    rewindTime = attacker.commandTime          // = cmd->serverTime du tireur  (CoD1: player+0x73e4)
sinon :
    rewindTime = level.time                     // pas de rewind  (CoD1: ds:0x202cdd68)
```

C'est le branchement CoD2 (`aimTime = client[+0x2864] : level.serverTime`) et CoD4x. La version Unlagged ajoute deux raffinements optionnels (recommandés en phase 4) :

```
// raffinement interpolation client (corrige cl_timenudge) :
rewindTime = cmd->serverTime + cmdTimeNudge

// raffinement "fenêtre d'une frame" + borne ping :
rewindTime = max( cmd->serverTime + cmdTimeNudge,
                  level.previousTime + frameOffset - 50 )       // -50 = 1 frame @ sv_fps 20
// clamp dur recommandé : borner rewindTime à  [ping+50 ± 100 ms]
```

### 3.3 Garde-fous de timing (repris verbatim de CoD2)

- No-rewind-if-recent : si `level.time - rewindTime <= 1000/sv_fps` → return (le tir est dans la frame courante, positions déjà à jour).
- Clamp max rewind : borner `rewindTime` à `level.time - 1000` (1 s max) et à la profondeur du ring (`NUM_CLIENT_HISTORY` frames). Au-delà → on tape la plus vieille entrée (jamais lire hors buffer).

### 3.4 Lookup de position = interpolation entre 2 snapshots

Pour chaque cible vivante, trouver les deux entrées d'historique `j` (leveltime ≤ rewindTime) et `k` (suivante) qui encadrent `rewindTime`, puis lerp linéaire (mêmes ops flottantes que le client pour éviter le micro-désalignement) :

```
frac = (rewindTime - history[j].leveltime) / (history[k].leveltime - history[j].leveltime);
origin = history[j].origin + frac * (history[k].origin - history[j].origin);
// idem mins/maxs ; SnapVector(origin) pour matcher exactement la grille client
```

C'est l'équivalent du `SV_GetClientPositionAtTime` CoD2 (`0x0018EBA0`, lerp 2 frames, tolérance 10 frames manquantes) mais alimenté par notre ring, pas par le snapshot archive moteur (inexistant en CoD1).

### 3.5 Séquence d'un tir (pseudo-final)

```
EV_FIRE_WEAPON / FireWeapon(attacker)
 └─ rewindTime = g_antilag ? attacker.commandTime : level.time
 └─ Hook_BulletFire :
      ├─ SI g_antilag ET (level.time - rewindTime > 1000/sv_fps):
      │     pour chaque client c != attacker, vivant (health>0, svFlags ok, alive):
      │        si GetHistoryPos(c, rewindTime, &pos):
      │           saved[c] = { c.r.currentOrigin, c.r.mins, c.r.maxs }
      │           Unlink(c); c.r.currentOrigin = pos (+ bbox); Link(c)
      │           moved[c] = 1
      ├─ Bullet_Fire_Extended(...)            // trace 0x2e contre positions rembobinées
      └─ SI g_antilag: pour chaque moved[c]: Unlink(c); restore saved[c]; Link(c)
```

> Le tireur n'est jamais rembobiné (sa prédiction client est authoritative sur sa propre position). Spectateurs/morts/déconnectés exclus.

---

## 4. Points de hook exacts (`game_mp_x86.dll`, Windows)

### 4.1 Capture d'historique (par frame)

| Cible | VMA | Rôle | Confiance |
|---|---|---|---|
| ClientEndFrame | `0x2001b380` | Calcule `clientNum=(ent-g_entities)/0x31c`, link, set clipmask `[ent+0x7c]=0x3ff`. Meilleur point pour snapshotter `r.currentOrigin` par frame. | [CONFIRMÉ] (fonction) / [CANDIDAT] (rôle exact) |
| vmMain GAME_CLIENT_THINK | `0x200193f0` (case 7) | entrée per-client (arg=clientNum) | [CONFIRMÉ] |
| build entityState | `0x20019470` | écrit `r.currentOrigin` (+0x138) | [CANDIDAT] |

Approche retenue : hook `0x2001b380`, ou plus robuste — hooker la fin du dispatch vmMain et itérer soi-même `g_entities[0..MAX_CLIENTS]` (réduit la dépendance au rôle exact de ClientEndFrame).

Globals [CONFIRMÉS] : `level.time = ds:0x202cdd68` (int ms) ; `level.num_entities = ds:0x202cdd60` ; base `level = 0x202cdb80` ; `g_entities` base = `0x201756a0`, stride `0x31c` (796 o).

### 4.2 Tir / trace balle (rewind/restore autour)

| Cible | VMA | Rôle | Confiance |
|---|---|---|---|
| Bullet_Fire_Extended | `0x2003f9c0` | lance le trace de hit (syscall `0x2e`), gère pénétration. `ebx`@entrée = gentity tireur. Point idéal pour wrap rewind/restore. | [CONFIRMÉ] (string `"Bullet_Fire_Extended: Too many resursions"` @0x2005906c + push 0x2e) |
| FireWeaponBullet | `0x2002a000` | parent direct, calcule spread, appelle Bullet_Fire_Extended @0x2002a06d | [CONFIRMÉ] |
| FireWeapon | `0x2002ab88` | parent de FireWeaponBullet | [CONFIRMÉ] |
| G_GetMuzzlePoint | `0x20029eb0` | origine muzzle (tags `tag_flash`/`tag_weapon`) | [CONFIRMÉ] |

Choix : hooker `0x2003f9c0` en wrap serré. Le tireur = `ebx` à l'entrée → exclusion directe.

### 4.3 Trace / collision

- Pas de wrapper `trap_Trace` dédié : syscall `0x2e` inliné (11 sites). Site balle pertinent : `0x2003fa3c`. [CONFIRMÉ]
- Trampoline syscall unique : `ds:0x2006d684` → `0x20029b90` → `jmp 0x2003f130`. [CONFIRMÉ]
- Alternative bas-niveau : hooker `ds:0x2006d684` et filtrer syscall `0x2e` (intercepte tous les traces).

### 4.4 Link / Unlink (pour que le trace voie les positions rembobinées)

- `trap_LinkEntity` = syscall `0x36` (54) [CONFIRMÉ] ; `trap_UnlinkEntity` = syscall voisin [CANDIDAT — numéro exact à confirmer].
- Émettre via le trampoline `0x20029b90` (numéro pushé en dernier avant `call`).

### 4.5 Struct gentity (stride `0x31c`) — offsets clés

| Offset | Champ | Confiance |
|---|---|---|
| `+0x0c` | `r.svFlags` (test `& 0x20000`) | [CONFIRMÉ] |
| `+0x104`/`+0x110` | `r.absmin` / `r.absmax` (bbox monde, vec3) | [CONFIRMÉ] |
| `+0x138/13c/140` | `r.currentOrigin` (vec3) — ce qu'on save/rewind/restore | [CONFIRMÉ] (écrit 181×, lu dans Bullet_Fire) |
| `+0x144/148/14c` | `r.currentAngles` (vec3) | [CONFIRMÉ] |
| `+0x15c` | `client` (gclient_t*) | [CONFIRMÉ] |
| `+0x17c` | `s.number` (entityNum, WORD) | [CONFIRMÉ] |
| `+0x184` | `flags` (bit0=FL_GODMODE) | [CONFIRMÉ] |
| `+0x238` | `health` (int) — gate alive | [CONFIRMÉ] (`sub [ebp+0x238],dmg` + string `"target:%i health:%i"`) |

> Gate dead/alive fiable = `health (+0x238) > 0` + `svFlags (+0xc)`. (`eFlags` [NON CONFIRMÉ] dans ce binaire — ne pas s'en servir.)

`gclient_t` : base `0x202cdb80+0x2180`, stride `0x22cc`. Posture : `[client+0x80] & 0xc000` (CROUCH 0x4000 | PRONE 0x8000) [CONFIRMÉ] — utile pour ajuster la bbox rembobinée selon la posture historique.

`G_Damage` = `0x20022a..` (`ebp`=cible) [CONFIRMÉ].

### 4.6 Mapping conceptuel CoD2 → CoD1 (pour porter, et pour Linux)

| Concept | CoD2 | CoD1 Windows |
|---|---|---|
| temps de visée | `client[+0x2864]` | `pmove+0x04` / `player+0x73e4` [CONFIRMÉ] |
| origin gentity | `ent+0x138` | `ent+0x138` [CONFIRMÉ] |
| serverTime / numClients | `level[123]` / `level[121]` | `ds:0x202cdd68` / `ds:0x202cdd60` [CONFIRMÉ] |
| ring buffer + archive | `svs[19]` 4096×9992 + `SV_ArchiveSnapshot` | à créer (ring maison sur hook frame) |
| link/unlink | `SV_LinkEntity`/`SV_UnlinkEntity` | syscall `0x36` / voisin [CONFIRMÉ/CANDIDAT] |

---

## 5. Plan d'implémentation par phases

### Phase 0 — Harnais d'injection & lecture mémoire *(première étape actionnable)*
But : prouver qu'on peut lire l'état du jeu depuis un hook, avant tout rewind.
1. Étendre le proxy `mss32.dll` existant pour, au chargement de `game_mp_x86.dll`, résoudre `imagebase` réel (ASLR) et calculer les adresses absolues = `imagebase + RVA`.
2. Poser un seul hook non destructif sur `0x2001b380` (ClientEndFrame) : à chaque appel, logger `clientNum`, `level.time (ds:0x202cdd68)`, et `r.currentOrigin (ent+0x138)`.
3. Critère de succès : en match local (CoDMP +dedicated 2 + 1 bot), le log montre des origines cohérentes avec le déplacement, et `level.time` qui avance par pas de 50 ms (sv_fps 20). → Confirme `0x2001b380`, `+0x138`, `level.time`, stride `0x31c`.

> Livrable Phase 0 : un `.log` prouvant la lecture correcte. Dérisque tous les offsets [CONFIRMÉS] avant d'écrire la logique.

### Phase 1 — Ring buffer d'historique
1. Allouer `History[MAX_CLIENTS][NUM_CLIENT_HISTORY=17]` + `historyHead[]` + `saved[]`.
2. Dans le hook frame : push `{origin, mins, maxs, leveltime}` par client connecté vivant, avance `historyHead` (modulo 17). `G_ResetHistory` au spawn/téléport (remplir avec la position courante → évite les trous).
3. Critère : dump du ring = trajectoire continue de 17 frames glissantes.

### Phase 2 — Cvar + lookup interpolé (lecture seule)
1. Enregistrer `g_antilag` (Cvar via syscall) défaut 1.
2. Implémenter `GetHistoryPos(client, rewindTime, &out)` (lerp 2 frames + clamp + SnapVector).
3. Hook `0x2003f9c0` en mode observation : calculer `rewindTime` depuis `player+0x73e4`, logger « position actuelle vs rembobinée » — sans rien déplacer. Valide la formule de timing sur trafic réel.

### Phase 3 — Rewind / restore (le cœur)
1. Confirmer le numéro de syscall `UnlinkEntity` (filtrer le trampoline `0x2006d684` autour de `0x36`).
2. Dans le hook `0x2003f9c0` : save → Unlink → écrire `currentOrigin`+bbox → Link → trace (code original) → au retour, restore miroir gated par `moved[]`.
3. Exclusions : tireur (`ebx`), morts (`+0x238<=0`), spectateurs (`svFlags`).
4. Critère : un bot qui strafe est touché là où il était au tir d'un client à ping simulé.

### Phase 4 — Durcissement + raffinements timing
1. Ajouter `cmdTimeNudge` + borne `max(..., previousTime+frameOffset-50)` + clamp `ping±100`.
2. Bbox selon posture historique (`client+0x80 & 0xc000`).
3. Anti double-restore (`saved.leveltime == level.time`), garde re-entrance (pénétration récursive ≤12).

### Phase 5 — Portage Linux (`cod_lnxded`)
Re-dériver uniquement le petit jeu d'offsets [À TROUVER] (gentity origin/health, `level.time`, syscall numbers, fonctions fire/trace) avec le même protocole Phase 0. Code algo inchangé.

---

## 6. Risques & garde-fous (honnête)

| Risque | Confiance / impact | Garde-fou |
|---|---|---|
| Rôle exact de `0x2001b380` [CANDIDAT] | moyen | Phase 0 le dérisque ; fallback = itérer `g_entities` post-vmMain |
| Numéro syscall UnlinkEntity [CANDIDAT] | moyen | Confirmer en Phase 3 ; sans relink le trace voit les anciennes positions (silencieusement faux) |
| Re-entrance : Bullet_Fire_Extended s'auto-appelle (pénétration ≤12) | élevé si ignoré | Rewind/restore au niveau racine seulement (flag de profondeur) |
| Joueurs encastrés après shift | connu (Unlagged) | Borner bbox + relink ; ne shifter que `currentOrigin` |
| `cl_timenudge` négatif → cmd time en avance | connu | Clamp au plus récent snapshot ; jamais extrapoler |
| Erreur flottante trace serveur ≠ client | subtil | `TimeShiftLerp` + `SnapVector` reproduisant les ops client |
| ASLR / imagebase | faible | Résoudre `imagebase` au runtime, jamais coder le VMA en dur |
| Offsets Linux ≠ Windows | élevé pour prod | Phase 5 dédiée ; Windows = banc de validation |
| Anti-cheat (PunkBuster legacy) | variable | Tester sur serveur sans PB ; documenter |

---

## 7. Fichiers de référence

- Binaire à hooker : `Call of Duty - R 1.5\Main\game_mp_x86.dll`
- Serveur : `Call of Duty - R 1.5\CoDMP.exe`
- Proxy d'injection : `cod1reloaded\mss32.dll` (doc : `docs\mss32_proxy.md`)
- Algo CoD2 de référence : `cod1reloaded\cod2x\src\other\Call_of_Duty_2_Multiplayer_MAC_1.3.c` (`FireWeaponAntiLag 0x001C0D30`, `Bullet_Fire 0x001C07E0`, `SV_GetClientPositionAtTime 0x0018EBA0`, `SV_GetArchivedClientInfo`, `SV_ArchiveSnapshot`)
- Symboles CoD1 : `cod1reloaded\docs\cod1_symbols.md`
- Étude antérieure : `cod1reloaded\docs\lag_compensation_cod1.md`
- Refs externes : CoD2rev_Server (voron00), CoD4x_Server, Q3 Unlagged (Neil Toronto)

*Plan issu de l'investigation (RE game_mp_x86.dll + CoD2rev + Q3 unlagged + decomp CoD2). À porter éventuellement vers le repo serveur `cod1plushookserver`.*
