// test_ini_lock.cpp - the .ini rewrite of core/ini_lock.cpp against the real 1.6.8 shipped
// file: the locked keys land on their values, everything else stays byte for byte, a
// second pass changes nothing, an edited key is put back, CRLF survives.
//   g++ -std=c++17 -O2 -I src tools/test_ini_lock.cpp src/core/ini_lock.cpp src/core/logger.cpp
//       -o build/test_ini_lock && build/test_ini_lock <old-shipped.ini>
#include "core/ini_lock.h"
#include "performance/frame_limiter.h"
#include "performance/gpu_sync.h"
#include "performance/fps_cap.h"
#include "performance/cpu_affinity.h"
#include "video/fullscreen_patch.h"
#include "features/settings_menu.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// the config globals the lock writes into (their modules are not linked here)
namespace patches {
FrameLimiterConfig  g_frame_limiter_config = { false, 0, false };
GpuSyncConfig       g_gpu_sync_config = { GPU_SYNC_OFF };
FpsCapConfig        g_fps_cap_config = { false };
CpuAffinityConfig   g_cpu_affinity_config = { 4, 0 };
FullscreenConfig    g_fullscreen_config = { true, false };
SettingsMenuConfig  g_settings_menu_config;
}  // namespace patches

using namespace patches;

static std::string slurp(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return "";
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static void spit(const char* p, const std::string& s) { FILE* f = fopen(p, "wb"); fwrite(s.data(), 1, s.size(), f); fclose(f); }
static std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> v; size_t p = 0;
    while (p < s.size()) { size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size(); v.push_back(s.substr(p, e - p)); p = e + 1; }
    return v;
}
static std::string key_of(const std::string& l) {
    size_t eq = l.find('='); if (eq == std::string::npos) return "";
    std::string k = l.substr(0, eq); size_t a = k.find_first_not_of(" \t"), b = k.find_last_not_of(" \t");
    if (a == std::string::npos || k[a] == ';' || k[a] == '[') return "";
    return k.substr(a, b - a + 1);
}
static std::string value_of(const std::string& l) {
    size_t eq = l.find('='); std::string v = l.substr(eq + 1); size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : v.substr(a, b - a + 1);
}
static int g_failed = 0;
static void check(bool ok, const char* what) { printf("  [%s] %s\n", ok ? "OK" : "FAIL", what); if (!ok) ++g_failed; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_ini_lock <old-shipped.ini>\n"); return 2; }
    const std::string original = slurp(argv[1]);
    if (original.empty()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    const char* tmp = "build/test_ini_lock.ini";
    spit(tmp, original);

    const char* locked[][2] = {
        { "frame_limiter_enable", "on" }, { "force_1ms_timer", "on" }, { "gpu_sync", "auto" },
        { "input_late_sampling", "on" }, { "fullscreen", "on" }, { "refresh_rate", "max" },
        { "smoothness_cpu_cores", "0" },
    };

    // 1. first pass over the 1.6.8 file; the file's own line ending survives
    ini_lock_apply(tmp);
    std::string after = slurp(tmp);
    {
        const bool in_crlf = original.find("\r\n") != std::string::npos;
        const bool out_crlf = after.find("\r\n") != std::string::npos;
        check(in_crlf == out_crlf, in_crlf ? "CRLF kept" : "LF kept");
        // and the other ending too: a CRLF copy of the same file
        std::string crlf;
        for (char c : original) { if (c == '\n') crlf += '\r'; crlf += c; }
        spit("build/test_ini_lock_crlf.ini", crlf);
        ini_lock_apply("build/test_ini_lock_crlf.ini");
        const std::string a2 = slurp("build/test_ini_lock_crlf.ini");
        check(a2.find("\r\n") != std::string::npos && a2.find("gpu_sync = auto\r\n") != std::string::npos,
              "CRLF file rewritten with CRLF");
    }
    for (auto& lk : locked) {
        bool ok = false;
        for (const std::string& l : lines_of(after)) if (key_of(l) == lk[0]) { ok = value_of(l) == lk[1]; break; }
        char what[96]; snprintf(what, sizeof(what), "%s = %s", lk[0], lk[1]);
        check(ok, what);
    }
    // every non-locked line of the original is still there, in order, byte for byte
    {
        std::vector<std::string> a = lines_of(original), b = lines_of(after);
        size_t j = 0; bool same = true; int kept = 0;
        for (const std::string& l : a) {
            bool is_locked = false;
            for (auto& lk : locked) if (key_of(l) == lk[0]) is_locked = true;
            if (is_locked) { ++j; continue; }
            if (j >= b.size() || b[j] != l) { same = false; printf("    differs at: %s\n", l.c_str()); break; }
            ++j; ++kept;
        }
        char what[96]; snprintf(what, sizeof(what), "%d untouched lines identical and in order", kept);
        check(same, what);
        check(after.find("IMPOSED BY THE MOD") != std::string::npos, "missing keys appended under their own header");
    }
    // 2. second pass: nothing changes
    ini_lock_apply(tmp);
    check(slurp(tmp) == after, "second pass leaves the file untouched");
    // 3. the player edits two keys and deletes one: put back, in place / re-appended
    {
        std::string edited = after;
        size_t p = edited.find("fullscreen               = on"); if (p != std::string::npos) edited.replace(p, strlen("fullscreen               = on"), "fullscreen               = off");
        p = edited.find("gpu_sync = auto"); if (p != std::string::npos) edited.replace(p, strlen("gpu_sync = auto"), "gpu_sync = off");
        spit(tmp, edited);
        ini_lock_apply(tmp);
        const std::string fixed = slurp(tmp);
        check(fixed == after, "edited keys put back, file identical to the first pass");
    }
    // 4. the forced runtime values
    check(g_frame_limiter_config.enable && g_frame_limiter_config.late_input && g_fps_cap_config.force_1ms_timer &&
          g_gpu_sync_config.mode == GPU_SYNC_AUTO && !g_fullscreen_config.force_windowed_default &&
          g_fullscreen_config.ini_key_present && !strcmp(g_settings_menu_config.refresh_rate, "max") &&
          g_cpu_affinity_config.cores_count == 0, "runtime values forced");
    printf("%s\n", g_failed ? "FAILED" : "ALL OK");
    return g_failed ? 1 : 0;
}
