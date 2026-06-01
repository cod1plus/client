// Patch fenetre borderless pour CoD1.
//
// Approche minimaliste (Phase 1) :
//   1. Watcher thread qui poll pour la fenetre principale de CoDMP.exe.
//   2. Quand elle est trouvee, on modifie ses styles GWL_STYLE / GWL_EXSTYLE
//      pour enlever bordures/titre/coins.
//   3. On la positionne pour remplir tout le monitor cible.
//
// Avantages vs vrai fullscreen exclusif :
//   - Alt-tab instantane (pas de switch de mode video).
//   - Multi-ecran propre : on choisit precisement quel monitor.
//   - Pas de glitch de gamma quand le jeu perd le focus.
//
// Limites :
//   - Ne gere pas un retour en mode fenetre normal sans relance (one-shot).
//   - Ne touche pas a la WindowProc ; certains events Windows comme le
//     redimensionnement utilisateur ne sont pas geres (pas grave en borderless).
//   - Pas de gestion du gamma (a porter dans une phase ulterieure).

#include "window_patch.h"
#include "logger.h"
#include "demo_upload.h"

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

bool      g_applied = false;
WNDPROC   g_original_wnd_proc = nullptr;
HWND      g_subclassed_hwnd = nullptr;

// EnumWindows callback : trouve une top-level visible appartenant au process.
struct FindData {
    DWORD  pid;
    HWND   hwnd;
};

BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam) {
    auto* d = (FindData*)lParam;

    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != d->pid) return TRUE; // continue

    if (!IsWindowVisible(hwnd))           return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE; // pas une top-level

    // Filtre supplementaire : la fenetre doit avoir une taille > 100x100
    // (evite de prendre une console ou un splash screen minuscule).
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    if ((rc.right - rc.left) < 200 || (rc.bottom - rc.top) < 200) return TRUE;

    d->hwnd = hwnd;
    return FALSE; // stop l'enumeration
}

HWND find_main_window() {
    FindData d = { GetCurrentProcessId(), NULL };
    EnumWindows(enum_windows_cb, (LPARAM)&d);
    return d.hwnd;
}

// Selection du monitor cible selon la config.
struct MonitorPickData {
    int   target_index;
    int   current_index;
    HMONITOR hmon;
};

BOOL CALLBACK enum_monitor_cb(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* d = (MonitorPickData*)lParam;
    if (d->current_index == d->target_index) {
        d->hmon = hMon;
        return FALSE; // stop
    }
    d->current_index++;
    return TRUE; // continue
}

HMONITOR pick_monitor(HWND hwnd) {
    // Mode "follow current" : on garde le monitor sur lequel la fenetre est
    // actuellement positionnee.
    if (g_window_config.follow_current_monitor) {
        return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }

    // Mode "preferred index" : on choisit le N-ieme monitor selon l'ordre
    // d'enumeration de Windows.
    if (g_window_config.preferred_monitor_index >= 0) {
        MonitorPickData d = { g_window_config.preferred_monitor_index, 0, NULL };
        EnumDisplayMonitors(NULL, NULL, enum_monitor_cb, (LPARAM)&d);
        if (d.hmon) return d.hmon;
    }

    // Fallback : monitor primaire (point 0,0).
    POINT zero = { 0, 0 };
    return MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
}

// WindowProc subclass : intercepte WM_ACTIVATEAPP pour minimiser le jeu
// quand il perd le focus. Tous les autres messages sont relayes a la
// WindowProc d'origine via CallWindowProc.
LRESULT CALLBACK cod1reloaded_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ACTIVATEAPP) {
        const BOOL activated = (BOOL)wParam;
        if (!activated && g_window_config.minimize_on_focus_loss) {
            // Important : relacher la souris avant de minimiser, sinon
            // le clic peut etre intercepte par notre fenetre qui n'est
            // plus visible.
            ReleaseCapture();
            while (ShowCursor(TRUE) < 0) {}
            ShowWindow(hwnd, SW_MINIMIZE);
        }
    }

    // Quand le user click le X ou Alt+F4 -> trigger upload immediat des demos
    // avant que le process meure. L'upload tourne en background, on ne bloque
    // pas la fermeture (sinon UX horrible). Les fichiers .dm_1 restent sur
    // disque si l'upload est interrompu, le retry se fera au prochain launch.
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        demo_upload_trigger_now();
    }

    if (g_original_wnd_proc) {
        return CallWindowProcA(g_original_wnd_proc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void subclass_window(HWND hwnd) {
    if (g_subclassed_hwnd == hwnd) return; // deja fait
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
    HMONITOR hMon = pick_monitor(hwnd);
    if (!hMon) return false;

    MONITORINFOEXA mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hMon, (LPMONITORINFO)&mi)) return false;

    // Enleve les styles de bordure / titre.
    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_DLGFRAME | WS_BORDER);
    style |= WS_POPUP;
    SetWindowLongA(hwnd, GWL_STYLE, style);

    LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
            WS_EX_WINDOWEDGE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex);

    const int x = mi.rcMonitor.left;
    const int y = mi.rcMonitor.top;
    const int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (!SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                      SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
        return false;
    }

    logger::logf("window_patch: borderless applied on %s (%dx%d at %d,%d)",
                 mi.szDevice, w, h, x, y);

    // Subclasse pour intercepter WM_ACTIVATEAPP et minimiser sur focus loss
    if (g_window_config.minimize_on_focus_loss) {
        subclass_window(hwnd);
    }
    return true;
}

DWORD WINAPI window_watcher_thread(LPVOID) {
    // Perpetual watcher. The engine destroys + recreates the main window
    // on vid_restart, mode switch, /r_mode change, etc. Without this loop,
    // the new window keeps its default styles (non-borderless) AND never
    // gets our WM_ACTIVATEAPP subclass, which breaks alt-tab : when the
    // user tries to leave the game, the window stays on top covering the
    // desktop because nothing minimizes it.
    //
    // We detect the destruction via IsWindow(g_subclassed_hwnd) returning
    // FALSE, then re-apply the full borderless + subclass pipeline to the
    // new window.
    for (;;) {
        bool need_reapply = false;

        if (!g_applied) {
            // Initial bring-up : no patched window yet.
            need_reapply = true;
        } else if (g_subclassed_hwnd && !IsWindow(g_subclassed_hwnd)) {
            // Our patched window vanished - likely vid_restart.
            logger::logf("window_patch: subclassed window 0x%p destroyed "
                         "(vid_restart?), will re-apply",
                         (void*)g_subclassed_hwnd);
            need_reapply = true;
        } else {
            // Defensive: if a NEW visible main window appeared that's
            // different from the one we subclassed, the old one became
            // stale even if its handle is still valid.
            HWND current = find_main_window();
            if (current && current != g_subclassed_hwnd) {
                logger::logf("window_patch: new main window 0x%p detected "
                             "(was 0x%p), will re-apply",
                             (void*)current, (void*)g_subclassed_hwnd);
                need_reapply = true;
            }
        }

        if (need_reapply) {
            // Reset globals so subclass_window picks up the fresh wndproc.
            g_applied = false;
            g_subclassed_hwnd = nullptr;
            g_original_wnd_proc = nullptr;

            HWND hwnd = find_main_window();
            if (hwnd) {
                Sleep(200);  // let new window stabilize before restyling
                if (make_borderless(hwnd)) {
                    g_applied = true;
                }
            }
        }

        // Healthy : slow poll (1s). Searching : fast poll (100ms).
        Sleep(g_applied ? 1000 : 100);
    }
    return 0;
}

}  // namespace

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

}  // namespace patches
