// CPU affinity pinning for CoD1.
//
// Background: id Tech 3 is single-threaded for game logic. On modern CPUs
// the OS scheduler keeps migrating the busy thread between cores to balance
// thermals and load. Each migration invalidates the L1/L2 cache, which
// produces visible microstutters during fast gameplay.
//
// Fix: pin the process to a small subset of cores. The thread stays put,
// the cache stays warm, microstutters disappear.
//
// On hybrid CPUs (Intel 12th gen +) starting at core 0 lands on a P-core
// which is typically 1.5-2x faster than an E-core. Critical for CoD1.

#include "cpu_affinity.h"
#include "logger.h"

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

    // Build the mask for the requested core range.
    DWORD_PTR mask = 0;
    const int first = g_cpu_affinity_config.first_core;
    const int count = g_cpu_affinity_config.cores_count;
    for (int i = 0; i < count; ++i) {
        const int core = first + i;
        if (core >= 64) break; // DWORD_PTR is 32/64 bits max
        mask |= (DWORD_PTR)1 << core;
    }

    // Check against the system's actual capability.
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

    // Count bits for logging
    int set_cores = 0;
    for (DWORD_PTR m = mask; m; m >>= 1) {
        if (m & 1) ++set_cores;
    }
    logger::logf("cpu_affinity: pinned to %d core%s (mask 0x%llx)",
                 set_cores, set_cores == 1 ? "" : "s",
                 (unsigned long long)mask);
}

}  // namespace patches
