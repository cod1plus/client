#include "core/patches.h"
#include "gameplay/viewheight_fix.h"
#include "gameplay/lean_fix.h"
#include "gameplay/swing_fix.h"
#include "core/logger.h"
#include "netcode/version_patch.h"
#include "netcode/protocol_patch.h"
#include "netcode/version_gate.h"
#include "video/window_patch.h"
#include "video/fullscreen_patch.h"
#include "performance/fps_cap.h"
#include "performance/frame_limiter.h"
#include "features/updater.h"
#include "features/demo_upload.h"
#include "performance/cpu_affinity.h"
#include "performance/process_priority.h"
#include "performance/working_set.h"
#include "performance/fso_disable.h"
#include "video/widescreen_fix.h"
#include "features/avatar_overlay.h"
#include "features/discord_rpc.h"
#include "netcode/antilag.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace patches {

namespace {

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

void read_ini_string(const char* path, const char* key,
                     char* out, size_t out_size, const char* fallback) {
    DWORD n = GetPrivateProfileStringA(
        "cod1reloaded", key, fallback, out, (DWORD)out_size, path);
    (void)n;
}

}  // namespace

static char g_ini_path_cache[MAX_PATH] = {0};

void hot_reload_lean_reshape() {
    if (!g_ini_path_cache[0]) return;
    const char* p = g_ini_path_cache;
    g_lean_fix_config.move_diag_fix = read_ini_int(
        p, "move_diag_fix", g_lean_fix_config.move_diag_fix);
    g_lean_fix_config.move_diag_parent = read_ini_int(
        p, "move_diag_parent", g_lean_fix_config.move_diag_parent);
    g_lean_fix_config.diag_k_pos = read_ini_float(
        p, "diag_k_pos", g_lean_fix_config.diag_k_pos);
    g_lean_fix_config.diag_k_neg = read_ini_float(
        p, "diag_k_neg", g_lean_fix_config.diag_k_neg);
    g_lean_fix_config.ctrl_smooth_enable = read_ini_bool(
        p, "ctrl_smooth_enable", g_lean_fix_config.ctrl_smooth_enable);
    g_lean_fix_config.ctrl_smooth_time = read_ini_int(
        p, "ctrl_smooth_time", g_lean_fix_config.ctrl_smooth_time);
    g_lean_fix_config.lean_diag_scale = read_ini_float(
        p, "lean_diag_scale", g_lean_fix_config.lean_diag_scale);
    g_lean_fix_config.body_shift_lean_scale = read_ini_float(
        p, "body_shift_lean_scale", g_lean_fix_config.body_shift_lean_scale);
    g_lean_fix_config.body_yaw_lock = read_ini_float(
        p, "body_yaw_lock", g_lean_fix_config.body_yaw_lock);
    // read live by redirected mov
    g_torso_yaw_speed_live = read_ini_float(
        p, "swing_torso_yaw_speed", g_torso_yaw_speed_live);
    g_torso_yaw_movefrac_live = read_ini_float(
        p, "swing_torso_yaw_movefrac", g_torso_yaw_movefrac_live);
}

void load_config(HMODULE self_module) {
    char ini_path[MAX_PATH];
    if (!get_config_path(self_module, ini_path, sizeof(ini_path))) return;
    if (GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES) return;
    strncpy(g_ini_path_cache, ini_path, sizeof(g_ini_path_cache) - 1);
    g_ini_path_cache[sizeof(g_ini_path_cache) - 1] = '\0';

    g_viewheight_config.viewheight_lerp_speed = read_ini_float(
        ini_path, "viewheight_lerp_speed", VIEWHEIGHT_LERP_SPEED_FALLBACK);

    g_lean_fix_config.enable = read_ini_bool(
        ini_path, "lean_fix_enable", g_lean_fix_config.enable);
    g_protocol_version = read_ini_int(
        ini_path, "protocol_version", g_protocol_version);
    g_net_version = read_ini_int(
        ini_path, "net_version", g_net_version);
    g_version_gate_config.enable = read_ini_bool(
        ini_path, "version_gate_enable", g_version_gate_config.enable);
    g_version_gate_config.allow_unversioned = read_ini_bool(
        ini_path, "version_gate_allow_unversioned", g_version_gate_config.allow_unversioned);
    {
        char buf[CODMP_MASTER_HOST_MAX + 1];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "master_server", g_master_host, buf, sizeof(buf), ini_path);
        if (n > 0) { strncpy(g_master_host, buf, CODMP_MASTER_HOST_MAX); g_master_host[CODMP_MASTER_HOST_MAX] = 0; }
    }
    g_lean_fix_config.apply_in_stand = read_ini_bool(
        ini_path, "lean_fix_apply_in_stand", g_lean_fix_config.apply_in_stand);
    g_lean_fix_config.move_diag_fix = read_ini_int(
        ini_path, "move_diag_fix", g_lean_fix_config.move_diag_fix);
    g_lean_fix_config.move_diag_parent = read_ini_int(
        ini_path, "move_diag_parent", g_lean_fix_config.move_diag_parent);
    g_lean_fix_config.diag_k_pos = read_ini_float(
        ini_path, "diag_k_pos", g_lean_fix_config.diag_k_pos);
    g_lean_fix_config.diag_k_neg = read_ini_float(
        ini_path, "diag_k_neg", g_lean_fix_config.diag_k_neg);
    g_lean_fix_config.ctrl_smooth_enable = read_ini_bool(
        ini_path, "ctrl_smooth_enable", g_lean_fix_config.ctrl_smooth_enable);
    g_lean_fix_config.ctrl_smooth_time = read_ini_int(
        ini_path, "ctrl_smooth_time", g_lean_fix_config.ctrl_smooth_time);
    g_lean_fix_config.lean_diag_scale = read_ini_float(
        ini_path, "lean_diag_scale", g_lean_fix_config.lean_diag_scale);
    g_lean_fix_config.body_shift_lean_scale = read_ini_float(
        ini_path, "body_shift_lean_scale", g_lean_fix_config.body_shift_lean_scale);
    g_lean_fix_config.body_yaw_lock = read_ini_float(
        ini_path, "body_yaw_lock", g_lean_fix_config.body_yaw_lock);
    g_swing_fix_config.enable = read_ini_bool(
        ini_path, "swing_fix_enable", g_swing_fix_config.enable);
    g_swing_fix_config.legs_tolerance = read_ini_float(
        ini_path, "swing_legs_tolerance", g_swing_fix_config.legs_tolerance);
    g_swing_fix_config.torso_pitch_speed = read_ini_float(
        ini_path, "swing_torso_pitch_speed", g_swing_fix_config.torso_pitch_speed);
    g_torso_yaw_speed_live = read_ini_float(
        ini_path, "swing_torso_yaw_speed", g_torso_yaw_speed_live);
    g_torso_yaw_movefrac_live = read_ini_float(
        ini_path, "swing_torso_yaw_movefrac", g_torso_yaw_movefrac_live);
    {
        char buf[SHORT_VERSION_MAX_LEN + 1];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "short_version", "", buf, sizeof(buf), ini_path);
        if (n > 0 && n <= SHORT_VERSION_MAX_LEN) {
            strncpy(g_short_version_buffer, buf, SHORT_VERSION_MAX_LEN);
            g_short_version_buffer[SHORT_VERSION_MAX_LEN] = '\0';
        }
    }
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_cpu_cores", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_cpu_affinity_config.cores_count = atoi(buf);
        n = GetPrivateProfileStringA(
            "cod1reloaded", "smoothness_cpu_first_core", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_cpu_affinity_config.first_core = atoi(buf);
    }
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
    g_fso_disable_config.enable = read_ini_bool(
        ini_path, "smoothness_disable_fso", g_fso_disable_config.enable);

    g_fps_cap_config.force_1ms_timer = read_ini_bool(
        ini_path, "force_1ms_timer", g_fps_cap_config.force_1ms_timer);
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
    g_frame_limiter_config.enable = read_ini_bool(
        ini_path, "frame_limiter_enable", g_frame_limiter_config.enable);
    {
        char buf[16];
        DWORD n = GetPrivateProfileStringA(
            "cod1reloaded", "frame_limiter_bias_us", "", buf, sizeof(buf), ini_path);
        if (n > 0) g_frame_limiter_config.deadline_bias_us = atoi(buf);
    }
    g_fullscreen_config.force_windowed_default = read_ini_bool(
        ini_path, "force_windowed_default", g_fullscreen_config.force_windowed_default);
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

    g_widescreen_config.horplus_fov_enable = read_ini_bool(
        ini_path, "widescreen_horplus_fov", g_widescreen_config.horplus_fov_enable);
    g_widescreen_config.horplus_hook_caller2 = read_ini_bool(
        ini_path, "widescreen_horplus_hook_caller2", g_widescreen_config.horplus_hook_caller2);

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

    g_antilag_config.diag_enable = read_ini_bool(
        ini_path, "antilag_diag_enable", g_antilag_config.diag_enable);
    g_antilag_config.diag_log_count = read_ini_int(
        ini_path, "antilag_diag_log_count", g_antilag_config.diag_log_count);
    g_antilag_config.fire_hook_enable = read_ini_bool(
        ini_path, "antilag_fire_hook_enable", g_antilag_config.fire_hook_enable);
    g_antilag_config.capture_enable = read_ini_bool(
        ini_path, "antilag_capture_enable", g_antilag_config.capture_enable);
    g_antilag_config.rewind_enable = read_ini_bool(
        ini_path, "antilag_rewind_enable", g_antilag_config.rewind_enable);
    g_antilag_config.rewind_test_z = read_ini_int(
        ini_path, "antilag_rewind_test_z", g_antilag_config.rewind_test_z);
    g_antilag_config.rewind_test_self = read_ini_bool(
        ini_path, "antilag_rewind_test_self", g_antilag_config.rewind_test_self);

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
        logger::logf("  lean: diag_fix=%d parent=%d diag_k=%.2f/%.2f in_stand=%d",
                     g_lean_fix_config.move_diag_fix,
                     g_lean_fix_config.move_diag_parent,
                     g_lean_fix_config.diag_k_pos,
                     g_lean_fix_config.diag_k_neg,
                     g_lean_fix_config.apply_in_stand);
        logger::logf("  lean: lean_diag=%.2f body_shift=%.2f body_yaw_lock=%.2f ctrl_smooth=%d/%dms",
                     g_lean_fix_config.lean_diag_scale,
                     g_lean_fix_config.body_shift_lean_scale,
                     g_lean_fix_config.body_yaw_lock,
                     g_lean_fix_config.ctrl_smooth_enable,
                     g_lean_fix_config.ctrl_smooth_time);
    }
    logger::logf("  swing_fix: enable=%d legs_tolerance=%.2f torso_pitch_speed=%.2f",
                 g_swing_fix_config.enable,
                 g_swing_fix_config.legs_tolerance,
                 g_swing_fix_config.torso_pitch_speed);
    logger::logf("  discord_rpc: enable=%s client_id=%s",
                 g_discord_rpc_config.enable ? "true" : "false",
                 g_discord_rpc_config.client_id[0] ? g_discord_rpc_config.client_id : "(none)");
    logger::logf("  antilag: diag=%s log_count=%d fire_hook=%s capture=%s rewind=%s (test_z=%d test_self=%s)",
                 g_antilag_config.diag_enable ? "true" : "false",
                 g_antilag_config.diag_log_count,
                 g_antilag_config.fire_hook_enable ? "true" : "false",
                 g_antilag_config.capture_enable ? "true" : "false",
                 g_antilag_config.rewind_enable ? "true" : "false",
                 g_antilag_config.rewind_test_z,
                 g_antilag_config.rewind_test_self ? "true" : "false");
}

}  // namespace patches
