#ifndef COD1RELOADED_PATCHES_H
#define COD1RELOADED_PATCHES_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// Charge cod1reloaded.ini depuis le meme dossier que la DLL hote.
// Les valeurs manquantes conservent leurs defaults.
void load_config(HMODULE self_module);

}  // namespace patches

#endif
