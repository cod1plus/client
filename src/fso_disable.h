#ifndef COD1RELOADED_FSO_DISABLE_H
#define COD1RELOADED_FSO_DISABLE_H

#include <windows.h>

namespace patches {

struct FsoDisableConfig {
    // When true, write the AppCompatFlags registry entry that disables
    // Windows 10/11 "Fullscreen Optimization" for CoDMP.exe.
    //
    // Effect applies at the NEXT launch (we can't change FSO state mid-run).
    bool enable;
};

extern FsoDisableConfig g_fso_disable_config;

// Write the registry tweak. Idempotent.
void fso_disable_apply();

}  // namespace patches

#endif
