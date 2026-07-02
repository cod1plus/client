#ifndef COD1RELOADED_UPDATER_H
#define COD1RELOADED_UPDATER_H

#include <windows.h>

namespace patches {

constexpr const char* COD1RELOADED_VERSION = "1.6.1";

struct UpdaterConfig {
    bool        enable;
    char        manifest_url[512];  // empty = disabled
    bool        auto_download;
    bool        show_dialog;
};

extern UpdaterConfig g_updater_config;

void updater_start();          // non-blocking, after logger::init

// rename .dll.new -> .dll; call early before anything uses the DLL
void updater_apply_pending();

}  // namespace patches

#endif
