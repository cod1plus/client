# COD1.6X

**COD1.6X** is a client mod for **Call of Duty 1 multiplayer** (`CoDMP.exe`). It
modernizes the 2003 game without breaking anything: a working server browser
again, competitive player-model fixes ported from CoD2x, smooth stutter-free
frames, an exact FPS cap, real widescreen, and instant alt-tab. No launcher —
you drop one file into the game folder and the mod **updates itself**.

> **Status: 1.6.1 — beta.** See [CHANGELOG.md](CHANGELOG.md).

---

## ⚡ Installation (3 steps)

**1. Download the latest release**

👉 **https://github.com/cod1plus/client/releases/latest**

Grab **`mss32.dll`** and **`cod1reloaded.ini`** (or `cod1reloaded-x.y.z.zip`,
which contains both).

**2. Open the game folder** (the one with `CoDMP.exe`)

- Steam: `…\steamapps\common\Call of Duty\`
- Retail: `C:\Program Files (x86)\Call of Duty\`

In that folder, **rename the existing `mss32.dll` to `mss32_original.dll`**.

> ⚠️ **Don't skip this.** COD1.6X's `mss32.dll` is a proxy that forwards audio to
> the original one. If you skip the rename, the game loses sound (or won't start).

**3. Copy the two files**

Put **`mss32.dll`** and **`cod1reloaded.ini`** into the game folder, then launch
**`CoDMP.exe`** as usual. Done. ✅

The folder should end up with:

| File | Purpose |
|---|---|
| `mss32.dll` | COD1.6X (our file) |
| `mss32_original.dll` | the old `mss32.dll`, renamed (audio) |
| `cod1reloaded.ini` | the configuration |

---

## 🌐 The COD1.6X ecosystem

COD1.6X runs on its **own network protocol (10)**, separate from vanilla CoD1:

- The dead Activision master server is **replaced**, so the in-game **Internet
  server browser works again**.
- Servers **verify** that everyone runs a compatible client (version gate).
- Everyone plays with the **same model fixes**, so the playing field is fair.

> A COD1.6X client connects **only to COD1.6X servers** (protocol 10), not to old
> vanilla protocol-6 servers. That separation is intentional.

---

## ✨ What's included

### 🎮 Competitive player-model fixes (ported from CoD2x)
- **Upright torso** when moving and aiming — no more forward "dip" when you
  advance while leaning.
- **Clean lateral lean**, and the **right lean now exposes the body** properly
  instead of showing only the arm *(new in 1.6.1)*.
- **Weapon and torso locked to the view** — no leg/torso swing lag.
- **Synchronized controller smoothing** — no model jitter when your movement
  direction changes.
- **Anti lean-spam** — spamming the lean key no longer makes your model flicker
  for opponents; it stays smooth and trackable.
- **Crouch view-height** desync fixed.
- All fixes **persist across map changes** (rotation, `/devmap`).

### 🖥️ Display
- **Widescreen / Hor+ FOV** — fixes CoD1's forced 4:3; you see *wider* on 16:9 /
  21:9 instead of a stretched image. Optional forced resolution.
- **Borderless window** — instant alt-tab, no black-screen freeze.

### ⚡ Smoothness & FPS
- **Exact FPS cap** — 1 ms Windows timer + microsecond frame limiter: with
  `com_maxfps 250` you actually get 250 (not 240–248).
- **Anti-microstutter** — CPU-core affinity, raised process/thread priority,
  working-set lock, and Windows Fullscreen-Optimization disabled.

### 🌐 Network
- **Working server browser** via the new master server.
- **Protocol 10** ecosystem + client **version gate**.

### 🔌 Integrations
- **Auto-updater** — checks a manifest at launch, updates itself.
- **Discord Rich Presence** (optional — set `discord_rpc_client_id`).
- **Demo auto-upload** and **avatar overlay** — proof-of-concept, off by default.

### 🧪 Experimental
- **Antilag** (lag compensation) — server-side, **off** by default.

> PunkBuster-safe: COD1.6X doesn't touch sensitive dvars, only the engine's
> internal logic.

---

## 🔄 Auto-updates

Nothing to do. On each launch COD1.6X checks for a newer version; if there is one
it downloads in the background and **applies it on the next launch**.

> To disable: set `updater_enable = false` in `cod1reloaded.ini`.

---

## ⚙️ Configuration (optional)

Everything is tunable in **`cod1reloaded.ini`** — each option is commented. Edit,
save, relaunch. The defaults are fine for most players.

---

## 🗑️ Uninstall

1. Delete COD1.6X's `mss32.dll`.
2. Rename `mss32_original.dll` back to `mss32.dll`.
3. *(optional)* Delete `cod1reloaded.ini`.

The game is back to vanilla.

---

## ❓ Troubleshooting

**No sound / the game won't start**
→ You probably skipped step 2: the old `mss32.dll` must be renamed to
`mss32_original.dll` and kept in the folder.

**No servers in the Internet list**
→ Make sure you're on the latest version (it queries the new master). The browser
only lists COD1.6X (protocol-10) servers. Direct-connect (`/connect ip`) always
works.

**Antivirus flags the `.dll`**
→ Common false positive for this kind of mod (a DLL loaded by the game). Allow
the file / add an exception.

**The update didn't apply**
→ Expected on the first launch: it downloads, then applies on the **next** launch.
Start the game once more.

---

## 🛠️ Building from source

COD1.6X builds as an `mss32.dll` proxy with a **32-bit** MinGW toolchain
(`i686-w64-mingw32`).

**Locally (MSYS2 MinGW32):**
```sh
cmake -B build -G Ninja
cmake --build build --target mss32
# -> build/mss32.dll
```

**Reproduce the CI (Ubuntu cross-compile):**
```sh
sudo apt-get install -y g++-mingw-w64-i686 cmake
cmake -B build -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=i686-w64-mingw32-windres
cmake --build build --target mss32
```

Publishing a release on GitHub triggers the **Release** Action, which builds
`mss32.dll` and attaches it to the release automatically.

Source is organized by domain under `src/`:

| Folder | Contents |
|---|---|
| `core/` | entry point, config loader, logging |
| `gameplay/` | lean, swing, viewheight (player-model fixes) |
| `netcode/` | protocol, master server, version gate, antilag |
| `video/` | Hor+ FOV, windowing |
| `performance/` | FPS cap, frame limiter, CPU/priority/working-set |
| `features/` | auto-updater, Discord RPC, demo upload, avatar overlay |

See [docs/](docs/) for the reverse-engineering notes.

---

## Requirements

- **Call of Duty (2003)**, multiplayer (`CoDMP.exe`), patched to **1.5**.
- Windows.
