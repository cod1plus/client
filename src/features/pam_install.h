#ifndef COD1RELOADED_PAM_INSTALL_H
#define COD1RELOADED_PAM_INSTALL_H

// "INSTALL / UPDATE PAM" from the 1.6X menu: fetch the competitive mod's pk3s without
// joining a server (the engine's own download runs at connect time, one server at a
// time, and it is what makes a first join take minutes).
//
// Source of truth = a text manifest published by the PAM repo
// (https://github.com/cod1plus/cod1pluspam, tools/make_manifest.py):
//     mod <folder>                               e.g. __rPAMv115b5
//     version <n>                                informative
//     file <name> <bytes> <sha256> <url>         one per pk3
//     remove <name>                              stale pk3 to delete (optional)
// The manifest is generated from the pk3s a SERVER runs, so what lands here passes
// that server's sv_pure check byte for byte.
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
    char manifest_url[256];   // ini: pam_manifest_url
};
extern PamInstallConfig g_pam_install_config;

enum PamState { PAM_IDLE = 0, PAM_CHECKING = 1, PAM_DOWNLOADING = 2, PAM_DONE = 3, PAM_ERROR = 4, PAM_RESTART = 5 };

struct PamStatus {
    int   state;            // PamState
    char  text[160];        // one line for the menu
    float progress;         // 0..1 over the whole job (bytes), valid while DOWNLOADING
    int   files_done, files_total;
    char  mod[64];          // folder name once the manifest is known
};

void pam_install_start();                 // menu button; no-op while a job runs
void pam_install_status(PamStatus* out);  // snapshot for the menu (any thread)
void pam_install_swap_pending();          // DllMain: apply <mod>\*.pk3.new left by a locked install
bool pam_install_running();

}  // namespace patches

#endif
