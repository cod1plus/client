// tray balloon via Shell_NotifyIcon (not ToastNotificationManager: no AppUserModelID/COM needed)

#include "core/toast.h"
#include "core/logger.h"

#include <shellapi.h>
#include <cstring>
#include <cstdio>

namespace patches {

namespace {

constexpr UINT WM_TOAST_TRAY = WM_USER + 1;

HWND  g_hidden_hwnd = NULL;
UINT  g_next_uid    = 100;
ATOM  g_class_atom  = 0;
CRITICAL_SECTION g_lock;
bool  g_lock_init   = false;

LRESULT CALLBACK toast_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TOAST_TRAY) {
        if (LOWORD(lp) == NIN_BALLOONHIDE || LOWORD(lp) == NIN_BALLOONTIMEOUT ||
            LOWORD(lp) == NIN_BALLOONUSERCLICK) {
            NOTIFYICONDATAA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd   = hwnd;
            nid.uID    = (UINT)wp;
            Shell_NotifyIconA(NIM_DELETE, &nid);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

bool ensure_hidden_window() {
    if (g_hidden_hwnd) return true;

    HINSTANCE inst = GetModuleHandleA(NULL);

    if (!g_class_atom) {
        WNDCLASSEXA wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = toast_wnd_proc;
        wc.hInstance     = inst;
        wc.lpszClassName = "cod1reloaded_toast";
        g_class_atom = RegisterClassExA(&wc);
        if (!g_class_atom) return false;
    }

    g_hidden_hwnd = CreateWindowExA(0, "cod1reloaded_toast", "", 0,
                                    0, 0, 0, 0,
                                    HWND_MESSAGE, NULL, inst, NULL);
    return g_hidden_hwnd != NULL;
}

}  // namespace

void toast_show(const char* title, const char* body, ToastType type) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }
    EnterCriticalSection(&g_lock);

    if (!ensure_hidden_window()) {
        LeaveCriticalSection(&g_lock);
        logger::logf("toast: couldn't create hidden window");
        return;
    }

    UINT uid = g_next_uid++;

    NOTIFYICONDATAA nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hidden_hwnd;
    nid.uID              = uid;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    nid.uCallbackMessage = WM_TOAST_TRAY;
    nid.hIcon            = LoadIconA(NULL, IDI_APPLICATION);
    strncpy(nid.szTip, "cod1reloaded", sizeof(nid.szTip) - 1);
    strncpy(nid.szInfoTitle, title ? title : "cod1reloaded", sizeof(nid.szInfoTitle) - 1);
    strncpy(nid.szInfo, body ? body : "", sizeof(nid.szInfo) - 1);

    switch (type) {
        case ToastType::Warning: nid.dwInfoFlags = NIIF_WARNING; break;
        case ToastType::Error:   nid.dwInfoFlags = NIIF_ERROR;   break;
        case ToastType::Info:
        default:                 nid.dwInfoFlags = NIIF_INFO;    break;
    }

    Shell_NotifyIconA(NIM_ADD, &nid);
    LeaveCriticalSection(&g_lock);

    logger::logf("toast: \"%s\" - %s", title, body);
}

void toast_shutdown() {
    if (g_hidden_hwnd) {
        DestroyWindow(g_hidden_hwnd);
        g_hidden_hwnd = NULL;
    }
    if (g_lock_init) {
        DeleteCriticalSection(&g_lock);
        g_lock_init = false;
    }
}

}  // namespace patches
