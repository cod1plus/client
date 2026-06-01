#ifndef COD1RELOADED_CPU_AFFINITY_H
#define COD1RELOADED_CPU_AFFINITY_H

#include <windows.h>

namespace patches {

struct CpuAffinityConfig {
    // Number of logical cores the game may run on (0 = OS default = all).
    // Locking to few cores reduces CPU migration -> fewer microstutters.
    // Typical:
    //   2 = great for CoD1 (single-threaded engine + audio thread)
    //   4 = safer if mods spawn extra threads
    int cores_count;
    // Index of the first core to use. 0 = core 0.
    // On Intel hybrid CPUs (Alder Lake +) keep at 0 to land on a P-core.
    int first_core;
};

extern CpuAffinityConfig g_cpu_affinity_config;

// Apply the process affinity mask. Idempotent.
void cpu_affinity_apply();

}  // namespace patches

#endif
