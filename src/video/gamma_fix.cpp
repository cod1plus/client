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
#include "ui/gl_overlay.h"            // overlay_game_window() fallback

#include <cstdio>                      // snprintf (ramp_file_path)
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

// The engine's own DC (the fallback path) targets the PRIMARY display whatever monitor
// the game is on. Whenever that path is taken its pre-ramp is saved here and put back
// with ours - otherwise a two-monitor setup keeps the game gamma on the desktop screen
// ("brightness is weird with 2 screens", 2026-09-17).
WORD g_fb_ramp[3][256];
bool g_fb_saved = false;
bool g_fb_dirty = false;


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

// THE window resolution. get_game_window() is only populated by the borderless
// watcher, which never even starts with window_borderless=off - enzo's setup and
// most players' (fullscreen stretched). With it NULL the whole pipeline silently
// degraded to the vanilla path: no desktop-ramp capture, no unfocus restore, a
// no-op restore at exit - i.e. BOTH reported symptoms ("brightness stays on the
// desktop after quit / after the Windows key", 2026-08-25). The overlay learns
// the window from WindowFromDC at every SwapBuffers, in every display mode.
HWND resolve_game_window() {
    HWND w = get_game_window();
    if (!w) w = overlay_game_window();
    return w;
}

// Crash insurance: the desktop ramp is persisted next to the exe while our ramp
// is live, and deleted again on a clean restore. If the process dies without
// restoring (TerminateProcess by the single-instance reaper, driver crash, kill
// from task manager), the NEXT launch finds the file and puts the desktop ramp
// back before the game touches anything. Presence of the file IS the dirty flag:
// no heuristics on ramp contents, calibrated desktops stay untouched.
void ramp_file_path(char* out, size_t n) {
    char base[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, base, MAX_PATH);
    char* sl = strrchr(base, 92);          // backslash
    if (sl) sl[1] = 0;
    snprintf(out, n, "%scod1reloaded.gammadesk", base);
}

void persist_desktop_ramp_locked() {
    char path[MAX_PATH];
    ramp_file_path(path, sizeof(path));
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, g_cur_device, sizeof(g_cur_device), &wr, NULL);
    WriteFile(f, g_saved_ramp, sizeof(g_saved_ramp), &wr, NULL);
    CloseHandle(f);
}

void delete_desktop_ramp_file() {
    char path[MAX_PATH];
    ramp_file_path(path, sizeof(path));
    DeleteFileA(path);
}

// Called ONCE, lazily, from the first gamma call made on the engine's own thread
// (the SetDeviceGammaRamp hook or the watcher tick), BEFORE the game's ramp is
// captured as "the desktop": if the insurance file survived, the previous session
// died with our ramp still on the screen (crash / TerminateProcess) - put the
// desktop ramp back now.
//
// NEVER call this from DllMain / gamma_fix_start(). SetDeviceGammaRamp goes
// gdi32full -> mscms -> Windows.Internal.Graphics.Display.DisplayColorManagement
// (WinRT/COM) on Windows 11, and that activation needs the loader lock DllMain is
// holding: the main thread waits forever (5 threads idle, 0 CPU, no window) and the
// game "does not start". Seen 2026-09-14 on enzo's dev install: a stale
// cod1reloaded.gammadesk from a session killed on 09-10 made EVERY launch hang at
// exactly this call (stack read from the stuck process).
void recover_stale_desktop_ramp() {
    char path[MAX_PATH];
    ramp_file_path(path, sizeof(path));
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;          // clean previous exit
    char dev[32] = {0};
    WORD ramp[3][256];
    DWORD rd = 0;
    BOOL ok = ReadFile(f, dev, sizeof(dev), &rd, NULL) && rd == sizeof(dev);
    ok = ok && ReadFile(f, ramp, sizeof(ramp), &rd, NULL) && rd == sizeof(ramp);
    CloseHandle(f);
    if (ok && dev[0] && real_SetDeviceGammaRamp) {
        dev[sizeof(dev) - 1] = 0;
        HDC dc = CreateDCA(NULL, dev, NULL, NULL);
        if (dc) {
            real_SetDeviceGammaRamp(dc, ramp);
            DeleteDC(dc);
            logger::logf("gamma_fix: previous session died dirty - desktop ramp "
                         "restored on %s", dev);
        }
    }
    DeleteFileA(path);                              // one shot either way
}

// One-shot gate for the deferred recovery above (engine thread, outside DllMain).
volatile LONG g_recovery_done = 0;

void recover_stale_desktop_ramp_once() {
    if (InterlockedCompareExchange(&g_recovery_done, 1, 0) == 0)
        recover_stale_desktop_ramp();
}

// Restore the saved desktop ramp on the device we modified, release the DC.
void restore_current_locked() {
    if (g_cur_dc) {
        if (g_applied && g_saved) {
            real_SetDeviceGammaRamp(g_cur_dc, g_saved_ramp);
            delete_desktop_ramp_file();             // clean exit: no stale insurance
        }
        DeleteDC(g_cur_dc);
        g_cur_dc = NULL;
    }
    g_cur_device[0] = '\0';
    g_applied = false;
    g_saved   = false;
    // and the primary display, if the engine's own DC ever carried the ramp
    if (g_fb_dirty && g_fb_saved) {
        HDC scr = GetDC(NULL);
        if (scr) { real_SetDeviceGammaRamp(scr, g_fb_ramp); ReleaseDC(NULL, scr); }
        g_fb_dirty = false;
    }
}

// Approximate ramp equality - some drivers round-trip a ramp with a few LSBs of
// drift, so an exact memcmp would false-positive "failed" on a perfectly good apply.
bool ramps_close(const WORD a[3][256], const WORD b[3][256]) {
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; i += 3) {
            int d = (int)a[c][i] - (int)b[c][i];
            if (d > 512 || d < -512) return false;   // > ~2/255
        }
    return true;
}

// Apply the engine ramp to the given display device (saving its original first).
// Reads back what actually landed and retries once on mismatch: "revient IG avec
// une brightness hyper basse" (2026-08-15) has no confirmed single cause, but a
// SetDeviceGammaRamp call that reports success while a driver silently drops or
// partially applies it - especially right after a device-context churn like
// alt-tab-back-in creating a fresh DC - is a known class of driver flake this
// closes the loop on regardless of why any individual instance happens.
bool apply_to_device_locked(const char* device) {
    if (!g_have_engine_ramp) return false;

    HDC dc;
    bool fresh = !(g_cur_dc && strcmp(g_cur_device, device) == 0);
    if (fresh) {
        restore_current_locked();                    // leaving the old monitor
        g_cur_dc = CreateDCA(NULL, device, NULL, NULL);
        if (!g_cur_dc) return false;
        lstrcpynA(g_cur_device, device, sizeof(g_cur_device));
        if (GetDeviceGammaRamp(g_cur_dc, g_saved_ramp)) {
            g_saved = true;                          // desktop ramp to restore later
            persist_desktop_ramp_locked();           // crash insurance (see above)
        }
    }
    dc = g_cur_dc;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!real_SetDeviceGammaRamp(dc, g_engine_ramp)) {
            if (fresh) restore_current_locked();
            return false;
        }
        WORD check[3][256];
        if (GetDeviceGammaRamp(dc, check) && ramps_close(check, g_engine_ramp)) {
            g_applied = true;
            return true;
        }
        logger::logf("gamma_fix: readback mismatch on %s (attempt %d) - retrying",
                     device, attempt + 1);
    }
    // still applied best-effort even if the readback never matched - some drivers
    // legitimately quantise more coarsely than our tolerance, and refusing to mark
    // it applied would leave the tick loop retrying every 250ms forever
    g_applied = true;
    return true;
}

BOOL WINAPI Hook_SetDeviceGammaRamp(HDC hdc, LPVOID ramp) {
    // first gamma call of the session, on the engine thread: restore a desktop ramp a
    // dead session left behind BEFORE this call's ramp gets captured as the desktop's
    recover_stale_desktop_ramp_once();
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
    HWND w = resolve_game_window();
    bool ok = false;
    if (w && monitor_device_of_window(w, dev))
        ok = apply_to_device_locked(dev);

    // Window not known yet (the engine's first ramp lands before the first SwapBuffers):
    // HOLD the ramp, the tick applies it to the right monitor a few frames later. Going
    // through the engine's DC here would light the PRIMARY display instead - the whole
    // two-monitor bug. Give up on holding only if the window never shows up (overlay off).
    static int held = 0;
    if (!ok && !w && held < 400) {
        ++held;
        g_applied = false;                          // tick: (re)apply once the window exists
        LeaveCriticalSection(&g_cs);
        static bool logged_hold = false;
        if (!logged_hold) {
            logged_hold = true;
            logger::logf("gamma_fix: engine ramp captured before the window is known - held, "
                         "the tick applies it to the game's monitor");
        }
        return TRUE;
    }
    if (!ok) {
        // last resort: the engine's DC = the primary display. Remember what it held.
        if (!g_fb_saved && GetDeviceGammaRamp(hdc, g_fb_ramp)) g_fb_saved = true;
        g_fb_dirty = true;
    }
    LeaveCriticalSection(&g_cs);

    static bool logged = false;
    if (!logged) {
        logged = true;
        logger::logf("gamma_fix: engine ramp captured -> %s (redirect %s)",
                     ok ? g_cur_device : "window DC (fallback, primary display)", ok ? "ok" : "failed");
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
    // the stale-ramp recovery is deferred to the engine thread (see the function)
}

// Watcher tick (~200 Hz, self-throttled to 4 Hz): keep the ramp on the monitor the
// window is on while focused; give the desktop its ramp back when unfocused.
void gamma_fix_tick() {
    if (!g_gamma_fix_config.enable || !real_SetDeviceGammaRamp)
        return;
    recover_stale_desktop_ramp_once();   // engine never called SetDeviceGammaRamp? still recover

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
                // Read via .value (float), NOT .integer: r_gamma two lines up already
                // (correctly) does this, and the asymmetry was the bug. idTech3's
                // .integer shadow field is a cached derivative that is not guaranteed
                // populated for every cvar registration path - if it silently reads 0
                // while the REAL overbright is 1 or 2 (CoD1's normal default), this
                // branch wrongly believes it is safe to use the plain pow-curve
                // approximation, which has no overbright compensation baked in and
                // therefore reads visibly darker than the true engine ramp. "ma
                // brightness de ma config main est divisee" (2026-08-15) traced to
                // exactly this: .value is what Cvar_Set always populates, unconditionally.
                void* ob = cv_find("r_overBrightBits");
                const int obi = ob ? (int)(*(float*)((char*)ob + CVAR_OFF_VALUE) + 0.5f) : 0;
                logger::logf("gamma_fix: r_gamma live-check g=%.3f overbright=%d "
                             "(obi==0 -> %s)", g, obi, obi == 0 ? "pow-ramp" : "skip");
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

    HWND w = resolve_game_window();
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
