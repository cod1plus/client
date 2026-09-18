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
#include "features/settings_menu.h"   // refresh_rate "max", max_hz_native_res, menu_ini_writeback
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

long find_fullscreen_value(const char* buf, int* value, int* value_len);

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

    // One line a support thread can be settled with: what the player's config asks for.
    {
        char* cbuf = nullptr;
        int rf = -1, rfl = 0;
        if (read_config_mp(&cbuf)) {
            find_fullscreen_value(cbuf, &rf, &rfl);
            free(cbuf);
        }
        int ddw = 0, ddh = 0;
        probe_desktop_resolution(&ddw, &ddh);
        logger::logf("display_probe: config asks %dx%d, r_fullscreen %s (%s), desktop %dx%d, "
                     "ini fullscreen=%s borderless=%s", gw, gh,
                     rf < 0 ? "absent" : (rf ? "1" : "0"),
                     rf < 0 ? "engine default 1 = exclusive" : (rf ? "exclusive fullscreen" : "WINDOWED"),
                     ddw, ddh,
                     g_fullscreen_config.force_windowed_default ? "off" : "on",
                     g_window_config.borderless_enable ? "on" : "off");
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

// Position of the value of the LAST `seta r_fullscreen "N"` / `set r_fullscreen N` in
// the buffer (the one the engine ends up with), or -1. The name must stand alone.
long find_fullscreen_value(const char* buf, int* value, int* value_len) {
    const char* name = "r_fullscreen";
    const size_t nlen = strlen(name);
    long pos = -1;
    for (const char* p = strstr(buf, name); p; p = strstr(p + nlen, name)) {
        if (p != buf && !is_sep(*(p - 1))) continue;
        const char* v = p + nlen;
        if (!is_sep(*v)) continue;
        while (*v == ' ' || *v == '\t' || *v == '"') ++v;
        if (*v < '0' || *v > '9') continue;
        const char* e = v;
        while (*e >= '0' && *e <= '9') ++e;
        pos = (long)(v - buf);
        *value = atoi(v);
        *value_len = (int)(e - v);
    }
    return pos;
}

void enforce_ini_fullscreen() {
    if (!g_fullscreen_config.ini_key_present) return;
    const int want = g_fullscreen_config.force_windowed_default ? 0 : 1;

    char* buf = nullptr;
    if (!read_config_mp(&buf)) return;              // fresh install: default covers it
    int have = -1, vlen = 0;
    const long pos = find_fullscreen_value(buf, &have, &vlen);
    if (pos < 0 || have == want) { free(buf); return; }

    // rewrite just the digit(s), everything else byte-identical
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) { free(buf); return; }
    char* slash = strrchr(exe_path, '\\');
    if (!slash) { free(buf); return; }
    *(slash + 1) = '\0';
    char cfg_path[MAX_PATH];
    if (snprintf(cfg_path, sizeof(cfg_path), "%smain\\config_mp.cfg", exe_path)
        >= (int)sizeof(cfg_path)) { free(buf); return; }

    const size_t total = strlen(buf);
    FILE* f = fopen(cfg_path, "wb");
    if (!f) {
        logger::logf("display_probe: config_mp.cfg has r_fullscreen %d but the .ini says "
                     "fullscreen = %s - cannot rewrite it (err=%lu)", have,
                     want ? "on" : "off", GetLastError());
        free(buf);
        return;
    }
    fwrite(buf, 1, (size_t)pos, f);
    fprintf(f, "%d", want);
    fwrite(buf + pos + vlen, 1, total - (size_t)pos - (size_t)vlen, f);
    fclose(f);
    free(buf);
    logger::logf("display_probe: config_mp.cfg had r_fullscreen %d but cod1reloaded.ini "
                 "says fullscreen = %s -> rewritten to %d (an older build left it there; "
                 "set the .ini key to what you want)", have, want ? "on" : "off", want);
}

// ---------------------------------------------------------------------------------
// MAX HZ REGARDLESS OF THE RESOLUTION (enzo, 2026-09-17: "the Hz are stuck at 180").
//
// A custom resolution only exists with the refresh rates the driver created it with:
// enzo's 1440x1080 is listed up to 180 Hz while the panel does 320 Hz at 1920x1080.
// The engine asks 1440x1080 @ 200 -> BADMODE -> mode_guard falls back to the best the
// driver lists there, 180. No API creates a driver mode, so the only way to the panel's
// maximum is to RUN AT THE RESOLUTION THAT HAS IT and emulate the old look:
//   * config_mp.cfg is rewritten to that resolution @ max Hz (before Com_Init reads it),
//   * a 4:3 config on a 16:9 panel was a stretched setup (the monitor stretched it):
//     view_mode = stretched makes the mod do that stretch instead - same 4:3 vertical
//     FOV, same 640x480 HUD scaling, only sharper.
// Only with refresh_rate = max and exclusive fullscreen; opt out: max_hz_native_res = off.
// ---------------------------------------------------------------------------------
namespace {

// value of the LAST `seta <name> "<v>"` in the buffer -> position/len of <v>, or -1
long find_cvar_value(const char* buf, const char* name, int* value_len) {
    const size_t nlen = strlen(name);
    long pos = -1;
    for (const char* p = strstr(buf, name); p; p = strstr(p + nlen, name)) {
        if (p != buf && !is_sep(*(p - 1))) continue;
        const char* v = p + nlen;
        if (!is_sep(*v)) continue;
        while (*v == ' ' || *v == '\t') ++v;
        bool quoted = false;
        if (*v == '"') { quoted = true; ++v; }
        const char* e = v;
        if (quoted) { while (*e && *e != '"' && *e != '\n') ++e; }
        else        { while (*e && !is_sep(*e)) ++e; }
        pos = (long)(v - buf);
        *value_len = (int)(e - v);
    }
    return pos;
}

// rewrite the last occurrence in place, or append a seta line when absent
void cfg_put(char** buf, const char* name, const char* value) {
    int vlen = 0;
    const long pos = find_cvar_value(*buf, name, &vlen);
    const size_t total = strlen(*buf);
    const size_t nv = strlen(value);
    if (pos >= 0) {
        char* out = (char*)malloc(total - (size_t)vlen + nv + 1);
        memcpy(out, *buf, (size_t)pos);
        memcpy(out + pos, value, nv);
        memcpy(out + pos + nv, *buf + pos + vlen, total - (size_t)pos - (size_t)vlen + 1);
        free(*buf);
        *buf = out;
    } else {
        char line[128];
        const int n = snprintf(line, sizeof(line), "%sseta %s \"%s\"\r\n",
                               (total && (*buf)[total - 1] != '\n') ? "\r\n" : "", name, value);
        char* out = (char*)malloc(total + (size_t)n + 1);
        memcpy(out, *buf, total);
        memcpy(out + total, line, (size_t)n + 1);
        free(*buf);
        *buf = out;
    }
}

bool write_config_mp(const char* buf) {
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe_path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    char cfg_path[MAX_PATH];
    if (snprintf(cfg_path, sizeof(cfg_path), "%smain\\config_mp.cfg", exe_path) >= (int)sizeof(cfg_path)) return false;
    FILE* f = fopen(cfg_path, "wb");
    if (!f) return false;
    const size_t n = strlen(buf);
    const bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

}  // namespace

void enforce_max_hz_native() {
    if (!g_settings_menu_config.max_hz_native_res) return;
    // "max" -> the panel's maximum; a number (the menu writes "320") -> that rate;
    // "auto" -> the player wants the engine default, nothing to chase
    const bool want_max = _stricmp(g_settings_menu_config.refresh_rate, "max") == 0;
    const int  want_hz  = want_max ? 0 : atoi(g_settings_menu_config.refresh_rate);
    if (!want_max && want_hz <= 0) return;

    char* buf = nullptr;
    if (!read_config_mp(&buf)) return;
    int fs = 1;
    if (cfg_int(buf, "r_fullscreen", &fs) && fs == 0) { free(buf); return; }  // a window never switches modes
    int gw = 0, gh = 0;
    if (!probe_config_resolution(&gw, &gh)) { free(buf); return; }

    // what the driver lists: the panel's maximum (and where), and the maximum at the game res
    int best_any = 0, best_any_w = 0, best_any_h = 0, best_at_res = 0;
    DEVMODEA dm; ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32) continue;
        const int hz = (int)dm.dmDisplayFrequency, w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight;
        if (hz > best_any || (hz == best_any && w * h > best_any_w * best_any_h)) { best_any = hz; best_any_w = w; best_any_h = h; }
        if (w == gw && h == gh && hz > best_at_res) best_at_res = hz;
    }
    // target: the maximum, or the asked rate (capped at what the panel does at all)
    const int target = want_max ? best_any : (want_hz < best_any ? want_hz : best_any);
    if (!best_any || !best_any_w) { free(buf); return; }
    if (best_at_res >= target) {
        // the resolution has the rate: just make sure the config asks for it (an older
        // build's engine cap left "200.000000" in the file -> BADMODE on every launch)
        int have = 0;
        if (cfg_int(buf, "r_displayRefresh", &have) && have != target) {
            char v[16]; snprintf(v, sizeof(v), "%d", target);
            cfg_put(&buf, "r_displayRefresh", v);
            if (write_config_mp(buf))
                logger::logf("display_probe: config_mp.cfg asked %d Hz, %dx%d is listed at %d Hz -> r_displayRefresh %d",
                             have, gw, gh, target, target);
        }
        free(buf);
        return;
    }
    // for an asked rate below the maximum, prefer the largest resolution that lists it
    if (!want_max && target < best_any) {
        int bw = 0, bh = 0;
        ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i) {
            if (dm.dmBitsPerPel < 32 || (int)dm.dmDisplayFrequency != target) continue;
            if ((int)(dm.dmPelsWidth * dm.dmPelsHeight) > bw * bh) { bw = (int)dm.dmPelsWidth; bh = (int)dm.dmPelsHeight; }
        }
        if (bw) { best_any_w = bw; best_any_h = bh; best_any = target; }
    }

    // the same look: a 4:3 config on a wide panel was stretched by the monitor -> mod stretch
    const float cfg_aspect = (float)gw / (float)gh, nat_aspect = (float)best_any_w / (float)best_any_h;
    const bool was_stretched = cfg_aspect < nat_aspect - 0.02f;

    char v[16];
    cfg_put(&buf, "r_mode", "-1");
    snprintf(v, sizeof(v), "%d", best_any_w); cfg_put(&buf, "r_customwidth", v);
    snprintf(v, sizeof(v), "%d", best_any_h); cfg_put(&buf, "r_customheight", v);
    snprintf(v, sizeof(v), "%d", best_any);   cfg_put(&buf, "r_displayRefresh", v);
    const bool written = write_config_mp(buf);
    free(buf);
    if (!written) {
        logger::logf("display_probe: %dx%d is listed up to %d Hz, the display does %d Hz at %dx%d - "
                     "could not rewrite config_mp.cfg (err=%lu)", gw, gh, best_at_res, best_any, best_any_w, best_any_h, GetLastError());
        return;
    }
    if (was_stretched) {
        g_widescreen_config.stretch_enable     = true;
        g_widescreen_config.horplus_fov_enable = false;
        menu_ini_writeback("view_mode", "stretched");
    }
    logger::logf("display_probe: %dx%d is listed up to %d Hz only, the display does %d Hz at %dx%d -> "
                 "config_mp.cfg set to %dx%d @ %d Hz%s (max_hz_native_res = off in cod1reloaded.ini to keep the custom res)",
                 gw, gh, best_at_res, best_any, best_any_w, best_any_h, best_any_w, best_any_h, best_any,
                 was_stretched ? ", view_mode = stretched (the 4:3 stretch is now done by the mod, same look)" : "");
}

}  // namespace patches
