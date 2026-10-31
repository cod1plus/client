// demo_mod.cpp - see demo_mod.h
#include "ui/demo_mod.h"
#include "ui/demo_seek.h"                 // demo_map_installed, demo_maps_forget
#include "features/demo_index.h"          // demo_index_head
#include "features/settings_menu.h"       // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/competitive.h"          // CODMP_CVAR_SET_VA (force set)
#include "features/pak_checksum.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace patches {

namespace {

constexpr uintptr_t VA_PLAYDEMO  = 0x0040f810;   // CL_PlayDemo_f
constexpr uintptr_t VA_CMD_ARGC  = 0x008d5020;   // cmd_argc  (CL_PlayDemo_f: cmp [0x8d5020], 2)
constexpr uintptr_t VA_CMD_ARGV1 = 0x008d2624;   // cmd_argv[1]
const uint8_t PROLOGUE[6] = { 0x81, 0xEC, 0x24, 0x01, 0x00, 0x00 };   // sub esp, 0x124
constexpr int EXEC_APPEND = 2;

typedef void  (__cdecl* PlayDemo_t)();
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void* (__cdecl* Cvar_SetForce_t)(const char*, const char*);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);

PlayDemo_t g_orig = nullptr;
std::string g_last_switch;                       // "<demo>|<mod>": never switch twice in a row
DWORD g_last_switch_at = 0;

std::string cvar_str(const char* name) {
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(name);
    const char* s = cv ? *(const char* const*)((const char*)cv + 4) : nullptr;
    return s ? s : "";
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string exe_dir() {
    char p[MAX_PATH];
    const DWORD r = GetModuleFileNameA(NULL, p, MAX_PATH);
    char* sl = r ? strrchr(p, '\\') : nullptr;
    if (!sl) return "";
    sl[1] = 0;
    return p;
}

bool is_dir(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// the folders of the game dir that hold pk3s or demos: Main and every mod
std::vector<std::string> game_dirs(const std::string& root) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

// <root><dir>\demos\<name>[.dm_*] - the active mod first, then Main, then the other folders
std::string find_demo(const std::string& root, const std::string& name, const std::string& active) {
    std::vector<std::string> order;
    if (!active.empty()) order.push_back(active);
    order.push_back("Main");
    for (const std::string& d : game_dirs(root)) order.push_back(d);
    const bool has_ext = lower(name).find(".dm_") != std::string::npos;
    for (const std::string& d : order) {
        const std::string base = root + d + "\\demos\\";
        if (has_ext) {
            if (GetFileAttributesA((base + name).c_str()) != INVALID_FILE_ATTRIBUTES) return base + name;
            continue;
        }
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((base + name + ".dm_*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        return base + fd.cFileName;
    }
    return "";
}

// does a pk3's central directory list <entry> (lower case)? reads the zip directory only
bool pk3_has(const std::string& path, const std::string& entry) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool found = false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    const long tail = size < 65557 ? size : 65557;              // EOCD + max comment
    std::vector<unsigned char> t((size_t)tail);
    fseek(f, size - tail, SEEK_SET);
    if (fread(t.data(), 1, t.size(), f) == t.size()) {
        for (long i = tail - 22; i >= 0 && !found; --i) {
            if (t[i] != 0x50 || t[i + 1] != 0x4b || t[i + 2] != 0x05 || t[i + 3] != 0x06) continue;
            uint32_t cd_size, cd_off;
            memcpy(&cd_size, &t[i + 12], 4);
            memcpy(&cd_off, &t[i + 16], 4);
            if ((long)cd_off + (long)cd_size > size || cd_size > 64u * 1024 * 1024) break;
            std::vector<unsigned char> cd(cd_size);
            fseek(f, (long)cd_off, SEEK_SET);
            if (fread(cd.data(), 1, cd.size(), f) != cd.size()) break;
            size_t p = 0;
            while (p + 46 <= cd.size()) {
                if (cd[p] != 0x50 || cd[p + 1] != 0x4b || cd[p + 2] != 0x01 || cd[p + 3] != 0x02) break;
                uint16_t nlen, xlen, clen;
                memcpy(&nlen, &cd[p + 28], 2);
                memcpy(&xlen, &cd[p + 30], 2);
                memcpy(&clen, &cd[p + 32], 2);
                if (p + 46 + nlen > cd.size()) break;
                if (nlen == entry.size()) {
                    std::string n((const char*)&cd[p + 46], nlen);
                    if (lower(n) == entry) { found = true; break; }
                }
                p += 46 + (size_t)nlen + xlen + clen;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

bool mod_has_map(const std::string& root, const std::string& mod, const std::string& map) {
    const std::string entry = "maps/mp/" + lower(map) + ".bsp";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + mod + "\\*.pk3").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (pk3_has(root + mod + "\\" + fd.cFileName, entry)) { found = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// "" = play as is; else the mod to switch to first
std::string mod_for(const std::string& name, std::string* why) {
    const std::string root = exe_dir();
    const std::string active = cvar_str("fs_game");
    const std::string path = find_demo(root, name, active);
    if (path.empty()) { *why = "file not found"; return ""; }
    DemoInfo info;
    if (!demo_index_head(path.c_str(), &info) || info.maps.empty() || info.maps[0].empty()) {
        *why = "no gamestate in " + path;
        return "";
    }
    const std::string& map = info.maps[0];
    if (demo_map_installed(map)) { *why = map + " is reachable in the active mod"; return ""; }
    // 1. the mod the demo was recorded in, if this install has it
    const std::string rec = info.fs_game;
    if (!rec.empty() && _stricmp(rec.c_str(), active.c_str()) != 0 && is_dir(root + rec) &&
        mod_has_map(root, rec, map)) {
        *why = map + " is in " + rec + " (the demo's own fs_game)";
        return rec;
    }
    // 2. any mod folder whose pk3s hold the map
    for (const std::string& d : game_dirs(root)) {
        if (_stricmp(d.c_str(), "Main") == 0 || _stricmp(d.c_str(), active.c_str()) == 0) continue;
        if (mod_has_map(root, d, map)) { *why = map + " found in " + d + "'s pk3s"; return d; }
    }
    *why = map + " is installed nowhere (demo recorded with fs_game '" + rec + "')";
    return "";
}

// the engine calls it with a 4-byte aligned stack
__attribute__((force_align_arg_pointer)) void __cdecl hk_playdemo() {
    if (*(volatile int*)VA_CMD_ARGC == 2) {
        const char* arg = *(const char* const*)VA_CMD_ARGV1;
        if (arg && arg[0] && !strpbrk(arg, ";\"\n\r")) {
            std::string why;
            const std::string mod = mod_for(arg, &why);
            const std::string key = lower(std::string(arg) + "|" + mod);
            const DWORD now = GetTickCount();
            if (!mod.empty() && !(key == g_last_switch && now - g_last_switch_at < 30000)) {
                g_last_switch = key;
                g_last_switch_at = now;
                logger::logf("demo_mod: demo %s - %s", arg, why.c_str());
                demo_play_in(mod, arg);
                return;
            }
            logger::logf("demo_mod: demo %s - %s", arg, why.c_str());
        }
    }
    g_orig();
}

// --- the recorded server's pure list during playback -------------------------------------------
// CL_SystemInfoChanged (0x4176f0) returns early while clc.demoplaying (0x41774b), skipping
// FS_PureServerSetLoadedPaks too: a demo was played with EVERY pk3 of the install, also those the
// pure server never allowed. demo0080 (FPSChallenge, rPAM): the server's player animation script
// was zz_smgstrafe's, the playback took zzzzz_rPAM_n01_client_v9.pk3's ("LEANFIX_V5") -> players
// animated wrongly. CL_ParseGamestate's call to it (0x417a78) now also sets, for a demo, the list
// the recorded server sent - once every pak it referenced is here with the same checksum (else
// nothing changes: without its map pak the demo could not load at all).
constexpr uintptr_t VA_SYSINFO_CALL       = 0x00417a78;   // CL_ParseGamestate: call CL_SystemInfoChanged
constexpr uintptr_t VA_SYSINFO_CHANGED    = 0x004176f0;
constexpr uintptr_t VA_CS_OFFSET_SYSINFO  = 0x0148dcc0;   // cl.gameState.stringOffsets[CS_SYSTEMINFO]
constexpr uintptr_t VA_CS_STRINGDATA      = 0x0148fcbc;   // cl.gameState.stringData
constexpr uintptr_t VA_PURE_SET           = 0x0043f010;   // FS_PureServerSetLoadedPaks(sums, names)
constexpr uintptr_t VA_NUM_SERVER_PAKS    = 0x01904e44;   // fs_numServerPaks
// CL_SystemInfoChanged +0x18: mov esi,[0x148dcc0]; lea esi,[esi+0x148fcbc] - the two addresses above
constexpr uintptr_t VA_SYSINFO_SIG = 0x00417708;
const uint8_t SYSINFO_SIG[12] = { 0x8b, 0x35, 0xc0, 0xdc, 0x48, 0x01, 0x8d, 0xb6, 0xbc, 0xfc, 0x48, 0x01 };

typedef void (__cdecl* Void_t)();
typedef void (__cdecl* PureSet_t)(const char*, const char*);

bool g_pure_applied = false;
struct PakSum { DWORD size; FILETIME mtime; int32_t sum; bool ok; };
std::map<std::string, PakSum> g_sums;                    // path -> checksum, recomputed on change

std::string info_get(const char* info, const char* key) {
    const char* p = info;
    while (p && *p == '\\') {
        const char* k = p + 1;
        const char* ke = strchr(k, '\\');
        if (!ke) break;
        const char* v = ke + 1;
        const char* ve = strchr(v, '\\');
        if (!ve) ve = v + strlen(v);
        if ((size_t)(ke - k) == strlen(key) && _strnicmp(k, key, ke - k) == 0) return std::string(v, ve);
        p = ve;
    }
    return "";
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// checksum -> "dir/file.pk3" of the pk3s the engine searches: Main and the active mod
std::map<int32_t, std::string> local_paks() {
    std::map<int32_t, std::string> out;
    const std::string root = exe_dir();
    std::vector<std::string> dirs = { "Main" };
    const std::string mod = cvar_str("fs_game");
    if (!mod.empty()) dirs.push_back(mod);
    for (const std::string& dir : dirs) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + dir + "\\*.pk3").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string path = root + dir + "\\" + fd.cFileName;
            PakSum& c = g_sums[lower(path)];
            if (!c.ok || c.size != fd.nFileSizeLow || CompareFileTime(&c.mtime, &fd.ftLastWriteTime) != 0) {
                c.size = fd.nFileSizeLow;
                c.mtime = fd.ftLastWriteTime;
                c.ok = pak_checksum(path.c_str(), &c.sum);
            }
            if (c.ok) out.emplace(c.sum, dir + "/" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return out;
}

void apply_demo_pure() {
    const int off = *(volatile int*)VA_CS_OFFSET_SYSINFO;
    if (off <= 0) return;
    const char* si = (const char*)(VA_CS_STRINGDATA + off);
    const std::string sums = info_get(si, "sv_paks"), names = info_get(si, "sv_pakNames");
    const std::vector<std::string> ref_sums = words(info_get(si, "sv_referencedPaks"));
    const std::vector<std::string> ref_names = words(info_get(si, "sv_referencedPakNames"));
    const std::vector<std::string> sum_list = words(sums);
    if (sum_list.empty()) {                              // not a pure server: no list, as live
        if (*(volatile int*)VA_NUM_SERVER_PAKS != 0) {
            ((PureSet_t)VA_PURE_SET)("", "");
            logger::logf("demo_mod: demo of a non-pure server - pure list cleared");
        }
        g_pure_applied = false;
        return;
    }
    const DWORD t0 = GetTickCount();
    const std::map<int32_t, std::string> local = local_paks();
    std::string missing;
    int nmissing = 0;
    for (size_t i = 0; i < ref_sums.size(); ++i) {
        if (local.count((int32_t)strtol(ref_sums[i].c_str(), nullptr, 10))) continue;
        ++nmissing;
        if (missing.size() < 400) missing += " " + (i < ref_names.size() ? ref_names[i] : ref_sums[i]);
    }
    if (nmissing) {
        logger::logf("demo_mod: recorded server's pure list NOT applied - %d of the %d paks it used are missing or "
                     "different here:%s (every pk3 of the install is searched, animations may not match)",
                     nmissing, (int)ref_sums.size(), missing.c_str());
        return;
    }
    std::map<int32_t, bool> allowed;
    for (const std::string& s : sum_list) allowed[(int32_t)strtol(s.c_str(), nullptr, 10)] = true;
    std::string left_out;
    int nleft = 0;
    for (const auto& kv : local) {
        if (allowed.count(kv.first)) continue;
        ++nleft;
        if (left_out.size() < 600) left_out += " " + kv.second;
    }
    ((PureSet_t)VA_PURE_SET)(sums.c_str(), names.c_str());
    g_pure_applied = true;
    logger::logf("demo_mod: recorded server's pure list applied (%d paks, the %d it used all here, %lu ms) - "
                 "%d local pk3(s) left out for this demo, as when connected:%s",
                 (int)sum_list.size(), (int)ref_sums.size(), (unsigned long)(GetTickCount() - t0), nleft, left_out.c_str());
}

// replaces CL_ParseGamestate's call to CL_SystemInfoChanged (no argument, no register in or out)
__attribute__((force_align_arg_pointer)) void __cdecl hk_sysinfo() {
    ((Void_t)VA_SYSINFO_CHANGED)();
    if (*(volatile int*)CODMP_CLC_DEMOPLAYING_VA != 0) apply_demo_pure();
}

void install_demo_pure() {
    uint8_t* call = (uint8_t*)VA_SYSINFO_CALL;
    const int32_t rel = (int32_t)(VA_SYSINFO_CHANGED - (VA_SYSINFO_CALL + 5));
    if (call[0] != 0xE8 || memcmp(call + 1, &rel, 4) != 0 ||
        memcmp((const void*)VA_SYSINFO_SIG, SYSINFO_SIG, sizeof(SYSINFO_SIG)) != 0) {
        logger::logf("demo_mod: CL_ParseGamestate / CL_SystemInfoChanged not 1.5's - demo pure list not installed");
        return;
    }
    const int32_t nrel = (int32_t)((uintptr_t)&hk_sysinfo - (VA_SYSINFO_CALL + 5));
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(call + 1, &nrel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    logger::logf("demo_mod: demos are played with the recorded server's pure pk3 list (when its paks are all here)");
}

}  // namespace

void demo_pure_release() {
    if (!g_pure_applied) return;
    g_pure_applied = false;
    ((PureSet_t)VA_PURE_SET)("", "");
    logger::logf("demo_mod: playback over - the recorded server's pure list removed");
}

void demo_play_in(const std::string& dir, const std::string& name) {
    const std::string want = _stricmp(dir.c_str(), "Main") == 0 ? std::string() : dir;
    const std::string active = cvar_str("fs_game");
    char c[512];
    if (_stricmp(want.c_str(), active.c_str()) == 0) {
        snprintf(c, sizeof(c), "demo %s\n", name.c_str());
        ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(EXEC_APPEND, c);
        return;
    }
    // `seta fs_game` is refused ("fs_game is write protected", CVAR_INIT 0x10): the engine's
    // own Cvar_Set (force, what the Mods menu uses) sets it and marks it modified, and
    // vid_restart restarts the file system when fs_game is modified (CL_Vid_Restart_f 0x41095d)
    demo_maps_forget();                                   // the search path is about to change
    ((Cvar_SetForce_t)CODMP_CVAR_SET_VA)("fs_game", want.c_str());
    snprintf(c, sizeof(c), "vid_restart\ndemo %s\n", name.c_str());
    ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(EXEC_APPEND, c);
    logger::logf("demo_mod: fs_game '%s' -> '%s' (reads back '%s') + vid_restart, then demo %s",
                 active.c_str(), want.c_str(), cvar_str("fs_game").c_str(), name.c_str());
}

void play_in_mod(const std::string& dir, const std::string& cmds, bool rescan) {
    const std::string want = _stricmp(dir.c_str(), "Main") == 0 ? std::string() : dir;
    const std::string active = cvar_str("fs_game");
    const bool sw = _stricmp(want.c_str(), active.c_str()) != 0;
    std::string c;
    if (sw || rescan) {
        // the same restart as demo_play_in; with the folder already active, fs_game goes
        // through another value and back so it still reads as modified for vid_restart.
        // A game in progress is left first: the file system restarts with no server attached
        if (GetModuleHandleA("cgame_mp_x86.dll")) c += "disconnect\n";
        demo_maps_forget();
        if (!sw) ((Cvar_SetForce_t)CODMP_CVAR_SET_VA)("fs_game", want.empty() ? "main" : "");
        ((Cvar_SetForce_t)CODMP_CVAR_SET_VA)("fs_game", want.c_str());
        c += "vid_restart\n";
    }
    c += cmds;
    ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(EXEC_APPEND, c.c_str());
    logger::logf("demo_mod: play in '%s' (fs_game was '%s', reads back '%s'%s): %s", want.c_str(), active.c_str(),
                 cvar_str("fs_game").c_str(), sw || rescan ? ", file system restarted" : "", cmds.c_str());
}

void demo_mod_install() {
    uint8_t* site = (uint8_t*)VA_PLAYDEMO;
    if (memcmp(site, PROLOGUE, sizeof(PROLOGUE)) != 0) {
        logger::logf("demo_mod: CL_PlayDemo_f 0x%08x = %02x %02x %02x (not 1.5's) - not installed",
                     (unsigned)VA_PLAYDEMO, site[0], site[1], site[2]);
        return;
    }
    // trampoline: the 6 original bytes (position independent) + jmp back to +6
    uint8_t* tr = (uint8_t*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logger::logf("demo_mod: no trampoline memory - not installed"); return; }
    memcpy(tr, PROLOGUE, 6);
    tr[6] = 0xE9;
    *(int32_t*)(tr + 7) = (int32_t)((uintptr_t)site + 6 - ((uintptr_t)tr + 11));
    g_orig = (PlayDemo_t)tr;
    DWORD old = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) { logger::logf("demo_mod: VirtualProtect failed"); return; }
    site[0] = 0xE9;
    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)&hk_playdemo - ((uintptr_t)site + 5));
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    logger::logf("demo_mod: `demo` plays in the demo's mod (fs_game switch when its map is elsewhere)");
    install_demo_pure();
}

}  // namespace patches
