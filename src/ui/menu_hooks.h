#ifndef COD1RELOADED_MENU_HOOKS_H
#define COD1RELOADED_MENU_HOOKS_H
// Which engine menu is on top? Three entry hooks in ui_mp_x86.dll (the whole .menu
// system lives there, not in the exe) tell us, without any pk3 override:
//     Menus_ActivateByName(eax = name)   rva 0x14010   every `open X` and UI_SetActiveMenu
//     Menus_CloseByName(eax = name)      rva 0x10e60   every `close X`
//     Menus_CloseAll()                   rva 0x10e80   UIMENU_NONE (connecting, loading)
// (name in eax: MSVC register-arg optimisation; ui_mp_x86.dll md5 90274612 is the
// same file in every 1.5/1.6X install.) The DLL is reloaded on vid_restart, so the
// hooks are re-installed by the watcher whenever the entry bytes are not ours.
//
// This is what lets the native home screen (home_menu.cpp) replace the engine's
// "main" exactly while "main" is the active menu - and get out of the way the moment
// the engine opens anything else (server browser, popups, connect screen).
#include <windows.h>

namespace patches {

void menu_hooks_tick();                 // watcher thread: (re)install when the UI DLL is up
bool ui_main_menu_active();             // "main" is the engine's top menu right now
const char* ui_menu_top();              // its name ("" when none / unknown)

}  // namespace patches

#endif
