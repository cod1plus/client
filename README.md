# cod1reloaded

A client mod for **Call of Duty 1 multiplayer** (`CoDMP.exe`). It modernizes the
game without breaking anything: smoother frames, stable FPS, widescreen support,
instant alt-tab, and animation fixes. No launcher — you just drop one file into
the game folder. The mod **updates itself**.

---

## ⚡ Installation (3 steps)

**1. Download the latest version**

Grab `cod1reloaded-X.X.X.zip` from the releases page:

👉 **https://github.com/cod1plus/client/releases/latest**

Unzip it: you get **`mss32.dll`** and **`cod1reloaded.ini`**.

**2. Open the game folder** (the one with `CoDMP.exe`)

- Steam: `…\steamapps\common\Call of Duty\`
- Retail: `C:\Program Files (x86)\Call of Duty\`

In that folder, **rename the existing `mss32.dll` to `mss32_original.dll`**.

> ⚠️ **Don't skip this.** Our `mss32.dll` reuses the original one for audio.
> If you skip it, the game loses sound (or won't start).

**3. Copy the two files**

Put **`mss32.dll`** and **`cod1reloaded.ini`** into the game folder, then launch
**`CoDMP.exe`** as usual.

Done. ✅

The folder should end up with:

| File | Purpose |
|---|---|
| `mss32.dll` | the mod (our file) |
| `mss32_original.dll` | the old `mss32.dll`, renamed (audio) |
| `cod1reloaded.ini` | the configuration |

---

## 🔄 Auto-updates

Nothing to do. On each launch the mod checks for a newer version. If there is
one, it downloads it in the background and **applies it on the next launch**.

> To disable: set `updater_enable = false` in `cod1reloaded.ini`.

---

## ✨ What it improves

- **Smoothness** — kills microstutter (pins the game to the right CPU cores, raises priority, blocks throttling).
- **Stable FPS** — precise frame limiter + 1 ms Windows timer: with `com_maxfps 250` you actually get 250 (not 240–248).
- **Widescreen / Hor+ FOV** — fixes CoD1's forced 4:3; you finally see wider on 16:9 / 21:9 instead of a stretched image.
- **Instant alt-tab** — borderless windowed (fake fullscreen), no more black screen when switching windows.
- **Animation fixes** — cleaner posture and view height.
- **Custom version** — text shown in the main menu corner.

> PunkBuster-safe: we don't touch sensitive dvars, only the engine's internal logic.

---

## ⚙️ Configuration (optional)

Everything is tunable in **`cod1reloaded.ini`** (each option is commented).
Edit, save, relaunch. The defaults are fine for most players — you can leave it
as is.

---

## 🗑️ Uninstall

1. Delete our `mss32.dll`.
2. Rename `mss32_original.dll` back to `mss32.dll`.
3. (optional) Delete `cod1reloaded.ini`.

The game is back to vanilla.

---

## ❓ Troubleshooting

**No sound / the game won't start**
→ You probably skipped step 2: the old `mss32.dll` must be renamed to
`mss32_original.dll` and kept in the folder.

**Antivirus flags the `.dll`**
→ Common false positive for this kind of mod (a DLL loaded by the game).
Allow the file / add an exception.

**The update didn't apply**
→ Expected on the first launch: it downloads, then applies on the **next**
launch. Start the game once more.

**I can't find the game folder**
→ On Steam: right-click the game → *Manage* → *Browse local files*.

---

## Requirements

- **Call of Duty (2003)**, multiplayer (`CoDMP.exe`), patched to **1.5**.
- Windows.
