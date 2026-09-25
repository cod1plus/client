// session_report.cpp - see session_report.h
#include "core/session_report.h"
#include "core/logger.h"
#include "performance/frame_limiter.h"
#include "performance/gpu_sync.h"
#include "video/mode_guard.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"   // CODMP_CVAR_COUNT_VA

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
constexpr int CVAR_OFF_INTEGER = 0x20;

DWORD g_since = 0;          // start of the current window
bool  g_final_done = false;

bool engine_ready() {
    return (uintptr_t)GetModuleHandleA(NULL) == 0x400000 && *(volatile int*)CODMP_CVAR_COUNT_VA > 0;
}

int cvar_int(const char* name, int fallback) {
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(name);
    return cv ? *(int*)((char*)cv + CVAR_OFF_INTEGER) : fallback;
}

void report(const char* tag, DWORD now) {
    const DWORD elapsed = now - g_since;
    if (elapsed < 1000) return;

    FrameLimiterStats fs;
    frame_limiter_stats(&fs, true);
    double gpu_avg = 0, gpu_max = 0;
    long gpu_frames = 0;
    gpu_sync_stats(&gpu_avg, &gpu_max, &gpu_frames, true);

    int w = 0, h = 0, hz = 0;
    const bool exclusive = mode_guard_current_mode(&w, &h, &hz);
    if (!exclusive) {
        DEVMODEA dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
            w = (int)dm.dmPelsWidth; h = (int)dm.dmPelsHeight; hz = (int)dm.dmDisplayFrequency;
        }
    }

    char gpu[64];
    if (gpu_frames > 0)
        snprintf(gpu, sizeof(gpu), "GPU %.2f ms (max %.1f)", gpu_avg / 1000.0, gpu_max / 1000.0);
    else
        snprintf(gpu, sizeof(gpu), "GPU sync %s", gpu_sync_state());

    const double fps = fs.frames * 1000.0 / (double)elapsed;
    logger::logf("bilan %s: %.1f fps (%ld frames, %ld en retard, %ld longues, max %.1f ms) | %s | "
                 "%dx%d @ %d Hz %s | com_maxfps %d | rinput %s",
                 tag, fps, fs.frames, fs.late, fs.long_frames, fs.max_frame_ms, gpu,
                 w, h, hz, exclusive ? "exclusif" : "fenetre",
                 cvar_int("com_maxfps", 0), cvar_int("m_rinput", 0) ? "on" : "off");
}

}  // namespace

void session_report_tick() {
    if (g_final_done || !engine_ready()) return;
    const DWORD now = GetTickCount();
    if (!g_since) { g_since = now ? now : 1; frame_limiter_stats(nullptr, true); return; }
    if (now - g_since < 60000) return;
    report("60 s", now);
    g_since = now;
}

void session_report_final(const char* why) {
    if (g_final_done) return;
    g_final_done = true;
    if (!g_since || !engine_ready()) return;
    report(why, GetTickCount());
}

}  // namespace patches
