// Patch fenetre borderless pour CoD1.
//
// Approche :
//   1. Watcher thread qui poll pour la fenetre principale de CoDMP.exe.
//   2. Quand elle est trouvee, on enleve bordures/titre (WS_POPUP) et on la
//      positionne pour remplir le monitor cible.
//   3. On subclasse sa WindowProc pour minimiser le jeu sur perte de focus
//      (alt-tab propre en borderless).
//
// CoDMP cree souvent une fenetre temporaire au demarrage puis recree la vraie
// fenetre de rendu (contexte GL) ; les deux peuvent coexister un instant. Le
// watcher doit donc :
//   - se verrouiller sur la fenetre AU PREMIER PLAN (la vraie fenetre active),
//     sinon il fait du yo-yo entre les deux et re-style en boucle ;
//   - ne JAMAIS re-subclasser une fenetre deja subclassee (sinon
//     g_original_wnd_proc pointe sur notre propre proc -> recursion infinie
//     sur chaque message -> fenetre figee, "on ne peut cliquer nulle part") ;
//   - restaurer la proc d'origine avant de basculer sur une autre fenetre.

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

volatile bool g_applied  = false;
volatile bool g_applying = false;   // vrai pendant make_borderless (anti-minimize)
WNDPROC       g_original_wnd_proc = nullptr;
HWND          g_subclassed_hwnd   = nullptr;

LRESULT CALLBACK cod1reloaded_wnd_proc(HWND, UINT, WPARAM, LPARAM);  // fwd

// --- Recherche de la fenetre principale -----------------------------------

struct FindData { DWORD pid; HWND hwnd; };

BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lParam) {
    auto* d = (FindData*)lParam;
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != d->pid)                 return TRUE;
    if (!IsWindowVisible(hwnd))            return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE; // pas une top-level
    RECT rc;
    if (!GetWindowRect(hwnd, &rc))         return TRUE;
    if ((rc.right - rc.left) < 200 || (rc.bottom - rc.top) < 200) return TRUE;
    d->hwnd = hwnd;
    return FALSE; // stop
}

// Privilegie la fenetre AU PREMIER PLAN si elle nous appartient : ca verrouille
// sur la vraie fenetre de rendu active et evite le yo-yo entre la fenetre
// temporaire de demarrage et la vraie (qui re-stylait en boucle).
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

// --- Selection du monitor --------------------------------------------------

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

// --- Subclass --------------------------------------------------------------

// Restaure la WindowProc d'origine sur la fenetre actuellement subclassee,
// puis oublie l'etat. A appeler avant de basculer sur une autre fenetre.
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
        // Minimise UNIQUEMENT si : perte de focus reelle, option active, patch
        // entierement applique, et PAS pendant notre propre setup (sinon on
        // minimise la fenetre en cours d'init -> inutilisable).
        if (!activated && g_window_config.minimize_on_focus_loss &&
            g_applied && !g_applying) {
            ReleaseCapture();
            while (ShowCursor(TRUE) < 0) {}
            ShowWindow(hwnd, SW_MINIMIZE);
        }
    }

    // Fermeture -> upload immediat des demos avant la mort du process.
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        demo_upload_trigger_now();
    }

    // Anti-recursion : ne JAMAIS rappeler notre propre proc.
    WNDPROC orig = g_original_wnd_proc;
    if (orig && orig != cod1reloaded_wnd_proc) {
        return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void subclass_window(HWND hwnd) {
    if (!hwnd) return;
    if (g_subclassed_hwnd == hwnd) return; // deja fait par nous

    // Si la proc actuelle est DEJA la notre (globals obsoletes apres une
    // recreation), on adopte sans reinstaller : reinstaller ferait pointer
    // g_original_wnd_proc sur notre propre proc -> recursion infinie.
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

// --- Borderless ------------------------------------------------------------

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

    const int x = mi.rcMonitor.left;
    const int y = mi.rcMonitor.top;
    const int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

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

    // S'assure que la fenetre a le focus pour capter souris/clavier
    // (best-effort : cross-thread, peut etre ignore par Windows).
    SetForegroundWindow(hwnd);

    g_applying = false;
    return true;
}

DWORD WINAPI window_watcher_thread(LPVOID) {
    // Watcher perpetuel. Le moteur detruit + recree la fenetre principale sur
    // vid_restart / changement de mode. On re-applique le pipeline complet a
    // la nouvelle fenetre, en restaurant proprement l'ancienne.
    for (;;) {
        HWND target = find_main_window();
        bool need = false;

        if (!g_applied) {
            need = (target != NULL);
        } else if (!IsWindow(g_subclassed_hwnd) || !IsWindowVisible(g_subclassed_hwnd)) {
            // Notre fenetre a disparu/ete cachee : elle n'existe plus, on
            // oublie sans restaurer (rien sur quoi restaurer).
            logger::logf("window_patch: fenetre subclassee 0x%p disparue, re-apply",
                         (void*)g_subclassed_hwnd);
            g_subclassed_hwnd   = nullptr;
            g_original_wnd_proc = nullptr;
            g_applied = false;
            need = (target != NULL);
        } else if (target && target != g_subclassed_hwnd &&
                   target == GetForegroundWindow()) {
            // Une AUTRE fenetre du jeu est devenue active (temp demarrage ->
            // vraie fenetre GL). On restaure l'ancienne puis on bascule.
            logger::logf("window_patch: fenetre active changee 0x%p -> 0x%p",
                         (void*)g_subclassed_hwnd, (void*)target);
            restore_subclass();
            g_applied = false;
            need = true;
        }

        if (need && target) {
            Sleep(150);                 // laisse la nouvelle fenetre se stabiliser
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
    if (!g_window_config.borderless_enable) {
        logger::logf("window_patch: borderless disabled in INI");
        return;
    }
    HANDLE h = CreateThread(NULL, 0, window_watcher_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

}  // namespace patches
