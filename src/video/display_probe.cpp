// What resolution is this player actually going to run at, and can a WINDOW show it?
//
// Reported 2026-08-02: a player on a custom 2128x1330 (and 1776x1332) saw the game
// simply not use it, while 1440x1080 worked. Both broken values are TALLER than his
// desktop; the working one is not. That is the whole mechanism: only an exclusive
// fullscreen mode switch can present a resolution the desktop is not already in. The
// mod ships fullscreen=off + window_borderless=on, so the engine never switches mode,
// and anything bigger than the desktop renders into a window that cannot contain it.
//
// Stretched 4:3 resolutions are the norm in competitive CoD, so this has to
// self-correct instead of costing a support thread each time.

#include "video/display_probe.h"
#include "video/fullscreen_patch.h"
#include "video/widescreen_fix.h"
#include "video/window_patch.h"
#include "core/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

namespace {

// CoD1 default r_mode is 3 (640x480). Shared with settings_menu.cpp via the header so
// the two never drift apart.
constexpr int VIDMODES[][2] = {
    {320,240},{400,300},{512,384},{640,480},{800,600},{960,720},{1024,768},
    {1152,864},{1280,1024},{1600,1200},{2048,1536},{856,480}
};

bool is_sep(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; }

// Last `seta <name> <int>` in the buffer wins, as it would when the engine execs it.
// The name must stand alone so r_mode never matches a longer cvar.
bool cfg_int(const char* buf, const char* name, int* out) {
    const size_t nlen = strlen(name);
    bool found = false;
    for (const char* p = strstr(buf, name); p; p = strstr(p + nlen, name)) {
        if (p != buf && !is_sep(*(p - 1))) continue;
        const char* v = p + nlen;
        if (!is_sep(*v)) continue;
        while (*v == ' ' || *v == '\t' || *v == '"') ++v;
        if (*v != '-' && (*v < '0' || *v > '9')) continue;
        *out  = atoi(v);
        found = true;
    }
    return found;
}

bool read_config_mp(char** buf_out) {
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe_path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';

    char cfg_path[MAX_PATH];
    if (snprintf(cfg_path, sizeof(cfg_path), "%smain\\config_mp.cfg", exe_path)
        >= (int)sizeof(cfg_path)) return false;

    FILE* f = fopen(cfg_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    *buf_out = buf;
    return true;
}

}  // namespace

bool resolution_for_r_mode(int mode, int* w, int* h) {
    if (mode < 0 || mode >= (int)(sizeof(VIDMODES) / sizeof(VIDMODES[0]))) return false;
    *w = VIDMODES[mode][0];
    *h = VIDMODES[mode][1];
    return true;
}

bool probe_config_resolution(int* w, int* h) {
    char* buf = nullptr;
    if (!read_config_mp(&buf)) return false;

    int mode = 0;
    bool ok = false;
    if (cfg_int(buf, "r_mode", &mode)) {
        if (mode == -1) {
            int cw = 0, ch = 0;
            if (cfg_int(buf, "r_customwidth", &cw) && cfg_int(buf, "r_customheight", &ch) &&
                cw > 0 && ch > 0) {
                *w = cw; *h = ch; ok = true;
            }
        } else {
            ok = resolution_for_r_mode(mode, w, h);
        }
    }
    free(buf);
    return ok;
}

bool probe_desktop_resolution(int* w, int* h) {
    DEVMODEA dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) return false;
    if (dm.dmPelsWidth == 0 || dm.dmPelsHeight == 0) return false;
    *w = (int)dm.dmPelsWidth;
    *h = (int)dm.dmPelsHeight;
    return true;
}

void display_mode_guard() {
    int gw = 0, gh = 0;
    // force_resolution writes r_mode/-width/-height from the .ini through our autoexec,
    // so it overrides whatever the config currently holds.
    if (g_widescreen_config.force_resolution &&
        g_widescreen_config.width > 0 && g_widescreen_config.height > 0) {
        gw = g_widescreen_config.width;
        gh = g_widescreen_config.height;
    } else if (!probe_config_resolution(&gw, &gh)) {
        logger::logf("display_probe: no resolution in config_mp.cfg - leaving display "
                     "settings as configured");
        return;
    }

    int dw = 0, dh = 0;
    if (!probe_desktop_resolution(&dw, &dh)) {
        logger::logf("display_probe: EnumDisplaySettings failed - leaving display "
                     "settings as configured");
        return;
    }

    if (gw == dw && gh == dh) {
        logger::logf("display_probe: game %dx%d == desktop, windowed/borderless is safe",
                     gw, gh);
        return;
    }

    const bool windowed = g_fullscreen_config.force_windowed_default ||
                          g_window_config.borderless_enable;

    if (gw > dw || gh > dh) {
        // No window can hold it: the engine would build a backbuffer larger than the
        // screen and the player sees a crop of his own game, or nothing at all.
        if (windowed) {
            logger::logf("display_probe: game %dx%d is LARGER than the desktop %dx%d - "
                         "only exclusive fullscreen can show it. Forcing fullscreen=on / "
                         "window_borderless=off for this launch.", gw, gh, dw, dh);
            g_fullscreen_config.force_windowed_default = false;
            g_window_config.borderless_enable          = false;
        }
        return;
    }

    // Smaller than the desktop: a window WORKS, it just is not scaled up by the GPU, so
    // a 4:3 "stretched" resolution stops being stretched. That is a legitimate choice,
    // so only say so - loudly enough to explain a "my resolution feels wrong" report.
    if (windowed) {
        logger::logf("display_probe: game %dx%d is smaller than the desktop %dx%d and "
                     "windowed/borderless is on -> the GPU will NOT stretch it (black "
                     "bars, thinner models). Set fullscreen=on + window_borderless=off "
                     "in cod1reloaded.ini to play stretched.", gw, gh, dw, dh);
    }
}

}  // namespace patches
