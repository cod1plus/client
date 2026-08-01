#ifndef COD1RELOADED_SETTINGS_MENU_H
#define COD1RELOADED_SETTINGS_MENU_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// In-game "1.6X SETTINGS" menu bridge: registers cvars the ui_mp/cod1x_settings.menu
// binds to (FOV + screen ratio), unlocks cg_fov, opens the menu on a hotkey, and
// mirrors live changes back into cod1reloaded.ini.
struct SettingsMenuConfig {
    bool enable       = true;
    int  toggle_key   = 0;      // 0 = no hotkey. The menu is reached from the main
                                // menu; an in-game hotkey fired during play (and
                                // INSERT is bindable by players). Set menu_key in
                                // the .ini to a VK code to re-enable one.
    bool fov_unlock   = true;   // patch cgame so cg_fov isn't cheat-locked/clamped to 80
    char menu_name[48] = "cod1x_settings";
    char menu_file[96] = "ui_mp/cod1x_settings.menu";
    // "auto" (engine/desktop default), "max" (highest Hz the display supports at the game's
    // resolution), or a number ("144"). Applied via seta r_displayRefresh + vid_restart.
    char refresh_rate[16] = "max";
};

extern SettingsMenuConfig g_settings_menu_config;

// cgame_mp_x86.dll RVAs (RE'd 2026-07-05, see memory cod1-fov-cvar-clamp).
//   FLAGS    : cg_fov cvarTable flags dword 0x00000201 (ARCHIVE|CHEAT) -> 0x00000001 (drop CHEAT)
//   MINCLAMP : CG_GetEffectiveFov min-80 branch 'jp +0x0a' (7A 0A) -> 'jmp +0x0a' (EB 0A)
constexpr uintptr_t CGAME_FOV_CVAR_FLAGS_RVA  = 0x000769ec;
constexpr uintptr_t CGAME_FOV_MINCLAMP_JP_RVA = 0x000344e6;

// CoDMP.exe (base 0x400000) engine fns not already declared in other headers.
constexpr uintptr_t CODMP_CVAR_FINDVAR_VA  = 0x0043b790; // cvar_t* Cvar_FindVar(name)
constexpr uintptr_t CODMP_CBUF_EXECTEXT_VA = 0x0042a180; // void Cbuf_ExecuteText(int when, const char* text)

void settings_menu_start();                        // DllMain: log enable state
void settings_menu_apply_to_cgame(HMODULE cgame);  // FOV unlock (idempotent, safe to re-run)
void settings_menu_tick();                         // watcher thread: register cvars, poll key, poll ratio

}  // namespace patches

#endif
