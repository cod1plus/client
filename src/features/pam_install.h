#ifndef COD1RELOADED_PAM_INSTALL_H
#define COD1RELOADED_PAM_INSTALL_H

// "INSTALL / UPDATE PAM" and "INSTALL HD TEXTURES" from the 1.6X menu: fetch a
// package of pk3s without joining a server (the engine's own download runs at connect
// time, one server at a time, and it is what makes a first join take minutes).
//
// One mechanism, several PACKAGES (PkgId): each has its own manifest URL, status and
// worker; the menu shows one button per package. Source of truth = a text manifest
// published by the package's repo (cod1pluspam/tools/make_manifest.py writes it):
//     mod <folder>                               target folder: __rPAMv115b5, main, ...
//     version <n>                                informative
//     file <name> <bytes> <sha256> <url>         one per pk3
//     remove <name>                              stale pk3 to delete (optional)
//     cvar <name> <value>                        applied (seta) once the install is done
//                                                (optional; e.g. com_hunkMegs 512 for a
//                                                texture pack the default hunk cannot hold)
// A PAM manifest is generated from the pk3s a SERVER runs, so what lands here passes
// that server's sv_pure check byte for byte. The same holds for a texture pack in
// main\: on a pure server the pk3 must ALSO sit on the server, or the client is
// dropped as unpure the moment it loads a texture from it.
//
// A pk3 is a zip the engine reads as-is: nothing to unpack, each file goes straight to
// <game dir>\<mod>\<name>. Download to <name>.part, verify size + SHA-256, rename over.
// A file already present with the right hash is skipped, so the same button updates.
// A file the engine holds open (the mod is the active fs_game) cannot be replaced:
// it is left as <name>.new and swapped in at the next launch (pam_install_swap_pending,
// DllMain, before the engine opens any pak).
//
// HONEST LIMIT: nothing here talks to the server; a manifest that lags the server's
// pk3s just means the engine downloads the difference at connect, as before.

#include <windows.h>

namespace patches {

struct PamInstallConfig {
    bool enable;
    char manifest_url[256];   // ini: pam_manifest_url / hdtex_manifest_url
};
extern PamInstallConfig g_pam_install_config;     // the competitive mod
extern PamInstallConfig g_hdtex_install_config;   // HD texture pack -> main\

enum PkgId { PKG_PAM = 0, PKG_HDTEX = 1, PKG_COUNT = 2 };

enum PamState { PAM_IDLE = 0, PAM_CHECKING = 1, PAM_DOWNLOADING = 2, PAM_DONE = 3, PAM_ERROR = 4, PAM_RESTART = 5 };

struct PamStatus {
    int   state;            // PamState
    char  text[160];        // one line for the menu
    float progress;         // 0..1 over the whole job (bytes), valid while DOWNLOADING
    int   files_done, files_total;
    char  mod[64];          // folder name once the manifest is known
};

struct PkgCvar { char name[48]; char value[48]; };
constexpr int PKG_MAX_CVARS = 4;

void pkg_install_start(PkgId id);                      // menu button; no-op while that job runs
void pkg_install_status(PkgId id, PamStatus* out);     // snapshot for the menu (any thread)
bool pkg_install_running(PkgId id);
// The manifest's `cvar` lines of a finished job, handed out ONCE (cleared on return) so
// the menu can apply them on the main thread. Returns the count.
int  pkg_install_take_cvars(PkgId id, PkgCvar* out, int max);

// PAM shorthands (the original API, kept for the callers and the host test)
inline void pam_install_start()                { pkg_install_start(PKG_PAM); }
inline void pam_install_status(PamStatus* out) { pkg_install_status(PKG_PAM, out); }
inline bool pam_install_running()              { return pkg_install_running(PKG_PAM); }
void pam_install_swap_pending();          // DllMain: apply <folder>\*.pk3.new left by a locked install

}  // namespace patches

#endif
