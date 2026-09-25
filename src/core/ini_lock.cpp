// ini_lock.cpp - see ini_lock.h
#include "core/ini_lock.h"
#include "core/logger.h"
#include "performance/frame_limiter.h"
#include "performance/gpu_sync.h"
#include "performance/fps_cap.h"
#include "performance/cpu_affinity.h"
#include "video/fullscreen_patch.h"
#include "features/settings_menu.h"   // g_settings_menu_config, CODMP_CVAR_FINDVAR_VA
#include "netcode/protocol_patch.h"   // CODMP_CVAR_COUNT_VA
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA, COMP_CVAR_*

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace patches {

namespace {

struct LockedKey {
    const char* key;
    const char* value;
    const char* why;     // one line, written above an appended key
};

// ASCII only: the .ini is read by GetPrivateProfileString and shown in Notepad
const LockedKey kLocked[] = {
    { "frame_limiter_enable", "on",   "exact frame cadence (the mod's limiter)" },
    { "force_1ms_timer",      "on",   "the limiter sleeps in 1 ms steps" },
    { "gpu_sync",             "auto", "one frame in flight; switches itself off if the GPU sets the fps" },
    { "input_late_sampling",  "on",   "mouse and keys read after the frame wait, not before" },
    { "fullscreen",           "on",   "exclusive fullscreen: no desktop compositor between the game and the screen" },
    { "refresh_rate",         "max",  "the highest Hz the display lists" },
    { "smoothness_cpu_cores", "0",    "no CPU affinity: the scheduler places the game's threads" },
};

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_Set_t)(const char*, const char*);
constexpr int CV_STRING = 0x04, CV_LATCHED = 0x0c, CV_FLAGS = 0x10, CV_INT = 0x20;

bool engine_ready() {
    return (uintptr_t)GetModuleHandleA(NULL) == 0x400000 && *(volatile int*)CODMP_CVAR_COUNT_VA > 0;
}

bool ieq(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "key = value": the key of a setting line, "" for comments / blanks / sections
std::string line_key(const std::string& line, size_t* eq_pos) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return "";
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return "";
    *eq_pos = eq;
    return trim(line.substr(0, eq));
}

// Rewrites `path` so every locked key holds its value. Returns the changes made
// ("key: old -> new"); an empty list = the file was already right (not rewritten).
bool rewrite_ini(const char* path, std::vector<std::string>& changes) {
    changes.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    const std::string nl = data.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    // split, keeping each line without its ending
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) { lines.push_back(data.substr(pos)); break; }
        std::string l = data.substr(pos, e - pos);
        if (!l.empty() && l[l.size() - 1] == '\r') l.resize(l.size() - 1);
        lines.push_back(l);
        pos = e + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();   // trailing newline

    bool changed = false;
    std::vector<const LockedKey*> missing;
    for (const LockedKey& lk : kLocked) {
        bool found = false;
        for (std::string& line : lines) {
            size_t eq = 0;
            if (!ieq(line_key(line, &eq), lk.key)) continue;
            found = true;
            const std::string cur = trim(line.substr(eq + 1));
            if (!ieq(cur, lk.value)) {
                changes.push_back(std::string(lk.key) + ": " + (cur.empty() ? "(vide)" : cur) + " -> " + lk.value);
                line = line.substr(0, eq + 1) + " " + lk.value;
                changed = true;
            }
            break;                         // the first setting line is the one GetPrivateProfileString reads
        }
        if (!found) missing.push_back(&lk);
    }
    if (!missing.empty()) {
        lines.push_back("");
        lines.push_back("; ----- IMPOSED BY THE MOD (rewritten at every launch, editing changes nothing) ---");
        for (const LockedKey* lk : missing) {
            lines.push_back(std::string(";  ") + lk->key + ": " + lk->why);
            lines.push_back(std::string(lk->key) + " = " + lk->value);
            changes.push_back(std::string(lk->key) + ": (absent) -> " + lk->value);
        }
        changed = true;
    }
    if (!changed) return true;

    std::string out;
    for (const std::string& l : lines) { out += l; out += nl; }
    // atomic swap: a crash mid-write must not leave a half .ini
    std::string tmp = std::string(path) + ".tmp";
    FILE* w = fopen(tmp.c_str(), "wb");
    if (!w) return false;
    const bool ok = fwrite(out.data(), 1, out.size(), w) == out.size();
    fclose(w);
    if (!ok || !MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// the values the mod runs with, whatever the file says
void force_values() {
    g_frame_limiter_config.enable     = true;
    g_frame_limiter_config.late_input = true;
    g_fps_cap_config.force_1ms_timer  = true;
    g_gpu_sync_config.mode            = GPU_SYNC_AUTO;
    g_fullscreen_config.force_windowed_default = false;   // fullscreen = on
    g_fullscreen_config.ini_key_present        = true;    // and repair a stale r_fullscreen 0
    snprintf(g_settings_menu_config.refresh_rate, sizeof(g_settings_menu_config.refresh_rate), "max");
    g_cpu_affinity_config.cores_count = 0;
}

// the highest Hz the display lists (any resolution): what refresh_rate = max resolves to
int max_listed_hz() {
    static int s_max = -1;
    if (s_max >= 0) return s_max;
    int best = 0;
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i)
        if (dm.dmBitsPerPel >= 32 && (int)dm.dmDisplayFrequency > best) best = (int)dm.dmDisplayFrequency;
    s_max = best;
    return best;
}

}  // namespace

void ini_lock_apply(const char* ini_path) {
    std::vector<std::string> changes;
    const bool ok = rewrite_ini(ini_path, changes);
    force_values();
    if (!ok) {
        logger::logf("ini_lock: cod1reloaded.ini could not be rewritten (err=%lu) - locked values used anyway",
                     GetLastError());
        return;
    }
    if (changes.empty()) {
        logger::logf("ini_lock: %d locked key(s) already in line", (int)(sizeof(kLocked) / sizeof(kLocked[0])));
        return;
    }
    std::string all;
    for (size_t i = 0; i < changes.size(); ++i) { if (i) all += ", "; all += changes[i]; }
    logger::logf("ini_lock: cod1reloaded.ini brought in line (%d change(s)): %s", (int)changes.size(), all.c_str());
}

void ini_lock_tick() {
    if (!engine_ready()) return;
    static DWORD s_last = 0;
    const DWORD now = GetTickCount();
    if (s_last && now - s_last < 1000) return;
    s_last = now;

    const Cvar_FindVar_t find = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
    const Cvar_Set_t     set  = (Cvar_Set_t)CODMP_CVAR_SET_VA;      // force=1: passes our own ROM

    // r_fullscreen: 1, then read-only (the 1.6X Display tab and the console get "read only")
    static bool s_fs_locked = false;
    if (!s_fs_locked) {
        void* cv = find("r_fullscreen");
        if (cv) {
            if (*(int*)((char*)cv + CV_INT) != 1) set("r_fullscreen", "1");
            *(uint32_t*)((char*)cv + CV_FLAGS) |= COMP_CVAR_ROM;
            s_fs_locked = true;
            logger::logf("ini_lock: r_fullscreen 1, read-only");
        }
    }

    // r_displayRefresh: the display's maximum, re-asserted (latched: takes effect at the
    // next vid_restart; a refused mode falls back to the best listed one in mode_guard)
    const int want = max_listed_hz();
    if (want > 1) {
        void* cv = find("r_displayRefresh");
        if (cv) {
            const int cur = *(int*)((char*)cv + CV_INT);
            const char* latched = *(const char**)((char*)cv + CV_LATCHED);
            const bool pending_ok = latched && atoi(latched) == want;
            if (cur != want && !pending_ok) {
                char v[16];
                snprintf(v, sizeof(v), "%d", want);
                set("r_displayRefresh", v);
                static int s_logged = 0;
                if (s_logged++ < 3) logger::logf("ini_lock: r_displayRefresh %d -> %d (display maximum)", cur, want);
            }
        }
    }
}

}  // namespace patches
