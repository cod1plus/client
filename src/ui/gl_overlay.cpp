// See gl_overlay.h for why this exists. Mechanics:
//
//   SwapBuffers (gdi32, IAT of CoDMP.exe) - idTech3 ends every frame there, from
//   GLimp_EndFrame, with the GL context current. We draw after the engine's frame
//   is complete and before it is presented: the UI composites over the finished
//   game image at native resolution.
//
//   Text is rasterised by GDI into DIBs (white on black, ANTIALIASED_QUALITY),
//   converted to GL_ALPHA textures and cached per (string, size, weight). That
//   gives real Segoe UI with proper antialiasing for the cost of one texture per
//   distinct string - a settings menu has a few dozen.
//
//   Input: the game window is subclassed. While the overlay is open every mouse
//   and keyboard message is consumed, so the engine sees a frozen world. The
//   cursor is VIRTUAL - integrated from WM_MOUSEMOVE deltas and drawn by us -
//   because in exclusive fullscreen the engine recenters the real cursor every
//   frame; moves that land exactly on the recenter point are ignored (that is
//   the engine's own SetCursorPos echoing back, not the player's hand).
//
//   GL state: glPushAttrib(ALL)+glPushClientAttrib(ALL) around the pass. The
//   engine is fixed-function GL1.x, so immediate mode is both safe and simplest.

#include "ui/gl_overlay.h"
#include "ui/ui_draw.h"
#include "core/wndhub.h"
#include "features/news.h"
#include "ui/modern_menu.h"
#include "ui/home_menu.h"
#include "ui/demo_seek.h"
#include "features/pam_install.h"
#include "ui/eye_debug.h"
#include "features/hitbox_view.h"
#include "ui/streamer_hud.h"
#include "core/iat.h"
#include "core/logger.h"
#include "input/rinput.h"
#include "video/gamma_fix.h"

#include <GL/gl.h>
#include <gdiplus.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace patches {

namespace {

typedef BOOL (WINAPI* SwapBuffers_t)(HDC);
SwapBuffers_t g_orig_swap = nullptr;
// The window is wndhub's (core/wndhub.cpp): ONE subclass for the whole DLL. This
// module only registers a listener - the 1.6.6 crash was two modules subclassing
// the same window on their own and looping through each other.
volatile long g_swap_calls = 0;       // proof the SwapBuffers hook still runs (see overlay_tick)
DWORD  g_hotkey_wndproc_tick = 0;     // last Ctrl+M seen through the WindowProc
bool   g_wndproc_dead = false;        // the key poll saw a chord the WindowProc never did
bool   g_news_visible = false;        // the main-menu news card was drawn this frame (Ctrl+N)

// 0 hidden, 1 settings panel, 2 home screen (the modern main-menu replacement).
// Two screens share one overlay: SETTINGS reached FROM home returns to home on
// close instead of hiding outright, so Ctrl+M/ESC behave like "back", not "quit".
int     g_mode = 0;
bool    g_came_from_home = false;
// Latched the instant a home-screen action navigates away (Join/Start/Resume/
// Disconnect/Quit): stops the level-triggered cod1x_home_active poll from
// immediately repainting home over whatever the engine just opened. Two real
// bugs made this necessary, not just a timing nicety: (1) our own forwarded
// `set cod1x_home_active 0` takes a frame to land, and (2) Quit's popup does
// NOT close "main" underneath it (matches the original engine button), so the
// cvar stays legitimately true while a popup is on top - the poll alone can
// never tell "still home" apart from "home, but something is deliberately on
// top of it" without this latch. Clears itself once the cvar is OBSERVED false.
bool    g_home_suppress = false;
UiInput g_in;
bool    g_keycap = false;           // Keys tab is waiting for the next input
bool    g_prev_down = false;
bool    g_saw_raw = false;   // raw tap delivered: ignore the WM_MOUSEMOVE fallback
POINT   g_last_move = { -1, -1 };
int     g_vw = 0, g_vh = 0;         // viewport size this frame
float   g_dt = 0.016f;
DWORD   g_last_tick = 0;

}  // namespace

const UiInput& ui_input() { return g_in; }

void ui_key_capture(bool on) { g_keycap = on; if (!on) g_in.vk = 0; }
bool overlay_visible() { return g_mode != 0; }
HWND overlay_game_window() { return wndhub_window(); }

void overlay_toggle(bool on) {
    int was = g_mode;
    g_mode = on ? 1 : 0;
    g_came_from_home = false;
    g_last_move.x = -1;
    rinput_ui_capture(on);
    if (on && was == 0) {
        // start the virtual cursor mid-screen (a mode swap keeps the old position)
        g_in.mx = g_vw > 0 ? g_vw / 2.0f : 400.0f;
        g_in.my = g_vh > 0 ? g_vh / 2.0f : 300.0f;
    }
}

// Enter/return-to home, or step from home into settings and back. All navigation
// besides the raw open/close pair above goes through here so g_came_from_home can
// never drift out of sync with g_mode.
void set_mode(int m, bool from_home) {
    if (m != g_mode)
        logger::logf("overlay: mode %d -> %d (from_home=%d, suppress=%d)",
                     g_mode, m, from_home, g_home_suppress);
    if (g_mode == 0 && m != 0) rinput_ui_capture(true);
    if (g_mode != 0 && m == 0) rinput_ui_capture(false);
    g_mode = m;
    g_came_from_home = from_home;
}

// ------------------------------------------------------------------- wndproc
namespace {

// wndhub listener. `return true` = consumed (*res goes back to Windows, nothing
// below us - not the engine - sees the message); `return false` = pass it on.
bool overlay_listener(HWND w, UINT m, WPARAM wp, LPARAM lp, LRESULT* res) {
    *res = 0;
    // Restore the desktop's gamma HERE, not (only) from DLL_PROCESS_DETACH. Calling
    // GDI functions from DllMain's detach handler is a documented Microsoft anti-
    // pattern (loader lock), and worse: on ExitProcess() - which is how Sys_Quit
    // leaves - Windows frequently SKIPS DLL_PROCESS_DETACH entirely for already-
    // loaded DLLs, as a fast-exit optimisation. That is the "parfois" in "la
    // brightness du jeu... l'applique parfois au bureau" (2026-08-15): the restore
    // call simply never ran on those exits. WM_DESTROY fires on the main thread while
    // the window and full GDI state are still alive, well before ExitProcess - a
    // reliable hook regardless of how the process is about to end. Must run before the
    // g_mode==0 early-return below: quitting from gameplay (overlay hidden) must
    // still restore gamma, not just quitting from our own menus.
    if (m == WM_DESTROY) gamma_fix_shutdown();

    // Timestamp the fullscreen minimize/restore dance in the log. These are the
    // exact transitions where AMD machines fall apart (two windows + dead
    // keyboard, 2026-08-25) and a support log without them can't order events.
    // A handful of messages per alt-tab: no spam risk.
    if (m == WM_ACTIVATE)
        logger::logf("window: WM_ACTIVATE %s%s", LOWORD(wp) ? "active" : "inactive",
                     HIWORD(wp) ? " (minimized)" : "");
    else if (m == WM_DISPLAYCHANGE)
        logger::logf("window: WM_DISPLAYCHANGE %ux%u %ubpp", (unsigned)LOWORD(lp),
                     (unsigned)HIWORD(lp), (unsigned)wp);
    else if (m == WM_SIZE && (wp == SIZE_MINIMIZED || wp == SIZE_RESTORED))
        logger::logf("window: WM_SIZE %s %ux%u",
                     wp == SIZE_MINIMIZED ? "minimized" : "restored",
                     (unsigned)LOWORD(lp), (unsigned)HIWORD(lp));

    // bind-capture: the Keys tab asked for the next input. EVERYTHING routes into
    // UiInput::vk - including our own hotkeys and ESC - so any key can be bound
    // (the menu itself treats ESC as cancel). Must run before the hotkey handler
    // below or CTRL+M could never be observed.
    if (g_mode != 0 && g_keycap) {
        switch (m) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: g_in.vk = (int)wp;      return true;
        case WM_LBUTTONDOWN:                 g_in.vk = VK_LBUTTON;   return true;
        case WM_RBUTTONDOWN:                 g_in.vk = VK_RBUTTON;   return true;
        case WM_MBUTTONDOWN:                 g_in.vk = VK_MBUTTON;   return true;
        case WM_XBUTTONDOWN:
            g_in.vk = (HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            return true;
        case WM_MOUSEWHEEL:
            g_in.vk = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0xF001 : 0xF002;
            return true;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR:
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            return true;
        }
    }

    // Ctrl+N: the news card's link (main menu only - the card is not drawn in game)
    if (m == WM_KEYDOWN && wp == 'N' && (GetKeyState(VK_CONTROL) & 0x8000) &&
        ((g_mode == 0 && g_news_visible) || g_mode == 2)) {
        news_open_link();
        return true;
    }
    // Ctrl+M only (enzo, 2026-09-17): INSERT is a key players bind, and a bare key
    // fires while aiming or typing; a chord does not
    if (m == WM_KEYDOWN && wp == 'M' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        // One line per press: a player who "cannot open the menu" either has this
        // line (the overlay saw the key: look at the mode transitions) or does not
        // (nothing reached our proc: a hook above us ate it, or we are not in the
        // chain - see hotkey_poll, which then takes over).
        g_hotkey_wndproc_tick = GetTickCount();
        g_wndproc_dead = false;
        logger::logf("overlay: Ctrl+M via WindowProc (mode %d)", g_mode);
        if (g_mode == 0) {
            if (home_menu_is_active()) { g_home_suppress = false; set_mode(2, false); }
            else overlay_toggle(true);
        }
        else if (g_mode == 2) set_mode(1, true);                        // home -> settings
        else if (g_mode == 1 && g_came_from_home) set_mode(2, false);  // settings -> home
        else overlay_toggle(false);
        return true;
    }
    if (g_mode == 0)
        return false;

    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_mode == 1 && g_came_from_home) set_mode(2, false);   // settings -> home
            else if (g_mode == 2) home_menu_escape();                  // quit confirm on/off
            else overlay_toggle(false);
            return true;
        }
        return true;
    case WM_KEYUP: case WM_SYSKEYUP:
        // RELEASES go through: the Ctrl of Ctrl+M went down before we opened, and if
        // its release is swallowed the engine keeps Ctrl held - console "a" = home,
        // crouch stuck (enzo, 2026-09-18). A release of an already-up key is a no-op.
        return false;
    case WM_CHAR: case WM_SYSKEYDOWN:
        return true;
    case WM_MOUSEMOVE: {
        if (g_saw_raw) return true;            // raw input owns the cursor
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc; GetClientRect(w, &rc);
        POINT centre = { rc.right / 2, rc.bottom / 2 };
        // The engine pins the cursor: after EVERY physical move it SetCursorPos's back
        // to centre, which echoes as one more WM_MOUSEMOVE landing (about) dead centre.
        // The echo must not move our cursor, but it MUST update the reference point -
        // v1 reset the reference instead, so the "previous position" was forgotten
        // after every single move and no delta ever accumulated: frozen cursor.
        // Exact match on purpose: physical moves START at the pin point, so a 1-pixel
        // move lands at centre+1 and any tolerance would eat it (sticky slow aiming).
        // The engine computes its pin point as width/2 exactly like we do.
        bool is_recentre = (p.x == centre.x && p.y == centre.y);
        if (g_last_move.x >= 0 && !is_recentre) {
            g_in.mx += (float)(p.x - g_last_move.x);
            g_in.my += (float)(p.y - g_last_move.y);
            if (g_in.mx < 0) g_in.mx = 0;
            if (g_in.my < 0) g_in.my = 0;
            if (g_vw > 0 && g_in.mx > g_vw) g_in.mx = (float)g_vw;
            if (g_vh > 0 && g_in.my > g_vh) g_in.my = (float)g_vh;
        }
        g_last_move = p;
        return true;
    }
    case WM_LBUTTONDOWN: g_in.down = true;  return true;
    case WM_LBUTTONUP:   g_in.down = false; return true;
    case WM_MOUSEWHEEL:
        g_in.wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return true;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        return true;
    }
    return false;
}

// The HDC handed to SwapBuffers belongs to the game's window in every display
// mode (WindowFromDC is the standard overlay technique); wndhub refuses anything
// that is not the engine's window class, so a third party's HDC cannot fool it.
void attach_window(HDC dc) {
    HWND w = WindowFromDC(dc);
    if (w) wndhub_attach(w);
}

// Ctrl+M / ESC / clicks normally arrive through the WindowProc, where they are also
// SWALLOWED so the engine never sees them. If a third-party hook above ours eats
// the chord (or we are out of the chain and the ping has not healed it yet), this
// per-frame poll still opens and closes the menu - and says so in the log, which is
// the diagnostic the "menu impossible to open" reports have been missing. Only
// while our window is in the foreground, and only once the chord has been held
// for 100 ms WITHOUT the WindowProc reporting it: GetAsyncKeyState sees the key
// the instant it goes down, while its WM_KEYDOWN waits in the queue until the
// engine pumps messages at the start of the next frame - firing on the first
// frame would race the proc and toggle twice. Once the poll has caught a press
// the proc missed, the mouse button is polled too, so the menu stays usable; the
// engine then also receives those inputs - a degraded mode, logged, never the
// normal one.
void hotkey_poll(HWND w) {
    static DWORD s_chord_since = 0;      // 0 = chord not down
    static bool  s_fired = false;        // once per chord
    const bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('M') & 0x8000);
    const DWORD now = GetTickCount();
    if (!chord) { s_chord_since = 0; s_fired = false; return; }
    if (!s_chord_since) { s_chord_since = now ? now : 1; return; }
    if (s_fired || now - s_chord_since < 100) return;
    if (!w || GetForegroundWindow() != w) return;
    if (g_hotkey_wndproc_tick >= s_chord_since) { s_fired = true; return; }   // the proc saw it
    s_fired = true;
    if (!g_wndproc_dead) {
        g_wndproc_dead = true;
        logger::logf("overlay: Ctrl+M seen by the key poll but NOT by the WindowProc - "
                     "another hook on the window eats it; menu driven by polling from now on");
    }
    if (g_mode == 0) overlay_toggle(true); else overlay_toggle(false);
}

// ------------------------------------------------------------------- news card
// "MAJOR IN PROGRESS" and the like, fed by news.txt online (features/news.cpp), on
// the MAIN MENU only (drawn while no cgame is loaded). Broadcast lower-third, not a
// box: right-aligned type over a gradient that fades into the screen edge, a live
// dot, a hairline, the link hint in tracked small caps. Same mono palette as the
// menu. Ctrl+N opens the link - the card cannot be clicked, the engine owns the
// cursor on its own menu.

// Uppercase, letter-spaced: the "eyebrow" style. Per glyph through the text cache.
float tracked_width(int px, int weight, const char* s, float tracking) {
    float w = 0;
    for (const char* c = s; *c; ++c) {
        char g[2] = { *c, 0 };
        w += ui_text_width(px, weight, g) + tracking;
    }
    return w > tracking ? w - tracking : 0;
}
void text_tracked(float x, float y, int px, int weight, DWORD col, const char* s, float tracking) {
    for (const char* c = s; *c; ++c) {
        char g[2] = { *c, 0 };
        x += ui_text(x, y, px, weight, col, g) + tracking;
    }
}

void draw_news_card(HWND wnd) {
    static DWORD s_probe = 0, s_since = 0;
    static bool  s_in_game = true;
    const DWORD now = GetTickCount();
    if (now - s_probe > 500) { s_probe = now; s_in_game = GetModuleHandleA("cgame_mp_x86.dll") != NULL; }
    char title[64], text[128], url[256];
    if (s_in_game || !news_get(title, sizeof(title), text, sizeof(text), url, sizeof(url))) {
        g_news_visible = false;
        s_since = 0;
        return;
    }
    RECT rc;
    GetClientRect(wnd, &rc);
    g_vw = rc.right; g_vh = rc.bottom;
    if (g_vw <= 0 || g_vh <= 0) return;
    g_news_visible = true;
    if (!s_since) s_since = now ? now : 1;
    for (char* c = title; *c; ++c) *c = (char)toupper((unsigned char)*c);

    ui_begin_2d(g_vw, g_vh);
    // ease in over 700 ms: fade + a slide of a few pixels from the right
    float t = (now - s_since) / 700.0f; if (t > 1) t = 1;
    const float ease = t * t * (3 - 2 * t);
    const float sc = g_vh / 1080.0f > 0.6f ? g_vh / 1080.0f : 0.6f;
    const float margin = 44 * sc, pad = 22 * sc, slide = (1 - ease) * 18 * sc;
    const int   px_eye = (int)(12 * sc), px_head = (int)(24 * sc), px_hint = (int)(11 * sc);
    const float tr_eye = 2.6f * sc, tr_hint = 1.8f * sc;
    const char* hint = "CTRL + N   OPEN THE PAGE";
    const float dot_r = 3.5f * sc, dot_gap = 12 * sc;

    const float w_eye  = tracked_width(px_eye, 600, title, tr_eye) + dot_r * 2 + dot_gap;
    const float w_head = text[0] ? ui_text_width(px_head, 600, text) : 0;
    const float w_hint = url[0] ? tracked_width(px_hint, 600, hint, tr_hint) : 0;
    float bw = w_eye; if (w_head > bw) bw = w_head; if (w_hint > bw) bw = w_hint;
    const float x1 = g_vw - margin + slide;                 // right edge of the type
    const float y0 = margin;
    float h = 20 * sc;                                      // eyebrow row
    if (text[0]) h += 30 * sc;
    if (url[0])  h += 26 * sc;
    h += 8 * sc;

    ui_alpha(ease);
    // backdrop: a gradient that starts transparent to the left of the type and
    // becomes near-black at the screen edge, so the type reads on any menu art
    // - soft on every side: a horizontal ramp times a vertical one, so it is a
    // shadow behind the type, never a band with edges
    {
        const float gx0 = x1 - bw - 220 * sc, gx1 = (float)g_vw;
        const float gy0 = y0 - pad * 1.6f, gy1 = y0 + h + pad * 1.6f;
        const int cols = 40, rows = 14;
        for (int j = 0; j < rows; ++j) {
            const float r0 = (float)j / rows, r1 = (float)(j + 1) / rows;
            const float d = r0 < 0.5f ? r0 * 2 : (1 - r0) * 2;      // 0 at the edges, 1 mid
            const float fy = d * d * (3 - 2 * d);
            for (int i = 0; i < cols; ++i) {
                const float f0 = (float)i / cols, f1 = (float)(i + 1) / cols;
                const float a = f0 * f0 * 0.85f * fy;
                if (a < 0.01f) continue;
                ui_rect(gx0 + (gx1 - gx0) * f0, gy0 + (gy1 - gy0) * r0,
                        (gx1 - gx0) * (f1 - f0) + 1, (gy1 - gy0) * (r1 - r0) + 1,
                        ((DWORD)(a * 255) << 24) | 0x000000);
            }
        }
    }
    float y = y0;
    // eyebrow: live dot + tracked small caps, right-aligned
    {
        const float pulse = 0.55f + 0.45f * sinf(now * 0.0045f);
        const float ex = x1 - tracked_width(px_eye, 600, title, tr_eye);
        const float cx = ex - dot_gap - dot_r, cy = y + px_eye * 0.72f;
        ui_ellipse(cx, cy, dot_r * (1.6f + 0.9f * pulse), dot_r * (1.6f + 0.9f * pulse), 1.0f,
                   ((DWORD)(70 * pulse) << 24) | 0xFFFFFF);
        ui_rect_rounded(cx - dot_r, cy - dot_r, dot_r * 2, dot_r * 2, dot_r, UI_ACCENT);
        text_tracked(ex, y, px_eye, 600, UI_ACCENT, title, tr_eye);
        y += 20 * sc;
    }
    // headline
    if (text[0]) {
        ui_text(x1 - w_head, y, px_head, 600, UI_TEXT, text);
        y += 30 * sc;
    }
    // hairline + hint
    if (url[0]) {
        ui_rect(x1 - bw, y + 4 * sc, bw, 1, 0x40FFFFFF);
        text_tracked(x1 - w_hint, y + 11 * sc, px_hint, 600, UI_MUTED, hint, tr_hint);
    }
    ui_alpha(1.0f);
    ui_end_2d();
}

// ------------------------------------------------------------------- swap
BOOL WINAPI hk_swapbuffers(HDC dc) {
    InterlockedIncrement(&g_swap_calls);
    attach_window(dc);
    demo_seek_frame();                            // demo fast-forward to a kill (ui/demo_seek)
    pkg_install_frame();                          // package cvars + cod1x_install (features/pkg_frame)
    eye_debug_frame();                            // player_debugEyePosition (ui/eye_debug)
    hitbox_view_frame();                          // cod1x_drawhitbox (features/hitbox_view)
    HWND wnd = wndhub_window();                   // this frame's window
    hotkey_poll(wnd);
    if (g_wndproc_dead && g_mode != 0) g_in.down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (g_mode == 0 && modern_menu_poll_open()) overlay_toggle(true);
    // Level-triggered: enter home the moment the engine opens "main", leave it the
    // moment the engine closes "main" through a path WE did not initiate (e.g. some
    // other mod screen, or a future engine flow we never anticipated) - self-correcting
    // instead of having to enumerate every way the underlying menu can change.
    bool home_active = home_menu_is_active();
    static bool s_last_home_active = false;
    if (home_active != s_last_home_active) {
        logger::logf("overlay: cod1x_home_active %d -> %d (mode=%d suppress=%d)",
                     s_last_home_active, home_active, g_mode, g_home_suppress);
        s_last_home_active = home_active;
    }
    if (!home_active) g_home_suppress = false;      // cvar caught up: re-arm
    if (g_mode == 0 && home_active && !g_home_suppress) set_mode(2, false);
    if (g_mode == 2 && !home_active) set_mode(0, false);
    if (g_mode != 0 && wnd) {
        RECT rc;
        GetClientRect(wnd, &rc);
        g_vw = rc.right; g_vh = rc.bottom;
        if (g_vw > 0 && g_vh > 0) {
            ui_begin_2d(g_vw, g_vh);

            // PRIMARY cursor source: the raw-input tap. WM_MOUSEMOVE cannot do
            // this job - Windows coalesces it to the latest position and the
            // engine re-pins the cursor to centre ~250x/s, so the physical
            // positions are overwritten before we ever see them (frozen cursor,
            // 2026-08-10). Raw deltas come straight from the device.
            long rdx = 0, rdy = 0;
            rinput_ui_take_delta(&rdx, &rdy);
            if (rdx || rdy) {
                g_saw_raw = true;
                g_in.mx += (float)rdx;
                g_in.my += (float)rdy;
                if (g_in.mx < 0) g_in.mx = 0;
                if (g_in.my < 0) g_in.my = 0;
                if (g_in.mx > g_vw) g_in.mx = (float)g_vw;
                if (g_in.my > g_vh) g_in.my = (float)g_vh;
            }

            DWORD now = GetTickCount();
            g_dt = g_last_tick ? (now - g_last_tick) / 1000.0f : 0.016f;
            if (g_dt > 0.05f) g_dt = 0.05f;
            ui_frame_dt(g_dt);
            g_last_tick = now;

            g_in.clicked = g_in.down && !g_prev_down;
            if (g_mode == 2) {
                HomeAction act = home_menu_draw((float)g_vw, (float)g_vh);
                if (act == HomeAction::OpenSettings) set_mode(1, true);
                else if (act == HomeAction::Navigated) { g_home_suppress = true; set_mode(0, false); }
            } else {
                modern_menu_draw((float)g_vw, (float)g_vh);
            }
            ui_alpha(1.0f);                     // cursor always fully opaque
            // cursor: small triangle pointer, drawn last
            float mx = g_in.mx, my = g_in.my;
            glDisable(GL_TEXTURE_2D);
            ui_set_color(0xFF000000);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx, my); glVertex2f(mx + 15, my + 6); glVertex2f(mx + 6, my + 15);
            glEnd();
            ui_set_color(0xFFFFFFFF);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx + 1, my + 2); glVertex2f(mx + 12, my + 6.5f); glVertex2f(mx + 6.5f, my + 12);
            glEnd();
            g_prev_down = g_in.down;
            g_in.wheel = 0;
            g_in.vk = 0;                        // capture events are one-frame

            ui_end_2d();
        }
    } else if (g_mode == 0 && wnd && demo_seek_curtain_visible()) {
        // a demo is being fast-forwarded to a kill: cover it
        RECT rc;
        GetClientRect(wnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            ui_begin_2d(rc.right, rc.bottom);
            demo_seek_draw_curtain((float)rc.right, (float)rc.bottom);
            ui_end_2d();
        }
    } else if (g_mode == 0 && wnd) {
        // no menu open: the announcement card, main menu only (no cgame = no game loaded)
        draw_news_card(wnd);
        // in game, PAM streamer team: the native caster overlay (ui/streamer_hud.cpp)
        if (streamer_hud_frame()) {
            RECT rc;
            GetClientRect(wnd, &rc);
            g_vw = rc.right; g_vh = rc.bottom;
            if (g_vw > 0 && g_vh > 0) {
                const DWORD now = GetTickCount();
                g_dt = g_last_tick ? (now - g_last_tick) / 1000.0f : 0.016f;
                if (g_dt > 0.05f) g_dt = 0.05f;
                ui_frame_dt(g_dt);
                g_last_tick = now;
                ui_begin_2d(g_vw, g_vh);
                streamer_hud_draw((float)g_vw, (float)g_vh);
                ui_alpha(1.0f);
                ui_end_2d();
            }
        }
        // the SERVER's tested hit boxes over the player you look at (cod1x_drawhitbox)
        if (hitbox_view_active()) {
            RECT rc;
            GetClientRect(wnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                ui_begin_2d(rc.right, rc.bottom);
                hitbox_view_draw((float)rc.right, (float)rc.bottom);
                ui_alpha(1.0f);
                ui_end_2d();
            }
        }
        // local server debug: the camera point over the body (player_debugEyePosition)
        if (eye_debug_active()) {
            RECT rc;
            GetClientRect(wnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                ui_begin_2d(rc.right, rc.bottom);
                eye_debug_draw((float)rc.right, (float)rc.bottom);
                ui_alpha(1.0f);
                ui_end_2d();
            }
        }
    }
    return g_orig_swap ? g_orig_swap(dc) : FALSE;
}

}  // namespace

void overlay_start() {
    wndhub_add_listener(overlay_listener, "overlay");
    void* orig = iat_hook("gdi32.dll", "SwapBuffers", (void*)hk_swapbuffers);
    if (orig) {
        g_orig_swap = (SwapBuffers_t)orig;
        logger::logf("  overlay: SwapBuffers hooked - modern menu on Ctrl+M");
    } else {
        logger::logf("  overlay: SwapBuffers import not found, modern menu OFF");
    }
}

// Watcher thread, ~1/s. The only way the menu can be entirely absent is our
// SwapBuffers hook no longer running: a third-party overlay that rewrote the exe's
// import slot AFTER us without chaining to what it found. Re-hooking on top would
// loop if that hook DOES chain to us, so this only diagnoses - with the module that
// owns the slot's new target, which names the culprit in the player's log.
void overlay_tick() {
    static long  s_last_calls = -1;
    static DWORD s_stalled_since = 0;
    static bool  s_logged = false;
    if (!g_orig_swap || s_logged) return;
    const long calls = g_swap_calls;
    if (calls != s_last_calls) { s_last_calls = calls; s_stalled_since = 0; return; }
    HWND w = wndhub_window();
    if (!w || IsIconic(w)) return;                                // no frames expected
    const DWORD now = GetTickCount();
    if (!s_stalled_since) { s_stalled_since = now; return; }
    if (now - s_stalled_since < 3000) return;
    void* slot = iat_peek("gdi32.dll", "SwapBuffers");
    if (slot == (void*)hk_swapbuffers) return;                    // ours, just no frames (paused)
    char mod[MAX_PATH] = "?";
    HMODULE hm = NULL;
    if (slot && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)slot, &hm))
        GetModuleFileNameA(hm, mod, sizeof(mod));
    s_logged = true;
    logger::logf("overlay: SwapBuffers hook BYPASSED - the import slot now points to %p (%s) "
                 "and our hook has not run for 3 s: modern menu unavailable on this machine",
                 slot, mod);
}

}  // namespace patches
