// minimap.cpp - see minimap.h
#include "ui/minimap.h"
#include "ui/gl_overlay.h"
#include "ui/ui_draw.h"
#include "ui/radar_map.h"
#include "ui/hud_editor.h"
#include "core/logger.h"

#include <GL/gl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace patches {

bool ruleset_active();                     // netcode/ruleset.cpp: a competitive ruleset is enforced

namespace {

// ---- engine (CoDMP.exe 1.5, base 0x400000) ----
constexpr uintptr_t VA_CVAR_FINDVAR  = 0x0043b790;   // cvar_t* Cvar_FindVar(name): string +4, value +0x1c
constexpr uintptr_t VA_CVAR_GET      = 0x0043b880;   // cvar_t* Cvar_Get(name, value, flags) (protocol_patch.h)
constexpr uintptr_t VA_CBUF_EXECTEXT = 0x0042a180;   // Cbuf_ExecuteText(when, text)
constexpr uintptr_t VA_FS_OPENREAD   = 0x0042bc70;   // see streamer_hud.cpp: no FS_ReadFile (hunk)
constexpr uintptr_t VA_FS_READ       = 0x0042c610;
constexpr uintptr_t VA_FS_CLOSE      = 0x0042b600;
constexpr uintptr_t VA_FS_OPENFLAG   = 0x01908e60;
// cls.keyCatchers: 1 console, 2 UI menu, 4 chat line, 8 cgame (Con_ToggleConsole_f 0x408860
// flips bit 1; 0x40a49a tests 0xa)
constexpr uintptr_t VA_KEYCATCHERS   = 0x015b8aa4;
// cl.gameState: stringOffsets[0] = CS_SERVERINFO ([1] = systeminfo, netcode/competitive.cpp)
constexpr uintptr_t VA_GS_SERVERINFO = 0x0148dcbc;
constexpr uintptr_t VA_GS_SYSTEMINFO = 0x0148dcc0;
constexpr uintptr_t VA_GS_STRINGDATA = 0x0148fcbc;
constexpr int       GS_MAX_CHARS     = 16000;
// cl.snap, the last snapshot parsed (clSnapshot_t, 0x20fc bytes: CL_ParseSnapshot copies it
// at 0x41762b). Its entities and client states sit in the parse rings, 2048 each:
//   CL_ParsePacketEntities 0x416b80: cl.parseEntities 0x1512914, entityState_t 0xf0
//   CL_ParsePacketClients ~0x4171ea: cl.parseClients 0x158a914, clientState_t 0x5c
//   0x41a130 (name of a client): numClients +0x20ec, first +0x20f4, clientIndex +0, name +0x3c
constexpr uintptr_t VA_SNAP_VALID    = 0x0148bba4;
constexpr uintptr_t VA_SNAP_NUMENTS  = 0x0148dc8c;   // + 0x20e8
constexpr uintptr_t VA_SNAP_NUMCLI   = 0x0148dc90;   // + 0x20ec
constexpr uintptr_t VA_SNAP_FIRSTENT = 0x0148dc94;   // + 0x20f0
constexpr uintptr_t VA_SNAP_FIRSTCLI = 0x0148dc98;   // + 0x20f4
constexpr uintptr_t VA_PARSE_ENTS    = 0x01512914;
constexpr uintptr_t VA_PARSE_CLIENTS = 0x0158a914;
constexpr int PARSE_MASK = 0x7ff, ENT_SIZE = 0xf0, CLI_SIZE = 0x5c;
constexpr int ENT_NUMBER = 0x0, ENT_ETYPE = 0x4, CLI_INDEX = 0x0, CLI_TEAM = 0x4;
constexpr int ET_PLAYER = 1;
constexpr int TEAM_AXIS = 1, TEAM_ALLIES = 2;        // clientState team: free 0, axis 1, allies 2, spectator 3
// cgame_mp_x86.dll (relative to where it is loaded)
constexpr uintptr_t RVA_REF_FOVX = 0x20d340;         // refdef fov_x, vieworg, viewaxis (forward first)
constexpr uintptr_t RVA_REF_ORG  = 0x20d348;
constexpr uintptr_t RVA_REF_AXIS = 0x20d354;
constexpr uintptr_t RVA_PS       = 0x20af14;         // cg.predictedPlayerState: clientNum +0xac
constexpr int       PS_CLIENTNUM = 0xac;
constexpr uintptr_t RVA_CENTS    = 0x211940;         // cg_entities (21 x `imul 0x228; add 0x30211940`)
constexpr uintptr_t CENT_SIZE    = 0x228;            // lerpOrigin +0x1f8, lerpAngles +0x204 (bone_probe)
constexpr int CENT_ORIGIN = 0x1f8, CENT_YAW = 0x208;

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
typedef int   (__cdecl* FS_FOpenFileRead_t)(const char*, int*, int, int);
typedef int   (__cdecl* FS_Read_t)(void*, int, int);
typedef void  (__cdecl* FS_FCloseFile_t)(int);

// ---- look ----
constexpr DWORD COL_BACK     = 0xFF0B0D10;   // under the map (outside the playable area)
constexpr DWORD COL_RIM_DARK = 0xB0000000;
constexpr DWORD COL_RIM      = 0x66FFFFFF;
constexpr DWORD COL_SELF     = 0xFFFFFFFF;
constexpr DWORD COL_OUTLINE  = 0xF0101215;
constexpr DWORD COL_SITE     = 0xFFE3C88D;   // the bombsite discs of the radar PNGs
constexpr DWORD COL_AXIS     = 0xFFE99F47;   // HUD kit: axis orange, allies blue
constexpr DWORD COL_ALLIES   = 0xFF67B2EE;
constexpr DWORD COL_MATE     = 0xFF7FD67A;   // free-for-all fallback (never shown: no team)

// ---- runtime radar (drawn from the BSP) ----
constexpr float BSP_UNITS_PER_PX = 3.0f;
constexpr int   BSP_MAX_SIDE     = 2048;
constexpr float BSP_WALL_UNITS   = 13.0f;

// ------------------------------------------------------------------ engine IO
void* find_cvar(const char* name) {
    static std::unordered_map<std::string, void*> s_cache;
    auto it = s_cache.find(name);
    if (it != s_cache.end()) return it->second;
    void* cv = ((Cvar_FindVar_t)VA_CVAR_FINDVAR)(name);
    if (cv) s_cache.emplace(name, cv);                // cvar_t never moves once created
    return cv;
}
const char* eng_cvar(const char* name) {
    void* cv = find_cvar(name);
    const char* s = cv ? *(const char* const*)((const char*)cv + 4) : nullptr;
    return s ? s : "";
}
void eng_reg(const char* name, const char* def) { ((Cvar_Get_t)VA_CVAR_GET)(name, def, 1); }   // CVAR_ARCHIVE
void eng_command(const char* text) { ((Cbuf_ExecuteText_t)VA_CBUF_EXECTEXT)(2, text); }
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

// "\key\value\..." of one configstring (case-insensitive keys, like Info_ValueForKey)
bool gs_value(uintptr_t ofs_va, const char* key, char* out, int outsz) {
    out[0] = 0;
    const int off = *(volatile int*)ofs_va;
    if (off <= 0 || off >= GS_MAX_CHARS) return false;
    const char* s = (const char*)VA_GS_STRINGDATA + off;
    const char* end = (const char*)VA_GS_STRINGDATA + GS_MAX_CHARS;
    const int klen = (int)strlen(key);
    while (s < end && *s == '\\') {
        const char* k = s + 1;
        const char* ke = k;
        while (ke < end && *ke && *ke != '\\') ++ke;
        if (ke >= end || !*ke) return false;
        const char* v = ke + 1;
        const char* ve = v;
        while (ve < end && *ve && *ve != '\\') ++ve;
        if (ke - k == klen && !_strnicmp(k, key, klen)) {
            const int n = std::min((int)(ve - v), outsz - 1);
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        if (ve >= end || !*ve) return false;
        s = ve;
    }
    return false;
}

uintptr_t cgame() { return (uintptr_t)GetModuleHandleA("cgame_mp_x86.dll"); }

const char* eng_map() {
    static char s_map[64];
    s_map[0] = 0;
    if (!cgame() || !*(volatile int*)VA_SNAP_VALID) return s_map;
    char v[64];
    if (gs_value(VA_GS_SERVERINFO, "mapname", v, sizeof(v))) {
        for (int i = 0; v[i] && i < 63; ++i) s_map[i] = (char)tolower((unsigned char)v[i]), s_map[i + 1] = 0;
    }
    return s_map;
}

bool eng_view(float org[3], float* yaw, float* fov_x) {
    const uintptr_t cg = cgame();
    if (!cg) return false;
    float ax[3];
    memcpy(org, (const void*)(cg + RVA_REF_ORG), 12);
    memcpy(ax, (const void*)(cg + RVA_REF_AXIS), 12);
    *fov_x = *(const float*)(cg + RVA_REF_FOVX);
    if (fabsf(ax[0]) + fabsf(ax[1]) < 1e-4f) return false;
    *yaw = atan2f(ax[1], ax[0]) * 57.2957795f;
    return true;
}

int client_team(int cn) {
    const int n = *(volatile int*)VA_SNAP_NUMCLI, first = *(volatile int*)VA_SNAP_FIRSTCLI;
    for (int i = 0; i < n && i < 64; ++i) {
        const uint8_t* cs = (const uint8_t*)VA_PARSE_CLIENTS + (size_t)((first + i) & PARSE_MASK) * CLI_SIZE;
        if (*(const int*)(cs + CLI_INDEX) == cn) return *(const int*)(cs + CLI_TEAM);
    }
    return -1;
}

int eng_mates(MinimapMate* out, int max) {
    const uintptr_t cg = cgame();
    if (!cg || !*(volatile int*)VA_SNAP_VALID) return 0;
    const int me = *(const int*)(cg + RVA_PS + PS_CLIENTNUM);
    const int team = client_team(me);
    if (team != TEAM_AXIS && team != TEAM_ALLIES) return 0;     // free-for-all / spectator: nobody
    const int n = *(volatile int*)VA_SNAP_NUMENTS, first = *(volatile int*)VA_SNAP_FIRSTENT;
    int k = 0;
    for (int i = 0; i < n && k < max; ++i) {
        const uint8_t* es = (const uint8_t*)VA_PARSE_ENTS + (size_t)((first + i) & PARSE_MASK) * ENT_SIZE;
        const int num = *(const int*)(es + ENT_NUMBER);
        if (num >= 64) break;                               // sorted by number: the clients come first
        if (num == me || *(const int*)(es + ENT_ETYPE) != ET_PLAYER) continue;
        if (client_team(num) != team) continue;             // NEVER an enemy
        const uint8_t* cent = (const uint8_t*)(cg + RVA_CENTS + (uintptr_t)num * CENT_SIZE);
        if (*(const int*)cent != num) continue;
        out[k].x = *(const float*)(cent + CENT_ORIGIN);
        out[k].y = *(const float*)(cent + CENT_ORIGIN + 4);
        out[k].yaw = *(const float*)(cent + CENT_YAW);
        out[k].team = team;
        ++k;
    }
    return k;
}

int eng_allowed() {
    char v[16];
    if (gs_value(VA_GS_SYSTEMINFO, "cod1x_minimap", v, sizeof(v)) ||
        gs_value(VA_GS_SERVERINFO, "cod1x_minimap", v, sizeof(v))) {
        if (v[0] >= '0' && v[0] <= '2') return v[0] - '0';
    }
    return ruleset_active() ? 1 : 2;
}

bool eng_hidden() {
    if (*(volatile int*)VA_KEYCATCHERS & 3) return true;    // console or a menu on top
    void* d2 = find_cvar("cg_draw2D");
    if (d2 && *(const int*)((const char*)d2 + 0x20) == 0) return true;
    return false;
}

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
    s_k = k;
    return s_k;
}

const char* eng_root() {
    static char s_root[MAX_PATH] = "";
    if (!s_root[0]) {
        GetModuleFileNameA(NULL, s_root, MAX_PATH);
        char* sl = strrchr(s_root, '\\');
        if (sl) *sl = 0;
    }
    return s_root;
}

const MinimapIO g_engine_io = { eng_cvar, eng_reg, eng_command, eng_read, eng_free, eng_map, eng_view,
                                eng_mates, eng_allowed, eng_hidden, eng_stretch, eng_root, false };
const MinimapIO* g_io = &g_engine_io;

// ------------------------------------------------------------------ radar job
struct Radar {
    std::string map, source, error;
    bool ok = false;
    int w = 0, h = 0;
    std::vector<std::vector<unsigned char>> mips;
    float x0 = 0, y0 = 0, sx = 1, sy = 1;           // world rectangle, north up
    bool site[2] = { false, false };
    float site_x[2] = { 0, 0 }, site_y[2] = { 0, 0 };
};
struct Job {
    Radar r;
    std::string png, json;                          // a Workshop image, else:
    std::vector<unsigned char> bsp;                 // the map's BSP
    volatile LONG done = 0;
};
Job*  g_job = nullptr;
Radar g_radar;                                      // the one on screen (pixels dropped once uploaded)
bool  g_uploaded = false;
std::string g_want;                                 // map the radar should be of
DWORD g_failed_at = 0;

bool file_exists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool read_text(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    char b[4096];
    size_t n;
    out.clear();
    while ((n = fread(b, 1, sizeof(b), f)) > 0 && out.size() < (1u << 20)) out.append(b, n);
    fclose(f);
    return true;
}
bool json_num(const std::string& j, size_t from, const char* key, float& v) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    v = (float)strtod(j.c_str() + p + 1, nullptr);
    return true;
}

bool decode_png(const std::string& path, Radar& r) {
    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&tok, &in, nullptr) != Gdiplus::Ok) { r.error = "GDI+ unavailable"; return false; }
    bool ok = false;
    {
        wchar_t wp[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wp, MAX_PATH);
        Gdiplus::Bitmap bmp(wp);
        if (bmp.GetLastStatus() == Gdiplus::Ok && bmp.GetWidth() >= 16 && bmp.GetHeight() >= 16 &&
            bmp.GetWidth() <= 4096 && bmp.GetHeight() <= 4096) {
            r.w = (int)bmp.GetWidth(); r.h = (int)bmp.GetHeight();
            Gdiplus::Rect rc(0, 0, r.w, r.h);
            Gdiplus::BitmapData bd;
            if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                r.mips.assign(1, std::vector<unsigned char>((size_t)r.w * r.h * 4));
                unsigned char* d = r.mips[0].data();
                for (int y = 0; y < r.h; ++y) {
                    const unsigned char* s = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
                    for (int x = 0; x < r.w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                }
                bmp.UnlockBits(&bd);
                ok = true;
            }
        }
        if (!ok) r.error = "cannot decode " + path;
    }
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

void build_mips(Radar& r) {
    int w = r.w, h = r.h;
    while (w > 1 || h > 1) {
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        std::vector<unsigned char> nxt((size_t)nw * nh * 4);
        ui_mip_half(r.mips.back().data(), w, h, nxt.data());
        r.mips.push_back(std::move(nxt));
        w = nw; h = nh;
    }
}

DWORD WINAPI radar_worker(LPVOID p) {
    Job* j = (Job*)p;
    Radar& r = j->r;
    const DWORD t0 = GetTickCount();
    if (!j->png.empty()) {
        std::string js;
        float size = 0, sx = 0, sy = 0;
        if (read_text(j->json, js) && json_num(js, 0, "world_x0", r.x0) && json_num(js, 0, "world_y0", r.y0) &&
            ((json_num(js, 0, "world_size_x", sx) && json_num(js, 0, "world_size_y", sy)) || json_num(js, 0, "world_size", size))) {
            r.sx = sx > 0 ? sx : size; r.sy = sy > 0 ? sy : size;
            // "sites": [{"site": "A", "x": .., "y": ..}, ...]
            for (size_t at = js.find("\"site\""); at != std::string::npos; at = js.find("\"site\"", at + 6)) {
                const size_t q = js.find('"', js.find(':', at) + 1);
                if (q == std::string::npos) break;
                const int k = js[q + 1] == 'A' || js[q + 1] == 'a' ? 0 : js[q + 1] == 'B' || js[q + 1] == 'b' ? 1 : -1;
                float x, y;
                if (k >= 0 && json_num(js, q, "x", x) && json_num(js, q, "y", y)) { r.site[k] = true; r.site_x[k] = x; r.site_y[k] = y; }
            }
            r.ok = r.sx > 0 && r.sy > 0 && decode_png(j->png, r);
            r.source = "Workshop pack";
        } else {
            r.error = j->json + ": no world_x0 / world_y0 / world_size";
        }
    } else {
        RadarMap m;
        if (radar_map_build_detail(j->bsp.data(), (int)j->bsp.size(), BSP_UNITS_PER_PX, BSP_MAX_SIDE, BSP_WALL_UNITS, m)) {
            r.w = m.w; r.h = m.h;
            r.mips.assign(1, std::move(m.rgba));
            r.x0 = m.x0; r.y0 = m.y0; r.sx = m.span_x; r.sy = m.span_y;
            for (int k = 0; k < 2; ++k) { r.site[k] = m.site[k]; r.site_x[k] = m.site_x[k]; r.site_y[k] = m.site_y[k]; }
            r.ok = true;
        } else {
            r.error = m.error;
        }
        r.source = "map BSP";
        j->bsp.clear();
        j->bsp.shrink_to_fit();
    }
    if (r.ok) build_mips(r);
    logger::logf("minimap: radar of %s %s in %lu ms: %s", r.map.c_str(), r.ok ? "ready" : "FAILED",
                 GetTickCount() - t0, r.ok ? r.source.c_str() : r.error.c_str());
    InterlockedExchange(&j->done, 1);
    return 0;
}

void start_job(const std::string& map) {
    Job* j = new Job();
    j->r.map = map;
    const std::string base = std::string(g_io->root()) + "\\minimaps\\" + map;
    if (file_exists(base + ".png") && file_exists(base + ".json")) {
        j->png = base + ".png";
        j->json = base + ".json";
    } else {
        const std::string path = "maps/mp/" + map + ".bsp";
        void* buf = nullptr;
        const int len = g_io->read_file(path.c_str(), &buf);
        if (len > 0 && buf) {
            j->bsp.assign((const unsigned char*)buf, (const unsigned char*)buf + len);
            g_io->free_file(buf);
        } else {
            j->r.error = path + " not readable";
            j->done = 1;
            g_job = j;
            return;
        }
    }
    g_job = j;
    if (g_io->sync) { radar_worker(j); return; }
    HANDLE th = CreateThread(nullptr, 0, radar_worker, j, 0, nullptr);
    if (!th) { radar_worker(j); return; }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
}

// main thread, inside a 2D pass (GL state saved): takes a finished job, uploads its texture
void collect() {
    if (g_job && InterlockedCompareExchange(&g_job->done, 0, 0)) {
        Job* j = g_job;
        g_job = nullptr;
        if (j->r.ok || g_radar.map != j->r.map) g_radar = std::move(j->r);
        if (!g_radar.ok) g_failed_at = GetTickCount();
        g_uploaded = false;
        delete j;
    }
    if (g_radar.ok && (!g_uploaded || !ui_tex_known("minimap:radar"))) {
        if (!g_radar.mips.empty()) {
            std::vector<const unsigned char*> lv;
            for (auto& m : g_radar.mips) lv.push_back(m.data());
            ui_tex_upload_levels("minimap:radar", g_radar.w, g_radar.h, lv.data(), (int)lv.size());
            // the pixels are kept: a vid_restart drops every texture and needs them again
        }
        g_uploaded = true;
    }
}

// ------------------------------------------------------------------ layout
MinimapLayout g_lay;
bool  g_registered = false;
DWORD g_poll = 0;

float cv_float(const char* n, float fb) {
    const char* s = g_io->cvar(n);
    return s && s[0] ? (float)atof(s) : fb;
}
int cv_int(const char* n, int fb) {
    const char* s = g_io->cvar(n);
    return s && s[0] ? atoi(s) : fb;
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

MinimapLayout read_layout() {
    const MinimapLayout d;
    MinimapLayout l;
    l.on     = cv_int("cod1x_minimap", d.on) != 0;
    l.x      = clampf(cv_float("cod1x_minimap_x", d.x), 0, 1);
    l.y      = clampf(cv_float("cod1x_minimap_y", d.y), 0, 1);
    l.size   = clampi(cv_int("cod1x_minimap_size", d.size), 120, 640);
    l.shape  = clampi(cv_int("cod1x_minimap_shape", d.shape), 0, 1);
    l.rotate = cv_int("cod1x_minimap_rotate", d.rotate) != 0;
    l.range  = clampi(cv_int("cod1x_minimap_range", d.range), 500, 6000);
    l.alpha  = clampi(cv_int("cod1x_minimap_alpha", d.alpha), 20, 100);
    l.mates  = cv_int("cod1x_minimap_mates", d.mates) != 0;
    return l;
}

void register_cvars() {
    const MinimapLayout d;
    char b[32];
    g_io->reg("cod1x_minimap", "1");
    snprintf(b, sizeof(b), "%.4f", d.x); g_io->reg("cod1x_minimap_x", b);
    snprintf(b, sizeof(b), "%.4f", d.y); g_io->reg("cod1x_minimap_y", b);
    snprintf(b, sizeof(b), "%d", d.size);  g_io->reg("cod1x_minimap_size", b);
    snprintf(b, sizeof(b), "%d", d.shape); g_io->reg("cod1x_minimap_shape", b);
    snprintf(b, sizeof(b), "%d", d.rotate); g_io->reg("cod1x_minimap_rotate", b);
    snprintf(b, sizeof(b), "%d", d.range); g_io->reg("cod1x_minimap_range", b);
    snprintf(b, sizeof(b), "%d", d.alpha); g_io->reg("cod1x_minimap_alpha", b);
    snprintf(b, sizeof(b), "%d", d.mates); g_io->reg("cod1x_minimap_mates", b);
}

// ------------------------------------------------------------------ drawing
// Local frame of the minimap: origin at its centre, y down, R = half its side.
struct Pt2 { float x, y; };

void outline(int shape, float R, float inset, std::vector<Pt2>& out) {
    out.clear();
    if (shape == 1) {
        const int n = 128;
        const float r = std::max(0.0f, R - inset);
        for (int i = 0; i < n; ++i) {
            const float a = 6.2831853f * i / n;
            out.push_back({ cosf(a) * r, sinf(a) * r });
        }
        return;
    }
    const float rc = R * 0.16f, c = R - rc, r = std::max(0.0f, rc - inset);
    const float cx[4] = { c, -c, -c, c }, cy[4] = { c, c, -c, -c };
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= 12; ++i) {
            const float a = 1.5707963f * (k + i / 12.0f);
            out.push_back({ cx[k] + cosf(a) * r, cy[k] + sinf(a) * r });
        }
}

// a direction from the centre pushed onto the frame, `m` inside its edge
Pt2 to_edge(int shape, float R, float m, float dx, float dy) {
    const float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) return { 0, -(R - m) };
    if (shape == 1) return { dx / l * (R - m), dy / l * (R - m) };
    const float s = (R - m) / std::max(fabsf(dx), fabsf(dy));
    return { dx * s, dy * s };
}
bool inside(int shape, float R, float m, float x, float y) {
    if (shape == 1) return x * x + y * y <= (R - m) * (R - m);
    return fabsf(x) <= R - m && fabsf(y) <= R - m;
}

DWORD with_alpha(DWORD c, float a) {
    const float v = ((c >> 24) & 0xff) * clampf(a, 0, 1);
    return (c & 0x00FFFFFF) | ((DWORD)(v + 0.5f) << 24);
}

// the chevron of a player, pointing along local direction (dx, dy)
void chevron(float x, float y, float dx, float dy, float s, DWORD fill, float outline_px) {
    const float a = atan2f(dx, -dy), ca = cosf(a), sa = sinf(a);
    const float P[4][2] = { { 0, -1.15f }, { 0.78f, 0.9f }, { 0, 0.42f }, { -0.78f, 0.9f } };
    float xy[8];
    for (int i = 0; i < 4; ++i) {
        const float px = P[i][0] * s, py = P[i][1] * s;
        xy[i * 2]     = x + px * ca - py * sa;
        xy[i * 2 + 1] = y + px * sa + py * ca;
    }
    ui_aa_poly(xy, 4, fill);
    if (outline_px > 0) ui_aa_poly_stroke(xy, 4, outline_px, COL_OUTLINE);
}

void text_centered(float x, float y, int px, int weight, DWORD col, const char* s) {
    if (px < 6) return;                                  // the menu's miniature: too small to read
    const float w = ui_text_font_width(px, weight, s, UI_FONT_SANS);
    const float h = ui_text_font_height(px, weight, UI_FONT_SANS);
    ui_text_font(roundf(x - w / 2), roundf(y - h / 2), px, weight, col, s, UI_FONT_SANS);
}

struct View {
    float cx = 0, cy = 0;          // world point at the centre
    float yaw = 90;                // degrees, world (0 = +X, 90 = north)
    float fov = 80;
    bool  live = false;
};

// Draws the minimap centred on screen point (sx, sy), half side R (panel pixels), the
// panel being `k` times wider than the screen shows (drawn 1/k narrower).
void draw_core(float sx, float sy, float R, float k, float S, const MinimapLayout& lay, const View& v,
               const MinimapMate* mates, int nmates, bool status_text) {
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(roundf(sx), roundf(sy), 0);
    if (k != 1.0f) glScalef(1.0f / k, 1.0f, 1.0f);
    const int shape = lay.shape;
    const float upp = lay.range / R;                     // world units per local pixel
    const float th = v.yaw * 0.0174532925f;
    float fx = 0, fy = 1, rx = 1, ry = 0;                // forward / right in world
    if (lay.rotate) { fx = cosf(th); fy = sinf(th); rx = sinf(th); ry = -cosf(th); }
    auto to_local = [&](float wx, float wy, float* lx, float* ly) {
        const float dx = wx - v.cx, dy = wy - v.cy;
        *lx = (dx * rx + dy * ry) / upp;
        *ly = -(dx * fx + dy * fy) / upp;
    };
    const float alpha = lay.alpha / 100.0f;

    // shadow: two gradient bands around the frame, 2 px lower (the outlines of one shape
    // at several insets have matching points, so the bands never seam)
    const float rr = shape == 1 ? R : R * 0.16f;
    std::vector<Pt2> ring, inner;
    {
        std::vector<Pt2> a, b, c;
        outline(shape, R, 2 * S, a);
        outline(shape, R, -5 * S, b);
        outline(shape, R, -15 * S, c);
        const float oy = 2 * S;
        const DWORD c0 = with_alpha(0x70000000, 0.6f + 0.4f * alpha), c1 = with_alpha(0x24000000, 0.6f + 0.4f * alpha);
        glDisable(GL_TEXTURE_2D);
        auto band = [&](const std::vector<Pt2>& in, const std::vector<Pt2>& out, DWORD ci, DWORD co) {
            glBegin(GL_TRIANGLE_STRIP);
            for (size_t i = 0; i <= in.size(); ++i) {
                const size_t j = i % in.size();
                ui_set_color(ci); glVertex2f(in[j].x, in[j].y + oy);
                ui_set_color(co); glVertex2f(out[j].x, out[j].y + oy);
            }
            glEnd();
        };
        band(a, b, c0, c1);
        band(b, c, c1, 0x00000000);
    }
    outline(shape, R, 0, ring);
    glDisable(GL_TEXTURE_2D);
    ui_set_color(with_alpha(COL_BACK, 0.55f + 0.4f * alpha));
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(0, 0);
    for (const Pt2& p : ring) glVertex2f(p.x, p.y);
    glVertex2f(ring[0].x, ring[0].y);
    glEnd();

    // the map: the same fan, texture coordinates = world position of each vertex
    const bool have = g_radar.ok && ui_tex_bind("minimap:radar");
    if (have) {
        ui_set_color(with_alpha(0xFFFFFFFF, alpha));
        glTexEnvf(0x8500 /*GL_TEXTURE_FILTER_CONTROL*/, 0x8501 /*GL_TEXTURE_LOD_BIAS*/, -0.35f);
        auto vert = [&](float lx, float ly) {
            const float wx = v.cx + upp * (lx * rx - ly * fx);
            const float wy = v.cy + upp * (lx * ry - ly * fy);
            glTexCoord2f((wx - g_radar.x0) / g_radar.sx, (g_radar.y0 + g_radar.sy - wy) / g_radar.sy);
            glVertex2f(lx, ly);
        };
        glBegin(GL_TRIANGLE_FAN);
        vert(0, 0);
        for (const Pt2& p : ring) vert(p.x, p.y);
        vert(ring[0].x, ring[0].y);
        glEnd();
        glTexEnvf(0x8500, 0x8501, 0.0f);
        glDisable(GL_TEXTURE_2D);
    }

    // field of view: a soft wedge from the player
    glDisable(GL_TEXTURE_2D);
    if (v.live) {
        const float half = clampf(v.fov, 20, 140) * 0.5f * 0.0174532925f;
        const float L = R * 0.72f;
        const float dir = lay.rotate ? 0.0f : 1.5707963f - th;      // rotation of "up"
        glBegin(GL_TRIANGLE_FAN);
        ui_set_color(0x2EFFFFFF);
        glVertex2f(0, 0);
        ui_set_color(0x00FFFFFF);
        for (int i = 0; i <= 24; ++i) {
            const float a = dir - half + 2 * half * i / 24.0f;
            glVertex2f(sinf(a) * L, -cosf(a) * L);
        }
        glEnd();
    }

    // depth: a dark band inside the rim
    outline(shape, R, 11 * S, inner);
    glBegin(GL_TRIANGLE_STRIP);
    for (size_t i = 0; i <= ring.size(); ++i) {
        const size_t j = i % ring.size();
        ui_set_color(0x5C000000); glVertex2f(ring[j].x, ring[j].y);
        ui_set_color(0x00000000); glVertex2f(inner[j].x, inner[j].y);
    }
    glEnd();

    if (status_text && !have) {
        text_centered(0, -8 * S, (int)(13 * S), 700, 0x99FFFFFF, "MINIMAP");
        text_centered(0, 10 * S, (int)(11 * S), 400, 0x66FFFFFF,
                      g_job ? "drawing the radar..." : g_radar.map.empty() ? "no map loaded" : "no radar for this map");
    }

    // bombsites: on the map, or pinned to the edge when out of range
    for (int s = 0; s < 2; ++s) {
        if (!g_radar.ok || !g_radar.site[s]) continue;
        float lx, ly;
        to_local(g_radar.site_x[s], g_radar.site_y[s], &lx, &ly);
        const float rad = 9.0f * S;
        float a = 1.0f;
        if (!inside(shape, R, rad + 4 * S, lx, ly)) {
            const Pt2 e = to_edge(shape, R, rad + 4 * S, lx, ly);
            lx = e.x; ly = e.y; a = 0.7f;
        }
        ui_aa_disc(lx, ly, rad + 1.6f * S, with_alpha(COL_OUTLINE, a));
        ui_aa_disc(lx, ly, rad, with_alpha(COL_SITE, a));
        text_centered(lx, ly + 0.5f * S, (int)(12.5f * S), 800, with_alpha(0xFF15171A, a), s == 0 ? "A" : "B");
    }

    // teammates (only what the server allows, only your team: see minimap.h)
    for (int i = 0; i < nmates; ++i) {
        const DWORD mate_col = mates[i].team == TEAM_AXIS ? COL_AXIS : mates[i].team == TEAM_ALLIES ? COL_ALLIES : COL_MATE;
        float lx, ly;
        to_local(mates[i].x, mates[i].y, &lx, &ly);
        if (!inside(shape, R, 6 * S, lx, ly)) continue;
        const float ya = mates[i].yaw * 0.0174532925f;
        float dx, dy;
        to_local(v.cx + cosf(ya) * 100, v.cy + sinf(ya) * 100, &dx, &dy);
        chevron(lx, ly, dx, dy, 6.5f * S, mate_col, 1.4f * S);
    }

    // rim: dark outside line, light inside line
    if (shape == 1) {
        ui_aa_ring(0, 0, R + 0.5f * S, 1.6f * S, COL_RIM_DARK);
        ui_aa_ring(0, 0, R - 0.8f * S, 1.3f * S, COL_RIM);
    } else {
        ui_aa_stroke_rect(-R - 0.5f * S, -R - 0.5f * S, 2 * R + S, 2 * R + S, rr + 0.5f * S, 1.6f * S, COL_RIM_DARK);
        ui_aa_stroke_rect(-R + 0.8f * S, -R + 0.8f * S, 2 * R - 1.6f * S, 2 * R - 1.6f * S, rr - 0.8f * S, 1.3f * S, COL_RIM);
    }

    // north on the rim
    {
        float nx, ny;
        to_local(v.cx, v.cy + 1000, &nx, &ny);
        const Pt2 e = to_edge(shape, R, 10.5f * S, nx, ny);
        ui_aa_disc(e.x, e.y, 7.5f * S, 0xD0101215);
        text_centered(e.x, e.y + 0.5f * S, (int)(10.5f * S), 800, 0xFFFFFFFF, "N");
    }

    // you
    {
        float dx = 0, dy = -1;
        if (!lay.rotate) { dx = cosf(th); dy = -sinf(th); }
        chevron(0, 0, dx, dy, 8.5f * S, COL_SELF, 1.8f * S);
    }
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

View g_last_view;                    // last live view: the editor's preview out of a game

}  // namespace

void minimap_set_io(const MinimapIO* io) { g_io = io ? io : &g_engine_io; }

MinimapLayout minimap_layout_default() { return MinimapLayout(); }
MinimapLayout minimap_layout() { return g_registered ? read_layout() : g_lay; }

void minimap_layout_set(const MinimapLayout& l) {
    const MinimapLayout c = minimap_layout();
    char b[96];
    auto seti = [&](const char* n, int a, int was) { if (a != was) { snprintf(b, sizeof(b), "seta %s %d\n", n, a); g_io->command(b); } };
    auto setf = [&](const char* n, float a, float was) { if (fabsf(a - was) > 1e-5f) { snprintf(b, sizeof(b), "seta %s %.4f\n", n, a); g_io->command(b); } };
    seti("cod1x_minimap", l.on, c.on);
    setf("cod1x_minimap_x", l.x, c.x);
    setf("cod1x_minimap_y", l.y, c.y);
    seti("cod1x_minimap_size", l.size, c.size);
    seti("cod1x_minimap_shape", l.shape, c.shape);
    seti("cod1x_minimap_rotate", l.rotate, c.rotate);
    seti("cod1x_minimap_range", l.range, c.range);
    seti("cod1x_minimap_alpha", l.alpha, c.alpha);
    seti("cod1x_minimap_mates", l.mates, c.mates);
    g_lay = l;                                    // the cvars catch up when the buffer runs
}

const char* minimap_source() { return g_radar.ok ? g_radar.source.c_str() : ""; }
const char* minimap_map() { return g_radar.map.c_str(); }
int minimap_server_rule() { return g_io->map()[0] ? g_io->allowed() : 2; }

void minimap_frame() {
    if (!g_registered) {
        register_cvars();
        g_registered = true;
    }
    const DWORD now = GetTickCount();
    if (now - g_poll < 200) return;
    g_poll = now;
    g_lay = read_layout();
    if (!g_lay.on) return;
    const std::string map = g_io->map();
    if (map.empty()) return;
    if (map != g_want) { g_want = map; g_failed_at = 0; }
    if (g_job) return;                                    // collected in the 2D pass
    if (g_radar.map == map && g_radar.ok && g_radar.source != "Workshop pack") {
        // drawn from the BSP: switches to the pack's picture the moment it is installed
        static DWORD s_look = 0;
        if (now - s_look < 3000) return;
        s_look = now;
        const std::string base = std::string(g_io->root()) + "\\minimaps\\" + map;
        if (!file_exists(base + ".png") || !file_exists(base + ".json")) return;
        logger::logf("minimap: a picture of %s appeared in minimaps: switching to it", map.c_str());
        start_job(map);
        return;
    }
    if (g_radar.map == map && (g_radar.ok || now - g_failed_at < 60000)) return;
    logger::logf("minimap: radar of %s requested", map.c_str());
    start_job(map);
}

bool g_test_running = false;              // the in-game check draws over the team menu

bool minimap_active() {
    if (!g_registered || !g_lay.on) return false;
    if (g_io->map()[0] == 0) return false;
    if (g_io->allowed() == 0) return false;
    if (g_io->hidden() && !g_test_running) return false;
    return true;
}

void minimap_box(const MinimapLayout& l, float vw, float vh, float* x, float* y, float* w, float* h) {
    const float S = std::max(0.5f, vh / 1080.0f);
    const float k = g_io->stretch ? g_io->stretch(vw, vh) : 1.0f;
    const float side = roundf(l.size * S);
    *h = side;
    *w = roundf(side / (k > 1.02f && k < 3.0f ? k : 1.0f));
    *x = roundf(clampf(l.x, 0, 1) * std::max(0.0f, vw - *w));
    *y = roundf(clampf(l.y, 0, 1) * std::max(0.0f, vh - *h));
}

void minimap_draw_box(float x, float y, float w, float h, float vw, float vh, const MinimapLayout& l) {
    collect();
    const float S = h / std::max(1, l.size);             // the markers follow the box
    const float k = h / std::max(1.0f, w);
    View v;
    MinimapMate mates[64];
    int n = 0;
    float org[3], yaw, fov;
    if (g_io->map()[0] && g_io->view(org, &yaw, &fov)) {
        v.cx = org[0]; v.cy = org[1]; v.yaw = yaw; v.fov = fov; v.live = true;
        g_last_view = v;
        if (l.mates && g_io->allowed() >= 2) n = g_io->mates(mates, 64);
    } else if (g_last_view.live || g_last_view.cx != 0) {
        v = g_last_view;
    } else if (g_radar.ok) {
        v.cx = g_radar.x0 + g_radar.sx / 2; v.cy = g_radar.y0 + g_radar.sy / 2;
    }
    draw_core(x + w / 2, y + h / 2, h / 2, k, S, l, v, mates, n, true);
}

void minimap_draw(float vw, float vh) {
    collect();
    float x, y, w, h;
    minimap_box(g_lay, vw, vh, &x, &y, &w, &h);
    float org[3], yaw, fov;
    if (!g_io->view(org, &yaw, &fov)) return;
    View v;
    v.cx = org[0]; v.cy = org[1]; v.yaw = yaw; v.fov = fov; v.live = true;
    g_last_view = v;
    MinimapMate mates[64];
    int n = 0;
    if (g_lay.mates && g_io->allowed() >= 2) n = g_io->mates(mates, 64);
    draw_core(x + w / 2, y + h / 2, h / 2, h / std::max(1.0f, w), h / std::max(1, g_lay.size), g_lay, v, mates, n,
              g_job != nullptr);
}


// ------------------------------------------------------------------ in-game check (dev)
namespace {

constexpr uintptr_t VA_UI_DRAW_JE = 0x00418a72;   // SCR_DrawScreenField: je over UI_REFRESH (bench.cpp)

struct TestStep { std::string kind; float x = 0, y = 0, z = 0, yaw = 0, secs = 0; std::vector<std::string> sets; };
std::vector<TestStep> g_steps;
size_t g_step = 0;
int    g_tframe = 0;
DWORD  g_tstart = 0;
char   g_ttag[16] = "";
bool   g_tquit = false, g_ui_patched = false;

void ui_draw_skip(bool on) {
    uint8_t* p = (uint8_t*)VA_UI_DRAW_JE;
    if (on == g_ui_patched || p[1] != 0x1a || (p[0] != 0x74 && p[0] != 0xeb)) return;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = on ? 0xeb : 0x74;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    g_ui_patched = on;
}

struct Shot { int w, h; std::vector<unsigned char> px; std::string path; };

DWORD WINAPI save_worker(LPVOID p) {
    Shot* s = (Shot*)p;
    ULONG_PTR tok = 0;
    Gdiplus::GdiplusStartupInput gin;
    if (Gdiplus::GdiplusStartup(&tok, &gin, nullptr) == Gdiplus::Ok) {
        {
            Gdiplus::Bitmap bmp(s->w, s->h, PixelFormat32bppARGB);
            Gdiplus::BitmapData bd;
            Gdiplus::Rect rc(0, 0, s->w, s->h);
            if (bmp.LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                for (int y = 0; y < s->h; ++y) {
                    const unsigned char* src = &s->px[(size_t)(s->h - 1 - y) * s->w * 4];
                    unsigned char* d = (unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
                    for (int x = 0; x < s->w; ++x) {
                        d[x * 4] = src[x * 4 + 2]; d[x * 4 + 1] = src[x * 4 + 1]; d[x * 4 + 2] = src[x * 4]; d[x * 4 + 3] = 255;
                    }
                }
                bmp.UnlockBits(&bd);
                UINT n = 0, size = 0;
                Gdiplus::GetImageEncodersSize(&n, &size);
                std::vector<unsigned char> buf(size);
                Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
                Gdiplus::GetImageEncoders(n, size, codecs);
                for (UINT i = 0; i < n; ++i)
                    if (!wcscmp(codecs[i].MimeType, L"image/png")) {
                        wchar_t wp[MAX_PATH];
                        MultiByteToWideChar(CP_ACP, 0, s->path.c_str(), -1, wp, MAX_PATH);
                        const bool ok = bmp.Save(wp, &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
                        logger::logf("minimap test: %s %s", ok ? "wrote" : "could not write", s->path.c_str());
                    }
            }
        }
        Gdiplus::GdiplusShutdown(tok);
    }
    delete s;
    return 0;
}

void capture(int vw, int vh, const std::string& path) {
    Shot* s = new Shot();
    s->w = vw; s->h = vh; s->path = path;
    s->px.resize((size_t)vw * vh * 4);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, s->px.data());
    glPopClientAttrib();
    HANDLE th = CreateThread(nullptr, 0, save_worker, s, 0, nullptr);
    if (th) CloseHandle(th); else save_worker(s);
}

// who the snapshot holds at a shot: the players, their team, what the minimap kept
void log_snapshot(unsigned shot) {
    const uintptr_t cg = cgame();
    if (!cg || !*(volatile int*)VA_SNAP_VALID) { logger::logf("minimap test: v%02u no snapshot", shot); return; }
    const int me = *(const int*)(cg + RVA_PS + PS_CLIENTNUM);
    char line[512];
    int at = snprintf(line, sizeof(line), "minimap test: v%02u me %d team %d rule %d, clients %d, players:", shot, me,
                      client_team(me), eng_allowed(), *(volatile int*)VA_SNAP_NUMCLI);
    const int n = *(volatile int*)VA_SNAP_NUMENTS, first = *(volatile int*)VA_SNAP_FIRSTENT;
    for (int i = 0; i < n && at < (int)sizeof(line) - 24; ++i) {
        const uint8_t* es = (const uint8_t*)VA_PARSE_ENTS + (size_t)((first + i) & PARSE_MASK) * ENT_SIZE;
        const int num = *(const int*)(es + ENT_NUMBER);
        if (num >= 64) break;
        const uint8_t* cent = (const uint8_t*)(cg + RVA_CENTS + (uintptr_t)num * CENT_SIZE);
        at += snprintf(line + at, sizeof(line) - at, " %d:e%d/t%d%s", num, *(const int*)(es + ENT_ETYPE), client_team(num),
                       *(const int*)cent == num ? "" : "/nocent");
    }
    MinimapMate m[64];
    logger::logf("%s -> mates %d", line, eng_mates(m, 64));
}

bool load_steps() {
    g_steps.clear();
    g_tquit = false;
    std::string txt;
    if (!read_text(std::string(g_io->root()) + "\\cod1x_minimap_views.txt", txt)) return false;
    size_t at = 0;
    while (at < txt.size()) {
        size_t e = txt.find('\n', at);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(at, e - at);
        at = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        TestStep st;
        char word[64] = "";
        sscanf(line.c_str(), "%63s", word);
        if (!strcmp(word, "quit")) { g_tquit = true; continue; }
        if (!strcmp(word, "editor") || !strcmp(word, "menu") || !strcmp(word, "close")) { st.kind = word; g_steps.push_back(st); continue; }
        if (!strcmp(word, "wait")) {                          // a demo: no setviewpos, a shot S seconds on
            st.kind = word;
            sscanf(line.c_str() + 4, "%f", &st.secs);
            g_steps.push_back(st);
            continue;
        }
        int used = 0;
        if (sscanf(line.c_str(), "%f %f %f %f%n", &st.x, &st.y, &st.z, &st.yaw, &used) < 4) continue;
        st.kind = "view";
        const char* p = line.c_str() + used;
        char tok[96];
        int n = 0;
        while (sscanf(p, "%95s%n", tok, &n) == 1) {
            p += n;
            char* eq = strchr(tok, '=');
            if (eq) { *eq = 0; st.sets.push_back(std::string("seta ") + tok + " " + (eq + 1) + "\n"); }
        }
        g_steps.push_back(st);
    }
    return !g_steps.empty();
}

}  // namespace

void minimap_test_frame(int vw, int vh) {
    static DWORD s_poll = 0;
    static void* s_cv = nullptr;
    if (g_io != &g_engine_io) return;
    if (!g_test_running) {
        const DWORD now = GetTickCount();
        if (now - s_poll < 500) return;
        s_poll = now;
        if (!s_cv) s_cv = ((Cvar_Get_t)VA_CVAR_GET)("cod1x_minimap_test", "0", 0);
        if (!s_cv || *(const int*)((const char*)s_cv + 0x20) == 0) return;
        if (!eng_map()[0]) return;                                  // waits for the map
        if (!load_steps()) {
            logger::logf("minimap test: no steps in %s\\cod1x_minimap_views.txt", g_io->root());
            g_io->command("set cod1x_minimap_test 0\n");
            return;
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(g_ttag, sizeof(g_ttag), "%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
        g_test_running = true;
        g_step = 0; g_tframe = -1; g_tstart = now;
        ui_draw_skip(true);
        logger::logf("minimap test: %u steps on %s (tag %s)", (unsigned)g_steps.size(), eng_map(), g_ttag);
        return;
    }
    // the radar first (at most 30 s)
    if (g_tframe < 0) {
        if (!(g_radar.ok && g_radar.map == eng_map()) && GetTickCount() - g_tstart < 30000) return;
        g_tframe = 0;
    }
    if (g_step >= g_steps.size()) {
        g_test_running = false;
        ui_draw_skip(false);
        hud_editor_open(false);
        overlay_toggle(false);
        g_io->command("set cod1x_minimap_test 0\n");
        logger::logf("minimap test: done");
        if (g_tquit) g_io->command("quit\n");
        return;
    }
    const TestStep& st = g_steps[g_step];
    const int settle = 90;
    static DWORD s_step_at = 0;
    if (g_tframe == 0) {
        s_step_at = GetTickCount();
        if (st.kind == "view") {
            for (const std::string& c : st.sets) g_io->command(c.c_str());
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "setviewpos %.0f %.0f %.0f %.0f\n", st.x, st.y, st.z, st.yaw);
            g_io->command(cmd);
        } else if (st.kind == "menu" || st.kind == "editor") {
            overlay_toggle(true);
        } else if (st.kind == "close") {
            hud_editor_open(false);
            overlay_toggle(false);
        }
    }
    if (st.kind == "editor" && g_tframe == 10) hud_editor_open(true);
    if (++g_tframe < settle) return;
    if (st.kind == "wait" && GetTickCount() - s_step_at < (DWORD)(st.secs * 1000)) return;
    if (st.kind != "close") {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\Main\\screenshots\\minimap_%s_v%02u.png", g_io->root(), g_ttag, (unsigned)(g_step + 1));
        capture(vw, vh, path);
        log_snapshot((unsigned)(g_step + 1));
    }
    ++g_step;
    g_tframe = 0;
}

}  // namespace patches
