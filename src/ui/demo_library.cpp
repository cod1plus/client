// demo_library.cpp - see demo_library.h
#include "ui/demo_library.h"
#include "core/logger.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace patches {

namespace {

typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
constexpr int CV_STRING = 0x04;
const char* kCacheName = "cod1reloaded-demos.idx";
const char* kCacheMagic = "cod1x-demo-index 2";    // 2: maps per gamestate + kill segment (1 had the LAST map)

std::vector<DemoItem> g_items;

// ---- worker ---------------------------------------------------------------
struct Job { std::string path; uint64_t size, mtime; };
struct Done { std::string path; uint64_t size, mtime; DemoInfo info; };

CRITICAL_SECTION g_cs;
bool g_cs_init = false;
std::vector<Job> g_queue;            // front = next
std::vector<Done> g_done;
HANDLE g_thread = nullptr;
HANDLE g_wake = nullptr;
volatile LONG g_active_until = 0;    // GetTickCount() before which background indexing runs
volatile LONG g_priority_pending = 0;// a requested job sits at the front

// ---- cache ----------------------------------------------------------------
struct CacheEntry { uint64_t size, mtime; DemoInfo info; };
std::map<std::string, CacheEntry> g_cache;    // lower-case path -> entry
bool g_cache_loaded = false;
bool g_cache_dirty = false;
DWORD g_cache_dirty_since = 0;

void lock()   { EnterCriticalSection(&g_cs); }
void unlock() { LeaveCriticalSection(&g_cs); }

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

bool game_root(char* out, size_t n) {
    DWORD r = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!r || r >= n) return false;
    char* sl = strrchr(out, '\\');
    if (!sl) return false;
    sl[1] = 0;
    return true;
}

std::string cache_path() {
    char root[MAX_PATH];
    if (!game_root(root, sizeof(root))) return "";
    return std::string(root) + kCacheName;
}

// one field of a cache line: tabs / newlines / backslashes escaped
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}
std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
        size_t e = line.find('\t', p);
        f.push_back(line.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return f;
}

void cache_load() {
    g_cache_loaded = true;
    const std::string cp = cache_path();
    FILE* f = cp.empty() ? nullptr : fopen(cp.c_str(), "rb");
    if (!f) return;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    size_t p = data.find('\n');
    if (p == std::string::npos || data.compare(0, strlen(kCacheMagic), kCacheMagic) != 0) return;
    ++p;
    CacheEntry* cur = nullptr;
    int demos = 0;
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        const std::string line = data.substr(p, e - p);
        p = e + 1;
        const std::vector<std::string> f2 = split_tabs(line);
        if (f2.empty()) continue;
        if (f2[0] == "D" && f2.size() >= 18) {
            CacheEntry ce;
            ce.size = _strtoui64(f2[2].c_str(), nullptr, 10);
            ce.mtime = _strtoui64(f2[3].c_str(), nullptr, 10);
            DemoInfo& d = ce.info;
            d.ok = f2[4] == "1";
            d.map = unesc(f2[5]);
            d.gametype = unesc(f2[6]);
            d.hostname = unesc(f2[7]);
            d.recorder = atoi(f2[8].c_str());
            d.recorder_name = unesc(f2[9]);
            d.first_time = atoi(f2[10].c_str());
            d.last_time = atoi(f2[11].c_str());
            d.duration_ms = atoi(f2[12].c_str());
            d.messages = atoi(f2[13].c_str());
            d.snapshots = atoi(f2[14].c_str());
            d.gamestates = atoi(f2[15].c_str());
            d.error = unesc(f2[16]);
            {
                const std::string ml = unesc(f2[17]);          // "german_town,mp_carentan"
                size_t q = 0;
                while (!ml.empty()) {
                    const size_t c = ml.find(',', q);
                    d.maps.push_back(ml.substr(q, c == std::string::npos ? std::string::npos : c - q));
                    if (c == std::string::npos) break;
                    q = c + 1;
                }
            }
            cur = &(g_cache[lower(unesc(f2[1]))] = ce);
            ++demos;
        } else if (f2[0] == "K" && f2.size() >= 14 && cur) {
            DemoKill k;
            k.server_time = atoi(f2[1].c_str());
            k.t_ms = atoi(f2[2].c_str());
            k.attacker = atoi(f2[3].c_str());
            k.victim = atoi(f2[4].c_str());
            k.weapon = atoi(f2[5].c_str());
            k.mod = atoi(f2[6].c_str());
            k.pov = atoi(f2[7].c_str());
            k.pov_weapon = atoi(f2[8].c_str());
            k.attacker_name = unesc(f2[9]);
            k.victim_name = unesc(f2[10]);
            k.weapon_name = unesc(f2[11]);
            k.segment = atoi(f2[12].c_str());
            cur->info.kills.push_back(k);
        }
    }
    logger::logf("demo_library: %d demo(s) known from %s", demos, kCacheName);
}

void cache_save() {
    const std::string cp = cache_path();
    if (cp.empty()) return;
    const std::string tmp = cp + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "%s\n", kCacheMagic);
    for (const auto& kv : g_cache) {
        const DemoInfo& d = kv.second.info;
        std::string maps;
        for (size_t i = 0; i < d.maps.size(); ++i) { if (i) maps += ','; maps += d.maps[i]; }
        fprintf(f, "D\t%s\t%llu\t%llu\t%d\t%s\t%s\t%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
                esc(kv.first).c_str(), (unsigned long long)kv.second.size, (unsigned long long)kv.second.mtime,
                d.ok ? 1 : 0, esc(d.map).c_str(), esc(d.gametype).c_str(), esc(d.hostname).c_str(),
                d.recorder, esc(d.recorder_name).c_str(), d.first_time, d.last_time, d.duration_ms,
                d.messages, d.snapshots, d.gamestates, esc(d.error).c_str(), esc(maps).c_str());
        for (const DemoKill& k : d.kills)
            fprintf(f, "K\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%d\t\n",
                    k.server_time, k.t_ms, k.attacker, k.victim, k.weapon, k.mod, k.pov, k.pov_weapon,
                    esc(k.attacker_name).c_str(), esc(k.victim_name).c_str(), esc(k.weapon_name).c_str(),
                    k.segment);
    }
    const bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp.c_str(), cp.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
    g_cache_dirty = false;
}

DWORD WINAPI worker(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Job job;
        bool have = false;
        lock();
        const bool active = (LONG)(GetTickCount() - (DWORD)g_active_until) < 0;
        if (!g_queue.empty() && (active || g_priority_pending)) {
            job = g_queue.front();
            g_queue.erase(g_queue.begin());
            g_priority_pending = 0;
            have = true;
        }
        unlock();
        if (!have) { WaitForSingleObject(g_wake, 500); continue; }
        Done d;
        d.path = job.path;
        d.size = job.size;
        d.mtime = job.mtime;
        demo_index_file(job.path.c_str(), &d.info);
        lock();
        g_done.push_back(std::move(d));
        unlock();
    }
    return 0;
}

void ensure_worker() {
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
    if (!g_wake) g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_thread) {
        g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (!g_thread) logger::logf("demo_library: worker thread failed (err=%lu)", GetLastError());
    }
}

void scan_root(const std::string& root_in, std::vector<DemoItem>& out) {
    if (root_in.empty()) return;
    std::string root = root_in;
    if (root.back() != '\\' && root.back() != '/') root += '\\';
    WIN32_FIND_DATAA dd;
    HANDLE hd = FindFirstFileA((root + "*").c_str(), &dd);
    if (hd == INVALID_HANDLE_VALUE) return;
    do {
        if (!(dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dd.cFileName[0] == '.') continue;
        const std::string dir = dd.cFileName;
        const std::string base = root + dir + "\\demos\\";
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((base + "*.dm_*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DemoItem it;
            it.name = fd.cFileName;
            it.dir = dir;
            it.path = base + fd.cFileName;
            it.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            it.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            bool dup = false;                   // same demo reached through two roots
            for (const DemoItem& o : out)
                if (_stricmp(o.dir.c_str(), it.dir.c_str()) == 0 && _stricmp(o.name.c_str(), it.name.c_str()) == 0) { dup = true; break; }
            if (!dup) out.push_back(it);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    } while (FindNextFileA(hd, &dd));
    FindClose(hd);
}

const char* cvar_str(const char* name) {
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(name);
    return cv ? *(const char**)((char*)cv + CV_STRING) : "";
}

void apply_cache(DemoItem& it) {
    auto c = g_cache.find(lower(it.path));
    if (c != g_cache.end() && c->second.size == it.size && c->second.mtime == it.mtime) {
        it.info = c->second.info;
        it.state = it.info.ok ? DEMO_READY : DEMO_FAILED;
    }
}

}  // namespace

void demo_library_scan() {
    ensure_worker();
    if (!g_cache_loaded) cache_load();
    std::vector<DemoItem> fresh;
    char root[MAX_PATH];
    if (game_root(root, sizeof(root))) scan_root(root, fresh);
    scan_root(cvar_str("fs_homepath"), fresh);          // CoD records into fs_homepath
    scan_root(cvar_str("fs_basepath"), fresh);
    // keep what the old list knew; the cache fills the rest
    for (DemoItem& it : fresh) {
        for (const DemoItem& o : g_items)
            if (o.path == it.path && o.size == it.size && o.mtime == it.mtime) { it.state = o.state; it.info = o.info; break; }
        if (it.state == DEMO_UNKNOWN || it.state == DEMO_INDEXING) {
            it.state = DEMO_UNKNOWN;
            apply_cache(it);
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const DemoItem& a, const DemoItem& b) { return a.mtime > b.mtime; });
    g_items.swap(fresh);
    // queue what is not known yet, newest first
    lock();
    g_queue.clear();
    for (DemoItem& it : g_items)
        if (it.state == DEMO_UNKNOWN) { g_queue.push_back({ it.path, it.size, it.mtime }); it.state = DEMO_INDEXING; }
    unlock();
    SetEvent(g_wake);
}

std::vector<DemoItem>& demo_library_items() { return g_items; }

void demo_library_pump(bool tab_visible) {
    if (!g_cs_init) return;
    if (tab_visible) InterlockedExchange(&g_active_until, (LONG)(GetTickCount() + 3000));
    std::vector<Done> done;
    lock();
    done.swap(g_done);
    unlock();
    for (Done& d : done) {
        for (DemoItem& it : g_items) {
            if (it.path != d.path) continue;
            it.info = d.info;
            it.state = d.info.ok ? DEMO_READY : DEMO_FAILED;
            break;
        }
        g_cache[lower(d.path)] = CacheEntry{ d.size, d.mtime, std::move(d.info) };
        if (!g_cache_dirty) g_cache_dirty_since = GetTickCount();
        g_cache_dirty = true;
    }
    if (!done.empty()) SetEvent(g_wake);
    // write the cache once the burst is over (or at least every 10 s during a long one)
    bool idle;
    lock();
    idle = g_queue.empty();
    unlock();
    if (g_cache_dirty && (idle || GetTickCount() - g_cache_dirty_since > 10000)) cache_save();
}

void demo_library_request(int index) {
    if (index < 0 || index >= (int)g_items.size() || !g_cs_init) return;
    DemoItem& it = g_items[index];
    if (it.state == DEMO_READY || it.state == DEMO_FAILED) return;
    lock();
    for (size_t i = 0; i < g_queue.size(); ++i)
        if (g_queue[i].path == it.path) { g_queue.erase(g_queue.begin() + i); break; }
    g_queue.insert(g_queue.begin(), Job{ it.path, it.size, it.mtime });
    g_priority_pending = 1;
    unlock();
    it.state = DEMO_INDEXING;
    SetEvent(g_wake);
}

void demo_library_progress(int* ready, int* total) {
    int r = 0;
    for (const DemoItem& it : g_items) if (it.state == DEMO_READY || it.state == DEMO_FAILED) ++r;
    if (ready) *ready = r;
    if (total) *total = (int)g_items.size();
}

}  // namespace patches
