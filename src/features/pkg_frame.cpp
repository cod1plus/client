// pkg_frame.cpp - the main-thread side of the package installer (features/pam_install.h).
//
// The download runs on a worker; two things need the engine and so the main thread:
//   - the finished job's `cvar` lines (seta com_hunkMegs 512 for the HD textures). They
//     used to be applied by the menu while its Files tab was drawn: close the menu during
//     a 6 GB download and the cvar was never set.
//   - `set cod1x_install hdtex` / `set cod1x_install pam` (console or command line): the
//     same job as the menu button, without the menu.
#include "features/pam_install.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA
#include "core/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace patches {

namespace {
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef void (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
constexpr int CV_STRING = 0x04;
constexpr int EXEC_APPEND = 2;
const char* kCvar = "cod1x_install";

int g_last_state[PKG_COUNT] = {};
}  // namespace

void pkg_install_frame() {
    static DWORD s_last = 0;
    const DWORD now = GetTickCount();
    if (now - s_last < 250) return;
    s_last = now;

    // the console / command-line trigger
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(kCvar);
    if (!cv) {
        static bool s_made = false;                 // register it once, empty
        if (!s_made) { s_made = true; ((Cvar_Set_t)CODMP_CVAR_SET_VA)(kCvar, ""); }
    } else {
        const char* v = *(const char**)((char*)cv + CV_STRING);
        if (v && v[0]) {
            char what[16];
            snprintf(what, sizeof(what), "%s", v);
            ((Cvar_Set_t)CODMP_CVAR_SET_VA)(kCvar, "");
            if (!_stricmp(what, "hdtex"))    { logger::logf("pkg_install: cod1x_install hdtex"); pkg_install_start(PKG_HDTEX); }
            else if (!_stricmp(what, "pam")) { logger::logf("pkg_install: cod1x_install pam");   pkg_install_start(PKG_PAM); }
            else logger::logf("pkg_install: cod1x_install %s - unknown package (hdtex, pam)", what);
        }
    }

    // finished jobs: their cvars, and one line in the log for whoever started it blind
    for (int id = 0; id < PKG_COUNT; ++id) {
        PamStatus ps;
        pkg_install_status((PkgId)id, &ps);
        if (ps.state == PAM_DONE || ps.state == PAM_RESTART) {
            PkgCvar cvs[PKG_MAX_CVARS];
            const int n = pkg_install_take_cvars((PkgId)id, cvs, PKG_MAX_CVARS);
            for (int i = 0; i < n; ++i) {
                char line[128];
                snprintf(line, sizeof(line), "seta %s %s\n", cvs[i].name, cvs[i].value);
                ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(EXEC_APPEND, line);
                logger::logf("pkg_install: package cvar applied: seta %s %s", cvs[i].name, cvs[i].value);
            }
        }
        if (ps.state != g_last_state[id]) {
            g_last_state[id] = ps.state;
            if (ps.state == PAM_ERROR) logger::logf("pkg_install: package %d failed: %s", id, ps.text);
        }
    }
}

}  // namespace patches
