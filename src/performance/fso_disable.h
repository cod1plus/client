#ifndef COD1RELOADED_FSO_DISABLE_H
#define COD1RELOADED_FSO_DISABLE_H

#include <windows.h>

namespace patches {

struct FsoDisableConfig {
    bool enable;
};

extern FsoDisableConfig g_fso_disable_config;

void fso_disable_apply();

}  // namespace patches

#endif
