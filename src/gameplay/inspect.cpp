// inspect.cpp - see inspect.h
#include "gameplay/inspect.h"
#include "gameplay/inspect_grips.h"   // how the left hand holds each magazine (from the reload anims)
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
constexpr uintptr_t SYSCALL_PTR_RVA = 0x76898; // the engine's syscall entry, written at cgame init
constexpr uintptr_t WEAPONS_RVA = 0x1aa4e0;   // cg_weapons[], 0x198 each; +0 = the view DObj (hands + weapon)
constexpr int WEAPON_STRIDE = 0x198;
constexpr intptr_t SC_BONE_INDEX = 0xaf;      // (dobj, name) -> bone index, -1 when absent (CoDMP 0x487b90)
constexpr int RE_AXIS = 0x1c, RE_ORIGIN = 0x44;
constexpr int PS_WEAPON = 0xb0, PS_WEAPONSTATE = 0xb4, PS_ADSFRAC = 0xb8;   // the crosshair hides on ps+0xb4 = 1/2/5/10/11
constexpr int CV_STRING = 0x04, CV_VALUE = 0x1c, CV_INT = 0x20;

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef intptr_t (*Syscall_t)(intptr_t, ...);

// ---- vectors and quaternions, view-local frame: x forward, y left, z up ----
struct V3 { float x, y, z; };
struct Q  { float w, x, y, z; };
V3 add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 scl(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 norm(V3 a) { const float l = sqrtf(dot(a, a)); return l > 1e-6f ? scl(a, 1.0f / l) : V3{ 1, 0, 0 }; }
Q qmul(Q a, Q b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
Q qaxis(V3 u, float deg) { const float h = deg * 3.14159265f / 360.0f, s = sinf(h); return { cosf(h), u.x * s, u.y * s, u.z * s }; }
// yaw > 0 = muzzle to the left, pitch > 0 = muzzle up, roll > 0 = top of the gun to the right
Q qeuler(float yaw, float pitch, float roll) {
    return qmul(qaxis({ 0, 0, 1 }, yaw), qmul(qaxis({ 0, 1, 0 }, -pitch), qaxis({ 1, 0, 0 }, roll)));
}
Q slerp(Q a, Q b, float t) {
    float c = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (c < 0) { b = { -b.w, -b.x, -b.y, -b.z }; c = -c; }
    float ka = 1 - t, kb = t;
    if (c < 0.9995f) { const float th = acosf(c), s = sinf(th); ka = sinf((1 - t) * th) / s; kb = sinf(t * th) / s; }
    Q r = { a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb };
    const float l = sqrtf(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    return { r.w / l, r.x / l, r.y / l, r.z / l };
}
void qmat(Q q, float m[3][3]) {
    const float w = q.w, x = q.x, y = q.y, z = q.z;
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - w * z);     m[0][2] = 2 * (x * z + w * y);
    m[1][0] = 2 * (x * y + w * z);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - w * x);
    m[2][0] = 2 * (x * z - w * y);     m[2][1] = 2 * (y * z + w * x);     m[2][2] = 1 - 2 * (x * x + y * y);
}
Q qfrom(const float m[3][3]) {
    const float tr = m[0][0] + m[1][1] + m[2][2];
    Q q;
    if (tr > 0) { const float s = sqrtf(tr + 1) * 2; q = { s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s }; }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { const float s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2; q = { (m[2][1] - m[1][2]) / s, s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s }; }
    else if (m[1][1] > m[2][2]) { const float s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2; q = { (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s }; }
    else { const float s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2; q = { (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4 }; }
    return q;
}
V3 qrot(Q q, V3 v) { float m[3][3]; qmat(q, m); return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z }; }

float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
float ease(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }
float ramp(float t, float a, float b) { return ease((t - a) / (b - a)); }
float bump(float t, float t0, float dur) { return (t < t0 || t > t0 + dur) ? 0.0f : sinf(3.14159265f * (t - t0) / dur); }

struct Key { float t; Q q; V3 slide; };

// ---- the bones of the view DObj that v2 moves (bone_probe hands us every skeleton built) ----
struct Skel {
    const uint8_t* volatile dobj = nullptr;   // non-null only while a v2 inspect runs
    int clip = -1, bolt = -1, flash = -1;
    bool boltaction = false;
    volatile float mag = 0, back = 0, lift = 0, twist = 0, grab = 0;
    volatile bool dirs = false;               // bore axis / magazine direction measured on a fresh skeleton
    V3 b = { 1, 0, 0 }, o = { 0, 0, -1 }, P = { 0, 0, 0 };
    V3 out_local = { 0, 0, -1 };              // the way the magazine leaves the gun (reload anim), in its bone's frame
    bool has_out = false;
    unsigned builds = 0;
} g_sk;

// the left arm, for the hand that takes the magazine: upper arm, forearm, hand, and every bone
// under the upper arm (parents first) with the local rotation it has in the reload animation
struct Arm {
    bool ok = false;
    int up = -1, fore = -1, hand = -1, n = 0;
    int idx[40], par[40];
    Q lq[40];
    Q gq = { 1, 0, 0, 0 };            // the hand in the magazine's frame
    V3 gp = { 0, 0, 0 };
} g_arm;

const char* const CLIPS[] = { "mp44_clip", "mp40_clip", "thompson_clip", "sten_clip", "ppsh_clip", "bar_clip", "bren_clip",
                              "fg42_clip", "m1carbine_clip", "colt_clip", "luger_clip" };
const char* const SLIDES[] = { "mp44_bolt", "mp40_bolt", "thompson_bolt", "sten_bolt", "ppsh_bolt", "bar_bolt", "bren_bolt",
                               "fg42_bolt", "m1carbine_bolt", "garand_bolt" };
const char* const BOLTS[] = { "kar98_bolt", "springfield_bolt01", "enfield_bolt" };   // bolt actions: lift, pull, push, lower

uintptr_t g_base = 0;
void* g_orig = nullptr;
void* g_cv_go = nullptr;
void* g_cv_pivot = nullptr;
void* g_cv_speed = nullptr;
void* g_cv_mag = nullptr;
int g_mode = 0;                   // 0 = idle, 1 = inspect, 2 = full inspect (bolt + magazine)
LARGE_INTEGER g_t0, g_freq;
int g_weapon = 0, g_state = 0;

float now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (float)((double)(t.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart);
}
float cvar_f(void* cv, float def, float lo, float hi) {
    const float v = cv ? *(const float*)((const uint8_t*)cv + CV_VALUE) : def;
    return (v >= lo && v <= hi) ? v : def;
}

int find_bone(Syscall_t sys, const uint8_t* dobj, const char* const* names, int n, const char** found) {
    for (int i = 0; i < n; ++i) {
        const int k = (int)sys(SC_BONE_INDEX, dobj, names[i]);
        if (k >= 0 && k < dobj[0x17]) { *found = names[i]; return k; }
    }
    return -1;
}

// the magazine turned toward the screen centre and the camera, the muzzle kept ahead
Q present_mag() {
    const V3 b = g_sk.dirs ? g_sk.b : V3{ 1, 0, 0 };
    V3 o = g_sk.dirs ? g_sk.o : V3{ 0, 0, -1 };
    o = norm(sub(o, scl(b, dot(o, b))));
    const V3 c = cross(b, o);
    const V3 beta = norm({ 0.93f, 0.30f, 0.20f });
    V3 tau = norm({ -0.35f, 0.85f, 0.40f });
    tau = norm(sub(tau, scl(beta, dot(tau, beta))));
    const V3 c2 = cross(beta, tau);
    const float S[3][3] = { { b.x, o.x, c.x }, { b.y, o.y, c.y }, { b.z, o.z, c.z } };          // columns b, o, c
    const float T[3][3] = { { beta.x, tau.x, c2.x }, { beta.y, tau.y, c2.y }, { beta.z, tau.z, c2.z } };
    float R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i][j] = T[i][0] * S[j][0] + T[i][1] * S[j][1] + T[i][2] * S[j][2];
    return qfrom(R);
}

int build_keys(int mode, Key* k) {
    const Q I = { 1, 0, 0, 0 };
    if (mode == 1) {
        k[0] = { 0.00f, I, { 0, 0, 0 } };
        k[1] = { 0.55f, qeuler(26, 8, -42), { 2, 3, 1.5f } };   // right side up to the camera
        k[2] = { 1.40f, qeuler(21, 5, -50), { 2, 3, 1.5f } };   // slow drift
        k[3] = { 2.00f, qeuler(-12, 18, 26), { 1, 1, 1 } };     // flip: left side and underside
        k[4] = { 2.60f, qeuler(-8, 14, 32), { 1, 1, 1 } };
        k[5] = { 3.20f, I, { 0, 0, 0 } };
        return 6;
    }
    const Q P = present_mag();
    k[0] = { 0.00f, I, { 0, 0, 0 } };
    k[1] = { 0.55f, qeuler(26, 8, -42), { 2, 3, 1.5f } };       // right side: bolt / ejection port
    k[2] = { 1.20f, qeuler(22, 6, -48), { 2, 3, 1.5f } };       // chamber check happens here
    k[3] = { 2.10f, qeuler(20, 5, -44), { 2, 3, 1.5f } };
    k[4] = { 2.75f, P, { 1.5f, 4, 2 } };                        // magazine to the camera
    k[5] = { 4.25f, qmul(qeuler(3, 2, 5), P), { 1.5f, 4, 2 } }; // magazine out, looked at, back in
    k[6] = { 4.90f, qeuler(-14, 14, 28), { 1, 1, 1 } };         // left side
    k[7] = { 5.50f, qeuler(-10, 11, 32), { 1, 1, 1 } };
    k[8] = { 6.20f, I, { 0, 0, 0 } };
    return 9;
}

const char* resolve_arm(Syscall_t sys, const uint8_t* dobj, const InspectGrip* gr) {
    auto idx = [&](const char* name) { const int k = (int)sys(SC_BONE_INDEX, dobj, name); return (k >= 0 && k < dobj[0x17]) ? k : -1; };
    g_arm.up = idx("bip01 l upperarm");
    g_arm.fore = idx("bip01 l forearm");
    g_arm.hand = idx("bip01 l hand");
    if (g_arm.up < 0 || g_arm.fore < 0 || g_arm.hand < 0) return "left arm bones not found";
    g_arm.n = 0;
    for (int i = 0; i < gr->nbones && g_arm.n < 40; ++i) {
        const int k = idx(gr->bones[i].name), p = idx(gr->bones[i].parent);
        if (k < 0 || p < 0) continue;
        const float* q = gr->bones[i].q;
        g_arm.idx[g_arm.n] = k; g_arm.par[g_arm.n] = p; g_arm.lq[g_arm.n] = { q[3], q[0], q[1], q[2] };
        ++g_arm.n;
    }
    g_arm.gq = { gr->q[3], gr->q[0], gr->q[1], gr->q[2] };
    g_arm.gp = { gr->p[0], gr->p[1], gr->p[2] };
    g_arm.ok = true;
    static char s_msg[64];
    snprintf(s_msg, sizeof(s_msg), "takes it (%d arm bones)", g_arm.n);
    return s_msg;
}

void stop(const char* why) {
    if (g_mode == 2) logger::logf("inspect: view skeleton rebuilt %u time(s) with the bones moved", g_sk.builds);
    g_sk.dobj = nullptr;
    g_mode = 0;
    logger::logf("inspect: %s", why);
}

void start(int mode, int weapon, int state) {
    g_sk.dobj = nullptr;
    g_sk.clip = g_sk.bolt = g_sk.flash = -1;
    g_sk.boltaction = false;
    g_sk.mag = g_sk.back = g_sk.lift = g_sk.twist = g_sk.grab = 0;
    g_arm.ok = false;
    g_sk.dirs = false;
    g_sk.builds = 0;
    QueryPerformanceCounter(&g_t0);
    g_mode = mode; g_weapon = weapon; g_state = state;
    char bones[256] = "";
    if (mode == 2) {
        const uint8_t* dobj = (weapon > 0 && weapon < 64) ? *(const uint8_t* const*)(g_base + WEAPONS_RVA + (uintptr_t)weapon * WEAPON_STRIDE) : nullptr;
        const Syscall_t sys = *(const Syscall_t*)(g_base + SYSCALL_PTR_RVA);
        if (dobj && sys && dobj[0x17] >= 4 && dobj[0x17] <= 128 && dobj[0x16] >= 1 && dobj[0x16] <= 8 && *(void* const*)(dobj + 4)) {
            const char* fc = "-"; const char* fb = "-"; const char* ff = "-";
            static const char* const FLASH[] = { "tag_flash" };
            g_sk.flash = find_bone(sys, dobj, FLASH, 1, &ff);
            g_sk.clip = find_bone(sys, dobj, CLIPS, sizeof(CLIPS) / sizeof(*CLIPS), &fc);
            g_sk.bolt = find_bone(sys, dobj, BOLTS, sizeof(BOLTS) / sizeof(*BOLTS), &fb);
            if (g_sk.bolt >= 0) g_sk.boltaction = true;
            else g_sk.bolt = find_bone(sys, dobj, SLIDES, sizeof(SLIDES) / sizeof(*SLIDES), &fb);
            const char* hand = "no grip for this magazine";
            g_sk.has_out = false;
            if (g_sk.clip >= 0)
                for (int i = 0; i < INSPECT_NGRIPS; ++i)
                    if (!strcmp(INSPECT_GRIPS[i].clip, fc)) {
                        const InspectGrip& gr = INSPECT_GRIPS[i];
                        g_sk.out_local = { gr.out[0], gr.out[1], gr.out[2] };
                        g_sk.has_out = true;
                        hand = gr.nbones ? resolve_arm(sys, dobj, &gr) : "never takes it (not in its reload)";
                    }
            snprintf(bones, sizeof(bones), "view DObj %p (%d bones, %d models, tag %d): flash %s=%d, magazine %s=%d, bolt %s=%d%s, left hand: %s",
                     (const void*)dobj, dobj[0x17], dobj[0x16], dobj[0x54], ff, g_sk.flash, fc, g_sk.clip, fb, g_sk.bolt,
                     g_sk.boltaction ? " (bolt action)" : "", hand);
            g_sk.dobj = dobj;
        } else {
            snprintf(bones, sizeof(bones), "no usable view DObj (%p) - the weapon turns, no bone moves", (const void*)dobj);
        }
    }
    logger::logf("inspect: start %s (weapon %d, weaponstate %d, pivot \"%s\", speed %.2f)%s%s", mode == 2 ? "FULL" : "basic",
                 weapon, state, *(const char* const*)((const uint8_t*)g_cv_pivot + CV_STRING), cvar_f(g_cv_speed, 1, 0.05f, 20),
                 bones[0] ? " - " : "", bones);
}

// v' = view-local rotation of a world vector
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
        g_cv_pivot = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_pivot", "12 0 -3", 0);
        g_cv_speed = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_speed", "1", 0);
        g_cv_mag   = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_mag", "5", 0);
        QueryPerformanceFrequency(&g_freq);
        if (!g_cv_go || !g_cv_pivot || !g_cv_speed) return;
    }
    if (!ref || !ps) return;
    const int weapon = *(const int*)(ps + PS_WEAPON);
    const int state = *(const int*)(ps + PS_WEAPONSTATE);
    const float ads = *(const float*)(ps + PS_ADSFRAC);
    const int asked = *(const int*)((const uint8_t*)g_cv_go + CV_INT);
    if (asked) {
        ((Cvar_Set_t)CODMP_CVAR_SET_VA)("cod1x_inspect", "0");
        if (ads > 0.001f) logger::logf("inspect: ignored while aiming (ads %.2f)", ads);
        else start(asked == 2 ? 2 : 1, weapon, state);
    }
    if (!g_mode) return;
    if (weapon != g_weapon) { stop("cut - weapon switch"); return; }
    if (state != g_state) { logger::logf("inspect: weaponstate %d -> %d", g_state, state); stop("cut - fire / reload"); return; }
    if (ads > 0.001f) { stop("cut - aiming"); return; }

    const float t = now_s() * cvar_f(g_cv_speed, 1, 0.05f, 20);
    Key k[9];
    const int n = build_keys(g_mode, k);
    if (t >= k[n - 1].t) { stop("done"); return; }
    int i = 0;
    while (i < n - 2 && t >= k[i + 1].t) ++i;
    const float s = ease((t - k[i].t) / (k[i + 1].t - k[i].t));
    Q q = slerp(k[i].q, k[i + 1].q, s);
    V3 slide = add(k[i].slide, scl(sub(k[i + 1].slide, k[i].slide), s));

    if (g_mode == 2) {
        // bolt: pull / release (a bolt action lifts its handle first), then the magazine
        float jolt = 0;
        if (g_sk.bolt >= 0 && g_sk.boltaction) {
            g_sk.lift = ramp(t, 0.90f, 1.10f) * (1 - ramp(t, 1.85f, 1.98f));
            g_sk.back = ramp(t, 1.10f, 1.35f) * (1 - ramp(t, 1.70f, 1.85f));
            jolt += bump(t, 1.98f, 0.16f);
        } else if (g_sk.bolt >= 0) {
            g_sk.back = ramp(t, 1.00f, 1.20f) * (1 - ramp(t, 1.75f, 1.82f));
            jolt += bump(t, 1.82f, 0.16f);                      // the bolt slams home
        }
        if (g_sk.clip >= 0) {
            // the left hand goes to the magazine, pulls it, turns it to look at it, seats it, goes back
            g_sk.grab = g_arm.ok ? ramp(t, 2.85f, 3.15f) * (1 - ramp(t, 4.20f, 4.50f)) : 0.0f;
            g_sk.mag = ramp(t, 3.15f, 3.45f) * (1 - ramp(t, 4.05f, 4.17f));
            g_sk.twist = (t > 3.45f && t < 4.05f) ? 14.0f * sinf(3.14159265f * (t - 3.45f) / 0.60f) : 0.0f;
            jolt += bump(t, 4.17f, 0.16f);                      // the magazine seated
        }
        if (jolt > 0) { q = qmul(qeuler(0, 2.2f * jolt, -1.5f * jolt), q); slide.z += 0.25f * jolt; }
    }

    float R[3][3];
    qmat(q, R);
    const float* F = (const float*)(g_base + VIEWAXIS_RVA);
    const float* L = F + 3;
    const float* U = F + 6;
    float piv[3] = { 12, 0, -3 };
    sscanf(*(const char* const*)((const uint8_t*)g_cv_pivot + CV_STRING), "%f %f %f", &piv[0], &piv[1], &piv[2]);

    float* axis = (float*)(ref + RE_AXIS);
    for (int r = 0; r < 3; ++r) { float o[3]; turn(F, L, U, R, axis + 3 * r, o); memcpy(axis + 3 * r, o, sizeof(o)); }
    // origin' = origin + pivot - R pivot + slide   (turning about origin + pivot)
    float pw[3], rp[3];
    for (int j = 0; j < 3; ++j) pw[j] = piv[0] * F[j] + piv[1] * L[j] + piv[2] * U[j];
    turn(F, L, U, R, pw, rp);
    float* org = (float*)(ref + RE_ORIGIN);
    for (int j = 0; j < 3; ++j) org[j] += pw[j] - rp[j] + slide.x * F[j] + slide.y * L[j] + slide.z * U[j];
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

// bone b of a built skeleton: rows = the bone's axes, [12..14] = position, model space
float* bone(const uint8_t* d, int b) { return (float*)(*(uint8_t* const*)(d + 4) + 0x30 + b * 0x40); }
V3 row(const float* m, int r) { return { m[4 * r], m[4 * r + 1], m[4 * r + 2] }; }
void set_row(float* m, int r, V3 v) { m[4 * r] = v.x; m[4 * r + 1] = v.y; m[4 * r + 2] = v.z; }
// the stored rows are the bone's axes = the columns of its rotation
Q get_rot(const float* m) { float R[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i][j] = m[4 * j + i]; return qfrom(R); }
void set_rot(float* m, Q q) { float R[3][3]; qmat(q, R); for (int r = 0; r < 3; ++r) set_row(m, r, { R[0][r], R[1][r], R[2][r] }); }
Q qconj(Q q) { return { q.w, -q.x, -q.y, -q.z }; }
float len(V3 a) { return sqrtf(dot(a, a)); }
// turn a bone about a point
void spin(const uint8_t* d, int b, Q r, V3 pivot) {
    float* m = bone(d, b);
    for (int k = 0; k < 3; ++k) set_row(m, k, qrot(r, row(m, k)));
    set_row(m, 3, add(pivot, qrot(r, sub(row(m, 3), pivot))));
}
Q between(V3 a, V3 b) {
    a = norm(a); b = norm(b);
    const float c = dot(a, b);
    if (c < -0.9999f) return qaxis(norm(fabsf(a.x) < 0.9f ? cross(a, { 1, 0, 0 }) : cross(a, { 0, 1, 0 })), 180.0f);
    const V3 x = cross(a, b);
    const Q q = { 1 + c, x.x, x.y, x.z };
    const float l = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return { q.w / l, q.x / l, q.y / l, q.z / l };
}
// chain entries at or under bone `root` (the chain is parents first)
void subtree(int root, bool* in) {
    for (int i = 0; i < g_arm.n; ++i) {
        in[i] = g_arm.idx[i] == root || g_arm.par[i] == root;
        for (int j = 0; j < i && !in[i]; ++j) in[i] = in[j] && g_arm.idx[j] == g_arm.par[i];
    }
}

// Left hand onto the magazine: the grip of the reload animation placed on the magazine as it
// is now, the arm re-solved (upper arm and forearm keep their lengths, the elbow stays in the
// plane it bends in), then the hand's rotation and the fingers of the grip.
void solve_arm(const uint8_t* d) {
    const float w = g_sk.grab;
    const float* C = bone(d, g_sk.clip);
    const Q qc = get_rot(C);
    const V3 pc = row(C, 3);
    const Q qt = qmul(qc, g_arm.gq);
    const V3 pt = add(pc, qrot(qc, g_arm.gp));
    float* H = bone(d, g_arm.hand);
    const V3 W0 = row(H, 3);
    const Q tq = slerp(get_rot(H), qt, w);
    const V3 tp = add(W0, scl(sub(pt, W0), w));
    const V3 S = row(bone(d, g_arm.up), 3), E = row(bone(d, g_arm.fore), 3);
    const float a = len(sub(E, S)), b = len(sub(W0, E));
    if (a < 0.5f || b < 0.5f) return;
    const V3 dir = norm(sub(tp, S));
    float dl = len(sub(tp, S));
    const float lo = fabsf(a - b) + 0.05f, hi = a + b - 0.05f;
    dl = dl < lo ? lo : dl > hi ? hi : dl;
    const float along = (a * a - b * b + dl * dl) / (2 * dl);
    const float h = sqrtf(a * a - along * along > 0 ? a * a - along * along : 0);
    V3 pole = sub(E, S);
    pole = sub(pole, scl(dir, dot(pole, dir)));
    if (dot(pole, pole) < 1e-4f) pole = cross(dir, { 0, 0, 1 });
    const V3 E2 = add(S, add(scl(dir, along), scl(norm(pole), h)));
    const V3 T2 = add(S, scl(dir, dl));
    bool in[40];
    // 1. upper arm: the elbow to E2
    const Q r1 = between(sub(E, S), sub(E2, S));
    spin(d, g_arm.up, r1, S);
    for (int i = 0; i < g_arm.n; ++i) spin(d, g_arm.idx[i], r1, S);
    // 2. forearm: the wrist to the target
    const Q r2 = between(sub(row(H, 3), E2), sub(T2, E2));
    subtree(g_arm.fore, in);
    for (int i = 0; i < g_arm.n; ++i) if (in[i]) spin(d, g_arm.idx[i], r2, E2);
    // 3. the hand turned as it holds the magazine
    const Q r3 = qmul(tq, qconj(get_rot(H)));
    const V3 W2 = row(H, 3);
    subtree(g_arm.hand, in);
    for (int i = 0; i < g_arm.n; ++i) if (in[i]) spin(d, g_arm.idx[i], r3, W2);
    // 4. fingers closed as in the reload (local rotations blended, bone lengths kept)
    Q lcur[40]; V3 tloc[40];
    for (int i = 0; i < g_arm.n; ++i) {
        if (!in[i] || g_arm.idx[i] == g_arm.hand) continue;
        const float* m = bone(d, g_arm.idx[i]);
        const float* pm = bone(d, g_arm.par[i]);
        const Q pq = qconj(get_rot(pm));
        lcur[i] = qmul(pq, get_rot(m));
        tloc[i] = qrot(pq, sub(row(m, 3), row(pm, 3)));
    }
    for (int i = 0; i < g_arm.n; ++i) {
        if (!in[i] || g_arm.idx[i] == g_arm.hand) continue;
        float* m = bone(d, g_arm.idx[i]);
        const float* pm = bone(d, g_arm.par[i]);
        const Q pq = get_rot(pm);
        set_rot(m, qmul(pq, slerp(lcur[i], g_arm.lq[i], w)));
        set_row(m, 3, add(row(pm, 3), qrot(pq, tloc[i])));
    }
}
}  // namespace

void inspect_on_skeleton(const void* dobj) {
    const uint8_t* d = g_sk.dobj;
    if (!d || dobj != d || !*(void* const*)(d + 4)) return;
    const int n = d[0x17];
    if (!g_sk.dirs) {
        V3 b = { 1, 0, 0 }, P = { 0, 0, 0 };
        if (g_sk.flash >= 0 && g_sk.flash < n) {
            const float* m = bone(d, g_sk.flash);
            b = norm(row(m, 0));
            if (b.x < 0) b = scl(b, -1);
            if (b.x < 0.8f) b = { 1, 0, 0 };     // a tag_flash not laid along the barrel (seen: 0.62 0.69 0.39)
            P = row(m, 3);
        }
        V3 o = { 0, 0, -1 };
        if (g_sk.clip >= 0 && g_sk.clip < n) {
            if (g_sk.has_out) {
                // the magazine bone sits at the top of the magazine, on the bore: its position says
                // nothing of the way out, the reload animation does
                o = norm(qrot(get_rot(bone(d, g_sk.clip)), g_sk.out_local));
            } else {
                V3 v = sub(row(bone(d, g_sk.clip), 3), P);
                v = sub(v, scl(b, dot(v, b)));
                if (dot(v, v) > 0.25f) o = norm(v);
            }
        }
        g_sk.b = b; g_sk.o = o; g_sk.P = P;
        g_sk.dirs = true;
        logger::logf("inspect: bore axis (%.2f %.2f %.2f), magazine side (%.2f %.2f %.2f)", b.x, b.y, b.z, o.x, o.y, o.z);
    }
    ++g_sk.builds;
    if (g_sk.bolt >= 0 && g_sk.bolt < n && (g_sk.back > 0 || g_sk.lift > 0)) {
        float* m = bone(d, g_sk.bolt);
        if (g_sk.boltaction && g_sk.lift > 0) {
            // handle up: turn about the bore line (the handle sits on the right, -y)
            const Q r = qaxis(g_sk.b, -62.0f * g_sk.lift);
            for (int k = 0; k < 3; ++k) set_row(m, k, qrot(r, row(m, k)));
            set_row(m, 3, add(g_sk.P, qrot(r, sub(row(m, 3), g_sk.P))));
        }
        set_row(m, 3, sub(row(m, 3), scl(g_sk.b, g_sk.back * (g_sk.boltaction ? 3.2f : 2.5f))));
    }
    if (g_sk.clip >= 0 && g_sk.clip < n && g_sk.mag > 0) {
        float* m = bone(d, g_sk.clip);
        set_row(m, 3, add(row(m, 3), scl(g_sk.o, g_sk.mag * cvar_f(g_cv_mag, 5, 0, 20))));
        if (g_sk.twist != 0) spin(d, g_sk.clip, qaxis(g_sk.o, g_sk.twist), row(m, 3));   // turned to be looked at
    }
    if (g_arm.ok && g_sk.grab > 0 && g_arm.up < n && g_arm.fore < n && g_arm.hand < n && g_sk.clip < n) solve_arm(d);
}

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
    g_sk.dobj = nullptr;              // the old cgame's DObj pointers die with it
    g_base = base;
    g_orig = (void*)target;
    g_mode = 0;
    const int32_t rel = (int32_t)((uintptr_t)&stub - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(site + 1) = rel;
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    logger::logf("inspect: view weapon hooked (cgame+0x%x) - `set cod1x_inspect 1` (turn) or 2 (full: bolt + magazine)", (unsigned)CALL_RVA);
    return true;
}

}  // namespace patches
