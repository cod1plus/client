#include "video/window_patch.h"
#include "video/mode_guard.h"
#include "core/logger.h"
#include "features/demo_upload.h"

#include <cstring>
#include <cstdio>

namespace patches {

WindowConfig g_window_config = {
    /* borderless_enable        */ true,
    /* follow_current_monitor   */ true,
    /* preferred_monitor_index  */ -1,
    /* minimize_on_focus_loss   */ true,
};

namespace {

volatile bool g_applied  = false;
volatile bool g_applying = false;   // suppresses anti-minimize during make_borderless
WNDPROC       g_original_wnd_proc = nullptr;
HWND          g_subclassed_hwnd   = nullptr;

// Exclusive fullscreen (r_fullscreen 1 beats our windowed default): the engine owns
// window style/size/mode and fighting it IS the two-windows/dead-keyboard bug (meta
// 2026-08-25) - and, up to 1.6.8, the hybrid "display mode switched by the engine,
// window restyled borderless by us" that every exclusive-fullscreen player got. But
// the engine does NOT minimize itself on focus loss, so without our subclass the game
// stays glued over the desktop (enzo, Win key, same day). In this mode the watcher
// only maintains the minimize-on-focus-loss subclass: no restyle, no resize, no
// SetForegroundWindow. NOT sticky: r_fullscreen can change live (vid_restart), and
// the watcher must come back when the player goes fullscreen -> borderless.
volatile bool g_exclusive = false;

// Read an engine cvar integer (same RE'd VAs/offsets settings_menu ships on). This is
// how the watcher decides borderless-vs-exclusive BEFORE touching any window:
// r_fullscreen is registered (with its config value) just before the engine creates
// its window, so waiting for it closes the startup race where we restyled a window
// the engine was about to take exclusive.
typedef void* (__cdecl* WpCvarFindVar_t)(const char*);
constexpr uintptr_t WP_CVAR_FINDVAR_VA = 0x0043b790;  // Cvar_FindVar
constexpr uintptr_t WP_CVAR_COUNT_VA   = 0x01912aec;  // cvar count (system up)
constexpr int       WP_CVAR_OFF_INTEGER = 0x20;
// the window class the engine registers (Sys_CreateWindow); nothing else of ours
constexpr const char* GAME_WINDOW_CLASS = "Call of Duty Multiplayer";

bool engine_cvar_int(const char* name, int* out) {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return false;
    if (*(volatile int*)WP_CVAR_COUNT_VA <= 0) return false;
    void* cv = ((WpCvarFindVar_t)WP_CVAR_FINDVAR_VA)(name);
    if (!cv) return false;
    *out = *(int*)((char*)cv + WP_CVAR_OFF_INTEGER);
    return true;
}

LRESULT CALLBACK cod1reloaded_wnd_proc(HWND, UINT, WPARAM, LPARAM);

struct FindData { DWORD pid; HWND hwnd; };

// The engine's window and nothing else: the class it registers, in our process,
// top-level, not minimized. The old search ("any visible window >= 200x200 of this
// process") picked a third-party overlay's window the moment the game was minimized
// (its rect is 160x28 at -32000 then), and subclassed/restyled THAT.
bool is_game_window(HWND hwnd) {
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != GetCurrentProcessId())  return false;
    char cls[64] = {0};
    if (!GetClassNameA(hwnd, cls, sizeof(cls)) || strcmp(cls, GAME_WINDOW_CLASS) != 0) return false;
    if (!IsWindowVisible(hwnd))            return false;
    if (IsIconic(hwnd))                    return false;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return false; // not top-level
    return true;
}

BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam) {
    auto* d = (FindData*)lParam;
    if (!is_game_window(hwnd)) return TRUE;
    d->hwnd = hwnd;
    return FALSE;
}

HWND find_main_window() {
    HWND fg = GetForegroundWindow();
    if (fg && is_game_window(fg)) return fg;
    FindData d = { GetCurrentProcessId(), NULL };
    EnumWindows(enum_windows_cb, (LPARAM)&d);
    return d.hwnd;
}

struct MonitorPickData {
    int      target_index;
    int      current_index;
    HMONITOR hmon;
};

BOOL CALLBACK enum_monitor_cb(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* d = (MonitorPickData*)lParam;
    if (d->current_index == d->target_index) { d->hmon = hMon; return FALSE; }
    d->current_index++;
    return TRUE;
}

HMONITOR pick_monitor(HWND hwnd) {
    if (g_window_config.follow_current_monitor) {
        return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    if (g_window_config.preferred_monitor_index >= 0) {
        MonitorPickData d = { g_window_config.preferred_monitor_index, 0, NULL };
        EnumDisplayMonitors(NULL, NULL, enum_monitor_cb, (LPARAM)&d);
        if (d.hmon) return d.hmon;
    }
    POINT zero = { 0, 0 };
    return MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
}


void restore_subclass() {
    if (g_subclassed_hwnd && g_original_wnd_proc && IsWindow(g_subclassed_hwnd)) {
        WNDPROC cur = (WNDPROC)GetWindowLongPtrA(g_subclassed_hwnd, GWLP_WNDPROC);
        if (cur == cod1reloaded_wnd_proc) {
            SetWindowLongPtrA(g_subclassed_hwnd, GWLP_WNDPROC,
                              (LONG_PTR)g_original_wnd_proc);
        }
    }
    g_subclassed_hwnd   = nullptr;
    g_original_wnd_proc = nullptr;
}

LRESULT CALLBACK cod1reloaded_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ACTIVATEAPP) {
        const BOOL activated = (BOOL)wParam;
        // Exclusive fullscreen, coming back from minimize: Windows restored the
        // desktop mode when we minimized but does not reliably re-apply the
        // game's mode on return - leaving a small backbuffer letterboxed on a
        // native desktop (meta). Re-set the engine's own last mode, and re-fit
        // the window if some earlier accident resized it.
        if (activated && g_exclusive && !g_applying) {
            if (mode_guard_reapply_fullscreen()) {
                int mw = 0, mh = 0;
                RECT rc;
                if (mode_guard_fullscreen_size(&mw, &mh) && GetWindowRect(hwnd, &rc) &&
                    (rc.left != 0 || rc.top != 0 ||
                     rc.right - rc.left != mw || rc.bottom - rc.top != mh)) {
                    SetWindowPos(hwnd, HWND_TOP, 0, 0, mw, mh,
                                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
                    logger::logf("window_patch: fullscreen window re-fitted to %dx%d",
                                 mw, mh);
                }
            }
        }
        if (!activated && g_window_config.minimize_on_focus_loss &&
            (g_applied || g_exclusive) && !g_applying) {
            ReleaseCapture();
            while (ShowCursor(TRUE) < 0) {}
            ShowWindow(hwnd, SW_MINIMIZE);
        }
    }

    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        demo_upload_trigger_now();
    }

    WNDPROC orig = g_original_wnd_proc;
    if (orig && orig != cod1reloaded_wnd_proc) {
        return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void subclass_window(HWND hwnd) {
    if (!hwnd) return;
    if (g_subclassed_hwnd == hwnd) return;

    WNDPROC cur = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (cur == cod1reloaded_wnd_proc) {
        g_subclassed_hwnd = hwnd;
        return;
    }

    g_original_wnd_proc = (WNDPROC)SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC, (LONG_PTR)cod1reloaded_wnd_proc);
    if (g_original_wnd_proc) {
        g_subclassed_hwnd = hwnd;
        logger::logf("window_patch: WindowProc subclassed (orig=0x%p)",
                     (void*)g_original_wnd_proc);
    } else {
        logger::logf("window_patch: SetWindowLongPtr a echoue");
    }
}

// the subclassed window is gone (vid_restart recreates it): forget it
void forget_dead_subclass() {
    if (g_subclassed_hwnd && !IsWindow(g_subclassed_hwnd)) {
        g_subclassed_hwnd   = nullptr;
        g_original_wnd_proc = nullptr;
    }
}

bool make_borderless(HWND hwnd) {
    g_applying = true;

    HMONITOR hMon = pick_monitor(hwnd);
    if (!hMon) { g_applying = false; return false; }

    MONITORINFOEXA mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hMon, (LPMONITORINFO)&mi)) { g_applying = false; return false; }

    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_DLGFRAME | WS_BORDER);
    style |= WS_POPUP;
    SetWindowLongA(hwnd, GWL_STYLE, style);

    LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
            WS_EX_WINDOWEDGE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex);

    int x = mi.rcMonitor.left;
    int y = mi.rcMonitor.top;
    int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // The renderer's backbuffer is whatever size the engine created the window at
    // (r_mode / r_customwidth+height). Filling the monitor with a smaller backbuffer
    // leaves the rendered image pinned to one corner and the rest of the window black:
    // e.g. r_customwidth 1440 on a 1920 monitor = a 480px black band on the right.
    // Centering on the backbuffer size was tried (2026-07-24) and REVERTED: it broke
    // fullscreen even at native resolution, GetClientRect does not reliably report the
    // final backbuffer size here. The black band is a RESOLUTION MISMATCH: fix it by
    // running the game at the monitor's resolution (view_mode=stretched then gives the
    // 4:3 look without changing the render size). Only diagnose it here, never resize.
    {
        RECT cr;
        if (GetClientRect(hwnd, &cr)) {
            const int rw = cr.right - cr.left, rh = cr.bottom - cr.top;
            if (rw >= 640 && rh >= 480 && (rw < w || rh < h))
                logger::logf("window_patch: NOTE backbuffer %dx%d < monitor %dx%d - the "
                             "unused area will be black; set the game's resolution to "
                             "%dx%d to fill the screen", rw, rh, w, h, w, h);
        }
    }

    if (!SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                      SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
        g_applying = false;
        return false;
    }

    logger::logf("window_patch: borderless applied on %s (%dx%d at %d,%d)",
                 mi.szDevice, w, h, x, y);

    if (g_window_config.minimize_on_focus_loss) {
        subclass_window(hwnd);
    }

    SetForegroundWindow(hwnd);

    g_applying = false;
    return true;
}

DWORD WINAPI window_watcher_thread(LPVOID) {
    for (;;) {
        // See g_exclusive above. Ground truth first (the engine holds a fullscreen
        // mode right now - survives the OS parking it on minimize), r_fullscreen
        // second (decides BEFORE the window exists, and after a vid_restart to
        // windowed). While neither is readable the video mode is undecided:
        // touch NOTHING (this closed the startup race that mangled meta's window).
        {
            bool excl = mode_guard_exclusive_active();
            int  rf   = 0;
            if (!excl) {
                if (!engine_cvar_int("r_fullscreen", &rf)) { Sleep(100); continue; }
                excl = (rf != 0);
            }
            if (excl != g_exclusive) {
                g_exclusive = excl;
                g_applied   = false;
                logger::logf(excl
                    ? "window_patch: exclusive fullscreen -> borderless off, keeping "
                      "only the minimize-on-focus-loss subclass"
                    : "window_patch: windowed mode -> borderless watcher resuming");
            }
        }

        forget_dead_subclass();

        if (g_exclusive) {
            // nothing to maintain but the subclass (minimize on focus loss, re-apply
            // of the mode on return): no restyle, no resize, no SetForegroundWindow
            if (g_window_config.minimize_on_focus_loss) {
                HWND target = find_main_window();
                if (target && target != g_subclassed_hwnd) {
                    restore_subclass();
                    subclass_window(target);
                }
            }
            Sleep(500);
            continue;
        }

        // Windowed with borders and borderless turned off: the player asked for
        // vanilla - no restyle, and no minimize (a bordered window alt-tabs fine).
        if (!g_window_config.borderless_enable) { Sleep(500); continue; }

        HWND target = find_main_window();
        bool need = false;

        if (!g_applied) {
            need = (target != NULL);
        } else if (!IsWindow(g_subclassed_hwnd) || !IsWindowVisible(g_subclassed_hwnd)) {
            logger::logf("window_patch: fenetre subclassee 0x%p disparue, re-apply",
                         (void*)g_subclassed_hwnd);
            g_subclassed_hwnd   = nullptr;
            g_original_wnd_proc = nullptr;
            g_applied = false;
            need = (target != NULL);
        } else if (target && target != g_subclassed_hwnd &&
                   target == GetForegroundWindow()) {
            // the engine recreated its window (vid_restart): restyle the new one
            logger::logf("window_patch: fenetre active changee 0x%p -> 0x%p",
                         (void*)g_subclassed_hwnd, (void*)target);
            restore_subclass();
            g_applied = false;
            need = true;
        }

        if (need && target) {
            Sleep(150);                 // let the new window settle
            target = find_main_window();
            if (target && make_borderless(target)) {
                g_applied = true;
            }
        }

        Sleep(g_applied ? 1000 : 100);
    }
    return 0;
}

}  // namespace

HWND get_game_window() {
    return g_subclassed_hwnd;
}

void start_window_watcher() {
    // The thread runs UNCONDITIONALLY. borderless_enable only governs the restyle
    // branch inside the loop: the minimize-on-focus-loss subclass must exist for
    // EXCLUSIVE fullscreen players too - with `window_borderless = off` the old
    // code killed this whole thread and with it the only thing that makes the
    // Windows key leave the game.
    if (!g_window_config.borderless_enable)
        logger::logf("window_patch: borderless disabled in INI - watcher in "
                     "minimize-on-focus-loss-only mode");
    HANDLE h = CreateThread(NULL, 0, window_watcher_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

}  // namespace patches
