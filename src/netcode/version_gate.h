#ifndef COD1RELOADED_VERSION_GATE_H
#define COD1RELOADED_VERSION_GATE_H

#include <windows.h>

namespace patches {

struct VersionGateConfig {
    bool enable = true;
    bool allow_unversioned = true; // allow client cod1reloaded==0 (bots / non-cvar clients)
};

extern VersionGateConfig g_version_gate_config;

// Spawn a watcher; when game_mp_x86.dll loads, redirect the ClientConnect call so
// the server rejects clients whose "cod1reloaded" userinfo version is below ours.
void version_gate_start();

}  // namespace patches

#endif
