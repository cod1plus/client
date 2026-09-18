// ruleset_fetch.cpp - ruleset store + HTTPS refresh. See ruleset_fetch.h.

#include "netcode/ruleset_fetch.h"
#include "netcode/ruleset_table.h"
#include "core/logger.h"

#include <windows.h>
#include <wininet.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace patches {

RulesetFetchConfig g_ruleset_fetch_config = {
    /* enable   */ true,
    /* base_url */ "https://raw.githubusercontent.com/cod1plus/rulesets/main/",
};

namespace {

CRITICAL_SECTION g_cs;
bool             g_inited = false;
std::vector<RuleList*> g_lists;          // one per base id: the best we have

// download state (all under g_cs except inside the worker itself)
bool        g_busy = false;
std::string g_want_base;
int         g_want_version = 0;
struct Backoff { std::string base; DWORD until; };
std::vector<Backoff> g_backoff;           // per id: GetTickCount() before which no new attempt
char        g_cache_dir[MAX_PATH] = "";

constexpr size_t MAX_PB_BYTES = 512 * 1024;

bool valid_id(const char* id) {
    if (!id || !id[0] || strlen(id) > 48) return false;
    for (const char* p = id; *p; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return false;
    }
    return strcmp(id, ".") != 0 && strcmp(id, "..") != 0;
}

RuleList* find_list(const char* base) {
    for (size_t i = 0; i < g_lists.size(); ++i)
        if (!_stricmp(g_lists[i]->id.c_str(), base)) return g_lists[i];
    return nullptr;
}

// takes ownership of `nl`; returns false (and deletes it) when it is not better
bool install(RuleList* nl) {
    RuleList* cur = find_list(nl->id.c_str());
    if (cur) {
        const bool better = nl->version > cur->version ||
                            (nl->version == cur->version && nl->source == "download");
        if (!better) {
            logger::logf("ruleset: %s v%d from %s not newer than v%d (%s) - kept",
                         nl->id.c_str(), nl->version, nl->source.c_str(), cur->version, cur->source.c_str());
            delete nl;
            return false;
        }
        for (size_t i = 0; i < g_lists.size(); ++i)
            if (g_lists[i] == cur) { g_lists[i] = nl; break; }
        delete cur;
    } else {
        g_lists.push_back(nl);
    }
    logger::logf("ruleset: %s v%d (%s, %d rules) is now the list for that id",
                 nl->id.c_str(), nl->version, nl->source.c_str(), (int)nl->rules.size());
    return true;
}

void cache_path(const char* base, char* out, size_t n, const char* suffix) {
    snprintf(out, n, "%s%s.pb%s", g_cache_dir, base, suffix);
}

bool read_file(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > MAX_PB_BYTES) { fclose(f); return false; }
    }
    fclose(f);
    return !out.empty();
}

bool write_cache(const char* base, const std::string& text) {
    CreateDirectoryA(g_cache_dir, NULL);
    char tmp[MAX_PATH], fin[MAX_PATH];
    cache_path(base, tmp, sizeof(tmp), ".tmp");
    cache_path(base, fin, sizeof(fin), "");
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok) { DeleteFileA(tmp); return false; }
    return MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING) != 0;
}

// parse `text` into a fresh list tagged base/source (nullptr when it holds no rule)
RuleList* make_list(const char* base, const char* source, const std::string& text) {
    RuleList* nl = new RuleList();
    std::string warns;
    if (!ruleset_parse(text.data(), text.size(), *nl, &warns)) { delete nl; return nullptr; }
    nl->id = base;
    nl->source = source;
    if (!warns.empty()) logger::logf("ruleset: %s (%s) parser warnings:\n%s", base, source, warns.c_str());
    return nl;
}

bool http_get(const char* url, std::string& out) {
    HINTERNET hi = InternetOpenA("cod1reloaded ruleset", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hi) return false;
    DWORD tmo = 8000;
    InternetSetOptionA(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    HINTERNET hu = InternetOpenUrlA(hi, url, NULL, 0,
                                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!hu) {
        logger::logf("ruleset: GET %s failed (err=%lu)", url, GetLastError());
        InternetCloseHandle(hi);
        return false;
    }
    DWORD rtmo = 15000;
    InternetSetOptionA(hu, INTERNET_OPTION_RECEIVE_TIMEOUT, &rtmo, sizeof(rtmo));
    DWORD status = 0, ssz = sizeof(status);
    if (HttpQueryInfoA(hu, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &ssz, NULL) && status != 200) {
        logger::logf("ruleset: GET %s -> HTTP %lu", url, status);
        InternetCloseHandle(hu);
        InternetCloseHandle(hi);
        return false;
    }
    out.clear();
    char buf[8192];
    DWORD n = 0;
    bool ok = true;
    while (InternetReadFile(hu, buf, sizeof(buf), &n) && n > 0) {
        out.append(buf, n);
        if (out.size() > MAX_PB_BYTES) { ok = false; break; }
    }
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    return ok && !out.empty();
}

// under g_cs; until == 0 clears
void set_backoff(const char* base, DWORD until) {
    for (size_t i = 0; i < g_backoff.size(); ++i) {
        if (_stricmp(g_backoff[i].base.c_str(), base)) continue;
        if (until) g_backoff[i].until = until;
        else g_backoff.erase(g_backoff.begin() + i);
        return;
    }
    if (until) { Backoff b; b.base = base; b.until = until; g_backoff.push_back(b); }
}

DWORD WINAPI worker(LPVOID) {
    std::string base;
    int want = 0;
    EnterCriticalSection(&g_cs);
    base = g_want_base;
    want = g_want_version;
    LeaveCriticalSection(&g_cs);

    char url[512];
    snprintf(url, sizeof(url), "%s%s.pb", g_ruleset_fetch_config.base_url, base.c_str());
    std::string text;
    const bool got = http_get(url, text);
    RuleList* nl = got ? make_list(base.c_str(), "download", text) : nullptr;
    if (got && !nl) logger::logf("ruleset: %s holds no rule - ignored", url);

    EnterCriticalSection(&g_cs);
    g_busy = false;
    DWORD until = 0;
    if (!nl) {
        until = GetTickCount() + 60 * 1000;
    } else {
        const int got_version = nl->version;
        if (install(nl)) write_cache(base.c_str(), text);
        // the file is up but still behind what the server asks for: do not hammer
        if (want > 0 && got_version < want) {
            until = GetTickCount() + 300 * 1000;
            logger::logf("ruleset: %s is v%d online, the server asks for v%d - retry in 5 min",
                         base.c_str(), got_version, want);
        }
    }
    set_backoff(base.c_str(), until);
    LeaveCriticalSection(&g_cs);
    return 0;
}

// under g_cs
void start_download(const char* base, int want_version) {
    if (!g_ruleset_fetch_config.enable || !g_ruleset_fetch_config.base_url[0]) return;
    if (g_busy) return;
    for (size_t i = 0; i < g_backoff.size(); ++i)
        if (!_stricmp(g_backoff[i].base.c_str(), base) && (int)(GetTickCount() - g_backoff[i].until) < 0) return;
    g_busy = true;
    g_want_base = base;
    g_want_version = want_version;
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!h) { g_busy = false; return; }
    CloseHandle(h);
    logger::logf("ruleset: fetching %s%s.pb%s", g_ruleset_fetch_config.base_url, base,
                 want_version ? " (server asks a newer version)" : "");
}

}  // namespace

void ruleset_store_init() {
    if (g_inited) return;
    g_inited = true;
    InitializeCriticalSection(&g_cs);

    // "<dir of this DLL>/rulesets/"
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ruleset_store_init, &self);
    char p[MAX_PATH] = "";
    if (self && GetModuleFileNameA(self, p, MAX_PATH)) {
        char* s = strrchr(p, '\\');
        if (s) s[1] = 0;
    }
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%srulesets\\", p);

    EnterCriticalSection(&g_cs);
    RuleList* emb = new RuleList();
    ruleset_from_table(RULESET_TABLE, RULESET_COUNT, RULESET_ID, RULESET_VERSION, *emb);
    g_lists.push_back(emb);
    logger::logf("ruleset: embedded %s v%d (%d rules), cache dir %s, url %s%s",
                 RULESET_ID, RULESET_VERSION, RULESET_COUNT, g_cache_dir,
                 g_ruleset_fetch_config.enable ? g_ruleset_fetch_config.base_url : "(fetch disabled)",
                 "");
    char cp[MAX_PATH];
    cache_path(RULESET_ID, cp, sizeof(cp), "");
    std::string text;
    if (read_file(cp, text)) {
        RuleList* nl = make_list(RULESET_ID, "cache", text);
        if (nl) install(nl);
    }
    start_download(RULESET_ID, 0);       // start-up refresh, unconditional
    LeaveCriticalSection(&g_cs);
}

void ruleset_store_tick() {
    if (!g_inited || !g_ruleset_fetch_config.enable) return;
    static DWORD s_last_mark = 0;
    static std::vector<std::string> s_due;      // ids still to refresh in this round
    const DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    if (s_last_mark == 0) s_last_mark = now;    // the start-up refresh covers the first round
    if (now - s_last_mark >= (DWORD)RULESET_REFRESH_MIN * 60 * 1000) {
        s_last_mark = now;
        s_due.clear();
        for (size_t i = 0; i < g_lists.size(); ++i) s_due.push_back(g_lists[i]->id);
    }
    if (!s_due.empty() && !g_busy) {
        // one download at a time; a failed one is retried on the next round, not now
        const std::string id = s_due.back();
        s_due.pop_back();
        start_download(id.c_str(), 0);
    }
    LeaveCriticalSection(&g_cs);
}

const RuleList* ruleset_store_acquire(const char* base, int version) {
    if (!g_inited || !valid_id(base)) return nullptr;
    EnterCriticalSection(&g_cs);
    RuleList* cur = find_list(base);
    if (!cur) {
        // unknown id: the disk cache may hold it from a previous session
        char cp[MAX_PATH];
        cache_path(base, cp, sizeof(cp), "");
        std::string text;
        if (read_file(cp, text)) {
            RuleList* nl = make_list(base, "cache", text);
            if (nl) { install(nl); cur = find_list(base); }
        }
    }
    if (!cur || (version > 0 && cur->version < version)) start_download(base, version);
    if (!cur) { LeaveCriticalSection(&g_cs); return nullptr; }
    return cur;                           // lock stays held
}

void ruleset_store_release() {
    LeaveCriticalSection(&g_cs);
}

}  // namespace patches
