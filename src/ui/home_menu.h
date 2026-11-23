#ifndef COD1RELOADED_HOME_MENU_H
#define COD1RELOADED_HOME_MENU_H

namespace patches {

enum class HomeAction {
    None,          // stay on the home screen
    OpenSettings,  // caller should switch to the settings panel, home stays "underneath"
    Navigated,     // a button already forwarded its engine command and cleared the
                   // bridge cvar - caller should just stop drawing (mode -> hidden)
};

// The native replacement for ui_mp/main.menu's top screen. One call per frame while
// shown. Every action it offers forwards the EXACT command sequence the original
// engine buttons used (read out of main.menu, see home_menu.cpp) - deeper screens
// (server browser, quit confirmation) stay the engine's own, untouched.
HomeAction home_menu_draw(float screen_w, float screen_h);

// Polled every frame: true while "main" is the engine's top menu (ui/menu_hooks.cpp)
// and no game is loaded. Level-triggered, so gl_overlay drops the home screen in
// step the moment the engine opens anything else (server browser, connect screen).
bool home_menu_is_active();

// ESC on the home screen: toggles the quit confirmation (the engine's own "main"
// ignores ESC too). Returns true (always handled).
bool home_menu_escape();
void home_menu_close_via_escape();   // drops the confirmation, if any

}  // namespace patches

#endif
