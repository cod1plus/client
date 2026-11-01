// render_clock.cpp - see render_clock.h
//
// CoDMP.exe 1.5:
//   0x412915  CL_Frame: call SCR_UpdateScreen (E8 rel32 -> 0x418b30), after CL_SendCmd 0x40ca60
//             and CL_SetCGameTime 0x404fe0. SCR_UpdateScreen pumps the window messages only
//             in state 4 (loading); in game it draws (SCR_DrawScreenField 0x4188d0 ->
//             CL_CGameRendering 0x404cd0 = VM_Call(cgvm, CG_DRAW_ACTIVE_FRAME,
//             cl.serverTime [0x148dca0], stereo, demoplaying [0x18ba6cc])) and ends the frame
//             (re.EndFrame [0x148bb2c] -> SwapBuffers). It guards its own recursion [0x1460340].
//   cls.state [0x15b8aa0] == 6: in game.
//   CL_MouseMove 0x40bd60: accelSens = sensitivity + rate * cl_mouseAccel, times
//             cl.cgameSensitivity [0x1493bbc]; viewangles[YAW] [0x1493be4] -= m_yaw * mx * it,
//             viewangles[PITCH] [0x1493be0] += m_pitch * my * it when cl_freelook; turn-rate
//             limits [0x1493bc4] (yaw) / [0x1493bc0] (pitch) when non-zero; m_filter averages.
// cgame_mp_x86.dll (RVAs):
//   0x355e9   CG_DrawActiveFrame: call CG_CalcViewValues (E8 -> 0x34bb0), before the scene is
//             built - CG_AddViewWeapon 0x38b60 positions the gun from the refdef afterwards.
//   0x34bb0   CG_CalcViewValues: refdef (0x20d330) vieworg 0x20d348 = ps.origin, refdefViewAngles
//             0x20d380 = ps.viewangles, bob / lean, then AnglesToAxis -> axis 0x20d354 (forward,
//             left, up). [0x1e5f10] set = a special view (early branch): left alone.
//   cg.time 0x20af00, cg.predictedPlayerState 0x20af14: commandTime +0, origin +0x14,
//             velocity +0x20, viewangles +0xc0.
#include "performance/render_clock.h"
#include "core/hook_prof.h"
#include "performance/frame_limiter.h"   // CODMP_MOUSE_ACTIVE_VA / _INITIALIZED_VA
#include "input/rinput.h"
#include "input/sensitivity_mg.h"
#include "core/logger.h"
#include "features/settings_menu.h"      // CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t VA_SCR_UPDATESCREEN = 0x00418b30;
constexpr uintptr_t VA_CLFRAME_SCR_CALL = 0x00412915;
constexpr uintptr_t VA_CL_SERVERTIME    = 0x0148dca0;
constexpr uintptr_t VA_CLS_STATE        = 0x015b8aa0;
constexpr uintptr_t VA_CLC_DEMOPLAYING  = 0x018ba6cc;
constexpr uintptr_t VA_CL_CGAMESENS     = 0x01493bbc;
constexpr uintptr_t VA_CL_YAWLIMIT      = 0x01493bc4;
constexpr uintptr_t VA_CL_PITCHLIMIT    = 0x01493bc0;
constexpr int       CA_ACTIVE           = 6;

constexpr uintptr_t RVA_CG_CALCVIEW_CALL = 0x355e9;
constexpr uintptr_t RVA_CG_CALCVIEW      = 0x34bb0;
constexpr uintptr_t RVA_CG_SPECIALVIEW   = 0x1e5f10;
constexpr uintptr_t RVA_CG_TIME          = 0x20af00;
constexpr uintptr_t RVA_PS               = 0x20af14;
constexpr int       PS_CMDTIME = 0x00, PS_VELOCITY = 0x20;
constexpr uintptr_t RVA_REF_ORG          = 0x20d348;
constexpr uintptr_t RVA_REF_AXIS         = 0x20d354;
constexpr uintptr_t RVA_REF_ANGLES       = 0x20d380;

constexpr int  MAX_EXTRAP_MS = 12;      // beyond: not our own prediction (following, spectating)
constexpr int  CVAR_VALUE = 0x1c, CVAR_INTEGER = 0x20;

typedef void  (__cdecl* VoidFn_t)();
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);

volatile LONG g_hz = 0;                  // cod1x_render_clock (watcher writes, game thread reads)
volatile LONG g_msec = 4;                // 1000 / com_maxfps, for the between-frames clamp
bool      g_installed = false;
uintptr_t g_cgame = 0;                   // cgame base whose CG_CalcViewValues call we own
VoidFn_t  g_calcview = nullptr;          // that cgame's CG_CalcViewValues

// cvars the view hook reads (pointers cached by the watcher, value read at use)
void* volatile g_cv_sens = nullptr;
void* volatile g_cv_myaw = nullptr;
void* volatile g_cv_mpitch = nullptr;
void* volatile g_cv_freelook = nullptr;
void* volatile g_cv_filter = nullptr;
void* volatile g_cv_accel = nullptr;

LARGE_INTEGER g_freq = {};
LONGLONG g_next = 0;                     // next render instant (QPC), 0 = not started
LONGLONG g_frame_qpc = 0;                // the last command frame: when ...
int      g_frame_st = 0;                 // ... and its cl.serverTime
bool     g_between = false;              // the view hook acts only on frames drawn between
LONGLONG g_draw_qpc = 0;                 // the instant the current draw shows (QPC)
int      g_last_st = 0;                  // cl.serverTime of the last draw: never shown backwards
long     g_st_held = 0;                  // draws whose engine time was behind the last one shown
LONGLONG g_cost = 0;                     // moving average of one draw (QPC ticks)
LONGLONG g_cost_hi = 0;                  // its recent peak, decaying over ~256 draws: what a between draw must fit
long     g_st_in_frame = 0, g_st_between = 0, g_st_skipped = 0;
int      g_axis_checks = 0;              // first in-frame draws: our AnglesToAxis vs the engine's

inline int& cl_server_time() { return *(int*)VA_CL_SERVERTIME; }

bool in_game() {
    return *(volatile int*)VA_CLS_STATE == CA_ACTIVE && !*(volatile int*)VA_CLC_DEMOPLAYING &&
           g_cgame && g_calcview;
}

float cvf(void* cv, float fallback) { return cv ? *(float*)((char*)cv + CVAR_VALUE) : fallback; }
int   cvi(void* cv, int fallback)   { return cv ? *(int*)((char*)cv + CVAR_INTEGER) : fallback; }

// Q3 AngleVectors; the refdef axis is (forward, left = -right, up)
void angles_to_axis(const float a[3], float axis[9]) {
    const float k = 3.14159265358979f / 180.0f;
    const float sy = sinf(a[1] * k), cy = cosf(a[1] * k);
    const float sp = sinf(a[0] * k), cp = cosf(a[0] * k);
    const float sr = sinf(a[2] * k), cr = cosf(a[2] * k);
    axis[0] = cp * cy; axis[1] = cp * sy; axis[2] = -sp;
    const float rx = -sr * sp * cy + cr * sy, ry = -sr * sp * sy - cr * cy, rz = -sr * cp;
    axis[3] = -rx; axis[4] = -ry; axis[5] = -rz;
    axis[6] = cr * sp * cy + sr * sy; axis[7] = cr * sp * sy - sr * cy; axis[8] = cr * cp;
}

void draw(LONGLONG now, bool between) {
    const LONG hz = g_hz;
    const LONGLONG period = hz > 0 ? g_freq.QuadPart / hz : g_freq.QuadPart / 250;
    const LONGLONG slot = g_next ? g_next : now;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    // The cgame's clock is whole milliseconds and the engine nudges cl.serverTime by a
    // millisecond now and then (CL_SetCGameTime's drift correction): a frame drawn between
    // two commands at base + 3 can be followed by a command frame at base + 2. Shown as is,
    // everything driven by that clock (the other players, animated / scrolling textures)
    // steps back a millisecond, several times a second - a visible shiver. The time a draw
    // shows therefore never goes below the last one shown.
    const int saved = cl_server_time();
    int show = saved;
    if (between) {
        LONGLONG add = (now - g_frame_qpc) * 1000 / g_freq.QuadPart;
        const LONG msec = g_msec;
        if (add > msec - 1) add = msec - 1;
        if (add < 0) add = 0;
        show = g_frame_st + (int)add;
    }
    if (g_last_st && show < g_last_st && g_last_st - show < 50) { show = g_last_st; ++g_st_held; }
    g_last_st = show;
    cl_server_time() = show;
    g_between = between;
    g_draw_qpc = now;
    if (between) { PROF_SCOPE(PROF_RCDRAW); ((VoidFn_t)VA_SCR_UPDATESCREEN)(); }
    else ((VoidFn_t)VA_SCR_UPDATESCREEN)();
    cl_server_time() = saved;
    g_between = false;
    QueryPerformanceCounter(&t1);
    const LONGLONG c = t1.QuadPart - t0.QuadPart;
    g_cost = g_cost ? (g_cost * 7 + c) / 8 : c;
    if (c > g_cost_hi) g_cost_hi = c; else g_cost_hi -= g_cost_hi / 256;
    g_next = slot + period;
    if (g_next <= now) g_next = now + period;          // fell behind: restart the cadence
}

// replaces CL_Frame's call to SCR_UpdateScreen
extern "C" void __cdecl render_gate() {
    if (g_hz <= 0 || !in_game()) {
        g_next = 0;
        g_last_st = 0;
        ((VoidFn_t)VA_SCR_UPDATESCREEN)();
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_frame_qpc = now.QuadPart;
    g_frame_st = cl_server_time();
    const LONGLONG period = g_freq.QuadPart / g_hz;
    if (g_next == 0 || now.QuadPart >= g_next - period / 8) {
        draw(now.QuadPart, false);
        ++g_st_in_frame;
    } else {
        ++g_st_skipped;               // a render instant comes before the next frame: drawn then
    }
}

void adjust_view() {
    const char* cg = (const char*)g_cgame;
    if (*(const int*)(cg + RVA_CG_SPECIALVIEW)) return;
    const char* ps = cg + RVA_PS;
    const int dt = *(const int*)(cg + RVA_CG_TIME) - *(const int*)(ps + PS_CMDTIME);
    if (dt < 0 || dt > MAX_EXTRAP_MS) return;            // following / spectating: not ours

    // where the predicted motion has carried the eye since the last command - over the REAL
    // time since that command frame (QPC), not the cgame's whole-millisecond clock: at 320 Hz
    // the latter steps 3, 3, 3, 3, 3, 3, 3 then 4 ms and the eye would advance unevenly
    // (0.2 u at 190 u/s, enough to make near walls shiver while moving)
    double secs = (double)(g_draw_qpc - g_frame_qpc) / (double)g_freq.QuadPart;
    if (secs < 0) secs = 0;
    if (secs > MAX_EXTRAP_MS * 0.001) secs = MAX_EXTRAP_MS * 0.001;
    float* org = (float*)(cg + RVA_REF_ORG);
    const float* vel = (const float*)(ps + PS_VELOCITY);
    const float s = (float)secs;
    org[0] += vel[0] * s; org[1] += vel[1] * s; org[2] += vel[2] * s;

    // the mouse movement the next command will carry, turned exactly like CL_MouseMove will
    if (!*(volatile int*)CODMP_MOUSE_INITIALIZED_VA || !*(volatile int*)CODMP_MOUSE_ACTIVE_VA) return;
    if (*(const float*)VA_CL_YAWLIMIT != 0.0f || *(const float*)VA_CL_PITCHLIMIT != 0.0f) return;
    if (cvi(g_cv_filter, 0) || cvf(g_cv_accel, 0.0f) != 0.0f) return;   // frame-dependent: not predicted
    long mx = 0, my = 0;
    rinput_peek_delta(&mx, &my);
    if (!mx && !my) return;
    float kx, ky;                                          // on a mounted MG: sensitivity_mg's factors
    if (!sensitivity_mg_active(&kx, &ky)) kx = ky = cvf(g_cv_sens, 5.0f) * *(const float*)VA_CL_CGAMESENS;
    float* ang = (float*)(cg + RVA_REF_ANGLES);
    ang[1] -= cvf(g_cv_myaw, 0.022f) * kx * (float)mx;
    if (cvi(g_cv_freelook, 1)) ang[0] += cvf(g_cv_mpitch, 0.022f) * ky * (float)my;
    angles_to_axis(ang, (float*)(cg + RVA_REF_AXIS));
}

// replaces CG_DrawActiveFrame's call to CG_CalcViewValues
extern "C" void __cdecl calc_view_hook() {
    VoidFn_t orig = g_calcview;
    if (orig) orig();
    if (!g_cgame) return;
    if (g_between) {
        adjust_view();
    } else if (g_axis_checks < 3 && g_hz > 0) {
        // the convention check: our AnglesToAxis must give the engine's own axis
        const char* cg = (const char*)g_cgame;
        float ax[9];
        angles_to_axis((const float*)(cg + RVA_REF_ANGLES), ax);
        const float* eng = (const float*)(cg + RVA_REF_AXIS);
        float worst = 0;
        for (int i = 0; i < 9; ++i) worst = fmaxf(worst, fabsf(ax[i] - eng[i]));
        ++g_axis_checks;
        logger::logf("render_clock: axis check %d - ours vs the engine's, max difference %.6f%s",
                     g_axis_checks, worst, worst < 0.001f ? "" : " (MISMATCH: the turn would be wrong)");
    }
}

bool patch_call(uintptr_t site, uintptr_t expect, uintptr_t to) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return false;
    const uintptr_t cur = site + 5 + *(int32_t*)(p + 1);
    if (cur == to) return true;                         // already ours
    if (cur != expect) return false;
    DWORD old = 0;
    if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(p + 1) = (int32_t)(to - (site + 5));
    VirtualProtect(p + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void render_clock_install() {
    if (g_installed) return;
    QueryPerformanceFrequency(&g_freq);
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("render_clock: CoDMP.exe not at its preferred base - not installed");
        return;
    }
    if (!patch_call(VA_CLFRAME_SCR_CALL, VA_SCR_UPDATESCREEN, (uintptr_t)&render_gate)) {
        logger::logf("render_clock: CL_Frame's SCR_UpdateScreen call not found at 0x%08x - not installed",
                     (unsigned)VA_CLFRAME_SCR_CALL);
        return;
    }
    g_installed = true;
    logger::logf("render_clock: installed (cod1x_render_clock 0 = off; N = the screen gets N evenly spaced "
                 "frames a second, commands stay at com_maxfps)");
}

void render_clock_install_cgame(HMODULE cgame) {
    if (!g_installed || !cgame) return;
    const uintptr_t base = (uintptr_t)cgame;
    if (!patch_call(base + RVA_CG_CALCVIEW_CALL, base + RVA_CG_CALCVIEW, (uintptr_t)&calc_view_hook)) {
        logger::logf("render_clock: CG_CalcViewValues call not found in this cgame - view hook off");
        g_cgame = 0;
        return;
    }
    g_calcview = (VoidFn_t)(base + RVA_CG_CALCVIEW);
    g_cgame = base;
    g_axis_checks = 0;
}

void render_clock_tick() {
    if (!g_installed) return;
    static bool registered = false;
    static DWORD last_log = 0;
    static LONG last_hz = -1;
    if (!registered) {
        // 1.6.15: a new name, so every player starts OFF again - 1.6.13/1.6.14 archived
        // cod1x_render_hz in each config_mp.cfg and its guard below never fired (see THE BUDGET)
        ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_render_clock", "0", 1 /* archive */);
        registered = true;
    }
    Cvar_FindVar_t find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
    int hz = cvi(find("cod1x_render_clock"), 0);
    bool follow = false;
    if (hz == -1) {                       // follow the screen: r_displayRefresh, else the mode in use
        follow = true;
        hz = cvi(find("r_displayRefresh"), 0);
        if (hz <= 0) {
            DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
        }
    }
    if (hz < 0) hz = 0;
    if (hz > 1000) hz = 1000;
    if (hz && hz < 30) hz = 30;
    const int maxfps = cvi(find("com_maxfps"), 250);
    g_msec = maxfps > 0 ? 1000 / maxfps : 4;
    g_cv_sens = find("sensitivity");
    g_cv_myaw = find("m_yaw");
    g_cv_mpitch = find("m_pitch");
    g_cv_freelook = find("cl_freelook");
    g_cv_filter = find("m_filter");
    g_cv_accel = find("cl_mouseAccel");

    // THE BUDGET (10-23). Every draw past com_maxfps is extra CPU work: the engine's whole
    // render, 2.2-4 ms a draw on a modest CPU with the window up (1.4 ms minimized). 320 a
    // second beside 250 command frames asked this PC for more than a second of work per
    // second: the command frames came late (the limiter's reloops, 1400-3200 a minute),
    // the fps counter sat at 232 and the game felt unstable - with the clock off the same
    // session had 0 late frames. When the command frames start coming late, the rate is
    // capped at com_maxfps (no extra draw: the clock-off work, the draws merely re-timed)
    // and the asked rate tried again 30 s later, three times a session.
    // 1.6.15: that guard NEVER fired. This tick runs every ~5 ms and the late poll resets its
    // counters at each call, so "frames >= 100" could not be met: 1.6.13/1.6.14 drew the extra
    // frames whatever they cost - players on a modest CPU lost their fps as soon as players came
    // into view (each one animated and drawn again for every extra frame). Now the counts are
    // summed over half-second windows, 2 % of late command frames is enough, and the cap holds
    // until the player changes the setting (no retry in the middle of a fight).
    static LONG  s_fit = 0;                 // 0 = none, else the cap in force
    static int   s_fit_req = 0;             // the requested rate the cap was set for
    static long  s_acc_late = 0, s_acc_frames = 0;
    static DWORD s_acc_since = 0;
    long late = 0, frames = 0;
    frame_limiter_late_poll(&late, &frames);
    const DWORD tnow = GetTickCount();
    const int requested = hz;
    if (s_fit && requested != s_fit_req) {  // the player changed the setting: the cap goes
        s_fit = 0;
        s_acc_late = s_acc_frames = 0; s_acc_since = tnow;
    }
    s_acc_late += late;
    s_acc_frames += frames;
    if (!s_acc_since) s_acc_since = tnow;
    if (s_fit && hz > s_fit) {
        hz = s_fit;
    } else if (tnow - s_acc_since >= 500) {
        if (hz > maxfps && s_acc_frames >= 60 && s_acc_late * 50 > s_acc_frames) {   // > 2 % late in half a second
            s_fit = maxfps; s_fit_req = requested;
            hz = maxfps;
            logger::logf("render_clock: %d a second does not fit beside com_maxfps %d on this CPU (a draw costs %.2f ms; "
                         "%ld of %ld command frames came late in %lu ms) - %d a second until the setting changes",
                         requested, maxfps, g_freq.QuadPart ? (double)g_cost * 1000.0 / (double)g_freq.QuadPart : 0.0,
                         s_acc_late, s_acc_frames, (unsigned long)(tnow - s_acc_since), maxfps);
        }
        s_acc_late = s_acc_frames = 0;
        s_acc_since = tnow;
    }
    if (hz != last_hz) {
        InterlockedExchange(&g_hz, hz);
        if (hz) logger::logf("render_clock: ON - %d frames a second on screen%s, commands at com_maxfps %d", hz,
                             follow ? " (follows the screen's rate)" : "", maxfps);
        else    logger::logf("render_clock: off (engine behaviour)");
        last_hz = hz;
        g_axis_checks = 0;
    }
    const DWORD now = GetTickCount();
    if (hz && now - last_log >= 10000) {
        if (last_log && (g_st_in_frame || g_st_between))
            logger::logf("render_clock: last 10 s - %ld drawn with a command, %ld between commands, "
                         "%ld command frames without a draw, %ld engine times held; draw %.2f ms (peaks %.2f)",
                         g_st_in_frame, g_st_between, g_st_skipped, g_st_held,
                         g_freq.QuadPart ? (double)g_cost * 1000.0 / (double)g_freq.QuadPart : 0.0,
                         g_freq.QuadPart ? (double)g_cost_hi * 1000.0 / (double)g_freq.QuadPart : 0.0);
        g_st_in_frame = g_st_between = g_st_skipped = g_st_held = 0;
        last_log = now;
    }
}

bool render_clock_slot_before(LONGLONG deadline, LONGLONG now, LONGLONG* slot) {
    if (g_hz <= 0 || !g_next || !in_game()) return false;
    // the draw at its recent WORST + 0.33 ms: the mean (1.85 ms) let 2-5 ms draws through and
    // the command frame behind them came late (10-23)
    const LONGLONG guard = g_cost_hi + g_freq.QuadPart / 3000;
    if (g_next + guard >= deadline) return false;
    *slot = g_next > now ? g_next : now;
    return true;
}

void render_clock_draw_between(LONGLONG now) {
    draw(now, true);
    ++g_st_between;
}

}  // namespace patches
