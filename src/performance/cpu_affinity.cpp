// pin to a few cores: engine is single-threaded, core migration trashes L1/L2

#include "performance/cpu_affinity.h"
#include "core/logger.h"

namespace patches {

CpuAffinityConfig g_cpu_affinity_config = {
    /* cores_count */ 2,
    /* first_core  */ 0,
};

void cpu_affinity_apply() {
    if (g_cpu_affinity_config.cores_count <= 0) {
        logger::logf("cpu_affinity: disabled (cores_count=0)");
        return;
    }

    DWORD_PTR mask = 0;
    const int first = g_cpu_affinity_config.first_core;
    const int count = g_cpu_affinity_config.cores_count;
    for (int i = 0; i < count; ++i) {
        const int core = first + i;
        if (core >= 64) break; // mask is 64 bits
        mask |= (DWORD_PTR)1 << core;
    }

    DWORD_PTR sys_mask = 0, proc_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask)) {
        const DWORD_PTR intersected = mask & sys_mask;
        if (intersected == 0) {
            logger::logf(
                "cpu_affinity: requested mask 0x%llx has no overlap with "
                "system mask 0x%llx - aborting",
                (unsigned long long)mask, (unsigned long long)sys_mask);
            return;
        }
        mask = intersected;
    }

    if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        logger::logf("cpu_affinity: SetProcessAffinityMask failed (err=%lu)",
                     GetLastError());
        return;
    }

    int set_cores = 0;
    for (DWORD_PTR m = mask; m; m >>= 1) {
        if (m & 1) ++set_cores;
    }
    logger::logf("cpu_affinity: pinned to %d core%s (mask 0x%llx)",
                 set_cores, set_cores == 1 ? "" : "s",
                 (unsigned long long)mask);
}

}  // namespace patches
