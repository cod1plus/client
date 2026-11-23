// bench.cpp - see bench.h
#include "performance/bench.h"
#include "performance/smodel_lod.h"
#include "performance/smodel_path.h"
#include "core/logger.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
extern volatile long cod1x_sl_calls;
extern volatile long cod1x_sl_verts;
extern volatile long cod1x_sl_big;
extern volatile long cod1x_sp_draws;
extern volatile long cod1x_sp_dverts;
extern volatile long long cod1x_sp_ticks;
extern volatile long cod1x_sp_calls[13];
extern volatile long cod1x_sp_verts[13];
extern volatile long cod1x_tc_calls;
extern long long cod1x_swap_ticks;
extern long long cod1x_fence_ticks;
}

namespace patches {
namespace {

constexpr uintptr_t RE_COPY          = 0x0148baa0;   // the client's refexport_t
constexpr uintptr_t SLOT_RENDERSCENE = RE_COPY + 0x64;
constexpr uintptr_t SLOT_ENDFRAME    = RE_COPY + 0x8c;
constexpr uintptr_t FN_RENDERSCENE   = 0x004e0ac0;
constexpr uintptr_t FN_ENDFRAME      = 0x004eaec0;
constexpr uintptr_t QGL_DRAWELEMENTS = 0x0199c628;
constexpr uintptr_t QGL_DRAWRANGE    = 0x0199c9a8;   // glDrawRangeElements(EXT): what r_ext_draw_range_elements 1 uses (0x4c4da3, 0x4cb16e)
constexpr uintptr_t CVAR_GET         = 0x0043b880;
constexpr uintptr_t CBUF_EXECTEXT    = 0x0042a180;
// SCR_DrawScreenField: "test byte [cls.keyCatchers 0x15b8aa4], 2; je" (74 1a) then VM_Call(uivm, UI_REFRESH).
// Mode 3 makes the je a jmp (eb) while it runs: no menu over the screenshots.
constexpr uintptr_t VA_UI_DRAW_JE    = 0x00418a72;

typedef void (__cdecl* RenderScene_t)(void*);
typedef void (__cdecl* EndFrame_t)(int*, int*);
typedef void (__stdcall* DrawElements_t)(unsigned, int, unsigned, const void*);
typedef void (__stdcall* DrawRange_t)(unsigned, unsigned, unsigned, int, unsigned, const void*);
typedef void* (__cdecl* CvarGet_t)(const char*, const char*, int);
typedef void (__cdecl* CbufExec_t)(int, const char*);

LONGLONG g_freq = 0;
inline double ms(LONGLONG t) { return 1000.0 * (double)t / (double)g_freq; }

// ---------------------------------------------------------------- per-frame accumulation (main thread)
LONGLONG g_release = 0;
LONGLONG g_scene = 0, g_end = 0, g_draw = 0;
long g_draws = 0, g_indices = 0;
bool g_world_seen = false;
DrawElements_t g_real_draw = nullptr;
DrawRange_t g_real_range = nullptr;

// ---------------------------------------------------------------- the camera
bool g_cam_on = false;
float g_cam_org[3] = {0, 0, 0};
float g_cam_axis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

void set_camera(const float org[3], float yaw, float pitch) {
    const float y = yaw * 3.14159265f / 180.0f, p = pitch * 3.14159265f / 180.0f;
    const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    g_cam_org[0] = org[0]; g_cam_org[1] = org[1]; g_cam_org[2] = org[2];
    // Q3 AnglesToAxis (roll 0): forward, left, up
    g_cam_axis[0] = cp * cy; g_cam_axis[1] = cp * sy; g_cam_axis[2] = -sp;
    g_cam_axis[3] = -sy;     g_cam_axis[4] = cy;      g_cam_axis[5] = 0.0f;
    g_cam_axis[6] = sp * cy; g_cam_axis[7] = sp * sy; g_cam_axis[8] = cp;
}

void __cdecl render_scene_wrap(void* fd) {
    uint8_t* r = (uint8_t*)fd;
    const bool world = r && !(*(const int*)(r + 0x4c) & 1);
    float save[12];
    const bool cam = g_cam_on && world;
    if (cam) {
        memcpy(save, r + 0x18, sizeof save);                 // vieworg[3] + viewaxis[3][3]
        memcpy(r + 0x18, g_cam_org, 12);
        memcpy(r + 0x24, g_cam_axis, 36);
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ((RenderScene_t)FN_RENDERSCENE)(fd);
    QueryPerformanceCounter(&b);
    g_scene += b.QuadPart - a.QuadPart;
    if (cam) memcpy(r + 0x18, save, sizeof save);
    if (world) g_world_seen = true;
}

void __cdecl end_frame_wrap(int* front, int* back) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ((EndFrame_t)FN_ENDFRAME)(front, back);
    QueryPerformanceCounter(&b);
    g_end += b.QuadPart - a.QuadPart;
}

void __stdcall draw_elements_wrap(unsigned mode, int count, unsigned type, const void* indices) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_real_draw(mode, count, type, indices);
    QueryPerformanceCounter(&b);
    g_draw += b.QuadPart - a.QuadPart;
    ++g_draws;
    g_indices += count;
}

void __stdcall draw_range_wrap(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_real_range(mode, start, end, count, type, indices);
    QueryPerformanceCounter(&b);
    g_draw += b.QuadPart - a.QuadPart;
    ++g_draws;
    g_indices += count;
}

void ensure_hooks() {
    uintptr_t* rs = (uintptr_t*)SLOT_RENDERSCENE;
    uintptr_t* ef = (uintptr_t*)SLOT_ENDFRAME;
    static bool logged = false;
    if (*rs == FN_RENDERSCENE) *rs = (uintptr_t)&render_scene_wrap;
    if (*ef == FN_ENDFRAME) *ef = (uintptr_t)&end_frame_wrap;
    // (no GL pointer hooks: wrapping the renderer's qgl slots 0x199c778 / 0x199c2b0 crashed the map load with an
    //  access violation - the draw-call count stays 0, the backend time is what the EndFrame wrapper measures)
    if (!logged && *rs == (uintptr_t)&render_scene_wrap && *ef == (uintptr_t)&end_frame_wrap) {
        logged = true;
        logger::logf("bench: frame budget timers on (re.RenderScene 0x%08x, re.EndFrame 0x%08x, qglDrawElements 0x%08x); "
                     "cod1x_bench 1|2|3 + cod1x_bench_views.txt = camera benchmark", (unsigned)SLOT_RENDERSCENE, (unsigned)SLOT_ENDFRAME,
                     (unsigned)QGL_DRAWELEMENTS);
    }
}

// ---------------------------------------------------------------- 10 s budget line
struct Win {
    std::vector<float> work;      // CPU work per frame = frame work minus the time blocked in SwapBuffers and the GPU fence
    double raw = 0, scene = 0, end = 0, present = 0, fence = 0;
    void clear() { work.clear(); raw = scene = end = present = fence = 0; }
};
Win g_win;
ULONGLONG g_win_start = 0;

double pct(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::min<double>((double)v.size() - 1, std::floor(q * (double)(v.size() - 1) + 0.5));
    return v[i];
}

void log_window(const Win& w, const char* tag) {
    const size_t n = w.work.size();
    if (!n) return;
    double sum = 0, mx = 0;
    size_t over = 0;
    for (float x : w.work) { sum += x; mx = std::max<double>(mx, x); if (x > 4.0f) ++over; }
    const double avg = sum / n, sc = w.scene / n, pr = w.present / n, fe = w.fence / n, en = w.end / n - pr - fe;
    logger::logf("%s: %zu frames, CPU work avg %.2f ms (p50 %.2f, p99 %.2f, max %.2f), %.1f%% over 4.0 ms = scene %.2f + backend %.2f "
                 "+ rest %.2f | blocked: present %.2f, GPU fence %.2f (raw frame work %.2f)",
                 tag, n, avg, pct(w.work, 0.5), pct(w.work, 0.99), mx, 100.0 * over / n, sc, en, avg - sc - en, pr, fe, w.raw / n);
}

// ---------------------------------------------------------------- the camera benchmark
struct View { float org[3]; float yaw, pitch; int settle, measure; };
struct Result {
    View v; Win w;
    long sl_calls = 0, sl_big = 0, sl_verts = 0, traces = 0;
    long draws = 0, dverts = 0, cpu_surf = 0, cpu_verts = 0;   // smodel_path: ARB VBO draws, CPU-path surfaces
    long long vbo_ticks = 0;                                   // smodel_path: time inside those draws
};
enum State { B_IDLE, B_WARMUP, B_RUN, B_DONE };
State g_state = B_IDLE;
std::vector<View> g_views;
std::vector<Result> g_results;
size_t g_vi = 0;
int g_vf = 0, g_warm = 0;
long g_c0[8] = {0, 0, 0, 0, 0, 0, 0, 0};
long long g_t0 = 0;
void* g_cv_bench = nullptr;
int g_mode = 0;
char g_tag[32] = "";   // mode 3: screenshot names bench_<hhmmss>_vNN
char g_draw2d[16] = "";   // mode 3: cg_draw2D before, put back at the end
bool g_ui_hidden = false;

// mode 3: no menu (UI_REFRESH skipped) and no HUD (cg_draw2D 0) over the screenshots; false puts both back
void hide_2d(bool hide) {
    uint8_t* p = (uint8_t*)VA_UI_DRAW_JE;
    if (hide == g_ui_hidden || p[1] != 0x1a || (p[0] != 0x74 && p[0] != 0xeb)) return;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = hide ? 0xeb : 0x74;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    g_ui_hidden = hide;
    char cmd[48];
    if (hide) {
        void* cv = ((CvarGet_t)CVAR_GET)("cg_draw2D", "1", 0);
        const char* v = cv ? *(const char* const*)((const uint8_t*)cv + 4) : "1";
        snprintf(g_draw2d, sizeof g_draw2d, "%s", v && *v ? v : "1");
        snprintf(cmd, sizeof cmd, "set cg_draw2D 0\n");
    } else {
        snprintf(cmd, sizeof cmd, "set cg_draw2D %s\n", g_draw2d);
    }
    ((CbufExec_t)CBUF_EXECTEXT)(2, cmd);
}

bool load_views() {
    g_views.clear();
    FILE* f = fopen("cod1x_bench_views.txt", "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        View v{};
        v.settle = 30; v.measure = 90;
        const int n = sscanf(line, "%f %f %f %f %f %d %d", &v.org[0], &v.org[1], &v.org[2], &v.yaw, &v.pitch, &v.settle, &v.measure);
        if (n >= 4) { if (n < 5) v.pitch = 0; g_views.push_back(v); }
    }
    fclose(f);
    return !g_views.empty();
}

void counters(long out[8]) {
    out[0] = cod1x_sl_calls; out[1] = cod1x_sl_big; out[2] = cod1x_sl_verts; out[3] = cod1x_tc_calls;
    out[4] = cod1x_sp_draws; out[5] = cod1x_sp_dverts; out[6] = cod1x_sp_calls[0]; out[7] = cod1x_sp_verts[0];
}

void finish() {
    g_cam_on = false;
    hide_2d(false);
    g_state = B_DONE;
    Win all;
    std::vector<std::pair<double, size_t>> order;
    for (size_t i = 0; i < g_results.size(); ++i) {
        const Win& w = g_results[i].w;
        for (float x : w.work) all.work.push_back(x);
        all.raw += w.raw; all.scene += w.scene; all.end += w.end; all.present += w.present; all.fence += w.fence;
        double s = 0;
        for (float x : w.work) s += x;
        order.push_back({w.work.empty() ? 0 : s / w.work.size(), i});
    }
    log_window(all, "bench SUMMARY");
    std::sort(order.rbegin(), order.rend());
    for (size_t k = 0; k < order.size() && k < 12; ++k) {
        const Result& r = g_results[order[k].second];
        logger::logf("bench worst #%zu: view %zu (%.0f %.0f %.0f yaw %.0f) work avg %.2f ms, max %.2f", k + 1, order[k].second + 1,
                     r.v.org[0], r.v.org[1], r.v.org[2], r.v.yaw, order[k].first, pct(r.w.work, 1.0));
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof name, "cod1x_bench_%02d%02d%02d.csv", st.wHour, st.wMinute, st.wSecond);
    if (FILE* f = fopen(name, "w")) {
        fprintf(f, "view,x,y,z,yaw,pitch,frames,work_avg,work_p50,work_p95,work_max,scene,backend,present,fence,raw,rest,sl_calls,sl_small_lit,sl_verts,traces,vbo_draws,vbo_verts,cpu_surf,cpu_verts,vbo_ms\n");
        for (size_t i = 0; i < g_results.size(); ++i) {
            const Result& r = g_results[i];
            const size_t n = r.w.work.size();
            if (!n) continue;
            double s = 0;
            for (float x : r.w.work) s += x;
            const double avg = s / n;
            fprintf(f, "%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.3f,%.1f,%.1f,%.0f,%.0f,%.1f,%.0f,%.1f,%.0f,%.3f\n", i + 1,
                    r.v.org[0], r.v.org[1], r.v.org[2], r.v.yaw, r.v.pitch, n, avg, pct(r.w.work, 0.5), pct(r.w.work, 0.95), pct(r.w.work, 1.0),
                    r.w.scene / n, r.w.end / n - r.w.present / n - r.w.fence / n, r.w.present / n, r.w.fence / n, r.w.raw / n,
                    avg - r.w.scene / n - (r.w.end / n - r.w.present / n - r.w.fence / n),
                    (double)r.sl_calls / n, (double)(r.sl_calls - r.sl_big) / n, (double)r.sl_verts / n, (double)r.traces / n,
                    (double)r.draws / n, (double)r.dverts / n, (double)r.cpu_surf / n, (double)r.cpu_verts / n,
                    ms(r.vbo_ticks) / n);
        }
        fclose(f);
        logger::logf("bench: %zu views written to %s", g_results.size(), name);
    }
    if (g_mode >= 2) ((CbufExec_t)CBUF_EXECTEXT)(2, "quit\n");
}

void bench_step(double work, double raw, double scene, double end, double present, double fence) {
    if (!g_cv_bench) g_cv_bench = ((CvarGet_t)CVAR_GET)("cod1x_bench", "0", 0);
    const int mode = g_cv_bench ? *(const int*)((const uint8_t*)g_cv_bench + 0x20) : 0;
    if (mode == 0) {
        if (g_state != B_IDLE && g_state != B_DONE) { g_cam_on = false; hide_2d(false); g_state = B_IDLE; logger::logf("bench: stopped"); }
        if (g_state == B_DONE) g_state = B_IDLE;
        return;
    }
    switch (g_state) {
    case B_IDLE:
        if (!g_world_seen) return;
        if (!load_views()) {
            logger::logf("bench: cod1x_bench %d but no views in cod1x_bench_views.txt - off", mode);
            g_state = B_DONE;
            return;
        }
        g_mode = mode;
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(g_tag, sizeof g_tag, "bench_%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
        }
        g_results.clear();
        g_state = B_WARMUP;
        g_warm = 0;
        logger::logf("bench: %zu views loaded, warming up", g_views.size());
        return;
    case B_WARMUP:
        if (++g_warm < 500) return;
        g_state = B_RUN;
        g_vi = 0; g_vf = 0;
        if (g_mode == 3) hide_2d(true);
        set_camera(g_views[0].org, g_views[0].yaw, g_views[0].pitch);
        g_cam_on = true;
        return;
    case B_RUN: {
        const View& v = g_views[g_vi];
        if (g_vf == v.settle && g_mode == 3) {   // the next frame runs it, the camera still on this view
            char cmd[80];
            snprintf(cmd, sizeof cmd, "screenshotJPEG %s_v%02u\n", g_tag, (unsigned)(g_vi + 1));
            ((CbufExec_t)CBUF_EXECTEXT)(2, cmd);
        }
        if (g_vf == v.settle) {
            g_results.push_back(Result());
            g_results.back().v = v;
            counters(g_c0);
            g_t0 = cod1x_sp_ticks;
        }
        if (g_vf >= v.settle) {
            Result& r = g_results.back();
            r.w.work.push_back((float)work);
            r.w.raw += raw; r.w.scene += scene; r.w.end += end; r.w.present += present; r.w.fence += fence;
        }
        if (++g_vf >= v.settle + v.measure) {
            Result& r = g_results.back();
            long c1[8];
            counters(c1);
            r.vbo_ticks = cod1x_sp_ticks - g_t0;
            r.sl_calls = c1[0] - g_c0[0]; r.sl_big = c1[1] - g_c0[1]; r.sl_verts = c1[2] - g_c0[2]; r.traces = c1[3] - g_c0[3];
            r.draws = c1[4] - g_c0[4]; r.dverts = c1[5] - g_c0[5]; r.cpu_surf = c1[6] - g_c0[6]; r.cpu_verts = c1[7] - g_c0[7];
            const size_t n = r.w.work.size();
            double s = 0;
            for (float x : r.w.work) s += x;
            logger::logf("bench view %zu/%zu (%.0f %.0f %.0f yaw %.0f): CPU work %.2f ms (max %.2f) | scene %.2f backend %.2f | blocked present %.2f "
                         "fence %.2f | smodel calls %.0f small-lit %.1f | traces %.0f",
                         g_vi + 1, g_views.size(), v.org[0], v.org[1], v.org[2], v.yaw, s / n, pct(r.w.work, 1.0), r.w.scene / n,
                         (r.w.end - r.w.present - r.w.fence) / n, r.w.present / n, r.w.fence / n, (double)r.sl_calls / n,
                         (double)(r.sl_calls - r.sl_big) / n, (double)r.traces / n);
            if (++g_vi >= g_views.size()) { finish(); return; }
            g_vf = 0;
            set_camera(g_views[g_vi].org, g_views[g_vi].yaw, g_views[g_vi].pitch);
        }
        return;
    }
    case B_DONE:
        return;
    }
}

}  // namespace

void bench_frame_end(long long qpc) {
    g_release = qpc;
    cod1x_swap_ticks = 0;
    cod1x_fence_ticks = 0;
    g_scene = g_end = g_draw = 0;
    g_draws = g_indices = 0;
}

void bench_frame_begin(long long qpc) {
    if (!g_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    ensure_hooks();
    smodel_lod_frame();
    smodel_path_frame();
    if (!g_release) return;
    const double raw = ms(qpc - g_release), scene = ms(g_scene), end = ms(g_end);
    const double present = ms(cod1x_swap_ticks), fence = ms(cod1x_fence_ticks);
    const double work = raw - present - fence;
    if (g_world_seen && raw < 250.0 && raw > 0.05) {   // a map load / alt-tab is not a frame, a limiter re-loop not one either
        g_win.work.push_back((float)work);
        g_win.raw += raw; g_win.scene += scene; g_win.end += end; g_win.present += present; g_win.fence += fence;
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_win_start) g_win_start = now;
    if (now - g_win_start >= 10000) {
        log_window(g_win, "frame budget (last 10 s)");
        g_win.clear();
        g_win_start = now;
    }
    bench_step(work, raw, scene, end, present, fence);
}

}  // namespace patches
