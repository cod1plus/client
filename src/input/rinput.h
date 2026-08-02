#ifndef COD1RELOADED_RINPUT_H
#define COD1RELOADED_RINPUT_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// Raw mouse input (port of cod2x's m_rinput). The engine reads the mouse through
// GetCursorPos + a re-centre, which goes through the Windows pointer pipeline:
// pointer ballistics ("enhance pointer precision"), the desktop DPI/speed slider and
// the 1-pixel quantisation of the cursor position all sit between the sensor and the
// view angle. WM_INPUT delivers the device counts themselves - no acceleration, no
// clamping to the desktop, and full resolution at 1000 Hz polling.
struct RInputConfig {
    // Default for the m_rinput cvar. The cvar is archived, so a player who has set it
    // once keeps his choice and this only decides what a fresh install gets.
    bool enable_default = false;
};

extern RInputConfig g_rinput_config;

// CoDMP.exe, image base 0x400000. win_input.c equivalents, RE'd 2026-08-02:
//   IN_GetMouseDelta @0x00466a00  GetCursorPos -> *outX/*outY (menu cursor path)
//   IN_MouseMove     @0x00466b70  GetCursorPos -> Sys_QueEvent(SE_MOUSE) (in-game path)
// Both are byte-identical up to the subtraction and are the ONLY two callers of
// GetCursorPos in the whole executable (verified: 2 xrefs). Both do
//     delta = GetCursorPos() - centre ; SetCursorPos(centre)
// so a GetCursorPos that answers "centre + raw delta" makes both produce the raw
// delta, whatever the centre holds - we add exactly what they subtract.
constexpr uintptr_t CODMP_MOUSE_CENTER_X_VA = 0x0093b398;
constexpr uintptr_t CODMP_MOUSE_CENTER_Y_VA = 0x0093b228;

void rinput_start();      // DllMain: hook GetCursorPos (inert until m_rinput is on)
void rinput_tick();       // watcher thread: follow m_rinput, publish m_rinput_hz
void rinput_shutdown();   // DLL_PROCESS_DETACH: stop the thread, unregister the device

}  // namespace patches

#endif
