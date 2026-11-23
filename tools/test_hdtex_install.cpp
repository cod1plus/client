// test_hdtex_install.cpp - host test of the second package (HD textures -> main\) on the
// shared downloader, against a local directory server. Run by tools/test_hdtex.sh.
//   g++ -std=c++17 -I src tools/test_hdtex_install.cpp src/features/pam_install.cpp src/core/logger.cpp
//       -lwininet -ladvapi32 -o build/test_hdtex_install && build/test_hdtex_install
// The "game dir" is the exe's folder (build/), so files land in build/main_test/.
#include "features/pam_install.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace patches;

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static long long fsize(const char* p) {
    WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static PamStatus run_job(PkgId id, int timeout_ms) {
    pkg_install_start(id);
    PamStatus s;
    for (int t = 0; t < timeout_ms; t += 50) {
        Sleep(50);
        pkg_install_status(id, &s);
        if (s.state == PAM_DONE || s.state == PAM_ERROR || s.state == PAM_RESTART) break;
    }
    printf("   -> [%d] state %d: %s (%d/%d)\n", (int)id, s.state, s.text, s.files_done, s.files_total);
    return s;
}

int main() {
    logger::init(NULL);

    // 1. no link configured: the button's job reports it, touches nothing
    g_hdtex_install_config.manifest_url[0] = 0;
    PamStatus s = run_job(PKG_HDTEX, 2000);
    CHECK(s.state == PAM_ERROR && strstr(s.text, "no download link") != nullptr);

    // 2. fresh install into the manifest's folder ("main_test" here), cvar line handed out once.
    //    zz_hd_a.pk3 starts as a truncated .part (an interrupted earlier run): the server
    //    honours Range, so it must be RESUMED, not restarted (log: "resuming ... at 0.1 MB")
    CreateDirectoryA("build\\main_test", NULL);
    {
        FILE* src = fopen("build\\hdtex_test\\srv\\zz_hd_a.pk3", "rb");
        FILE* dst = fopen("build\\main_test\\zz_hd_a.pk3.part", "wb");
        CHECK(src && dst);
        static char b[100000]; size_t k = fread(b, 1, sizeof(b), src); fwrite(b, 1, k, dst);
        fclose(src); fclose(dst);
    }
    snprintf(g_hdtex_install_config.manifest_url, sizeof(g_hdtex_install_config.manifest_url),
             "http://127.0.0.1:8767/hdtex.manifest");
    s = run_job(PKG_HDTEX, 60000);
    CHECK(s.state == PAM_DONE);
    CHECK(!strcmp(s.mod, "main_test"));
    CHECK(s.files_total == 2 && s.files_done == 2);
    CHECK(strstr(s.text, "HD textures ready") != nullptr);
    CHECK(exists("build\\main_test\\zz_hd_a.pk3") && exists("build\\main_test\\zz_hd_b.pk3"));
    CHECK(fsize("build\\main_test\\zz_hd_a.pk3") == fsize("build\\hdtex_test\\srv\\zz_hd_a.pk3"));
    PkgCvar cv[PKG_MAX_CVARS];
    int n = pkg_install_take_cvars(PKG_HDTEX, cv, PKG_MAX_CVARS);
    CHECK(n == 1 && !strcmp(cv[0].name, "com_hunkMegs") && !strcmp(cv[0].value, "512"));
    CHECK(pkg_install_take_cvars(PKG_HDTEX, cv, PKG_MAX_CVARS) == 0);     // handed out once

    // 3. the PAM job is independent: its status is untouched by the HD job
    PamStatus ps; pkg_install_status(PKG_PAM, &ps);
    CHECK(ps.state == PAM_IDLE);

    // 4. second run: up to date
    s = run_job(PKG_HDTEX, 60000);
    CHECK(s.state == PAM_DONE && s.files_total == 0 && strstr(s.text, "up to date") != nullptr);

    // 5. same interrupted .part against a server WITHOUT Range support (port 8768: 200 for
    //    everything): started over, still correct
    {
        DeleteFileA("build\\main_test\\zz_hd_b.pk3");
        FILE* src = fopen("build\\hdtex_test\\srv\\zz_hd_b.pk3", "rb");
        FILE* dst = fopen("build\\main_test\\zz_hd_b.pk3.part", "wb");
        CHECK(src && dst);
        static char b[50000]; size_t k = fread(b, 1, sizeof(b), src); fwrite(b, 1, k, dst);
        fclose(src); fclose(dst);
        snprintf(g_hdtex_install_config.manifest_url, sizeof(g_hdtex_install_config.manifest_url),
                 "http://127.0.0.1:8768/hdtex_norange.manifest");
        s = run_job(PKG_HDTEX, 60000);
        CHECK(s.state == PAM_DONE && s.files_total == 1 && s.files_done == 1);
        CHECK(fsize("build\\main_test\\zz_hd_b.pk3") == fsize("build\\hdtex_test\\srv\\zz_hd_b.pk3"));
    }

    // 6. a manifest whose cvar line carries junk (quotes / semicolons) is ignored, files still fine
    snprintf(g_hdtex_install_config.manifest_url, sizeof(g_hdtex_install_config.manifest_url),
             "http://127.0.0.1:8767/hdtex_badcvar.manifest");
    s = run_job(PKG_HDTEX, 60000);
    CHECK(s.state == PAM_DONE);
    CHECK(pkg_install_take_cvars(PKG_HDTEX, cv, PKG_MAX_CVARS) == 0);

    printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
