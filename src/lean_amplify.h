#ifndef COD1RELOADED_LEAN_AMPLIFY_H
#define COD1RELOADED_LEAN_AMPLIFY_H

#include <windows.h>
#include <stdint.h>

// Amplification de l'effet de lean (camera shift + body lean).
//
// En CoD1 (comme en CoD2), le shift lateral de la camera/du body pendant
// un lean est calcule par la fonction AddLeanToPosition :
//   pos += AngleVectors(yaw).right * ((2.0 - |leanFrac|) * leanFrac * a5)
// avec a5 = 20.0 et a4 = 16.0 (constantes baked-in).
//
// Cod2x utilise les memes valeurs (20.0 / 16.0). Pour rendre le lean
// "plus prononce" (= camera/body decalent plus quand on penche), on
// remplace ces immediates push imm32 dans les 5 call sites de cgame.
//
// AVERTISSEMENT : ces constantes sont aussi utilisees par les call sites
// de prediction client-side. Trop augmenter peut casser la collision
// (e.g. traverser des murs). Garder factor a 1.5x ou moins par defaut.

namespace patches {

// RVAs des 5 sites identifies par scan du DLL (push 20.0 + push 16.0
// dans une fenetre de 30 bytes, signature exacte verifiee).
//
// File offset == RVA pour cette DLL (verifie via swing_fix RVA 0x451d).
//
// Chaque site appelle AddLeanToPosition(pos, yaw, leanFrac, 16.0, 20.0).
// L'ordre push cdecl est : push 20.0 (a5, dernier arg) puis push 16.0.

struct LeanSite {
    uintptr_t push20_rva;   // address de l'instruction `push 20.0f`
    uintptr_t push16_rva;   // address de l'instruction `push 16.0f`
};

constexpr LeanSite kLeanSites[5] = {
    { 0xbaea,  0xbaf5  },  // site 1
    { 0x1479d, 0x147b3 },  // site 2
    { 0x34473, 0x34478 },  // site 3 (pushes adjacents)
    { 0x38776, 0x3878c },  // site 4
    { 0x3abc9, 0x3abdd },  // site 5
};

struct LeanAmplifyConfig {
    bool  enable;
    // Multiplicateur du shift lateral max.
    //   1.0  = vanilla (no change, push 20.0 stays 20.0)
    //   1.3  = subtil (push 20.0 -> 26.0, +30% de shift)
    //   1.5  = visible (push 20.0 -> 30.0, +50%)
    //   2.0  = exagere (push 20.0 -> 40.0, +100%) - risque collision
    float factor;
};

extern LeanAmplifyConfig g_lean_amplify_config;

// Patche les 5 sites. Idempotent. Returns true si tout OK.
bool apply_lean_amplify(HMODULE cgame_module);

}  // namespace patches

#endif
