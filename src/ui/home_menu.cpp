// The modern home screen: total visual replacement of ui_mp/main.menu's top level,
// same monochrome language as modern_menu.cpp (fullscreen black, white ink, gliding
// selection). Scoped deliberately to ONE screen - "menu d'accueil", not "tous les
// menus": Join/Start Server forward into the engine's own joinserver/createserver
// screens, Quit opens the engine's own confirmation popup. Reimplementing those
// would be a second project; replacing the first thing a player sees is this one.
//
// EVERY ACTION HERE IS A VERBATIM COPY of the corresponding itemDef's `action` block
// in the original ui_mp/main.menu (2026-08-10), so behaviour does not drift from what
// the engine already does - only the picture changes.
//
// NAVIGATION BUG FIXED 2026-08-10: `setcvar` is a MENU-SCRIPT keyword, valid only
// inside a .menu file's own action{} block (interpreted by the UI VM). Calling
// Cbuf_ExecuteText("setcvar ...") from here goes through the ordinary CONSOLE command
// table instead, where it does not exist - the bridge cvar never actually cleared, so
// the level-triggered home poll in gl_overlay.cpp re-painted home over whatever had
// just opened, one frame later. The console equivalent is `set`. Settings was never
// affected because that path never touches the cvar at all - which is why it alone
// appeared to "work" and every other button looked broken.
//
// RESPONSIVE: every size in this file is derived from ONE scale factor, `s`, computed
// from the backbuffer's HEIGHT against a 1080px reference (home_menu_draw). Height,
// not width: enzo runs 1440x1080 (4:3, stretched) today and may switch to a native
// 16:9/21:9 resolution later - both share the same 1080 height, so a height-driven
// scale keeps type and spacing IDENTICAL between them, while the available WIDTH
// (which differs) only affects how far things can spread out, handled separately by
// capping the nav column against sw rather than scaling it with s.

#include "ui/home_menu.h"
#include "ui/gl_overlay.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA / CODMP_CBUF_EXECTEXT_VA
#include "core/logger.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <windows.h>

namespace patches {

// mixc lives in modern_menu.cpp's anonymous namespace and is not reachable from here;
// a tiny local copy avoids coupling two translation units over one 6-line function.
DWORD mixc_local(DWORD a, DWORD b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    DWORD r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        r |= (DWORD)(ca + (int)((cb - ca) * t)) << sh;
    }
    return r;
}

namespace {

typedef void (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
const Cbuf_ExecuteText_t Cbuf    = (Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA;
const Cvar_FindVar_t     FindVar = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
constexpr int EXEC_APPEND  = 2;
constexpr int CV_INTEGER   = 0x20;

int cv_int(const char* n, int fb) {
    void* c = FindVar(n);
    return c ? *(int*)((char*)c + CV_INTEGER) : fb;
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

// A numbered nav row with its own staggered entrance: row `index` slides and fades in
// slightly after the one before it, so the list reads top-to-bottom instead of
// snapping in all at once. `ot` is the screen's overall entrance progress (0..1).
// `railx` is the shared vertical rail the whole list is threaded on (see draw()) -
// the row draws its own marker on it, turning four separate buttons into one
// composed instrument instead of a stack of cards. `s` is the responsive scale.
bool nav_row(float x, float y, float w, int index, const char* label, const char* caption,
            long key, float ot, float railx, float s, bool danger = false) {
    const float h = 78 * s;
    const float delay = index * 0.10f;
    float rowt = (ot - delay) / (1.0f - delay > 0.05f ? 1.0f - delay : 0.05f);
    rowt = rowt < 0 ? 0 : (rowt > 1 ? 1 : rowt);
    float slide = (1.0f - rowt) * 30.0f * s;
    ui_alpha(rowt);

    // clickable immediately, even mid-animation - the cascade is decoration, not a gate
    bool over = hit(x, y, w, h);
    float hv = ui_smooth(key, over ? 1.0f : 0.0f, 12.0f);
    float dx = x + slide;

    DWORD ink = danger ? UI_DANGER : mixc_local(UI_MUTED, UI_TEXT, hv);
    ui_rect(dx, y + h - 1, w, 1, 0xFF1A1A1A);
    ui_rect(dx, y, (3 + hv * 3) * s, h, danger ? UI_DANGER : UI_ACCENT);
    if (hv > 0.01f)
        ui_rect(dx + 10 * s, y, (w - 10 * s) * hv, h, 0x07FFFFFF);
    // FOCUS RING, not an underline: a thin outline fades in around the whole row on
    // hover - reads as a premium app's focus state instead of a hyperlink. Inset by
    // half its own thickness so the stroke sits fully inside the row's bounds.
    if (hv > 0.02f) {
        float th = 1.0f * s;
        DWORD ring = mixc_local(0x00FFFFFF, danger ? 0x60E05252 : 0x50F2F2F2, hv);
        ui_rect_border(dx + th * 0.5f, y + th * 0.5f, w - th, h - th, 0, th, ring);
    }

    // marker on the shared rail, brightening on hover - the "stepper" tick
    float my = y + h / 2.0f, mk = 3 * s;
    ui_rect(railx - mk, my - mk, mk * 2, mk * 2,
           mixc_local(0xFF2A2A2A, danger ? UI_DANGER : UI_ACCENT, hv));

    char num[4];
    snprintf(num, sizeof(num), "%02d", index + 1);
    ui_text(dx + 32 * s, y + 16 * s, px(13, s), 600, mixc_local(0xFF3A3A3A, UI_MUTED, hv), num);
    ui_text(dx + 66 * s, y + 13 * s, px(30, s), 600, ink, label);
    ui_text(dx + 66 * s, y + 50 * s, px(13, s), 400, danger ? 0xFF9A6060 : UI_MUTED, caption);
    // chevron that slides in on hover
    float cx = dx + w - (34 + 10) * s + hv * 8 * s;
    ui_text(cx, y + 26 * s, px(20, s), 400,
           danger ? UI_DANGER : mixc_local(0x00E8EAED, UI_TEXT, hv), ">");
    return over && ui_input().clicked;
}

// Small bordered key hint, e.g. [ CTRL+M ] label - reads as a real shortcut badge
// instead of a run of dashed plain text.
float keycap(float x, float y, const char* key, const char* label, float s) {
    const float pad = 10 * s, h = 24 * s;
    float kw = ui_text_width(px(12, s), 600, key) + pad * 2;
    ui_rect_rounded(x, y, kw, h, 5 * s, 0xFF161616);
    ui_rect_border(x, y, kw, h, 5 * s, 1.0f * s, 0xFF333333);
    ui_text(x + pad, y + 5 * s, px(12, s), 600, UI_MUTED, key);
    float total = kw;
    if (label && *label) {
        ui_text(x + kw + 10 * s, y + 5 * s, px(13, s), 400, 0xFF5A5A5A, label);
        total += 10 * s + ui_text_width(px(13, s), 400, label);
    }
    return total;
}

// Four L-shaped corner marks, viewfinder-style - the one purely graphic gesture that
// says "tactical/competitive" without spending any of the monochrome rule's one
// allowed color on it.
void corner_frame(float sw, float sh, float alpha, float s) {
    ui_alpha(alpha);
    const float m = 34 * s, a = 36 * s, t = 1.5f * s;
    DWORD c = 0x30FFFFFF;
    ui_rect(m, m, a, t, c);            ui_rect(m, m, t, a, c);
    ui_rect(sw - m - a, m, a, t, c);   ui_rect(sw - m - t, m, t, a, c);
    ui_rect(m, sh - m - t, a, t, c);   ui_rect(m, sh - m - a, t, a, c);
    ui_rect(sw - m - a, sh - m - t, a, t, c); ui_rect(sw - m - t, sh - m - a, t, a, c);
}

}  // namespace

bool home_menu_is_active() { return cv_int("cod1x_home_active", 0) != 0; }

void home_menu_close_via_escape() {
    // verbatim copy of the original "main" menuDef's onESC block
    cmdf("close mods_menu\nclose options_multi\ningameclose main\n"
         "set cod1x_home_active 0\n");
}

HomeAction home_menu_draw(float sw, float sh) {
    // one scale factor, height-driven (see file header) - clamped so an oddly tiny
    // or huge backbuffer cannot make text vanish or overrun the layout
    float s = sh / 1080.0f;
    if (s < 0.65f) s = 0.65f;
    if (s > 1.8f)  s = 1.8f;

    static DWORD s_lastdraw = 0;
    DWORD nowt = GetTickCount();
    if (nowt - s_lastdraw > 250) ui_anim_set(7000, 0.0f);
    s_lastdraw = nowt;
    float ot = ui_smooth(7000, 1.0f, 5.5f);

    // Backing plate: a scene photo, cover-cropped to fill the screen exactly (never
    // letterboxed - see ui_image_cover), with a dark scrim over it so white text
    // stays legible at any brightness the source image happens to have. Falls back
    // to the flat monochrome ground if the file is not there yet - nothing breaks
    // before it is dropped in place, this is not a hard dependency.
    ui_alpha(1.0f);
    char bgpath[MAX_PATH];
    {
        DWORD n = GetModuleFileNameA(NULL, bgpath, MAX_PATH);
        char* sl = n ? strrchr(bgpath, 92) : nullptr;         // 92 = '\\'
        if (sl) { strcpy(sl + 1, "home_bg.jpg"); } else bgpath[0] = 0;
    }
    bool have_bg = bgpath[0] && ui_image_cover(0, 0, sw, sh, bgpath);
    if (!have_bg) ui_rect(0, 0, sw, sh, UI_BG);
    ui_rect(0, 0, sw, sh, have_bg ? 0x8C060606 : 0x00000000);   // scrim, ~55% dark

    // faint background rhythm, echoes the Display-tab preview grid for cross-screen
    // cohesion - barely visible, just enough to keep pure black from feeling flat.
    // Skipped over a real photo: a grid over a scene reads as noise, not texture.
    if (!have_bg) {
        ui_alpha(0.55f);
        float grid = 140 * s;
        for (float gx = fmodf(sw, grid); gx < sw; gx += grid)
            ui_rect(gx, 0, 1, sh, 0xFF101010);
    }

    // A colossal ghost glyph bleeding off the right edge - pure typographic weight,
    // no meaning beyond balance. On a NARROW 4:3 canvas the nav column already uses
    // more of the width (see nw below), so the glyph is scaled down and pushed
    // further off-screen instead of competing with it for space.
    {
        float room = sw > sh * 1.5f ? 1.0f : 0.55f;      // less headroom on 4:3
        int gpx = (int)(sh * 0.85f * room);
        float gw = ui_text_width(gpx, 600, "X");
        ui_alpha(ot * 0.05f);
        ui_text(sw - gw * 0.55f, sh * 0.04f, gpx, 600, UI_TEXT, "X");
    }

    corner_frame(sw, sh, ot * 0.9f, s);

    // system status, top-right: local time + a build/version readout
    SYSTEMTIME st; GetLocalTime(&st);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", st.wHour, st.wMinute);
    ui_alpha(ot);
    float ctw = ui_text_width(px(15, s), 600, clock);
    ui_text(sw - 90 * s - ctw, 46 * s, px(15, s), 600, UI_TEXT, clock);
    ui_rect(sw - 100 * s, 50 * s, 6 * s, 6 * s, UI_ACCENT);
    ui_text(sw - 90 * s, 68 * s, px(12, s), 400, UI_MUTED, "1.6X BUILD  -  READY");

    // Hero lockup, top-left. The numeral IS the identity - no logo asset, just type.
    // No underline: a rule beneath a word reads as a hyperlink, not a wordmark. The
    // corner_frame motif already established for the whole screen scales down instead
    // - two small brackets hugging the numeral, the same idea at a second size, which
    // is what makes it read as ONE system rather than a screen with two different
    // graphic ideas competing.
    float lx = 90 * s, ly = sh * 0.15f + (1.0f - ot) * 20 * s;
    ui_alpha(ot);
    ui_text(lx, ly, px(15, s), 600, UI_MUTED, "CALL OF DUTY  -  COMPETITIVE");
    float hero_px = (float)px(96, s);
    ui_text(lx - 4 * s, ly + 26 * s, (int)hero_px, 600, UI_TEXT, "1.6X");
    {
        float tw = ui_text_width((int)hero_px, 600, "1.6X");
        float bx0 = lx - 10 * s, by0 = ly + 14 * s;
        float bx1 = lx - 4 * s + tw + 8 * s, by1 = ly + 26 * s + hero_px * 0.86f;
        float arm = 16 * s, th = 1.5f * s;
        DWORD c = 0x50FFFFFF;
        ui_rect(bx0, by0, arm, th, c);            ui_rect(bx0, by0, th, arm, c);
        ui_rect(bx1 - arm, by1 - th, arm, th, c); ui_rect(bx1 - th, by1 - arm, th, arm, c);
    }

    bool ingame = cv_int("cl_ingame", 0) != 0;
    float nx = lx, ny = ly + 172 * s;
    // width scales with s but never crowds a narrow 4:3 canvas or overrspreads a
    // wide one - capped against the actual screen, not just the reference height
    float nw = sw * 0.42f;
    if (nw > 620 * s) nw = 620 * s;
    if (nw < 420 * s) nw = 420 * s;
    int   idx = 0;
    const float railx = nx + (32 + 9) * s, row_h = 78 * s, row_pitch = 86 * s;

    // the rail itself, threading all four rows into one instrument before any of
    // them draw their own marker on top of it
    ui_alpha(ot * 0.5f);
    ui_rect(railx - 0.5f, ny + row_h / 2.0f, 1, row_pitch * 3, 0xFF2A2A2A);

    HomeAction result = HomeAction::None;

    if (ingame) {
        if (nav_row(nx, ny, nw, idx++, "RESUME", "Back to your match", 7101, ot, railx, s)) {
            cmdf("ingameclose main\nclose mods_menu\nclose options_multi\nclose main\n"
                 "set cod1x_home_active 0\n");
            logger::logf("home_menu: RESUME clicked");
            result = HomeAction::Navigated;
        }
        ny += row_pitch;
        if (nav_row(nx, ny, nw, idx++, "DISCONNECT", "Leave the current match", 7102, ot,
                   railx, s, true)) {
            cmdf("close mods_menu\nclose options_multi\nexec disconnect\n"
                 "set cod1x_home_active 0\n");
            logger::logf("home_menu: DISCONNECT clicked");
            result = HomeAction::Navigated;
        }
        ny += row_pitch;
    } else {
        if (nav_row(nx, ny, nw, idx++, "JOIN SERVER", "Browse and connect to a match", 7099,
                   ot, railx, s)) {
            // NO "close main" here (2026-08-15): the original vanilla button included
            // it, but forwarded from OUTSIDE the menu-script VM (console Cbuf, not a
            // .menu action{} block) it appears to corrupt the UI stack enough that the
            // following "open joinserver" silently fails and the engine's own
            // always-show-something fallback reopens "main" - looked, from the
            // player's seat, exactly like "Join Server does nothing". joinserver.menu
            // is `fullscreen 1` (confirmed in pak0.pk3), so it fully covers "main"
            // regardless of whether main is formally closed underneath it - dropping
            // the close is visually harmless either way and sidesteps the failure.
            cmdf("close mods_menu\nclose options_multi\nopen joinserver\n"
                 "set cod1x_home_active 0\n");
            logger::logf("home_menu: JOIN SERVER clicked - forwarded open joinserver");
            result = HomeAction::Navigated;
        }
        ny += row_pitch;
        if (nav_row(nx, ny, nw, idx++, "START SERVER", "Host a new match", 7100, ot, railx, s)) {
            cmdf("close mods_menu\nclose options_multi\nopen createserver\n"
                 "set cod1x_home_active 0\n");
            logger::logf("home_menu: START SERVER clicked - forwarded open createserver");
            result = HomeAction::Navigated;
        }
        ny += row_pitch;
    }

    if (nav_row(nx, ny, nw, idx++, "SETTINGS", "Display, mouse, netcode, files", 7103, ot,
               railx, s))
        result = HomeAction::OpenSettings;
    ny += row_pitch;

    if (nav_row(nx, ny, nw, idx++, "QUIT", "Exit to desktop", 7104, ot, railx, s, true)) {
        cmdf("close mods_menu\nclose options_multi\nopen quit_popmenu\n");
        logger::logf("home_menu: QUIT clicked - forwarded open quit_popmenu");
        result = HomeAction::Navigated;   // quit_popmenu is the engine's own confirm dialog
    }

    // footer - real shortcut badges instead of dashed plain text
    ui_alpha(ot);
    float fx = 90 * s, fy = sh - 50 * s;
    fx += keycap(fx, fy, "CTRL+M", "settings", s) + 28 * s;
    fx += keycap(fx, fy, "ESC", "back", s) + 28 * s;
    ui_text(fx, fy + 5 * s, px(13, s), 400, 0xFF444444, "cod1reloaded 1.6X");

    ui_alpha(1.0f);
    return result;
}

}  // namespace patches
