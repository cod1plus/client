#ifndef COD1RELOADED_WINDOW_PATCH_H
#define COD1RELOADED_WINDOW_PATCH_H

#include <windows.h>

namespace patches {

// Configuration window mode.
struct WindowConfig {
    bool borderless_enable;       // active le mode borderless windowed
    bool follow_current_monitor;  // si true : utilise le monitor ou est la
                                  // fenetre. Sinon : monitor primaire.
    int  preferred_monitor_index; // -1 = current/primary, 0/1/2... = index

    // Quand true, le jeu se minimise automatiquement quand il perd le focus.
    // Necessaire en borderless fullscreen : sans ca, la fenetre couvre tout
    // l'ecran et tu ne peux pas cliquer ailleurs.
    bool minimize_on_focus_loss;
};

extern WindowConfig g_window_config;

// Demarre le watcher qui detecte la fenetre CoDMP et applique le borderless.
// Non bloquant : spawn un thread.
void start_window_watcher();

// Returns the game's top-level HWND if known (subclassed by window_patch).
// nullptr until the watcher has located it (a few seconds after startup).
HWND get_game_window();

}  // namespace patches

#endif
