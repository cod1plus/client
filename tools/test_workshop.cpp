// test_workshop.cpp - host test of the Workshop backend (features/workshop.cpp) against a real
// catalog: read by file:// and over http, installed into the exe's folder (the "game dir").
// Built and run by tools/test_workshop.sh, which prepares the served folder:
//     test_workshop.exe <file:///.../srv/> <http://127.0.0.1:8771/>
#include "features/workshop.h"
#include "features/pam_install.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace patches;

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static long long fsize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
static std::string slurp(const std::string& p) {
    std::string s;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return s;
    char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static void load(const std::string& url, WsCatalogInfo& info, std::vector<WsItem>& items) {
    snprintf(g_workshop_config.manifest_url, sizeof(g_workshop_config.manifest_url), "%s", url.c_str());
    const DWORD t0 = GetTickCount();
    workshop_refresh();
    for (int t = 0; t < 60000; t += 20) {
        Sleep(20);
        workshop_snapshot(info, items);
        if (!info.busy) break;
    }
    printf("   catalog -> state %d, %d item(s), %lu ms: %s\n", info.state, (int)items.size(),
           GetTickCount() - t0, info.text.c_str());
}

static WsItem install(const std::string& id) {
    workshop_install(id);
    WsCatalogInfo info;
    std::vector<WsItem> items;
    WsItem it;
    float last = -1.f;
    int updates = 0;
    const DWORD t0 = GetTickCount();
    for (int t = 0; t < 180000; t += 20) {
        Sleep(20);
        workshop_snapshot(info, items);
        for (size_t i = 0; i < items.size(); ++i) if (items[i].id == id) it = items[i];
        if (it.job == WSJ_DOWNLOADING && it.progress != last) { last = it.progress; ++updates; }
        if (!workshop_busy() && (it.job == WSJ_DONE || it.job == WSJ_ERROR)) break;
    }
    printf("   install -> job %d, local %d, %lu ms, %d progress steps: %s\n", it.job, it.local,
           GetTickCount() - t0, updates, it.job_text.c_str());
    return it;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: test_workshop <file:///srv/> <http://host:port/>\n"); return 2; }
    logger::init(NULL);
    const std::string fbase = argv[1], hbase = argv[2];
    char d[MAX_PATH];
    pkg_game_dir(d, sizeof(d));
    const std::string game = d, pk3 = game + "main\\zz_mp_liberation.pk3";
    WsCatalogInfo info;
    std::vector<WsItem> items;

    printf("1. local catalog (file://), nothing installed\n");
    load(fbase + "workshop.json", info, items);
    CHECK(info.state == WSC_READY && info.local);
    CHECK(info.title == "1.6X Workshop (local test)");
    CHECK(items.size() == 1);
    if (items.empty()) { printf("cannot go on\n"); return 1; }
    const WsItem first = items[0];
    CHECK(first.id == "mp_liberation" && first.name == "Liberation" && first.type == "map" && first.version == "27");
    CHECK(first.folder == "main" && first.files.size() == 1 && first.files[0].file == "zz_mp_liberation.pk3");
    CHECK(first.size > 1000000 && first.files[0].sha256.size() == 64 && first.size == first.files[0].size);
    CHECK(first.files[0].url.compare(0, 8, "file:///") == 0);
    CHECK(first.local == WS_ABSENT);
    CHECK(!first.thumb_path.empty() && exists(first.thumb_path) && fsize(first.thumb_path) > 1000);
    printf("   thumbnail cached at %s\n", first.thumb_path.c_str());

    printf("2. install from the local catalog\n");
    WsItem it = install("mp_liberation");
    CHECK(it.job == WSJ_DONE && it.local == WS_INSTALLED && it.installed_now);
    CHECK(fsize(pk3) == (long long)first.size);
    CHECK(!exists(pk3 + ".part") && !exists(pk3 + ".new"));
    CHECK(slurp(game + "cod1reloaded\\workshop\\installed.txt").find("mp_liberation 27 " + first.files[0].sha256) != std::string::npos);

    printf("3. refresh: installed, from the hash memo\n");
    load(fbase + "workshop.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_INSTALLED && items[0].local_version == "27");
    CHECK(items[0].installed_now);                     // kept across the refresh

    printf("4. another file under that name = update; held open by the game = staged\n");
    {
        FILE* f = fopen(pk3.c_str(), "wb");
        std::string junk(4096, 'x');
        fwrite(junk.data(), 1, junk.size(), f);
        fclose(f);
    }
    load(fbase + "workshop.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_OUTDATED && items[0].local_version.empty());
    HANDLE lock = CreateFileA(pk3.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    it = install("mp_liberation");
    CHECK(it.job == WSJ_DONE && it.local == WS_STAGED);
    CHECK(fsize(pk3 + ".new") == (long long)first.size && fsize(pk3) == 4096);
    CloseHandle(lock);
    load(fbase + "workshop.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_STAGED);
    pam_install_swap_pending();                        // = the next launch
    CHECK(!exists(pk3 + ".new") && fsize(pk3) == (long long)first.size);
    load(fbase + "workshop.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_INSTALLED);

    printf("5. a catalog hash that does not match the file: nothing placed\n");
    CHECK(MoveFileExA(pk3.c_str(), (pk3 + ".keep").c_str(), MOVEFILE_REPLACE_EXISTING));
    load(fbase + "bad_sha.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_ABSENT);
    it = install("mp_liberation");
    CHECK(it.job == WSJ_ERROR && it.job_text.find("Checksum") != std::string::npos);
    CHECK(!exists(pk3) && !exists(pk3 + ".part") && !exists(pk3 + ".new"));

    printf("6. catalog over http, relative links\n");
    load(hbase + "workshop.json", info, items);
    CHECK(info.state == WSC_READY && !info.local && items.size() == 1);
    if (!items.empty()) {
        CHECK(items[0].files[0].url == hbase + "zz_mp_liberation.pk3");
        CHECK(items[0].local == WS_ABSENT);
        CHECK(!items[0].thumb_path.empty() && exists(items[0].thumb_path) && items[0].thumb_path != first.thumb_path);
    }
    it = install("mp_liberation");
    CHECK(it.job == WSJ_DONE && it.local == WS_INSTALLED);
    CHECK(fsize(pk3) == (long long)first.size);

    printf("7. a file:// link inside an online catalog is refused\n");
    load(hbase + "http_fileurl.json", info, items);
    CHECK(info.state == WSC_READY && items.empty());

    printf("8. malformed items skipped, the good one kept (and \\u escapes decoded)\n");
    load(fbase + "bad_items.json", info, items);
    CHECK(info.state == WSC_READY && items.size() == 1);
    if (items.size() == 1) {
        CHECK(items[0].id == "good");
        CHECK(items[0].name == "Lib\xC3\xA9ration");
        CHECK(items[0].local == WS_INSTALLED);
    }

    printf("9. offline: the last catalog that loaded stays visible\n");
    load("http://127.0.0.1:1/workshop.json", info, items);
    CHECK(info.state == WSC_CACHED && items.size() == 1);

    printf("10. a pack of several pk3s with a cvar (the HD textures' shape)\n");
    load(fbase + "pack.json", info, items);
    CHECK(info.state == WSC_READY && items.size() == 1);
    if (items.size() == 1) {
        CHECK(items[0].files.size() == 2 && items[0].local == WS_ABSENT);
        CHECK(items[0].size == items[0].files[0].size + items[0].files[1].size);
        CHECK(items[0].cvars.size() == 1 && items[0].cvars[0].first == "com_hunkMegs" && items[0].cvars[0].second == "512");
    }
    CHECK(workshop_take_cvars().empty());
    it = install("hdpack");
    CHECK(it.job == WSJ_DONE && it.local == WS_INSTALLED);
    CHECK(fsize(game + "main\\zz_pack_a.pk3") > 0 && fsize(game + "main\\zz_pack_b.pk3") > 0);
    {
        auto cv = workshop_take_cvars();
        CHECK(cv.size() == 1 && cv[0].first == "com_hunkMegs" && cv[0].second == "512");
        CHECK(workshop_take_cvars().empty());            // handed out once
    }
    {   // one file of the pack changes on disk: update, and only that file comes back
        FILE* f = fopen((game + "main\\zz_pack_b.pk3").c_str(), "wb");
        fputs("not the pack", f);
        fclose(f);
    }
    load(fbase + "pack.json", info, items);
    CHECK(items.size() == 1 && items[0].local == WS_OUTDATED);
    it = install("hdpack");
    CHECK(it.job == WSJ_DONE && it.local == WS_INSTALLED);
    load(fbase + "pack_badcvar.json", info, items);
    CHECK(items.size() == 1 && items[0].cvars.empty());

    printf("11. no catalog configured\n");
    g_workshop_config.manifest_url[0] = 0;
    workshop_refresh();
    workshop_snapshot(info, items);
    CHECK(info.state == WSC_NONE && !info.busy);

    logger::flush();
    printf(fails ? "FAILED: %d check(s)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
