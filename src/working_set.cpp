// Working Set tuning for CoD1.
//
// "Working Set" = the subset of process memory currently in physical RAM.
// When Windows is under memory pressure, it swaps pages out to disk
// (the pagefile). Next access to those pages causes a hard page fault
// = a *huge* hitch (10-50ms freeze).
//
// CoD1 is small (< 200 MB) so we can comfortably ask Windows to keep the
// whole working set in RAM. Side effect: when you alt-tab out and back
// in, the game doesn't have to page-fault back into existence -> no
// "swap-in stutter" on return.
//
// SetProcessWorkingSetSize with min > 0 raises the minimum the OS will
// keep resident. It's a hint, not a guarantee, but on modern Windows
// with 16+ GB RAM it always holds.

#include "working_set.h"
#include "logger.h"

namespace patches {

WorkingSetConfig g_working_set_config = {
    /* enable  */ true,
    /* min_mb  */ 128,
    /* max_mb  */ 512,
};

void working_set_apply() {
    if (!g_working_set_config.enable) {
        logger::logf("working_set: disabled");
        return;
    }

    const SIZE_T min_bytes = (SIZE_T)g_working_set_config.min_mb * 1024 * 1024;
    const SIZE_T max_bytes = (SIZE_T)g_working_set_config.max_mb * 1024 * 1024;

    if (!SetProcessWorkingSetSize(GetCurrentProcess(), min_bytes, max_bytes)) {
        // Failure is non-fatal: the OS just keeps its default behavior.
        logger::logf("working_set: SetProcessWorkingSetSize failed (err=%lu) - skipping",
                     GetLastError());
        return;
    }

    logger::logf("working_set: min=%llu MB, max=%llu MB",
                 (unsigned long long)g_working_set_config.min_mb,
                 (unsigned long long)g_working_set_config.max_mb);
}

}  // namespace patches
