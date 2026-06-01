// Fix du cap FPS pour CoD1.
//
// Probleme observe : avec com_maxfps=250 le jeu plafonne plutot a 230-244,
// avec des fluctuations erratiques. La cause classique est la resolution
// du timer Windows : par defaut 15.625ms, ce qui empeche Sleep() / les
// timers ms-precision (Sys_Milliseconds, timeGetTime) d'etre exacts a 1ms.
//
// Fix : timeBeginPeriod(1) force le scheduler Windows a une periode de
// 1ms pour TOUT le process. Une fois actif, le moteur peut effectivement
// dormir 4ms (= 250 FPS) au lieu d'etre force a 15.6ms.
//
// Note : timeBeginPeriod augmente legerement la conso CPU/batterie du
// systeme entier tant qu'il est actif. C'est acceptable pour un jeu en
// foreground ; on appelle timeEndPeriod a la sortie pour rester propre.

#include "fps_cap.h"
#include "logger.h"

// timeBeginPeriod est dans winmm.lib (lie via CMake).
#include <timeapi.h>

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

    // Verifie la resolution actuelle pour info.
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
