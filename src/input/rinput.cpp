// Raw mouse input for CoD1 - port of cod2x's m_rinput (src/mss32/rinput.cpp).
//
// cod2x could rewrite Mouse_ProcessMovement wholesale because it reimplements the
// window layer. Here the engine's two mouse functions stay untouched: both do
//     delta = GetCursorPos() - centre ; SetCursorPos(centre)
// and both are the only callers of GetCursorPos in CoDMP.exe, so answering
// "centre + raw delta" from a hooked GetCursorPos hands them the device counts and
// nothing else changes - the re-centre, the menu cursor and m_filter all keep working.
// This is also what the original RInput.dll does, for the same reason.

#include "input/rinput.h"

#include "core/iat.h"
#include "core/logger.h"
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA, CODMP_CVAR_COUNT_VA
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA

#include <cstdio>
#include <cstring>

namespace patches {

RInputConfig g_rinput_config = {};

namespace {

constexpr int CVAR_OFF_INTEGER = 0x20;   // cvar_t layout, same as settings_menu.cpp
// CVAR_ARCHIVE / CVAR_ROM come from protocol_patch.h

constexpr int EXEC_APPEND = 2;   // Cbuf_ExecuteText: runs next frame on the main thread

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);

const Cvar_Get_t         Cvar_Get         = (Cvar_Get_t)CODMP_CVAR_GET_VA;
const Cvar_FindVar_t     Cvar_FindVar     = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
const Cvar_Set_t         Cvar_Set         = (Cvar_Set_t)CODMP_CVAR_SET_VA;
const Cbuf_ExecuteText_t Cbuf_ExecuteText = (Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA;

const char* kClassName = "COD1X_RawInput";

typedef BOOL (WINAPI *GetCursorPos_t)(LPPOINT);
GetCursorPos_t g_real_GetCursorPos = nullptr;

// Indirected rather than dereferenced inline so the hook can be exercised off-target.
volatile int* g_center_x = (volatile int*)CODMP_MOUSE_CENTER_X_VA;
volatile int* g_center_y = (volatile int*)CODMP_MOUSE_CENTER_Y_VA;

// --- shared with the raw-input thread ---------------------------------------------
CRITICAL_SECTION g_lock;
bool             g_lock_ready = false;
long             g_dx = 0, g_dy = 0;      // unread device counts
long             g_msg_total = 0;         // WM_INPUT messages ever seen (rate readout)

volatile LONG    g_active  = 0;           // hook answers raw only while this is 1
volatile LONG    g_thread_up = 0;
volatile LONG    g_thread_failed = 0;

HWND   g_rin_hwnd = NULL;
HANDLE g_thread   = NULL;
DWORD  g_tid      = 0;

bool at_preferred_base() { return (uintptr_t)GetModuleHandleA(NULL) == 0x400000; }

bool engine_ready() {
    return at_preferred_base() && *(volatile int*)CODMP_CVAR_COUNT_VA > 0;
}

int cvar_int(const char* name, int fallback) {
    void* cv = Cvar_FindVar(name);
    return cv ? *(int*)((char*)cv + CVAR_OFF_INTEGER) : fallback;
}

// --- accumulator -------------------------------------------------------------------

void take_delta(long* x, long* y) {
    EnterCriticalSection(&g_lock);
    *x = g_dx; *y = g_dy;
    g_dx = 0;  g_dy = 0;
    LeaveCriticalSection(&g_lock);
}

void drop_delta() {
    EnterCriticalSection(&g_lock);
    g_dx = 0; g_dy = 0;
    LeaveCriticalSection(&g_lock);
}

void on_wm_input(LPARAM lParam) {
    // RAWINPUT for a mouse is 40 bytes; ask for exactly that and ignore anything else.
    UINT size = sizeof(RAWINPUT);
    RAWINPUT raw;
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &raw, &size,
                        sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (raw.header.dwType != RIM_TYPEMOUSE) return;
    // MOUSE_MOVE_ABSOLUTE comes from tablets/RDP/some KVMs and carries screen
    // coordinates, not counts - adding it as a delta would fling the view.
    if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;

    EnterCriticalSection(&g_lock);
    g_dx += raw.data.mouse.lLastX;
    g_dy += raw.data.mouse.lLastY;
    g_msg_total++;
    LeaveCriticalSection(&g_lock);
}

LRESULT CALLBACK rin_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        on_wm_input(lp);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Own thread + own hidden window: raw input keeps flowing while the main thread is busy
// rendering, and a vid_restart (which destroys the game window) cannot unregister us.
DWORD WINAPI rin_thread(LPVOID) {
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = rin_wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = kClassName;

    if (!RegisterClassExA(&wc)) {
        InterlockedExchange(&g_thread_failed, 1);
        return 1;
    }

    // Deliberately a plain zero-size top-level window, NOT HWND_MESSAGE: with
    // dwFlags 0 the system only delivers raw input to a foreground target, and
    // message-only windows are outside that path. This is the shape RInput.dll and
    // cod2x both use, and it is known to work.
    g_rin_hwnd = CreateWindowExA(0, kClassName, kClassName, 0, 0, 0, 0, 0,
                                 NULL, NULL, wc.hInstance, NULL);
    if (!g_rin_hwnd) {
        UnregisterClassA(kClassName, wc.hInstance);
        InterlockedExchange(&g_thread_failed, 1);
        return 1;
    }

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;   // generic desktop
    rid.usUsage     = 0x02;   // mouse
    rid.dwFlags     = 0;      // foreground only: no input while alt-tabbed
    rid.hwndTarget  = g_rin_hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        DestroyWindow(g_rin_hwnd);
        g_rin_hwnd = NULL;
        UnregisterClassA(kClassName, wc.hInstance);
        InterlockedExchange(&g_thread_failed, 1);
        return 1;
    }

    InterlockedExchange(&g_thread_up, 1);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    rid.dwFlags    = RIDEV_REMOVE;
    rid.hwndTarget = NULL;    // RIDEV_REMOVE requires a null target
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    DestroyWindow(g_rin_hwnd);
    g_rin_hwnd = NULL;
    UnregisterClassA(kClassName, wc.hInstance);
    return 0;
}

bool rinput_enable() {
    if (g_thread) return true;
    InterlockedExchange(&g_thread_up, 0);
    InterlockedExchange(&g_thread_failed, 0);
    drop_delta();

    g_thread = CreateThread(NULL, 0, rin_thread, NULL, 0, &g_tid);
    if (!g_thread) {
        logger::logf("rinput: CreateThread failed (err=%lu)", GetLastError());
        return false;
    }

    // The window and the device registration both live on that thread; wait for it.
    for (int i = 0; i < 2000; ++i) {
        if (InterlockedCompareExchange(&g_thread_up, 0, 0) == 1) {
            InterlockedExchange(&g_active, 1);
            logger::logf("rinput: raw mouse input ON (dedicated thread, hidden window)");
            return true;
        }
        if (InterlockedCompareExchange(&g_thread_failed, 0, 0) == 1) break;
        Sleep(1);
    }

    logger::logf("rinput: could not register the raw input device (err=%lu) - "
                 "staying on the system cursor", GetLastError());
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    return false;
}

void rinput_disable() {
    InterlockedExchange(&g_active, 0);
    if (!g_thread) return;
    PostThreadMessageA(g_tid, WM_QUIT, 0, 0);
    if (WaitForSingleObject(g_thread, 2000) != WAIT_OBJECT_0)
        logger::logf("rinput: input thread did not exit in time");
    CloseHandle(g_thread);
    g_thread = NULL;
    g_tid    = 0;
    drop_delta();
    logger::logf("rinput: raw mouse input OFF (back to the system cursor)");
}

// --- the hook ----------------------------------------------------------------------

BOOL WINAPI hk_GetCursorPos(LPPOINT p) {
    if (InterlockedCompareExchange(&g_active, 0, 0) != 1 || !p)
        return g_real_GetCursorPos ? g_real_GetCursorPos(p) : FALSE;

    long dx = 0, dy = 0;
    take_delta(&dx, &dy);
    // Add exactly what the caller is about to subtract, so the centre's own value is
    // irrelevant and the caller ends up with the raw counts.
    p->x = *g_center_x + dx;
    p->y = *g_center_y + dy;
    return TRUE;
}

// --- polling rate readout ----------------------------------------------------------
// Messages seen over the last second = the mouse's effective polling rate. Comp players
// use it to check a 1000 Hz mouse really reports at 1000 Hz.

constexpr int   HZ_SLOTS   = 10;      // 10 x 100 ms
long  g_hz_slot[HZ_SLOTS]  = {0};
int   g_hz_index           = 0;
long  g_hz_sum             = 0;
long  g_hz_last_total      = 0;
long  g_hz_shown           = -1;   // last value pushed to the cvars
long  g_hz_shown_max       = -1;

void publish_hz() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 100) return;
    last = now;

    EnterCriticalSection(&g_lock);
    const long total = g_msg_total;
    LeaveCriticalSection(&g_lock);

    const long fresh = total - g_hz_last_total;
    g_hz_last_total = total;

    g_hz_sum -= g_hz_slot[g_hz_index];
    g_hz_slot[g_hz_index] = fresh;
    g_hz_sum += fresh;
    g_hz_index = (g_hz_index + 1) % HZ_SLOTS;

    if (fresh == 0) {   // mouse at rest: drop the window instead of showing a stale rate
        g_hz_sum = 0;
        memset(g_hz_slot, 0, sizeof(g_hz_slot));
    }

    // Publish through the command buffer, not Cvar_Set: this runs on the watcher
    // thread and the engine's cvar strings are not thread-safe. Ten writes a second
    // straight into the cvar system would race the main thread over the same heap.
    // Only push what actually changed, so a still mouse costs nothing.
    char cmd[96];
    if (g_hz_sum != g_hz_shown) {
        g_hz_shown = g_hz_sum;
        snprintf(cmd, sizeof(cmd), "set m_rinput_hz %ld\n", g_hz_sum);
        Cbuf_ExecuteText(EXEC_APPEND, cmd);
    }
    if (g_hz_sum > g_hz_shown_max) {
        g_hz_shown_max = g_hz_sum;
        snprintf(cmd, sizeof(cmd), "set m_rinput_hz_max %ld\n", g_hz_sum);
        Cbuf_ExecuteText(EXEC_APPEND, cmd);
    }
}

void reset_hz() {
    g_hz_sum = 0;
    g_hz_last_total = 0;
    g_hz_index = 0;
    g_hz_shown = 0;
    g_hz_shown_max = 0;
    memset(g_hz_slot, 0, sizeof(g_hz_slot));
    Cbuf_ExecuteText(EXEC_APPEND, "set m_rinput_hz 0\nset m_rinput_hz_max 0\n");
}

}  // namespace

void rinput_start() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    if (!at_preferred_base()) {
        logger::logf("rinput: CoDMP.exe is not at 0x400000 - raw input disabled");
        return;
    }
    void* real = iat_hook("user32.dll", "GetCursorPos", (void*)hk_GetCursorPos);
    if (!real) {
        logger::logf("rinput: GetCursorPos IAT hook FAILED - raw input unavailable");
        return;
    }
    g_real_GetCursorPos = (GetCursorPos_t)real;
    logger::logf("rinput: GetCursorPos IAT hook installed (idle until m_rinput 1)");
}

void rinput_tick() {
    if (!g_real_GetCursorPos || !engine_ready()) return;

    static bool registered = false;
    static int  applied    = 0;
    if (!registered) {
        Cvar_Get("m_rinput", g_rinput_config.enable_default ? "1" : "0", CVAR_ARCHIVE);
        // Plain readouts, not ROM: we publish them with `set` through the command
        // buffer, and the engine rejects `set` on a ROM cvar.
        Cvar_Get("m_rinput_hz", "0", 0);
        Cvar_Get("m_rinput_hz_max", "0", 0);
        registered = true;
        logger::logf("rinput: m_rinput registered (default %d)",
                     g_rinput_config.enable_default ? 1 : 0);
    }

    const int want = cvar_int("m_rinput", 0) > 0 ? 1 : 0;
    if (want != applied) {
        if (want) {
            // Refuse to stack on top of an external RInput.dll: both would consume the
            // same movement and the mouse would feel half-speed.
            if (FindWindowA("Rinput", NULL) != NULL) {
                logger::logf("rinput: another raw-input tool is already running "
                             "-> m_rinput forced back to 0");
                Cvar_Set("m_rinput", "0");
                applied = 0;
                return;
            }
            applied = rinput_enable() ? 1 : 0;
            if (!applied) Cvar_Set("m_rinput", "0");
        } else {
            rinput_disable();
            applied = 0;
        }
        reset_hz();
    }

    if (applied) publish_hz();
}

void rinput_shutdown() {
    rinput_disable();
}

}  // namespace patches
