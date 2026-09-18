// See mode_guard.h. IAT hook on CoDMP.exe's ChangeDisplaySettingsA.

#include "video/mode_guard.h"
#include "core/iat.h"
#include "core/logger.h"

#include <windows.h>
#include <cstring>

namespace patches {

namespace {

typedef LONG (WINAPI *ChangeDisplaySettingsA_t)(DEVMODEA*, DWORD);
ChangeDisplaySettingsA_t g_prev = nullptr;   // next in the IAT chain (updater's gate)
DEVMODEA      g_fs_mode;                     // last CDS_FULLSCREEN mode that stuck
volatile bool g_fs_mode_valid  = false;      // see mode_guard_exclusive_active()

void remember_fullscreen(const DEVMODEA* dm, DWORD flags) {
    if (!(flags & CDS_FULLSCREEN)) return;
    memcpy(&g_fs_mode, dm, sizeof(g_fs_mode));
    g_fs_mode_valid = true;
}

const char* cds_result_name(LONG rc) {
    switch (rc) {
        case DISP_CHANGE_SUCCESSFUL:  return "SUCCESSFUL";
        case DISP_CHANGE_RESTART:     return "RESTART";
        case DISP_CHANGE_FAILED:      return "FAILED";
        case DISP_CHANGE_BADMODE:     return "BADMODE";
        case DISP_CHANGE_NOTUPDATED:  return "NOTUPDATED";
        case DISP_CHANGE_BADFLAGS:    return "BADFLAGS";
        case DISP_CHANGE_BADPARAM:    return "BADPARAM";
        case DISP_CHANGE_BADDUALVIEW: return "BADDUALVIEW";
        default:                      return "?";
    }
}

LONG WINAPI hk_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    // Restore-to-desktop call: nothing to fix, but its timing and result are the
    // heart of the alt-tab timeline, so it is always logged.
    if (!dm) {
        g_fs_mode_valid = false;   // the engine is LEAVING fullscreen on purpose
        LONG rc = g_prev(NULL, flags);
        logger::logf("mode_guard: restore desktop (flags=0x%lx) -> %s",
                     (unsigned long)flags, cds_result_name(rc));
        return rc;
    }

    LONG rc = g_prev(dm, flags);
    if (rc == DISP_CHANGE_SUCCESSFUL) remember_fullscreen(dm, flags);
    logger::logf("mode_guard: set %lux%lu %lubpp %luHz fields=0x%lx flags=0x%lx -> %s",
                 (unsigned long)dm->dmPelsWidth, (unsigned long)dm->dmPelsHeight,
                 (unsigned long)dm->dmBitsPerPel, (unsigned long)dm->dmDisplayFrequency,
                 (unsigned long)dm->dmFields, (unsigned long)flags, cds_result_name(rc));
    if (rc == DISP_CHANGE_SUCCESSFUL) return rc;

    // The engine treats this failure as fatal (white console + self-relaunch), so
    // any mode we can still salvage is strictly better than the truth. Retry on a
    // COPY - the engine reuses its DEVMODE on the next restore and must never see
    // our edits.
    DEVMODEA fix;
    memcpy(&fix, dm, sizeof(fix));

    // First choice: the HIGHEST refresh the display actually lists at this
    // resolution (whatever was asked: a refused rate is not one the player can have). resolve_max_hz deliberately returns the panel's max across ALL
    // modes (a 200 Hz panel means 200), so at resolutions where that rate is not
    // offered the set comes in here as BADMODE - and stripping the frequency
    // outright lands on the mode DEFAULT, usually 60 Hz (enzo's log 00:35:13:
    // 1440x1080@200 BADMODE -> stripped -> 60). Downgrade to the best real rate
    // instead, and only then give the frequency up entirely.
    if ((fix.dmFields & DM_DISPLAYFREQUENCY) && fix.dmDisplayFrequency > 1) {
        DWORD best = 0;
        DEVMODEA e;
        memset(&e, 0, sizeof(e));
        e.dmSize = sizeof(e);
        for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &e); ++i) {
            if (e.dmPelsWidth != fix.dmPelsWidth)   continue;
            if (e.dmPelsHeight != fix.dmPelsHeight) continue;
            if (e.dmBitsPerPel < 32)                continue;
            // the HIGHEST rate listed - not the highest below the request: the request
            // was not a real mode (enzo's 200 = the engine's old clamp, 2026-09-17), and
            // "below" turned a 320 Hz panel into 180 Hz
            if (e.dmDisplayFrequency > best)
                best = e.dmDisplayFrequency;
        }
        if (best > 1) {
            fix.dmDisplayFrequency = best;
            rc = g_prev(&fix, flags);
            logger::logf("mode_guard: RETRY at %lu Hz (best listed at %lux%lu) -> %s",
                         (unsigned long)best, (unsigned long)fix.dmPelsWidth,
                         (unsigned long)fix.dmPelsHeight, cds_result_name(rc));
            if (rc == DISP_CHANGE_SUCCESSFUL) {
                remember_fullscreen(&fix, flags);
                return rc;
            }
        }
    }

    if (fix.dmFields & DM_DISPLAYFREQUENCY) {
        fix.dmFields &= ~(DWORD)DM_DISPLAYFREQUENCY;
        fix.dmDisplayFrequency = 0;
        rc = g_prev(&fix, flags);
        logger::logf("mode_guard: RETRY without forced refresh -> %s", cds_result_name(rc));
        if (rc == DISP_CHANGE_SUCCESSFUL) {
            remember_fullscreen(&fix, flags);
            return rc;
        }
    }

    if (fix.dmFields & DM_BITSPERPEL) {
        fix.dmFields &= ~(DWORD)DM_BITSPERPEL;
        rc = g_prev(&fix, flags);
        logger::logf("mode_guard: RETRY without color depth -> %s", cds_result_name(rc));
        if (rc == DISP_CHANGE_SUCCESSFUL) {
            remember_fullscreen(&fix, flags);
            return rc;
        }
    }

    logger::logf("mode_guard: all retries failed - engine will take its error path");
    return rc;
}

}  // namespace

bool mode_guard_exclusive_active() {
    return g_fs_mode_valid;
}

bool mode_guard_reapply_fullscreen() {
    if (!g_fs_mode_valid || !g_prev) return false;
    DEVMODEA cur;
    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &cur) &&
        cur.dmPelsWidth == g_fs_mode.dmPelsWidth &&
        cur.dmPelsHeight == g_fs_mode.dmPelsHeight) {
        return false;   // desktop already runs the game's mode
    }
    DEVMODEA want;
    memcpy(&want, &g_fs_mode, sizeof(want));
    LONG rc = g_prev(&want, CDS_FULLSCREEN);
    logger::logf("mode_guard: re-applying fullscreen %lux%lu on activate -> %s",
                 (unsigned long)want.dmPelsWidth, (unsigned long)want.dmPelsHeight,
                 cds_result_name(rc));
    return rc == DISP_CHANGE_SUCCESSFUL;
}

bool mode_guard_fullscreen_size(int* w, int* h) {
    if (!g_fs_mode_valid) return false;
    *w = (int)g_fs_mode.dmPelsWidth;
    *h = (int)g_fs_mode.dmPelsHeight;
    return true;
}

void mode_guard_start() {
    void* prev = iat_hook("user32.dll", "ChangeDisplaySettingsA",
                          (void*)hk_ChangeDisplaySettingsA);
    if (!prev) {
        logger::logf("mode_guard: IAT hook FAILED - display mode calls not guarded");
        return;
    }
    g_prev = (ChangeDisplaySettingsA_t)prev;
    logger::logf("mode_guard: ChangeDisplaySettingsA guarded (prev=%p)", prev);
}

}  // namespace patches
