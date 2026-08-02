// gamma_fix.cpp - per-monitor hardware gamma (cod2x port). See gamma_fix.h.
//
// idTech3 lineage: brightness = SetDeviceGammaRamp with a ramp that bakes
// r_gamma AND the r_overBrightBits shift (map lighting is pre-scaled and
// compensated by the ramp). Windows applies ramps PER DISPLAY DEVICE, but the
// engine targets its window DC — with two monitors the ramp can land on the
// wrong display, leak to the desktop, or get lost on focus changes. We keep the
// engine's exact ramp (overbright look preserved) and only fix WHERE and WHEN
// it is applied, mirroring cod2x's window.cpp gamma handler.

#include "video/gamma_fix.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "video/window_patch.h"       // get_game_window()
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"   // CODMP_CVAR_COUNT_VA
#include "core/logger.h"
#include "core/iat.h"

namespace patches {

GammaFixConfig g_gamma_fix_config = {};

namespace {

typedef BOOL (WINAPI* SetDeviceGammaRamp_t)(HDC, LPVOID);
SetDeviceGammaRamp_t real_SetDeviceGammaRamp = nullptr;

CRITICAL_SECTION g_cs;
bool             g_cs_init = false;

// the ramp the engine last requested (r_gamma + overbright baked in)
WORD g_engine_ramp[3][256];
bool g_have_engine_ramp = false;

// the display device we currently drive, and its pre-modification ramp
char g_cur_device[32] = "";
HDC  g_cur_dc         = NULL;
WORD g_saved_ramp[3][256];
bool g_saved   = false;
bool g_applied = false;

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
const Cvar_FindVar_t cv_find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
constexpr int CVAR_OFF_VALUE   = 0x1c;
constexpr int CVAR_OFF_INTEGER = 0x20;

// cod2x parity: a neutral ramp must RESTORE the desktop's own (possibly ICC-calibrated)
// ramp instead of stamping identity over it.
bool ramp_is_identity(const WORD r[3][256]) {
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; i += 5) {
            const int ideal = i * 257;
            const int d = (int)r[c][i] - ideal;
            if (d > 1028 || d < -1028) return false;   // > ~4/255 off
        }
    return true;
}

// cod2x gamma_createRamp: pure pow(i/255, 1/gamma) ramp (exact when overbright is 0).
void build_pow_ramp(float gamma, WORD out[3][256]) {
    for (int i = 0; i < 256; ++i) {
        double v = pow(i / 255.0, 1.0 / (double)gamma);
        int rv = (int)(v * 65535.0 + 0.5);
        if (rv > 65535) rv = 65535;
        out[0][i] = out[1][i] = out[2][i] = (WORD)rv;
    }
}

bool monitor_device_of_window(HWND w, char out[32]) {
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi)) return false;
    lstrcpynA(out, mi.szDevice, 32);
    return true;
}

// Restore the saved desktop ramp on the device we modified, release the DC.
void restore_current_locked() {
    if (g_cur_dc) {
        if (g_applied && g_saved)
            real_SetDeviceGammaRamp(g_cur_dc, g_saved_ramp);
        DeleteDC(g_cur_dc);
        g_cur_dc = NULL;
    }
    g_cur_device[0] = '\0';
    g_applied = false;
    g_saved   = false;
}

// Apply the engine ramp to the given display device (saving its original first).
bool apply_to_device_locked(const char* device) {
    if (!g_have_engine_ramp) return false;

    if (g_cur_dc && strcmp(g_cur_device, device) == 0) {
        if (real_SetDeviceGammaRamp(g_cur_dc, g_engine_ramp)) {
            g_applied = true;
            return true;
        }
        return false;
    }

    restore_current_locked();                       // leaving the old monitor

    g_cur_dc = CreateDCA(NULL, device, NULL, NULL);
    if (!g_cur_dc) return false;
    lstrcpynA(g_cur_device, device, sizeof(g_cur_device));

    if (GetDeviceGammaRamp(g_cur_dc, g_saved_ramp))
        g_saved = true;                             // desktop ramp to restore later

    if (!real_SetDeviceGammaRamp(g_cur_dc, g_engine_ramp)) {
        restore_current_locked();
        return false;
    }
    g_applied = true;
    return true;
}

BOOL WINAPI Hook_SetDeviceGammaRamp(HDC hdc, LPVOID ramp) {
    if (!g_gamma_fix_config.enable || !ramp)
        return real_SetDeviceGammaRamp(hdc, ramp);

    EnterCriticalSection(&g_cs);
    memcpy(g_engine_ramp, ramp, sizeof(g_engine_ramp));

    // neutral ramp (r_gamma 1.0, overbright 0): give the desktop its own calibrated
    // ramp back instead of applying identity (cod2x: "gamma 1.0 -> restore")
    if (ramp_is_identity(g_engine_ramp)) {
        g_have_engine_ramp = false;
        restore_current_locked();
        LeaveCriticalSection(&g_cs);
        static bool logged_id = false;
        if (!logged_id) {
            logged_id = true;
            logger::logf("gamma_fix: neutral engine ramp -> desktop calibration kept");
        }
        return TRUE;
    }
    g_have_engine_ramp = true;

    char dev[32];
    HWND w = get_game_window();
    bool ok = false;
    if (w && monitor_device_of_window(w, dev))
        ok = apply_to_device_locked(dev);
    LeaveCriticalSection(&g_cs);

    static bool logged = false;
    if (!logged) {
        logged = true;
        logger::logf("gamma_fix: engine ramp captured -> %s (redirect %s)",
                     ok ? g_cur_device : "window DC (fallback)", ok ? "ok" : "failed");
    }

    if (!ok)
        return real_SetDeviceGammaRamp(hdc, ramp);  // engine's own path as fallback
    return TRUE;   // report success so the engine keeps the hardware-gamma path
}

}  // namespace

void gamma_fix_start() {
    if (!g_gamma_fix_config.enable) {
        logger::logf("gamma_fix: disabled");
        return;
    }
    if (!g_cs_init) {
        InitializeCriticalSection(&g_cs);
        g_cs_init = true;
    }
    void* real = iat_hook("gdi32.dll", "SetDeviceGammaRamp", (void*)Hook_SetDeviceGammaRamp);
    if (real) {
        real_SetDeviceGammaRamp = (SetDeviceGammaRamp_t)real;
        logger::logf("gamma_fix: SetDeviceGammaRamp IAT hook installed (per-monitor gamma)");
    } else {
        // no import found (unexpected) — leave everything untouched
        real_SetDeviceGammaRamp =
            (SetDeviceGammaRamp_t)GetProcAddress(GetModuleHandleA("gdi32.dll"),
                                                 "SetDeviceGammaRamp");
        g_gamma_fix_config.enable = false;
        logger::logf("gamma_fix: IAT entry not found - fix disabled");
    }
}

// Watcher tick (~200 Hz, self-throttled to 4 Hz): keep the ramp on the monitor the
// window is on while focused; give the desktop its ramp back when unfocused.
void gamma_fix_tick() {
    if (!g_gamma_fix_config.enable || !real_SetDeviceGammaRamp)
        return;

    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 250) return;
    last = now;

    // cod2x live r_gamma tracking: rebuild the ramp ourselves when r_gamma changes so
    // the brightness setting applies WITHOUT vid_restart. Only when overbright is 0
    // (then a pure pow ramp is exactly what the engine would compute); with overbright
    // active we stay on the captured engine ramp (vid_restart re-captures it).
    if (*(volatile int*)CODMP_CVAR_COUNT_VA > 0) {
        void* gv = cv_find("r_gamma");
        if (gv) {
            const float g = *(float*)((char*)gv + CVAR_OFF_VALUE);
            static float last_g = 0.0f;
            if (last_g == 0.0f) last_g = g;          // first sight: engine ramp governs
            if (g != last_g) {
                last_g = g;
                void* ob = cv_find("r_overBrightBits");
                const int obi = ob ? *(int*)((char*)ob + CVAR_OFF_INTEGER) : 0;
                if (obi == 0) {
                    EnterCriticalSection(&g_cs);
                    if (g > 0.995f && g < 1.005f) {  // neutral -> desktop ramp back
                        g_have_engine_ramp = false;
                        restore_current_locked();
                    } else {
                        build_pow_ramp(g, g_engine_ramp);
                        g_have_engine_ramp = true;
                        g_applied = false;           // re-apply below
                    }
                    LeaveCriticalSection(&g_cs);
                    logger::logf("gamma_fix: r_gamma -> %.3f (live)", g);
                }
            }
        }
    }

    if (!g_have_engine_ramp) return;

    HWND w = get_game_window();
    if (!w) return;

    const bool focused = (GetForegroundWindow() == w);

    EnterCriticalSection(&g_cs);
    if (!focused) {
        if (g_applied)
            restore_current_locked();               // alt-tab: desktop gamma back
    } else {
        char dev[32];
        if (monitor_device_of_window(w, dev)) {
            // (re)apply when not applied (focus regained) or the window changed monitor
            if (!g_applied || strcmp(dev, g_cur_device) != 0) {
                const bool ok = apply_to_device_locked(dev);
                static char last_logged[32] = "";
                if (ok && strcmp(last_logged, g_cur_device) != 0) {
                    lstrcpynA(last_logged, g_cur_device, sizeof(last_logged));
                    logger::logf("gamma_fix: ramp applied to %s (tick)", g_cur_device);
                }
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

void gamma_fix_shutdown() {
    if (!g_cs_init) return;
    EnterCriticalSection(&g_cs);
    restore_current_locked();                       // never leave the desktop modified
    LeaveCriticalSection(&g_cs);
}

}  // namespace patches
