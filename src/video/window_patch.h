#ifndef COD1RELOADED_WINDOW_PATCH_H
#define COD1RELOADED_WINDOW_PATCH_H

#include <windows.h>

namespace patches {

struct WindowConfig {
    bool borderless_enable;
    bool follow_current_monitor;

    int  preferred_monitor_index;

    bool minimize_on_focus_loss;
};

extern WindowConfig g_window_config;



void start_window_watcher();



HWND get_game_window();

}  // namespace patches

#endif
