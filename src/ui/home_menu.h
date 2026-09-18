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

// Polled every frame while hidden: true once the engine's "main" menuDef fires its
// onOpen (cvar cod1x_home_active, wired into the pk3's ui_mp/main.menu override).
// Level-triggered - stays true for as long as the engine considers "main" the active
// top menu, so gl_overlay can also use it to detect an EXTERNAL close (ESC handled by
// the engine itself, cl_ingame changing, etc.) and drop the overlay in step.
bool home_menu_is_active();

// Forwards the original main.menu onESC engine commands and clears the bridge cvar -
// called when the player presses ESC while the home screen is showing. Does not touch
// overlay visibility itself; the caller (gl_overlay) still has to drop the mode.
void home_menu_close_via_escape();

}  // namespace patches

#endif
