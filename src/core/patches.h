#ifndef COD1RELOADED_PATCHES_H
#define COD1RELOADED_PATCHES_H

#include <windows.h>
#include <stdint.h>

namespace patches {

void load_config(HMODULE self_module);

// hot-reload lean knobs only; caller throttles
void hot_reload_lean_reshape();

}  // namespace patches

#endif
