#ifndef COD1RELOADED_WORKING_SET_H
#define COD1RELOADED_WORKING_SET_H

#include <windows.h>
#include <stddef.h>

namespace patches {

struct WorkingSetConfig {
    bool enable;
    // Minimum bytes the OS must keep in physical memory for CoD1.
    // Default: 128 MB - enough for the game code + most loaded textures.
    size_t min_mb;
    // Maximum bytes the OS may give to the process. Allows growth.
    size_t max_mb;
};

extern WorkingSetConfig g_working_set_config;

// Apply working set limits. Idempotent.
void working_set_apply();

}  // namespace patches

#endif
