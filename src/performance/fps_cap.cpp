// timeBeginPeriod(1): default 15.6ms timer clamps Sleep() so com_maxfps caps low/erratic.

#include "performance/fps_cap.h"
#include "core/logger.h"

#include <timeapi.h>  // winmm.lib

namespace patches {

FpsCapConfig g_fps_cap_config = {
    /* force_1ms_timer */ true,
};

namespace {
bool g_period_set = false;
}  // namespace

void fps_cap_init() {
    if (!g_fps_cap_config.force_1ms_timer) {
        logger::logf("fps_cap: force_1ms_timer disabled, skipping");
        return;
    }

    TIMECAPS tc;
    if (timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR) {
        logger::logf("fps_cap: timer resolution range [%u, %u] ms",
                     tc.wPeriodMin, tc.wPeriodMax);
    }

    MMRESULT r = timeBeginPeriod(1);
    if (r == TIMERR_NOERROR) {
        g_period_set = true;
        logger::logf("fps_cap: timer resolution forced to 1ms "
                     "(com_maxfps should now match actual FPS)");
    } else {
        logger::logf("fps_cap: timeBeginPeriod(1) failed (err=%u)", r);
    }
}

void fps_cap_shutdown() {
    if (g_period_set) {
        timeEndPeriod(1);
        g_period_set = false;
        logger::logf("fps_cap: timer resolution restored");
    }
}

}  // namespace patches
