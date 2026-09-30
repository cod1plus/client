// inspect.cpp - see inspect.h
#include "gameplay/inspect.h"
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA
#include "netcode/competitive.h"         // CODMP_CVAR_SET_VA
#include "core/logger.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace patches {
namespace {
constexpr uintptr_t CALL_RVA   = 0x38f41;     // CG_AddViewWeapon -> 0x30038660 (E8 rel32)
constexpr uintptr_t TARGET_RVA = 0x38660;     // 55 8b ec 83 e4 f8
constexpr uintptr_t VIEWAXIS_RVA = 0x20d354;  // cg.refdef.viewaxis: forward, left, up
constexpr int RE_AXIS = 0x1c, RE_ORIGIN = 0x44;
constexpr int PS_WEAPON = 0xb0, PS_WEAPONSTATE = 0xb4, PS_ADSFRAC = 0xb8;   // the crosshair hides on ps+0xb4 = 1/2/5/10/11
constexpr int CV_STRING = 0x04, CV_VALUE = 0x1c, CV_INT = 0x20;

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);

// the moves, in view axes: yaw > 0 = muzzle to the left, pitch > 0 = muzzle up, roll < 0 =
// top of the gun toward the screen centre (shows its right side); f/l/u = slide, units
struct Key { float t, yaw, pitch, roll, f, l, u; };
const Key KEYS[] = {
    { 0.00f,   0,  0,   0, 0, 0,   0   },
    { 0.55f,  26,  8, -42, 2, 3,   1.5f },   // right side up to the camera
    { 1.40f,  21,  5, -50, 2, 3,   1.5f },   // slow drift
    { 2.00f, -12, 18,  26, 1, 1,   1   },   // flip: left side and underside
    { 2.60f,  -8, 14,  32, 1, 1,   1   },
    { 3.20f,   0,  0,   0, 0, 0,   0   },
};
constexpr int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);

uintptr_t g_base = 0;
void* g_orig = nullptr;
void* g_cv_go = nullptr;
void* g_cv_pivot = nullptr;
void* g_cv_speed = nullptr;
bool g_active = false;
LARGE_INTEGER g_t0, g_freq;
int g_weapon = 0, g_state = 0;

float now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (float)((double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart);
}
float smooth(float x) { return x * x * (3.0f - 2.0f * x); }

// v (world) -> view-local (f, l, u) -> rotated -> world
void turn(const float* F, const float* L, const float* U, const float R[3][3], const float* v, float* out) {
    const float x = v[0] * F[0] + v[1] * F[1] + v[2] * F[2];
    const float y = v[0] * L[0] + v[1] * L[1] + v[2] * L[2];
    const float z = v[0] * U[0] + v[1] * U[1] + v[2] * U[2];
    const float a = R[0][0] * x + R[0][1] * y + R[0][2] * z;
    const float b = R[1][0] * x + R[1][1] * y + R[1][2] * z;
    const float c = R[2][0] * x + R[2][1] * y + R[2][2] * z;
    for (int i = 0; i < 3; ++i) out[i] = a * F[i] + b * L[i] + c * U[i];
}

__attribute__((force_align_arg_pointer)) void __cdecl inspect_apply(uint8_t* ref, const uint8_t* ps) {
    if (!g_cv_go) {
        g_cv_go    = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect", "0", 0);
        g_cv_pivot = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_pivot", "14 -5 -5", 0);
        g_cv_speed = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_speed", "1", 0);
        QueryPerformanceFrequency(&g_freq);
        if (!g_cv_go) return;
    }
    if (!ref || !ps) return;
    const int weapon = *(const int*)(ps + PS_WEAPON);
    const int state = *(const int*)(ps + PS_WEAPONSTATE);
    const float ads = *(const float*)(ps + PS_ADSFRAC);
    if (*(const int*)((const uint8_t*)g_cv_go + CV_INT)) {
        ((Cvar_Set_t)CODMP_CVAR_SET_VA)("cod1x_inspect", "0");
        if (ads > 0.001f) {
            logger::logf("inspect: ignored while aiming (ads %.2f)", ads);
        } else {
            QueryPerformanceCounter(&g_t0);
            g_active = true; g_weapon = weapon; g_state = state;
            logger::logf("inspect: start (weapon %d, weaponstate %d, pivot \"%s\", speed %.2f)", weapon, state,
                         *(const char* const*)((const uint8_t*)g_cv_pivot + CV_STRING), *(const float*)((const uint8_t*)g_cv_speed + CV_VALUE));
        }
    }
    if (!g_active) return;
    const char* why = weapon != g_weapon ? "weapon switch" : state != g_state ? "weapon state changed (fire / reload)" :
                      ads > 0.001f ? "aiming" : nullptr;
    if (why) { g_active = false; logger::logf("inspect: cut - %s (weaponstate %d -> %d)", why, g_state, state); return; }

    float speed = *(const float*)((const uint8_t*)g_cv_speed + CV_VALUE);
    if (!(speed > 0.05f && speed < 20.0f)) speed = 1.0f;
    const float t = now_s() * speed;
    if (t >= KEYS[NKEYS - 1].t) { g_active = false; logger::logf("inspect: done"); return; }
    int k = 0;
    while (k < NKEYS - 2 && t >= KEYS[k + 1].t) ++k;
    const Key& A = KEYS[k];
    const Key& B = KEYS[k + 1];
    const float s = smooth((t - A.t) / (B.t - A.t));
    auto mix = [s](float a, float b) { return a + (b - a) * s; };
    const float d2r = 3.14159265f / 180.0f;
    const float yaw = mix(A.yaw, B.yaw) * d2r, pitch = mix(A.pitch, B.pitch) * d2r, roll = mix(A.roll, B.roll) * d2r;
    const float slide[3] = { mix(A.f, B.f), mix(A.l, B.l), mix(A.u, B.u) };

    // R = yaw (about up) * pitch (about left) * roll (about forward), local (f, l, u) coords
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    const float Ry[3][3] = { { cy, -sy, 0 }, { sy, cy, 0 }, { 0, 0, 1 } };
    const float Rp[3][3] = { { cp, 0, -sp }, { 0, 1, 0 }, { sp, 0, cp } };
    const float Rr[3][3] = { { 1, 0, 0 }, { 0, cr, -sr }, { 0, sr, cr } };
    float T[3][3], R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        T[i][j] = Rp[i][0] * Rr[0][j] + Rp[i][1] * Rr[1][j] + Rp[i][2] * Rr[2][j];
    }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        R[i][j] = Ry[i][0] * T[0][j] + Ry[i][1] * T[1][j] + Ry[i][2] * T[2][j];
    }
    const float* F = (const float*)(g_base + VIEWAXIS_RVA);
    const float* L = F + 3;
    const float* U = F + 6;
    float piv[3] = { 14, -5, -5 };
    sscanf(*(const char* const*)((const uint8_t*)g_cv_pivot + CV_STRING), "%f %f %f", &piv[0], &piv[1], &piv[2]);

    float* axis = (float*)(ref + RE_AXIS);
    for (int r = 0; r < 3; ++r) { float o[3]; turn(F, L, U, R, axis + 3 * r, o); memcpy(axis + 3 * r, o, sizeof(o)); }
    // origin' = origin + pivot - R pivot + slide   (turning about origin + pivot)
    float pw[3], rp[3];
    for (int i = 0; i < 3; ++i) pw[i] = piv[0] * F[i] + piv[1] * L[i] + piv[2] * U[i];
    turn(F, L, U, R, pw, rp);
    float* org = (float*)(ref + RE_ORIGIN);
    for (int i = 0; i < 3; ++i) org[i] += pw[i] - rp[i] + slide[0] * F[i] + slide[1] * L[i] + slide[2] * U[i];
}
void (__cdecl* g_apply_ptr)(uint8_t*, const uint8_t*) = inspect_apply;

// In place of `call 0x30038660`: ecx = hand refEntity, [esp+4] = ps. Every register and the
// flags are kept, then the original is entered with the caller's return address and arguments.
__attribute__((naked)) void stub() {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "pushl 40(%%esp)\n\t"          /* ps: [esp+4] + pushal (32) + pushfl (4) */
        "pushl %%ecx\n\t"
        "call *%0\n\t"
        "addl $8, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        : : "m"(g_apply_ptr), "m"(g_orig));
}
}  // namespace

bool inspect_install(HMODULE cgame) {
    if (!cgame) return false;
    const uintptr_t base = (uintptr_t)cgame;
    const uintptr_t site = base + CALL_RVA, target = base + TARGET_RVA;
    if (*(const uint8_t*)site != 0xE8) { logger::logf("inspect: call site opcode 0x%02x, expected E8 - not installed", *(const uint8_t*)site); return false; }
    const uintptr_t cur = site + 5 + (intptr_t)*(const int32_t*)(site + 1);
    if (cur == (uintptr_t)&stub) return true;
    const uint8_t pro[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8 };
    if (cur != target || memcmp((const void*)target, pro, 6) != 0) {
        logger::logf("inspect: call site targets 0x%x (expected 0x%x with its prologue) - not installed", (unsigned)cur, (unsigned)target);
        return false;
    }
    g_base = base;
    g_orig = (void*)target;
    g_active = false;
    const int32_t rel = (int32_t)((uintptr_t)&stub - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(site + 1) = rel;
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    logger::logf("inspect: view weapon hooked (cgame+0x%x) - `set cod1x_inspect 1` to inspect", (unsigned)CALL_RVA);
    return true;
}

}  // namespace patches
