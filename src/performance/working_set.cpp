// keep pages resident to avoid page-fault hitches after alt-tab. min>0 is a hint, not a guarantee.

#include "performance/working_set.h"
#include "core/logger.h"

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
        logger::logf("working_set: SetProcessWorkingSetSize failed (err=%lu) - skipping",
                     GetLastError());
        return;
    }

    logger::logf("working_set: min=%llu MB, max=%llu MB",
                 (unsigned long long)g_working_set_config.min_mb,
                 (unsigned long long)g_working_set_config.max_mb);
}

}  // namespace patches
