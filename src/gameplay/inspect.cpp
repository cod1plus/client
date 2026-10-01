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
constexpr uintptr_t VIEWORG_RVA = 0x20d348;   // cg.refdef.vieworg, then viewaxis: forward, left, up
constexpr uintptr_t SYSCALL_PTR_RVA = 0x76898; // the engine's syscall entry, written at cgame init
constexpr uintptr_t WEAPONS_RVA = 0x1aa4e0;   // cg_weapons[], 0x198 each; +0 = the view DObj (hands + weapon)
constexpr int WEAPON_STRIDE = 0x198;
constexpr intptr_t SC_BONE_INDEX = 0xaf;      // (dobj, name) -> bone index, -1 when absent (CoDMP 0x487b90)
constexpr int RE_ORIGIN = 0x44;
constexpr int PS_WEAPON = 0xb0, PS_WEAPONSTATE = 0xb4, PS_ADSFRAC = 0xb8;   // the crosshair hides on ps+0xb4 = 1/2/5/10/11
constexpr int CV_STRING = 0x04, CV_VALUE = 0x1c, CV_INT = 0x20;

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
typedef intptr_t (*Syscall_t)(intptr_t, ...);

// ---- vectors and quaternions; model space of the view model = view axes: x forward, y left, z up ----
struct V3 { float x, y, z; };
struct Q  { float w, x, y, z; };
V3 add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 scl(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V3 a) { return sqrtf(dot(a, a)); }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 norm(V3 a) { const float l = len(a); return l > 1e-6f ? scl(a, 1.0f / l) : V3{ 1, 0, 0 }; }
Q qmul(Q a, Q b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
Q qconj(Q q) { return { q.w, -q.x, -q.y, -q.z }; }
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
Q between(V3 a, V3 b) {
    a = norm(a); b = norm(b);
    const float c = dot(a, b);
    if (c < -0.9999f) return qaxis(norm(fabsf(a.x) < 0.9f ? cross(a, { 1, 0, 0 }) : cross(a, { 0, 1, 0 })), 180.0f);
    const V3 x = cross(a, b);
    const Q q = { 1 + c, x.x, x.y, x.z };
    const float l = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return { q.w / l, q.x / l, q.y / l, q.z / l };
}

float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
float ease(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }
float ramp(float t, float a, float b) { return ease((t - a) / (b - a)); }
float bump(float t, float t0, float dur) { return (t < t0 || t > t0 + dur) ? 0.0f : sinf(3.14159265f * (t - t0) / dur); }

struct Key { float t; Q q; V3 slide; };

// ---- the view DObj (hands + weapon) and the bones the inspect moves ----
// An arm: upper arm, forearm, hand, and every bone under the upper arm (parents first). For the
// left one, lq = the local rotation of that bone in the grip of the reload animation.
struct Limb {
    int up = -1, fore = -1, hand = -1, n = 0;
    int idx[40], par[40];
    Q lq[40];
    bool has_lq[40];
};
struct Skel {
    const uint8_t* volatile dobj = nullptr;   // non-null only while an inspect runs
    int clip = -1, bolt = -1, flash = -1, gun0 = -1, htag = -1;
    bool boltaction = false, arms = false;
    // written each frame by inspect_apply, read by the skeleton hook
    volatile float mag = 0, back = 0, lift = 0, twist = 0, grab = 0;
    Q gq = { 1, 0, 0, 0 };                    // the gun's turn about its middle, model space
    V3 gslide = { 0, 0, 0 };
    volatile bool dirs = false;               // bore axis / magazine direction measured on a fresh skeleton
    V3 b = { 1, 0, 0 }, o = { 0, 0, -1 }, P = { 0, 0, 0 };
    V3 out_local = { 0, 0, -1 };              // the way the magazine leaves the gun (reload anim), in its bone's frame
    bool has_out = false, grip = false;
    Q grip_q = { 1, 0, 0, 0 };                // the left hand in the magazine's frame
    V3 grip_p = { 0, 0, 0 };
    // pistol spin (mode 3): the pistol turns about the right index finger, the hand open
    int idx11 = -1, idx12 = -1;
    volatile float spin_deg = 0, open = 0, away = 0;
    volatile float detach = 0;                // toss (mode 4): the hands have let go of the gun
    volatile float flick = 0;                 // the wrist: turn about the spin axis (deg) ...
    V3 flick_off = { 0, 0, 0 };               // ... and move (units)
    V3 com = { 0, 0, -1.6f };                 // the pistol's centre of mass from the finger, model space
    V3 axis = { 0, 1, 0 };                    // the spin axis: the pistol's lateral axis
    volatile bool spin_ready = false;         // com / axis measured on a fresh skeleton
    unsigned builds = 0;
} g_sk;
Limb g_R, g_L;

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
int g_mode = 0;                   // 0 = idle, 1 = inspect, 2 = full inspect (bolt + magazine), 3 = pistol spin, 4 = toss
LARGE_INTEGER g_t0, g_freq;
// the pistol on the finger: one degree of freedom about its lateral axis, gravity on its centre
// of mass, finger friction, the wrist's snap as a torque, the palm's catch as a stiff spring
struct Spin { float th = 0, om = 0, last = 0, tc = -1, tgt = 0; } g_spin;
constexpr float SPIN_G = 386.0f;          // in/s^2 - the view models are built in inches
constexpr float SPIN_K2 = 3.4f * 3.4f;    // radius of gyration^2 of a pistol about its centre of mass
constexpr float SPIN_VISC = 0.6f, SPIN_COUL = 3.5f;
constexpr float SPIN_KICK = 420.0f;       // rad/s^2 for 70 ms: ~2.4 turns/s, 2-3 turns
constexpr float SPIN_T_SNAP = 0.30f, SPIN_T_SNAP_END = 0.37f;
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
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

int find_bone(Syscall_t sys, const uint8_t* dobj, const char* const* names, int n, const char** found) {
    for (int i = 0; i < n; ++i) {
        const int k = (int)sys(SC_BONE_INDEX, dobj, names[i]);
        if (k >= 0 && k < dobj[0x17]) { *found = names[i]; return k; }
    }
    return -1;
}
// bones of the DObj's first model (the hands): the weapon's bones come after them
// (DObj +0x1c = its models; model +4 -> [0] -> +4 = parts -> [0] -> [0] = short bone count, as CoDMP 0x4867d0 walks it)
int first_model_bones(const uint8_t* d) {
    const uint8_t* m = *(const uint8_t* const*)(d + 0x1c);
    if (!readable(m, 8)) return -1;
    const uint8_t* a = *(const uint8_t* const*)(m + 4);
    if (!readable(a, 4)) return -1;
    const uint8_t* b = *(const uint8_t* const*)a;
    if (!readable(b, 8)) return -1;
    const uint8_t* parts = *(const uint8_t* const*)(b + 4);
    if (!readable(parts, 4)) return -1;
    const uint8_t* hp = *(const uint8_t* const*)parts;
    if (!readable(hp, 4)) return -1;
    const uint8_t* hdr = *(const uint8_t* const*)hp;
    if (!readable(hdr, 2)) return -1;
    return *(const short*)hdr;
}
bool resolve_limb(Syscall_t sys, const uint8_t* dobj, Limb& L, char side, const InspectArmBone* list, int n) {
    auto idx = [&](const char* name) { const int k = (int)sys(SC_BONE_INDEX, dobj, name); return (k >= 0 && k < dobj[0x17]) ? k : -1; };
    char nm[32];
    snprintf(nm, sizeof(nm), "bip01 %c upperarm", side); L.up = idx(nm);
    snprintf(nm, sizeof(nm), "bip01 %c forearm", side);  L.fore = idx(nm);
    snprintf(nm, sizeof(nm), "bip01 %c hand", side);     L.hand = idx(nm);
    L.n = 0;
    if (L.up < 0 || L.fore < 0 || L.hand < 0) return false;
    for (int i = 0; i < n && L.n < 40; ++i) {
        const int k = idx(list[i].name), p = idx(list[i].parent);
        if (k < 0 || p < 0) continue;
        L.idx[L.n] = k; L.par[L.n] = p; L.has_lq[L.n] = false;
        ++L.n;
    }
    return true;
}

// the magazine turned down-left, toward the middle of the screen but away from the eye (turned
// straight at the screen centre it came at the camera, the left arm with it), the muzzle a little up
Q present_mag() {
    const V3 b = g_sk.dirs ? g_sk.b : V3{ 1, 0, 0 };
    V3 o = g_sk.dirs ? g_sk.o : V3{ 0, 0, -1 };
    o = norm(sub(o, scl(b, dot(o, b))));
    const V3 c = cross(b, o);
    const V3 beta = norm({ 0.95f, 0.15f, 0.25f });
    V3 tau = norm({ -0.15f, 0.55f, -0.80f });
    tau = norm(sub(tau, scl(beta, dot(tau, beta))));
    const V3 c2 = cross(beta, tau);
    const float S[3][3] = { { b.x, o.x, c.x }, { b.y, o.y, c.y }, { b.z, o.z, c.z } };          // columns b, o, c
    const float T[3][3] = { { beta.x, tau.x, c2.x }, { beta.y, tau.y, c2.y }, { beta.z, tau.z, c2.z } };
    float R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i][j] = T[i][0] * S[j][0] + T[i][1] * S[j][1] + T[i][2] * S[j][2];
    return qfrom(R);
}

// the gun's turns (about its own middle; the hands stay on it, the shoulders do not move) and
// slides (toward the middle of the screen and up), model space
int build_keys(int mode, Key* k) {
    const Q I = { 1, 0, 0, 0 };
    if (mode == 1) {
        k[0] = { 0.00f, I, { 0, 0, 0 } };
        k[1] = { 0.55f, qeuler(20, 8, -40), { 0, 2.5f, 1.5f } };   // right side up to the camera
        k[2] = { 1.40f, qeuler(16, 6, -48), { 0, 2.5f, 1.5f } };   // slow drift
        k[3] = { 2.00f, qeuler(-10, 16, 30), { 0, 2, 1.5f } };     // flip: left side and underside
        k[4] = { 2.60f, qeuler(-7, 13, 36), { 0, 2, 1.5f } };
        k[5] = { 3.20f, I, { 0, 0, 0 } };
        return 6;
    }
    const Q P = present_mag();
    k[0] = { 0.00f, I, { 0, 0, 0 } };
    k[1] = { 0.55f, qeuler(20, 8, -40), { 0, 2.5f, 1.5f } };       // right side: bolt / ejection port
    k[2] = { 1.20f, qeuler(17, 6, -46), { 0, 2.5f, 1.5f } };       // chamber check happens here
    k[3] = { 2.10f, qeuler(15, 5, -42), { 0, 2.5f, 1.5f } };
    k[4] = { 2.75f, P, { 0, 2, 4 } };                              // gun up, magazine shown
    k[5] = { 4.25f, qmul(qeuler(3, 2, 5), P), { 0, 2, 4 } };       // the hand takes it out, looks at it, seats it
    k[6] = { 4.90f, qeuler(-10, 14, 30), { 0, 2, 1.5f } };         // left side
    k[7] = { 5.50f, qeuler(-7, 11, 34), { 0, 2, 1.5f } };
    k[8] = { 6.20f, I, { 0, 0, 0 } };
    return 9;
}

void stop(const char* why) {
    if (g_mode) logger::logf("inspect: view skeleton rebuilt %u time(s) with the bones moved", g_sk.builds);
    g_sk.dobj = nullptr;
    g_mode = 0;
    logger::logf("inspect: %s", why);
}

void start(int mode, int weapon, int state, const uint8_t* ref) {
    g_sk.dobj = nullptr;
    g_sk.clip = g_sk.bolt = g_sk.flash = g_sk.gun0 = g_sk.htag = -1;
    g_sk.boltaction = g_sk.arms = g_sk.has_out = g_sk.grip = false;
    g_sk.mag = g_sk.back = g_sk.lift = g_sk.twist = g_sk.grab = 0;
    g_sk.spin_deg = g_sk.open = g_sk.away = g_sk.flick = g_sk.detach = 0;
    g_sk.flick_off = { 0, 0, 0 };
    g_sk.spin_ready = false;
    g_spin = Spin();
    g_sk.idx11 = g_sk.idx12 = -1;
    g_sk.gq = { 1, 0, 0, 0 }; g_sk.gslide = { 0, 0, 0 };
    g_sk.dirs = false;
    g_sk.builds = 0;
    const uint8_t* dobj = (weapon > 0 && weapon < 64) ? *(const uint8_t* const*)(g_base + WEAPONS_RVA + (uintptr_t)weapon * WEAPON_STRIDE) : nullptr;
    const Syscall_t sys = *(const Syscall_t*)(g_base + SYSCALL_PTR_RVA);
    if (!dobj || !sys || !readable(dobj, 0x5c) || dobj[0x17] < 4 || dobj[0x17] > 128 || dobj[0x16] < 2 || dobj[0x16] > 8 || !*(void* const*)(dobj + 4)) {
        logger::logf("inspect: weapon %d has no usable view model (DObj %p) - nothing to turn", weapon, (const void*)dobj);
        return;
    }
    const char* fc = "-"; const char* fb = "-"; const char* ff = "-";
    static const char* const FLASH[] = { "tag_flash" };
    g_sk.flash = find_bone(sys, dobj, FLASH, 1, &ff);
    g_sk.gun0 = first_model_bones(dobj);
    if (g_sk.gun0 <= 0 || g_sk.gun0 >= dobj[0x17] || (g_sk.flash >= 0 && g_sk.flash < g_sk.gun0)) {
        logger::logf("inspect: weapon %d: the weapon's bones cannot be told from the hands' (first model %d bones, tag_flash %d of %d)",
                     weapon, g_sk.gun0, g_sk.flash, dobj[0x17]);
        return;
    }
    const int ht = (int)sys(SC_BONE_INDEX, dobj, "tag_weapon");
    g_sk.htag = (ht >= 0 && ht < g_sk.gun0) ? ht : -1;
    g_sk.arms = resolve_limb(sys, dobj, g_R, 'r', INSPECT_ARM_R, INSPECT_NARM_R) &&
                resolve_limb(sys, dobj, g_L, 'l', INSPECT_ARM_L, INSPECT_NARM_L);
    const char* hand = "-";
    if (mode == 3) {
        // only a pistol turns about a finger: anything else gets the full inspect
        g_sk.clip = find_bone(sys, dobj, CLIPS, sizeof(CLIPS) / sizeof(*CLIPS), &fc);
        const bool pistol = !strcmp(fc, "colt_clip") || !strcmp(fc, "luger_clip");
        g_sk.idx11 = (int)sys(SC_BONE_INDEX, dobj, "bip01 r finger11");
        g_sk.idx12 = (int)sys(SC_BONE_INDEX, dobj, "bip01 r finger12");
        if (!pistol || !g_sk.arms || g_sk.idx11 < 0 || g_sk.idx12 < 0 || g_sk.idx11 >= dobj[0x17] || g_sk.idx12 >= dobj[0x17]) {
            logger::logf("inspect: spin is for the pistols (magazine %s, arms %d, index finger %d/%d) - full inspect instead",
                         fc, (int)g_sk.arms, g_sk.idx11, g_sk.idx12);
            mode = 2;
            g_sk.clip = -1; fc = "-";
        } else {
            int with = 0;
            for (int j = 0; j < INSPECT_NOPEN_R; ++j) {
                const int k = (int)sys(SC_BONE_INDEX, dobj, INSPECT_OPEN_R[j].name);
                for (int e = 0; e < g_R.n; ++e)
                    if (g_R.idx[e] == k) { const float* q = INSPECT_OPEN_R[j].q; g_R.lq[e] = { q[3], q[0], q[1], q[2] }; g_R.has_lq[e] = true; ++with; }
            }
            static char s_spin[64];
            snprintf(s_spin, sizeof(s_spin), "spins it (%d finger bones open)", with);
            hand = s_spin;
            g_sk.clip = -1;               // the magazine stays in
        }
    }
    if (mode == 4) {
        // the right hand opens while the gun flies (the same open hand as the pistol spin)
        int with = 0;
        for (int j = 0; j < INSPECT_NOPEN_R; ++j) {
            const int k = (int)sys(SC_BONE_INDEX, dobj, INSPECT_OPEN_R[j].name);
            for (int e = 0; e < g_R.n; ++e)
                if (g_R.idx[e] == k) { const float* q = INSPECT_OPEN_R[j].q; g_R.lq[e] = { q[3], q[0], q[1], q[2] }; g_R.has_lq[e] = true; ++with; }
        }
        static char s_toss[64];
        snprintf(s_toss, sizeof(s_toss), "tosses it (%d finger bones open in flight)", with);
        hand = s_toss;
    }
    if (mode == 2) {
        g_sk.clip = find_bone(sys, dobj, CLIPS, sizeof(CLIPS) / sizeof(*CLIPS), &fc);
        g_sk.bolt = find_bone(sys, dobj, BOLTS, sizeof(BOLTS) / sizeof(*BOLTS), &fb);
        if (g_sk.bolt >= 0) g_sk.boltaction = true;
        else g_sk.bolt = find_bone(sys, dobj, SLIDES, sizeof(SLIDES) / sizeof(*SLIDES), &fb);
        hand = "no grip for this magazine";
        if (g_sk.clip >= 0)
            for (int i = 0; i < INSPECT_NGRIPS; ++i) {
                const InspectGrip& gr = INSPECT_GRIPS[i];
                if (strcmp(gr.clip, fc)) continue;
                g_sk.out_local = { gr.out[0], gr.out[1], gr.out[2] };
                g_sk.has_out = true;
                if (!gr.nbones) { hand = "never takes it (not in its reload)"; continue; }
                g_sk.grip_q = { gr.q[3], gr.q[0], gr.q[1], gr.q[2] };
                g_sk.grip_p = { gr.p[0], gr.p[1], gr.p[2] };
                int with = 0;
                for (int j = 0; j < gr.nbones; ++j) {
                    const int k = (int)sys(SC_BONE_INDEX, dobj, gr.bones[j].name);
                    for (int e = 0; e < g_L.n; ++e)
                        if (g_L.idx[e] == k) { const float* q = gr.bones[j].q; g_L.lq[e] = { q[3], q[0], q[1], q[2] }; g_L.has_lq[e] = true; ++with; }
                }
                g_sk.grip = g_sk.arms;
                static char s_msg[64];
                snprintf(s_msg, sizeof(s_msg), "takes it (%d finger/hand bones)", with);
                hand = g_sk.arms ? s_msg : "arm bones not found";
            }
    }
    // where the view model sits from the eye (the hip offset), for the offline previews
    const float* org = (const float*)(ref + RE_ORIGIN);
    const float* vo = (const float*)(g_base + VIEWORG_RVA);
    const float* F = vo + 3; const float* Lf = vo + 6; const float* U = vo + 9;
    const V3 dv = { org[0] - vo[0], org[1] - vo[1], org[2] - vo[2] };
    logger::logf("inspect: start %s (weapon %d, weaponstate %d, speed %.2f) - view DObj %p (%d bones, %d models, weapon from bone %d, hands' tag_weapon %d): "
                 "flash %s=%d, magazine %s=%d, bolt %s=%d%s, arms %s, left hand: %s; model origin %.1f %.1f %.1f (from the eye f %.1f l %.1f u %.1f)",
                 mode == 4 ? "TOSS" : mode == 3 ? "SPIN" : mode == 2 ? "FULL" : "basic", weapon, state, cvar_f(g_cv_speed, 1, 0.05f, 20), (const void*)dobj, dobj[0x17], dobj[0x16],
                 g_sk.gun0, g_sk.htag, ff, g_sk.flash, fc, g_sk.clip, fb, g_sk.bolt, g_sk.boltaction ? " (bolt action)" : "",
                 g_sk.arms ? "found" : "NOT found (the gun turns alone)", hand, org[0], org[1], org[2],
                 F[0] * dv.x + F[1] * dv.y + F[2] * dv.z, Lf[0] * dv.x + Lf[1] * dv.y + Lf[2] * dv.z, U[0] * dv.x + U[1] * dv.y + U[2] * dv.z);
    QueryPerformanceCounter(&g_t0);
    g_mode = mode; g_weapon = weapon; g_state = state;
    g_sk.dobj = dobj;
}

__attribute__((force_align_arg_pointer)) void __cdecl inspect_apply(uint8_t* ref, const uint8_t* ps) {
    if (!g_cv_go) {
        g_cv_go    = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect", "0", 0);
        g_cv_pivot = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_inspect_pivot", "0 0 0", 0);
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
        else start(asked == 4 ? 4 : asked == 3 ? 3 : asked == 2 ? 2 : 1, weapon, state, ref);
    }
    if (!g_mode) return;
    if (weapon != g_weapon) { stop("cut - weapon switch"); return; }
    if (state != g_state) { logger::logf("inspect: weaponstate %d -> %d", g_state, state); stop("cut - fire / reload"); return; }
    if (ads > 0.001f) { stop("cut - aiming"); return; }

    const float t = now_s() * cvar_f(g_cv_speed, 1, 0.05f, 20);
    if (g_mode == 3) {
        // the hand opens, the wrist cocks back then snaps forward (the torque that spins the
        // pistol), the pistol turns on the finger under gravity and friction, and the palm catches
        // it when the grip comes round slowly enough
        const float* F = (const float*)(g_base + VIEWORG_RVA + 12);
        const V3 g = scl({ -F[2], -F[5], -F[8] }, SPIN_G);          // world down, in view (model) axes
        const V3 a = g_sk.axis, r0 = g_sk.com;
        const float I = SPIN_K2 + dot(r0, r0);
        float dt = t - g_spin.last;
        dt = dt < 0 ? 0 : dt > 0.05f ? 0.05f : dt;
        g_spin.last = t;
        for (float done = 0; done < dt && g_sk.spin_ready; done += 0.001f) {
            const float h = dt - done < 0.001f ? dt - done : 0.001f;
            const float tt = t - dt + done;
            float acc;
            if (g_spin.tc < 0) {
                const V3 r = qrot(qaxis(a, g_spin.th * 57.29578f), r0);
                acc = dot(cross(r, g), a) / I - SPIN_VISC * g_spin.om - SPIN_COUL * tanhf(g_spin.om / 0.5f);
                if (tt >= SPIN_T_SNAP && tt < SPIN_T_SNAP_END) acc += SPIN_KICK;
                const float wrap = remainderf(g_spin.th, 6.2831853f);
                if ((tt > 0.95f && fabsf(wrap) < 0.52f && g_spin.om > 0 && g_spin.om < 20.0f) || tt > 3.0f) {
                    g_spin.tc = tt;
                    g_spin.tgt = g_spin.th - wrap;
                    logger::logf("inspect: caught after %.2f turn(s) at %.1f turns/s (%.2f s)", g_spin.th / 6.2831853f, g_spin.om / 6.2831853f, tt);
                }
            } else {
                acc = -900.0f * (g_spin.th - g_spin.tgt) - 60.0f * g_spin.om;
            }
            g_spin.om += acc * h;
            g_spin.th += g_spin.om * h;
        }
        const float tc = g_spin.tc;
        if (tc >= 0 && t > tc + 0.55f) { stop("done"); return; }
        g_sk.spin_deg = g_spin.th * 57.29578f;
        g_sk.open = ramp(t, 0.05f, 0.20f) * (tc < 0 ? 1.0f : 1 - ramp(t, tc, tc + 0.12f));
        g_sk.away = ramp(t, 0.0f, 0.30f) * (tc < 0 ? 1.0f : 1 - ramp(t, tc + 0.2f, tc + 0.5f));
        // the wrist: cocked back, snapped forward, back to rest; a small give when it catches
        const float back = ramp(t, 0.18f, SPIN_T_SNAP), snap = ramp(t, SPIN_T_SNAP, SPIN_T_SNAP_END), rest = ramp(t, SPIN_T_SNAP_END, 0.75f);
        const float give = tc < 0 ? 0.0f : bump(t, tc, 0.30f);
        g_sk.flick = (-14.0f * back + 24.0f * snap) * (1 - rest) + 7.0f * give;
        g_sk.flick_off = add(scl({ 0, 0, -0.6f }, back * (1 - snap)), add(scl({ 0.5f, 0, 1.5f }, snap * (1 - rest)), scl({ 0, 0, -1.0f }, give)));
        return;
    }
    if (g_mode == 4) {
        // toss: wind-up (the gun dips), a uniform push to V0, free flight under the view's gravity
        // with one full roll about the bore, caught where the hands wait, then a damped give
        const float* F = (const float*)(g_base + VIEWORG_RVA + 12);
        const V3 gv = scl({ -F[2], -F[5], -F[8] }, SPIN_G);       // world down, model axes
        const float gm = len(gv);
        const V3 up = scl(gv, -1.0f / gm);
        const V3 b = g_sk.dirs ? g_sk.b : V3{ 1, 0, 0 };
        constexpr float T0 = 0.30f, TR = 0.42f, V0 = 95.0f, ZC = 0.5f, WK = 14.0f;
        const float acc = V0 / (2.0f * (TR - T0));
        const float zr = -2.5f + acc * (TR - T0) * (TR - T0);       // release height (above rest)
        const float tf = (V0 + sqrtf(V0 * V0 + 2.0f * gm * (zr - ZC))) / gm;
        const float TC = TR + tf;                                   // caught
        const float w0 = 360.0f / (0.5f * (TR - T0) + tf);          // deg/s: one roll from push to catch
        float z, ang, pitch = 0;
        if (t < T0) {
            const float e = ease(t / T0); z = -2.5f * e; ang = 0; pitch = -8.0f * e;
        } else if (t < TR) {
            const float u = t - T0; z = -2.5f + acc * u * u; ang = 0.5f * (w0 / (TR - T0)) * u * u;
            pitch = -8.0f * (1.0f - ease(u / (TR - T0)));
        } else if (t < TC) {
            const float u = t - TR; z = zr + V0 * u - 0.5f * gm * u * u; ang = 0.5f * w0 * (TR - T0) + w0 * u;
        } else {
            const float u = t - TC, vc = V0 - gm * tf;               // vc < 0: the gun lands in the hands
            if (u > 0.6f) { stop("done"); return; }
            z = (ZC + (vc + WK * ZC) * u) * expf(-WK * u); ang = 360.0f;
        }
        g_sk.gq = qmul(qeuler(0, pitch, 0), qaxis(b, ang));
        g_sk.gslide = scl(up, z);
        g_sk.detach = ramp(t, TR - 0.02f, TR + 0.06f) * (1.0f - ramp(t, TC - 0.08f, TC));
        g_sk.open = g_sk.detach;
        return;
    }
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
            g_sk.grab = g_sk.grip ? ramp(t, 2.85f, 3.15f) * (1 - ramp(t, 4.20f, 4.50f)) : 0.0f;
            g_sk.mag = ramp(t, 3.15f, 3.45f) * (1 - ramp(t, 4.05f, 4.17f));
            g_sk.twist = (t > 3.45f && t < 4.05f) ? 14.0f * sinf(3.14159265f * (t - 3.45f) / 0.60f) : 0.0f;
            jolt += bump(t, 4.17f, 0.16f);                      // the magazine seated
        }
        if (jolt > 0) { q = qmul(qeuler(0, 2.2f * jolt, -1.5f * jolt), q); slide.z += 0.25f * jolt; }
    }
    g_sk.gq = q;
    g_sk.gslide = slide;
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
// turn a bone about a point
void spin(const uint8_t* d, int b, Q r, V3 pivot) {
    float* m = bone(d, b);
    for (int k = 0; k < 3; ++k) set_row(m, k, qrot(r, row(m, k)));
    set_row(m, 3, add(pivot, qrot(r, sub(row(m, 3), pivot))));
}
void shift(const uint8_t* d, int b, V3 v) { float* m = bone(d, b); set_row(m, 3, add(row(m, 3), v)); }
// limb entries at or under bone `root` (the list is parents first)
void subtree(const Limb& L, int root, bool* in) {
    for (int i = 0; i < L.n; ++i) {
        in[i] = L.idx[i] == root || L.par[i] == root;
        for (int j = 0; j < i && !in[i]; ++j) in[i] = in[j] && L.idx[j] == L.par[i];
    }
}

// Puts the hand of an arm at (tp, tq): upper arm and forearm keep their lengths and the elbow
// stays in the plane it bends in (two-bone solve from the shoulder, which does not move), then
// the hand is turned; its fingers ride with it. fingers > 0: the left hand's fingers take the
// local rotations of the reload grip, blended by that much.
void pose_fingers(const uint8_t* d, const Limb& L, float w);
void solve_limb(const uint8_t* d, const Limb& L, Q tq, V3 tp, float fingers) {
    float* H = bone(d, L.hand);
    const V3 W0 = row(H, 3);
    const V3 S = row(bone(d, L.up), 3), E = row(bone(d, L.fore), 3);
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
    const Q r1 = between(sub(E, S), sub(E2, S));                 // upper arm: the elbow to E2
    spin(d, L.up, r1, S);
    for (int i = 0; i < L.n; ++i) spin(d, L.idx[i], r1, S);
    const Q r2 = between(sub(row(H, 3), E2), sub(T2, E2));       // forearm: the wrist to the target
    subtree(L, L.fore, in);
    for (int i = 0; i < L.n; ++i) if (in[i]) spin(d, L.idx[i], r2, E2);
    const Q r3 = qmul(tq, qconj(get_rot(H)));                    // the hand turned
    const V3 W2 = row(H, 3);
    subtree(L, L.hand, in);
    for (int i = 0; i < L.n; ++i) if (in[i]) spin(d, L.idx[i], r3, W2);
    if (fingers > 0) pose_fingers(d, L, fingers);
}
// the fingers of a hand (every bone under it that has an lq) to their lq, blended by w; bone lengths kept
void pose_fingers(const uint8_t* d, const Limb& L, float w) {
    bool in[40];
    subtree(L, L.hand, in);
    Q lcur[40]; V3 tloc[40];
    for (int i = 0; i < L.n; ++i) {
        if (!in[i] || L.idx[i] == L.hand || !L.has_lq[i]) continue;
        const float* m = bone(d, L.idx[i]);
        const float* pm = bone(d, L.par[i]);
        const Q pq = qconj(get_rot(pm));
        lcur[i] = qmul(pq, get_rot(m));
        tloc[i] = qrot(pq, sub(row(m, 3), row(pm, 3)));
    }
    for (int i = 0; i < L.n; ++i) {
        if (!in[i] || L.idx[i] == L.hand || !L.has_lq[i]) continue;
        float* m = bone(d, L.idx[i]);
        const float* pm = bone(d, L.par[i]);
        const Q pq = get_rot(pm);
        set_rot(m, qmul(pq, slerp(lcur[i], L.lq[i], w)));
        set_row(m, 3, add(row(pm, 3), qrot(pq, tloc[i])));
    }
}
bool limb_ok(const Limb& L, int n) { return L.up >= 0 && L.up < n && L.fore < n && L.hand < n; }
}  // namespace

// Every skeleton the render builder makes goes through here (bone_probe); only the view model
// of an inspect is touched. The gun turns about its own middle, the shoulders stay where the
// animation put them, and both arms are re-solved to keep the hands where they hold the gun -
// or, for the left one, on the magazine.
void inspect_on_skeleton(const void* dobj) {
    const uint8_t* d = g_sk.dobj;
    if (!d || dobj != d || !*(void* const*)(d + 4)) return;
    const int n = d[0x17];
    if (g_sk.gun0 <= 0 || g_sk.gun0 >= n) return;
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
    // the middle of the gun (its bones), before anything moves
    V3 C = { 0, 0, 0 };
    for (int i = g_sk.gun0; i < n; ++i) C = add(C, row(bone(d, i), 3));
    C = scl(C, 1.0f / (float)(n - g_sk.gun0));
    {
        float off[3] = { 0, 0, 0 };
        sscanf(*(const char* const*)((const uint8_t*)g_cv_pivot + CV_STRING), "%f %f %f", &off[0], &off[1], &off[2]);
        C = add(C, { off[0], off[1], off[2] });
    }
    const bool arms = g_sk.arms && limb_ok(g_R, n) && limb_ok(g_L, n);
    Q qR = { 1, 0, 0, 0 }, qL = { 1, 0, 0, 0 };
    V3 pR = { 0, 0, 0 }, pL = { 0, 0, 0 };
    if (arms) {
        qR = get_rot(bone(d, g_R.hand)); pR = row(bone(d, g_R.hand), 3);
        qL = get_rot(bone(d, g_L.hand)); pL = row(bone(d, g_L.hand), 3);
    }
    if (g_mode == 3) {
        // pistol spin: the pistol hangs on the right index finger (in the trigger guard) and turns
        // about its lateral axis; the hand (wrist snap, catch) moves the finger it hangs on
        const V3 piv0 = scl(add(row(bone(d, g_sk.idx11), 3), row(bone(d, g_sk.idx12), 3)), 0.5f);
        const V3 a = norm(cross({ 0, 0, 1 }, g_sk.b));
        if (!g_sk.spin_ready) {
            // centre of mass: below the finger, toward the grip (the magazine's way out), a little forward
            g_sk.axis = a;
            g_sk.com = add(scl(g_sk.o, 1.6f), scl(g_sk.b, 0.3f));
            g_sk.spin_ready = true;
            logger::logf("inspect: spin axis (%.2f %.2f %.2f), centre of mass (%.2f %.2f %.2f) from the finger",
                         a.x, a.y, a.z, g_sk.com.x, g_sk.com.y, g_sk.com.z);
        }
        V3 piv = piv0;
        if (arms) {
            solve_limb(d, g_R, qmul(qaxis(a, g_sk.flick), qR), add(pR, g_sk.flick_off), 0);
            if (g_sk.open > 0) pose_fingers(d, g_R, g_sk.open);
            piv = scl(add(row(bone(d, g_sk.idx11), 3), row(bone(d, g_sk.idx12), 3)), 0.5f);
        }
        const Q qs = qaxis(a, g_sk.spin_deg);
        for (int i = g_sk.gun0; i < n; ++i) { spin(d, i, qs, piv0); shift(d, i, sub(piv, piv0)); }
        if (g_sk.htag >= 0) { spin(d, g_sk.htag, qs, piv0); shift(d, g_sk.htag, sub(piv, piv0)); }
        if (arms && g_sk.away > 0) solve_limb(d, g_L, qL, add(pL, scl({ 0, 3, -6 }, g_sk.away)), 0);
        return;
    }
    // 1. bolt and magazine, in the gun as it rests
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
    // 2. the gun turns about its middle and slides
    const Q gq = g_sk.gq;
    const V3 gs = g_sk.gslide;
    for (int i = g_sk.gun0; i < n; ++i) { spin(d, i, gq, C); shift(d, i, gs); }
    if (g_sk.htag >= 0) { spin(d, g_sk.htag, gq, C); shift(d, g_sk.htag, gs); }
    if (!arms) return;
    // 3. the right hand stays on its grip - or, while a tossed gun flies, waits a little higher, open
    const float dt = g_sk.detach;
    Q rq = qmul(gq, qR);
    V3 rp = add(add(C, qrot(gq, sub(pR, C))), gs);
    if (dt > 0) { rq = slerp(rq, qR, dt); rp = add(rp, scl(sub(add(pR, { 0.5f, 0, 1.5f }), rp), dt)); }
    solve_limb(d, g_R, rq, rp, 0);
    if (g_mode == 4 && g_sk.open > 0) pose_fingers(d, g_R, g_sk.open);
    // 4. the left hand stays on the gun, or goes to the magazine (or waits for the tossed gun)
    Q tq = qmul(gq, qL);
    V3 tp = add(add(C, qrot(gq, sub(pL, C))), gs);
    if (dt > 0) { tq = slerp(tq, qL, dt); tp = add(tp, scl(sub(add(pL, { 0.5f, 0.8f, 1.2f }), tp), dt)); }
    const float w = g_sk.grip ? g_sk.grab : 0.0f;
    if (w > 0 && g_sk.clip >= 0 && g_sk.clip < n) {
        const float* cm = bone(d, g_sk.clip);
        const Q qc = get_rot(cm);
        const V3 pc = row(cm, 3);
        tq = slerp(tq, qmul(qc, g_sk.grip_q), w);
        tp = add(tp, scl(sub(add(pc, qrot(qc, g_sk.grip_p)), tp), w));
    }
    solve_limb(d, g_L, tq, tp, w);
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
