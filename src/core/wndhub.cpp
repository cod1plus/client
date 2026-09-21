// See wndhub.h for the why.
#include "core/wndhub.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace patches {

namespace {

constexpr int MAX_LISTENERS = 8;
constexpr int MAX_RECORDS   = 8;      // vid_restart: the new window exists before the old one is gone
constexpr int MAX_DEPTH     = 64;     // nested SendMessage from inside a proc is normal; 64 deep is a loop
constexpr const char* PROP  = "cod1reloaded.wndhub";

struct Listener { WndHubListener fn; const char* name; };
Listener g_listeners[MAX_LISTENERS];
int      g_nlisteners = 0;

// One record per window we are (or were) attached to. Never freed while the window
// lives: the record is the only place the original proc is known.
struct Record {
    HWND    hwnd;
    WNDPROC orig;
    DWORD   last_ping;
    bool    above_logged;
    DWORD   reinstalled_at;   // 0, or the tick of a re-install after a failed ping
    WNDPROC orig_before;      // what orig was before that re-install (for the undo)
    bool    no_reinstall;     // a re-install produced a loop: never again on this window
};
Record g_rec[MAX_RECORDS];
HWND   g_current = NULL;              // the engine's window, last attached
UINT   g_ping_msg = 0;                // RegisterWindowMessage: unique system-wide
int    g_depth = 0;                   // main thread only (every proc call is)
bool   g_loop_logged = false;

Record* find(HWND w) {
    for (Record& r : g_rec) if (r.hwnd == w) return &r;
    return nullptr;
}

LRESULT ping_reply(HWND w) { return (LRESULT)(0x1C0D1600u ^ (UINT_PTR)w); }

LRESULT CALLBACK hub_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (g_ping_msg && m == g_ping_msg) return ping_reply(w);

    Record* r = find(w);
    WNDPROC orig = r ? r->orig : nullptr;

    if (++g_depth > MAX_DEPTH) {
        // Somebody built a loop through us (two subclassers each holding the other
        // as "original" - exactly what 1.6.6 did to itself). Cut it here instead of
        // overflowing the stack; the message is lost, the game is not.
        --g_depth;
        if (!g_loop_logged) {
            g_loop_logged = true;
            logger::logf("wndhub: WindowProc chain loops through us (msg 0x%x, %d deep) - "
                         "cut with DefWindowProc; another hook on this window re-subclasses "
                         "with a stale original", m, MAX_DEPTH);
        }
        // If WE just re-installed on top (failed ping), the loop is ours: a hook above
        // us swallowed the ping rather than having removed us. Undo - we are on top,
        // so writing back what we replaced is safe - and never re-install here again.
        if (r && r->reinstalled_at && GetTickCount() - r->reinstalled_at < 10000 &&
            (WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC) == hub_proc && orig && orig != hub_proc) {
            SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)orig);   // the hook goes back on top...
            r->orig = r->orig_before;                              // ...and below us is what was there before
            r->reinstalled_at = 0; r->no_reinstall = true;
            logger::logf("wndhub: the loop followed our re-install on %p - undone (proc %p "
                         "back on top); the hook above us swallows unknown messages, so the "
                         "in-chain ping is off for this window", (void*)w, (void*)orig);
        }
        return DefWindowProcA(w, m, wp, lp);
    }

    LRESULT res = 0;
    bool consumed = false;
    for (int i = 0; i < g_nlisteners && !consumed; ++i)
        consumed = g_listeners[i].fn(w, m, wp, lp, &res);

    if (!consumed) {
        if (orig && orig != hub_proc) res = CallWindowProcA(orig, w, m, wp, lp);
        else                          res = DefWindowProcA(w, m, wp, lp);
    }
    --g_depth;

    if (m == WM_NCDESTROY && r) {
        // the window is gone; the handle value may come back for another one
        RemovePropA(w, PROP);
        if (g_current == w) g_current = NULL;
        r->hwnd = NULL; r->orig = nullptr;
    }
    return res;
}

void describe(HWND w, char* out, size_t n) {
    char cls[64] = {0}, title[64] = {0};
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowTextA(w, title, sizeof(title));
    snprintf(out, n, "hwnd %p class \"%s\" title \"%s\" %s", (void*)w, cls, title,
             IsWindowUnicode(w) ? "(unicode proc on top)" : "(ansi)");
}

bool install(HWND w, Record* r, const char* why) {
    WNDPROC cur = (WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC);
    if (cur == hub_proc) {
        // on top but unknown to us: only possible after a record was lost, which the
        // code never does - refuse rather than store ourselves as our own original
        logger::logf("wndhub: %p already runs our proc with no record - not touching it",
                     (void*)w);
        return false;
    }
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)hub_proc);
    if (!prev) {
        logger::logf("wndhub: SetWindowLongPtr(%p) failed (err=%lu)", (void*)w, GetLastError());
        return false;
    }
    if (!r) {
        for (Record& x : g_rec) if (!x.hwnd) { r = &x; break; }
        if (!r) r = &g_rec[0];        // 8 live windows at once cannot happen; keep going
    }
    const bool keep_flags = (r->hwnd == w);      // re-install on the same window
    r->hwnd = w; r->orig = prev; r->last_ping = GetTickCount(); r->above_logged = false;
    if (!keep_flags) { r->reinstalled_at = 0; r->orig_before = nullptr; r->no_reinstall = false; }
    SetPropA(w, PROP, (HANDLE)1);
    g_current = w;
    char d[200]; describe(w, d, sizeof(d));
    logger::logf("wndhub: subclassed %s, previous proc %p (%s)", d, (void*)prev, why);
    return true;
}

}  // namespace

void wndhub_add_listener(WndHubListener fn, const char* name) {
    if (!fn || g_nlisteners >= MAX_LISTENERS) return;
    g_listeners[g_nlisteners++] = { fn, name };
}

HWND wndhub_window() {
    if (g_current && !IsWindow(g_current)) g_current = NULL;
    return g_current;
}

void wndhub_attach(HWND w) {
    if (!w) return;
    if (!g_ping_msg) g_ping_msg = RegisterWindowMessageA("cod1reloaded.wndhub.ping");

    Record* r = find(w);
    if (!r) {
        char cls[64] = {0};
        GetClassNameA(w, cls, sizeof(cls));
        if (strcmp(cls, WNDHUB_GAME_CLASS) != 0) {
            static HWND s_refused = NULL;
            if (s_refused != w) {
                s_refused = w;
                char d[200]; describe(w, d, sizeof(d));
                logger::logf("wndhub: not the game window, refused: %s", d);
            }
            return;
        }
        install(w, nullptr, "first frame on this window");
        return;
    }

    // Known window. Handle recycled for a NEW window? Our prop dies with the old one.
    if (!GetPropA(w, PROP)) {
        logger::logf("wndhub: hwnd %p is a new window with a recycled handle", (void*)w);
        r->hwnd = NULL; r->orig = nullptr; r->reinstalled_at = 0; r->orig_before = nullptr; r->no_reinstall = false;
        install(w, r, "recycled handle");
        return;
    }
    g_current = w;

    // Once a second: are we still in the chain? A hook above us is fine (Discord & co
    // pass everything down); a hook that RESTORED what it had saved has removed us.
    DWORD now = GetTickCount();
    if (now - r->last_ping < 1000) return;
    r->last_ping = now;
    WNDPROC cur = (WNDPROC)GetWindowLongPtrA(w, GWLP_WNDPROC);
    if (cur == hub_proc) return;
    if (SendMessageA(w, g_ping_msg, 0, 0) == ping_reply(w)) {
        if (!r->above_logged) {
            r->above_logged = true;
            logger::logf("wndhub: another hook sits above ours on %p (proc %p) and passes "
                         "messages down - fine", (void*)w, (void*)cur);
        }
        return;
    }
    if (r->no_reinstall) return;
    logger::logf("wndhub: our proc was removed from %p (top is now %p) - re-installing",
                 (void*)w, (void*)cur);
    r->above_logged = false;
    WNDPROC before = r->orig;
    if (install(w, r, "re-install after removal")) { r->reinstalled_at = now ? now : 1; r->orig_before = before; }
}

}  // namespace patches
