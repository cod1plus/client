# cod1reloaded — Changelog

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
