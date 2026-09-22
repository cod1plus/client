// The home screen: a native replacement for the engine's "main" menu, drawn by the GL
// overlay while "main" is the engine's top menu (ui/menu_hooks.cpp knows, from inside
// ui_mp_x86.dll - no pk3 override) and no game is loaded.
//
// The game's own player model, life-size on a slow turntable under a studio light,
// and three photo cards - PLAY / HOST / SETTINGS - that shift with the cursor
// (parallax) and catch a light sweep on hover; a wordmark, the player's name, a
// news line, a footer of shortcuts. A modern shooter's front end in the mod's own
// monochrome. PLAY and HOST forward the exact engine commands the original
// main.menu buttons ran (`open joinserver`, `open createserver` - the engine's own
// screens take over, and this screen returns when they close). QUIT is our own
// confirmation, then `quit`.
//
// Art: <game dir>\cod1reloaded\home\{play,host,settings}.jpg - quiet details cut
// from the game's own main-menu painting (pak0 ui/assets/main_back.tga: the Thompson's
// grip, the helmet's curve, the scabbard), desaturated. Every image is optional: a
// missing file leaves a flat panel, nothing breaks. The ground is flat black - the
// photos are the only texture on the screen.
//
// RESPONSIVE: one scale factor s = height / 1080 (height, not width: 1440x1080
// stretched and 1920x1080 share it, so type and spacing stay identical between
// them); the row of cards is centred in whatever width there is.
#include "ui/home_menu.h"
#include "ui/gl_overlay.h"
#include "ui/menu_hooks.h"
#include "ui/player_preview.h"
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

// The one colour on this screen: a signal red, used where something is live or
// chosen - the mark, the live dot, the rule under the card you are about to pick.
constexpr DWORD HOME_RED   = 0xFFE23B2E;
constexpr DWORD CARD_COOL  = 0xFF0A1220;   // the cards' shadow leans cold: the warm rim on the hero reads warmer

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
    const float pad = 8 * s, h = 22 * s;
    float kw = ui_text_width(px(11, s), 600, key) + pad * 2;
    ui_rect_rounded(x, y, kw, h, 3 * s, 0xFF121212);
    ui_rect_border(x, y, kw, h, 3 * s, 1.0f * s, 0xFF303030);
    ui_text(x + pad, y + 3 * s, px(11, s), 600, UI_MUTED, key);
    float total = kw;
    if (label && *label) {
        text_tracked(x + kw + 10 * s, y + 6 * s, px(10, s), 600, 0xFF6A6A6A, label, 1.8f * s);
        total += 10 * s + tracked_width(px(10, s), 600, label, 1.8f * s);
    }
    return total;
}

// One card: a quiet photo detail, an index, one type block bottom-left, a rule that
// grows under the title on hover, a light that sweeps across the photo the moment
// the cursor arrives. `sweep_t` is that sweep's clock (seconds since hover began,
// < 0 = none). Returns true on click.
bool card(int idx, float x, float y, float w, float h, const char* img,
          const char* caption, const char* title, float ot, float s, float* sweep_start) {
    // staggered entrance: fade + a short rise, each card 80 ms after the previous
    const float delay = idx * 0.08f;
    float t = (ot - delay) / (1.0f - delay);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float ease = t * t * (3 - 2 * t);
    y += (1 - ease) * 18 * s;
    ui_alpha(ease);

    const bool over = hit(x, y, w, h);
    const float hv = ui_smooth(7200 + idx, over ? 1.0f : 0.0f, 9.0f);
    const float now = GetTickCount() / 1000.0f;
    if (over && *sweep_start < 0) *sweep_start = now;          // arm the sweep on arrival
    if (!over) *sweep_start = -1;

    if (!(img && ui_image_cover(x, y, w, h, img))) ui_rect(x, y, w, h, 0xFF0E0E0E);
    ui_rect(x, y, w, h, with_alpha(CARD_COOL, 0.58f - 0.36f * hv));    // sits back (cold), lifts on hover
    // the sweep: a soft vertical band crossing the card once, left to right, 0.9 s
    if (*sweep_start >= 0) {
        const float p = (now - *sweep_start) / 0.9f;
        if (p < 1.0f) {
            const float bx = x - 80 * s + (w + 160 * s) * p, bw = 90 * s;
            ui_clip_push(x, y, w, h);
            for (int i = 0; i < 12; ++i) {
                const float f = (i + 0.5f) / 12.0f;
                const float a = (1.0f - fabsf(f - 0.5f) * 2.0f) * 0.13f * (1.0f - p * 0.4f);
                ui_rect(bx + bw * i / 12.0f, y, bw / 12.0f + 1, h, with_alpha(0xFFFFFFFF, a));
            }
            ui_clip_pop();
        }
    }
    vgrad(x, y + h * 0.45f, w, h * 0.55f, 0.95f, 30);
    ui_rect_border(x + 0.5f, y + 0.5f, w - 1, h - 1, 0, 1.0f * s, mixc(0x24FFFFFF, 0xE0FFFFFF, hv));

    char num[4]; snprintf(num, sizeof(num), "%02d", idx + 1);
    text_tracked(x + 24 * s, y + 22 * s, px(11, s), 600, mixc(0x50FFFFFF, HOME_RED, hv), num, 2.0f * s);
    text_tracked(x + 24 * s, y + h - 84 * s, px(10, s), 600, mixc(0xFF969696, 0xFFD2D2D2, hv), caption, 2.4f * s);
    ui_text(x + 24 * s, y + h - 66 * s, px(34, s), 600, UI_TEXT, title);
    ui_rect(x + 24 * s, y + h - 24 * s, (w - 48 * s) * hv, 2.0f * s, HOME_RED);
    return over && ui_input().clicked;
}

// a tracked text link with a rule that grows on hover; returns true on click
bool link(long key, float x, float y, const char* label, float s) {
    const float lw = tracked_width(px(10, s), 600, label, 1.8f * s);
    const bool over = hit(x - 8 * s, y - 10 * s, lw + 16 * s, 30 * s);
    const float hv = ui_smooth(key, over ? 1.f : 0.f, 12.f);
    text_tracked(x, y, px(10, s), 600, mixc(0xFF6A6A6A, UI_TEXT, hv), label, 1.8f * s);
    ui_rect(x, y + 17 * s, lw * hv, 1, HOME_RED);
    return over && ui_input().clicked;
}

// cinematic vignette: the four edges fall to black, the centre stays
void vignette(float sw, float sh, float s) {
    const int n = 18;
    const float ew = 220 * s, eh = 150 * s;
    for (int i = 0; i < n; ++i) {
        const float f0 = (float)i / n, f1 = (float)(i + 1) / n;
        const float a = (1 - f0) * (1 - f0) * 0.55f;
        const DWORD c = with_alpha(0xFF000000, a);
        ui_rect(ew * f0, 0, ew * (f1 - f0) + 1, sh, c);
        ui_rect(sw - ew * f1, 0, ew * (f1 - f0) + 1, sh, c);
        ui_rect(0, eh * f0, sw, eh * (f1 - f0) + 1, c);
        ui_rect(0, sh - eh * f1, sw, eh * (f1 - f0) + 1, c);
    }
}

bool  g_confirm_quit = false;
float g_sweep[3] = { -1, -1, -1 };

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
    const float ot = ui_smooth(7000, 1.0f, 3.2f);
    const float tsec = nowt / 1000.0f;
    const float M = 64 * s;                     // one margin for everything

    // parallax: the cursor's offset from the centre moves the layers by depth
    const UiInput& in = ui_input();
    const float pxn = sw > 0 ? (in.mx / sw - 0.5f) : 0.f, pyn = sh > 0 ? (in.my / sh - 0.5f) : 0.f;
    const float par_x = ui_smooth(7010, pxn, 4.0f), par_y = ui_smooth(7011, pyn, 4.0f);

    // ---- ground: flat black, then the light and the figure
    ui_alpha(1.0f);
    ui_rect(0, 0, sw, sh, UI_BG);

    // ---- the cards' column on the right decides how much room the hero has
    float cw = 280 * s, gap = 28 * s;
    const float room = sw - 2 * M - 440 * s;              // hero needs ~440 px at 1080p
    if (cw * 3 + gap * 2 > room) cw = (room - gap * 2) / 3;
    if (cw < 200 * s) cw = 200 * s;
    const float ch = cw * (500.0f / 280.0f);
    const float cards_w = cw * 3 + gap * 2;
    const float cx0 = sw - M - cards_w - par_x * 14 * s;
    const float cy  = (sh - ch) / 2 + 24 * s - par_y * 8 * s;

    // ---- hero: the game's own player model, life-size, on a slow turntable
    {
        const float hx = M + (cx0 - M) / 2 + par_x * 26 * s;
        const float hh = sh * 0.66f;
        const float gy = sh * 0.86f + par_y * 10 * s;
        // a ghost of the mark behind it, huge and almost invisible - depth
        ui_alpha(ot * 0.04f);
        const int gpx = (int)(sh * 0.50f);
        ui_text(hx - ui_text_width(gpx, 600, "1.6X") / 2 + par_x * 40 * s, sh * 0.24f, gpx, 600, UI_TEXT, "1.6X");
        // a horizon rule and a faint floor glow under the figure
        ui_alpha(ot);
        ui_rect(M, gy + 1, cx0 - M - 24 * s, 1, 0x14FFFFFF);
        const float fade = ot * ot;
        player_hero_draw(hx, gy, hh, 0.35f + tsec * 0.28f, tsec, fade);
    }

    vignette(sw, sh, s);

    // ---- top bar: wordmark left, player right, a rule
    ui_alpha(ot);
    {
        const float y = 40 * s;
        ui_text(M, y, px(22, s), 600, UI_TEXT, "CALL OF DUTY");
        const float tw = ui_text_width(px(22, s), 600, "CALL OF DUTY");
        const float bx = M + tw + 12 * s, bw = ui_text_width(px(16, s), 600, "1.6X") + 18 * s;
        ui_rect_rounded(bx, y + 3 * s, bw, 26 * s, 2 * s, HOME_RED);
        ui_text(bx + 9 * s, y + 4 * s, px(16, s), 600, UI_TEXT, "1.6X");

        char name[64]; plain_name(name, sizeof(name));
        const float wn = ui_text_width(px(14, s), 600, name);
        const float wp = tracked_width(px(10, s), 600, "PLAYER", 2.0f * s);
        text_tracked(sw - M - wn - 14 * s - wp, y + 8 * s, px(10, s), 600, 0xFF565656, "PLAYER", 2.0f * s);
        ui_text(sw - M - wn, y + 4 * s, px(14, s), 600, UI_TEXT, name);
        ui_rect(M, 96 * s, sw - 2 * M, 1, 0x18FFFFFF);
    }

    // ---- news line, under the rule
    {
        char nt[64], nx[128], nu[256];
        if (news_get(nt, sizeof(nt), nx, sizeof(nx), nu, sizeof(nu))) {
            for (char* c = nt; *c; ++c) *c = (char)toupper((unsigned char)*c);
            const float y = 118 * s;
            const float pulse = 0.55f + 0.45f * sinf(nowt * 0.0045f);
            float x = M;
            ui_ellipse(x + 3 * s, y + 7 * s, 3 * s * (1.5f + pulse), 3 * s * (1.5f + pulse), 1.0f,
                       with_alpha(HOME_RED, 0.35f * pulse));
            ui_rect_rounded(x, y + 4 * s, 6 * s, 6 * s, 3 * s, HOME_RED);
            x += 16 * s;
            text_tracked(x, y, px(10, s), 600, UI_ACCENT, nt, 2.4f * s);
            x += tracked_width(px(10, s), 600, nt, 2.4f * s) + 18 * s;
            if (nx[0]) { ui_text(x, y - 2 * s, px(13, s), 400, 0xFF969696, nx); x += ui_text_width(px(13, s), 400, nx) + 18 * s; }
            if (nu[0]) text_tracked(x, y + 1 * s, px(10, s), 600, 0xFF565656, "CTRL + N", 1.8f * s);
        }
    }

    // ---- the cards
    HomeAction result = HomeAction::None;
    char play[MAX_PATH], host[MAX_PATH], sett[MAX_PATH];
    const char* pplay = home_asset("play.jpg", play, sizeof(play)) ? play : nullptr;
    const char* phost = home_asset("host.jpg", host, sizeof(host)) ? host : nullptr;
    const char* psett = home_asset("settings.jpg", sett, sizeof(sett)) ? sett : nullptr;

    if (card(0, cx0, cy, cw, ch, pplay, "JOIN A MATCH", "PLAY", ot, s, &g_sweep[0]) && !g_confirm_quit) {
        // NO "close main" (2026-08-15): forwarded from outside the menu VM it corrupts
        // the UI stack and "open joinserver" silently fails; joinserver is fullscreen
        // and covers main regardless.
        cmdf("close mods_menu\nclose options_multi\nopen joinserver\n");
        logger::logf("home_menu: PLAY -> open joinserver");
        result = HomeAction::Navigated;
    }
    if (card(1, cx0 + cw + gap, cy, cw, ch, phost, "RUN A SERVER", "HOST", ot, s, &g_sweep[1]) && !g_confirm_quit) {
        cmdf("close mods_menu\nclose options_multi\nopen createserver\n");
        logger::logf("home_menu: HOST -> open createserver");
        result = HomeAction::Navigated;
    }
    if (card(2, cx0 + (cw + gap) * 2, cy, cw, ch, psett, "DISPLAY, MOUSE, NETCODE", "SETTINGS", ot, s, &g_sweep[2]) && !g_confirm_quit)
        result = HomeAction::OpenSettings;

    // ---- footer: version + the engine's own screens on the left, shortcuts on the right
    ui_alpha(ot);
    {
        const float fy = sh - 40 * s;
        ui_rect(M, sh - 72 * s, sw - 2 * M, 1, 0x18FFFFFF);
        char ver[48]; snprintf(ver, sizeof(ver), "1.6X  %s", COD1RELOADED_VERSION);
        ui_text(M, fy - 3 * s, px(12, s), 400, 0xFF565656, ver);
        // key bindings live in the vanilla Options, fs_game in Mods: the cards do not
        // cover them, these links do
        float lx = M + ui_text_width(px(12, s), 400, ver) + 40 * s;
        if (link(7400, lx, fy, "GAME OPTIONS", s) && !g_confirm_quit) {
            cmdf("close mods_menu\nopen options_multi\n");
            logger::logf("home_menu: GAME OPTIONS");
            result = HomeAction::Navigated;
        }
        lx += tracked_width(px(10, s), 600, "GAME OPTIONS", 1.8f * s) + 32 * s;
        if (link(7401, lx, fy, "MODS", s) && !g_confirm_quit) {
            cmdf("close options_multi\nopen mods_menu\n");
            logger::logf("home_menu: MODS");
            result = HomeAction::Navigated;
        }
        float fx = sw - M;
        const float w_quit = ui_text_width(px(11, s), 600, "ESC") + 16 * s + 10 * s + tracked_width(px(10, s), 600, "QUIT", 1.8f * s);
        fx -= w_quit;
        const bool over_quit = hit(fx - 8 * s, fy - 12 * s, w_quit + 16 * s, 34 * s);
        keycap(fx, fy - 6 * s, "ESC", "QUIT", s);
        if (over_quit && ui_input().clicked) g_confirm_quit = true;
        const float w_set = ui_text_width(px(11, s), 600, "CTRL+M") + 16 * s + 10 * s + tracked_width(px(10, s), 600, "SETTINGS", 1.8f * s);
        fx -= 32 * s + w_set;
        keycap(fx, fy - 6 * s, "CTRL+M", "SETTINGS", s);
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
        ui_rect_rounded(bx2, by, bw, bh, 4 * s, mixc(0xFFC9342A, HOME_RED, h2));
        text_c(bx2 + bw / 2, by + 11 * s, px(14, s), 600, UI_TEXT, "Quit");
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
