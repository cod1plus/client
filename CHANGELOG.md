# cod1reloaded — Changelog

## v1.6.6 (2026-09-18)

### 📦 Bouton « INSTALL / UPDATE PAM » (menu 1.6X, onglet Files)
- Télécharge les pk3 du mod compétitif (maps comprises) sans rejoindre de serveur, depuis le manifeste
  publié par https://github.com/cod1plus/cod1pluspam : vérification SHA-256, fichiers déjà bons sautés
  (le même bouton met à jour), pk3 en cours d'utilisation posé en `.new` et échangé au lancement suivant.
  Clés ini `pam_download_enable`, `pam_manifest_url`.

### 🖥️ Mode Stretched 4:3 (la façon cod2x)
- Display → View mode → **Stretched 4:3** : le look 1440×1080 étiré, rendu par le mod en résolution native
  (FOV vertical 4:3 sur le buffer 16:9, HUD étiré comme avant). Le menu 1.6X écrivait une cvar morte
  (`cod1x_widescreen`) et n'offrait pas ce mode ; un changement de mode s'applique désormais en direct.
- Le plafond moteur de `r_displayRefresh` (200 Hz, `Cvar_CheckRange` dans R_Register) est levé : 320 Hz
  et plus passent, et un mode refusé retombe sur le plus haut listé, plus sur « le plus haut en dessous ».

### 🖥️ Hz maximum quelle que soit la résolution
- Une résolution custom n'existe qu'avec les fréquences que le pilote lui a données (1440×1080 listée
  à 180 Hz max sur un écran 320 Hz) : avec `refresh_rate = max`, le mod passe le jeu à la résolution
  qui a le maximum et fait lui-même l'étirement 4:3 (`view_mode = stretched`). `max_hz_native_res = off`
  pour garder sa résolution custom.

### ⌨️ Menu 1.6X : Ctrl+M uniquement
- INSERT n'ouvre plus le menu (touche que les joueurs bindent, et qui partait en pleine visée) ;
  Ctrl+M ou le bouton « 1.6X SETTINGS » du menu principal. INSERT redevient bindable.

### 👁️ FOV plafonné à 95
- Le slider du menu 1.6X va de 80 à 95, et `cg_fov` est ramené dans cette plage en direct quelle que
  soit la source (console, config_mp.cfg). Les serveurs (`competitive.cfg`) appliquent la même plage.

### 🛡️ Ruleset PunkBuster (remplaçant de PB)
- Le client applique lui-même la liste CoDBase complète (~430 cvars) demandée par le serveur
  (`sv_competitive_ruleset`) : exact → forcé + verrouillé, plage → ramené dans la plage, `wait` neutralisé.
- La liste est téléchargée depuis https://github.com/cod1plus/rulesets au lancement puis toutes les
  10 min, et immédiatement quand un serveur demande une version plus récente (`<id>@<version>`) ;
  cache dans `rulesets\`, copie compilée en secours hors-ligne (`ruleset_fetch_enable`, `ruleset_url`).
- Aucune mise à jour serveur requise : un serveur qui pousse un `competitive.cfg` (tous les `.so`
  1.6.x) déclenche la liste par défaut `codbase-2023-05`. Le `.so` 1.6.6 ajoute le choix de l'id par
  serveur, la poussée immédiate d'une version et le kick des clients qui combattent l'enforcement.
- Le `competitive.cfg` du serveur garde la priorité pour les cvars qu'il nomme, y compris à chaud.
- La commande `wait` reste **autorisée** : les nade binds de CoD1 sont construits dessus (switch grenade,
  cook, throw, switch back). Le ruleset encadre les valeurs de cvars, pas la console.
- Corrigé : en 1.6.5 aucune règle à 0, négative ou flottante du `competitive.cfg` n'était appliquée
  (`r_fullbright 0`, `m_yaw 0.022`, `cl_timenudge -20 0`…).
- Corrigé : les cvars systeminfo gardaient la valeur du serveur précédent (spec et ruleset lus dans
  la configstring courante, plus dans la cvar).

## v1.6.4 (2026-08-02)

### 🧍 Up / down
- **Retour au comportement de la 1.6.0**, celui qui avait été validé au lancement.
  En 1.6.3 le modèle suivait de trop près un joueur qui spamme accroupi, et ça se
  lisait comme du clignotement (« on voit le joueur briller »). Un modèle lent et
  lisible est justement ce qui rend l'abus d'up-down punissable.
- **Correction du limiteur de FPS** : il renvoyait au moteur une horloge théorique
  au lieu du temps réel. C'est l'horloge qui sert à l'interpolation des snapshots,
  au blending d'animation et aux transitions de posture — elle dérivait de la
  timeline réseau et faisait scintiller les modèles adverses, y compris ceux qui
  ne spammaient rien.

### ⌨️ Menu 1.6X
- **La touche INSERT est totalement supprimée.** Le sondage clavier global se
  déclenchait pendant que tu vises, tires ou écris. Le menu s'ouvre désormais
  uniquement depuis le MENU PRINCIPAL.

### 🖱️ Raw input souris (`m_rinput`)
- **Portage du `m_rinput` de cod2x.** La souris est lue directement sur le
  périphérique au lieu de passer par le curseur Windows : plus d'accélération
  pointeur, plus de dépendance au curseur du bureau, plus d'arrondi au pixel. Ta
  sensibilité en jeu ne change pas, c'est la couche Windows qui disparaît.
- `m_rinput 0/1` (sauvegardé dans ta config) ou `raw_mouse_input` dans
  `cod1reloaded.ini`. **Désactivé par défaut** : ça modifie la visée, personne ne
  doit se le prendre sans l'avoir choisi.
- `m_rinput_hz` affiche le **taux de sondage réel** de ta souris (500, 1000…), et
  `m_rinput_hz_max` le maximum vu — de quoi vérifier qu'une souris annoncée à
  1000 Hz tient vraiment 1000 Hz.

### 🖥️ Résolutions personnalisées / stretch
- **Correction** : une résolution personnalisée plus grande que ton bureau
  (2128x1330, 1776x1332… sur un écran 1080p) était tout simplement ignorée. Seul
  le plein écran exclusif peut changer le mode d'affichage ; le mod démarrait en
  fenêtre sans bordure, où une telle résolution ne peut pas tenir. Le mod détecte
  maintenant le cas au lancement et **repasse en plein écran tout seul**.
- Une résolution plus **petite** que le bureau (1440x1080 par exemple) fonctionne
  en fenêtre mais n'est **pas étirée par le GPU** : le mod l'écrit désormais dans
  `cod1reloaded.log` au lieu de laisser chercher.
- `cod1reloaded.ini` explique la règle en clair dans la section DISPLAY.

### 🔒 Règles de jeu équitable
- Le serveur peut désormais pousser une **liste de réglages beaucoup plus longue**
  (découpée en plusieurs cvars). Au-delà de ~250 caractères l'ancienne version
  arrêtait le serveur au changement de map.

## v1.6.3 (2026-07-31)

### 🎯 Hitbox des joueurs penchés
- **Refonte complète du calcul des impacts sur un joueur qui lean.** Le squelette
  testé par le serveur est désormais posé à chaque tir et aligné sur le modèle que
  tu vois à l'écran : tirer sur la tête visible d'un joueur penché compte enfin
  comme un headshot, debout comme accroupi.
- **Fini les impacts fantômes** : plus de dégâts à côté du corps, plus de tir qui
  compte sur la silhouette « debout » d'un joueur penché, plus de balle qui touche
  à côté des jambes.
- **Plus de balles avalées** : un tir qui touche vraiment le corps ne peut plus
  être annulé en silence.
- Le tireur ne peut plus se toucher lui-même en leanant ; les joueurs à plat ventre
  et les morts ne bloquent plus les balles.

### ⏱️ Limiteur de FPS
- **Cadence réellement stable** à la valeur choisie (250 ou 125) : fini le
  248-250 qui oscillait, et le scénario de jeu ne « scintille » plus.

### 🔒 Client ↔ serveur
- **Correction majeure** : les informations envoyées par le client au serveur
  (version du mod, rapport anti-triche) n'arrivaient jamais à destination — un
  drapeau interne erroné les publiait dans le mauvais canal. La vérification de
  version et le contrôle anti-triche côté serveur sont donc **opérationnels pour
  la première fois**.
- Le serveur peut suivre automatiquement la dernière version publiée sur GitHub et
  refuser les clients périmés, sans intervention manuelle.
- **Règles de jeu équitable** poussées par le serveur à tous les clients du mod
  (fps, snaps, cl_maxpackets, rate, réglages graphiques avantageux…).

## v1.6 — BÊTA DE TEST (2026-06-30)

> ⚠️ **Build BÊTA destinée aux tests communautaires.** Merci de remonter tout bug
> (glitch de modèle, souci de connexion, crash) sur le Discord. Certaines fonctions
> sont expérimentales — voir « Limitations connues » plus bas.

### 🌐 Nouvel écosystème réseau
- **Protocole réseau 6 → 10** : cod1reloaded devient un écosystème **séparé** de CoD1
  vanilla. Les clients et serveurs proto-10 ne communiquent qu'entre eux.
- **Nouveau master server** (le master Activision officiel est mort) → la **liste de
  serveurs Internet refonctionne**.
- **Vérification de version client** : un serveur cod1reloaded rejette les clients
  trop anciens (cvar userinfo `cod1reloaded`).
- Version affichée dans le menu → **1.6**.

### 🎮 Modèle joueur (portage CoD2x)
- **Buste droit** en marche/visée + lean latéral propre — fini le « piqué » avant
  quand on avance en leanant.
- **Arme et torse verrouillés sur la vue** — plus de lag jambes/torse (swing fix).
- **Lissage synchronisé du contrôleur** (`ctrl_smooth`) — plus de saccade du modèle
  quand la direction de mouvement change.
- **Fix anti lean-spam / « clip »** : le lean du modèle est limité en vitesse
  (~1,4°/frame, velocity-clamp porté de CoD2x) → spammer la touche lean ne fait
  **plus clignoter ton modèle** pour les autres joueurs.
- **Persistance au changement de map** : les fix de modèle ne disparaissent plus
  après une rotation de map ou un `/devmap`.

### 🖥️ Affichage
- **FOV Hor+ widescreen** : vrai FOV sur écran large (fini l'étirement vertical).
- **Fenêtre borderless** : alt-tab instantané, sans freeze d'écran noir.

### ⚡ Fluidité / performance (anti-microstutter)
- Timer **1 ms** (com_maxfps précis) + **frame limiter** à la microseconde (FPS cap exact).
- Affinité CPU, priorité process, lock du working set, désactivation du
  Fullscreen Optimization Windows.

### 🔌 Intégrations
- **Auto-updater** (récupère les nouvelles versions automatiquement).
- **Discord Rich Presence** (optionnel).
- Upload auto de démos + overlay avatar (POC, désactivés par défaut).

### ⚠️ Limitations connues (bêta)
- **Antilag** (lag compensation) : expérimental, côté serveur, **désactivé** dans
  cette build.
- **Crouch-lean corner peek (headclip)** : pas encore empêché — prévu pour une
  prochaine version. Le fix de cette build couvre le *lean-spam*, **pas** le peek
  de coin accroupi.
- C'est une **bêta** : merci de tester et de remonter tout comportement anormal.

### 🎯 À tester en priorité
- Le **modèle des autres joueurs** : lean gauche/droite, strafe + lean, et surtout
  le **spam de lean** → tout doit rester lisse et naturel (pas de clignotement).
- La **liste Internet** : ton serveur apparaît bien, tu peux t'y connecter.
- **Stabilité** : changements de map, longues sessions, alt-tab.
