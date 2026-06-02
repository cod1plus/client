#include "patches.h"
#include "viewheight_fix.h"
#include "lean_fix.h"
#include "swing_fix.h"
#include "lean_amplify.h"
#include "logger.h"
#include "version_patch.h"
#include "window_patch.h"
#include "fullscreen_patch.h"
#include "fps_cap.h"
#include "frame_limiter.h"
#include "updater.h"
#include "demo_upload.h"
#include "cpu_affinity.h"
#include "process_priority.h"
#include "working_set.h"
#include "fso_disable.h"
#include "widescreen_fix.h"
#include "avatar_overlay.h"
#include "discord_rpc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

namespace {

// Locate "cod1reloaded.ini" in the same directory as the DLL.
bool get_config_path(HMODULE self_module, char* out, size_t out_size) {
    char dll_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(self_module, dll_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;

    char* slash = strrchr(dll_path, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';

    int written = snprintf(out, out_size, "%scod1reloaded.ini", dll_path);
    return written > 0 && (size_t)written < out_size;
}

// Read a float key from the [cod1reloaded] section. Returns fallback on miss.
float read_ini_float(const char* path, const char* key, float fallback) {
    char buf[64];
    DWORD n = GetPrivateProfileStringA(
        "cod1reloaded", key, "", buf, sizeof(buf), path);
    if (n == 0) return fallback;
    char* end = nullptr;
    float v = strtof(buf, &end);
    if (end == buf) return fallback;
    return v;
}

bool read_ini_bool(const char* path, const char* key, bool fallback) {
    char buf[16];
    DWORD n = GetPrivateProfileStringA(
        "cod1reloaded", key, "", buf, sizeof(buf), path);
    if (n == 0) return fallback;
    if (!_stricmp(buf, "true") || !_stricmp(buf, "1") || !_stricmp(buf, "yes")) return true;
    if (!_stricmp(buf, "false") || !_stricmp(buf, "0") || !_stricmp(buf, "no")) return false;
    return fallback;
}

int read_ini_int(const char* path, const char* key, int fallback) {
    char buf[32];
    DWORD n = GetPrivateProfileStringA(
        "cod1reloaded", key, "", buf, sizeof(buf), path);
    if (n == 0) return fallback;
    char* end = nullptr;
    long v = strtol(buf, &end, 10);
    if (end == buf) return fallback;
    return (int)v;
}

// Read a free-form string (e.g. "auto", "16:9"). Out buf must be >= 32 bytes.
void read_ini_string(const char* path, const char* key,
                     char* out, size_t out_size, const char* fallback) {
    DWORD n = GetPrivateProfileStringA(
        "cod1reloaded", key, fallback, out, (DWORD)out_size, path);
    (void)n;
}

}  // namespace

void load_config(HMODULE self_module) {
    char ini_path[MAX_PATH];
    if (!get_config_path(self_module, ini_path, sizeof(ini_path))) return;
    if (GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES) return;

    g_viewheight_config.viewheight_lerp_speed = read_ini_float(
        ini_path, "viewheight_lerp_speed", VIEWHEIGHT_LERP_SPEED_FALLBACK);

    g_lean_fix_config.enable = read_ini_bool(
        ini_path, "lean_fix_enable", g_lean_fix_config.enable);
    g_lean_fix_config.apply_in_stand = read_ini_bool(
        ini_path, "lean_fix_apply_in_stand", g_lean_fix_config.apply_in_stand);
    g_lean_fix_config.neck_yaw_scale = read_ini_float(
        ini_path, "lean_neck_yaw_scale", g_lean_fix_config.neck_yaw_scale);
    g_lean_fix_config.head_yaw_scale = read_ini_float(
        ini_path, "lean_head_yaw_scale", g_lean_fix_config.head_yaw_scale);
    g_lean_fix_config.back_up_yaw_scale = read_ini_float(
        ini_path, "lean_back_up_yaw_scale", g_lean_fix_config.back_up_yaw_scale);
    g_lean_fix_config.back_mid_yaw_scale = read_ini_float(
        ini_path, "lean_back_mid_yaw_scale", g_lean_fix_config.back_mid_yaw_scale);
    g_lean_fix_config.back_low_yaw_scale = read_ini_float(
        ini_path, "lean_back_low_yaw_scale", g_lean_fix_config.back_low_yaw_scale);
    g_lean_fix_config.pelvis_yaw_scale = read_ini_float(
        ini_path, "lean_pelvis_yaw_scale", g_lean_fix_config.pelvis_yaw_scale);
    g_lean_fix_config.neck_roll_scale = read_ini_float(
        ini_path, "lean_neck_roll_scale", g_lean_fix_config.neck_roll_scale);
    g_lean_fix_config.head_roll_scale = read_ini_float(
        ini_path, "lean_head_roll_scale", g_lean_fix_config.head_roll_scale);
    g_lean_fix_config.back_up_roll_scale = read_ini_float(
        ini_path, "lean_back_up_roll_scale", g_lean_fix_config.back_up_roll_scale);
    g_lean_fix_config.back_mid_roll_scale = read_ini_float(
        ini_path, "lean_back_mid_roll_scale", g_lean_fix_config.back_mid_roll_scale);
    g_lean_fix_config.back_low_roll_scale = read_ini_float(
        ini_path, "lean_back_low_roll_scale", g_lean_fix_config.back_low_roll_scale);
    g_lean_fix_config.pelvis_roll_scale = read_ini_float(
        ini_path, "lean_pelvis_roll_scale", g_lean_fix_config.pelvis_roll_scale);
    // PITCH scales (forward tilt) - fix "rolls forward" bug
    g_lean_fix_config.neck_pitch_scale = read_ini_float(
        ini_path, "lean_neck_pitch_scale", g_lean_fix_config.neck_pitch_scale);
    g_lean_fix_config.head_pitch_scale = read_ini_float(
        ini_path, "lean_head_pitch_scale", g_lean_fix_config.head_pitch_scale);
    g_lean_fix_config.back_up_pitch_scale = read_ini_float(
        ini_path, "lean_back_up_pitch_scale", g_lean_fix_config.back_up_pitch_scale);
    g_lean_fix_config.back_mid_pitch_scale = read_ini_float(
        ini_path, "lean_back_mid_pitch_scale", g_lean_fix_config.back_mid_pitch_scale);
    g_lean_fix_config.back_low_pitch_scale = read_ini_float(
        ini_path, "lean_back_low_pitch_scale", g_lean_fix_config.back_low_pitch_scale);
    g_lean_fix_config.pelvis_pitch_scale = read_ini_float(
        ini_path, "lean_pelvis_pitch_scale", g_lean_fix_config.pelvis_pitch_scale);
    // Left-lean PITCH overrides (pre-tuned to fix forward-tilt bug)
    g_lean_fix_config.left_neck_pitch_scale = read_ini_float(
        ini_path, "lean_left_neck_pitch_scale", g_lean_fix_config.left_neck_pitch_scale);
    g_lean_fix_config.left_head_pitch_scale = read_ini_float(
        ini_path, "lean_left_head_pitch_scale", g_lean_fix_config.left_head_pitch_scale);
    g_lean_fix_config.left_back_up_pitch_scale = read_ini_float(
        ini_path, "lean_left_back_up_pitch_scale", g_lean_fix_config.left_back_up_pitch_scale);
    g_lean_fix_config.left_back_mid_pitch_scale = read_ini_float(
        ini_path, "lean_left_back_mid_pitch_scale", g_lean_fix_config.left_back_mid_pitch_scale);
    g_lean_fix_config.left_back_low_pitch_scale = read_ini_float(
        ini_path, "lean_left_back_low_pitch_scale", g_lean_fix_config.left_back_low_pitch_scale);
    g_lean_fix_config.left_pelvis_pitch_scale = read_ini_float(
        ini_path, "lean_left_pelvis_pitch_scale", g_lean_fix_config.left_pelvis_pitch_scale);
    // Left-lean overrides (engine rig is asymmetric, left has different bugs).
    g_lean_fix_config.left_neck_yaw_scale = read_ini_float(
        ini_path, "lean_left_neck_yaw_scale", g_lean_fix_config.left_neck_yaw_scale);
    g_lean_fix_config.left_head_yaw_scale = read_ini_float(
        ini_path, "lean_left_head_yaw_scale", g_lean_fix_config.left_head_yaw_scale);
    g_lean_fix_config.left_back_up_yaw_scale = read_ini_float(
        ini_path, "lean_left_back_up_yaw_scale", g_lean_fix_config.left_back_up_yaw_scale);
    g_lean_fix_config.left_back_mid_yaw_scale = read_ini_float(
        ini_path, "lean_left_back_mid_yaw_scale", g_lean_fix_config.left_back_mid_yaw_scale);
    g_lean_fix_config.left_back_low_yaw_scale = read_ini_float(
        ini_path, "lean_left_back_low_yaw_scale", g_lean_fix_config.left_back_low_yaw_scale);
    g_lean_fix_config.left_pelvis_yaw_scale = read_ini_float(
        ini_path, "lean_left_pelvis_yaw_scale", g_lean_fix_config.left_pelvis_yaw_scale);
    g_lean_fix_config.left_neck_roll_scale = read_ini_float(
        ini_path, "lean_left_neck_roll_scale", g_lean_fix_config.left_neck_roll_scale);
    g_lean_fix_config.left_head_roll_scale = read_ini_float(
        ini_path, "lean_left_head_roll_scale", g_lean_fix_config.left_head_roll_scale);
    g_lean_fix_config.left_back_up_roll_scale = read_ini_float(
        ini_path, "lean_left_back_up_roll_scale", g_lean_fix_config.left_back_up_roll_scale);
    g_lean_fix_config.left_back_mid_roll_scale = read_ini_float(
        ini_path, "lean_left_back_mid_roll_scale", g_lean_fix_config.left_back_mid_roll_scale);
    g_lean_fix_config.left_back_low_roll_scale = read_ini_float(
        ini_path, "lean_left_back_low_roll_scale", g_lean_fix_config.left_back_low_roll_scale);
    g_lean_fix_config.left_pelvis_roll_scale = read_ini_float(
        ini_path, "lean_left_pelvis_roll_scale", g_lean_fix_config.left_pelvis_roll_scale);
    // Yaw offsets (additive degrees) - the REAL fix for "shift head left
    // during stationary lean" since engine writes ~0 yaws at peak lean.
    g_lean_fix_config.left_neck_yaw_offset = read_ini_float(
        ini_path, "lean_left_neck_yaw_offset", g_lean_fix_config.left_neck_yaw_offset);
    g_lean_fix_config.left_head_yaw_offset = read_ini_float(
        ini_path, "lean_left_head_yaw_offset", g_lean_fix_config.left_head_yaw_offset);
    g_lean_fix_config.left_back_up_yaw_offset = read_ini_float(
        ini_path, "lean_left_back_up_yaw_offset", g_lean_fix_config.left_back_up_yaw_offset);
    g_lean_fix_config.left_back_mid_yaw_offset = read_ini_float(
        ini_path, "lean_left_back_mid_yaw_offset", g_lean_fix_config.left_back_mid_yaw_offset);
    g_lean_fix_config.left_back_low_yaw_offset = read_ini_float(
        ini_path, "lean_left_back_low_yaw_offset", g_lean_fix_config.left_back_low_yaw_offset);
    g_lean_fix_config.left_pelvis_yaw_offset = read_ini_float(
        ini_path, "lean_left_pelvis_yaw_offset", g_lean_fix_config.left_pelvis_yaw_offset);
    g_lean_fix_config.right_neck_yaw_offset = read_ini_float(
        ini_path, "lean_right_neck_yaw_offset", g_lean_fix_config.right_neck_yaw_offset);
    g_lean_fix_config.right_head_yaw_offset = read_ini_float(
        ini_path, "lean_right_head_yaw_offset", g_lean_fix_config.right_head_yaw_offset);
    g_lean_fix_config.right_back_up_yaw_offset = read_ini_float(
        ini_path, "lean_right_back_up_yaw_offset", g_lean_fix_config.right_back_up_yaw_offset);
    g_lean_fix_config.right_back_mid_yaw_offset = read_ini_float(
        ini_path, "lean_right_back_mid_yaw_offset", g_lean_fix_config.right_back_mid_yaw_offset);
    g_lean_fix_config.right_back_low_yaw_offset = read_ini_float(
        ini_path, "lean_right_back_low_yaw_offset", g_lean_fix_config.right_back_low_yaw_offset);
    g_lean_fix_config.right_pelvis_yaw_offset = read_ini_float(
        ini_path, "lean_right_pelvis_yaw_offset", g_lean_fix_config.right_pelvis_yaw_offset);
    // Aimwalk fix (port cod2x animation.cpp:332-348)
    g_lean_fix_config.aimwalk_fix_enable = read_ini_bool(
        ini_path, "lean_aimwalk_fix_enable", g_lean_fix_config.aimwalk_fix_enable);
    g_lean_fix_config.aimwalk_stand_pitch = read_ini_float(
        ini_path, "lean_aimwalk_stand_pitch", g_lean_fix_config.aimwalk_stand_pitch);
    g_lean_fix_config.aimwalk_stand_roll = read_ini_float(
        ini_path, "lean_aimwalk_stand_roll", g_lean_fix_config.aimwalk_stand_roll);
    g_lean_fix_config.aimwalk_crouch_pitch = read_ini_float(
        ini_path, "lean_aimwalk_crouch_pitch", g_lean_fix_config.aimwalk_crouch_pitch);
    g_lean_fix_config.aimwalk_crouch_roll = read_ini_float(
        ini_path, "lean_aimwalk_crouch_roll", g_lean_fix_config.aimwalk_crouch_roll);
    g_lean_fix_config.aimwalk_stand_lift = read_ini_float(
        ini_path, "lean_aimwalk_stand_lift", g_lean_fix_config.aimwalk_stand_lift);
    g_lean_fix_config.aimwalk_crouch_lift = read_ini_float(
        ini_path, "lean_aimwalk_crouch_lift", g_lean_fix_config.aimwalk_crouch_lift);
    g_lean_fix_config.aimwalk_stand_yaw_scale = read_ini_float(
        ini_path, "lean_aimwalk_stand_yaw_scale", g_lean_fix_config.aimwalk_stand_yaw_scale);
    g_lean_fix_config.aimwalk_crouch_yaw_scale = read_ini_float(
        ini_path, "lean_aimwalk_crouch_yaw_scale", g_lean_fix_config.aimwalk_crouch_yaw_scale);
    g_lean_fix_config.aimwalk_smooth_rate = read_ini_float(
        ini_path, "lean_aimwalk_smooth_rate", g_lean_fix_config.aimwalk_smooth_rate);
    // Headclip fix (ported from CoD2x)
    g_lean_fix_config.headclip_fix_enable = read_ini_bool(
        ini_path, "lean_headclip_fix_enable", g_lean_fix_config.headclip_fix_enable);
    g_lean_fix_config.headclip_back_low_pitch = read_ini_float(
        ini_path, "lean_headclip_back_low_pitch", g_lean_fix_config.headclip_back_low_pitch);
    g_lean_fix_config.headclip_back_low_yaw = read_ini_float(
        ini_path, "lean_headclip_back_low_yaw", g_lean_fix_config.headclip_back_low_yaw);
    g_lean_fix_config.headclip_back_mid_pitch = read_ini_float(
        ini_path, "lean_headclip_back_mid_pitch", g_lean_fix_config.headclip_back_mid_pitch);
    g_lean_fix_config.headclip_back_up_pitch = read_ini_float(
        ini_path, "lean_headclip_back_up_pitch", g_lean_fix_config.headclip_back_up_pitch);
    // Stand variant
    g_lean_fix_config.headclip_apply_in_stand = read_ini_bool(
        ini_path, "lean_headclip_apply_in_stand", g_lean_fix_config.headclip_apply_in_stand);
    g_lean_fix_config.headclip_stand_back_low_pitch = read_ini_float(
        ini_path, "lean_headclip_stand_back_low_pitch", g_lean_fix_config.headclip_stand_back_low_pitch);
    g_lean_fix_config.headclip_stand_back_low_yaw = read_ini_float(
        ini_path, "lean_headclip_stand_back_low_yaw", g_lean_fix_config.headclip_stand_back_low_yaw);
    g_lean_fix_config.headclip_stand_back_mid_pitch = read_ini_float(
        ini_path, "lean_headclip_stand_back_mid_pitch", g_lean_fix_config.headclip_stand_back_mid_pitch);
    g_lean_fix_config.headclip_stand_back_up_pitch = read_ini_float(
        ini_path, "lean_headclip_stand_back_up_pitch", g_lean_fix_config.headclip_stand_back_up_pitch);
    // Body sideways shift (port cod2x animation.cpp:184-200)
    g_lean_fix_config.body_shift_enable = read_ini_bool(
        ini_path, "lean_body_shift_enable", g_lean_fix_config.body_shift_enable);
    g_lean_fix_config.body_shift_left_stand = read_ini_float(
        ini_path, "lean_body_shift_left_stand", g_lean_fix_config.body_shift_left_stand);
    g_lean_fix_config.body_shift_left_crouch = read_ini_float(
        ini_path, "lean_body_shift_left_crouch", g_lean_fix_config.body_shift_left_crouch);
    g_lean_fix_config.body_shift_right_stand = read_ini_float(
        ini_path, "lean_body_shift_right_stand", g_lean_fix_config.body_shift_right_stand);
    g_lean_fix_config.body_shift_right_crouch = read_ini_float(
        ini_path, "lean_body_shift_right_crouch", g_lean_fix_config.body_shift_right_crouch);
    // Crouch posture
    g_lean_fix_config.crouch_back_low_pitch_offset = read_ini_float(
        ini_path, "crouch_back_low_pitch", g_lean_fix_config.crouch_back_low_pitch_offset);
    g_lean_fix_config.crouch_back_mid_pitch_offset = read_ini_float(
        ini_path, "crouch_back_mid_pitch", g_lean_fix_config.crouch_back_mid_pitch_offset);
    g_lean_fix_config.crouch_back_up_pitch_offset = read_ini_float(
        ini_path, "crouch_back_up_pitch", g_lean_fix_config.crouch_back_up_pitch_offset);
    g_lean_fix_config.crouch_neck_pitch_offset = read_ini_float(
        ini_path, "crouch_neck_pitch", g_lean_fix_config.crouch_neck_pitch_offset);
    g_lean_fix_config.crouch_head_pitch_offset = read_ini_float(
        ini_path, "crouch_head_pitch", g_lean_fix_config.crouch_head_pitch_offset);
    g_lean_fix_config.crouch_origin_z_offset = read_ini_float(
        ini_path, "crouch_origin_z_offset", g_lean_fix_config.crouch_origin_z_offset);
    // v14 lean roll offsets (the real visible-lean fix)
    g_lean_fix_config.lean_back_low_roll_amount = read_ini_float(
        ini_path, "lean_back_low_roll_amount", g_lean_fix_config.lean_back_low_roll_amount);
    g_lean_fix_config.lean_back_mid_roll_amount = read_ini_float(
        ini_path, "lean_back_mid_roll_amount", g_lean_fix_config.lean_back_mid_roll_amount);
    g_lean_fix_config.lean_back_up_roll_amount = read_ini_float(
        ini_path, "lean_back_up_roll_amount", g_lean_fix_config.lean_back_up_roll_amount);
    g_lean_fix_config.lean_neck_counter_roll = read_ini_float(
        ini_path, "lean_neck_counter_roll", g_lean_fix_config.lean_neck_counter_roll);
    g_lean_fix_config.lean_head_counter_roll = read_ini_float(
        ini_path, "lean_head_counter_roll", g_lean_fix_config.lean_head_counter_roll);
    // v15 aimwalk (lean + moving = body bends forward like CoD2X)
    g_lean_fix_config.lean_aimwalk_v15_enable = read_ini_bool(
        ini_path, "lean_aimwalk_v15_enable", g_lean_fix_config.lean_aimwalk_v15_enable);
    g_lean_fix_config.lean_aimwalk_v15_speed_threshold = read_ini_float(
        ini_path, "lean_aimwalk_v15_speed_threshold", g_lean_fix_config.lean_aimwalk_v15_speed_threshold);
    g_lean_fix_config.lean_aimwalk_v15_pitch_back_low = read_ini_float(
        ini_path, "lean_aimwalk_v15_pitch_back_low", g_lean_fix_config.lean_aimwalk_v15_pitch_back_low);
    g_lean_fix_config.lean_aimwalk_v15_pitch_back_mid = read_ini_float(
        ini_path, "lean_aimwalk_v15_pitch_back_mid", g_lean_fix_config.lean_aimwalk_v15_pitch_back_mid);
    g_lean_fix_config.lean_aimwalk_v15_pitch_back_up = read_ini_float(
        ini_path, "lean_aimwalk_v15_pitch_back_up", g_lean_fix_config.lean_aimwalk_v15_pitch_back_up);
    // Swing fix (cod2x BG_PlayerAngles port - independent du lean fix)
    g_swing_fix_config.enable = read_ini_bool(
        ini_path, "swing_fix_enable", g_swing_fix_config.enable);
    g_swing_fix_config.legs_tolerance = read_ini_float(
        ini_path, "swing_legs_tolerance", g_swing_fix_config.legs_tolerance);
    g_swing_fix_config.torso_pitch_speed = read_ini_float(
        ini_path, "swing_torso_pitch_speed", g_swing_fix_config.torso_pitch_speed);
    // Lean amplify (5x AddLeanToPosition binary patch)
    g_lean_amplify_config.enable = read_ini_bool(
        ini_path, "lean_amplify_enable", g_lean_amplify_config.enable);
    g_lean_amplify_config.factor = read_ini_float(
        ini_path, "lean_amplify_factor", g_lean_amplify_config.factor);
    // Short version string
    {
        char buf[SHORT_VERSION_MAX_LEN + 1];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "short_version", "", buf, sizeof(buf), ini_path);
        if (n > 0 && n <= SHORT_VERSION_MAX_LEN) {
            strncpy(g_short_version_buffer, buf, SHORT_VERSION_MAX_LEN);
            g_short_version_buffer[SHORT_VERSION_MAX_LEN] = '\0';
        }
    }
    // CPU affinity
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_cpu_cores", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_cpu_affinity_config.cores_count = atoi(buf);
        n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_cpu_first_core", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_cpu_affinity_config.first_core = atoi(buf);
    }
    // Process & thread priority
    {
        char buf[32];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_process_priority", "", buf, sizeof(buf), ini_path);
        if (n > 0) {
            if      (!_stricmp(buf, "normal"))       g_process_priority_config.process_priority = ProcessPriorityLevel::Normal;
            else if (!_stricmp(buf, "above_normal")) g_process_priority_config.process_priority = ProcessPriorityLevel::AboveNormal;
            else if (!_stricmp(buf, "high"))         g_process_priority_config.process_priority = ProcessPriorityLevel::High;
            else if (!_stricmp(buf, "realtime"))     g_process_priority_config.process_priority = ProcessPriorityLevel::Realtime;
            else if (!_stricmp(buf, "default"))      g_process_priority_config.process_priority = ProcessPriorityLevel::Default;
        }
        n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_main_thread_priority", "", buf, sizeof(buf), ini_path);
        if (n > 0) {
            if      (!_stricmp(buf, "normal"))        g_process_priority_config.main_thread_priority = ThreadPriorityLevel::Normal;
            else if (!_stricmp(buf, "above_normal"))  g_process_priority_config.main_thread_priority = ThreadPriorityLevel::AboveNormal;
            else if (!_stricmp(buf, "highest"))       g_process_priority_config.main_thread_priority = ThreadPriorityLevel::Highest;
            else if (!_stricmp(buf, "time_critical")) g_process_priority_config.main_thread_priority = ThreadPriorityLevel::TimeCritical;
            else if (!_stricmp(buf, "default"))       g_process_priority_config.main_thread_priority = ThreadPriorityLevel::Default;
        }
    }
    g_process_priority_config.disable_power_throttling = read_ini_bool(
        ini_path, "smoothness_disable_power_throttling",
        g_process_priority_config.disable_power_throttling);
    // Working set
    g_working_set_config.enable = read_ini_bool(
        ini_path, "smoothness_lock_working_set", g_working_set_config.enable);
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_working_set_min_mb", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_working_set_config.min_mb = (size_t)atoi(buf);
        n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_working_set_max_mb", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_working_set_config.max_mb = (size_t)atoi(buf);
    }
    // FSO disable
    g_fso_disable_config.enable = read_ini_bool(
        ini_path, "smoothness_disable_fso", g_fso_disable_config.enable);

    // FPS cap (timer resolution)
    g_fps_cap_config.force_1ms_timer = read_ini_bool(
        ini_path, "force_1ms_timer", g_fps_cap_config.force_1ms_timer);
    // Demo upload
    g_demo_upload_config.enable = read_ini_bool(
        ini_path, "demo_upload_enable", g_demo_upload_config.enable);
    g_demo_upload_config.delete_after_upload = read_ini_bool(
        ini_path, "demo_upload_delete_after", g_demo_upload_config.delete_after_upload);
    g_demo_upload_config.show_toasts = read_ini_bool(
        ini_path, "demo_upload_show_toasts", g_demo_upload_config.show_toasts);
    GetPrivateProfileStringA(
        "cod1reloaded", "demo_upload_url", "",
        g_demo_upload_config.upload_url, sizeof(g_demo_upload_config.upload_url),
        ini_path);
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "demo_upload_poll_sec", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_demo_upload_config.poll_interval_sec = atoi(buf);
    }
    // Updater
    g_updater_config.enable = read_ini_bool(
        ini_path, "updater_enable", g_updater_config.enable);
    g_updater_config.auto_download = read_ini_bool(
        ini_path, "updater_auto_download", g_updater_config.auto_download);
    g_updater_config.show_dialog = read_ini_bool(
        ini_path, "updater_show_dialog", g_updater_config.show_dialog);
    GetPrivateProfileStringA(
        "cod1reloaded", "updater_manifest_url", "",
        g_updater_config.manifest_url, sizeof(g_updater_config.manifest_url),
        ini_path);
    // Frame limiter precis
    g_frame_limiter_config.enable = read_ini_bool(
        ini_path, "frame_limiter_enable", g_frame_limiter_config.enable);
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "frame_limiter_bias_us", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_frame_limiter_config.deadline_bias_us = atoi(buf);
    }
    // Fullscreen patch
    g_fullscreen_config.force_windowed_default = read_ini_bool(
        ini_path, "force_windowed_default", g_fullscreen_config.force_windowed_default);
    // Window patch
    g_window_config.borderless_enable = read_ini_bool(
        ini_path, "window_borderless", g_window_config.borderless_enable);
    g_window_config.follow_current_monitor = read_ini_bool(
        ini_path, "window_follow_current_monitor", g_window_config.follow_current_monitor);
    g_window_config.minimize_on_focus_loss = read_ini_bool(
        ini_path, "window_minimize_on_focus_loss", g_window_config.minimize_on_focus_loss);
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "window_monitor_index", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_window_config.preferred_monitor_index = atoi(buf);
    }
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "lean_diag_log_count", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_lean_fix_config.diag_log_count = atoi(buf);
    }

    // Widescreen fix
    g_widescreen_config.horplus_fov_enable = read_ini_bool(
        ini_path, "widescreen_horplus_fov", g_widescreen_config.horplus_fov_enable);
    g_widescreen_config.horplus_hook_caller2 = read_ini_bool(
        ini_path, "widescreen_horplus_hook_caller2", g_widescreen_config.horplus_hook_caller2);

    // Avatar overlay (HUD)
    g_avatar_overlay_config.enable = read_ini_bool(
        ini_path, "avatar_overlay_enable", g_avatar_overlay_config.enable);
    g_avatar_overlay_config.x = read_ini_int(
        ini_path, "avatar_overlay_x", g_avatar_overlay_config.x);
    g_avatar_overlay_config.y = read_ini_int(
        ini_path, "avatar_overlay_y", g_avatar_overlay_config.y);
    g_avatar_overlay_config.width = read_ini_int(
        ini_path, "avatar_overlay_width", g_avatar_overlay_config.width);
    g_avatar_overlay_config.height = read_ini_int(
        ini_path, "avatar_overlay_height", g_avatar_overlay_config.height);
    read_ini_string(ini_path, "avatar_overlay_test_url",
                    g_avatar_overlay_config.test_url,
                    sizeof(g_avatar_overlay_config.test_url),
                    g_avatar_overlay_config.test_url);
    g_widescreen_config.force_resolution = read_ini_bool(
        ini_path, "widescreen_force_resolution", g_widescreen_config.force_resolution);
    g_widescreen_config.width = read_ini_int(
        ini_path, "widescreen_width", g_widescreen_config.width);
    g_widescreen_config.height = read_ini_int(
        ini_path, "widescreen_height", g_widescreen_config.height);
    g_widescreen_config.refresh_hz = read_ini_int(
        ini_path, "widescreen_refresh_hz", g_widescreen_config.refresh_hz);
    g_widescreen_config.custom_ratio = read_ini_float(
        ini_path, "widescreen_custom_ratio", g_widescreen_config.custom_ratio);
    {
        char buf[16];
        read_ini_string(ini_path, "widescreen_aspect_mode", buf, sizeof(buf), "auto");
        if      (!_stricmp(buf, "auto"))   g_widescreen_config.aspect_mode = AspectMode::Auto;
        else if (!_stricmp(buf, "4:3"))    g_widescreen_config.aspect_mode = AspectMode::R_4_3;
        else if (!_stricmp(buf, "16:9"))   g_widescreen_config.aspect_mode = AspectMode::R_16_9;
        else if (!_stricmp(buf, "16:10"))  g_widescreen_config.aspect_mode = AspectMode::R_16_10;
        else if (!_stricmp(buf, "21:9"))   g_widescreen_config.aspect_mode = AspectMode::R_21_9;
        else if (!_stricmp(buf, "custom")) g_widescreen_config.aspect_mode = AspectMode::Custom;
    }

    // Discord Rich Presence
    g_discord_rpc_config.enable = read_ini_bool(
        ini_path, "discord_rpc_enable", g_discord_rpc_config.enable);
    g_discord_rpc_config.show_elapsed = read_ini_bool(
        ini_path, "discord_rpc_show_elapsed", g_discord_rpc_config.show_elapsed);
    read_ini_string(ini_path, "discord_rpc_client_id",
                    g_discord_rpc_config.client_id,
                    sizeof(g_discord_rpc_config.client_id),
                    g_discord_rpc_config.client_id);
    read_ini_string(ini_path, "discord_rpc_large_image",
                    g_discord_rpc_config.large_image,
                    sizeof(g_discord_rpc_config.large_image),
                    g_discord_rpc_config.large_image);
    read_ini_string(ini_path, "discord_rpc_large_text",
                    g_discord_rpc_config.large_text,
                    sizeof(g_discord_rpc_config.large_text),
                    g_discord_rpc_config.large_text);
    read_ini_string(ini_path, "discord_rpc_details_menu",
                    g_discord_rpc_config.details_menu,
                    sizeof(g_discord_rpc_config.details_menu),
                    g_discord_rpc_config.details_menu);
    read_ini_string(ini_path, "discord_rpc_details_match",
                    g_discord_rpc_config.details_match,
                    sizeof(g_discord_rpc_config.details_match),
                    g_discord_rpc_config.details_match);
    read_ini_string(ini_path, "discord_rpc_state",
                    g_discord_rpc_config.state_text,
                    sizeof(g_discord_rpc_config.state_text),
                    g_discord_rpc_config.state_text);

    logger::logf("config loaded from %s", ini_path);
    logger::logf("  viewheight_lerp_speed = %.2f", g_viewheight_config.viewheight_lerp_speed);
    logger::logf("  short_version         = \"%s\"", g_short_version_buffer);
    logger::logf("  updater_enable        = %s (url=\"%s\")",
                 g_updater_config.enable ? "true" : "false",
                 g_updater_config.manifest_url[0] ? g_updater_config.manifest_url : "(none)");
    logger::logf("  demo_upload_enable    = %s (url=\"%s\", poll=%ds)",
                 g_demo_upload_config.enable ? "true" : "false",
                 g_demo_upload_config.upload_url[0] ? g_demo_upload_config.upload_url : "(none)",
                 g_demo_upload_config.poll_interval_sec);
    logger::logf("  force_windowed_default= %s",
                 g_fullscreen_config.force_windowed_default ? "true" : "false");
    logger::logf("  window_borderless     = %s (follow_current=%d, mon_idx=%d)",
                 g_window_config.borderless_enable ? "true" : "false",
                 g_window_config.follow_current_monitor,
                 g_window_config.preferred_monitor_index);
    logger::logf("  lean_fix_enable       = %s",
                 g_lean_fix_config.enable ? "true" : "false");
    if (g_lean_fix_config.enable) {
        logger::logf("  yaw scales: neck=%.2f head=%.2f back_up=%.2f mid=%.2f low=%.2f (in_stand=%d)",
                     g_lean_fix_config.neck_yaw_scale,
                     g_lean_fix_config.head_yaw_scale,
                     g_lean_fix_config.back_up_yaw_scale,
                     g_lean_fix_config.back_mid_yaw_scale,
                     g_lean_fix_config.back_low_yaw_scale,
                     g_lean_fix_config.apply_in_stand);
        logger::logf("  aimwalk: enable=%d stand_pitch=%.2f stand_roll=%.2f crouch_pitch=%.2f crouch_roll=%.2f smooth_rate=%.2f",
                     g_lean_fix_config.aimwalk_fix_enable,
                     g_lean_fix_config.aimwalk_stand_pitch,
                     g_lean_fix_config.aimwalk_stand_roll,
                     g_lean_fix_config.aimwalk_crouch_pitch,
                     g_lean_fix_config.aimwalk_crouch_roll,
                     g_lean_fix_config.aimwalk_smooth_rate);
    }
    logger::logf("  swing_fix: enable=%d legs_tolerance=%.2f torso_pitch_speed=%.2f",
                 g_swing_fix_config.enable,
                 g_swing_fix_config.legs_tolerance,
                 g_swing_fix_config.torso_pitch_speed);
    logger::logf("  discord_rpc: enable=%s client_id=%s",
                 g_discord_rpc_config.enable ? "true" : "false",
                 g_discord_rpc_config.client_id[0] ? g_discord_rpc_config.client_id : "(none)");
}

}  // namespace patches
