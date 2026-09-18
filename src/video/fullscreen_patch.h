#ifndef COD1RELOADED_FULLSCREEN_PATCH_H
#define COD1RELOADED_FULLSCREEN_PATCH_H

#include <windows.h>
#include <stdint.h>

namespace patches {

// CoDMP.exe RVAs, image base 0x00400000. Cvar_Get(fcn.0043b880) seq @0x0046c254:
//   push 0x21 / push data.005a7c54 ("1\0..") / push str.r_fullscreen / call
constexpr uintptr_t CODMP_RFULLSCREEN_PUSH_OPCODE_RVA  = 0x0006c256; // opcode 0x68
constexpr uintptr_t CODMP_RFULLSCREEN_PUSH_OPERAND_RVA = 0x0006c257; // 4 bytes to patch
constexpr uintptr_t CODMP_RFULLSCREEN_ORIGINAL_PTR     = 0x005a7c54; // ptr to "1\0"

struct FullscreenConfig {
    // gotcha: CVAR_ARCHIVE seta in config_mp.cfg overrides this default
    bool force_windowed_default;
    // true when cod1reloaded.ini spells out `fullscreen = on/off`: then the .ini is the
    // player's decision and display_probe rewrites a disagreeing seta r_fullscreen in
    // config_mp.cfg (see enforce_ini_fullscreen). false = key absent, config wins.
    bool ini_key_present;
};

extern FullscreenConfig g_fullscreen_config;

// idempotent
bool apply_fullscreen_patch();

}  // namespace patches

#endif
