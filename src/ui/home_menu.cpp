// The home screen: a native replacement for the engine's "main" menu, drawn by the GL
// overlay while "main" is the engine's top menu (ui/menu_hooks.cpp knows, from inside
// ui_mp_x86.dll - no pk3 override) and no game is loaded.
//
// Three photo cards - PLAY / HOST / SETTINGS - a wordmark, a player plate, a footer
// of shortcuts: the layout of a modern shooter's front end, in the mod's own
// monochrome. PLAY and HOST forward the exact engine commands the original
// main.menu buttons ran (`open joinserver`, `open createserver` - the engine's own
// screens take over, and this screen returns when they close). QUIT is our own
// confirmation, then `quit`.
//
// Art: <game dir>\cod1reloaded\home\{bg,play,host,settings}.jpg - crops of the
// game's own main-menu painting (pak0 ui/assets/main_back.tga: the Thompson, the
// helmet, the first-aid pouch), desaturated. Every image is optional: a missing
// file leaves a flat panel, nothing breaks.
//
// RESPONSIVE: one scale factor s = height / 1080 (height, not width: 1440x1080
// stretched and 1920x1080 share it, so type and spacing stay identical between
// them); the row of cards is centred in whatever width there is.
#include "ui/home_menu.h"
#include "ui/gl_overlay.h"
#include "ui/menu_hooks.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA / CODMP_CBUF_EXECTEXT_VA
#include "features/news.h"
#include "features/updater.h"         // COD1RELOADED_VERSION
#include "core/logger.h"

#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <windows.h>

namespace patches {

namespace {

typedef void (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
const Cbuf_ExecuteText_t Cbuf    = (Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA;
const Cvar_FindVar_t     FindVar = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
constexpr int EXEC_APPEND = 2;
constexpr int CV_STRING   = 0x04;

const char* cv_str(const char* n, const char* fb) {
    void* c = FindVar(n);
    return c ? *(const char**)((char*)c + CV_STRING) : fb;
}
void cmdf(const char* fmt, ...) {
    char b[256]; va_list a; va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    Cbuf(EXEC_APPEND, b);
}
bool hit(float x, float y, float w, float h) {
    const UiInput& in = ui_input();
    return in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
}
int px(float v, float s) { int r = (int)(v * s + 0.5f); return r < 1 ? 1 : r; }

DWORD mixc(DWORD a, DWORD b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    DWORD r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        r |= (DWORD)(ca + (int)((cb - ca) * t)) << sh;
    }
    return r;
}
DWORD with_alpha(DWORD c, float a) {
    if (a < 0) a = 0; if (a > 1) a = 1;
    return (c & 0x00FFFFFF) | ((DWORD)(a * 255) << 24);
}

// the game folder + a file under cod1reloaded\home\ (art is optional, see header)
bool home_asset(const char* name, char* out, size_t n) {
    char p[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, p, MAX_PATH);
    char* sl = len ? strrchr(p, '\\') : nullptr;
    if (!sl) { out[0] = 0; return false; }
    *(sl + 1) = 0;
    snprintf(out, n, "%scod1reloaded\\home\\%s", p, name);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// player name without the engine's ^N colour codes
void plain_name(char* out, size_t n) {
    const char* s = cv_str("name", "Player");
    size_t k = 0;
    for (; *s && k + 1 < n; ++s) {
        if (*s == '^' && s[1]) { ++s; continue; }
        out[k++] = *s;
    }
    out[k] = 0;
    if (!k) snprintf(out, n, "Player");
}

// uppercase, letter-spaced (the eyebrow / caption style), centred on cx
float tracked_width(int fpx, int weight, const char* s, float tr) {
    float w = 0;
    for (const char* c = s; *c; ++c) { char g[2] = { *c, 0 }; w += ui_text_width(fpx, weight, g) + tr; }
    return w > tr ? w - tr : 0;
}
void text_tracked(float x, float y, int fpx, int weight, DWORD col, const char* s, float tr) {
    for (const char* c = s; *c; ++c) { char g[2] = { *c, 0 }; x += ui_text(x, y, fpx, weight, col, g) + tr; }
}
void text_tracked_c(float cx, float y, int fpx, int weight, DWORD col, const char* s, float tr) {
    text_tracked(cx - tracked_width(fpx, weight, s, tr) / 2, y, fpx, weight, col, s, tr);
}
void text_c(float cx, float y, int fpx, int weight, DWORD col, const char* s) {
    ui_text(cx - ui_text_width(fpx, weight, s) / 2, y, fpx, weight, col, s);
}

// vertical gradient: transparent at the top, `a1` black at the bottom
void vgrad(float x, float y, float w, float h, float a1, int rows = 24) {
    for (int i = 0; i < rows; ++i) {
        const float f0 = (float)i / rows, f1 = (float)(i + 1) / rows;
        const float a = f0 * f0 * a1;
        ui_rect(x, y + h * f0, w, h * (f1 - f0) + 1, ((DWORD)(a * 255) << 24));
    }
}

// Small bordered key hint, e.g. [ CTRL+M ] label
float keycap(float x, float y, const char* key, const char* label, float s) {
    const float pad = 9 * s, h = 24 * s;
    float kw = ui_text_width(px(12, s), 600, key) + pad * 2;
    ui_rect_rounded(x, y, kw, h, 4 * s, 0xFF141414);
    ui_rect_border(x, y, kw, h, 4 * s, 1.0f * s, 0xFF333333);
    ui_text(x + pad, y + 4 * s, px(12, s), 600, UI_MUTED, key);
    float total = kw;
    if (label && *label) {
        text_tracked(x + kw + 10 * s, y + 6 * s, px(11, s), 600, 0xFF6A6A6A, label, 1.6f * s);
        total += 10 * s + tracked_width(px(11, s), 600, label, 1.6f * s);
    }
    return total;
}

// the three glyphs, geometric and monochrome, drawn inside a thin diamond
enum Glyph { G_PLAY, G_HOST, G_SETTINGS };
void diamond(float cx, float cy, float r, float th, DWORD c) {
    // a thin ring: the primitives have no rotated rectangle, and a ring reads as
    // the same "marker" at this size
    ui_ellipse(cx, cy, r, r, th, c);
}
void glyph(Glyph g, float cx, float cy, float s, float hv) {
    const DWORD c = mixc(0xFFBDBDBD, UI_TEXT, hv);
    const float r = 19 * s;
    diamond(cx, cy, r, 1.2f * s, with_alpha(c, 0.55f + 0.45f * hv));
    switch (g) {
    case G_PLAY:      // a filled point: the objective
        ui_rect_rounded(cx - 5 * s, cy - 5 * s, 10 * s, 10 * s, 5 * s, c);
        break;
    case G_HOST: {    // three bars: a rack
        const float bw = 14 * s, bh = 2.5f * s;
        ui_rect(cx - bw / 2, cy - 6 * s, bw, bh, c);
        ui_rect(cx - bw / 2, cy - bh / 2, bw, bh, c);
        ui_rect(cx - bw / 2, cy + 6 * s - bh, bw, bh, c);
        break;
    }
    case G_SETTINGS:  // two rings: a dial
        ui_ellipse(cx, cy, 9 * s, 9 * s, 1.6f * s, c);
        ui_rect_rounded(cx - 2.5f * s, cy - 2.5f * s, 5 * s, 5 * s, 2.5f * s, c);
        break;
    }
}

// One card. Returns true on click.
bool card(int idx, float x, float y, float w, float h, const char* img, Glyph g,
          const char* title, const char* caption, const char* line1, const char* line2,
          float ot, float s) {
    // staggered entrance: each card 90 ms after the previous
    const float delay = idx * 0.09f;
    float t = (ot - delay) / (1.0f - delay);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float ease = t * t * (3 - 2 * t);
    const float rise = (1 - ease) * 24 * s;
    y += rise;
    ui_alpha(ease);

    const bool over = hit(x, y, w, h);
    const float hv = ui_smooth(7200 + idx, over ? 1.0f : 0.0f, 10.0f);
    // hover: the card lifts a little and breathes
    const float grow = 6 * s * hv;
    x -= grow / 2; y -= grow; w += grow; h += grow;

    // photo (or a flat plate), then the scrim that carries the type
    bool have = img && ui_image_cover(x, y, w, h, img);
    if (!have) ui_rect(x, y, w, h, 0xFF101010);
    ui_rect(x, y, w, h, with_alpha(0xFF000000, 0.42f - 0.30f * hv));      // dim, lifts on hover
    vgrad(x, y + h * 0.35f, w, h * 0.65f, 0.92f);
    // frame: hairline, brighter on hover; viewfinder ticks in the corners
    ui_rect_border(x + 0.5f, y + 0.5f, w - 1, h - 1, 0, 1.0f * s, mixc(0x2EFFFFFF, 0xFFFFFFFF, hv));
    {
        const float a = 10 * s, th = 1.5f * s;
        const DWORD c = mixc(0x70FFFFFF, 0xFFFFFFFF, hv);
        ui_rect(x, y, a, th, c);                 ui_rect(x, y, th, a, c);
        ui_rect(x + w - a, y, a, th, c);         ui_rect(x + w - th, y, th, a, c);
        ui_rect(x, y + h - th, a, th, c);        ui_rect(x, y + h - a, th, a, c);
        ui_rect(x + w - a, y + h - th, a, th, c); ui_rect(x + w - th, y + h - a, th, a, c);
    }
    // type block, anchored to the lower part
    const float cx = x + w / 2;
    float ty = y + h * 0.60f;
    glyph(g, cx, ty, s, hv);
    ty += 40 * s;
    text_c(cx, ty, px(30, s), 600, mixc(0xFFE6E6E6, UI_TEXT, hv), title);
    ty += 42 * s;
    text_tracked_c(cx, ty, px(11, s), 600, mixc(0xFF8A8A8A, 0xFFC8C8C8, hv), caption, 2.2f * s);
    ty += 30 * s;
    text_c(cx, ty, px(13, s), 400, 0xFF7A7A7A, line1);
    if (line2 && *line2) text_c(cx, ty + 19 * s, px(13, s), 400, 0xFF7A7A7A, line2);
    return over && ui_input().clicked;
}

bool g_confirm_quit = false;

}  // namespace

// ---------------------------------------------------------------------------------

bool home_menu_is_active() {
    static DWORD s_probe = 0;
    static bool  s_in_game = false;
    const DWORD now = GetTickCount();
    if (now - s_probe > 300) { s_probe = now; s_in_game = GetModuleHandleA("cgame_mp_x86.dll") != NULL; }
    return ui_main_menu_active() && !s_in_game;
}

bool home_menu_escape() {
    g_confirm_quit = !g_confirm_quit;
    return true;
}

void home_menu_close_via_escape() { g_confirm_quit = false; }

HomeAction home_menu_draw(float sw, float sh) {
    float s = sh / 1080.0f;
    if (s < 0.6f) s = 0.6f;
    if (s > 1.8f) s = 1.8f;

    // entrance: re-armed after 250 ms without a frame (i.e. every time the screen appears)
    static DWORD s_lastdraw = 0;
    const DWORD nowt = GetTickCount();
    if (nowt - s_lastdraw > 250) { ui_anim_set(7000, 0.0f); g_confirm_quit = false; }
    s_lastdraw = nowt;
    const float ot = ui_smooth(7000, 1.0f, 4.5f);

    // ---- ground: the painting, desaturated and pushed back, then a vignette
    ui_alpha(1.0f);
    char path[MAX_PATH];
    bool have_bg = home_asset("bg.jpg", path, sizeof(path)) && ui_image_cover(0, 0, sw, sh, path);
    if (!have_bg) ui_rect(0, 0, sw, sh, UI_BG);
    ui_rect(0, 0, sw, sh, have_bg ? 0x9E000000 : 0x00000000);
    vgrad(0, sh * 0.55f, sw, sh * 0.45f, 0.85f, 20);
    // top band
    for (int i = 0; i < 12; ++i) {
        const float f = 1.0f - (float)i / 12;
        ui_rect(0, i * (90 * s / 12), sw, 90 * s / 12 + 1, with_alpha(0xFF000000, f * f * 0.7f));
    }

    // ---- wordmark, top-left: CALL OF DUTY  [1.6X]
    ui_alpha(ot);
    {
        const float x = 48 * s, y = 26 * s;
        ui_text(x, y, px(28, s), 600, UI_TEXT, "CALL OF DUTY");
        const float tw = ui_text_width(px(28, s), 600, "CALL OF DUTY");
        const float bx = x + tw + 14 * s, bw = ui_text_width(px(22, s), 600, "1.6X") + 22 * s;
        ui_rect_rounded(bx, y + 3 * s, bw, 34 * s, 3 * s, UI_ACCENT);
        ui_text(bx + 11 * s, y + 6 * s, px(22, s), 600, 0xFF0A0A0A, "1.6X");
    }
    // ---- player plate, top-right
    {
        char name[64]; plain_name(name, sizeof(name));
        const float ph = 44 * s, pw = 260 * s, x = sw - 48 * s - pw, y = 22 * s;
        ui_rect_rounded(x, y, pw, ph, 3 * s, 0xB0101010);
        ui_rect_border(x + 0.5f, y + 0.5f, pw - 1, ph - 1, 3 * s, 1.0f * s, 0x33FFFFFF);
        ui_rect(x, y, 3 * s, ph, UI_ACCENT);
        // avatar square with a "?" - a rank/avatar placeholder
        ui_rect_rounded(x + 12 * s, y + 7 * s, 30 * s, 30 * s, 2 * s, 0xFF1E1E1E);
        ui_rect_border(x + 12 * s, y + 7 * s, 30 * s, 30 * s, 2 * s, 1.0f * s, 0x40FFFFFF);
        text_c(x + 27 * s, y + 9 * s, px(18, s), 600, UI_MUTED, "?");
        text_tracked(x + 54 * s, y + 7 * s, px(10, s), 600, 0xFF8A8A8A, "PLAYER", 2.0f * s);
        ui_text(x + 54 * s, y + 20 * s, px(14, s), 600, UI_TEXT, name);
    }
    ui_rect(0, 90 * s, sw, 1, 0x1EFFFFFF);

    // ---- the cards
    const float cw = 300 * s, ch = 560 * s, gap = 54 * s;
    float x0 = (sw - (cw * 3 + gap * 2)) / 2;
    const float cy = 150 * s;
    HomeAction result = HomeAction::None;
    char play[MAX_PATH], host[MAX_PATH], sett[MAX_PATH];
    const char* pplay = home_asset("play.jpg", play, sizeof(play)) ? play : nullptr;
    const char* phost = home_asset("host.jpg", host, sizeof(host)) ? host : nullptr;
    const char* psett = home_asset("settings.jpg", sett, sizeof(sett)) ? sett : nullptr;

    if (card(0, x0, cy, cw, ch, pplay, G_PLAY, "PLAY", "ENTER THE BATTLEFIELD",
             "Browse the 1.6X servers and join", "a match.", ot, s) && !g_confirm_quit) {
        // NO "close main" (2026-08-15): forwarded from outside the menu VM it corrupts
        // the UI stack and "open joinserver" silently fails; joinserver is fullscreen
        // and covers main regardless.
        cmdf("close mods_menu\nclose options_multi\nopen joinserver\n");
        logger::logf("home_menu: PLAY -> open joinserver");
        result = HomeAction::Navigated;
    }
    if (card(1, x0 + cw + gap, cy, cw, ch, phost, G_HOST, "HOST", "RUN YOUR OWN MATCH",
             "Start a server on this machine", "for friends or practice.", ot, s) && !g_confirm_quit) {
        cmdf("close mods_menu\nclose options_multi\nopen createserver\n");
        logger::logf("home_menu: HOST -> open createserver");
        result = HomeAction::Navigated;
    }
    if (card(2, x0 + (cw + gap) * 2, cy, cw, ch, psett, G_SETTINGS, "SETTINGS", "CHANGE YOUR OPTIONS",
             "Display, mouse, netcode, PAM,", "textures and demos.", ot, s) && !g_confirm_quit) {
        result = HomeAction::OpenSettings;
    }

    // ---- news line, under the cards
    {
        char nt[64], nx[128], nu[256];
        if (news_get(nt, sizeof(nt), nx, sizeof(nx), nu, sizeof(nu))) {
            ui_alpha(ot);
            for (char* c = nt; *c; ++c) *c = (char)toupper((unsigned char)*c);
            const float y = cy + ch + 34 * s;
            const float pulse = 0.55f + 0.45f * sinf(nowt * 0.0045f);
            char line[256];
            snprintf(line, sizeof(line), "%s", nx);
            const int fe = px(11, s), fx = px(14, s);
            const float we = tracked_width(fe, 600, nt, 2.4f * s), wx = ui_text_width(fx, 400, line);
            const float wh = nu[0] ? tracked_width(px(10, s), 600, "CTRL + N", 1.8f * s) : 0;
            const float total = 10 * s + 14 * s + we + 22 * s + wx + (nu[0] ? 22 * s + wh : 0);
            float x = (sw - total) / 2;
            const float cyy = y + fe * 0.7f;
            ui_ellipse(x + 5 * s, cyy, 5 * s * (1.4f + 0.8f * pulse), 5 * s * (1.4f + 0.8f * pulse), 1.0f,
                       with_alpha(0xFFFFFFFF, 0.25f * pulse));
            ui_rect_rounded(x + 2 * s, cyy - 3 * s, 6 * s, 6 * s, 3 * s, UI_ACCENT);
            x += 10 * s + 14 * s;
            text_tracked(x, y, fe, 600, UI_ACCENT, nt, 2.4f * s); x += we + 22 * s;
            ui_text(x, y - 2 * s, fx, 400, 0xFFB0B0B0, line);     x += wx;
            if (nu[0]) { x += 22 * s; text_tracked(x, y + 1 * s, px(10, s), 600, UI_MUTED, "CTRL + N", 1.8f * s); }
        }
    }

    // ---- footer
    ui_alpha(ot);
    {
        char ver[48]; snprintf(ver, sizeof(ver), "1.6X  %s", COD1RELOADED_VERSION);
        ui_rect(0, sh - 58 * s, sw, 1, 0x1EFFFFFF);
        ui_text(48 * s, sh - 40 * s, px(12, s), 400, 0xFF5A5A5A, ver);
        // the engine's own screens the cards do not cover: key bindings live in the
        // vanilla Options, and Mods is where fs_game is switched. Tracked text links.
        {
            float lx = 48 * s + ui_text_width(px(12, s), 400, ver) + 40 * s;
            const char* links[2] = { "GAME OPTIONS", "MODS" };
            const char* cmds[2]  = { "close mods_menu\nopen options_multi\n", "close options_multi\nopen mods_menu\n" };
            for (int i = 0; i < 2; ++i) {
                const float lw = tracked_width(px(11, s), 600, links[i], 1.8f * s);
                const bool over = hit(lx - 8 * s, sh - 46 * s, lw + 16 * s, 34 * s);
                const float hv = ui_smooth(7400 + i, over ? 1.f : 0.f, 12.f);
                text_tracked(lx, sh - 38 * s, px(11, s), 600, mixc(0xFF6A6A6A, UI_TEXT, hv), links[i], 1.8f * s);
                ui_rect(lx, sh - 22 * s, lw * hv, 1, UI_ACCENT);
                if (over && ui_input().clicked && !g_confirm_quit) {
                    cmdf("%s", cmds[i]);
                    logger::logf("home_menu: %s", links[i]);
                    result = HomeAction::Navigated;
                }
                lx += lw + 34 * s;
            }
        }
        float fx = sw - 48 * s;
        // right-aligned run: [ESC] QUIT   [CTRL+M] SETTINGS
        const float w_quit = ui_text_width(px(12, s), 600, "ESC") + 18 * s + 10 * s + tracked_width(px(11, s), 600, "QUIT", 1.6f * s);
        fx -= w_quit;
        const bool over_quit = hit(fx - 8 * s, sh - 46 * s, w_quit + 16 * s, 34 * s);
        keycap(fx, sh - 41 * s, "ESC", "QUIT", s);
        if (over_quit && ui_input().clicked) g_confirm_quit = true;
        const float w_set = ui_text_width(px(12, s), 600, "CTRL+M") + 18 * s + 10 * s + tracked_width(px(11, s), 600, "SETTINGS", 1.6f * s);
        fx -= 34 * s + w_set;
        keycap(fx, sh - 41 * s, "CTRL+M", "SETTINGS", s);
    }

    // ---- quit confirmation
    if (g_confirm_quit) {
        ui_alpha(1.0f);
        ui_rect(0, 0, sw, sh, 0xB4000000);
        const float pw = 440 * s, ph = 190 * s, x = (sw - pw) / 2, y = (sh - ph) / 2;
        ui_rect_rounded(x, y, pw, ph, 6 * s, 0xFF0C0C0C);
        ui_rect_border(x + 0.5f, y + 0.5f, pw - 1, ph - 1, 6 * s, 1.0f * s, 0x3CFFFFFF);
        text_tracked_c(x + pw / 2, y + 26 * s, px(11, s), 600, UI_MUTED, "LEAVE THE GAME", 2.4f * s);
        text_c(x + pw / 2, y + 52 * s, px(24, s), 600, UI_TEXT, "Quit to desktop?");
        const float bw = 150 * s, bh = 42 * s, by = y + ph - 32 * s - bh;
        const float bx1 = x + pw / 2 - bw - 10 * s, bx2 = x + pw / 2 + 10 * s;
        const bool o1 = hit(bx1, by, bw, bh), o2 = hit(bx2, by, bw, bh);
        const float h1 = ui_smooth(7301, o1 ? 1.f : 0.f, 12.f), h2 = ui_smooth(7302, o2 ? 1.f : 0.f, 12.f);
        ui_rect_rounded(bx1, by, bw, bh, 4 * s, mixc(0xFF1A1A1A, 0xFF2A2A2A, h1));
        ui_rect_border(bx1 + 0.5f, by + 0.5f, bw - 1, bh - 1, 4 * s, 1.0f * s, mixc(0x30FFFFFF, 0x80FFFFFF, h1));
        text_c(bx1 + bw / 2, by + 11 * s, px(14, s), 600, UI_TEXT, "Stay");
        ui_rect_rounded(bx2, by, bw, bh, 4 * s, mixc(0xFFE0E0E0, 0xFFFFFFFF, h2));
        text_c(bx2 + bw / 2, by + 11 * s, px(14, s), 600, 0xFF0A0A0A, "Quit");
        if (o1 && ui_input().clicked) g_confirm_quit = false;
        if (o2 && ui_input().clicked) {
            logger::logf("home_menu: QUIT confirmed");
            cmdf("quit\n");
            g_confirm_quit = false;
        }
    }

    ui_alpha(1.0f);
    return result;
}

}  // namespace patches
