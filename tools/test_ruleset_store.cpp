// test_ruleset_store.cpp - host test of the ruleset store: embedded fallback, disk cache,
// download over HTTP from a local directory server, version-driven refresh.
//   python -m http.server 8765 --directory build/rulesets_test --bind 127.0.0.1 &
//   g++ -std=c++17 -I src tools/test_ruleset_store.cpp src/netcode/ruleset_fetch.cpp
//       src/netcode/ruleset_parse.cpp src/netcode/ruleset_eval.cpp src/core/logger.cpp
//       -lwininet -o build/test_ruleset_store && build/test_ruleset_store
// Run from the repo root. Writes build/rulesets_test/ (the cache dir is next to the exe: build/rulesets/).
#include "netcode/ruleset_fetch.h"
#include "netcode/ruleset_table.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace patches;

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool write_text(const char* path, const std::string& s) {
    FILE* f = fopen(path, "wb"); if (!f) return false;
    fwrite(s.data(), 1, s.size(), f); fclose(f); return true;
}

// wait until the worker thread is idle again (a fetch landed or failed)
static const RuleList* wait_for(const char* base, int version, int min_version_seen, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 50) {
        const RuleList* rl = ruleset_store_acquire(base, version);
        if (rl && rl->version >= min_version_seen) return rl;   // caller releases
        if (rl) ruleset_store_release();
        Sleep(50);
    }
    return nullptr;
}

int main() {
    char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
    char srv[MAX_PATH]; snprintf(srv, sizeof(srv), "%s\\build\\rulesets_test", cwd);
    CreateDirectoryA(srv, NULL);
    // the "server": build/rulesets_test served by `python -m http.server 8765`
    snprintf(g_ruleset_fetch_config.base_url, sizeof(g_ruleset_fetch_config.base_url), "http://127.0.0.1:8765/");
    g_ruleset_fetch_config.enable = true;

    // v2 of the embedded id online, with one changed rule
    char pb[MAX_PATH]; snprintf(pb, sizeof(pb), "%s\\%s.pb", srv, RULESET_ID);
    write_text(pb, "; version 2\npb_sv_cvar m_yaw IN 0.022\npb_sv_cvar r_picmip IN 0 1\n");
    // a second id, v5
    char pb2[MAX_PATH]; snprintf(pb2, sizeof(pb2), "%s\\other-set.pb", srv);
    write_text(pb2, "; version 5\npb_sv_cvar cg_fov IN 80 120\n");
    // stale cache of the embedded id must be replaced by the download
    char cache[MAX_PATH]; snprintf(cache, sizeof(cache), "%s\\build\\rulesets\\%s.pb", cwd, RULESET_ID);
    CreateDirectoryA("build\\rulesets", NULL);
    write_text(cache, "; version 1\npb_sv_cvar m_yaw IN 0.022\n");
    char cache2[MAX_PATH]; snprintf(cache2, sizeof(cache2), "%s\\build\\rulesets\\other-set.pb", cwd);
    DeleteFileA(cache2);

    logger::init(NULL);
    ruleset_store_init();

    // immediately: the embedded list (or the v1 cache, same version) is the fallback
    {
        const RuleList* rl = ruleset_store_acquire(RULESET_ID, 0);
        CHECK(rl != nullptr);
        if (rl) { CHECK(rl->version == RULESET_VERSION); ruleset_store_release(); }
    }
    // the start-up refresh lands: v2 from "online"
    {
        const RuleList* rl = wait_for(RULESET_ID, 0, 2, 5000);
        CHECK(rl != nullptr);
        if (rl) {
            CHECK(rl->version == 2 && rl->source == "download" && rl->rules.size() == 2);
            bool picmip = false;
            for (size_t i = 0; i < rl->rules.size(); ++i)
                if (!strcmp(rl->rules[i].cvar, "r_picmip")) picmip = rl->rules[i].mode == RM_RANGE && rl->rules[i].b == 1.f;
            CHECK(picmip);
            ruleset_store_release();
        }
        // and the cache now holds v2
        FILE* f = fopen(cache, "rb"); CHECK(f != nullptr);
        if (f) { char b[64] = ""; fgets(b, sizeof(b), f); fclose(f); CHECK(!strncmp(b, "; version 2", 11)); }
    }
    // an id we never saw: nullptr now, downloaded on request
    {
        const RuleList* rl = ruleset_store_acquire("other-set", 0);
        CHECK(rl == nullptr);
        rl = wait_for("other-set", 0, 5, 5000);
        CHECK(rl != nullptr);
        if (rl) { CHECK(rl->version == 5 && rl->rules.size() == 1); ruleset_store_release(); }
    }
    // the server asks v3 of the embedded id: the file online is still v2 -> keep v2, retry later
    {
        const RuleList* rl = ruleset_store_acquire(RULESET_ID, 3);
        CHECK(rl != nullptr && rl->version == 2);
        if (rl) ruleset_store_release();
        Sleep(600);
        // now v3 appears online, but the 5-minute backoff holds the retry: still v2
        write_text(pb, "; version 3\npb_sv_cvar m_yaw IN 0.022\n");
        rl = ruleset_store_acquire(RULESET_ID, 3);
        CHECK(rl != nullptr && rl->version == 2);
        if (rl) ruleset_store_release();
    }
    // a bad id never touches the disk or the network
    CHECK(ruleset_store_acquire("../evil", 0) == nullptr);
    CHECK(ruleset_store_acquire("", 0) == nullptr);
    // a 404 (missing file) leaves what we have
    {
        const RuleList* rl = ruleset_store_acquire("nope-set", 0);
        CHECK(rl == nullptr);
        Sleep(800);
        CHECK(ruleset_store_acquire("nope-set", 0) == nullptr);
    }

    // leave no fake cache behind: another host run in build/ would take the v2 stub for real
    DeleteFileA(cache);
    DeleteFileA(cache2);
    printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
