// The 1.6X modern settings panel (English). gl_overlay owns GL + input; this file is
// layout and settings logic only.
//
// DESIGN (2026-08-21 restyle, validated direction: FLAT engine-drawn, monochrome,
// no textures, Segoe UI): black ground, hairline rules, one accent (white pill),
// red reserved for danger. Every control carries a one-line hint - the menu should
// TEACH what a setting does, not just expose it. The display preview is a plain
// viewfinder frame ("cadran", corner brackets), not a fake monitor - the old
// bezel-and-stand TV drawing is gone by request.
//
// THE PREVIEW is the point of the rework: it shows what a display mode will really
// do BEFORE applying - the REAL player model (player_preview.cpp) stretched by the
// exact factor GPU scaling applies, or black bars at true proportions. Born from a
// live support session (2026-08-10) where 1440x1080-in-borderless surprised even
// the mod's own admin.
//
// Everything applies through the same engine paths as the old menu bridge: seta +
// vid_restart via Cbuf_ExecuteText, cod1reloaded.ini via settings_menu's writeback,
// and the cod1x_* bridge cvars the old menu already bound (widescreen mode).
// Every cvar named here was grep-verified against CoDMP.exe / cgame_mp_x86.dll.

#include "ui/modern_menu.h"
#include "ui/gl_overlay.h"
#include "ui/player_preview.h"
#include "core/logger.h"
#include "features/settings_menu.h"
#include "features/pam_install.h"
#include "video/display_probe.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

namespace patches {

void menu_ini_writeback(const char* key, const char* value);

namespace {

typedef void (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
const Cbuf_ExecuteText_t Cbuf = (Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA;
const Cvar_FindVar_t     FindVar = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
constexpr int EXEC_APPEND = 2;
// cvar_t layout (same RE as settings_menu.cpp)
constexpr int CV_STRING = 0x04, CV_INTEGER = 0x20;

int cv_int(const char* n, int fb) {
    void* c = FindVar(n);
    return c ? *(int*)((char*)c + CV_INTEGER) : fb;
}
const char* cv_str(const char* n, const char* fb) {
    void* c = FindVar(n);
    return c ? *(const char**)((char*)c + CV_STRING) : fb;
}
void cmdf(const char* fmt, ...) {
    char b[256]; va_list a; va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    Cbuf(EXEC_APPEND, b);
}

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
long wkey(float x, float y, int tag) { return ((long)x << 14) ^ (long)y ^ (tag << 27); }

constexpr DWORD UI_LINE = 0xFF232323;    // hairline
constexpr DWORD UI_HINT = 0xFF5E5E5E;    // explanatory text, darker than MUTED
constexpr DWORD UI_EYEB = 0xFF6B7280;    // section eyebrows

// ------------------------------------------------------------------ state
struct Mode { int w, h; };
std::vector<Mode> g_modes;
std::vector<int>  g_hz;              // rates for the selected mode; [0] slot = auto
int  g_desk_w = 1920, g_desk_h = 1080;
int  g_sel = -1, g_hz_sel = 0;       // g_hz_sel 0 = Auto
int  g_display_mode = 0;             // 0 fullscreen (stretch), 1 borderless
int  g_view_mode = 0;                // 0 classic, 1 widescreen, 2 stretched (= cod1x_viewmode)
int  g_fov = 80;
int  g_tab = 0;
bool g_dirty = false, g_init = false;
std::vector<std::string> g_cfgs;
struct DemoEnt { std::string name, dir; };     // dir = search-path folder (Main, mod)
std::vector<DemoEnt> g_demo_ents;
float g_scroll_c = 0, g_scroll_d = 0;
DWORD g_files_scan = 0;

bool game_root(char* out, size_t n) {
    DWORD r = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!r) return false;
    char* sl = strrchr(out, 92); if (!sl) return false; sl[1] = 0;
    return true;
}

void scan_dir(const char* sub, const char* pat, std::vector<std::string>& out) {
    out.clear();
    char base[MAX_PATH];
    if (!game_root(base, sizeof(base))) return;
    char q[MAX_PATH];
    snprintf(q, sizeof(q), "%sMain/%s/%s", base, sub, pat);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(q, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName); }
    while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
}

// demos live in <searchpath>/demos of EVERY mod dir, not just Main - rPAM records
// into __rPAMv115b5/demos (found 2026-08-21: the old Main-only *.dm_1 scan showed
// an empty list while 25 rPAM demos sat next door, in .dm_2 no less)
// scan <root>/<mod>/demos/*.dm_* for every top-level folder, adding to g_demo_ents
// (deduped by dir+name, so the same demo present in two roots is listed once).
void scan_demos_root(const char* root) {
    if (!root || !root[0]) return;
    char base[MAX_PATH];
    int n = snprintf(base, sizeof(base), "%s", root);
    if (n <= 0 || n >= (int)sizeof(base)) return;
    if (base[n - 1] != '\\' && base[n - 1] != '/') { base[n] = '\\'; base[n + 1] = 0; }
    char q[MAX_PATH];
    snprintf(q, sizeof(q), "%s*", base);
    WIN32_FIND_DATAA dd; HANDLE hd = FindFirstFileA(q, &dd);
    if (hd == INVALID_HANDLE_VALUE) return;
    do {
        if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (dd.cFileName[0] == '.') continue;
        snprintf(q, sizeof(q), "%s%s/demos/*.dm_*", base, dd.cFileName);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(q, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            bool dup = false;
            for (const DemoEnt& e : g_demo_ents)
                if (_stricmp(e.name.c_str(), fd.cFileName) == 0 && _stricmp(e.dir.c_str(), dd.cFileName) == 0) { dup = true; break; }
            if (!dup) g_demo_ents.push_back({ fd.cFileName, dd.cFileName });
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    } while (FindNextFileA(hd, &dd));
    FindClose(hd);
}

void scan_demos() {
    g_demo_ents.clear();
    // the exe's own directory (= fs_basepath in the normal case)
    char base[MAX_PATH];
    if (game_root(base, sizeof(base))) scan_demos_root(base);
    // and the engine's real read/write roots - CoD records demos into fs_homepath,
    // which is NOT always the install dir (a mod / server demo can land there instead,
    // invisible to a basepath-only scan). Scan both; scan_demos_root dedups.
    scan_demos_root(cv_str("fs_homepath", ""));
    scan_demos_root(cv_str("fs_basepath", ""));
    // Main first, then mods; newest (highest demo number) first within each dir
    std::sort(g_demo_ents.begin(), g_demo_ents.end(),
              [](const DemoEnt& a, const DemoEnt& b) {
                  int md = _stricmp(a.dir.c_str(), "Main") == 0 ? 0 : 1;
                  int nd = _stricmp(b.dir.c_str(), "Main") == 0 ? 0 : 1;
                  if (md != nd) return md < nd;
                  if (a.dir != b.dir) return a.dir < b.dir;
                  return a.name > b.name;
              });
}

// ------------------------------------------------------------- key bindings
// Action table: every command grep-verified against CoDMP.exe / the game's own
// developer configs in pak0 (ned_mp.cfg: MOUSE2 = "+speed" IS aim-down-sight in
// CoD1 - there is no +toggleads in this engine).
struct BindAct { const char* cmd; const char* label; };
const BindAct k_move[] = {
    { "+forward",           "Move forward" },
    { "+back",              "Move back" },
    { "+moveleft",          "Strafe left" },
    { "+moveright",         "Strafe right" },
    { "+gostand",           "Jump / stand" },
    { "gocrouch",           "Crouch" },
    { "goprone",            "Prone" },
    { "+leanleft",          "Lean left" },
    { "+leanright",         "Lean right" },
};
const BindAct k_combat[] = {
    { "+attack",            "Fire" },
    { "+speed",             "Aim down sight (hold)" },
    { "+reload",            "Reload" },
    { "+melee",             "Melee" },
    { "+activate",          "Use / plant / defuse" },
    { "weapnext",           "Next weapon" },
    { "weapprev",           "Previous weapon" },
    { "weapalt",            "Weapon alternate" },
    { "weaponslot primary", "Primary weapon" },
    { "weaponslot primaryb","Secondary weapon" },
    { "weaponslot pistol",  "Pistol" },
    { "weaponslot grenade", "Grenade" },
    { "+dropweapon",        "Drop weapon" },
};
const BindAct k_comm[] = {
    { "messagemode",        "Chat" },
    { "messagemode2",       "Team chat" },
    { "mp_QuickMessage",    "Quick chat" },
    { "vote yes",           "Vote yes" },
    { "vote no",            "Vote no" },
    { "+scores",            "Scoreboard" },
};
const BindAct k_misc[] = {
    { "record",             "Record demo" },
    { "stoprecord",         "Stop recording" },
    { "screenshotJPEG",     "Screenshot" },
    { "toggleconsole",      "Console" },
};
struct BindGroup { const char* title; const BindAct* acts; int n; };
const BindGroup k_groups[] = {
    { "MOVEMENT",      k_move,   (int)(sizeof(k_move)   / sizeof(k_move[0])) },
    { "COMMUNICATION", k_comm,   (int)(sizeof(k_comm)   / sizeof(k_comm[0])) },
    { "COMBAT",        k_combat, (int)(sizeof(k_combat) / sizeof(k_combat[0])) },
    { "MISC",          k_misc,   (int)(sizeof(k_misc)   / sizeof(k_misc[0])) },
};

std::vector<std::pair<std::string, std::string>> g_binds;   // keyname -> command
DWORD g_binds_scan = 0;
const BindAct* g_cap_act = nullptr;                          // capture target

// live binds are not readable without more engine RE; the config file is the
// engine's own persisted truth, and we writeconfig after every change so the
// two never drift for long
void parse_binds() {
    g_binds.clear();
    char base[MAX_PATH];
    if (!game_root(base, sizeof(base))) return;
    const char* fsg = cv_str("fs_game", "");
    char path[MAX_PATH];
    FILE* f = nullptr;
    if (fsg && fsg[0]) {
        snprintf(path, sizeof(path), "%s%s/config_mp.cfg", base, fsg);
        f = fopen(path, "r");
    }
    if (!f) {
        snprintf(path, sizeof(path), "%sMain/config_mp.cfg", base);
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "bind", 4) != 0) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') ++p;
        char key[32]; int ki = 0;
        while (*p && *p != ' ' && *p != '\t' && ki < 31) key[ki++] = *p++;
        key[ki] = 0;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '"') ++p;
        char cmd[256]; int ci = 0;
        while (*p && *p != '"' && *p != '\n' && *p != '\r' && ci < 255) cmd[ci++] = *p++;
        cmd[ci] = 0;
        if (key[0] && cmd[0]) g_binds.push_back({ key, cmd });
    }
    fclose(f);
}

// bound keys for an action, joined "F / MOUSE2"; true if any
bool keys_for(const char* cmd, char* out, size_t n) {
    out[0] = 0;
    int found = 0;
    for (const auto& b : g_binds) {
        if (_stricmp(b.second.c_str(), cmd) != 0) continue;
        if (found == 2) { strncat(out, " +", n - strlen(out) - 1); break; }
        if (found) strncat(out, " / ", n - strlen(out) - 1);
        strncat(out, b.first.c_str(), n - strlen(out) - 1);
        ++found;
    }
    return found > 0;
}

// VK -> the engine's Q3-style key names (TAB/SPACE/KP_INS/MWHEELUP... as seen in
// every config the game writes). NULL = not bindable from here.
const char* keyname_from_vk(int vk) {
    static char one[2] = "?";
    if (vk >= 'A' && vk <= 'Z') { one[0] = (char)vk; return one; }
    if (vk >= '0' && vk <= '9') { one[0] = (char)vk; return one; }
    if (vk >= VK_F1 && vk <= VK_F12) {
        static char fn[4];
        snprintf(fn, sizeof(fn), "F%d", vk - VK_F1 + 1);
        return fn;
    }
    switch (vk) {
    case VK_SPACE:      return "SPACE";
    case VK_TAB:        return "TAB";
    case VK_RETURN:     return "ENTER";
    case VK_BACK:       return "BACKSPACE";
    case VK_CAPITAL:    return "CAPSLOCK";
    case VK_SHIFT:      return "SHIFT";
    case VK_CONTROL:    return "CTRL";
    case VK_MENU:       return "ALT";
    case VK_UP:         return "UPARROW";
    case VK_DOWN:       return "DOWNARROW";
    case VK_LEFT:       return "LEFTARROW";
    case VK_RIGHT:      return "RIGHTARROW";
    case VK_DELETE:     return "DEL";
    case VK_PRIOR:      return "PGUP";
    case VK_NEXT:       return "PGDN";
    case VK_HOME:       return "HOME";
    case VK_END:        return "END";
    case VK_PAUSE:      return "PAUSE";
    case VK_OEM_3:      return "~";
    case VK_OEM_1:      return "SEMICOLON";
    case VK_OEM_COMMA:  return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2:      return "/";
    case VK_OEM_MINUS:  return "-";
    case VK_OEM_PLUS:   return "=";
    case VK_OEM_4:      return "[";
    case VK_OEM_6:      return "]";
    case VK_OEM_5:      return "\\";
    case VK_OEM_7:      return "'";
    case VK_NUMPAD0:    return "KP_INS";
    case VK_NUMPAD1:    return "KP_END";
    case VK_NUMPAD2:    return "KP_DOWNARROW";
    case VK_NUMPAD3:    return "KP_PGDN";
    case VK_NUMPAD4:    return "KP_LEFTARROW";
    case VK_NUMPAD5:    return "KP_5";
    case VK_NUMPAD6:    return "KP_RIGHTARROW";
    case VK_NUMPAD7:    return "KP_HOME";
    case VK_NUMPAD8:    return "KP_UPARROW";
    case VK_NUMPAD9:    return "KP_PGUP";
    case VK_DECIMAL:    return "KP_DEL";
    case VK_DIVIDE:     return "KP_SLASH";
    case VK_SUBTRACT:   return "KP_MINUS";
    case VK_ADD:        return "KP_PLUS";
    case VK_LBUTTON:    return "MOUSE1";
    case VK_RBUTTON:    return "MOUSE2";
    case VK_MBUTTON:    return "MOUSE3";
    case VK_XBUTTON1:   return "MOUSE4";
    case VK_XBUTTON2:   return "MOUSE5";
    case 0xF001:        return "MWHEELUP";
    case 0xF002:        return "MWHEELDOWN";
    }
    return nullptr;
}

void collect_hz() {
    g_hz.clear();
    if (g_sel < 0) return;
    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i) {
        if ((int)dm.dmPelsWidth != g_modes[g_sel].w) continue;
        if ((int)dm.dmPelsHeight != g_modes[g_sel].h) continue;
        int hz = (int)dm.dmDisplayFrequency;
        if (hz > 1 && std::find(g_hz.begin(), g_hz.end(), hz) == g_hz.end())
            g_hz.push_back(hz);
    }
    std::sort(g_hz.begin(), g_hz.end());
    g_hz_sel = 0;                     // Auto until the player picks
}

void collect_modes() {
    g_modes.clear();
    probe_desktop_resolution(&g_desk_w, &g_desk_h);
    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32) continue;
        Mode m = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight };
        bool dup = false;
        for (const Mode& e : g_modes) if (e.w == m.w && e.h == m.h) { dup = true; break; }
        if (!dup) g_modes.push_back(m);
    }
    const Mode classics[] = { {1440,1080}, {1280,960}, {1152,864}, {1024,768} };
    for (const Mode& c : classics) {
        bool dup = false;
        for (const Mode& e : g_modes) if (e.w == c.w && e.h == c.h) { dup = true; break; }
        if (!dup && c.w <= g_desk_w * 2) g_modes.push_back(c);
    }
    std::sort(g_modes.begin(), g_modes.end(),
              [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
    int cw = 0, ch = 0;
    if (!probe_config_resolution(&cw, &ch)) { cw = 640; ch = 480; }
    g_sel = -1;
    for (size_t i = 0; i < g_modes.size(); ++i)
        if (g_modes[i].w == cw && g_modes[i].h == ch) { g_sel = (int)i; break; }
    if (g_sel < 0) { g_modes.push_back({ cw, ch }); g_sel = (int)g_modes.size() - 1; }
    collect_hz();
    g_view_mode = cv_int("cod1x_viewmode", 0);        // what the bridge currently applies
    if (g_view_mode < 0 || g_view_mode > 2) g_view_mode = 0;
    g_fov = cv_int("cg_fov", COD1X_FOV_MIN);
    if (g_fov < COD1X_FOV_MIN) g_fov = COD1X_FOV_MIN;
    if (g_fov > COD1X_FOV_MAX) g_fov = COD1X_FOV_MAX;
}

// ---------------------------------------------------------------- widgets
bool hit(float x, float y, float w, float h) {
    const UiInput& in = ui_input();
    return in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
}

bool button(float x, float y, float w, float h, const char* label, bool primary) {
    bool over = hit(x, y, w, h);
    float hv = ui_smooth(wkey(x, y, 1), over ? 1.0f : 0.0f, 14.0f);
    ui_rect_rounded(x, y, w, h, 8,
                    primary ? mixc(UI_ACCENT, 0xFF66B2FF, hv) : mixc(UI_ROW, UI_ROW_HOT, hv));
    float tw = ui_text_width(16, 600, label);
    ui_text(x + (w - tw) / 2, y + (h - 22) / 2, 16, 600,
            primary ? 0xFF10151C : UI_TEXT, label);
    return over && ui_input().clicked;
}

int cycler(float x, float y, float w, const char* value) {
    const float h = 44;
    ui_rect_rounded(x, y, w, h, 8, UI_ROW);
    int r = 0;
    bool lo = hit(x, y, 44, h), ro = hit(x + w - 44, y, 44, h);
    if (lo) ui_rect_rounded(x, y, 44, h, 8, UI_ROW_HOT);
    if (ro) ui_rect_rounded(x + w - 44, y, 44, h, 8, UI_ROW_HOT);
    ui_text(x + 17, y + 9, 18, 600, lo ? UI_ACCENT : UI_MUTED, "<");
    ui_text(x + w - 27, y + 9, 18, 600, ro ? UI_ACCENT : UI_MUTED, ">");
    float tw = ui_text_width(17, 600, value);
    ui_text(x + (w - tw) / 2, y + 10, 17, 600, UI_TEXT, value);
    if (ui_input().clicked) { if (lo) r = -1; else if (ro) r = 1; }
    return r;
}

int segmented(float x, float y, float w, const char* const* opts, int n, int sel) {
    const float h = 40, seg = w / n, pad = 5;   // pad = gap between pills (was 3, looked glued)
    ui_rect_rounded(x, y, w, h, 8, UI_ROW);
    // the accent pill GLIDES between segments instead of teleporting
    float selpos = ui_smooth(wkey(x, y, 3), (float)sel, 13.0f);
    ui_rect_rounded(x + seg * selpos + pad, y + pad, seg - pad * 2, h - pad * 2, 6, UI_ACCENT);
    for (int i = 0; i < n; ++i) {
        float sx = x + seg * i;
        bool over = hit(sx, y, seg, h);
        if (i != sel) {
            float hv = ui_smooth(wkey(sx, y, 2), over ? 1.0f : 0.0f, 14.0f);
            if (hv > 0.01f) {
                DWORD hc = mixc(0x0020242D, 0x662A2F3A, hv);   // translucent hover veil
                ui_rect_rounded(sx + pad, y + pad, seg - pad * 2, h - pad * 2, 6, hc);
            }
        }
        // thin divider between segments (not before the first)
        if (i > 0) ui_rect(sx, y + 9, 1, h - 18, 0x22FFFFFF);
        // text darkens as the pill arrives under it
        float prox = 1.0f - (selpos > i ? selpos - i : i - selpos);
        if (prox < 0) prox = 0;
        float tw = ui_text_width(15, 600, opts[i]);
        ui_text(sx + (seg - tw) / 2, y + 9, 15, 600, mixc(UI_TEXT, 0xFF10151C, prox), opts[i]);
        if (over && ui_input().clicked) sel = i;
    }
    return sel;
}

int slider(float x, float y, float w, int lo, int hi, int v) {
    const float h = 40, ty = y + h / 2;
    ui_rect_rounded(x, ty - 3, w, 6, 3, UI_ROW);
    float t = (float)(v - lo) / (float)(hi - lo);
    ui_rect_rounded(x, ty - 3, w * t, 6, 3, UI_ACCENT);
    bool over = hit(x - 10, y, w + 20, h);
    ui_rect_rounded(x + w * t - 9, ty - 9, 18, 18, 9, over ? 0xFFFFFFFF : UI_TEXT);
    const UiInput& in = ui_input();
    if (over && in.down) {
        float nt = (in.mx - x) / w;
        nt = nt < 0 ? 0 : (nt > 1 ? 1 : nt);
        v = lo + (int)(nt * (hi - lo) + 0.5f);
    }
    return v;
}

bool toggle(float x, float y, const char* label, bool on) {
    const float w = 52, h = 28;
    bool over = hit(x, y, w, h);
    ui_rect_rounded(x, y, w, h, 14, on ? UI_ACCENT : (over ? UI_ROW_HOT : UI_ROW));
    float kt = ui_smooth(wkey(x, y, 4), on ? 1.0f : 0.0f, 15.0f);
    ui_rect_rounded(x + 4 + kt * (w - 28), y + 4, 20, 20, 10, 0xFFFFFFFF);
    ui_text(x + w + 14, y + 2, 16, 400, UI_TEXT, label);
    if ((over || hit(x + w, y, 300, h)) && ui_input().clicked) return !on;
    return on;
}

// section eyebrow: SMALL CAPS + a hairline running to the column's right edge -
// the line is what makes columns scan as a structured sheet instead of a list
void section(float x, float y, float w, const char* s) {
    ui_text(x, y, 13, 600, UI_EYEB, s);
    float tw = ui_text_width(13, 600, s);
    if (w > tw + 26) ui_rect(x + tw + 14, y + 9, w - tw - 14, 1, UI_LINE);
}
void hint(float x, float y, const char* s) { ui_text(x, y, 13, 400, UI_HINT, s); }

float keychip(float x, float y, const char* k) {
    float w = ui_text_width(11, 600, k) + 14;
    ui_rect_rounded(x, y, w, 20, 5, UI_ROW);
    ui_rect_border(x, y, w, 20, 5, 1.0f, UI_LINE);
    ui_text(x + 7, y + 2, 11, 600, 0xFF9A9A9A, k);
    return w;
}

// ------------------------------------------------------- rail glyph icons
// Drawn with primitives (the flat direction bans textures); c follows the label.
void icon_tab(int id, float x, float y, DWORD c) {
    switch (id) {
    case 0:                                             // display: frame + base
        ui_rect_border(x, y + 2, 18, 12, 0, 1.5f, c);
        ui_rect(x + 5, y + 16.5f, 8, 1.5f, c);
        break;
    case 1:                                             // mouse: shell + wheel
        ui_rect_border(x + 3.5f, y, 11, 18, 0, 1.5f, c);
        ui_rect(x + 8.2f, y + 3, 1.6f, 5, c);
        break;
    case 2:                                             // keys: keyboard
        ui_rect_border(x, y + 3, 18, 12, 0, 1.5f, c);
        ui_rect(x + 3.5f, y + 6.5f, 2, 2, c);
        ui_rect(x + 8,    y + 6.5f, 2, 2, c);
        ui_rect(x + 12.5f, y + 6.5f, 2, 2, c);
        ui_rect(x + 5, y + 10.5f, 8, 2, c);
        break;
    case 3:                                             // game: crosshair
        ui_rect(x + 8, y, 2, 5.5f, c);
        ui_rect(x + 8, y + 12.5f, 2, 5.5f, c);
        ui_rect(x, y + 8, 5.5f, 2, c);
        ui_rect(x + 12.5f, y + 8, 5.5f, 2, c);
        ui_rect(x + 8, y + 8, 2, 2, c);
        break;
    case 4:                                             // files: folder
        ui_rect(x, y + 2, 8, 3, c);
        ui_rect_border(x, y + 5, 18, 11, 0, 1.5f, c);
        break;
    default:                                            // about: the letter i
        ui_ellipse(x + 9, y + 9, 8.5f, 8.5f, 1.5f, c);
        ui_rect(x + 8, y + 4, 2, 2, c);
        ui_rect(x + 8, y + 8, 2, 6, c);
        break;
    }
}

// ---------------------------------------------------------------- preview
// Two preview views, switchable under the frame:
//   Shapes  - the analytical one: grid + the circle that stretching turns into an
//             ellipse (ghost circle = the truth it left behind);
//   Soldier - the visceral one: the REAL player model, stretched by the exact
//             screen-space factor the GPU applies.
int g_pv_view = 1;

void scene_frame(float sx, float sy, float sw2, float sh2) {
    ui_rect(sx, sy, sw2, sh2 * 0.55f, 0xFF161616);
    ui_rect(sx, sy, sw2, sh2 * 0.30f, 0xFF101010);
    ui_rect(sx, sy + sh2 * 0.55f, sw2, 1, 0xFF3A3A3A);
    ui_rect(sx, sy + sh2 * 0.55f, sw2, sh2 * 0.45f, 0xFF0C0C0C);
}

int sel_hz() {
    // Auto = HIGHEST rate listed at this resolution (g_hz is sorted ascending) -
    // the old 60 fallback made the soldier preview judder at 60 while the label
    // promised "highest available" ("on a l'impression c'est 59hz", 2026-08-25).
    int hz = (g_hz_sel > 0 && g_hz_sel <= (int)g_hz.size()) ? g_hz[g_hz_sel - 1]
             : (!g_hz.empty() ? g_hz.back() : 60);
    return hz < 30 ? 30 : hz;
}

// THE JUDDER, made visible. Speed alone does not show what a low refresh rate FEELS
// like - everything stays fluid, just slower. So the scene contains continuous motion
// (a strafe sway) whose position is only sampled at the simulated refresh rate:
// time is floored to the display's update grid. At 144 Hz the sway is silk; at 50 Hz
// it visibly steps. (Scaled 1:6 so the eye can perceive the steps at all.)
float judder_time() {
    float simfps = sel_hz() / 6.0f;
    float t = GetTickCount() / 1000.0f;
    return floorf(t * simfps) / simfps;
}

void sheen(float sx, float sy, float sw2, float sh2) {
    // A refresh SCAN, not a glass sheen: sweeps top to bottom like the real raster,
    // and its speed follows the SELECTED refresh rate immediately - no Apply needed.
    // 144 Hz reads as a quick sweep, 50 Hz as a slow crawl; the difference is the
    // point. (Scaled ~1:100 so the eye can follow what a monitor does too fast to see.)
    int hz = sel_hz();
    DWORD period = (DWORD)(240000 / hz);
    float simfps = hz / 6.0f;
    float tq = floorf((GetTickCount() / 1000.0f) * simfps) / simfps;
    float ph = fmodf(tq * 1000.0f, (float)period) / (float)period;
    float yy = sy + ph * sh2;
    ui_rect(sx, yy, sw2, sh2 * 0.055f, 0x0EFFFFFF);
    if (yy + sh2 * 0.075f < sy + sh2)
        ui_rect(sx, yy + sh2 * 0.065f, sw2, sh2 * 0.018f, 0x08FFFFFF);
}

void draw_scene(float sx, float sy, float sw2, float sh2, float st, float fovx) {
    float sway = sinf(judder_time() * 1.1f) * sw2 * 0.16f;
    if (g_pv_view == 0) {
        // Shapes: grid stretched with the image + circle/ellipse morph
        ui_rect(sx, sy, sw2, sh2, 0xFF101010);
        for (int i = 1; i < 4; ++i) {
            ui_rect(sx + sw2 * i / 4 - 0.5f, sy, 1, sh2, 0xFF262626);
            ui_rect(sx, sy + sh2 * i / 4 - 0.5f, sw2, 1, 0xFF262626);
        }
        if (fovx > 1.02f) {
            ui_rect(sx + sw2 / 8 - 0.5f, sy, 1, sh2, 0xFF303030);
            ui_rect(sx + sw2 * 7 / 8 - 0.5f, sy, 1, sh2, 0xFF303030);
        }
        float rr = sh2 * 0.28f;
        ui_ellipse(sx + sw2 / 2 + sway, sy + sh2 / 2, rr, rr, 1.5f, 0xFF555555);
        ui_ellipse(sx + sw2 / 2 + sway, sy + sh2 / 2, rr * st, rr, 2.5f, UI_ACCENT);
    } else {
        scene_frame(sx, sy, sw2, sh2);
        ui_rect(sx + sw2 * 0.08f, sy + sh2 * 0.38f, sw2 * 0.10f * st, sh2 * 0.17f, 0xFF1E1E1E);
        ui_rect(sx + sw2 * 0.78f, sy + sh2 * 0.30f, sw2 * 0.13f * st, sh2 * 0.25f, 0xFF1A1A1A);
        // real model, running along the sway; judder-quantised time makes a low
        // refresh rate step the run cycle exactly like it steps the sway
        float jt = judder_time();
        player_preview_draw(sx + sw2 / 2 + sway, sy + sh2 * 0.92f, sh2 * 0.66f,
                            st, jt, cosf(jt * 1.1f));
        float ccx = sx + sw2 / 2, ccy = sy + sh2 * 0.50f, cl = sh2 * 0.05f;
        ui_rect(ccx - cl - 2, ccy - 0.75f, cl, 1.5f, 0xFFB6B6B6);
        ui_rect(ccx + 2,      ccy - 0.75f, cl, 1.5f, 0xFFB6B6B6);
        ui_rect(ccx - 0.75f, ccy - cl - 2, 1.5f, cl, 0xFFB6B6B6);
        ui_rect(ccx - 0.75f, ccy + 2,      1.5f, cl, 0xFFB6B6B6);
    }
    sheen(sx, sy, sw2, sh2);
}

// viewfinder brackets: four L corners over the scene - the "cadran". This replaced
// the fake monitor (bezel + stand) on request: the frame should present the image,
// not pretend to be furniture.
void brackets(float sx, float sy, float w, float h) {
    const float a = 16, th = 2, in = 8;
    const DWORD c = 0xFF585858;
    ui_rect(sx + in,           sy + in,           a,  th, c);
    ui_rect(sx + in,           sy + in,           th, a,  c);
    ui_rect(sx + w - in - a,   sy + in,           a,  th, c);
    ui_rect(sx + w - in - th,  sy + in,           th, a,  c);
    ui_rect(sx + in,           sy + h - in - th,  a,  th, c);
    ui_rect(sx + in,           sy + h - in - a,   th, a,  c);
    ui_rect(sx + w - in - a,   sy + h - in - th,  a,  th, c);
    ui_rect(sx + w - in - th,  sy + h - in - a,   th, a,  c);
}

void draw_preview(float x, float y, float w, const Mode& m, int dmode, int vmode) {
    const float pad = 12, header = 34, caph = 30;
    float sw2 = w - pad * 2;
    float sh2 = sw2 * g_desk_h / g_desk_w;
    float cardh = header + sh2 + caph + pad;

    // the card
    ui_rect_rounded(x, y, w, cardh, 10, UI_PANEL);
    ui_rect_border(x, y, w, cardh, 10, 1.0f, UI_LINE);
    ui_text(x + pad + 2, y + 9, 12, 600, UI_EYEB, "LIVE PREVIEW");

    // res badge, top-right in the header
    char badge[48];
    snprintf(badge, sizeof(badge), "%dx%d @ %d Hz", m.w, m.h, sel_hz());
    float bw = ui_text_width(12, 600, badge) + 16;
    ui_rect_rounded(x + w - pad - bw, y + 6, bw, 20, 5, UI_ROW);
    ui_text(x + w - pad - bw + 8, y + 8, 12, 600, 0xFFDADADA, badge);

    const float sx = x + pad, sy = y + header;
    const float rar = (float)m.w / m.h, dar = (float)g_desk_w / g_desk_h;
    char cap[160];

    if (dmode == 0) {
        float raw_st = dar / rar;
        float st = ui_smooth(9001, vmode == 1 ? 1.0f : raw_st, 8.0f);
        float fovx = ui_smooth(9004, vmode == 1 ? 1.15f : 1.0f, 8.0f);
        draw_scene(sx, sy, sw2, sh2, st, fovx);
        brackets(sx, sy, sw2, sh2);
        if (vmode == 1)
            snprintf(cap, sizeof(cap), "Hor+ widescreen  -  true proportions, wider view");
        else if (raw_st > 1.01f)
            snprintf(cap, sizeof(cap), "Stretched over %dx%d  -  models %d%% wider",
                     g_desk_w, g_desk_h, (int)((raw_st - 1.0f) * 100 + 0.5f));
        else
            snprintf(cap, sizeof(cap), "Native fullscreen %dx%d", m.w, m.h);
        ui_text(sx, y + header + sh2 + 8, 13, 400, UI_MUTED, cap);
    } else {
        if (m.w > g_desk_w || m.h > g_desk_h) {
            ui_rect(sx, sy, sw2, sh2, 0xFF0A0A0A);
            ui_rect_border(sx, sy, sw2, sh2, 0, 2.0f, UI_DANGER);
            snprintf(cap, sizeof(cap), "%dx%d exceeds the %dx%d desktop - the mod "
                     "will force fullscreen", m.w, m.h, g_desk_w, g_desk_h);
            ui_text(sx, y + header + sh2 + 8, 13, 400, UI_DANGER, cap);
        } else {
            // the scene sits inside a desktop-shaped dark field, at true scale -
            // a window is never stretched, black surrounds it instead
            ui_rect(sx, sy, sw2, sh2, 0xFF0A0A0A);
            float sc = sw2 / g_desk_w;
            float cw2 = ui_smooth(9002, m.w * sc, 8.0f);
            float ch2 = ui_smooth(9003, m.h * sc, 8.0f);
            float cx = sx + (sw2 - cw2) / 2, cy = sy + (sh2 - ch2) / 2;
            draw_scene(cx, cy, cw2, ch2, 1.0f, 1.0f);
            ui_rect_border(cx, cy, cw2, ch2, 0, 1.0f, 0xFF3A3A3A);
            brackets(sx, sy, sw2, sh2);
            if (m.w < g_desk_w)
                snprintf(cap, sizeof(cap), "Black bars (%d px each side)  -  a window "
                         "is never stretched", (g_desk_w - m.w) / 2);
            else
                snprintf(cap, sizeof(cap), "Borderless, fills the desktop");
            ui_text(sx, y + header + sh2 + 8, 13, 400, UI_MUTED, cap);
        }
    }

    static const char* views[] = { "Shapes", "Player model" };
    g_pv_view = segmented(x, y + cardh + 14, 260, views, 2, g_pv_view);
    hint(x + 274, y + cardh + 24, "sway + scan follow the selected refresh rate");
}

void apply_display() {
    const Mode& m = g_modes[g_sel];
    int fs = (g_display_mode == 0) ? 1 : 0;
    // Auto must RESOLVE to the highest listed rate. r_displayRefresh 0 ("driver
    // picks") is not auto-max: a mode set without a frequency falls back to the
    // mode's DEFAULT rate - 60 Hz on most panels - and the archived seta plus the
    // refresh_rate=auto ini writeback made that downgrade stick across launches.
    int hz = g_hz_sel > 0 && g_hz_sel <= (int)g_hz.size() ? g_hz[g_hz_sel - 1]
             : (!g_hz.empty() ? g_hz.back() : 0);
    cmdf("seta r_mode -1\nseta r_customwidth %d\nseta r_customheight %d\n"
         "seta r_fullscreen %d\nseta r_displayRefresh %d\nvid_restart\n",
         m.w, m.h, fs, hz);
    cmdf("seta cod1x_viewmode %d\n", g_view_mode);   // the bridge cvar (settings_menu.cpp) - applied live
    menu_ini_writeback("fullscreen", fs ? "on" : "off");
    menu_ini_writeback("window_borderless", fs ? "off" : "on");
    if (g_hz_sel == 0) {
        // "max" re-resolves at every launch (settings_menu), so a new monitor or
        // cable keeps getting its true maximum - never write the literal "auto"
        // token here, resolve_refresh_hz maps it to 0 = the 60 Hz trap above.
        menu_ini_writeback("refresh_rate", "max");
    } else if (hz > 0) {
        char b[16]; snprintf(b, sizeof(b), "%d", hz);
        menu_ini_writeback("refresh_rate", b);
    }
    logger::logf("modern_menu: apply %dx%d fs=%d hz=%d view=%d", m.w, m.h, fs, hz, g_view_mode);
    g_dirty = false;
}

// muted explainer card for the right-hand side of text-only tabs; lines are
// pre-wrapped (ui_text has no wrapping) - keep them under ~52 chars
void info_card(float x, float y, float w, const char* title,
               const char* const* lines, int n) {
    float h = 46 + n * 22 + 14;
    ui_rect_rounded(x, y, w, h, 10, UI_PANEL);
    ui_rect_border(x, y, w, h, 10, 1.0f, UI_LINE);
    ui_text(x + 18, y + 14, 12, 600, UI_EYEB, title);
    for (int i = 0; i < n; ++i) {
        if (lines[i][0])
            ui_text(x + 18, y + 42 + i * 22, 13, 400, 0xFF9A9A9A, lines[i]);
    }
}

}  // namespace

bool modern_menu_poll_open() {
    if (cv_int("cod1x_open_modern", 0)) {
        cmdf("set cod1x_open_modern 0\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------- draw
void modern_menu_draw(float sw, float sh) {
    if (!g_init) { collect_modes(); g_init = true; }

    // entrance: fade in, content rises a few pixels
    static DWORD s_lastdraw = 0;
    DWORD nowt = GetTickCount();
    if (nowt - s_lastdraw > 250) ui_anim_set(8000, 0.0f);
    s_lastdraw = nowt;
    float ot = ui_smooth(8000, 1.0f, 9.0f);
    ui_alpha(ot);

    // FULLSCREEN, monochrome: black ground, thin hairlines, a left rail for nav.
    ui_rect(0, 0, sw, sh, UI_BG);
    const float rail = 280;
    ui_rect(0, 0, rail, sh, UI_PANEL);
    ui_rect(rail, 0, 1, sh, 0xFF2A2A2A);

    // brand block
    ui_text(36, 40, 36, 600, UI_TEXT, "1.6X");
    ui_text(38, 88, 11, 600, UI_MUTED, "C O D 1  R E L O A D E D");
    ui_rect(36, 118, rail - 72, 1, UI_LINE);

    static const char* tabs[] = { "Display", "Mouse", "Keys", "Game", "Files", "About" };
    static const char* subs[] = {
        "Resolution, refresh rate and how the image reaches the screen.",
        "Raw input and aim feel. What your hand does is what the game gets.",
        "Click an action, press the new key. Saved to your config instantly.",
        "Performance, HUD, effects and the competitive netcode.",
        "Configs and demos - Main and every mod folder. One click to run.",
        "What this client is and what runs under the hood.",
    };
    float taby = 150;
    float tsel = ui_smooth(8002, (float)g_tab, 13.0f);
    ui_rect_rounded(20, 150 + tsel * 54 - 6, rail - 40, 46, 10, UI_ACCENT);
    for (int i = 0; i < 6; ++i) {
        bool over = hit(20, taby - 6, rail - 40, 46);
        float prox = 1.0f - (tsel > i ? tsel - i : i - tsel);
        if (prox < 0) prox = 0;
        if (i != g_tab && over)
            ui_rect_rounded(20, taby - 6, rail - 40, 46, 10, 0x33242424);
        DWORD tc = mixc(UI_MUTED, 0xFF0A0A0A, prox);
        icon_tab(i, 40, taby + 7, tc);
        ui_text(72, taby + 4, 18, 600, tc, tabs[i]);
        if (over && ui_input().clicked) g_tab = i;
        taby += 54;
    }
    // leaving the Keys tab mid-capture must release the keyboard, or every key
    // would keep vanishing into a bind that no longer has a visible target
    if (g_tab != 2 && g_cap_act) { g_cap_act = nullptr; ui_key_capture(false); }

    // rail footer: key chips + version
    {
        float ky = sh - 96;
        float kx = 36;
        kx += keychip(kx, ky, "CTRL+M") + 10;
        ui_text(kx, ky + 2, 12, 400, UI_HINT, "open / close");
        ky += 30;
        kx = 36;
        kx += keychip(kx, ky, "ESC") + 10;
        ui_text(kx, ky + 2, 12, 400, UI_HINT, "back");
        ui_text(36, sh - 34, 11, 400, 0xFF4A4A4A, "1.6X dev build - 2026-08-21");
    }

    // content frame of reference (kept so the tab bodies below stay untouched)
    const float pw = sw - rail;
    const float px = rail + 24, py = 40 + (1.0f - ot) * 14.0f;
    ui_text(px + 36, py + 10, 30, 600, UI_TEXT, tabs[g_tab]);
    ui_text(px + 36, py + 52, 14, 400, UI_MUTED, subs[g_tab]);
    ui_rect(px + 36, py + 84, pw - 120, 1, 0xFF2A2A2A);

    // tab content slides horizontally between tabs
    float tt = ui_smooth(8001, (float)g_tab, 12.0f);
    float slide = (tt - g_tab) * 46.0f;
    float frac = tt - g_tab; if (frac < 0) frac = -frac;
    ui_alpha(ot * (1.0f - (frac > 1 ? 1 : frac) * 0.85f));
    float cx = px + 36 + slide, cy = py + 122;
    const float colw = 430;
    // second column (Game/Mouse info) only when it truly fits
    const float col2x = cx + colw + 60;
    const float col2w = pw - 120 - colw - 60;
    const bool  two_col = col2w >= 320;

    if (g_tab == 0) {
        section(cx, cy, colw, "DISPLAY MODE"); cy += 24;
        static const char* dmodes[] = { "Fullscreen", "Borderless" };
        int nm = segmented(cx, cy, colw, dmodes, 2, g_display_mode);
        if (nm != g_display_mode) { g_display_mode = nm; g_dirty = true; }
        cy += 46;
        hint(cx, cy, "Fullscreen stretches to the whole screen; borderless never stretches.");
        cy += 30;

        section(cx, cy, colw, "RESOLUTION"); cy += 24;
        char rv[32];
        snprintf(rv, sizeof(rv), "%d x %d", g_modes[g_sel].w, g_modes[g_sel].h);
        int d = cycler(cx, cy, colw, rv);
        if (d) { g_sel = (g_sel + d + (int)g_modes.size()) % (int)g_modes.size();
                 collect_hz(); g_dirty = true; }
        cy += 58;

        section(cx, cy, colw, "REFRESH RATE"); cy += 24;
        char hv[32];
        if (g_hz_sel == 0) snprintf(hv, sizeof(hv), "Auto  (max: %d Hz)", sel_hz());
        else snprintf(hv, sizeof(hv), "%d Hz", g_hz[g_hz_sel - 1]);
        int dh = cycler(cx, cy, colw, hv);
        if (dh) { int n = (int)g_hz.size() + 1;
                  g_hz_sel = (g_hz_sel + dh + n) % n; g_dirty = true; }
        cy += 58;

        section(cx, cy, colw, "VIEW"); cy += 24;
        static const char* vmodes[] = { "Classic", "Widescreen", "Stretched" };
        int nv = segmented(cx, cy, colw, vmodes, 3, g_view_mode);
        if (nv != g_view_mode) { g_view_mode = nv; g_dirty = true; }
        cy += 40;
        static const char* vmode_hint[] = {
            "Classic - vanilla FOV, no correction.",
            "Widescreen - Hor+ FOV correction on wide screens.",
            "Stretched - the 4:3 look of a stretched res, done at native resolution.",
        };
        hint(cx, cy, vmode_hint[g_view_mode < 0 || g_view_mode > 2 ? 0 : g_view_mode]);
        cy += 24;

        section(cx, cy, colw, "FIELD OF VIEW"); cy += 24;
        int nf = slider(cx, cy, colw - 64, COD1X_FOV_MIN, COD1X_FOV_MAX, g_fov);
        char fv[16]; snprintf(fv, sizeof(fv), "%d", g_fov);
        ui_text(cx + colw - 44, cy + 8, 17, 600, UI_TEXT, fv);
        if (nf != g_fov) { g_fov = nf; cmdf("seta cg_fov %d\n", g_fov); }
        cy += 40;
        hint(cx, cy, "80 = vanilla, 95 = the competitive maximum. Higher sees more, draws smaller.");
        cy += 28;

        section(cx, cy, colw, "BRIGHTNESS"); cy += 24;
        int g10 = (int)(atof(cv_str("r_gamma", "1")) * 10 + 0.5f);
        if (g10 < 5) g10 = 5;
        if (g10 > 20) g10 = 20;
        int ng = slider(cx, cy, colw - 64, 5, 20, g10);
        char gv[16]; snprintf(gv, sizeof(gv), "%.1f", ng / 10.0);
        ui_text(cx + colw - 44, cy + 8, 17, 600, UI_TEXT, gv);
        if (ng != g10) cmdf("seta r_gamma %.1f\n", ng / 10.0);
        cy += 46;

        section(cx, cy, colw, "V-SYNC"); cy += 26;
        bool vs = cv_int("r_swapInterval", 0) != 0;
        bool nvs = toggle(cx, cy, "Sync to the display (r_swapInterval)", vs);
        if (nvs != vs) cmdf("seta r_swapInterval %d\n", nvs ? 1 : 0);
        cy += 36;
        hint(cx, cy, "Off = lowest input lag. On removes tearing but adds delay.");
        cy += 32;

        if (g_dirty) {
            if (button(cx, cy, 180, 46, "Apply", true)) apply_display();
            ui_text(cx + 196, cy + 14, 13, 400, UI_MUTED, "restarts the renderer");
        } else {
            ui_text(cx, cy + 14, 14, 400, UI_OK, "Current configuration active");
        }
        // GROUND TRUTH: what the display is really doing right now. When a mode/Hz
        // combination is refused, Windows silently falls back - this line is how the
        // player finds out (asked-for-320-got-50, 2026-08-10).
        {
            DEVMODEA curm = {}; curm.dmSize = sizeof(curm);
            if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &curm)) {
                char now[96];
                snprintf(now, sizeof(now), "Screen right now:  %dx%d @ %d Hz",
                         (int)curm.dmPelsWidth, (int)curm.dmPelsHeight,
                         (int)curm.dmDisplayFrequency);
                ui_text(cx, cy + 44, 13, 600, UI_MUTED, now);
            }
        }
        float pvw = pw - 120 - colw - 70;
        if (pvw > 560) pvw = 560;
        draw_preview(px + 36 + colw + 70, py + 122, pvw,
                     g_modes[g_sel], g_display_mode, g_view_mode);
    } else if (g_tab == 1) {
        section(cx, cy, colw, "RAW INPUT"); cy += 26;
        bool ri = cv_int("m_rinput", 0) != 0;
        bool nri = toggle(cx, cy, "Read the mouse at device level (m_rinput)", ri);
        if (nri != ri) cmdf("seta m_rinput %d\n", nri ? 1 : 0);
        cy += 38;
        char hz[64];
        snprintf(hz, sizeof(hz), "Measured polling rate: %s Hz", cv_str("m_rinput_hz", "-"));
        ui_text(cx, cy, 14, 400, UI_MUTED, hz);
        cy += 40;

        section(cx, cy, colw, "SENSITIVITY"); cy += 24;
        int s10 = (int)(atof(cv_str("sensitivity", "5")) * 10 + 0.5f);
        if (s10 < 1) s10 = 1;
        if (s10 > 300) s10 = 300;
        int ns = slider(cx, cy, colw - 70, 1, 300, s10);
        char sv[16]; snprintf(sv, sizeof(sv), "%.1f", ns / 10.0);
        ui_text(cx + colw - 50, cy + 8, 17, 600, UI_TEXT, sv);
        if (ns != s10) cmdf("seta sensitivity %.1f\n", ns / 10.0);
        cy += 50;

        section(cx, cy, colw, "ACCELERATION"); cy += 24;
        int a10 = (int)(atof(cv_str("cl_mouseAccel", "0")) * 10 + 0.5f);
        if (a10 < 0) a10 = 0;
        if (a10 > 10) a10 = 10;
        int na = slider(cx, cy, colw - 70, 0, 10, a10);
        char av[16]; snprintf(av, sizeof(av), "%.1f", na / 10.0);
        ui_text(cx + colw - 50, cy + 8, 17, 600, UI_TEXT, av);
        if (na != a10) cmdf("seta cl_mouseAccel %.1f\n", na / 10.0);
        cy += 40;
        hint(cx, cy, "0 = linear, the competitive default. Speed changes distance otherwise.");
        cy += 32;

        // cm/360: the unit aimers actually reason in. DPI is not knowable from
        // inside the game, so the player states it once (kept in a seta cvar).
        section(cx, cy, colw, "MOUSE DPI  +  CM PER 360"); cy += 24;
        int dpi = cv_int("cod1x_mouse_dpi", 800);
        if (dpi < 400) dpi = 400;
        if (dpi > 3200) dpi = 3200;
        int nd50 = slider(cx, cy, colw - 84, 8, 64, dpi / 50);
        char dv[16]; snprintf(dv, sizeof(dv), "%d", nd50 * 50);
        ui_text(cx + colw - 64, cy + 8, 17, 600, UI_TEXT, dv);
        if (nd50 * 50 != dpi) cmdf("seta cod1x_mouse_dpi %d\n", nd50 * 50);
        cy += 44;
        {
            float sens = (float)atof(cv_str("sensitivity", "5"));
            float myaw = (float)atof(cv_str("m_yaw", "0.022"));
            if (myaw < 0) myaw = -myaw;
            float denom = nd50 * 50 * myaw * sens;
            if (denom > 0.0001f) {
                char cm[48];
                snprintf(cm, sizeof(cm), "%.1f cm / 360", 2.54f * 360.0f / denom);
                ui_text(cx, cy, 24, 600, UI_TEXT, cm);
                hint(cx + ui_text_width(24, 600, cm) + 16, cy + 8,
                     "distance for one full turn - updates live");
            }
        }
        cy += 44;

        section(cx, cy, colw, "OPTIONS"); cy += 26;
        bool inv = atof(cv_str("m_pitch", "0.022")) < 0;
        bool ninv = toggle(cx, cy, "Invert mouse (m_pitch)", inv);
        if (ninv != inv) cmdf("seta m_pitch %s\n", ninv ? "-0.022" : "0.022");
        cy += 44;
        bool mf = cv_int("m_filter", 0) != 0;
        bool nmf = toggle(cx, cy, "Mouse smoothing (m_filter)", mf);
        if (nmf != mf) cmdf("seta m_filter %d\n", nmf ? 1 : 0);
        cy += 36;
        hint(cx, cy, "Smoothing averages two frames of movement: soft feel, real latency.");

        if (two_col) {
            static const char* L[] = {
                "Raw input reads the device itself, skipping the",
                "Windows pointer pipeline: no pointer ballistics,",
                "no desktop acceleration, identical at any Hz.",
                "",
                "The measured polling rate shows what the mouse",
                "actually delivers in-game - a 1000 Hz mouse on a",
                "USB2 hub often runs at 500.",
                "",
                "Sensitivity is engine-side and unaffected by",
                "Windows settings while raw input is on.",
            };
            info_card(col2x, cy - 300 < py + 122 ? py + 122 : cy - 300,
                      col2w > 480 ? 480 : col2w, "HOW AIM REACHES THE GAME",
                      L, sizeof(L) / sizeof(L[0]));
        }
    } else if (g_tab == 2) {
        // ------------------------------------------------------------ KEYS
        if (GetTickCount() - g_binds_scan > 2000) {
            parse_binds();
            g_binds_scan = GetTickCount();
        }
        // capture resolution: the next input becomes the bind
        if (g_cap_act) {
            int vkin = ui_input().vk;
            if (vkin == VK_ESCAPE) {
                g_cap_act = nullptr; ui_key_capture(false);
            } else if (vkin) {
                const char* kn = keyname_from_vk(vkin);
                if (kn) {
                    cmdf("bind %s \"%s\"\n", kn, g_cap_act->cmd);
                    cmdf("writeconfig config_mp.cfg\n");
                    // optimistic local update so the row refreshes this frame;
                    // the file re-parse (2 s) then confirms the engine's truth
                    for (auto& b : g_binds)
                        if (_stricmp(b.first.c_str(), kn) == 0) b.second.clear();
                    g_binds.push_back({ kn, g_cap_act->cmd });
                    g_binds_scan = GetTickCount();       // delay re-parse
                    g_cap_act = nullptr; ui_key_capture(false);
                }
            }
        }
        const float kw = (pw - 120 - 40) / 2 > 470 ? 470 : (pw - 120 - 40) / 2;
        const float rowh = 34;
        float col_y[2] = { cy, cy };
        for (int gi = 0; gi < 4; ++gi) {
            int col = gi < 2 ? 0 : 1;                    // move+comm | combat+misc
            float kx = cx + col * (kw + 40);
            float& ky = col_y[col];
            section(kx, ky, kw, k_groups[gi].title); ky += 26;
            for (int ai = 0; ai < k_groups[gi].n; ++ai) {
                const BindAct& a = k_groups[gi].acts[ai];
                bool capme = g_cap_act == &a;
                bool over = hit(kx, ky, kw, rowh - 4);
                float hv = ui_smooth(wkey(kx, ky, 6), over ? 1.0f : 0.0f, 14.0f);
                ui_rect_rounded(kx, ky, kw, rowh - 4, 6,
                                capme ? UI_ROW_HOT : mixc(UI_ROW, UI_ROW_HOT, hv));
                if (capme) ui_rect_border(kx, ky, kw, rowh - 4, 6, 1.5f, UI_ACCENT);
                ui_text(kx + 14, ky + 5, 14, 400, UI_TEXT, a.label);
                if (capme) {
                    const char* msg = "PRESS A KEY  -  ESC cancels";
                    float mw = ui_text_width(12, 600, msg);
                    ui_text(kx + kw - mw - 14, ky + 7, 12, 600, UI_ACCENT, msg);
                } else {
                    char kb[64];
                    if (keys_for(a.cmd, kb, sizeof(kb))) {
                        float bw2 = ui_text_width(12, 600, kb) + 14;
                        ui_rect_rounded(kx + kw - bw2 - 10, ky + 5, bw2, 20, 5, 0xFF101010);
                        ui_text(kx + kw - bw2 - 3, ky + 7, 12, 600, 0xFFC9C9C9, kb);
                    } else {
                        ui_text(kx + kw - 60, ky + 7, 12, 600, 0xFF4A4A4A, "unbound");
                    }
                }
                if (over && !g_cap_act && ui_input().clicked) {
                    g_cap_act = &a;
                    ui_key_capture(true);
                }
                ky += rowh;
            }
            ky += 16;
        }
        float endy = col_y[0] > col_y[1] ? col_y[0] : col_y[1];
        hint(cx, endy, "Keys, mouse buttons and the wheel all bind. Binding a key that is in use");
        hint(cx, endy + 20, "steals it from the old action. Written to config_mp.cfg immediately.");
    } else if (g_tab == 3) {
        float cy2 = cy;                       // right column runs in parallel
        section(cx, cy, colw, "FRAMERATE CAP"); cy += 24;
        static const char* caps[] = { "125 fps", "200 fps", "250 fps" };
        int fpsv = cv_int("com_maxfps", 250);
        int fsel = fpsv <= 125 ? 0 : (fpsv <= 200 ? 1 : 2);
        int nfs = segmented(cx, cy, colw, caps, 3, fsel);
        if (nfs != fsel) cmdf("seta com_maxfps %d\n", nfs == 0 ? 125 : nfs == 1 ? 200 : 250);
        cy += 46;
        hint(cx, cy, "idTech3 physics quantise at the cap - these are the known-good values.");
        cy += 30;

        section(cx, cy, colw, "TEXTURE QUALITY  (next map load)"); cy += 24;
        static const char* pm[] = { "Max", "High", "Med", "Low" };
        int pv = cv_int("r_picmip", 0);
        if (pv < 0) pv = 0;
        if (pv > 3) pv = 3;
        int npv = segmented(cx, cy, colw, pm, 4, pv);
        if (npv != pv) cmdf("seta r_picmip %d\n", npv);
        cy += 56;

        section(cx, cy, colw, "VOLUME"); cy += 24;
        int v10 = (int)(atof(cv_str("s_volume", "0.8")) * 10 + 0.5f);
        if (v10 < 0) v10 = 0;
        if (v10 > 10) v10 = 10;
        int nv10 = slider(cx, cy, colw - 70, 0, 10, v10);
        char vv[16]; snprintf(vv, sizeof(vv), "%d%%", nv10 * 10);
        ui_text(cx + colw - 52, cy + 8, 17, 600, UI_TEXT, vv);
        if (nv10 != v10) cmdf("seta s_volume %.1f\n", nv10 / 10.0);
        cy += 52;

        section(cx, cy, colw, "FRAME LIMITER  (next launch)"); cy += 26;
        static bool fl_cache = true;
        bool nfl = toggle(cx, cy, "Anti-stutter frame limiter", fl_cache);
        if (nfl != fl_cache) {
            fl_cache = nfl;
            menu_ini_writeback("frame_limiter_enable", nfl ? "on" : "off");
        }
        cy += 36;
        hint(cx, cy, "Precise frame pacing in the mod instead of the engine's busy loop.");

        // ---- right column: HUD + effects + the locked netcode card
        float rx = two_col ? col2x : cx;
        float rw = two_col ? (col2w > 430 ? 430 : col2w) : colw;
        if (!two_col) { cy += 44; cy2 = cy; }
        section(rx, cy2, rw, "HUD"); cy2 += 26;
        bool fps = cv_int("cg_drawFPS", 0) != 0;
        bool nfps = toggle(rx, cy2, "FPS counter (cg_drawFPS)", fps);
        if (nfps != fps) cmdf("seta cg_drawFPS %d\n", nfps ? 1 : 0);
        cy2 += 44;
        bool lag = cv_int("cg_lagometer", 0) != 0;
        bool nlag = toggle(rx, cy2, "Lagometer (cg_lagometer)", lag);
        if (nlag != lag) cmdf("seta cg_lagometer %d\n", nlag ? 1 : 0);
        cy2 += 44;
        bool cmp = cv_int("cg_drawCompass", 1) != 0;
        bool ncmp = toggle(rx, cy2, "Compass (cg_drawCompass)", cmp);
        if (ncmp != cmp) cmdf("seta cg_drawCompass %d\n", ncmp ? 1 : 0);
        cy2 += 44;

        section(rx, cy2, rw, "EFFECTS"); cy2 += 26;
        bool br = cv_int("cg_brass", 1) != 0;
        bool nbr = toggle(rx, cy2, "Ejected shells (cg_brass)", br);
        if (nbr != br) cmdf("seta cg_brass %d\n", nbr ? 1 : 0);
        cy2 += 44;
        bool mk = cv_int("cg_marks", 1) != 0;
        bool nmk = toggle(rx, cy2, "Bullet marks (cg_marks)", mk);
        if (nmk != mk) cmdf("seta cg_marks %d\n", nmk ? 1 : 0);
        cy2 += 44;
        bool bl = cv_int("cg_blood", 1) != 0;
        bool nbl = toggle(rx, cy2, "Blood (cg_blood)", bl);
        if (nbl != bl) cmdf("seta cg_blood %d\n", nbl ? 1 : 0);
        cy2 += 54;

        // the competitive lock, presented as a sealed card - informative, not editable
        {
            float lh = 118;
            ui_rect_rounded(rx, cy2, rw, lh, 10, UI_PANEL);
            ui_rect_border(rx, cy2, rw, lh, 10, 1.0f, UI_LINE);
            ui_text(rx + 18, cy2 + 14, 12, 600, UI_EYEB, "NETCODE - LOCKED BY THE COMPETITIVE SPEC");
            char net[128];
            snprintf(net, sizeof(net), "snaps %s    cl_maxpackets %s    rate %s",
                     cv_str("snaps", "?"), cv_str("cl_maxpackets", "?"), cv_str("rate", "?"));
            ui_text(rx + 18, cy2 + 42, 15, 600, UI_TEXT, net);
            ui_text(rx + 18, cy2 + 70, 13, 400, UI_HINT,
                    "Enforced server-side so every player runs the same");
            ui_text(rx + 18, cy2 + 90, 13, 400, UI_HINT,
                    "netcode. 40 Hz + antilag: what you see is what you hit.");
        }
    } else if (g_tab == 4) {
        if (GetTickCount() - g_files_scan > 3000) {
            scan_dir("configs", "*.cfg", g_cfgs);
            scan_demos();
            g_files_scan = GetTickCount();
        }
        const float lw = (pw - 72 - 40) / 2;
        float ly = cy;
        char hdr[64];

        // ---- PAM: the competitive mod, fetched here instead of at the first connect
        if (g_pam_install_config.enable) {
            section(cx, ly, pw - 72, "PAM MOD  (the competitive mod every 1.6X match server runs)"); ly += 26;
            PamStatus ps; pam_install_status(&ps);
            const bool busy = ps.state == PAM_CHECKING || ps.state == PAM_DOWNLOADING;
            const char* lbl = busy ? "WORKING..." : (ps.state == PAM_DONE ? "CHECK AGAIN" : "INSTALL / UPDATE PAM");
            if (button(cx, ly, 250, 38, lbl, !busy) && !busy) pam_install_start();
            const char* txt = ps.state == PAM_IDLE
                ? "Downloads the mod's pk3s (maps included) so joining a match server never waits on a download."
                : ps.text;
            const DWORD col = ps.state == PAM_ERROR ? 0xFFE05252 : (ps.state == PAM_DONE ? 0xFF6FCF7A : UI_TEXT);
            ui_text(cx + 268, ly + 9, 14, ps.state == PAM_IDLE ? 400 : 500, ps.state == PAM_IDLE ? UI_MUTED : col, txt);
            if (busy || ps.state == PAM_RESTART) {
                const float bw = pw - 72;
                ui_rect_rounded(cx, ly + 46, bw, 6, 3, UI_ROW);
                float f = ps.progress < 0 ? 0 : (ps.progress > 1 ? 1 : ps.progress);
                if (ps.state == PAM_RESTART) f = 1.f;
                ui_rect_rounded(cx, ly + 46, bw * f, 6, 3, ps.state == PAM_RESTART ? 0xFFE0B252 : UI_ACCENT);
            }
            ly += 68;
        }

        snprintf(hdr, sizeof(hdr), "CONFIGS - %d  (click to exec)", (int)g_cfgs.size());
        section(cx, ly, lw - 20, hdr);
        snprintf(hdr, sizeof(hdr), "DEMOS - %d  (click to play)", (int)g_demo_ents.size());
        section(cx + lw + 40, ly, lw - 20, hdr);
        ly += 26;
        const int vis = g_pam_install_config.enable ? 9 : 11; const float rh = 36;   // the PAM block takes two rows
        auto list = [&](float lx, std::vector<std::string>& v, float& scroll,
                        const char* cmd_fmt, bool strip) {
            if (hit(lx, ly, lw, vis * rh)) scroll -= ui_input().wheel * 2;
            int maxoff = (int)v.size() - vis; if (maxoff < 0) maxoff = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > maxoff) scroll = (float)maxoff;
            for (int i = 0; i < vis && i + (int)scroll < (int)v.size(); ++i) {
                int idx = i + (int)scroll;
                float ry = ly + i * rh;
                bool over = hit(lx, ry, lw, rh - 4);
                float hv = ui_smooth(wkey(lx, ry, 5), over ? 1.0f : 0.0f, 14.0f);
                ui_rect_rounded(lx, ry, lw, rh - 4, 6, mixc(UI_ROW, UI_ROW_HOT, hv));
                ui_text(lx + 14, ry + 6, 15, 400, UI_TEXT, v[idx].c_str());
                if (over && ui_input().clicked) {
                    std::string nm = v[idx];
                    if (strip) { size_t p = nm.rfind('.'); if (p != std::string::npos) nm.resize(p); }
                    cmdf(cmd_fmt, nm.c_str());
                    overlay_toggle(false);
                }
            }
            if (v.empty()) ui_text(lx + 4, ly + 6, 14, 400, UI_MUTED, "(empty)");
            // scrollbar: only when the list overflows
            if (maxoff > 0) {
                float trackh = vis * rh - 4;
                float thumbh = trackh * vis / (float)v.size();
                float thumby = ly + (trackh - thumbh) * (scroll / maxoff);
                ui_rect_rounded(lx + lw + 6, ly, 3, trackh, 1.5f, UI_ROW);
                ui_rect_rounded(lx + lw + 6, thumby, 3, thumbh, 1.5f, 0xFF3A3A3A);
            }
        };
        list(cx, g_cfgs, g_scroll_c, "exec configs/%s\n", false);
        // demos: every search-path folder, tagged with its source; a mod demo can
        // only be opened with that mod's filesystem active, so switching fs_game +
        // vid_restart first is the engine's own way in (the classic Q3 mod idiom)
        {
            float lx = cx + lw + 40;
            std::vector<DemoEnt>& v = g_demo_ents;
            float& scroll = g_scroll_d;
            if (hit(lx, ly, lw, vis * rh)) scroll -= ui_input().wheel * 2;
            int maxoff = (int)v.size() - vis; if (maxoff < 0) maxoff = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > maxoff) scroll = (float)maxoff;
            const char* fsg = cv_str("fs_game", "");
            for (int i = 0; i < vis && i + (int)scroll < (int)v.size(); ++i) {
                int idx = i + (int)scroll;
                float ry = ly + i * rh;
                bool over = hit(lx, ry, lw, rh - 4);
                float hv = ui_smooth(wkey(lx, ry, 5), over ? 1.0f : 0.0f, 14.0f);
                ui_rect_rounded(lx, ry, lw, rh - 4, 6, mixc(UI_ROW, UI_ROW_HOT, hv));
                ui_text(lx + 14, ry + 6, 15, 400, UI_TEXT, v[idx].name.c_str());
                // source tag, leading underscores stripped for display
                const char* tag = v[idx].dir.c_str();
                while (*tag == '_') ++tag;
                float tw2 = ui_text_width(11, 600, tag) + 12;
                ui_rect_rounded(lx + lw - tw2 - 10, ry + 6, tw2, 19, 5, 0xFF101010);
                ui_text(lx + lw - tw2 - 4, ry + 8, 11, 600, 0xFF8A8A8A, tag);
                if (over && ui_input().clicked) {
                    std::string nm = v[idx].name;
                    size_t p = nm.rfind('.'); if (p != std::string::npos) nm.resize(p);
                    bool sw = _stricmp(v[idx].dir.c_str(), "Main") != 0 &&
                              _stricmp(v[idx].dir.c_str(), fsg) != 0;
                    if (sw)
                        cmdf("seta fs_game %s\nvid_restart\ndemo %s\n",
                             v[idx].dir.c_str(), nm.c_str());
                    else
                        cmdf("demo %s\n", nm.c_str());
                    overlay_toggle(false);
                }
            }
            if (v.empty()) ui_text(lx + 4, ly + 6, 14, 400, UI_MUTED, "(empty)");
            if (maxoff > 0) {
                float trackh = vis * rh - 4;
                float thumbh = trackh * vis / (float)v.size();
                float thumby = ly + (trackh - thumbh) * (scroll / maxoff);
                ui_rect_rounded(lx + lw + 6, ly, 3, trackh, 1.5f, UI_ROW);
                ui_rect_rounded(lx + lw + 6, thumby, 3, thumbh, 1.5f, 0xFF3A3A3A);
            }
        }
        hint(cx, ly + vis * rh + 10,
             "Configs from Main/configs. Demos from every folder's demos/ - playing a mod's");
        hint(cx, ly + vis * rh + 30,
             "demo loads that mod first (fs_game + renderer restart), then plays it.");
    } else {
        // ABOUT: what 1.6X actually is - the feature list doubles as a changelog
        ui_text(cx, cy, 22, 600, UI_TEXT, "cod1reloaded  -  the 1.6X competitive client");
        cy += 34;
        char info[160];
        snprintf(info, sizeof(info), "Desktop %d x %d    overlay %.0f x %.0f    native GL, real resolution",
                 g_desk_w, g_desk_h, sw, sh);
        ui_text(cx, cy, 14, 400, UI_MUTED, info);
        cy += 40;

        section(cx, cy, colw, "WHAT RUNS UNDER THE HOOD"); cy += 28;
        static const char* feats[] = {
            "Antilag - the server rewinds to what YOU saw when you fired",
            "40 Hz tickrate - twice the vanilla update rate",
            "Lean hitbox sync - the head you see is the head you can hit",
            "Raw mouse input - device-level, no Windows pipeline",
            "Per-display gamma - brightness stays on the game screen",
            "FOV 80-95 unlocked, widescreen Hor+",
            "Anti-stutter frame limiter with precise pacing",
            "In-menu bind editor, demo browser across mod folders",
            "Native-resolution UI - this menu is drawn by the mod",
        };
        for (const char* f : feats) {
            ui_rect(cx + 2, cy + 8, 6, 2, UI_ACCENT);
            ui_text(cx + 20, cy, 14, 400, 0xFFB9B9B9, f);
            cy += 26;
        }
        cy += 16;
        section(cx, cy, colw, "KEYS"); cy += 26;
        float kx = cx;
        kx += keychip(kx, cy, "CTRL+M") + 10;
        ui_text(kx, cy + 2, 13, 400, UI_HINT, "open / close this panel");
        cy += 30;
        kx = cx;
        kx += keychip(kx, cy, "ESC") + 10;
        ui_text(kx, cy + 2, 13, 400, UI_HINT, "back / close");
        cy += 40;
        ui_text(cx, cy, 13, 400, UI_HINT,
                "Settings persist in cod1reloaded.ini and your player config.");
    }
}

}  // namespace patches
