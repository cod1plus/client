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
#include "ui/modern_menu.h"
#include "ui/home_menu.h"
#include "core/iat.h"
#include "core/logger.h"
#include "input/rinput.h"
#include "video/gamma_fix.h"

#include <GL/gl.h>
#include <gdiplus.h>
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
WNDPROC       g_orig_wndproc = nullptr;
HWND          g_hooked_wnd = nullptr;

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
float   g_alpha_mul = 1.0f;
float   g_dt = 0.016f;
DWORD   g_last_tick = 0;
std::map<long, float> g_anim;
HGLRC   g_glrc = NULL;              // vid_restart detector: context change = dead textures

// ---------------------------------------------------------------- text cache
struct TextTex { GLuint id = 0; int w = 0, h = 0; };
std::map<std::string, TextTex> g_text_cache;

TextTex make_text(const char* utf8, int px, int weight) {
    TextTex out;
    wchar_t wide[512];
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 511);
    if (wn <= 0) return out;
    wide[511] = 0;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return out;
    HFONT font = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, wide, wn - 1, &sz);
    int w = sz.cx + 2, h = sz.cy + 2;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;                    // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ ob = SelectObject(dc, bmp);
        RECT rc = { 0, 0, w, h };
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 255, 255));
        ExtTextOutW(dc, 1, 1, ETO_OPAQUE, &rc, wide, wn - 1, NULL);
        GdiFlush();

        // luminance of white-on-black == coverage == alpha
        unsigned char* a = (unsigned char*)malloc((size_t)w * h);
        const DWORD* p = (const DWORD*)bits;
        for (int i = 0; i < w * h; ++i) a[i] = (unsigned char)(p[i] & 0xff);

        glGenTextures(1, &out.id);
        glBindTexture(GL_TEXTURE_2D, out.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0,
                     GL_ALPHA, GL_UNSIGNED_BYTE, a);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(a);
        out.w = w; out.h = h;
        SelectObject(dc, ob);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, of);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
}

ULONG_PTR g_gdiplus_token = 0;
bool      g_gdiplus_up    = false;

void ensure_gdiplus() {
    if (g_gdiplus_up) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &in, nullptr) == Gdiplus::Ok)
        g_gdiplus_up = true;
}

// GDI+ locks 32bpp bitmaps as BGRA in memory (little-endian); GL wants RGBA - swap
// the R/B bytes per pixel once at load time rather than fighting GL_BGRA support
// across whatever ancient GL1.x driver this idTech3 build ends up on.
TextTex load_image(const char* path) {
    TextTex out;
    ensure_gdiplus();
    if (!g_gdiplus_up) return out;

    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    Gdiplus::Bitmap bmp(wpath);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return out;

    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return out;

    Gdiplus::BitmapData bd;
    Gdiplus::Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return out;

    unsigned char* px = (unsigned char*)malloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        unsigned char* dst = px + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];   // B -> R
            dst[x * 4 + 1] = src[x * 4 + 1];   // G
            dst[x * 4 + 2] = src[x * 4 + 0];   // R -> B
            dst[x * 4 + 3] = src[x * 4 + 3];   // A
        }
    }
    bmp.UnlockBits(&bd);

    glGenTextures(1, &out.id);
    glBindTexture(GL_TEXTURE_2D, out.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px);
    out.w = w; out.h = h;
    return out;
}

const TextTex& get_text(const char* utf8, int px, int weight) {
    char key[560];
    snprintf(key, sizeof(key), "%d|%d|%s", px, weight, utf8);
    auto it = g_text_cache.find(key);
    if (it != g_text_cache.end()) return it->second;
    return g_text_cache.emplace(key, make_text(utf8, px, weight)).first->second;
}

inline void set_color(DWORD c) {
    float a = ((c >> 24) & 0xff) * g_alpha_mul;
    glColor4ub((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff,
               (GLubyte)(a > 255 ? 255 : a));
}

// ------------------------------------------------------------------ drawing
void arc_fan(float cx, float cy, float r, float a0, float a1) {
    for (int i = 0; i <= 6; ++i) {
        float a = a0 + (a1 - a0) * i / 6.0f;
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
}

}  // namespace

void ui_rect(float x, float y, float w, float h, DWORD rgba) {
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glBegin(GL_QUADS);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
}

void ui_rect_rounded(float x, float y, float w, float h, float r, DWORD rgba) {
    if (r <= 0.5f) { ui_rect(x, y, w, h, rgba); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    const float PI = 3.14159265f;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x + w / 2, y + h / 2);
    arc_fan(x + r,     y + r,     r, PI,          PI * 1.5f);   // top-left
    arc_fan(x + w - r, y + r,     r, PI * 1.5f,   PI * 2.0f);   // top-right
    arc_fan(x + w - r, y + h - r, r, 0,           PI * 0.5f);   // bottom-right
    arc_fan(x + r,     y + h - r, r, PI * 0.5f,   PI);          // bottom-left
    glVertex2f(x, y + r);                                       // close the fan
    glEnd();
}

void ui_rect_border(float x, float y, float w, float h, float r, float th, DWORD rgba) {
    // two rounded rects would need stenciling; a line loop is enough at 1-2px
    (void)r;
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glLineWidth(th);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y); glVertex2f(x + w, y);
    glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
    glLineWidth(1.0f);
}

void ui_ellipse(float cx, float cy, float rx, float ry, float th, DWORD rgba) {
    glDisable(GL_TEXTURE_2D);
    set_color(rgba);
    glLineWidth(th);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 40; ++i) {
        float a = 2.0f * 3.14159265f * i / 40.0f;
        glVertex2f(cx + cosf(a) * rx, cy + sinf(a) * ry);
    }
    glEnd();
    glLineWidth(1.0f);
}

float ui_text(float x, float y, int px, int weight, DWORD rgba, const char* utf8) {
    const TextTex& t = get_text(utf8, px, weight);
    if (!t.id) return 0;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    set_color(rgba);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + t.w, y);
    glTexCoord2f(1, 1); glVertex2f(x + t.w, y + t.h);
    glTexCoord2f(0, 1); glVertex2f(x, y + t.h);
    glEnd();
    return (float)t.w;
}

bool ui_image_cover(float x, float y, float w, float h, const char* path) {
    char key[560];
    snprintf(key, sizeof(key), "img:%s", path);
    auto it = g_text_cache.find(key);
    if (it == g_text_cache.end()) {
        it = g_text_cache.emplace(key, load_image(path)).first;
    }
    const TextTex& t = it->second;
    if (!t.id) return false;

    float img_ar = (float)t.w / t.h, box_ar = w / h;
    float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
    if (img_ar > box_ar) {                    // image wider than box: crop left/right
        float keep = box_ar / img_ar;
        u0 = (1.0f - keep) * 0.5f; u1 = 1.0f - u0;
    } else {                                   // image taller than box: crop top/bottom
        float keep = img_ar / box_ar;
        v0 = (1.0f - keep) * 0.5f; v1 = 1.0f - v0;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    set_color(0xFFFFFFFF);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
    glTexCoord2f(u0, v1); glVertex2f(x, y + h);
    glEnd();
    return true;
}

float ui_text_width(int px, int weight, const char* utf8) {
    return (float)get_text(utf8, px, weight).w;
}

const UiInput& ui_input() { return g_in; }

float ui_smooth(long key, float target, float speed) {
    auto it = g_anim.find(key);
    if (it == g_anim.end()) it = g_anim.emplace(key, target).first;
    float k = 1.0f - expf(-speed * g_dt);
    it->second += (target - it->second) * k;
    if (it->second > target - 0.001f && it->second < target + 0.001f)
        it->second = target;
    return it->second;
}
void ui_anim_set(long key, float v) { g_anim[key] = v; }
void ui_alpha(float mul) { g_alpha_mul = mul < 0 ? 0 : (mul > 1 ? 1 : mul); }
float ui_alpha_get() { return g_alpha_mul; }
void ui_key_capture(bool on) { g_keycap = on; if (!on) g_in.vk = 0; }
bool overlay_visible() { return g_mode != 0; }
HWND overlay_game_window() { return g_hooked_wnd; }

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

LRESULT CALLBACK hk_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    // Restore the desktop's gamma HERE, not (only) from DLL_PROCESS_DETACH. Calling
    // GDI functions from DllMain's detach handler is a documented Microsoft anti-
    // pattern (loader lock), and worse: on ExitProcess() - which is how Sys_Quit
    // leaves - Windows frequently SKIPS DLL_PROCESS_DETACH entirely for already-
    // loaded DLLs, as a fast-exit optimisation. That is the "parfois" in "la
    // brightness du jeu... l'applique parfois au bureau" (2026-08-15): the restore
    // call simply never ran on those exits. WM_DESTROY fires on the main thread while
    // the window and full GDI state are still alive, well before ExitProcess - a
    // reliable hook regardless of how the process is about to end. This wndproc is
    // ALSO the one subclass that installs unconditionally in every display mode (via
    // WindowFromDC, see ensure_subclass) - window_patch.cpp's own WM_DESTROY handler
    // exists but only subclasses when window_borderless=on, which is not enzo's
    // config (fullscreen=on) - so it never even runs for him. Must run before the
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
        case WM_KEYDOWN: case WM_SYSKEYDOWN: g_in.vk = (int)wp;      return 0;
        case WM_LBUTTONDOWN:                 g_in.vk = VK_LBUTTON;   return 0;
        case WM_RBUTTONDOWN:                 g_in.vk = VK_RBUTTON;   return 0;
        case WM_MBUTTONDOWN:                 g_in.vk = VK_MBUTTON;   return 0;
        case WM_XBUTTONDOWN:
            g_in.vk = (HIWORD(wp) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            return 0;
        case WM_MOUSEWHEEL:
            g_in.vk = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0xF001 : 0xF002;
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR:
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            return 0;
        }
    }

    // Ctrl+M only (enzo, 2026-09-17): INSERT is a key players bind, and a bare key
    // fires while aiming or typing; a chord does not
    if (m == WM_KEYDOWN && wp == 'M' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (g_mode == 0) {
            if (home_menu_is_active()) { g_home_suppress = false; set_mode(2, false); }
            else overlay_toggle(true);
        }
        else if (g_mode == 1 && g_came_from_home) set_mode(2, false);  // settings -> home
        else overlay_toggle(false);
        return 0;
    }
    if (g_mode == 0)
        return CallWindowProcA(g_orig_wndproc, w, m, wp, lp);

    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_mode == 1 && g_came_from_home) set_mode(2, false);   // settings -> home
            else if (g_mode == 2) { home_menu_close_via_escape(); set_mode(0, false); }
            else overlay_toggle(false);
            return 0;
        }
        return 0;                                   // swallow
    case WM_KEYUP: case WM_SYSKEYUP:
        // RELEASES go through: the Ctrl of Ctrl+M went down before we opened, and if
        // its release is swallowed the engine keeps Ctrl held - console "a" = home,
        // crouch stuck (enzo, 2026-09-18). A release of an already-up key is a no-op.
        return CallWindowProcA(g_orig_wndproc, w, m, wp, lp);
    case WM_CHAR: case WM_SYSKEYDOWN:
        return 0;
    case WM_MOUSEMOVE: {
        if (g_saw_raw) return 0;            // raw input owns the cursor
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
        return 0;
    }
    case WM_LBUTTONDOWN: g_in.down = true;  return 0;
    case WM_LBUTTONUP:   g_in.down = false; return 0;
    case WM_MOUSEWHEEL:
        g_in.wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        return 0;
    }
    return CallWindowProcA(g_orig_wndproc, w, m, wp, lp);
}

void ensure_subclass(HDC dc) {
    // Do NOT use window_patch::get_game_window(): it only returns a handle once the
    // BORDERLESS watcher thread has run, and that thread is entirely gated behind
    // `window_borderless = on` (window_patch.cpp:264). With window_borderless = off
    // (enzo's own 1440x1080-stretched fullscreen setup) the watcher never starts, the
    // handle stays null forever, and Ctrl+M has nothing to attach to - found
    // live 2026-08-10 right after that exact config was applied via ini + vid_restart.
    // WindowFromDC is the standard overlay technique instead: the HDC handed to us at
    // every SwapBuffers already belongs to the game's window, in every display mode.
    HWND w = WindowFromDC(dc);
    if (!w || w == g_hooked_wnd) return;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)hk_wndproc);
    if (prev) {
        g_orig_wndproc = prev;
        g_hooked_wnd = w;
        logger::logf("  overlay: window subclassed (hwnd %p)", (void*)w);
    }
}


// ------------------------------------------------------------------- swap
BOOL WINAPI hk_swapbuffers(HDC dc) {
    ensure_subclass(dc);
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
    if (g_mode != 0 && g_hooked_wnd) {
        RECT rc;
        GetClientRect(g_hooked_wnd, &rc);
        g_vw = rc.right; g_vh = rc.bottom;
        if (g_vw > 0 && g_vh > 0) {
            // vid_restart destroys the GL context; every cached texture id then
            // belongs to a dead context and draws as garbage squares (seen live
            // 2026-08-10: the whole menu turned into black-and-white blocks).
            // Forget the cache and re-rasterise lazily in the new context.
            HGLRC rc = wglGetCurrentContext();
            if (rc != g_glrc) { g_glrc = rc; g_text_cache.clear(); }
            glPushAttrib(GL_ALL_ATTRIB_BITS);
            glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
            glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
            glOrtho(0, g_vw, g_vh, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
            glViewport(0, 0, g_vw, g_vh);
            glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glDisable(GL_LIGHTING);   glDisable(GL_SCISSOR_TEST);
            glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
            glDisableClientState(GL_VERTEX_ARRAY);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

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
            set_color(0xFF000000);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx, my); glVertex2f(mx + 15, my + 6); glVertex2f(mx + 6, my + 15);
            glEnd();
            set_color(0xFFFFFFFF);
            glBegin(GL_TRIANGLES);
            glVertex2f(mx + 1, my + 2); glVertex2f(mx + 12, my + 6.5f); glVertex2f(mx + 6.5f, my + 12);
            glEnd();
            g_prev_down = g_in.down;
            g_in.wheel = 0;
            g_in.vk = 0;                        // capture events are one-frame

            glMatrixMode(GL_MODELVIEW); glPopMatrix();
            glMatrixMode(GL_PROJECTION); glPopMatrix();
            glPopClientAttrib();
            glPopAttrib();
        }
    }
    return g_orig_swap ? g_orig_swap(dc) : FALSE;
}

}  // namespace

void overlay_start() {
    void* orig = iat_hook("gdi32.dll", "SwapBuffers", (void*)hk_swapbuffers);
    if (orig) {
        g_orig_swap = (SwapBuffers_t)orig;
        logger::logf("  overlay: SwapBuffers hooked - modern menu on Ctrl+M");
    } else {
        logger::logf("  overlay: SwapBuffers import not found, modern menu OFF");
    }
}

}  // namespace patches
