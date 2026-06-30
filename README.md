# cod1reloaded

A client mod for **Call of Duty 1 multiplayer** (`CoDMP.exe`). It modernizes the
game without breaking anything: a working server browser again, smoother frames,
stable FPS, widescreen support, instant alt-tab, and competitive player-model
fixes ported from CoD2x. No launcher — you drop one file into the game folder,
and the mod **updates itself**.

> **Status: 1.6 — beta.** See [CHANGELOG.md](CHANGELOG.md).

---

## ⚡ Installation (3 steps)

**1. Download the latest version**

Grab the release from:

👉 **https://github.com/cod1plus/client/releases**

You get **`mss32.dll`** and **`cod1reloaded.ini`**.

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

## 🌐 The cod1reloaded ecosystem (new in 1.6)

cod1reloaded runs on its **own network protocol (10)**, separate from vanilla
CoD1. Concretely:

- The dead Activision master server is **replaced**, so the in-game **Internet
  server browser works again**.
- Servers **verify** that everyone runs a compatible client (version gate).
- Everyone plays with the **same model fixes**, so the playing field is fair.

> A 1.6 client connects **only to cod1reloaded servers** (protocol 10), not to old
> vanilla protocol-6 servers. That separation is intentional.

---

## 🔄 Auto-updates

Nothing to do. On each launch the mod checks for a newer version. If there is
one, it downloads it in the background and **applies it on the next launch**.

> To disable: set `updater_enable = false` in `cod1reloaded.ini`.

---

## ✨ What it improves

- **Working server browser** — new master server, so the Internet tab lists
  cod1reloaded servers again (the official one died years ago).
- **Player model fixes** (ported from CoD2x) — **upright torso** when
  moving/aiming, clean lateral lean, **weapon locked to the view** (no leg/torso
  lag), and **anti lean-spam**: spamming the lean key no longer makes your model
  flicker for opponents — it stays smooth and trackable. Also fixes the crouch
  view-height desync. The fixes **persist across map changes**.
- **Smoothness** — kills microstutter (pins the game to the right CPU cores,
  raises priority, blocks throttling).
- **Stable FPS** — precise frame limiter + 1 ms Windows timer: with
  `com_maxfps 250` you actually get 250 (not 240–248).
- **Widescreen / Hor+ FOV** — fixes CoD1's forced 4:3; you finally see wider on
  16:9 / 21:9 instead of a stretched image.
- **Instant alt-tab** — borderless windowed (fake fullscreen), no more black
  screen when switching windows.
- **Custom version** — text shown in the main menu corner.

> PunkBuster-safe: we don't touch sensitive dvars, only the engine's internal logic.

---

## ⚙️ Configuration (optional)

Everything is tunable in **`cod1reloaded.ini`** (each option is commented).
Edit, save, relaunch. The defaults are fine for most players.

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

**No servers in the Internet list**
→ Make sure you're on 1.6 (it queries the new master). The browser only lists
cod1reloaded (protocol-10) servers. Direct-connect (`/connect ip`) always works.

**Antivirus flags the `.dll`**
→ Common false positive for this kind of mod (a DLL loaded by the game).
Allow the file / add an exception.

**The update didn't apply**
→ Expected on the first launch: it downloads, then applies on the **next**
launch. Start the game once more.

---

## 🛠️ Building from source

Requires a **32-bit** MinGW (`i686-w64-mingw32`):

```sh
cmake -B build -G "MinGW Makefiles"
cmake --build build
# -> build/mss32.dll
```

Source is organized by domain under `src/`:

| Folder | Contents |
|---|---|
| `core/` | entry point, config loader, logging |
| `gameplay/` | lean, swing, viewheight (player-model fixes) |
| `netcode/` | protocol, master server, version gate, antilag |
| `video/` | Hor+ FOV, windowing |
| `performance/` | FPS cap, frame limiter, CPU/priority/working-set |
| `features/` | auto-updater, Discord RPC, demo upload, avatar overlay |

It builds as `mss32.dll` (an mss32 proxy) — see [docs/](docs/) for the
reverse-engineering notes.

---

## Requirements

- **Call of Duty (2003)**, multiplayer (`CoDMP.exe`), patched to **1.5**.
- Windows.
