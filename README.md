# cod1reloaded

Mod client pour **Call of Duty 1 multijoueur** (`CoDMP.exe`). Il modernise le jeu
sans rien casser : fluidité, FPS stable, support écran large, alt-tab instantané,
et corrections d'animation. Aucun launcher : on dépose un fichier dans le dossier
du jeu et c'est tout. Le mod se **met à jour tout seul**.

---

## ⚡ Installation (3 étapes)

**1. Télécharge la dernière version**

Récupère le fichier `cod1reloaded-X.X.X.zip` sur la page des releases :

👉 **https://github.com/cod1plus/client/releases/latest**

Décompresse-le : tu obtiens **`mss32.dll`** et **`cod1reloaded.ini`**.

**2. Ouvre le dossier du jeu** (celui qui contient `CoDMP.exe`)

- Steam : `…\steamapps\common\Call of Duty\`
- Version boîte : `C:\Program Files (x86)\Call of Duty\`

Dans ce dossier, **renomme le `mss32.dll` existant en `mss32_original.dll`**.

> ⚠️ **Étape essentielle.** Notre `mss32.dll` réutilise l'ancien pour le son.
> Si tu sautes cette étape, le jeu n'aura plus de son (ou ne démarrera pas).

**3. Copie les 2 fichiers**

Place **`mss32.dll`** et **`cod1reloaded.ini`** dans le dossier du jeu, puis lance
**`CoDMP.exe`** normalement.

C'est terminé. ✅

Au final, le dossier doit contenir :

| Fichier | Rôle |
|---|---|
| `mss32.dll` | le mod (notre fichier) |
| `mss32_original.dll` | l'ancien `mss32.dll`, renommé (le son) |
| `cod1reloaded.ini` | la configuration |

---

## 🔄 Mises à jour automatiques

Rien à faire. À chaque lancement, le mod vérifie s'il existe une version plus
récente. Si oui, il la télécharge en arrière-plan et **l'applique au prochain
démarrage** du jeu.

> Pour désactiver : mets `updater_enable = false` dans `cod1reloaded.ini`.

---

## ✨ Ce que ça améliore

- **Fluidité** — supprime les micro-saccades (épingle le jeu sur les bons cœurs CPU, priorité haute, anti-throttling).
- **FPS stable** — limiteur de frames précis + timer Windows 1 ms : avec `com_maxfps 250`, tu as vraiment 250 (et pas 240–248).
- **Écran large / FOV Hor+** — corrige le 4:3 forcé de CoD1 ; tu vois enfin plus large en 16:9 / 21:9 au lieu d'une image étirée.
- **Alt-tab instantané** — mode fenêtré sans bordure (fake fullscreen), plus d'écran noir au changement de fenêtre.
- **Corrections d'animation** — posture et hauteur de vue plus propres.
- **Version perso** — texte affiché dans le coin du menu principal.

> Compatible PunkBuster : on ne touche pas aux dvars sensibles, juste à la logique
> interne du moteur.

---

## ⚙️ Configuration (optionnel)

Tout est réglable dans **`cod1reloaded.ini`** (chaque option est commentée).
Modifie le fichier, enregistre, relance le jeu. Les réglages par défaut
conviennent à la plupart des joueurs — tu peux laisser tel quel.

---

## 🗑️ Désinstallation

1. Supprime notre `mss32.dll`.
2. Renomme `mss32_original.dll` → `mss32.dll`.
3. (optionnel) Supprime `cod1reloaded.ini`.

Le jeu revient à l'état d'origine.

---

## ❓ Problèmes courants

**Plus de son / le jeu ne démarre pas**
→ Tu as probablement oublié l'étape 2 : l'ancien `mss32.dll` doit être renommé en
`mss32_original.dll` et rester dans le dossier.

**L'antivirus bloque le `.dll`**
→ Faux positif classique pour ce type de mod (DLL chargée par le jeu).
Autorise le fichier / ajoute une exception.

**La mise à jour ne s'applique pas**
→ Normal au premier lancement : elle se télécharge puis s'applique au lancement
**suivant**. Relance le jeu une fois de plus.

**Je ne trouve pas le dossier du jeu**
→ Sur Steam : clic droit sur le jeu → *Gérer* → *Parcourir les fichiers locaux*.

---

## Prérequis

- **Call of Duty (2003)**, mode multijoueur (`CoDMP.exe`), patché en **1.5**.
- Windows.
