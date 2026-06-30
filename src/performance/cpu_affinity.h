#ifndef COD1RELOADED_CPU_AFFINITY_H
#define COD1RELOADED_CPU_AFFINITY_H

#include <windows.h>

namespace patches {

struct CpuAffinityConfig {
    int cores_count;   // 0 = disabled
    int first_core;    // keep 0 on Intel hybrid -> P-core
};

extern CpuAffinityConfig g_cpu_affinity_config;

void cpu_affinity_apply();

}  // namespace patches

#endif
