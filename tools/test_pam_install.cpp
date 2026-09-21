// test_pam_install.cpp - host test of the PAM downloader against a local directory server.
//   python ../cod1pluspam/tools/make_manifest.py build/pam_test/srv --mod __pamtest --url http://127.0.0.1:8766/ -o build/pam_test/srv/pam.manifest
//   python -m http.server 8766 --directory build/pam_test/srv --bind 127.0.0.1 &
//   g++ -std=c++17 -I src tools/test_pam_install.cpp src/features/pam_install.cpp src/core/logger.cpp
//       -lwininet -ladvapi32 -o build/test_pam_install && build/test_pam_install
// The "game dir" is the exe's folder (build/), so files land in build/<mod>/.
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

static PamStatus run_job(int timeout_ms) {
    pam_install_start();
    PamStatus s;
    for (int t = 0; t < timeout_ms; t += 50) {
        Sleep(50);
        pam_install_status(&s);
        if (s.state == PAM_DONE || s.state == PAM_ERROR || s.state == PAM_RESTART) break;
    }
    printf("   -> state %d: %s (%d/%d)\n", s.state, s.text, s.files_done, s.files_total);
    return s;
}

int main(int argc, char** argv) {
    const char* mod = argc > 1 ? argv[1] : "__pamtest";
    logger::init(NULL);
    snprintf(g_pam_install_config.manifest_url, sizeof(g_pam_install_config.manifest_url),
             "http://127.0.0.1:8766/pam.manifest");

    char moddir[MAX_PATH]; snprintf(moddir, sizeof(moddir), "build\\%s\\", mod);

    // 1. fresh install: every file fetched, sizes match
    PamStatus s = run_job(60000);
    CHECK(s.state == PAM_DONE);
    CHECK(!strcmp(s.mod, mod));
    CHECK(s.files_total >= 2 && s.files_done == s.files_total);
    char p1[MAX_PATH]; snprintf(p1, sizeof(p1), "%szzzzz_kvcodPAM_nolib_v2_REV19.pk3", moddir);
    char p2[MAX_PATH]; snprintf(p2, sizeof(p2), "%szzzzz_rPAM_kvcodPAMext_v29_REV20net.pk3", moddir);
    CHECK(exists(p1) && exists(p2));
    CHECK(fsize(p1) == fsize("build\\pam_test\\srv\\zzzzz_kvcodPAM_nolib_v2_REV19.pk3"));

    // 2. second run: nothing to fetch
    s = run_job(60000);
    CHECK(s.state == PAM_DONE && s.files_total == 0);
    CHECK(strstr(s.text, "up to date") != nullptr);

    // 3. a corrupted local copy is re-fetched
    { FILE* f = fopen(p1, "ab"); if (f) { fputs("junk", f); fclose(f); } }
    s = run_job(60000);
    CHECK(s.state == PAM_DONE && s.files_total == 1 && s.files_done == 1);
    CHECK(fsize(p1) == fsize("build\\pam_test\\srv\\zzzzz_kvcodPAM_nolib_v2_REV19.pk3"));

    // 4. a locked pk3 (the engine holds it open) is staged as .new, then swapped at "launch"
    {
        HANDLE h = CreateFileA(p1, GENERIC_READ, 0 /* no sharing: like the engine's open pak */, NULL,
                               OPEN_EXISTING, 0, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        // make the server copy differ so the file must be replaced
        char srv[MAX_PATH]; snprintf(srv, sizeof(srv), "build\\pam_test\\srv\\zzzzz_kvcodPAM_nolib_v2_REV19.pk3");
        { FILE* f = fopen(srv, "ab"); if (f) { fputs("v2", f); fclose(f); } }
        system("python ../cod1pluspam/tools/make_manifest.py build/pam_test/srv --mod __pamtest --url http://127.0.0.1:8766/ -o build/pam_test/srv/pam.manifest >nul");
        s = run_job(60000);
        CHECK(s.state == PAM_RESTART);
        char pnew[MAX_PATH]; snprintf(pnew, sizeof(pnew), "%s.new", p1);
        CHECK(exists(pnew));
        CloseHandle(h);
        pam_install_swap_pending();
        CHECK(!exists(pnew));
        CHECK(fsize(p1) == fsize(srv));
    }

    // 5. bad manifest URL -> error, nothing touched
    snprintf(g_pam_install_config.manifest_url, sizeof(g_pam_install_config.manifest_url),
             "http://127.0.0.1:8766/nope.manifest");
    s = run_job(20000);
    CHECK(s.state == PAM_ERROR && strstr(s.text, "404") != nullptr);

    printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
