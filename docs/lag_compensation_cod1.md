# Lag Compensation (antilag) pour CoD1 — Plan & RE

Document de travail : portage d'une lag compensation (anti-lag / backward
reconciliation) sur un serveur CoD1 1.5, pour la communauté compétitive cod1plus.

> Statut : étude / plan. Rien d'implémenté. Cible = serveur (`cod1plus.so` /
> `cod_lnxded`), pas le client `cod1reloaded`.

---

## 1. Objectif

En CoD1, il n'y a aucune lag compensation : le serveur teste les tirs contre
la position actuelle des joueurs, pas contre celle qu'ils avaient au moment
où le tireur a tiré (corrigé de son ping). Résultat : il faut viser devant
l'ennemi proportionnellement à son ping.

Objectif : que le serveur rembobine la position des autres joueurs au temps
de tir du tireur, fasse le trace de la balle, puis restaure — pour que « ce que
tu vois = ce que tu touches ». C'est le standard compétitif (Quake3 unlag, CoD2
`g_antilag`, CoD4x).

---

## 2. Constat : CoD2 a l'antilag natif, CoD1 ne l'a pas

### CoD2 = natif (pas une feature cod2x)

`g_antilag` apparait uniquement dans les décompilations IDA du binaire CoD2
d'origine (`cod2x/src/other/*.c`, dossier "reversed/testing"), jamais dans
le code ajouté par cod2x (`src/shared`, `src/mss32`). Le jeu CoD2 l'enregistre
lui-meme :

```c
// cod2x/src/other/Call_of_Duty_2_Multiplayer_MAC_1.3.c
g_antilag = Dvar_RegisterBool(..., "g_antilag", 1, 0x1005u);   // defaut = 1 (ON)
...
if ( *(_BYTE *)(g_antilag + 8) )          // si dvar->enabled
    a3 = FireWeaponAntiLag(...);          // = chemin de tir avec lag comp
```

Donc cod2x s'appuie sur l'antilag natif de CoD2, il ne l'ajoute pas.

### CoD1 = aucune lag comp (vérifié)

Scan ASCII des binaires CoD1 (`cod1dll/gamex86.dll`, `cod1dll/cgame_mp_x86.dll`,
`CoDMP.exe` 1.5) :

| terme | hits |
|---|---|
| `g_antilag` / `antilag` | 0 |
| `lagcomp` / `unlag` | 0 |
| `g_smoothClients` / `sv_smoothClients` | 0 |
| `timenudge` / `cl_timenudge` | 0 |
| `compensat` / `rewind` | 0 |

Sanity check (le scan marche bien) : `g_gravity`=1, `g_speed`=1, `bg_`=25 trouvés.

CoD1 (2003) n'a aucune lag compensation. Introduite par CoD2 (2005).

---

## 3. L'algorithme (portable, indépendant du moteur)

C'est le même concept partout (Quake3 unlag, CoD2, CoD4x) :

1. Capture d'historique — à chaque frame serveur, stocker pour chaque joueur
   sa position (origin) + idéalement les positions de hitbox/bones, avec le
   `serverTime` de la frame. Ring buffer de N frames (assez pour couvrir le ping
   max, ex. ~1 s).
2. Au tir d'un joueur (bullet trace) :
   - calculer le temps de visée du tireur ≈ `serverTime − latence` (RTT/2 +
     interp côté client, à caler ; CoD2 utilise le command time du tireur).
   - rembobiner tous les autres joueurs à leur position interpolée à ce temps
     (lerp entre 2 snapshots de l'historique).
   - exécuter le trace de la balle contre ces positions rembobinées.
   - restaurer les positions réelles.
3. Bornes de sécurité : clamp le rewind (ex. max 250–500 ms) pour éviter les
   abus ; ne pas rembobiner les joueurs morts/spectateurs.

---

## 4. CoD2rev comme référence

`CoD2rev_Server` = réimplémentation complète du serveur CoD2 1.3 (binaire
réécrit from scratch), open-source.

- Blueprint d'algorithme : montre en C++ lisible le ring buffer, le
  rewind/restore, le calcul du temps, le trace rembobiné. Comme CoD1 et CoD2
  partagent la base id Tech 3, structures et concepts sont proches.
- Pas du copier-coller : autre jeu + réimplémentation (≠ hook). Sa lag comp
  est liée à ses structures CoD2. On adapte l'algo, pas le code.

À récupérer : le GitHub CoD2rev (à étudier : sa fonction antilag, sa struct
d'historique, son timing).

Voir aussi : `CoD4x_Server`, `zk_libcod` (mentionnés dans le README cod2x) — autres
implémentations de référence.

---

## 5. Architecture cible

```
   cod_lnxded (serveur CoD1 Linux, binaire d'origine)
        |  LD_PRELOAD
   cod1plus.so  <-- on ajoute la lag comp ICI (hook)
        - ring buffer historique positions (par frame serveur)
        - hook fire/trace : rewind -> trace -> restore
```

- Serveur uniquement. Le client `cod1reloaded` n'est pas concerné.
- Approche = hook dans le binaire d'origine (comme le stats tracker actuel),
  pas une réimplémentation.

---

## 6. RE nécessaire sur `cod_lnxded`

Le dur : localiser dans le serveur CoD1 Linux les points à brancher.

| À trouver | Pourquoi | Notes |
|---|---|---|
| `G_RunFrame` (ou la boucle de frame serveur) | capturer l'historique chaque frame | hook d'entrée/sortie |
| Fonction de tir / bullet trace (FireWeapon / Bullet_Fire) | injecter rewind avant / restore après | cœur de l'antilag |
| `G_Trace` / système de collision | le trace doit voir les positions rembobinées | |
| Struct entity/client : offset origin (+ hitbox/bones) | lire/écrire les positions | comparer avec gamex86.dll Windows |
| Latence / command time par client | calculer le temps de visée du tireur | ping serveur ou cl cmd time |
| `serverTime` global | timestamper l'historique | |

Les offsets du serveur Linux (`cod_lnxded`) diffèrent du `gamex86.dll`
Windows. Le `gamex86.dll` peut servir d'indice (même logique de jeu) mais la
cible réelle est le binaire Linux.

---

## 7. Plan d'implémentation (étapes)

1. Étudier CoD2rev : extraire l'algo exact (struct historique, rewind/restore,
   timing, clamp).
2. RE `cod_lnxded` : localiser G_RunFrame, FireWeapon/trace, G_Trace, struct
   client (origin/hitbox), latence, serverTime.
3. Ring buffer : capter les positions de tous les joueurs à chaque frame
   (hook G_RunFrame). Valider via logs.
4. Rewind/restore : hook autour du trace de tir ; rembobiner les autres au
   temps de visée, tracer, restaurer. Garde-fous (clamp, joueurs valides).
5. Diagnostics : logger temps de visée, frames rembobinées, deltas — pour
   tuner et vérifier.
6. Tests live itératifs sur le serveur, avec joueurs à pings variés.
7. Toggle : un cvar type `sv_antilag 0/1` pour activer/désactiver à chaud.

---

## 8. Dépendances / ce qu'il faut

- [ ] GitHub CoD2rev_Server (référence algorithme).
- [ ] Binaire `cod_lnxded` (serveur CoD1 Linux) à désassembler.
- [ ] Serveur de test + joueurs testeurs (pings variés) — validation.
- [ ] Accès au repo serveur `cod1plushookserver` (là où le code irait).

---

## 9. Caveats & risques

- Non testable côté Claude : serveur Linux + vrais joueurs requis. La lag comp
  touche le hit-reg de tout le monde → validation forcément chez l'utilisateur,
  en plusieurs passes.
- RE profond : succès dépend de la localisation correcte du fire/trace/struct
  client dans `cod_lnxded`.
- Subtil : timing (combien rembobiner exactement), interpolation, hitbox vs
  origin. Plusieurs itérations attendues.
- Pas plug & play : on peut avancer sérieusement et probablement sortir
  quelque chose qui marche, mais pas de promesse turnkey.
- Cohérence prédiction client : le client CoD1 prédit avec sa propre logique ;
  la lag comp serveur doit rester cohérente (pas de téléportation visible).

---

## 10. Références

- CoD2rev_Server — https://github.com/voron00/CoD2rev_Server
- CoD4x_Server — https://github.com/callofduty4x/CoD4x_Server
- zk_libcod — https://github.com/ibuddieat/zk_libcod
- Enemy-Territory (id Software) — base id Tech 3, unlag de référence
- Concept Quake3 "unlag" / backward reconciliation (Neil/“Ratbert” lag comp)

---

*Créé pendant la session cod1reloaded. À déplacer éventuellement vers le repo
serveur `cod1plushookserver` puisque la feature est server-side.*
