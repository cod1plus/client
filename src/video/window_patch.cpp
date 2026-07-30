#include "video/window_patch.h"
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

LRESULT CALLBACK cod1reloaded_wnd_proc(HWND, UINT, WPARAM, LPARAM);

struct FindData { DWORD pid; HWND hwnd; };

BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam) {
    auto* d = (FindData*)lParam;
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != d->pid)                 return TRUE;
    if (!IsWindowVisible(hwnd))            return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE; // not top-level
    RECT rc;
    if (!GetWindowRect(hwnd, &rc))         return TRUE;
    if ((rc.right - rc.left) < 200 || (rc.bottom - rc.top) < 200) return TRUE;
    d->hwnd = hwnd;
    return FALSE;
}


HWND find_main_window() {
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(fg) &&
            GetWindow(fg, GW_OWNER) == NULL) {
            RECT rc;
            if (GetWindowRect(fg, &rc) &&
                (rc.right - rc.left) >= 200 && (rc.bottom - rc.top) >= 200) {
                return fg;
            }
        }
    }
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
        if (!activated && g_window_config.minimize_on_focus_loss &&
            g_applied && !g_applying) {
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
    // (r_mode / r_customwidth+height). Blindly stretching the window to the monitor
    // leaves the rendered image pinned to one corner and the rest of the window black:
    // e.g. r_customwidth 1440 on a 1920 monitor = a 480px black band on the right.
    // That is exactly what happened after a vid_restart, because on the first apply the
    // engine had switched the DISPLAY MODE to 1440x1080 (so monitor == backbuffer), and
    // after the restart the desktop was back to 1920 while the backbuffer stayed 1440.
    // So: only fill the monitor when the backbuffer actually covers it; otherwise keep
    // the backbuffer size and CENTER it (symmetric bars instead of a one-sided band).
    // Self-correcting: the engine resizes the window on a real mode change, and we read
    // the client rect fresh on every apply.
    // REVERTED 2026-07-24: an attempt to center the window on the backbuffer size (to
    // avoid the one-sided black band when the game renders smaller than the monitor)
    // broke fullscreen even at native resolution - GetClientRect at this point does not
    // reliably report the final backbuffer size. Back to filling the monitor, which is
    // the known-good behaviour. The black band is a RESOLUTION MISMATCH: fix it by
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

}  

HWND get_game_window() {
    return g_subclassed_hwnd;
}

void start_window_watcher() {
    if (!g_window_config.borderless_enable) {
        logger::logf("window_patch: borderless disabled in INI");
        return;
    }
    HANDLE h = CreateThread(NULL, 0, window_watcher_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

}  
