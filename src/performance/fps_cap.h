#ifndef COD1RELOADED_FPS_CAP_H
#define COD1RELOADED_FPS_CAP_H

#include <windows.h>

namespace patches {

struct FpsCapConfig {
    bool force_1ms_timer;
};

extern FpsCapConfig g_fps_cap_config;

void fps_cap_init();      // call early in DllMain
void fps_cap_shutdown();  // call in DLL_PROCESS_DETACH

}  // namespace patches

#endif
