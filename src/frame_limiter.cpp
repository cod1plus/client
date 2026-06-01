// Frame limiter precis (microsecond-grade) pour CoD1.
//
// Bug original : avec com_maxfps=250 le moteur cap a 240-248 a cause de
// la math entiere en ms. Augmenter com_maxfps > 250 est detecte par
// PunkBuster -> ban. Donc on doit garder com_maxfps=250 pour PB mais faire
// que le LIMITER interne respecte vraiment 250 FPS.
//
// Strategie : on patche UNIQUEMENT le call site a 0x0043a4f5 (le call
// dans le spin-loop). On le redirige vers frame_wait_replacement() qui :
//   1. Spin-wait avec QueryPerformanceCounter jusqu'au deadline exact
//   2. Retourne une valeur ms qui satisfait la condition de la boucle
//      (elapsed >= target_ms)
//
// La fcn.00438a70 originale n'est PAS touchee. Elle est encore utilisee
// par 2 autres call sites dans d'autres contextes.

#include "frame_limiter.h"
#include "logger.h"
#include "version_patch.h" // CODMP_PREFERRED_BASE

#include <cstdio>

namespace patches {

FrameLimiterConfig g_frame_limiter_config = {
    /* enable           */ true,
    /* deadline_bias_us */ 0,
};

namespace {

LARGE_INTEGER g_qpc_freq = {0};
LONGLONG      g_last_frame_qpc = 0;
bool          g_applied = false;

// Lit le pointeur global du dvar com_maxfps puis son integer.
// Renvoie 0 si invalide. Marche meme avec ASLR.
int read_com_maxfps_dvar() {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;
    uintptr_t base = (uintptr_t)exe;
    void** slot = (void**)(base + (CODMP_COM_MAXFPS_DVAR_SLOT_VA - CODMP_PREFERRED_BASE));
    void* dvar = *slot;
    if (!dvar) return 0;
    return *(int*)((char*)dvar + CODMP_DVAR_INTEGER_OFFSET);
}

}  // namespace

// Note : on garde le typedef au cas ou on veuille re-essayer une
// approche basee sur l'appel a l'original. Pas utilise actuellement.
typedef int (__cdecl *sys_ms_fn)();
static sys_ms_fn g_orig_sys_ms = nullptr;

// Notre remplacant pour fcn.00438a70 (le 'call' dans le spin-loop).
// Convention : __cdecl, no args, retourne int en eax.
//
// Flow :
//   1. Spin-wait QPC jusqu'au deadline exact (1000000/maxfps us depuis
//      le dernier appel).
//   2. Appel a l'original fcn.00438a70 -> retourne le ms attendu par le
//      moteur. Comme ca le diff (return - prev) est >= target, le moteur
//      sort de la boucle, ET le frametime physique downstream est correct.
extern "C" int __cdecl frame_wait_replacement() {
    if (g_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    int maxfps = read_com_maxfps_dvar();

    if (maxfps > 0 && maxfps <= 10000 && g_last_frame_qpc > 0) {
        const LONGLONG ticks_per_frame = g_qpc_freq.QuadPart / maxfps;
        const LONGLONG bias_ticks = (LONGLONG)g_frame_limiter_config.deadline_bias_us
                                    * g_qpc_freq.QuadPart / 1000000LL;
        const LONGLONG deadline = g_last_frame_qpc + ticks_per_frame + bias_ticks;

        while (now.QuadPart < deadline) {
            // Sleep si on a le temps, spin pour les <1.5ms restants.
            const LONGLONG remaining_us =
                ((deadline - now.QuadPart) * 1000000LL) / g_qpc_freq.QuadPart;
            if (remaining_us > 1500) {
                Sleep(1);
            } else {
                _mm_pause();
            }
            QueryPerformanceCounter(&now);
        }
    }
    g_last_frame_qpc = now.QuadPart;

    // Retour direct en ms via QPC. Le moteur stocke cette valeur dans
    // [0x01912ad0] et compare avec [0x008eda90] dans le spin loop.
    // Comme on retourne une valeur consistente d'appel en appel, le
    // diff fonctionne et le moteur exit la boucle a la 1ere iteration.
    //
    // Note historique : on a essaye 2 alternatives (appel a l'original,
    // calibration offset+timeGetTime) qui causaient soit overhead soit
    // imprecision. La version QPC est la plus precise pour le cap FPS.
    // La physique etait suspectee d'etre cassee mais c'etait en fait un
    // probleme de `sv_fps` cote serveur (a mettre a 30+).
    return (int)((now.QuadPart * 1000) / g_qpc_freq.QuadPart);
}

bool apply_frame_limiter_patch() {
    if (g_applied) return true;
    if (!g_frame_limiter_config.enable) {
        logger::logf("frame_limiter: disabled in config, skipping");
        return true;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        logger::logf("frame_limiter: GetModuleHandleA(NULL) returned null");
        return false;
    }

    const uintptr_t exe_base = (uintptr_t)exe;
    const uintptr_t opcode_addr  = exe_base + CODMP_FRAME_LIMIT_CALL_OPCODE_RVA;
    const uintptr_t operand_addr = exe_base + CODMP_FRAME_LIMIT_CALL_OPERAND_RVA;

    // Sanity #1 : opcode CALL rel32
    const uint8_t opcode = *(const uint8_t*)opcode_addr;
    if (opcode != 0xE8) {
        logger::logf("frame_limiter: opcode inattendu (0x%02x, attendu 0xE8)", opcode);
        return false;
    }

    // Sanity #2 : la cible actuelle est bien fcn.00438a70
    const int32_t current_offset = *(const int32_t*)operand_addr;
    const uintptr_t current_target = (uintptr_t)((intptr_t)opcode_addr + 5 + current_offset);
    const uintptr_t expected_target = exe_base + (CODMP_FRAME_LIMIT_ORIGINAL_TARGET - CODMP_PREFERRED_BASE);
    if (current_target != expected_target) {
        logger::logf(
            "frame_limiter: cible inattendue 0x%08x (attendu 0x%08x) - patch annule",
            (unsigned)current_target, (unsigned)expected_target);
        return false;
    }

    // Sauvegarde l'adresse runtime de l'original (pour le retour de notre
    // wrapper). Necessaire car le moteur s'attend a la VRAIE valeur ms,
    // pas notre QPC.
    g_orig_sys_ms = (sys_ms_fn)expected_target;

    // Patch : redirige vers notre wrapper
    const uintptr_t hook = (uintptr_t)&frame_wait_replacement;
    const int32_t new_offset = (int32_t)((intptr_t)hook - (intptr_t)(opcode_addr + 5));

    DWORD old_protect = 0;
    if (!VirtualProtect((void*)operand_addr, 4, PAGE_READWRITE, &old_protect)) {
        logger::logf("frame_limiter: VirtualProtect a echoue");
        return false;
    }
    *(int32_t*)operand_addr = new_offset;
    VirtualProtect((void*)operand_addr, 4, old_protect, &old_protect);

    g_applied = true;
    logger::logf(
        "frame_limiter: call at CoDMP+0x%lx redirige -> our wait (0x%08x)",
        (unsigned long)CODMP_FRAME_LIMIT_CALL_OPCODE_RVA, (unsigned)hook);
    return true;
}

}  // namespace patches
