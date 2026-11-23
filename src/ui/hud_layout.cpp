// hud_layout.cpp - see hud_layout.h
#include "ui/hud_layout.h"
#include "ui/gl_overlay.h"
#include "core/logger.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace patches {

namespace {

// ---- engine (CoDMP.exe 1.5, base 0x400000) ----
constexpr uintptr_t VA_CVAR_FINDVAR  = 0x0043b790;   // cvar_t* Cvar_FindVar(name): string +4, integer +0x20
constexpr uintptr_t VA_CVAR_GET      = 0x0043b880;
constexpr uintptr_t VA_CBUF_EXECTEXT = 0x0042a180;
constexpr uintptr_t VA_KEYCATCHERS   = 0x015b8aa4;   // 1 console, 2 UI menu (ui/minimap.cpp)
constexpr uintptr_t VA_SNAP_VALID    = 0x0148bba4;

// ---- cgame_mp_x86.dll (RVAs; RE'd 2026-10-28, the menu code is Q3TA's ui_shared.c) ----
// Menu_New 0x4c970: Menus[menuCount] at 0x5477c0, menuDef_t 0x70c bytes, at most 64,
// menuCount 0xf0f60. windowDef_t (menus and items alike): rect +0, rectClient +0x10, name
// +0x20 (Menus_FindByName 0x43350), border +0x38, borderSize +0x44, flags +0x48 (visible 4).
// menuDef_t: itemCount +0xc0, items[] +0x50c. Menu_UpdatePosition 0x42a60: item rect = menu
// rect.xy (+ borderSize if border) (+ the item's borderSize if border) + rectClient.xy,
// w h = rectClient's, and the item's textRect w h (+0xc0 +0xc4) zeroed to be measured again.
constexpr uintptr_t RVA_MENUS = 0x5477c0, RVA_MENUCOUNT = 0xf0f60;
constexpr int MENU_STRIDE = 0x70c, MENU_MAX = 64, ITEMS_MAX = 128;
constexpr int W_CLIENT = 0x10, W_NAME = 0x20, W_BORDER = 0x38, W_BORDERSIZE = 0x44, W_FLAGS = 0x48;
constexpr int M_ITEMCOUNT = 0xc0, M_ITEMS = 0x50c, I_TEXTRECT_W = 0xc0, I_TEXTRECT_H = 0xc4;
constexpr int WINDOW_VISIBLE = 4;
// CG_DrawUpperRight 0x16150: mov eax,[cg_drawFPS.integer] ; test eax,eax (0x16175) ; je skip.
// CG_DrawLagometer 0x165e0: mov eax,[cg_lagometer.integer] ; test eax,eax (0x165e8) ; je (the
// "connection interrupted" icon only). test -> xor eax,eax skips the game's drawing.
constexpr uintptr_t RVA_FPS_TEST = 0x16175, RVA_LAG_TEST = 0x165e8;
// lagometer_t (Q3): frameSamples[128] +0, frameCount +0x200, snapshotFlags[128] +0x204,
// snapshotSamples[128] +0x404, snapshotCount +0x604. A frame sample is latestSnapshotTime -
// cg.time (0x16430: > 0 interpolating... drawn as the game does: > 0 up in yellow, < 0 down
// in blue, over 1/3 of the height at 1/300 per ms); a snapshot sample = its ping (-1 dropped,
// flag 1 = held back by the rate), 1/900 per ms over the other 2/3.
constexpr uintptr_t RVA_LAGOMETER = 0x1a1d80;
// CG_DrawCompass scales by cg_hudCompassSize (0x25b70): x' = (x + 25) s - 25,
// y' = (y - 345) s + 345 - 160 (s - 1), w' h' = w h s.

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);

void* find_cvar(const char* name) { return ((Cvar_FindVar_t)VA_CVAR_FINDVAR)(name); }
const char* cvar_str(const char* name) {
    void* cv = find_cvar(name);
    const char* s = cv ? *(const char* const*)((const char*)cv + 4) : nullptr;
    return s ? s : "";
}
int cvar_int(const char* name, int fb) {
    void* cv = find_cvar(name);
    return cv ? *(const int*)((const char*)cv + 0x20) : fb;
}
void command(const char* text) { ((Cbuf_ExecuteText_t)VA_CBUF_EXECTEXT)(2, text); }
uintptr_t cgame() { return (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll"); }

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
bool counter(int e) { return e == HUD_FPS || e == HUD_LAG; }   // ours, placed in the room left

// ---- the elements ----
const char* const AMMO_ITEMS[]   = { "ammocounterback", "weaponfiremode", "ammotex", nullptr };
const char* const WEAPON_ITEMS[] = { "weaponnameback", "weapname", nullptr };
struct Def {
    const char* key;          // cod1x_hud_<key>
    const char* label;
    const char* menu;         // a game element: its menu, and its items (null = all)
    const char* const* items;
};
const Def DEFS[HUD_COUNT] = {
    { "compass", "Compass",      "Compass",     nullptr },
    { "stance",  "Stance",       "stance",      nullptr },
    { "health",  "Health bar",   "Health",      nullptr },
    { "ammo",    "Ammo",         "weaponinfo",  AMMO_ITEMS },
    { "weapon",  "Weapon name",  "weaponinfo",  WEAPON_ITEMS },
    { "hints",   "Use hints",    "Cursorhints", nullptr },
    { "weaplist","Weapon list",  nullptr,       nullptr },     // code-drawn: see weapon list below
    { "fps",     "FPS counter",  nullptr,       nullptr },
    { "lag",     "Lagometer",    nullptr,       nullptr },
};
const char* const FPS_STYLES[] = { "Game", "Number", "+ ms", "Detailed" };
const char* const LAG_STYLES[] = { "Game", "1.6X" };

HudState g_st[HUD_COUNT];
std::string g_seen[HUD_COUNT];        // the cvar text last parsed
std::string g_written[HUD_COUNT];     // what we seta'd and the buffer has not run yet
DWORD g_written_at[HUD_COUNT];
bool  g_touched[HUD_COUNT];           // moved or hidden once: its items are ours to put back
bool  g_registered = false;
DWORD g_poll = 0;
bool  g_box_ok[HUD_COUNT];
float g_vbox[HUD_COUNT][4];           // game elements: their items' bounds, 640x480

// ---- the weapon list: CG_DrawWeaponSelect 0x39120 (no argument), called once, by CG_Draw2D
// at 0x19a66; its slot numbers through the helper 0x38fb0. Its pictures go out as
// DrawStretchPic (syscall 0x49: x y w h in pixels, through the wrapper 0x32820), its texts as
// DrawText (0x36: x y in 640x480) - found with cod1x_hud_trace. The cgame calls the engine
// through the pointer at 0x76898 (set at dllEntry). It fades over 1800 ms from
// cg.weaponSelectTime 0x2106d4 (cg.time 0x20af00); cgs.screenXScale / YScale 0x1d5a44 / 48.
constexpr uintptr_t RVA_SYSCALL_PTR = 0x76898, RVA_WS_CALL = 0x19a66, RVA_WS_FUNC = 0x39120;
constexpr uintptr_t RVA_PIC_WRAP = 0x32820;
constexpr uintptr_t RVA_WS_TIME = 0x2106d4, RVA_CG_TIME = 0x20af00, RVA_XSCALE = 0x1d5a44, RVA_YSCALE = 0x1d5a48;
constexpr intptr_t  SYS_DRAWPIC = 0x49, SYS_DRAWTEXT = 0x36;
float g_ws_box[4] = { 400, 2, 232, 72 };   // where it was last drawn (640x480, before our offset)
DWORD g_preview_at = 0;

HudState defaults(int e) {
    HudState s;
    if (e == HUD_FPS) { s.x = 1.0f; s.y = 0.0f; }
    if (e == HUD_LAG) { s.x = 1.0f; s.y = 0.73f; }
    return s;
}

std::string text_of(const HudState& s) {
    char b[64];
    snprintf(b, sizeof(b), "%d %.4g %.4g %d", s.on ? 1 : 0, s.x, s.y, s.style);
    return b;
}
HudState parse(int e, const char* t) {
    HudState s = defaults(e);
    int on = 1, style = 0;
    float x = s.x, y = s.y;
    const int n = sscanf(t, "%d %f %f %d", &on, &x, &y, &style);
    if (n >= 1) s.on = on != 0;
    if (n >= 3) {
        if (!counter(e)) { s.x = clampf(x, -640, 640); s.y = clampf(y, -480, 480); }
        else { s.x = clampf(x, 0, 1); s.y = clampf(y, 0, 1); }
    }
    if (n >= 4) s.style = std::max(0, std::min(style, e == HUD_FPS ? 3 : e == HUD_LAG ? 1 : 0));
    return s;
}

void read_cvars() {
    for (int e = 0; e < HUD_COUNT; ++e) {
        char name[48];
        snprintf(name, sizeof(name), "cod1x_hud_%s", DEFS[e].key);
        const char* t = cvar_str(name);
        if (!g_written[e].empty()) {                      // our own seta, not run yet: keep ours
            if (g_written[e] == t || GetTickCount() - g_written_at[e] > 1000) g_written[e].clear();
            else continue;
        }
        if (g_seen[e] == t) continue;
        g_seen[e] = t;
        g_st[e] = parse(e, t);
    }
}

// ---- the game's HUD menus ----
uint8_t* find_menu(uintptr_t cg, const char* name) {
    const int n = *(volatile int*)(cg + RVA_MENUCOUNT);
    if (n <= 0 || n > MENU_MAX) return nullptr;
    for (int i = 0; i < n; ++i) {
        uint8_t* m = (uint8_t*)(cg + RVA_MENUS + (uintptr_t)i * MENU_STRIDE);
        const char* mn = *(const char* const*)(m + W_NAME);
        if (mn && !_stricmp(mn, name)) return m;
    }
    return nullptr;
}
bool listed(const char* const* items, const char* name) {
    if (!items) return true;
    if (!name) return false;
    for (int i = 0; items[i]; ++i)
        if (!_stricmp(items[i], name)) return true;
    return false;
}

void apply_game(uintptr_t cg) {
    for (int e = 0; e < HUD_COUNT; ++e) {
        g_box_ok[e] = false;
        if (!DEFS[e].menu) continue;
        uint8_t* m = find_menu(cg, DEFS[e].menu);
        if (!m) continue;
        float mx = *(float*)(m + 0), my = *(float*)(m + 4);
        if (*(int*)(m + W_BORDER)) { mx += *(float*)(m + W_BORDERSIZE); my += *(float*)(m + W_BORDERSIZE); }
        const int cnt = std::min(*(int*)(m + M_ITEMCOUNT), ITEMS_MAX);
        const HudState& s = g_st[e];
        // an element never moved nor hidden is left to the game entirely
        if (!s.on || s.x != 0 || s.y != 0) g_touched[e] = true;
        const bool touch = g_touched[e];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int k = 0; k < cnt; ++k) {
            uint8_t* it = *(uint8_t**)(m + M_ITEMS + k * 4);
            if (!it || !listed(DEFS[e].items, *(const char* const*)(it + W_NAME))) continue;
            float ix = mx, iy = my;
            if (*(int*)(it + W_BORDER)) { ix += *(float*)(it + W_BORDERSIZE); iy += *(float*)(it + W_BORDERSIZE); }
            ix += *(float*)(it + W_CLIENT) + s.x;
            iy += *(float*)(it + W_CLIENT + 4) + s.y;
            float* r = (float*)it;
            if (touch) {
                if (r[0] != ix || r[1] != iy) {
                    r[0] = ix; r[1] = iy;
                    *(int*)(it + I_TEXTRECT_W) = 0;       // the text measured again, as the game does
                    *(int*)(it + I_TEXTRECT_H) = 0;
                }
                int& fl = *(int*)(it + W_FLAGS);
                fl = s.on ? (fl | WINDOW_VISIBLE) : (fl & ~WINDOW_VISIBLE);
            }
            x0 = std::min(x0, r[0]); y0 = std::min(y0, r[1]);
            x1 = std::max(x1, r[0] + r[2]); y1 = std::max(y1, r[1] + r[3]);
        }
        if (x1 <= x0 || y1 <= y0) continue;
        if (e == HUD_COMPASS) {                           // drawn scaled by cg_hudCompassSize
            const float sc = hud_compass_size();
            const float w = (x1 - x0) * sc, h = (y1 - y0) * sc;
            x0 = (x0 + 25) * sc - 25;
            y0 = (y0 - 345) * sc + 345 - 160 * (sc - 1);
            x1 = x0 + w; y1 = y0 + h;
        }
        g_vbox[e][0] = x0; g_vbox[e][1] = y0; g_vbox[e][2] = x1 - x0; g_vbox[e][3] = y1 - y0;
        g_box_ok[e] = true;
    }
}

// the game's fps counter / lagometer skipped while ours stand in (or the element is off)
void patch_test(uintptr_t at, bool skip) {
    uint8_t* p = (uint8_t*)at;
    const uint8_t want = skip ? 0x33 : 0x85;
    if (p[1] != 0xc0 || (p[0] != 0x85 && p[0] != 0x33) || p[0] == want) return;
    DWORD old = 0;
    if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = want;
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
}

// ---- the fps clock (every SwapBuffers) ----
constexpr int RING = 512;
double g_dt[RING];                    // seconds per frame
int    g_dt_n = 0;
LARGE_INTEGER g_qpf, g_last;
struct FpsShown { int fps = 0, lo = 0, hi = 0; float ms = 0; };
FpsShown g_fps;
DWORD g_fps_at = 0;

void fps_tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_last.QuadPart) {
        const double dt = (double)(now.QuadPart - g_last.QuadPart) / (double)g_qpf.QuadPart;
        if (dt > 0 && dt < 1.0) g_dt[g_dt_n++ % RING] = dt;
    }
    g_last = now;
    const DWORD t = GetTickCount();
    if (t - g_fps_at < 250) return;                  // steady digits: 4 updates a second
    g_fps_at = t;
    double sum = 0, lo = 1e9, hi = 0;
    int n = 0;
    for (int i = 0; i < std::min(g_dt_n, RING) && sum < 0.5; ++i) {
        const double dt = g_dt[(g_dt_n - 1 - i) % RING];
        sum += dt; ++n;
        lo = std::min(lo, dt); hi = std::max(hi, dt);
    }
    if (n == 0 || sum <= 0) return;
    g_fps.fps = (int)(n / sum + 0.5);
    g_fps.ms = (float)(sum / n * 1000.0);
    g_fps.lo = (int)(1.0 / hi + 0.5);
    g_fps.hi = (int)(1.0 / lo + 0.5);
}

bool in_game() { return cgame() && *(volatile int*)VA_SNAP_VALID; }
bool hidden() {
    if (*(volatile int*)VA_KEYCATCHERS & 3) return true;
    return cvar_int("cg_draw2D", 1) == 0;
}

// ---- the 1.6X counters ----
float scale(float sh) { return std::max(0.5f, sh / 1080.0f); }
float margin(float sh) { return roundf(8 * scale(sh)); }   // the counters keep off the very edge

void fps_lines(int style, char* a, char* b, size_t n) {
    b[0] = 0;
    if (style == 1) snprintf(a, n, "%d", g_fps.fps);
    else if (style == 2) snprintf(a, n, "%d FPS  %.1f ms", g_fps.fps, g_fps.ms);
    else { snprintf(a, n, "%d FPS", g_fps.fps); snprintf(b, n, "%.1f ms  %d - %d", g_fps.ms, g_fps.lo, g_fps.hi); }
}

// the counter's box: wide enough for its widest digits, so it does not shake
void fps_size(int style, float S, float* w, float* h) {
    const int px = (int)roundf((style == 1 ? 22 : 15) * S);
    const char* tmpl = style == 1 ? "888" : style == 2 ? "888 FPS  88.8 ms" : "88.8 ms  888 - 888";
    *w = roundf(ui_text_font_width(px, 700, tmpl, UI_FONT_SANS) + (style == 1 ? 34 : 18) * S);
    *h = roundf(ui_text_font_height(px, 700, UI_FONT_SANS) * (style == 3 ? 1.9f : 1.0f) + 10 * S);
}

void lag_size(float S, float* w, float* h) { *w = roundf(140 * S); *h = roundf(72 * S); }

void box_of(int e, float sw, float sh, const HudState& s, float* x, float* y, float* w, float* h) {
    const float S = scale(sh);
    if (e == HUD_FPS) fps_size(std::max(1, s.style), S, w, h);
    else lag_size(S, w, h);
    const float m = margin(sh);
    *x = roundf(m + s.x * std::max(0.0f, sw - *w - 2 * m));
    *y = roundf(m + s.y * std::max(0.0f, sh - *h - 2 * m));
}

void draw_fps(float x, float y, float w, float h, int style, float S, bool right) {
    char a[48], b[48];
    fps_lines(style, a, b, sizeof(a));
    ui_aa_rect(x, y, w, h, 6 * S, 0x8C0B0D10);
    const int px = (int)roundf((style == 1 ? 22 : 15) * S);
    const float lh = ui_text_font_height(px, 700, UI_FONT_SANS);
    const float pad = 9 * S;
    if (style == 1) {
        const int sp = (int)roundf(11 * S);
        const float tw = ui_text_font_width(px, 700, a, UI_FONT_SANS);
        const float uw = ui_text_font_width(sp, 700, "FPS", UI_FONT_SANS);
        const float tx = right ? x + w - pad - uw - 4 * S - tw : x + pad;
        const float ty = roundf(y + (h - lh) / 2);
        ui_text_font(roundf(tx), ty, px, 700, 0xFFFFFFFF, a, UI_FONT_SANS);
        ui_text_font(roundf(tx + tw + 4 * S), roundf(ty + lh - ui_text_font_height(sp, 700, UI_FONT_SANS) - 2 * S),
                     sp, 700, 0xB3FFFFFF, "FPS", UI_FONT_SANS);
        return;
    }
    const float ty = roundf(y + 5 * S);
    const float aw = ui_text_font_width(px, 700, a, UI_FONT_SANS);
    ui_text_font(roundf(right ? x + w - pad - aw : x + pad), ty, px, 700, 0xFFFFFFFF, a, UI_FONT_SANS);
    if (b[0]) {
        const int bp = (int)roundf(12 * S);
        const float bw = ui_text_font_width(bp, 600, b, UI_FONT_SANS);
        ui_text_font(roundf(right ? x + w - pad - bw : x + pad), roundf(ty + lh * 0.95f), bp, 600, 0xB3FFFFFF, b, UI_FONT_SANS);
    }
}

void draw_lag(float x, float y, float w, float h, float S) {
    ui_aa_rect(x, y, w, h, 6 * S, 0x8C0B0D10);
    const uintptr_t cg = cgame();
    const float gx = x + 6 * S, gy = y + 6 * S, gw = w - 12 * S, gh = h - 12 * S;
    const float top = gh / 3, mid = gy + top, bot = gh - top;
    ui_rect(gx, roundf(mid), gw, 1, 0x26FFFFFF);
    if (!cg) return;
    const int* lag = (const int*)(cg + RVA_LAGOMETER);
    const int fcount = lag[0x200 / 4], scount = lag[0x604 / 4];
    const int* frames = lag;
    const int* sflags = lag + 0x204 / 4;
    const int* snaps = lag + 0x404 / 4;
    const int n = 128;
    const float bw = gw / n;
    for (int a = 0; a < n; ++a) {                     // newest on the right
        const float bx = gx + gw - (a + 1) * bw;
        float v = frames[(fcount - 1 - a) & 127] * (top / 300.0f);
        if (v > 0) { v = std::min(v, top); ui_rect(bx, mid - v, bw, v, 0xFFFFC94D); }
        else if (v < 0) { v = std::min(-v, top); ui_rect(bx, mid, bw, std::min(v, top), 0xFF4DA3FF); }
        const int i = (scount - 1 - a) & 127;
        const int p = snaps[i];
        if (p > 0) {
            const float pv = std::min(p * (bot / 900.0f), bot);
            ui_rect(bx, gy + gh - pv, bw, pv, (sflags[i] & 1) ? 0xFFFFC94D : 0xFF56D483);
        } else if (p < 0) {
            ui_rect(bx, gy + gh - bot, bw, bot, 0xFFFF5A5A);
        }
    }
    const int last = snaps[(scount - 1) & 127];
    char t[24];
    if (last >= 0) snprintf(t, sizeof(t), "%d ms", last); else snprintf(t, sizeof(t), "drop");
    const int tp = (int)roundf(11 * S);
    const float tw = ui_text_font_width(tp, 700, t, UI_FONT_SANS);
    ui_text_font(roundf(x + w - 7 * S - tw), roundf(y + 4 * S), tp, 700, 0xD9FFFFFF, t, UI_FONT_SANS);
}

}  // namespace

const char* hud_elem_label(int e) { return e >= 0 && e < HUD_COUNT ? DEFS[e].label : ""; }
bool hud_elem_game(int e) { return e >= 0 && e < HUD_COUNT && !counter(e); }
int hud_elem_styles(int e, const char* const** names) {
    if (e == HUD_FPS) { *names = FPS_STYLES; return 4; }
    if (e == HUD_LAG) { *names = LAG_STYLES; return 2; }
    *names = nullptr;
    return 0;
}

HudState hud_elem_default(int e) { return defaults(e); }

HudState hud_elem_get(int e) {
    if (e < 0 || e >= HUD_COUNT) return HudState();
    HudState s = g_st[e];
    if ((e == HUD_FPS || e == HUD_LAG) && s.style == 0)        // the game's: its own cvar says
        s.on = cvar_int(e == HUD_FPS ? "cg_drawFPS" : "cg_lagometer", 0) != 0;
    return s;
}

void hud_elem_set(int e, const HudState& s) {
    if (e < 0 || e >= HUD_COUNT) return;
    HudState n = s;
    if ((e == HUD_FPS || e == HUD_LAG) && n.style == 0) {
        const char* cv = e == HUD_FPS ? "cg_drawFPS" : "cg_lagometer";
        if ((cvar_int(cv, 0) != 0) != (n.on != 0)) {
            char b[64];
            snprintf(b, sizeof(b), "seta %s %d\n", cv, n.on ? 1 : 0);
            command(b);
        }
        n.on = 1;                                     // ours keeps "shown" for when 1.6X is picked
    }
    const std::string t = text_of(n);
    if (t == text_of(g_st[e])) { g_st[e] = n; return; }
    g_st[e] = n;
    char b[96];
    snprintf(b, sizeof(b), "seta cod1x_hud_%s \"%s\"\n", DEFS[e].key, t.c_str());
    command(b);
    g_seen[e] = t;
    g_written[e] = t;                                 // the cvar catches up when the buffer runs
    g_written_at[e] = GetTickCount();
}

bool hud_elem_box(int e, float sw, float sh, float* x, float* y, float* w, float* h) {
    if (e < 0 || e >= HUD_COUNT) return false;
    if (DEFS[e].menu) {
        if (!g_box_ok[e]) return false;
        const float kx = sw / 640.0f, ky = sh / 480.0f;
        *x = g_vbox[e][0] * kx; *y = g_vbox[e][1] * ky; *w = g_vbox[e][2] * kx; *h = g_vbox[e][3] * ky;
        return true;
    }
    if (e == HUD_WEAPLIST) {
        if (!in_game()) return false;
        const float kx = sw / 640.0f, ky = sh / 480.0f;
        const HudState& s = g_st[e];
        *x = (g_ws_box[0] + s.x) * kx; *y = (g_ws_box[1] + s.y) * ky; *w = g_ws_box[2] * kx; *h = g_ws_box[3] * ky;
        return true;
    }
    if (g_st[e].style == 0) return false;            // the game's own: where the game puts it
    box_of(e, sw, sh, g_st[e], x, y, w, h);
    return true;
}

HudState hud_elem_moved(int e, float sw, float sh, float x, float y) {
    HudState s = g_st[e];
    if (!counter(e)) {
        float bx, by, bw, bh;
        if (!hud_elem_box(e, sw, sh, &bx, &by, &bw, &bh)) return s;
        float sc = e == HUD_COMPASS ? hud_compass_size() : 1.0f;
        if (sc < 0.05f) sc = 1.0f;
        s.x += (x - bx) * 640.0f / sw / sc;
        s.y += (y - by) * 480.0f / sh / sc;
        s.x = roundf(s.x * 4) / 4; s.y = roundf(s.y * 4) / 4;    // quarter units: tidy cvars
        return s;
    }
    float bx, by, bw, bh;
    box_of(e, sw, sh, s, &bx, &by, &bw, &bh);
    const float m = margin(sh);
    s.x = clampf((x - m) / std::max(1.0f, sw - bw - 2 * m), 0, 1);
    s.y = clampf((y - m) / std::max(1.0f, sh - bh - 2 * m), 0, 1);
    return s;
}

float hud_compass_size() {
    void* cv = find_cvar("cg_hudCompassSize");
    const char* t = cv ? *(const char* const*)((const char*)cv + 4) : nullptr;
    const float v = t && t[0] ? (float)atof(t) : 1.0f;
    return v > 0.05f ? v : 1.0f;
}
void hud_compass_size_set(float v) {
    char b[64];
    snprintf(b, sizeof(b), "seta cg_hudCompassSize %.3f\n", clampf(v, 0.5f, 1.6f));
    command(b);
}

void weaplist_frame(uintptr_t cg);        // below

void hud_layout_frame() {
    if (!g_registered) {
        g_registered = true;
        for (int e = 0; e < HUD_COUNT; ++e) {
            char name[48];
            snprintf(name, sizeof(name), "cod1x_hud_%s", DEFS[e].key);
            ((Cvar_Get_t)VA_CVAR_GET)(name, text_of(defaults(e)).c_str(), 1);   // CVAR_ARCHIVE
        }
        read_cvars();
    }
    fps_tick();
    const DWORD now = GetTickCount();
    if (now - g_poll >= 100) { g_poll = now; read_cvars(); }
    const uintptr_t cg = cgame();
    if (!cg) { for (bool& b : g_box_ok) b = false; return; }
    apply_game(cg);
    weaplist_frame(cg);
    patch_test(cg + RVA_FPS_TEST, g_st[HUD_FPS].style != 0);
    patch_test(cg + RVA_LAG_TEST, g_st[HUD_LAG].style != 0);
}

bool hud_layout_active() {
    if (!in_game() || hidden()) return false;
    return (g_st[HUD_FPS].style && g_st[HUD_FPS].on) || (g_st[HUD_LAG].style && g_st[HUD_LAG].on);
}

void hud_layout_draw_elem(int e, float sw, float sh) {
    if (e != HUD_FPS && e != HUD_LAG) return;
    const HudState& s = g_st[e];
    if (!s.style) return;
    float x, y, w, h;
    box_of(e, sw, sh, s, &x, &y, &w, &h);
    const float S = scale(sh);
    if (e == HUD_FPS) draw_fps(x, y, w, h, s.style, S, s.x > 0.5f);
    else draw_lag(x, y, w, h, S);
}

// ---- the weapon list: CG_DrawWeaponSelect routed through hud_weaplist_draw, its draws
// shifted (or dropped) by hud_syscall on their way to the engine. With cod1x_hud_trace 1
// (dev) every new caller of DrawStretchPic / DrawText is logged once with its place: how
// the weapon list was found.
typedef intptr_t (__cdecl* Syscall_t)(intptr_t, ...);
typedef void (__cdecl* Void_t)();
Syscall_t g_sys_orig = nullptr;
Void_t    g_ws_orig = nullptr;
bool      g_in_ws = false, g_ws_drew = false;
float     g_ws_lo[2], g_ws_hi[2];
uintptr_t g_trace_seen[256];
int       g_trace_n = 0;
bool      g_trace = false;

extern "C" intptr_t __cdecl hud_syscall(intptr_t id, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
                                        intptr_t a5, intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9,
                                        intptr_t a10, intptr_t a11, intptr_t a12, intptr_t a13, intptr_t a14) {
    if (g_in_ws && (id == SYS_DRAWPIC || id == SYS_DRAWTEXT)) {
        const HudState& s = g_st[HUD_WEAPLIST];
        if (!s.on) return 0;
        const uintptr_t cg = cgame();
        float x, y, vx, vy, vw, vh;
        memcpy(&x, &a1, 4); memcpy(&y, &a2, 4);
        if (id == SYS_DRAWPIC) {                         // pixels
            const float kx = std::max(0.01f, *(const float*)(cg + RVA_XSCALE));
            const float ky = std::max(0.01f, *(const float*)(cg + RVA_YSCALE));
            float w, h;
            memcpy(&w, &a3, 4); memcpy(&h, &a4, 4);
            vx = x / kx; vy = y / ky; vw = w / kx; vh = h / ky;
            x += s.x * kx; y += s.y * ky;
        } else {                                          // 640x480, on its baseline
            vx = x; vy = y - 12; vw = 12; vh = 14;
            x += s.x; y += s.y;
        }
        if (!g_ws_drew) { g_ws_lo[0] = vx; g_ws_lo[1] = vy; g_ws_hi[0] = vx + vw; g_ws_hi[1] = vy + vh; }
        g_ws_lo[0] = std::min(g_ws_lo[0], vx); g_ws_lo[1] = std::min(g_ws_lo[1], vy);
        g_ws_hi[0] = std::max(g_ws_hi[0], vx + vw); g_ws_hi[1] = std::max(g_ws_hi[1], vy + vh);
        g_ws_drew = true;
        memcpy(&a1, &x, 4); memcpy(&a2, &y, 4);
    }
    if (g_trace && (id == SYS_DRAWPIC || id == SYS_DRAWTEXT) && g_trace_n < 256) {
        const uintptr_t cg = cgame();
        uintptr_t ret = (uintptr_t)__builtin_return_address(0);
        if (id == SYS_DRAWPIC && ret - cg >= RVA_PIC_WRAP && ret - cg < RVA_PIC_WRAP + 0x79)
            ret = *(const uintptr_t*)((uintptr_t)&id + 40);   // the wrapper pushed id + 9: its caller's return
        bool seen = false;
        for (int i = 0; i < g_trace_n; ++i) seen |= g_trace_seen[i] == ret;
        if (!seen) {
            g_trace_seen[g_trace_n++] = ret;
            float f[4];
            const intptr_t raw[4] = { a1, a2, a3, a4 };
            for (int i = 0; i < 4; ++i) memcpy(&f[i], &raw[i], 4);
            logger::logf("hud_trace: %s from cgame+%05x  %.1f %.1f %.1f %.1f", id == SYS_DRAWPIC ? "pic " : "text",
                         (unsigned)(ret - cg), f[0], f[1], f[2], f[3]);
        }
    }
    return g_sys_orig(id, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

extern "C" void __cdecl hud_weaplist_draw() {
    g_in_ws = true;
    g_ws_drew = false;
    g_ws_orig();
    g_in_ws = false;
    if (g_ws_drew) {
        g_ws_box[0] = g_ws_lo[0]; g_ws_box[1] = g_ws_lo[1];
        g_ws_box[2] = g_ws_hi[0] - g_ws_lo[0]; g_ws_box[3] = g_ws_hi[1] - g_ws_lo[1];
    }
}

void weaplist_frame(uintptr_t cg) {
    static void* s_trace = nullptr;
    if (!s_trace) s_trace = ((Cvar_Get_t)VA_CVAR_GET)("cod1x_hud_trace", "0", 0);
    g_trace = s_trace && *(const int*)((const char*)s_trace + 0x20) != 0;
    const HudState& s = g_st[HUD_WEAPLIST];
    if (!s.on || s.x != 0 || s.y != 0) g_touched[HUD_WEAPLIST] = true;
    const bool preview = GetTickCount() - g_preview_at < 250;
    if (!g_touched[HUD_WEAPLIST] && !preview && !g_trace) return;

    Syscall_t* slot = (Syscall_t*)(cg + RVA_SYSCALL_PTR);
    if (*slot && *slot != (Syscall_t)hud_syscall) {      // set by the engine at the cgame's dllEntry
        g_sys_orig = *slot;
        *slot = (Syscall_t)hud_syscall;
        logger::logf("hud_layout: cgame syscalls through us (engine %p)", (void*)g_sys_orig);
    }
    uint8_t* site = (uint8_t*)(cg + RVA_WS_CALL);
    if (site[0] == 0xE8 && g_sys_orig) {
        int32_t rel;
        memcpy(&rel, site + 1, 4);
        const uintptr_t to = cg + RVA_WS_CALL + 5 + rel;
        if (to == cg + RVA_WS_FUNC) {
            g_ws_orig = (Void_t)to;
            const int32_t nrel = (int32_t)((uintptr_t)&hud_weaplist_draw - (cg + RVA_WS_CALL + 5));
            DWORD old = 0;
            if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(site + 1, &nrel, 4);
                VirtualProtect(site, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), site, 5);
                logger::logf("hud_layout: weapon list drawn through us");
            }
        }
    }
    if (preview && in_game()) *(int*)(cg + RVA_WS_TIME) = *(const int*)(cg + RVA_CG_TIME);   // kept on screen
}

void hud_layout_preview() { g_preview_at = GetTickCount(); }

void hud_layout_log() {
    const uintptr_t cg = cgame();
    if (!cg) { logger::logf("hud_layout: no cgame"); return; }
    const int n = *(volatile int*)(cg + RVA_MENUCOUNT);
    logger::logf("hud_layout: %d menus", n);
    for (int i = 0; i < n && i < MENU_MAX; ++i) {
        const uint8_t* m = (const uint8_t*)(cg + RVA_MENUS + (uintptr_t)i * MENU_STRIDE);
        const float* r = (const float*)m;
        const char* mn = *(const char* const*)(m + W_NAME);
        const int cnt = *(const int*)(m + M_ITEMCOUNT);
        logger::logf("  menu %d \"%s\" rect %.1f %.1f %.1f %.1f flags %x items %d", i, mn ? mn : "?", r[0], r[1], r[2], r[3],
                     *(const int*)(m + W_FLAGS), cnt);
        for (int k = 0; k < cnt && k < ITEMS_MAX; ++k) {
            const uint8_t* it = *(const uint8_t* const*)(m + M_ITEMS + k * 4);
            if (!it) continue;
            const float* a = (const float*)it;
            const float* c = (const float*)(it + W_CLIENT);
            const char* in = *(const char* const*)(it + W_NAME);
            logger::logf("    item \"%s\" rect %.1f %.1f %.1f %.1f client %.1f %.1f flags %x", in ? in : "?", a[0], a[1],
                         a[2], a[3], c[0], c[1], *(const int*)(it + W_FLAGS));
        }
    }
}

void hud_layout_draw(float sw, float sh) {
    for (int e : { HUD_FPS, HUD_LAG })
        if (g_st[e].style && g_st[e].on) hud_layout_draw_elem(e, sw, sh);
}

}  // namespace patches
