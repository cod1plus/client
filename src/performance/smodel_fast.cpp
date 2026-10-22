// smodel_fast.cpp - see smodel_fast.h
//
// CoDMP.exe 1.5 (disassembled 2026-10-23):
//   0x4e0800  RE_AddRefEntityToScene(ent [esp+4], flag [esp+8]): if [0x19b0b9c] (entities in
//             the list) < 0x3fe -> plain add at 0x4e088e; else the warning once per frame
//             ([0x11fbf70] = frameCount [0x199cf28] guard, Com_Printf 2 "too many visible
//             models (more than %i)"), then `call 0x4e06e0` at 0x4e086f: eax < 0 -> return
//             (the entity is dropped), else 0x4e07c0(eax, ent) copies the 0x27 dwords of ent
//             over slot eax.
//   0x4e06e0  no arguments. ebx = [0x19b0b9c] count, esi = [0x19b0bbc] first slot of the
//             scene, slots at [0x1999e34] + i*0x2b8 + 0x81700 (reType at +0, origin at
//             +0x44); skips reType 0 and 1 (DObj = players), dist = sqrt(|vieworg
//             [0x199d830] - origin|^2) (0x5727c0), keeps the max, returns its index or -1.
#include "performance/smodel_fast.h"
#include "core/logger.h"
#include "features/settings_menu.h"      // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA, CVAR_ARCHIVE

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace patches {

namespace {

constexpr uintptr_t VA_CALLSITE   = 0x004e086f;   // E8 rel32 -> 0x4e06e0, inside RE_AddRefEntityToScene
constexpr uintptr_t VA_FIND_FAR   = 0x004e06e0;
constexpr uintptr_t VA_ENT_COUNT  = 0x019b0b9c;
constexpr uintptr_t VA_ENT_FIRST  = 0x019b0bbc;
constexpr uintptr_t VA_ENT_BASE   = 0x01999e34;
constexpr uintptr_t VA_VIEWORG    = 0x0199d830;
constexpr uintptr_t VA_FRAMECOUNT = 0x0199cf28;
constexpr int SLOT_SIZE = 0x2b8, SLOT_OFF = 0x81700, RE_ORIGIN = 0x44;
constexpr int MAX_SLOTS = 0x400;
constexpr int CV_INT = 0x20;

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef int   (__cdecl* find_far_t)(void);

bool  g_installed = false;
void* g_cv_fast = nullptr;
volatile LONG g_fast = 1;                  // cvar value, polled by the watcher
LARGE_INTEGER g_freq;

// ---- the heap: (squared distance, slot) with the farthest on top ---------------------------
float g_d2[MAX_SLOTS];
int   g_idx[MAX_SLOTS];
int   g_n = 0;
int   g_frame = -1;        // frameCount the heap belongs to
int   g_count = -1;        // list count it was built for
int   g_first = -1;        // and the scene's first slot (a second scene in the frame rebuilds)
int   g_last_slot = -1;    // the slot handed out last time: the engine copied the new model there

inline const uint8_t* slot_ptr(int i) {
    return *(const uint8_t* const*)VA_ENT_BASE + (size_t)i * SLOT_SIZE + SLOT_OFF;
}
inline float dist2(const float* org) {
    const float* v = (const float*)VA_VIEWORG;
    const float dx = v[0] - org[0], dy = v[1] - org[1], dz = v[2] - org[2];
    return dx * dx + dy * dy + dz * dz;
}
inline void heap_push(float d, int i) {
    int k = g_n++;
    while (k > 0) {
        const int p = (k - 1) >> 1;
        if (g_d2[p] >= d) break;
        g_d2[k] = g_d2[p]; g_idx[k] = g_idx[p];
        k = p;
    }
    g_d2[k] = d; g_idx[k] = i;
}
inline int heap_pop() {                     // the farthest; g_n > 0
    const int top = g_idx[0];
    --g_n;
    if (g_n > 0) {
        const float d = g_d2[g_n]; const int i = g_idx[g_n];
        int k = 0;
        for (;;) {
            int c = 2 * k + 1;
            if (c >= g_n) break;
            if (c + 1 < g_n && g_d2[c + 1] > g_d2[c]) ++c;
            if (g_d2[c] <= d) break;
            g_d2[k] = g_d2[c]; g_idx[k] = g_idx[c];
            k = c;
        }
        g_d2[k] = d; g_idx[k] = i;
    }
    return top;
}
void heap_build(int first, int count) {
    g_n = 0;
    for (int i = first; i < count && g_n < MAX_SLOTS; ++i) {
        const uint8_t* s = slot_ptr(i);
        const int t = *(const int*)s;
        if (t == 0 || t == 1) continue;
        heap_push(dist2((const float*)(s + RE_ORIGIN)), i);
    }
}

// ---- per-frame and per-window accounting --------------------------------------------------
struct Frame { long calls; long dropped; LONGLONG ticks; };
Frame g_cur = {};
volatile long g_fb_calls = 0; LONGLONG g_fb_ticks = 0;   // since the last stall_trace poll
int   g_cur_frame = -1;
struct Window { long frames; long calls; long calls_max; long dropped; double ms_sum; double ms_max; };
Window g_win = {};
CRITICAL_SECTION g_win_lock;

void frame_close() {
    if (g_cur_frame < 0 || g_cur.calls == 0) return;
    const double ms = g_cur.ticks * 1000.0 / (double)g_freq.QuadPart;
    EnterCriticalSection(&g_win_lock);
    g_win.frames++;
    g_win.calls += g_cur.calls;
    if (g_cur.calls > g_win.calls_max) g_win.calls_max = g_cur.calls;
    g_win.dropped += g_cur.dropped;
    g_win.ms_sum += ms;
    if (ms > g_win.ms_max) g_win.ms_max = ms;
    LeaveCriticalSection(&g_win_lock);
    g_cur = Frame();
}

}  // namespace

// Called in place of 0x4e06e0 with the caller's entity (the model being added) - see the
// naked hook below. Returns the slot to overwrite, or -1 = do not add this one.
extern "C" int __cdecl smodel_find_slot(const uint8_t* ent) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const int frame = *(const int*)VA_FRAMECOUNT;
    if (frame != g_cur_frame) { frame_close(); g_cur_frame = frame; }
    g_cur.calls++;

    int r;
    if (!g_fast) {
        r = ((find_far_t)VA_FIND_FAR)();                       // the engine's own scan, timed
        g_frame = -1;                                           // a later switch rebuilds
    } else {
        const int count = *(const int*)VA_ENT_COUNT;
        const int first = *(const int*)VA_ENT_FIRST;
        if (frame != g_frame || count != g_count || first != g_first) {
            heap_build(first, count);
            g_frame = frame; g_count = count; g_first = first; g_last_slot = -1;
        } else if (g_last_slot >= 0) {
            // the engine copied the previous model into that slot: it is in the list now
            const uint8_t* s = slot_ptr(g_last_slot);
            const int t = *(const int*)s;
            if (t != 0 && t != 1 && g_n < MAX_SLOTS) heap_push(dist2((const float*)(s + RE_ORIGIN)), g_last_slot);
            g_last_slot = -1;
        }
        // EXACTLY the engine's choice (the farthest replaceable slot), never a different set of
        // models on screen: which props show is the game's business, not the DLL's (10-23)
        if (g_n == 0) {
            r = -1;
        } else {
            r = heap_pop();
            g_last_slot = r;
        }
    }
    QueryPerformanceCounter(&t1);
    g_cur.ticks += t1.QuadPart - t0.QuadPart;
    g_fb_calls++; g_fb_ticks += t1.QuadPart - t0.QuadPart;
    return r;
}

namespace {

// At the call site the caller's own argument (the entity) sits at [esp+4]; our call pushed
// the return address on top, so it is [esp+8] here. ebx/esi/edi/ebp are preserved by the
// cdecl callee, eax carries the result, ecx/edx are reloaded by the caller afterwards.
__attribute__((naked)) void find_hook() {
    __asm__ volatile(
        "movl 8(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "call _smodel_find_slot\n\t"
        "addl $4, %esp\n\t"
        "ret\n\t");
}

bool patch_callsite(uintptr_t site, uintptr_t expect, uintptr_t to) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return false;
    int32_t rel; memcpy(&rel, p + 1, 4);
    if ((uintptr_t)(site + 5 + rel) != expect) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = (int32_t)(to - (site + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void smodel_fast_install() {
    if (g_installed) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("smodel_fast: CoDMP.exe not at its preferred base - not installed");
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    InitializeCriticalSection(&g_win_lock);
    if (!patch_callsite(VA_CALLSITE, VA_FIND_FAR, (uintptr_t)&find_hook)) {
        logger::logf("smodel_fast: the overflow call at 0x%08x is not `call 0x%08x` - not installed",
                     (unsigned)VA_CALLSITE, (unsigned)VA_FIND_FAR);
        return;
    }
    g_installed = true;
    logger::logf("smodel_fast: scene overflow (> 1021 models) replaced at 0x%08x - heap instead of the engine's "
                 "sqrt scan per model, the same slot picked (cod1x_smodel_fast 0 = engine scan, timed)", (unsigned)VA_CALLSITE);
}

void smodel_fast_tick() {
    if (!g_installed) return;
    if (!g_cv_fast) {
        g_cv_fast = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_smodel_fast", "1", CVAR_ARCHIVE);
        if (!g_cv_fast) return;
    }
    const LONG f = *(const int*)((const uint8_t*)g_cv_fast + CV_INT) ? 1 : 0;
    if (f != g_fast) { InterlockedExchange(&g_fast, f); logger::logf("smodel_fast: %s", f ? "heap" : "engine scan (timed)"); }
}

void smodel_fast_report(char* buf, size_t n, bool reset) {
    buf[0] = 0;
    if (!g_installed) return;
    EnterCriticalSection(&g_win_lock);
    const Window w = g_win;
    if (reset) g_win = Window();
    LeaveCriticalSection(&g_win_lock);
    if (!w.frames) return;
    snprintf(buf, n, "scene > 1021 modeles: %ld images, %ld remplacements (max %ld/image, %ld refuses), "
                     "%.3f ms/image moy, %.2f ms max, %s",
             w.frames, w.calls, w.calls_max, w.dropped, w.ms_sum / w.frames, w.ms_max,
             g_fast ? "tas" : "scan moteur");
}

void smodel_fast_poll_frame(long* calls, double* ms) {
    *calls = g_fb_calls;
    *ms = g_freq.QuadPart ? (double)g_fb_ticks * 1000.0 / (double)g_freq.QuadPart : 0.0;
    g_fb_calls = 0; g_fb_ticks = 0;
}

}  // namespace patches
