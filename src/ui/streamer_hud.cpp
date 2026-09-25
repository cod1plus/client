// streamer_hud.cpp - see streamer_hud.h
#include "ui/streamer_hud.h"
#include "ui/gl_overlay.h"
#include "ui/menu_hooks.h"
#include "ui/game_image.h"
#include "ui/radar_map.h"
#include "core/logger.h"

#include <GL/gl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace patches {

namespace {

// ---- engine (CoDMP.exe, base 0x400000) ----
constexpr uintptr_t VA_CVAR_COUNT    = 0x01912aec;   // > 0 once the cvar system is up
constexpr uintptr_t VA_CVAR_FINDVAR  = 0x0043b790;   // cvar_t* Cvar_FindVar(name); string at +4
constexpr uintptr_t VA_CBUF_EXECTEXT = 0x0042a180;   // Cbuf_ExecuteText(when, text)
// Files are NOT read with FS_ReadFile (0x42ca30): it allocates with Hunk_AllocateTempMemory
// (0x434510), which ends in Com_Error(ERR_DROP, "Hunk_AllocateTempMemory: failed") when the
// hunk is short - a 20 MB BSP read mid-match could drop the streamer. Same calls as
// FS_ReadFile, into our own buffer:
constexpr uintptr_t VA_FS_OPENREAD   = 0x0042bc70;   // int FS_FOpenFileRead(qpath, int* f, 0, 0) -> length
constexpr uintptr_t VA_FS_READ       = 0x0042c610;   // int FS_Read(buf, len, f)
constexpr uintptr_t VA_FS_CLOSE      = 0x0042b600;   // FS_FCloseFile(f)
constexpr uintptr_t VA_FS_OPENFLAG   = 0x01908e60;   // set to 1 by every caller of FS_FOpenFileRead
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
typedef int   (__cdecl* FS_FOpenFileRead_t)(const char*, int*, int, int);
typedef int   (__cdecl* FS_Read_t)(void*, int, int);
typedef void  (__cdecl* FS_FCloseFile_t)(int);

// ---- cgame_mp_x86.dll refdef (RVAs verified by disasm, memory cod1-hitbox-draw-ingame) ----
constexpr uintptr_t RVA_REF_FOVX = 0x20d340;
constexpr uintptr_t RVA_REF_FOVY = 0x20d344;
constexpr uintptr_t RVA_REF_ORG  = 0x20d348;   // vec3
constexpr uintptr_t RVA_REF_AXIS = 0x20d354;   // 3 x vec3: forward, left, up

const char* MENU_ROOT  = "quickmessage_streamersystem_menu";
const char* MENU_OVER  = "quickmessage_streamersystem_menu_overlay";
const char* MENU_LEFT  = "quickmessage_streamersystem_menu_top_left";
const char* MENU_RIGHT = "quickmessage_streamersystem_menu_top_right";

// ---- the HUD kit (hud-kit-png/, hud/png/) ----
constexpr DWORD COL_AXIS     = 0xFFE99F47;   // orange
constexpr DWORD COL_ALLIES   = 0xFF67B2EE;   // blue
constexpr DWORD COL_PANEL_T  = 0xFF212428;   // panel gradient, top
constexpr DWORD COL_PANEL_B  = 0xFF15181B;   // panel gradient, bottom
constexpr DWORD COL_EDGE     = 0xFF494E54;   // 1 px highlight on the top edge
constexpr DWORD COL_DIVIDER  = 0xFF363A3F;
constexpr DWORD COL_TEXT     = 0xFFF0F2F4;
constexpr DWORD COL_MUTED    = 0xFF8C9298;
constexpr DWORD COL_LABEL    = 0xFF63676C;
constexpr DWORD COL_DANGER   = 0xFFE05252;
constexpr DWORD COL_DEADBAR  = 0xFF35383D;
constexpr DWORD COL_BLACKBOX = 0xFF050608;
constexpr DWORD COL_READY    = 0xFF59C97A;

// ------------------------------------------------------------------ engine IO
const char* eng_cvar(const char* name) {
    static std::unordered_map<std::string, void*> s_cache;
    void* cv = nullptr;
    auto it = s_cache.find(name);
    if (it != s_cache.end()) cv = it->second;
    else {
        cv = ((Cvar_FindVar_t)VA_CVAR_FINDVAR)(name);
        if (cv) s_cache.emplace(name, cv);          // cvar_t never moves once created
    }
    if (!cv) return "";
    const char* s = *(const char* const*)((const char*)cv + 4);
    return s ? s : "";
}
int eng_read(const char* path, void** buf) {
    *buf = nullptr;
    int f = 0;
    *(volatile int*)VA_FS_OPENFLAG = 1;
    const int len = ((FS_FOpenFileRead_t)VA_FS_OPENREAD)(path, &f, 0, 0);
    if (!f) return -1;
    void* p = len > 0 ? malloc((size_t)len) : nullptr;
    int got = 0;
    if (p) got = ((FS_Read_t)VA_FS_READ)(p, len, f);
    ((FS_FCloseFile_t)VA_FS_CLOSE)(f);
    if (!p || got != len) { free(p); return -1; }
    *buf = p;
    return len;
}
void eng_free(void* buf) { free(buf); }
bool eng_camera(float* fx, float* fy, float org[3], float axis[9]) {
    const uintptr_t cg = (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll");
    if (!cg) return false;
    *fx = *(const float*)(cg + RVA_REF_FOVX);
    *fy = *(const float*)(cg + RVA_REF_FOVY);
    memcpy(org, (const void*)(cg + RVA_REF_ORG), 12);
    memcpy(axis, (const void*)(cg + RVA_REF_AXIS), 36);
    return true;
}
void eng_command(const char* text) { ((Cbuf_ExecuteText_t)VA_CBUF_EXECTEXT)(2, text); }

// Exclusive fullscreen at a 4:3 resolution on a wider panel: the display mode is the
// game's own (1440x1080) while the panel's registry mode keeps its native shape (1920x1080),
// and the GPU / monitor stretches the picture to fill the panel. Borderless or windowed
// leaves the desktop mode alone: no stretch.
float eng_stretch(float vw, float vh) {
    static DWORD s_at = 0;
    static float s_k = 1;
    static int s_w = 0, s_h = 0;
    const DWORD now = GetTickCount();
    if (now - s_at < 2000 && s_w == (int)vw && s_h == (int)vh) return s_k;
    s_at = now; s_w = (int)vw; s_h = (int)vh;
    float k = 1;
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(mi);
    DEVMODEA cur, reg;
    memset(&cur, 0, sizeof(cur)); memset(&reg, 0, sizeof(reg));
    cur.dmSize = reg.dmSize = sizeof(DEVMODEA);
    HMONITOR mon = MonitorFromWindow(overlay_game_window(), MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoA(mon, &mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_REGISTRY_SETTINGS, &reg) &&
        (int)cur.dmPelsWidth == s_w && (int)cur.dmPelsHeight == s_h && reg.dmPelsHeight > 0 && vh > 0) {
        const float panel = (float)reg.dmPelsWidth / reg.dmPelsHeight, game = vw / vh;
        if (panel > game * 1.05f) k = panel / game;
    }
    if (fabsf(k - s_k) > 0.01f)
        logger::logf("streamer_hud: the display shows the game %.2fx wider (%dx%d on a %lux%lu panel): overlay drawn narrower",
                     k, s_w, s_h, reg.dmPelsWidth, reg.dmPelsHeight);
    s_k = k;
    return s_k;
}

const StreamerHudIO g_engine_io = { eng_cvar, eng_read, eng_free, eng_camera, eng_command, eng_stretch, false };
const StreamerHudIO* g_io = &g_engine_io;

bool engine_ready() {
    return (uintptr_t)GetModuleHandleA(NULL) == 0x400000 && *(volatile int*)VA_CVAR_COUNT > 0;
}

// strips ^N colour codes; *had_red set when a ^1 was there
std::string clean(const char* s, bool* had_red = nullptr) {
    std::string o;
    if (had_red) *had_red = false;
    for (; *s; ++s) {
        if (s[0] == '^' && s[1] && s[1] != '^') {
            if (had_red && s[1] == '1') *had_red = true;
            ++s;
            continue;
        }
        o += *s;
    }
    return o;
}

std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

DWORD with_alpha(DWORD c, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return ((DWORD)(((c >> 24) & 0xff) * a) << 24) | (c & 0xFFFFFF);
}

DWORD mix(DWORD a, DWORD b, float t) {
    DWORD o = 0;
    for (int s = 0; s < 32; s += 8) {
        const float x = (float)((a >> s) & 0xff), y = (float)((b >> s) & 0xff);
        o |= (DWORD)(x + (y - x) * t + 0.5f) << s;
    }
    return o;
}

// ------------------------------------------------------------------- data
struct Slot {
    bool present = false, alive = false, followed = false, low = false;
    int  team = 0;            // 1 allies, 2 axis, 0 unknown (dead players carry none)
    int  hp = 0, nades = 0;
    int  ready = -1;          // ready-up marker: -1 none, 0 not ready, 1 ready
    std::string name, weapon_text;
};

struct FeedPlayer {
    bool present = false, alive = false, visible = false;
    float x = 0, y = 0, z = 0, yaw = 0;
    char stance = 's';
    int kills = 0;
    std::string weapon;       // weapon file name ("kar98k_mp"), "" unknown
};

struct Feed {
    bool valid = false, radar = false, xray = false, bomb = false;
    int  t = -1, cam = -1;
    float bx = 0, by = 0, bz = 0;
    char clock_kind = 0;      // 'r' round, 's' strat, 'b' bomb, 0 none
    int  clock_tenths = 0;
    FeedPlayer p[10];
};

struct RadarWin {             // the PAM's radar window: bombsites (and a fallback frame)
    bool valid = false;
    float cx = 0, cy = 0, sx = 1, sy = 1;
    bool site[2] = { false, false };
    float site_x[2] = { 0, 0 }, site_y[2] = { 0, 0 };
};

Slot     g_slot[10];
Feed     g_feed;
RadarWin g_win;
std::string g_name[2], g_score[2], g_info;
bool     g_info_red = false;
int      g_team_side[2] = { 2, 1 };   // team shown on the left / right
DWORD    g_clock_at = 0;              // arrival of the current clock value

// smoothing / extrapolation of the positions (feed arrives at 10 Hz)
struct Track {
    bool  init = false;
    float x = 0, y = 0, z = 0;          // last sample
    float vx = 0, vy = 0, vz = 0;       // units per second
    DWORD at = 0;                       // arrival of the last sample
    float rx = 0, ry = 0;               // radar: rendered position (smoothed)
};
Track g_track[10];

std::string g_last_feed_raw, g_last_win_raw;
DWORD g_feed_changed_at = 0;
int   g_last_t = -1;
DWORD g_last_frame = 0;
float g_dt = 0.016f;

int slot_team(int k) {
    return g_slot[k].team ? g_slot[k].team : g_team_side[k / 5];
}
DWORD team_col(int team) { return team == 2 ? COL_AXIS : COL_ALLIES; }

void read_slots() {
    char name[96];
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 5; ++i) {
            Slot& s = g_slot[side * 5 + i];
            snprintf(name, sizeof(name), "ui_streamersystem_team%d_player%d_health", side + 1, i);
            const char* health = g_io->cvar(name);
            s = Slot();
            if (!health[0]) continue;
            s.present = true;
            s.alive = health[0] != '0';
            s.team = strstr(health, "allies") ? 1 : strstr(health, "axis") ? 2 : 0;
            s.followed = health[strlen(health) - 1] == '_';
            snprintf(name, sizeof(name), "ui_streamersystem_team%d_player%d_name", side + 1, i);
            const char* raw = g_io->cvar(name);
            if (!strncmp(raw, "^2o^7 ", 6)) { s.ready = 1; raw += 6; }
            else if (!strncmp(raw, "^1o^7 ", 6)) { s.ready = 0; raw += 6; }
            s.name = clean(raw);
            snprintf(name, sizeof(name), "ui_streamersystem_team%d_player%d_hptext", side + 1, i);
            s.hp = atoi(clean(g_io->cvar(name), &s.low).c_str());
            snprintf(name, sizeof(name), "ui_streamersystem_team%d_player%d_weapon", side + 1, i);
            s.weapon_text = clean(g_io->cvar(name));
            snprintf(name, sizeof(name), "ui_streamersystem_team%d_player%d_icons", side + 1, i);
            const char* icons = g_io->cvar(name);
            s.nades = !strncmp(icons, "grenade2", 8) ? 2 : !strncmp(icons, "grenade", 7) ? 1 : 0;
        }
        // team of each side: from its living players (dead ones carry no team)
        for (int i = 0; i < 5; ++i)
            if (g_slot[side * 5 + i].team) { g_team_side[side] = g_slot[side * 5 + i].team; break; }
    }
    if (g_team_side[0] == g_team_side[1]) g_team_side[1] = 3 - g_team_side[0];
    for (int k = 0; k < 2; ++k) {
        g_name[k] = clean(g_io->cvar(k ? "ui_streamersystem_top_name2" : "ui_streamersystem_top_name1"));
        g_score[k] = clean(g_io->cvar(k ? "ui_streamersystem_top_score2" : "ui_streamersystem_top_score1"));
    }
    g_info = clean(g_io->cvar("ui_streamersystem_top_info"), &g_info_red);
}

// Header fields start with a capital letter (T R X C B E ...), slots with a digit or "-"
// (see _streamer_native.gsc): unknown fields are skipped, older PAMs still parse.
void parse_feed(const char* raw) {
    Feed f;
    std::string s = raw;
    size_t pos = 0;
    int slot = 0;
    while (pos <= s.size()) {
        size_t bar = s.find('|', pos);
        if (bar == std::string::npos) bar = s.size();
        const std::string tok = s.substr(pos, bar - pos);
        pos = bar + 1;
        const char c0 = tok.empty() ? 0 : tok[0];
        const char c1 = tok.size() > 1 ? tok[1] : 0;
        if (c0 >= 'A' && c0 <= 'Z') {
            switch (c0) {
            case 'T': f.t = atoi(tok.c_str() + 1); break;
            case 'R': f.radar = c1 == '1'; break;
            case 'X': f.xray = c1 == '1'; break;
            case 'C': f.cam = atoi(tok.c_str() + 1); break;
            case 'B':
                if (c1 && c1 != '-') f.bomb = sscanf(tok.c_str() + 1, "%f,%f,%f", &f.bx, &f.by, &f.bz) == 3;
                break;
            case 'E':
                if (c1 == 'r' || c1 == 's' || c1 == 'b') { f.clock_kind = c1; f.clock_tenths = atoi(tok.c_str() + 2); }
                break;
            }
        } else if (slot < 10) {
            FeedPlayer& p = f.p[slot++];
            if (tok != "-" && !tok.empty()) {
                int alive = 0, vis = 0, kills = 0;
                char st = 's';
                char weapon[64] = "";
                const int n = sscanf(tok.c_str(), "%d,%f,%f,%f,%f,%c,%d,%63[^,],%d", &alive, &p.x, &p.y, &p.z, &p.yaw, &st, &vis, weapon, &kills);
                if (n >= 6) {
                    p.present = true;
                    p.alive = alive != 0;
                    p.stance = st;
                    p.visible = vis != 0;
                    if (n >= 8 && strcmp(weapon, "-")) p.weapon = weapon;
                    if (n >= 9) p.kills = kills;
                }
            }
        }
        if (bar >= s.size()) break;
    }
    f.valid = f.t >= 0 && slot == 10;
    if (f.clock_kind != g_feed.clock_kind || f.clock_tenths != g_feed.clock_tenths) g_clock_at = GetTickCount();
    g_feed = f;

    // a new sample for every living player: velocity for the extrapolation
    const DWORD now = GetTickCount();
    for (int k = 0; k < 10; ++k) {
        Track& t = g_track[k];
        const FeedPlayer& p = g_feed.p[k];
        if (!p.present) { t.init = false; continue; }
        if (!t.init || !p.alive) {
            t.init = true; t.x = p.x; t.y = p.y; t.z = p.z;
            t.vx = t.vy = t.vz = 0; t.at = now; t.rx = p.x; t.ry = p.y;
            continue;
        }
        float dt = (now - t.at) / 1000.0f;
        if (dt < 0.05f) dt = 0.05f;
        const float dx = p.x - t.x, dy = p.y - t.y, dz = p.z - t.z;
        if (dx * dx + dy * dy > 400.0f * 400.0f) {          // teleport / respawn: no velocity
            t.vx = t.vy = t.vz = 0; t.rx = p.x; t.ry = p.y;
        } else if (dt < 0.5f) {
            t.vx = dx / dt; t.vy = dy / dt; t.vz = dz / dt;
        } else {
            t.vx = t.vy = t.vz = 0;
        }
        t.x = p.x; t.y = p.y; t.z = p.z; t.at = now;
    }
}

void predicted(int k, float* x, float* y, float* z) {
    const Track& t = g_track[k];
    float age = (GetTickCount() - t.at) / 1000.0f;
    if (age > 0.15f) age = 0.15f;                          // never guess further than one sample and a half
    *x = t.x + t.vx * age; *y = t.y + t.vy * age; *z = t.z + t.vz * age;
}

void parse_window(const char* raw) {
    RadarWin w;
    char image[128] = "", ax[16], ay[16], bx[16], by[16];
    float up = 90;
    if (sscanf(raw, "%127s %f %f %f %f %f %15s %15s %15s %15s", image, &w.cx, &w.cy, &w.sx, &w.sy, &up, ax, ay, bx, by) >= 6) {
        w.valid = w.sx > 1 && w.sy > 1;
        const char* sx[2] = { ax, bx };
        const char* sy[2] = { ay, by };
        for (int k = 0; k < 2; ++k) {
            if (sx[k][0] && sx[k][0] != '-') {
                w.site[k] = true;
                w.site_x[k] = (float)atof(sx[k]);
                w.site_y[k] = (float)atof(sy[k]);
            }
        }
    }
    g_win = w;
}

// ------------------------------------------------------------------ game art
// A weapon's silhouette: the kill icon its weapon file names, out of the paks, turned
// into a flat white shape (the stock icons are shaded grey) cropped to its alpha.
struct Icon { bool tried = false, ok = false; float aspect = 1; std::string key; };
std::unordered_map<std::string, Icon> g_icons;

bool load_game_image(const char* path, std::vector<unsigned char>& rgba, int& w, int& h) {
    void* buf = nullptr;
    const int len = g_io->read_file(path, &buf);
    if (len <= 0 || !buf) return false;
    const bool ok = game_image_decode(path, (const unsigned char*)buf, len, rgba, w, h);
    g_io->free_file(buf);
    return ok;
}

// "gfx/hud/hud@death_kar98.tga" -> tries .dds then .tga (the engine does the same)
bool load_icon_art(const std::string& path, std::vector<unsigned char>& rgba, int& w, int& h) {
    std::string base = path;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && base.find('/', dot) == std::string::npos) base.resize(dot);
    return load_game_image((base + ".dds").c_str(), rgba, w, h) || load_game_image((base + ".tga").c_str(), rgba, w, h);
}

const Icon& icon_from_art(const std::string& key, const std::string& art_path) {
    Icon& ic = g_icons[key];
    if (ic.tried && ui_tex_known(ic.key.c_str())) return ic;
    ic.tried = true;
    ic.key = "hud:" + key;
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (art_path.empty() || !load_icon_art(art_path, px, w, h)) {
        ui_tex_upload(ic.key.c_str(), 0, 0, nullptr, false, false);
        logger::logf("streamer_hud: icon %s (%s) not found", key.c_str(), art_path.c_str());
        return ic;
    }
    // crop to the shape, flat white keeping a little of the shading as alpha
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (px[((size_t)y * w + x) * 4 + 3] > 24) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    if (x1 < x0) { ui_tex_upload(ic.key.c_str(), 0, 0, nullptr, false, false); return ic; }
    const int cw = x1 - x0 + 3, ch = y1 - y0 + 3;           // one clear pixel all around
    std::vector<unsigned char> out((size_t)cw * ch * 4, 0);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const int sx = x0 + x - 1, sy = y0 + y - 1;
            unsigned char* d = &out[((size_t)y * cw + x) * 4];
            d[0] = d[1] = d[2] = 255;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            const unsigned char* s = &px[((size_t)sy * w + sx) * 4];
            const float lum = (s[0] * 0.3f + s[1] * 0.59f + s[2] * 0.11f) / 255.0f;
            d[3] = (unsigned char)(s[3] * (0.72f + 0.28f * lum));
        }
    ic.ok = ui_tex_upload(ic.key.c_str(), cw, ch, out.data(), false, true);
    ic.aspect = (float)cw / ch;
    return ic;
}

const Icon& weapon_icon(const std::string& weapon) {
    auto it = g_icons.find("wpn:" + weapon);
    if (it != g_icons.end() && it->second.tried && ui_tex_known(it->second.key.c_str())) return it->second;
    std::string art;
    void* buf = nullptr;
    const std::string file = "weapons/mp/" + weapon;
    const int len = g_io->read_file(file.c_str(), &buf);
    if (len > 0 && buf) {
        // "\\killIcon\\gfx/hud/hud@death_kar98.tga\\..." (backslash-separated key/value pairs)
        const std::string txt((const char*)buf, (size_t)len);
        g_io->free_file(buf);
        const size_t k = txt.find("\\killIcon\\");
        if (k != std::string::npos) {
            const size_t a = k + 10, b = txt.find('\\', a);
            art = txt.substr(a, b == std::string::npos ? std::string::npos : b - a);
        }
    }
    return icon_from_art("wpn:" + weapon, art);
}

// draws an icon fitted in (x, y, w, h), aligned left (-1), centre (0) or right (1)
float draw_icon(const Icon& ic, float x, float y, float w, float h, int align, DWORD col) {
    if (!ic.ok) return 0;
    float iw = h * ic.aspect, ih = h;
    if (iw > w) { iw = w; ih = w / ic.aspect; }
    const float ix = align < 0 ? x : align > 0 ? x + w - iw : x + (w - iw) / 2;
    ui_tex_draw(ic.key.c_str(), ix, y + (h - ih) / 2, iw, ih, 0, 0, 1, 1, col);
    return iw;
}

// ------------------------------------------------------------------ radar map
// Built by ui/radar_map.cpp from the BSP, on a worker thread (seconds of CPU on big maps).
struct RadarJob {
    std::vector<unsigned char> bsp;
    int max_w = 0, max_h = 0;
    RadarMap result;
    volatile LONG done = 0;
};
RadarJob* g_job = nullptr;
std::string g_job_map;
std::string g_radar_map;          // map of the current texture
int   g_radar_w = 0, g_radar_h = 0, g_radar_req_w = 0, g_radar_req_h = 0;
bool  g_radar_ok = false;
RadarMap g_radar;                 // rect + sites of the texture (pixels dropped once uploaded)
DWORD g_radar_failed_at = 0;

DWORD WINAPI radar_worker(LPVOID p) {
    RadarJob* j = (RadarJob*)p;
    radar_map_build(j->bsp.data(), (int)j->bsp.size(), j->max_w, j->max_h, j->result);
    InterlockedExchange(&j->done, 1);
    return 0;
}

void radar_collect(RadarJob* j) {
    g_radar = std::move(j->result);
    g_radar_map = g_job_map;
    g_radar_req_w = j->max_w; g_radar_req_h = j->max_h;
    g_radar_ok = g_radar.ok && ui_tex_upload("radar:map", g_radar.w, g_radar.h, g_radar.rgba.data(), false, false);
    if (g_radar_ok) {
        g_radar_w = g_radar.w; g_radar_h = g_radar.h;
        logger::logf("streamer_hud: radar of %s drawn from its BSP: %dx%d px, world %.0f x %.0f",
                     g_radar_map.c_str(), g_radar.w, g_radar.h, g_radar.span_x, g_radar.span_y);
    } else {
        g_radar_failed_at = GetTickCount();
        logger::logf("streamer_hud: radar of %s: %s", g_radar_map.c_str(), g_radar.error.empty() ? "upload failed" : g_radar.error.c_str());
    }
    g_radar.rgba.clear();
    g_radar.rgba.shrink_to_fit();
    delete j;
}

// Keeps a radar texture of the current map at (about) the size it is shown at.
void radar_request(int max_w, int max_h) {
    if (g_job) {
        if (!InterlockedCompareExchange(&g_job->done, 0, 0)) return;   // still working
        RadarJob* j = g_job;
        g_job = nullptr;
        radar_collect(j);
    }
    const std::string map = g_io->cvar("mapname");
    if (map.empty()) return;
    const bool same_map = map == g_radar_map;
    if (same_map && !g_radar_ok && GetTickCount() - g_radar_failed_at < 30000) return;
    if (same_map && g_radar_ok && ui_tex_known("radar:map") &&
        abs(max_w - g_radar_req_w) < 12 && abs(max_h - g_radar_req_h) < 12) return;
    // the texture is gone (vid_restart) or the size changed a lot: build again
    const std::string path = "maps/mp/" + map + ".bsp";
    void* buf = nullptr;
    const int len = g_io->read_file(path.c_str(), &buf);
    RadarJob* j = new RadarJob();
    if (len > 0 && buf) {
        j->bsp.assign((const unsigned char*)buf, (const unsigned char*)buf + len);
        g_io->free_file(buf);
    }
    j->max_w = max_w; j->max_h = max_h;
    g_job_map = map;
    if (j->bsp.empty()) {
        j->result.error = path + " not readable";
        radar_collect(j);
        return;
    }
    if (g_io->sync) { radar_worker(j); radar_collect(j); return; }
    HANDLE th = CreateThread(nullptr, 0, radar_worker, j, 0, nullptr);
    if (!th) { radar_worker(j); radar_collect(j); return; }
    CloseHandle(th);
    g_job = j;
}

// --------------------------------------------------------------- primitives
float S = 1.0f;                   // scale: window height / 1080
int   px(float v) { return std::max(8, (int)(v * S + 0.5f)); }

// the kit's panel: vertical charcoal gradient, 1 px highlight on the top edge
void panel(float x, float y, float w, float h, float r, DWORD top = COL_PANEL_T, DWORD bottom = COL_PANEL_B) {
    ui_aa_rect_vgrad(x, y, w, h, r, top, bottom);
    ui_rect(x + r, y, w - 2 * r, 1, with_alpha(COL_EDGE, 0.9f));
}

// the kit's diagonal stripes (scoreboard.png), screen-anchored so the cards line up
void stripes(float x, float y, float w, float h, float alpha) {
    if (!ui_tex_known("hud:stripes")) {
        const int N = 12;                                  // 45 degrees, period 12 px
        unsigned char tex[N * N * 4];
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const float d = fmodf((float)(i + j) + 0.5f, (float)N) / N;     // 0..1 across a period
                const float e = std::min(fabsf(d - 0.25f), fabsf(d - 0.75f)) * N; // px from a stripe edge
                const float on = d < 0.25f || d > 0.75f ? 1.0f : 0.0f;
                const float soft = std::min(1.0f, e);                              // 1 px feather
                const float a = on * soft + (1 - on) * (1 - soft) * 0.0f;
                unsigned char* p = &tex[(j * N + i) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = (unsigned char)(a * 255);
            }
        ui_tex_upload("hud:stripes", N, N, tex, true, false);
    }
    const float N = 12;
    ui_tex_draw("hud:stripes", x, y, w, h, x / N, y / N, (x + w) / N, (y + h) / N, with_alpha(0xFFFFFFFF, alpha));
}

// letter-spaced small caps (the kit's labels: "EN VIE 5/5", "ROUND 1")
float label(float x, float y, float size, DWORD col, const char* s, int align = -1, int weight = 700) {
    const float tr = 1.6f * S;
    const float w = ui_text_tracked_width(px(size), weight, s, tr, UI_FONT_MONO);
    const float x0 = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    ui_text_tracked(x0, y, px(size), weight, col, s, tr, UI_FONT_MONO);
    return w;
}

float text(float x, float y, float size, int weight, DWORD col, const char* s, int align = -1, int font = UI_FONT_SANS) {
    const float w = ui_text_font_width(px(size), weight, s, font);
    const float x0 = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    ui_text_font(x0, y, px(size), weight, col, s, font);
    return w;
}

// type drawn with its box's vertical centre at cy
float text_mid(float x, float cy, float size, int weight, DWORD col, const char* s, int align = -1, int font = UI_FONT_SANS) {
    const float h = ui_text_font_height(px(size), weight, font);
    return text(x, cy - h / 2, size, weight, col, s, align, font);
}

// vector glyphs: crisp at the few pixels a card has (the stock icons are 32 px photos)
void glyph_kill(float cx, float cy, float r, DWORD c) {       // a crosshair
    ui_aa_ring(cx, cy, r * 0.62f, std::max(1.0f, r * 0.24f), c);
    const float a = r * 0.25f, b = r * 1.05f, th = std::max(1.0f, r * 0.22f);
    ui_aa_line(cx - b, cy, cx - a, cy, th, c);
    ui_aa_line(cx + a, cy, cx + b, cy, th, c);
    ui_aa_line(cx, cy - b, cx, cy - a, th, c);
    ui_aa_line(cx, cy + a, cx, cy + b, th, c);
}
void glyph_nade(float cx, float cy, float s, DWORD c) {       // a frag grenade, s = height
    const float r = s * 0.34f;
    ui_aa_disc(cx, cy + s * 0.14f, r, c);
    ui_aa_rect(cx - s * 0.13f, cy - s * 0.42f, s * 0.26f, s * 0.2f, 0.5f, c);           // fuse cap
    ui_aa_line(cx + s * 0.1f, cy - s * 0.36f, cx + s * 0.34f, cy - s * 0.06f, std::max(1.0f, s * 0.1f), c);   // lever
}

// ------------------------------------------------------------------- top bar
int alive_count(int side) {
    int n = 0;
    for (int i = 0; i < 5; ++i) if (g_slot[side * 5 + i].present && g_slot[side * 5 + i].alive) ++n;
    return n;
}
int present_count(int side) {
    int n = 0;
    for (int i = 0; i < 5; ++i) if (g_slot[side * 5 + i].present) ++n;
    return n;
}

void draw_card(float x, float y, float w, float h, int k) {
    const Slot& s = g_slot[k];
    const FeedPlayer& f = g_feed.p[k];
    const float r = 2 * S;
    if (!s.present) {
        ui_aa_rect(x, y, w, h, r, 0x6614171A);
        stripes(x, y, w, h, 0.03f);
        return;
    }
    const int team = slot_team(k);
    const DWORD tc = team_col(team);
    const bool followed = s.followed || g_feed.cam == k;

    ui_shadow(x, y + 2 * S, w, h, r, 10 * S, 0x70000000);
    if (followed) {
        const float pulse = 0.55f + 0.45f * sinf(GetTickCount() * 0.005f);
        ui_shadow(x, y, w, h, r, 14 * S, with_alpha(tc, 0.45f * pulse));
    }
    if (s.alive) panel(x, y, w, h, r, followed ? 0xFF2B2F34 : COL_PANEL_T, COL_PANEL_B);
    else         panel(x, y, w, h, r, 0xFF16181B, 0xFF0F1113);
    stripes(x, y, w, h, s.alive ? 0.035f : 0.02f);
    ui_rect(x, y, w, 3 * S, s.alive ? tc : COL_DEADBAR);                // team stripe on top
    if (followed) ui_aa_stroke_rect(x - 0.5f, y - 0.5f, w + 1, h + 1, r, 1.6f * S, with_alpha(0xFFFFFFFF, 0.9f));

    const float pad = 9 * S;
    const float dim = s.alive ? 1.0f : 0.45f;

    // row 1: ready dot, name (the whole width: the HP goes with its bar)
    float nx = x + pad;
    if (s.ready >= 0) {
        ui_aa_disc(nx + 3 * S, y + 17 * S, 3 * S, s.ready ? COL_READY : COL_DANGER);
        nx += 10 * S;
    }
    ui_clip_push(nx, y, x + w - pad - nx, h);
    text_mid(nx, y + 17 * S, 13.5f, 600, with_alpha(s.alive ? COL_TEXT : COL_MUTED, s.alive ? 1.0f : 0.8f), s.name.c_str());
    ui_clip_pop();

    // row 2: kills this round + grenades (left) ............ weapon silhouette (right)
    const float row2 = y + 38 * S;
    float lx = x + pad;
    if (f.kills > 0) {
        glyph_kill(lx + 5.5f * S, row2, 5.5f * S, with_alpha(COL_TEXT, dim));
        char kc[8];
        snprintf(kc, sizeof(kc), "%d", f.kills);
        lx += 13 * S;
        lx += text_mid(lx, row2, 12.5f, 800, with_alpha(COL_TEXT, dim), kc) + 7 * S;
    }
    if (s.alive) {
        for (int n = 0; n < s.nades; ++n) {
            glyph_nade(lx + 4.5f * S, row2 + 0.5f * S, 13 * S, 0xE6C9CED4);
            lx += 11 * S;
        }
        if (s.nades) lx += 3 * S;
    }
    if (s.alive) {
        if (!f.weapon.empty()) {
            const Icon& wi = weapon_icon(f.weapon);
            draw_icon(wi, lx + 4 * S, row2 - 10 * S, x + w - pad - lx - 4 * S, 20 * S, 1, with_alpha(0xFFFFFFFF, 0.92f));
        } else if (!s.weapon_text.empty()) {
            label(x + w - pad, row2 - 5 * S, 9, COL_MUTED, upper(s.weapon_text).c_str(), 1, 600);
        }
    } else {
        label(x + w - pad, row2 - 5 * S, 9.5f, 0xFF5A5F65, "MORT", 1);
    }

    // row 3: key badge, HP, HP bar
    const float badge = 15 * S, by = y + h - badge - 5 * S;
    ui_aa_rect(x + 5 * S, by, badge, badge, 1.5f * S, 0xFF0A0C0E);
    char key[4];
    snprintf(key, sizeof(key), "%d", (k + 1) % 10);
    text_mid(x + 5 * S + badge / 2, by + badge / 2, 10, 700, with_alpha(COL_MUTED, dim), key, 0, UI_FONT_MONO);
    if (s.alive) {
        char hp[8];
        snprintf(hp, sizeof(hp), "%d", s.hp);
        const float hx = x + 5 * S + badge + 6 * S;
        const float hw = ui_text_font_width(px(14), 800, "100", UI_FONT_SANS);
        text_mid(hx + hw, by + badge / 2, 14, 800, s.hp < 30 ? COL_DANGER : COL_TEXT, hp, 1);
        const float bx = hx + hw + 6 * S, bw = x + w - pad - bx, bh = 4 * S;
        const float byy = by + (badge - bh) / 2;
        const float shown = ui_smooth(0x57A0 + k, (float)s.hp, 9.0f);
        ui_rect(bx, byy, bw, bh, 0x40000000);
        ui_rect(bx, byy, bw, bh, 0x1AFFFFFF);
        float fr = std::min(1.0f, std::max(0.0f, shown / 100.0f));
        const DWORD fill = s.hp < 30 ? COL_DANGER : tc;
        if (fr > 0) {
            ui_rect_hgrad(bx, byy, bw * fr, bh, mix(fill, 0xFF000000, 0.25f), fill);
            ui_rect(bx, byy, bw * fr, 1, with_alpha(0xFFFFFFFF, 0.25f));
        }
    }
}

// the clock: tenths of a second left, extrapolated since the feed said so
int clock_tenths_now() {
    if (!g_feed.clock_kind) return -1;
    const int gone = (int)((GetTickCount() - g_clock_at) / 100);
    return std::max(0, g_feed.clock_tenths - std::min(gone, 20));   // at most 2 s of guessing
}

// returns the bottom of the whole top bar
float draw_topbar(float vw) {
    const float top = 10 * S;
    const float cw_top = 150 * S, cw_bot = 178 * S, ch = 112 * S;           // clock block (trapezoid)
    const float gap = 6 * S, cgap = 10 * S, margin = 12 * S;
    float card_w = 150 * S;
    const float card_h = 74 * S;
    const float avail = vw - 2 * margin - cw_bot - 2 * cgap - 8 * gap;
    if (card_w * 10 > avail) card_w = std::max(96 * S, avail / 10);
    const float cx = vw / 2;
    const float row_w = 5 * card_w + 4 * gap;
    const float left_x = cx - cw_bot / 2 - cgap - row_w;
    const float right_x = cx + cw_bot / 2 + cgap;

    for (int i = 0; i < 5; ++i) draw_card(left_x + i * (card_w + gap), top, card_w, card_h, i);
    for (int j = 0; j < 5; ++j) draw_card(right_x + j * (card_w + gap), top, card_w, card_h, 5 + j);

    // team names + players alive, under each row
    for (int side = 0; side < 2; ++side) {
        const int team = g_team_side[side];
        const DWORD tc = team_col(team);
        std::string nm = upper(g_name[side].empty() ? (team == 2 ? "AXIS" : "ALLIES") : g_name[side]);
        char alive[24];
        snprintf(alive, sizeof(alive), "EN VIE %d/%d", alive_count(side), std::max(present_count(side), alive_count(side)));
        const float y = top + card_h + 7 * S;
        {
            const float w = ui_text_tracked_width(px(12.5f), 700, nm.c_str(), 1.6f * S, UI_FONT_MONO) +
                            ui_text_tracked_width(px(10), 700, alive, 1.6f * S, UI_FONT_MONO) + 40 * S;
            const float x0 = side == 0 ? left_x : right_x + row_w - w;
            ui_aa_rect(x0 - 2 * S, y - 3 * S, w + 4 * S, 19 * S, 2 * S, 0xC80D0F11);
        }
        if (side == 0) {
            float x = left_x + 2 * S;
            ui_rect(x, y + 1 * S, 3 * S, 13 * S, tc);
            x += 9 * S;
            x += label(x, y, 12.5f, tc, nm.c_str()) + 12 * S;
            label(x, y + 1.5f * S, 10, 0xFF7A7F85, alive);
        } else {
            float x = right_x + row_w - 2 * S;
            ui_rect(x - 3 * S, y + 1 * S, 3 * S, 13 * S, tc);
            x -= 9 * S;
            x -= label(x, y, 12.5f, tc, nm.c_str(), 1) + 12 * S;
            label(x, y + 1.5f * S, 10, 0xFF7A7F85, alive, 1);
        }
    }

    // the clock block
    const bool bomb = g_feed.clock_kind == 'b' || (!g_feed.clock_kind && g_info_red);
    const float y0 = top - 10 * S, y1 = top + ch - 10 * S;
    const float poly[8] = { cx - cw_top / 2, y0, cx + cw_top / 2, y0, cx + cw_bot / 2, y1, cx - cw_bot / 2, y1 };
    ui_shadow(cx - cw_bot / 2, y0, cw_bot, y1 - y0, 0, 12 * S, 0x80000000);
    if (bomb) ui_aa_poly_vgrad(poly, 4, 0xFF3C1616, 0xFF1C0B0B);
    else      ui_aa_poly_vgrad(poly, 4, 0xFF1F2226, 0xFF0B0D10);
    ui_aa_line(cx - cw_top / 2 + 1, y0 + 0.5f, cx + cw_top / 2 - 1, y0 + 0.5f, 1, bomb ? 0xFF6B2A2A : COL_EDGE);

    // label: dot + ROUND n / STRAT TIME / BOMBE / READY-UP...
    std::string lab = upper(g_info);
    if (g_feed.clock_kind == 'b' || lab == "BOMB PLANTED") lab = "BOMBE";
    else if (g_feed.clock_kind == 's') lab = "STRAT TIME";
    if (lab.empty()) lab = "LIVE";
    const float ly = top + 1 * S;
    {
        const float lw = ui_text_tracked_width(px(10), 700, lab.c_str(), 1.6f * S, UI_FONT_MONO);
        const float dot = 3 * S, total = dot * 2 + 7 * S + lw;
        const float lx0 = cx - total / 2;
        const float pulse = 0.5f + 0.5f * sinf(GetTickCount() * 0.008f);
        const DWORD dc = bomb ? with_alpha(COL_DANGER, 0.55f + 0.45f * pulse) : 0xFF6E7378;
        ui_aa_disc(lx0 + dot, ly + 7 * S, dot, dc);
        label(lx0 + dot * 2 + 7 * S, ly + 1 * S, 10, bomb ? COL_DANGER : 0xFF8A8F95, lab.c_str());
    }

    // timer box
    const float tb_w = 108 * S, tb_h = 36 * S, tb_y = top + 18 * S;
    ui_aa_rect(cx - tb_w / 2, tb_y, tb_w, tb_h, 2 * S, bomb ? 0xFF120606 : COL_BLACKBOX);
    const int tenths = clock_tenths_now();
    if (tenths >= 0) {
        char t[16];
        const int secs = (tenths + 9) / 10;
        if (g_feed.clock_kind == 'b' && tenths < 100) snprintf(t, sizeof(t), "%d.%d", tenths / 10, tenths % 10);
        else snprintf(t, sizeof(t), "%d:%02d", secs / 60, secs % 60);
        const DWORD c = bomb ? 0xFFFF5F55 : (g_feed.clock_kind == 'r' && tenths < 100 ? COL_DANGER : COL_TEXT);
        text_mid(cx, tb_y + tb_h / 2, 27, 700, c, t, 0);
    } else {
        text_mid(cx, tb_y + tb_h / 2, 22, 700, 0xFF4A4F55, "-:--", 0);
    }

    // scores: a box per team, team stripe on top, players alive as pips
    const float sb_y = tb_y + tb_h + 6 * S, sb_h = 44 * S, sb_gap = 6 * S;
    const float sb_w = (tb_w - sb_gap) / 2 + 12 * S;
    for (int side = 0; side < 2; ++side) {
        const DWORD tc = team_col(g_team_side[side]);
        const float bx = side == 0 ? cx - sb_gap / 2 - sb_w : cx + sb_gap / 2;
        ui_aa_rect_vgrad(bx, sb_y, sb_w, sb_h, 1.5f * S, 0xFF15181B, 0xFF0E1012);
        ui_rect(bx, sb_y, sb_w, 3 * S, tc);
        const char* sc = g_score[side].empty() ? "0" : g_score[side].c_str();
        text_mid(bx + sb_w / 2, sb_y + 19 * S, 25, 800, tc, sc, 0);
        // pips
        const int alive = alive_count(side), total = std::max(5, present_count(side));
        const float pw = 5 * S, pg = 2.5f * S, ph = 5 * S;
        const float pws = total * pw + (total - 1) * pg;
        float pxx = bx + (sb_w - pws) / 2;
        for (int n = 0; n < total; ++n, pxx += pw + pg)
            ui_rect(pxx, sb_y + sb_h - 9 * S, pw, ph, n < alive ? tc : with_alpha(tc, 0.18f));
    }
    return std::max(top + card_h + 24 * S, y1);
}

// ------------------------------------------------------------------- radar
struct RadarBox { float x, y, w, h; float wx0, wy0, wsx, wsy; };   // screen rect <- world rect

void radar_point(const RadarBox& b, float wx, float wy, float* sx, float* sy, bool* inside) {
    float x = b.x + (wx - b.wx0) / b.wsx * b.w;
    float y = b.y + (1 - (wy - b.wy0) / b.wsy) * b.h;
    bool in = true;
    const float m = 5 * S;
    if (x < b.x + m) { x = b.x + m; in = false; }
    if (x > b.x + b.w - m) { x = b.x + b.w - m; in = false; }
    if (y < b.y + m) { y = b.y + m; in = false; }
    if (y > b.y + b.h - m) { y = b.y + b.h - m; in = false; }
    *sx = x; *sy = y;
    if (inside) *inside = in;
}

void corner_brackets(float x, float y, float w, float h, float arm, float th, DWORD c) {
    ui_rect(x, y, arm, th, c);             ui_rect(x, y, th, arm, c);
    ui_rect(x + w - arm, y, arm, th, c);   ui_rect(x + w - th, y, th, arm, c);
    ui_rect(x, y + h - th, arm, th, c);    ui_rect(x, y + h - arm, th, arm, c);
    ui_rect(x + w - arm, y + h - th, arm, th, c); ui_rect(x + w - th, y + h - arm, th, arm, c);
}

void draw_radar(float top) {
    if (!g_feed.radar) return;
    const float pad = 12 * S;
    const float max_w = 372 * S, max_h = 340 * S;
    radar_request((int)(max_w - 2 * pad), (int)(max_h - 2 * pad - 14 * S));

    // world window: the client's own drawing, or the PAM's frame while it is being built
    RadarBox b;
    float img_w, img_h;
    if (g_radar_ok) {
        b.wx0 = g_radar.x0; b.wy0 = g_radar.y0; b.wsx = g_radar.span_x; b.wsy = g_radar.span_y;
        img_w = (float)g_radar_w; img_h = (float)g_radar_h;
    } else if (g_win.valid) {
        b.wx0 = g_win.cx - g_win.sx / 2; b.wy0 = g_win.cy - g_win.sy / 2; b.wsx = g_win.sx; b.wsy = g_win.sy;
        const float fit = std::min((max_w - 2 * pad) / g_win.sx, (max_h - 2 * pad - 14 * S) / g_win.sy);
        img_w = g_win.sx * fit; img_h = g_win.sy * fit;
    } else {
        return;
    }
    const float pw = img_w + 2 * pad, ph = img_h + 2 * pad + 14 * S;
    const float x = 14 * S, y = top;
    b.x = x + pad; b.y = y + pad; b.w = img_w; b.h = img_h;

    // panel: the kit's minimap - near-black, a fine grid, orange corners, N
    ui_shadow(x, y, pw, ph, 2 * S, 14 * S, 0x90000000);
    ui_aa_rect(x, y, pw, ph, 2 * S, 0xF00D1013);
    {
        ui_clip_push(x + 1, y + 1, pw - 2, ph - 2);
        const float step = 24 * S;
        for (float gx = x + fmodf(pw / 2, step); gx < x + pw; gx += step) ui_rect(floorf(gx), y, 1, ph, 0x0DFFFFFF);
        for (float gy = y + fmodf(ph / 2, step); gy < y + ph; gy += step) ui_rect(x, floorf(gy), pw, 1, 0x0DFFFFFF);
        ui_clip_pop();
    }
    if (g_radar_ok) ui_tex_draw("radar:map", b.x, b.y, b.w, b.h, 0, 0, 1, 1, 0xFFFFFFFF);
    corner_brackets(x, y, pw, ph, 14 * S, 2 * S, COL_AXIS);
    text(x + 9 * S, y + 6 * S, 12, 800, COL_AXIS, "N");
    {
        std::string m = g_io->cvar("mapname");
        if (m.rfind("mp_", 0) == 0) m = m.substr(3);
        label(x + 10 * S, y + ph - 18 * S, 9.5f, 0xFF7A7F85, upper(m).c_str());
    }

    // bombsites: the kit's objective diamond
    const char* letter[2] = { "A", "B" };
    for (int k = 0; k < 2; ++k) {
        bool have = g_radar_ok ? g_radar.site[k] : false;
        float wx = have ? g_radar.site_x[k] : 0, wy = have ? g_radar.site_y[k] : 0;
        if (g_win.valid && g_win.site[k]) { have = true; wx = g_win.site_x[k]; wy = g_win.site_y[k]; }
        if (!have) continue;
        float sx, sy;
        radar_point(b, wx, wy, &sx, &sy, nullptr);
        const float r = 10 * S;
        const float d[8] = { sx, sy - r, sx + r, sy, sx, sy + r, sx - r, sy };
        ui_aa_poly(d, 4, 0xE6241A0F);
        ui_aa_poly_stroke(d, 4, 1.5f * S, COL_AXIS);
        text_mid(sx, sy, 10.5f, 800, COL_TEXT, letter[k], 0);
    }

    // planted bomb
    if (g_feed.bomb) {
        float sx, sy;
        radar_point(b, g_feed.bx, g_feed.by, &sx, &sy, nullptr);
        const float t = fmodf(GetTickCount() / 900.0f, 1.0f);
        ui_aa_ring(sx, sy, (5 + 12 * t) * S, 2 * S, with_alpha(COL_DANGER, 1 - t));
        ui_aa_disc(sx, sy, 4.5f * S, COL_DANGER);
        ui_aa_ring(sx, sy, 4.5f * S, 1.2f * S, 0xFF1A0808);
    }

    const float follow = 1 - expf(-g_dt * 14.0f);
    // dead first (under), then the living, the followed one last (on top)
    for (int pass = 0; pass < 3; ++pass) {
        for (int k = 0; k < 10; ++k) {
            const FeedPlayer& p = g_feed.p[k];
            if (!p.present) continue;
            const bool followed = g_slot[k].followed || g_feed.cam == k;
            const int want = !p.alive ? 0 : followed ? 2 : 1;
            if (want != pass) continue;
            Track& t = g_track[k];
            float wx, wy, wz;
            predicted(k, &wx, &wy, &wz);
            t.rx += (wx - t.rx) * follow;
            t.ry += (wy - t.ry) * follow;
            float sx, sy;
            bool inside;
            radar_point(b, t.rx, t.ry, &sx, &sy, &inside);
            const DWORD tc = team_col(slot_team(k));
            if (!p.alive) {
                const float d = 3.5f * S;
                ui_aa_line(sx - d, sy - d, sx + d, sy + d, 2.4f * S, 0xB0000000);
                ui_aa_line(sx - d, sy + d, sx + d, sy - d, 2.4f * S, 0xB0000000);
                ui_aa_line(sx - d, sy - d, sx + d, sy + d, 1.3f * S, with_alpha(tc, 0.55f));
                ui_aa_line(sx - d, sy + d, sx + d, sy - d, 1.3f * S, with_alpha(tc, 0.55f));
                continue;
            }
            const float a = inside ? 1.0f : 0.55f;
            const float r = (followed ? 7.5f : 6.5f) * S;
            // view cone
            const float yaw = p.yaw * 3.14159265f / 180.0f;
            const float ex = cosf(yaw), ey = -sinf(yaw);                  // world +Y is screen up
            const float spread = 0.42f, len = (followed ? 30.0f : 24.0f) * S;
            float cone[2 * 7];
            cone[0] = sx; cone[1] = sy;
            for (int q = 0; q < 6; ++q) {
                const float ang = -spread + 2 * spread * q / 5;
                const float c = cosf(ang), s = sinf(ang);
                cone[2 + q * 2] = sx + (ex * c - ey * s) * len;
                cone[3 + q * 2] = sy + (ex * s + ey * c) * len;
            }
            ui_aa_poly(cone, 7, with_alpha(tc, (followed ? 0.30f : 0.20f) * a));
            ui_aa_disc(sx, sy, r + 1.6f * S, with_alpha(0xFF0A0C0E, 0.9f * a));
            ui_aa_disc(sx, sy, r, with_alpha(tc, a));
            char num[4];
            snprintf(num, sizeof(num), "%d", (k + 1) % 10);
            text_mid(sx, sy, r * 1.35f / S, 800, with_alpha(0xFF0B0D10, a), num, 0);
            if (followed) ui_aa_ring(sx, sy, r + 4.5f * S, 1.8f * S, 0xFFFFFFFF);
        }
    }
}

// ------------------------------------------------------------------- xray
struct XrayItem {
    int k;
    float depth, x0, y0, bw, bh, x_mid;
    float tx, ty, tw, th;         // tag rect
    bool visible;
};

void draw_xray(float vw, float vh) {
    if (!g_feed.xray || !g_io->camera) return;
    float fovx, fovy, org[3], ax[9];
    if (!g_io->camera(&fovx, &fovy, org, ax)) return;
    if (!(fovx > 1 && fovx < 179 && fovy > 1 && fovy < 179)) return;
    const float tx = tanf(fovx * 3.14159265f / 360.0f), ty = tanf(fovy * 3.14159265f / 360.0f);
    const int cam = g_feed.cam;
    const int cam_team = cam >= 0 && cam < 10 ? slot_team(cam) : 0;

    std::vector<XrayItem> items;
    for (int k = 0; k < 10; ++k) {
        const FeedPlayer& p = g_feed.p[k];
        if (!p.present || !p.alive || k == cam) continue;
        const int team = slot_team(k);
        if (cam_team && team == cam_team) continue;              // following: enemies only

        float wx, wy, wz;
        predicted(k, &wx, &wy, &wz);
        const float head = p.stance == 'p' ? 20.0f : p.stance == 'c' ? 50.0f : 70.0f;
        float pts[2][2];
        bool ok = true;
        float depth = 0;
        for (int e = 0; e < 2; ++e) {
            const float d[3] = { wx - org[0], wy - org[1], wz + (e ? head : 0) - org[2] };
            const float xf = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
            const float yl = d[0] * ax[3] + d[1] * ax[4] + d[2] * ax[5];
            const float zu = d[0] * ax[6] + d[1] * ax[7] + d[2] * ax[8];
            if (xf < 16) { ok = false; break; }
            pts[e][0] = vw / 2 - (yl / xf) / tx * (vw / 2);
            pts[e][1] = vh / 2 - (zu / xf) / ty * (vh / 2);
            if (!e) depth = xf;
        }
        if (!ok) continue;
        const float ppu = (vw / 2) / tx / depth;                  // pixels per world unit there
        XrayItem it;
        it.k = k; it.depth = depth; it.visible = p.visible;
        it.bw = std::max(6.0f, (p.stance == 'p' ? 60.0f : 30.0f) * ppu);
        it.x_mid = (pts[0][0] + pts[1][0]) / 2;
        const float top = pts[1][1] - 6 * ppu, bottom = pts[0][1];
        it.bh = std::max(8.0f, bottom - top);
        it.x0 = it.x_mid - it.bw / 2; it.y0 = top;
        if (it.x0 > vw || it.x0 + it.bw < 0 || it.y0 > vh || it.y0 + it.bh < 0) continue;
        const Slot& s = g_slot[k];
        char dist[16];
        snprintf(dist, sizeof(dist), "%d M", (int)(depth / 39.37f + 0.5f));
        const float nw = ui_text_font_width(px(12), 600, s.name.c_str(), UI_FONT_SANS);
        const float dw = ui_text_tracked_width(px(9), 700, dist, 1.6f * S, UI_FONT_MONO);
        it.tw = nw + dw + 22 * S; it.th = 20 * S;
        it.tx = it.x_mid - it.tw / 2; it.ty = it.y0 - it.th - 10 * S;
        items.push_back(it);
    }
    // tags never overlap: the nearest keeps its place, the others climb above it
    std::sort(items.begin(), items.end(), [](const XrayItem& a, const XrayItem& b) { return a.depth < b.depth; });
    for (size_t i = 0; i < items.size(); ++i) {
        for (int guard = 0; guard < 12; ++guard) {
            bool moved = false;
            for (size_t j = 0; j < i; ++j) {
                const XrayItem& o = items[j];
                XrayItem& m = items[i];
                const float g = 3 * S;
                if (m.tx < o.tx + o.tw + g && o.tx < m.tx + m.tw + g && m.ty < o.ty + o.th + 6 * S + g && o.ty < m.ty + m.th + 6 * S + g) {
                    m.ty = o.ty - m.th - 6 * S - g;
                    moved = true;
                }
            }
            if (!moved) break;
        }
    }
    // far first, near on top
    for (size_t n = items.size(); n-- > 0;) {
        const XrayItem& it = items[n];
        const Slot& s = g_slot[it.k];
        const DWORD tc = team_col(slot_team(it.k));
        const float a = it.visible ? 0.4f : 1.0f;                 // behind a wall: strong
        const float L = std::max(5.0f, std::min(it.bw, it.bh) * 0.28f), th = std::max(1.5f, 2.0f * S);
        const float cx[4] = { it.x0, it.x0 + it.bw, it.x0, it.x0 + it.bw }, cy[4] = { it.y0, it.y0, it.y0 + it.bh, it.y0 + it.bh };
        const float sx[4] = { 1, -1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
        for (int pass = 0; pass < 2; ++pass) {
            const DWORD col = pass ? with_alpha(tc, a) : with_alpha(0xFF000000, a * 0.55f);
            const float o = pass ? 0 : 1;
            for (int q = 0; q < 4; ++q) {
                ui_aa_line(cx[q] + o, cy[q] + o, cx[q] + sx[q] * L + o, cy[q] + o, th, col);
                ui_aa_line(cx[q] + o, cy[q] + o, cx[q] + o, cy[q] + sy[q] * L + o, th, col);
            }
        }
        // a thin leader when the tag had to climb
        if (it.ty + it.th + 10 * S < it.y0 - 2)
            ui_aa_line(it.x_mid, it.ty + it.th + 5 * S, it.x_mid, it.y0 - 3 * S, 1.0f, with_alpha(tc, a * 0.6f));
        char dist[16];
        snprintf(dist, sizeof(dist), "%d M", (int)(it.depth / 39.37f + 0.5f));
        ui_aa_rect(it.tx, it.ty, it.tw, it.th, 2 * S, with_alpha(0xFF101214, 0.82f * a));
        ui_rect(it.tx, it.ty, 2 * S, it.th, with_alpha(tc, a));
        text_mid(it.tx + 8 * S, it.ty + it.th / 2, 12, 600, with_alpha(COL_TEXT, a), s.name.c_str());
        label(it.tx + it.tw - 7 * S, it.ty + it.th / 2 - 6 * S, 9, with_alpha(tc, a), dist, 1);
        const float hy = it.ty + it.th + 2 * S;
        ui_rect(it.tx, hy, it.tw, 3 * S, with_alpha(0xFF000000, a * 0.6f));
        const float f = std::min(1.0f, std::max(0.0f, s.hp / 100.0f));
        ui_rect(it.tx, hy, it.tw * f, 3 * S, with_alpha(s.hp < 30 ? COL_DANGER : tc, a));
    }
}

bool g_active = false;

}  // namespace

void streamer_hud_set_io(const StreamerHudIO* io) { g_io = io ? io : &g_engine_io; }

bool streamer_hud_update() {
    const DWORD now = GetTickCount();
    g_dt = g_last_frame ? (now - g_last_frame) / 1000.0f : 0.016f;
    if (g_dt > 0.1f) g_dt = 0.1f;
    g_last_frame = now;

    const char* feed = g_io->cvar("ui_streamersystem_feed");
    if (g_last_feed_raw != feed) {
        g_last_feed_raw = feed;
        parse_feed(feed);
        if (g_feed.valid && g_feed.t != g_last_t) { g_last_t = g_feed.t; g_feed_changed_at = now; }
    }
    const char* win = g_io->cvar("ui_streamersystem_radarwin");
    if (g_last_win_raw != win) { g_last_win_raw = win; parse_window(win); }
    read_slots();
    return g_feed.valid && now - g_feed_changed_at < 3000;
}

bool streamer_hud_frame() {
    if (!engine_ready()) return false;

    // the streamer overlay is on screen only while the PAM streamer menu is open
    if (!ui_menu_visible(MENU_ROOT) && !ui_menu_visible(MENU_OVER)) {
        if (g_active) { g_active = false; logger::logf("streamer_hud: off (streamer menu closed)"); }
        return false;
    }

    const bool fresh = streamer_hud_update();
    if (!fresh) {
        // a PAM with the native feed shows its top bar: ask for the feed, every 3 s
        static DWORD s_asked = 0;
        const DWORD now = GetTickCount();
        const char* sid = g_io->cvar("sv_serverid");
        if (g_io->cvar("ui_streamersystem_top_name1")[0] && sid[0] && now - s_asked > 3000) {
            s_asked = now;
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "cmd mr %s -1 streamersystem_native\n", sid);
            g_io->command(cmd);
            static int s_logged = 0;
            if (s_logged++ < 3) logger::logf("streamer_hud: asked the server for the native feed (sv_serverid %s)", sid);
        }
        if (g_active) { g_active = false; logger::logf("streamer_hud: off (no fresh feed)"); }
        return false;
    }
    if (!g_active) { g_active = true; logger::logf("streamer_hud: on"); }

    // the PAM's own top bar is replaced
    ui_menu_hide(MENU_LEFT);
    ui_menu_hide(MENU_RIGHT);
    return true;
}

void streamer_hud_draw(float vw, float vh) {
    S = vh / 1080.0f;
    if (S < 0.55f) S = 0.55f;
    const float k = g_io->stretch ? g_io->stretch(vw, vh) : 1.0f;
    const bool squeeze = k > 1.02f && k < 3.0f;
    if (squeeze) {
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glScalef(1.0f / k, 1.0f, 1.0f);
        ui_clip_xscale(1.0f / k);
        vw *= k;                                  // lay out on the panel's shape
    }
    const float bottom = draw_topbar(vw);
    draw_xray(vw, vh);
    draw_radar(bottom + 12 * S);
    if (squeeze) {
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        ui_clip_xscale(1.0f);
    }
}

}  // namespace patches
