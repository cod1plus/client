// In-game 1.6X settings menu bridge.
//
// The menu itself is a stock idTech3 ui_mp/cod1x_settings.menu shipped in a Main/ pk3;
// it binds: FOV slider -> cg_fov, View mode -> cod1x_widescreen, Aspect ratio -> cod1x_aspect.
// This module makes those bindings do something: registers the cod1x_* cvars (seeded from the
// .ini), unlocks cg_fov so it isn't cheat-locked to 80, opens the menu on a hotkey (runtime
// `loadmenu`+`open`, so it works regardless of pk3 load order / PAM), and mirrors changes
// back into cod1reloaded.ini (the persistent home).
//
// FOV needs no new render hook: the widescreen Hor+ hook already reads cg_fov every frame,
// so once the clamp/cheat lock is gone the chosen FOV flows straight through it.

#include "features/settings_menu.h"
#include "video/widescreen_fix.h"     // g_widescreen_config, AspectMode
#include "video/window_patch.h"       // get_game_window()
#include "video/display_probe.h"      // resolution_for_r_mode()
#include "netcode/protocol_patch.h"   // CODMP_CVAR_GET_VA, CODMP_CVAR_COUNT_VA
#include "netcode/competitive.h"      // CODMP_CVAR_SET_VA
#include "core/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace patches {

SettingsMenuConfig g_settings_menu_config = {};

namespace {

// cvar_t layout (RE'd): array stride 0x2c; the fields we touch:
constexpr int CVAR_OFF_STRING  = 0x04;
constexpr int CVAR_OFF_FLAGS   = 0x10;
constexpr int CVAR_OFF_VALUE   = 0x1c;
constexpr int CVAR_OFF_INTEGER = 0x20;
constexpr int CVAR_FLAG_CHEAT  = 0x200;
constexpr int EXEC_APPEND      = 2;   // Cbuf_ExecuteText: append (runs next frame on main thread)

typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);
typedef void* (__cdecl* Cvar_FindVar_t)(const char*);
typedef void  (__cdecl* Cbuf_ExecuteText_t)(int, const char*);

const Cvar_Get_t         Cvar_Get         = (Cvar_Get_t)CODMP_CVAR_GET_VA;
const Cvar_FindVar_t     Cvar_FindVar     = (Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA;
const Cbuf_ExecuteText_t Cbuf_ExecuteText = (Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA;

bool engine_ready() {
    // cvar system up + we really are inside CoDMP.exe (same gate protocol_patch/competitive use)
    return (uintptr_t)GetModuleHandleA(NULL) == 0x400000 &&
           *(volatile int*)CODMP_CVAR_COUNT_VA > 0;
}

// View mode (cod1x_viewmode): 0 = classic (Vert-, old zoomed), 1 = widescreen (Hor+),
// 2 = stretched (4:3 stretched to fill the screen, wider models). Mutually exclusive.
int config_to_viewmode() {
    if (g_widescreen_config.stretch_enable)     return 2;
    if (g_widescreen_config.horplus_fov_enable) return 1;
    return 0;
}
const char* viewmode_apply(int v) {
    switch (v) {
        case 2: g_widescreen_config.horplus_fov_enable = false;
                g_widescreen_config.stretch_enable     = true;  return "stretched";
        case 1: g_widescreen_config.horplus_fov_enable = true;
                g_widescreen_config.stretch_enable     = false; return "widescreen";
        default:g_widescreen_config.horplus_fov_enable = false;
                g_widescreen_config.stretch_enable     = false; return "classic";
    }
}

// ---- refresh rate ("hertz") --------------------------------------------------------------
// The engine honors r_displayRefresh (latched; vid_restart applies it, with a native
// fallback if the mode fails). "max" = highest Hz the display supports at the game's
// resolution (r_mode table / r_customwidth), else the highest it supports at all.

int resolve_max_hz() {
    int gw = 0, gh = 0;
    void* mode = Cvar_FindVar("r_mode");
    if (mode) {
        const int m = *(int*)((char*)mode + CVAR_OFF_INTEGER);
        if (resolution_for_r_mode(m, &gw, &gh)) {
            // resolved from the r_mode table
        } else if (m == -1) {
            void* cw = Cvar_FindVar("r_customwidth");
            void* ch = Cvar_FindVar("r_customheight");
            if (cw && ch) {
                gw = *(int*)((char*)cw + CVAR_OFF_INTEGER);
                gh = *(int*)((char*)ch + CVAR_OFF_INTEGER);
            }
        }
    }
    int best_at_res = 0, best_any = 0;
    DEVMODEA dm;
    dm.dmSize = sizeof(dm);
    dm.dmDriverExtra = 0;
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); ++i) {
        if (dm.dmBitsPerPel < 32) continue;
        const int hz = (int)dm.dmDisplayFrequency;
        if (hz > best_any) best_any = hz;
        if (gw && (int)dm.dmPelsWidth == gw && (int)dm.dmPelsHeight == gh && hz > best_at_res)
            best_at_res = hz;
    }
    // MAX = the display's true maximum (a 320Hz panel means 320), NOT the max at the game's
    // current resolution — at 640x480/800x600 Windows often only lists 60-75Hz, which is not
    // what a "max Hz" toggle means. If the res+Hz combo fails, the engine has a native
    // fallback ("Forcing default value for r_displayRefresh..."). at-res kept as diagnostic.
    logger::logf("settings_menu: display max %d Hz (any res); %d Hz listed at game res %dx%d",
                 best_any, best_at_res, gw, gh);
    return best_any ? best_any : best_at_res;
}

// "auto" -> 0 (engine/desktop default), "max" -> resolve_max_hz(), "144" -> 144.
int resolve_refresh_hz(const char* s) {
    if (!s || !*s || _stricmp(s, "auto") == 0) return 0;
    if (_stricmp(s, "max") == 0) return resolve_max_hz();
    return atoi(s);
}

void apply_refresh(const char* s) {
    const int hz = resolve_refresh_hz(s);
    char cmd[96];
    if (hz > 0) {
        snprintf(cmd, sizeof(cmd),
                 "seta r_displayRefresh %d\nset cod1x_refresh_hz \"%d HZ\"\n", hz, hz);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "seta r_displayRefresh 0\nset cod1x_refresh_hz \"DEFAULT\"\n");
    }
    Cbuf_ExecuteText(EXEC_APPEND, cmd);   // seta: archived, so it also holds without our ini
    logger::logf("settings_menu: refresh rate '%s' -> r_displayRefresh %d (applies on vid_restart)", s, hz);
}

// ---- config / demo file slots (menu FILES page) ------------------------------------------
// The menu engine cannot list files, so the mod scans the disk and publishes fixed slots:
//   cod1x_cfg_<i>       display name        cod1x_cfg_exec_<i>   "exec <path>"
//   cod1x_demo_<i>      display name        cod1x_demo_exec_<i>  "demo <basename>"
// The menu paints the display cvar (TEXT+cvar) and a click runs `vstr cod1x_*_exec_<i>`.
// Empty slots publish "" and the menu hides them via cvarTest/hideCvar.
constexpr int FILE_SLOTS = 10;

bool game_main_path(char* out, size_t cap, const char* sub) {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    slash[1] = '\0';
    if (snprintf(out, cap, "%sMain\\%s", path, sub) >= (int)cap) return false;
    return true;
}

struct FoundFile {
    char     display[80];   // shown in the menu ("name.cfg" / "configs/name.cfg" / demo basename)
    char     exec[112];     // console command the slot runs
    FILETIME mtime;
};

// filenames go into quoted `set` commands — refuse anything that would break parsing
bool filename_ok(const char* n) {
    return !strpbrk(n, "\";\n\r");
}

int scan_dir(const char* sub, const char* pattern, FoundFile* out, int cap,
             bool (*accept)(const char*), void (*fill)(FoundFile*, const char*)) {
    char dir[MAX_PATH];
    if (!game_main_path(dir, sizeof(dir), sub)) return 0;
    char glob[MAX_PATH];
    if (snprintf(glob, sizeof(glob), "%s%s", dir, pattern) >= (int)sizeof(glob)) return 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!filename_ok(fd.cFileName)) continue;
        if (accept && !accept(fd.cFileName)) continue;
        if (n >= cap) break;
        fill(&out[n], fd.cFileName);
        out[n].mtime = fd.ftLastWriteTime;
        ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

bool accept_cfg_mainroot(const char* n) {
    // skip the engine-managed configs; everything else in Main/ is a player config
    static const char* SKIP[] = { "config_mp.cfg", "config.cfg", "default_mp.cfg", "default.cfg",
                                  "uoconfig_mp.cfg", "uoconfig.cfg" };
    for (const char* s : SKIP)
        if (_stricmp(n, s) == 0) return false;
    return true;
}

void fill_cfg_mainroot(FoundFile* f, const char* n) {
    snprintf(f->display, sizeof(f->display), "%s", n);
    snprintf(f->exec, sizeof(f->exec), "exec %s", n);
}
void fill_cfg_subdir(FoundFile* f, const char* n) {
    snprintf(f->display, sizeof(f->display), "configs/%s", n);
    snprintf(f->exec, sizeof(f->exec), "exec configs/%s", n);
}
void fill_demo(FoundFile* f, const char* n) {
    // display without the .dm_<proto> extension, but PLAY with the full filename — the
    // engine appends its own protocol suffix to bare names, which breaks on old demos.
    char base[80];
    snprintf(base, sizeof(base), "%s", n);
    char* dot = strstr(base, ".dm_");
    if (dot) *dot = '\0';
    snprintf(f->display, sizeof(f->display), "%s", base);
    snprintf(f->exec, sizeof(f->exec), "demo %s", n);
}

int cmp_name(const void* a, const void* b) {
    return _stricmp(((const FoundFile*)a)->display, ((const FoundFile*)b)->display);
}
int cmp_mtime_desc(const void* a, const void* b) {
    return CompareFileTime(&((const FoundFile*)b)->mtime, &((const FoundFile*)a)->mtime);
}

void append_slots(char* buf, size_t cap, const char* prefix, const FoundFile* files, int n) {
    for (int i = 0; i < FILE_SLOTS; ++i) {
        char line[256];
        if (i < n) {
            snprintf(line, sizeof(line), "set %s_%d \"%s\"\nset %s_exec_%d \"%s\"\n",
                     prefix, i + 1, files[i].display, prefix, i + 1, files[i].exec);
        } else {
            snprintf(line, sizeof(line), "set %s_%d \"\"\nset %s_exec_%d \"\"\n",
                     prefix, i + 1, prefix, i + 1);
        }
        if (strlen(buf) + strlen(line) + 1 < cap) strcat(buf, line);
    }
}

void publish_files() {
    // make sure Main/configs exists so writeconfig + the convention have a home
    char cfgdir[MAX_PATH];
    if (game_main_path(cfgdir, sizeof(cfgdir), "configs"))
        CreateDirectoryA(cfgdir, NULL);

    FoundFile cfgs[FILE_SLOTS];
    int nc = scan_dir("", "*.cfg", cfgs, FILE_SLOTS, accept_cfg_mainroot, fill_cfg_mainroot);
    nc += scan_dir("configs\\", "*.cfg", cfgs + nc, FILE_SLOTS - nc, NULL, fill_cfg_subdir);
    qsort(cfgs, nc, sizeof(FoundFile), cmp_name);

    FoundFile demos[FILE_SLOTS];
    int nd = scan_dir("demos\\", "*.dm_*", demos, FILE_SLOTS, NULL, fill_demo);
    qsort(demos, nd, sizeof(FoundFile), cmp_mtime_desc);   // newest demo first

    static char buf[8192];
    buf[0] = '\0';
    append_slots(buf, sizeof(buf), "cod1x_cfg", cfgs, nc);
    append_slots(buf, sizeof(buf), "cod1x_demo", demos, nd);
    strcat(buf, "set cod1x_files_scan 0\n");
    Cbuf_ExecuteText(EXEC_APPEND, buf);
    logger::logf("settings_menu: published file slots (%d configs, %d demos)", nc, nd);
}

// SAVE AS: menu sets cod1x_files_save 1; we writeconfig Main/configs/<cod1x_savename>.cfg
void do_save_config() {
    char name[64] = {0};
    void* cv = Cvar_FindVar("cod1x_savename");
    if (cv) {
        const char* s = *(const char**)((char*)cv + CVAR_OFF_STRING);
        if (s) snprintf(name, sizeof(name), "%s", s);
    }
    // sanitize: keep [A-Za-z0-9_-] only, drop a typed ".cfg"
    char clean[64];
    int j = 0;
    for (int i = 0; name[i] && j < (int)sizeof(clean) - 1; ++i) {
        const char c = name[i];
        if (strcmp(name + i, ".cfg") == 0) break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            clean[j++] = c;
    }
    clean[j] = '\0';

    char cmd[192];
    if (j > 0) {
        snprintf(cmd, sizeof(cmd),
                 "writeconfig configs/%s.cfg\nset cod1x_files_save 0\nset cod1x_files_scan 1\n",
                 clean);
        logger::logf("settings_menu: save config -> Main/configs/%s.cfg", clean);
    } else {
        snprintf(cmd, sizeof(cmd), "set cod1x_files_save 0\n");
    }
    Cbuf_ExecuteText(EXEC_APPEND, cmd);
}

// cod1reloaded.ini sits next to CoDMP.exe / mss32.dll (same dir load_config reads from).
void ini_writeback(const char* key, const char* value) {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = '\0';
    if (strlen(path) + strlen("cod1reloaded.ini") >= MAX_PATH) return;
    strcat(path, "cod1reloaded.ini");
    WritePrivateProfileStringA("cod1reloaded", key, value, path);
}

bool poke_dword(uintptr_t va, uint32_t expect, uint32_t val) {
    uint32_t* p = (uint32_t*)va;
    if (*p == val) return true;      // already patched
    if (*p != expect) return false;  // unexpected / not ready
    DWORD old = 0;
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = val;
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    return true;
}

bool poke_byte(uintptr_t va, uint8_t expect, uint8_t val) {
    uint8_t* p = (uint8_t*)va;
    if (*p == val) return true;
    if (*p != expect) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *p = val;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return true;
}

// NO IN-GAME HOTKEY. A global key poll fires while the player is aiming, shooting
// or typing, and any key we pick (INSERT included) is one a player may have bound
// to something else - it caused more trouble than it solved. The settings menu is
// reached from the MAIN MENU only. Removed on purpose; do not add it back.

}  // namespace

void settings_menu_start() {
    if (!g_settings_menu_config.enable) {
        logger::logf("settings_menu: disabled");
        return;
    }
    logger::logf("settings_menu: enabled (no hotkey, fov_unlock=%d, menu=%s)",
                 g_settings_menu_config.fov_unlock,
                 g_settings_menu_config.menu_name);
}

// Called from apply_to_cgame() on every cgame (re)load, before CG_Init proceeds.
void settings_menu_apply_to_cgame(HMODULE cgame) {
    if (!g_settings_menu_config.enable || !g_settings_menu_config.fov_unlock || !cgame) return;
    const uintptr_t base = (uintptr_t)cgame;

    static bool logged = false;
    const bool f = poke_dword(base + CGAME_FOV_CVAR_FLAGS_RVA, 0x00000201, 0x00000001);
    const bool c = poke_byte (base + CGAME_FOV_MINCLAMP_JP_RVA, 0x7a, 0xeb);
    if (!logged && (f || c)) {
        logged = true;
        logger::logf("settings_menu: fov unlock applied (cvar-flags=%d min-clamp=%d)", f, c);
    }
}

// Watcher-thread tick (~200 Hz). Idempotent; self-throttled where it matters.
void settings_menu_tick() {
    if (!g_settings_menu_config.enable || !engine_ready()) return;

    // (1) register the menu cvars once, seeded from the current (ini) state. Non-archive:
    // the .ini is the persistent home, so we re-seed from it each launch and never let
    // config_mp.cfg shadow these.
    //   cod1x_viewmode : 0 = classic zoomed, 1 = widescreen, 2 = stretched
    //   cod1x_aspect   : "auto"/"4:3"/"16:9"/"16:10"/"21:9" or a number like "1.6"
    static bool registered = false;
    static int  last_viewmode = -1;
    static char last_aspect[24] = {0};
    static char last_refresh[16] = {0};
    if (!registered) {
        char abuf[24];
        widescreen_get_aspect(abuf, sizeof(abuf));
        char vbuf[4];
        snprintf(vbuf, sizeof(vbuf), "%d", config_to_viewmode());
        Cvar_Get("cod1x_viewmode", vbuf, 0);
        Cvar_Get("cod1x_aspect", abuf, 0);
        Cvar_Get("cod1x_refresh", g_settings_menu_config.refresh_rate, 0);
        Cvar_Get("cod1x_refresh_hz", "DEFAULT", 0);   // menu readout: resolved target Hz
        Cvar_Get("cod1x_files_scan", "0", 0);          // FILES page: rescan request
        Cvar_Get("cod1x_files_save", "0", 0);          // FILES page: save-as request
        Cvar_Get("cod1x_savename", "myconfig", 0);     // FILES page: save-as name
        publish_files();                               // initial CONFIGS/DEMOS slots
        last_viewmode = config_to_viewmode();
        snprintf(last_aspect, sizeof(last_aspect), "%s", abuf);
        snprintf(last_refresh, sizeof(last_refresh), "%s", g_settings_menu_config.refresh_rate);
        if (_stricmp(last_refresh, "auto") != 0)
            apply_refresh(last_refresh);   // archived seta: holds for every later vid init
        registered = true;
        logger::logf("settings_menu: registered cvars (cod1x_viewmode=%d cod1x_aspect=%s cod1x_refresh=%s)",
                     last_viewmode, abuf, last_refresh);
    }

    // (2) keep cg_fov's live CHEAT flag cleared — backup for the case where CG_RegisterCvars
    // ran before our static-table patch landed (cvar already registered with the flag).
    if (g_settings_menu_config.fov_unlock) {
        void* cv = Cvar_FindVar("cg_fov");
        if (cv) {
            int* flags = (int*)((char*)cv + CVAR_OFF_FLAGS);
            if (*flags & CVAR_FLAG_CHEAT) {
                *flags &= ~CVAR_FLAG_CHEAT;
                static bool once = false;
                if (!once) { once = true; logger::logf("settings_menu: cleared cg_fov CHEAT flag (live)"); }
            }
        }
    }

    // (3a) view mode (classic / widescreen / stretched). Applies live (the Hor+ hook and the
    // stretch operands are read every frame) and mirrors to the .ini.
    void* wcv = Cvar_FindVar("cod1x_viewmode");
    if (wcv) {
        const int v = *(int*)((char*)wcv + CVAR_OFF_INTEGER);
        if (v != last_viewmode) {
            const char* vm = viewmode_apply(v);
            ini_writeback("view_mode", vm);
            logger::logf("settings_menu: view mode -> %s (live + ini)", vm);
            last_viewmode = v;
        }
    }

    // (3b) aspect ratio. The menu now uses a click-to-cycle MULTI (complete values only),
    // so the debounce is just a short settle window; it still guards console edits.
    void* acv = Cvar_FindVar("cod1x_aspect");
    if (acv) {
        const char* s = *(const char**)((char*)acv + CVAR_OFF_STRING);
        if (s && strcmp(s, last_aspect) != 0) {
            static char  pending[24] = {0};
            static DWORD pending_since = 0;
            if (strcmp(s, pending) != 0) {
                snprintf(pending, sizeof(pending), "%s", s);
                pending_since = GetTickCount();
            } else if (GetTickCount() - pending_since >= 150) {
                widescreen_set_aspect(s);
                ini_writeback("aspect_ratio", s);
                logger::logf("settings_menu: aspect ratio -> %s (live + ini)", s);
                snprintf(last_aspect, sizeof(last_aspect), "%s", s);
            }
        }
    }

    // (3b2) refresh rate. MULTI presets ("auto"/"max"/"144"...); short settle window, then
    // resolve + seta r_displayRefresh. The menu's APPLY VIDEO button runs vid_restart.
    void* rcv = Cvar_FindVar("cod1x_refresh");
    if (rcv) {
        const char* s = *(const char**)((char*)rcv + CVAR_OFF_STRING);
        if (s && strcmp(s, last_refresh) != 0) {
            static char  rpending[16] = {0};
            static DWORD rpending_since = 0;
            if (strcmp(s, rpending) != 0) {
                snprintf(rpending, sizeof(rpending), "%s", s);
                rpending_since = GetTickCount();
            } else if (GetTickCount() - rpending_since >= 150) {
                apply_refresh(s);
                ini_writeback("refresh_rate", s);
                snprintf(last_refresh, sizeof(last_refresh), "%s", s);
            }
        }
    }

    // (3b3) FILES page requests: rescan (REFRESH button / page onOpen) and save-as.
    {
        void* scv = Cvar_FindVar("cod1x_files_scan");
        if (scv && *(int*)((char*)scv + CVAR_OFF_INTEGER) != 0)
            publish_files();                       // batch ends with set cod1x_files_scan 0
        void* sav = Cvar_FindVar("cod1x_files_save");
        if (sav && *(int*)((char*)sav + CVAR_OFF_INTEGER) != 0)
            do_save_config();                      // resets the flag, then triggers a rescan
    }

    // (3c) while the menu is open, keep cg_fov an integer with a clean string. The engine
    // slider writes the cvar as "%f" ("95.000000"); the menu's live value readout paints the
    // raw cvar string, so we re-set it to a clean integer via Cbuf (main-thread, same channel
    // as the hotkey). Only while the menu is open — console users keep fractional FOV.
    void* mcv = Cvar_FindVar("cod1x_menu_open");
    if (mcv && *(int*)((char*)mcv + CVAR_OFF_INTEGER) != 0) {
        void* fcv = Cvar_FindVar("cg_fov");
        if (fcv) {
            const char* fs = *(const char**)((char*)fcv + CVAR_OFF_STRING);
            if (fs && strchr(fs, '.')) {
                static DWORD last_snap = 0;
                const DWORD now = GetTickCount();
                if (now - last_snap >= 50) {
                    last_snap = now;
                    const float fv = *(float*)((char*)fcv + CVAR_OFF_VALUE);
                    char cmd[48];
                    snprintf(cmd, sizeof(cmd), "set cg_fov %d\n", (int)(fv + 0.5f));
                    Cbuf_ExecuteText(EXEC_APPEND, cmd);
                }
            }
        }
    }

    // (4) hotkey open/close
}

}  // namespace patches
