#ifndef COD1RELOADED_DEMO_UPLOAD_H
#define COD1RELOADED_DEMO_UPLOAD_H

#include <windows.h>

namespace patches {

struct DemoUploadConfig {
    bool enable;
    char upload_url[512];        // POST target, empty = off
    bool delete_after_upload;    // false = write .uploaded marker instead
    bool show_toasts;
    int  poll_interval_sec;      // 0 = scan once at startup
};

extern DemoUploadConfig g_demo_upload_config;

void demo_upload_start();

void demo_upload_trigger_now();  // wake thread now, thread-safe

}  // namespace patches

#endif
