// demo_seek.cpp - see demo_seek.h
#include "ui/demo_seek.h"
#include "ui/gl_overlay.h"
#include "core/logger.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA (force set)
#include "video/display_probe.h"      // config_mp_set
#include "features/demo_index.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef void (__cdecl* Cbuf_ExecuteText_t)(int, const char*);
constexpr int CV_STRING = 0x04, CV_VALUE = 0x1c;
constexpr int EXEC_APPEND = 2;
const char* kMarker = "cod1reloaded.demoseek";      // "<mss_volume before the seek>"

enum State { IDLE, WAIT_PLAY, SEEKING, SETTLE };

struct Seek {
    State       state = IDLE;
    std::string name;                 // demo name as given to `demo` (no extension)
    std::string label;                // curtain title
    int         target = 0;           // server time to land on
    int         first = 0;            // the demo's first snapshot (progress origin)
    int         from = 0;             // snapshot time when the fast-forward began
    DWORD       t_start = 0;          // GetTickCount at play
    DWORD       t_seek = 0;           // GetTickCount when the fast-forward began
    DWORD       t_settle = 0;
    float       progress = 0;
    bool        muted = false;
    char        saved_volume[32] = {};
    float       last_ts = -1;
    int         steps = 0;
} g;

LARGE_INTEGER g_qpf = {}, g_last = {};
double g_frame_ms = 4.0;                // smoothed real frame time

void* find(const char* n) { return ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(n); }
void set(const char* n, const char* v) { ((Cvar_Set_t)CODMP_CVAR_SET_VA)(n, v); }
const char* cvar_str(const char* n, const char* fb) {
    void* c = find(n);
    return c ? *(const char**)((char*)c + CV_STRING) : fb;
}
void cmd(const char* s) { ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(EXEC_APPEND, s); }

bool demo_playing() { return *(volatile int*)CODMP_CLC_DEMOPLAYING_VA != 0; }
bool snap_valid()   { return *(volatile int*)CODMP_CL_SNAP_VALID_VA != 0; }
int  snap_time()    { return *(volatile int*)CODMP_CL_SNAP_TIME_VA; }
bool playing_ours() {
    const char* dn = (const char*)CODMP_CLC_DEMONAME_VA;
    return _strnicmp(dn, g.name.c_str(), g.name.size()) == 0 &&
           (dn[g.name.size()] == 0 || dn[g.name.size()] == '.');
}

std::string marker_path() {
    char p[MAX_PATH];
    DWORD r = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (!r || r >= MAX_PATH) return "";
    char* sl = strrchr(p, '\\');
    if (!sl) return "";
    sl[1] = 0;
    return std::string(p) + kMarker;
}

void mute() {
    if (g.muted) return;
    snprintf(g.saved_volume, sizeof(g.saved_volume), "%s", cvar_str("mss_volume", "0.8"));
    if (atof(g.saved_volume) <= 0.0) snprintf(g.saved_volume, sizeof(g.saved_volume), "0.8");
    // a crash mid-seek must not leave the player muted for good (mss_volume is archived)
    const std::string mp = marker_path();
    if (FILE* f = mp.empty() ? nullptr : fopen(mp.c_str(), "wb")) { fputs(g.saved_volume, f); fclose(f); }
    set("mss_volume", "0");
    g.muted = true;
}

void unmute() {
    if (!g.muted) return;
    set("mss_volume", g.saved_volume);
    g.muted = false;
    const std::string mp = marker_path();
    if (!mp.empty()) DeleteFileA(mp.c_str());
}

void set_timescale(float ts) {
    if (g.last_ts > 0 && fabsf(ts - g.last_ts) < g.last_ts * 0.02f) return;
    char v[32];
    snprintf(v, sizeof(v), "%g", ts);
    set("timescale", v);
    g.last_ts = ts;
}

void finish(const char* why) {
    set_timescale(1.0f);
    g.last_ts = -1;
    unmute();
    logger::logf("demo_seek: %s", why);
}

}  // namespace

void demo_seek_play(const std::string& dir, const std::string& name_noext,
                    int target_server_time, int first_server_time, const std::string& label) {
    if (g.state == SEEKING) finish("previous seek dropped");
    g = Seek();
    g.name = name_noext;
    g.label = label;
    g.target = target_server_time;
    g.first = first_server_time;
    g.t_start = GetTickCount();
    const char* fsg = cvar_str("fs_game", "");
    char c[512];
    if (_stricmp(dir.c_str(), "Main") != 0 && _stricmp(dir.c_str(), fsg) != 0)
        snprintf(c, sizeof(c), "seta fs_game %s\nvid_restart\ndemo %s\n", dir.c_str(), name_noext.c_str());
    else
        snprintf(c, sizeof(c), "demo %s\n", name_noext.c_str());
    cmd(c);
    g.state = target_server_time > 0 ? WAIT_PLAY : IDLE;
    logger::logf("demo_seek: play %s/%s%s", dir.c_str(), name_noext.c_str(),
                 target_server_time > 0 ? " and fast-forward" : "");
    if (target_server_time > 0)
        logger::logf("demo_seek: target server time %d (%d ms into the demo)",
                     target_server_time, target_server_time - first_server_time);
}

namespace {

// `set cod1x_demo_goto "demo0105 8:00"` (console or command line): plays that demo from
// 8:00 of its own clock. The file is read here, on the main thread (~0.2 s for 10 MB):
// the target is the demo's first server time + the offset.
void poll_goto_cvar() {
    static DWORD s_last = 0;
    const DWORD now = GetTickCount();
    if (now - s_last < 250) return;
    s_last = now;
    void* cv = find("cod1x_demo_goto");
    if (!cv) {                                      // register it once, empty
        static bool s_made = false;
        if (!s_made) { s_made = true; set("cod1x_demo_goto", ""); }
        return;
    }
    const char* v = *(const char**)((char*)cv + CV_STRING);
    if (!v || !v[0]) return;
    char name[128] = {}, when[32] = {};
    const int n = sscanf(v, "%127s %31s", name, when);
    set("cod1x_demo_goto", "");
    if (n < 1) return;
    int off_ms = 0;
    if (n == 2) {
        int mm = 0, ss = 0;
        if (sscanf(when, "%d:%d", &mm, &ss) == 2) off_ms = (mm * 60 + ss) * 1000;
        else off_ms = atoi(when) * 1000;
    }
    std::string base = name;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos && base.compare(dot, 4, ".dm_") == 0) base.resize(dot);
    // the file: <exe dir>\<fs_game or Main>\demos\<name>.dm_*
    char root[MAX_PATH];
    DWORD r = GetModuleFileNameA(NULL, root, MAX_PATH);
    char* sl = r ? strrchr(root, '\\') : nullptr;
    if (!sl) return;
    sl[1] = 0;
    std::string found, dir;
    const char* fsg = cvar_str("fs_game", "");
    const char* dirs[] = { fsg, "Main" };
    for (const char* dd : dirs) {
        if (!dd[0]) continue;
        WIN32_FIND_DATAA fd;
        const std::string pat = std::string(root) + dd + "\\demos\\" + base + ".dm_*";
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        found = std::string(root) + dd + "\\demos\\" + fd.cFileName;
        dir = dd;
        FindClose(h);
        break;
    }
    if (found.empty()) { logger::logf("demo_seek: cod1x_demo_goto - %s not found in demos/", base.c_str()); return; }
    DemoInfo info;
    if (!demo_index_file(found.c_str(), &info)) {
        logger::logf("demo_seek: cod1x_demo_goto - %s unreadable (%s)", found.c_str(), info.error.c_str());
        return;
    }
    char label[64];
    snprintf(label, sizeof(label), "%d:%02d", off_ms / 60000, (off_ms / 1000) % 60);
    demo_seek_play(dir, base, off_ms > 1000 ? info.first_time + off_ms : 0, info.first_time, label);
}

}  // namespace

void demo_seek_frame() {
    poll_goto_cvar();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    if (g_last.QuadPart) {
        double ms = (double)(now.QuadPart - g_last.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        if (ms > 0.5 && ms < 200) g_frame_ms = g_frame_ms * 0.8 + ms * 0.2;
    }
    g_last = now;

    switch (g.state) {
    case IDLE:
        return;
    case WAIT_PLAY:
        if (GetTickCount() - g.t_start > 45000) {
            logger::logf("demo_seek: the demo never started (45 s) - gave up");
            g.state = IDLE;
            return;
        }
        if (!demo_playing() || !playing_ours() || !snap_valid() || snap_time() <= 0) return;
        if (g.target - snap_time() < 1500) {                 // already there
            logger::logf("demo_seek: nothing to skip (at %d, target %d)", snap_time(), g.target);
            g.state = IDLE;
            return;
        }
        g.from = snap_time();
        g.t_seek = GetTickCount();
        mute();
        g.state = SEEKING;
        logger::logf("demo_seek: fast-forward from %d to %d", g.from, g.target);
        return;
    case SEEKING: {
        if (!demo_playing()) {
            finish("the demo stopped during the fast-forward");
            g.state = IDLE;
            return;
        }
        // a gamestate inside the demo (map change) zeroes cl.snap: keep going
        if (!snap_valid() || snap_time() <= 0) return;
        const int t = snap_time();
        const int remaining = g.target - t;
        const int span = g.target - g.from;
        g.progress = span > 0 ? (float)(t - g.from) / (float)span : 1.0f;
        if (g.progress < 0) g.progress = 0;
        if (g.progress > 1) g.progress = 1;
        if (remaining <= 30) {
            char why[128];
            snprintf(why, sizeof(why), "reached %d (target %d, %+d ms) in %lu ms, %d timescale steps",
                     t, g.target, t - g.target, GetTickCount() - g.t_seek, g.steps);
            finish(why);
            g.state = SETTLE;
            g.t_settle = GetTickCount();
            return;
        }
        // each frame covers about half of what is left: fast, and never past the target
        float ts = (float)(remaining / (2.0 * (g_frame_ms > 1.0 ? g_frame_ms : 1.0)));
        if (ts < 1) ts = 1;
        if (ts > 400) ts = 400;
        set_timescale(ts);
        ++g.steps;
        return;
    }
    case SETTLE:
        if (GetTickCount() - g.t_settle > 350) g.state = IDLE;
        return;
    }
}

bool demo_seek_curtain_visible() {
    return g.state == SEEKING || g.state == SETTLE;
}

void demo_seek_draw_curtain(float w, float h) {
    float a = 1.0f;
    if (g.state == SETTLE) {
        a = 1.0f - (float)(GetTickCount() - g.t_settle) / 350.0f;
        if (a < 0) a = 0;
    }
    ui_alpha(a);
    ui_rect(0, 0, w, h, 0xFF050505);
    const float cx = w / 2;
    const float cy = h / 2;
    const char* eyebrow = "D E M O";
    ui_text(cx - ui_text_width(12, 600, eyebrow) / 2, cy - 92, 12, 600, 0xFF6B7280, eyebrow);
    const char* title = g.label.empty() ? "Jumping to the moment" : g.label.c_str();
    ui_text(cx - ui_text_width(30, 600, title) / 2, cy - 62, 30, 600, 0xFFF5F5F5, title);
    const char* sub = "fast-forwarding the demo - sound off until it lands";
    ui_text(cx - ui_text_width(14, 400, sub) / 2, cy - 14, 14, 400, 0xFF7E7E7E, sub);
    const float bw = w * 0.32f < 360 ? 360 : w * 0.32f;
    ui_rect_rounded(cx - bw / 2, cy + 22, bw, 4, 2, 0xFF1C1C1C);
    ui_rect_rounded(cx - bw / 2, cy + 22, bw * (g.state == SETTLE ? 1.0f : g.progress), 4, 2, 0xFFF2F2F2);
    ui_alpha(1.0f);
}

void demo_seek_restore_now() {
    if (g.state == SEEKING || g.muted) finish("restored at exit");
    g.state = IDLE;
}

void demo_seek_startup_recover() {
    const std::string mp = marker_path();
    FILE* f = mp.empty() ? nullptr : fopen(mp.c_str(), "rb");
    if (!f) return;
    char v[32] = {};
    const size_t n = fread(v, 1, sizeof(v) - 1, f);
    fclose(f);
    v[n] = 0;
    const double vol = atof(v);
    if (vol > 0.0 && vol <= 1.0 && config_mp_set("mss_volume", v)) {
        logger::logf("demo_seek: the last session ended during a demo fast-forward - mss_volume put back to %s", v);
    } else {
        logger::logf("demo_seek: stale %s ignored (\"%s\")", kMarker, v);
    }
    DeleteFileA(mp.c_str());
}

}  // namespace patches
