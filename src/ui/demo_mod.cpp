// demo_mod.cpp - see demo_mod.h
#include "ui/demo_mod.h"
#include "ui/demo_seek.h"                 // demo_map_installed, demo_maps_forget
#include "features/demo_index.h"          // demo_index_head
#include "features/settings_menu.h"       // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/competitive.h"          // CODMP_CVAR_SET_VA (force set)
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

}  // namespace

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
}

}  // namespace patches
