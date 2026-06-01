#ifndef COD1RELOADED_FPS_CAP_H
#define COD1RELOADED_FPS_CAP_H

#include <windows.h>

namespace patches {

struct FpsCapConfig {
    // Si true, force la resolution du timer systeme a 1ms via
    // timeBeginPeriod(1). C'est ce qui permet a Sleep() d'etre precis a
    // 1ms et donc au moteur de cap correctement com_maxfps.
    // Sans ca, le timer Windows par defaut est ~15.6ms -> framerate erratique.
    bool force_1ms_timer;
};

extern FpsCapConfig g_fps_cap_config;

// Active la haute resolution du timer. A appeler tot dans DllMain.
void fps_cap_init();

// Restore la resolution par defaut. A appeler dans DLL_PROCESS_DETACH.
void fps_cap_shutdown();

}  // namespace patches

#endif
