#ifndef COD1RELOADED_WORKING_SET_H
#define COD1RELOADED_WORKING_SET_H

#include <windows.h>
#include <stddef.h>

namespace patches {

struct WorkingSetConfig {
    bool enable;
    size_t min_mb;
    size_t max_mb;
};

extern WorkingSetConfig g_working_set_config;

void working_set_apply();

}  // namespace patches

#endif
