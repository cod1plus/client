// cod1reloaded - DLL entry point.
//
// Strategy:
//  - gamex86.dll is loaded lazily by CoDMP.exe when a server/map starts, not at
//    process startup. We therefore can't patch it from DllMain directly.
//  - We hook LoadLibraryA / LoadLibraryW / LoadLibraryExA / LoadLibraryExW so we
//    can detect the moment gamex86.dll is brought into the process and apply
//    our patches immediately after.
//  - We also probe once on DllMain in case gamex86.dll is already loaded
//    (e.g. when our DLL is injected late).
//
// The mss32 proxy layer (forwarding 360+ Miles exports) is not implemented yet;
// this DLL is meant to be injected manually for now. Once the patch is validated
// in-game, the cod2x mss32 proxy will be ported so installation becomes drop-in.

#include <windows.h>
#include <cstring>

#include "patches.h"
#include "viewheight_fix.h"
#include "lean_fix.h"
#include "logger.h"
#include "version_patch.h"
#include "window_patch.h"
#include "fullscreen_patch.h"
#include "fps_cap.h"
#include "frame_limiter.h"
#include "updater.h"
#include "demo_upload.h"
#include "toast.h"
#include "cpu_affinity.h"
#include "process_priority.h"
#include "working_set.h"
#include "fso_disable.h"
#include "widescreen_fix.h"
#include "avatar_overlay.h"

namespace {

bool g_gamex86_patched = false;
bool g_cgame_patched   = false;

void try_patch_gamex86() {
    if (g_gamex86_patched) return;
    HMODULE game = GetModuleHandleA("gamex86.dll");
    if (!game) return;
    logger::logf("gamex86.dll detected (base=0x%08x), applying patches...",
                 (unsigned)(uintptr_t)game);
    if (patches::apply_viewheight_fix(game)) {
        g_gamex86_patched = true;
        logger::logf("  [OK] viewheight_lerp_speed patched -> %.2f",
                     patches::g_viewheight_config.viewheight_lerp_speed);
    } else {
        logger::logf("  [FAIL] gamex86 patch aborted");
    }
}

void try_patch_cgame() {
    if (g_cgame_patched) return;
    HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
    if (!cgame) return;
    logger::logf("cgame_mp_x86.dll detected (base=0x%08x)",
                 (unsigned)(uintptr_t)cgame);
    if (patches::g_lean_fix_config.enable) {
        logger::logf("  applying lean fix (yaw scales neck=%.2f head=%.2f, in_stand=%d)...",
                     patches::g_lean_fix_config.neck_yaw_scale,
                     patches::g_lean_fix_config.head_yaw_scale,
                     patches::g_lean_fix_config.apply_in_stand);
    } else {
        logger::logf("  lean fix DISABLED (lean_fix_enable=false)");
    }
    if (patches::apply_to_cgame(cgame)) {
        g_cgame_patched = true;
        logger::logf("  [OK] cgame patches applied (lean_hook_active=%d)",
                     patches::g_lean_fix_config.enable);

        // Installe l'overlay HUD (avatars + scoreboard) au chargement du
        // cgame. Desactive par defaut via cod1reloaded.ini.
        patches::avatar_overlay_show_test();
    } else {
        logger::logf("  [FAIL] cgame patch aborted");
    }
}

DWORD WINAPI patch_watcher_thread(LPVOID) {
    // When loaded as mss32.dll, our DllMain fires at process startup. But
    // gamex86.dll loads when hosting a server, and cgame_mp_x86.dll only loads
    // when joining a multiplayer match - both can happen many minutes later.
    //
    // CRITICAL: we must catch cgame_mp_x86.dll WITHIN A FEW MS of its load,
    // otherwise the engine's CG_Init -> CG_RegisterCgameShaders runs before
    // we install our hooks and the registration callback never fires.
    // Polling 5ms = 200 checks/sec = ~0.3% CPU until detected. The thread
    // exits as soon as both DLLs are patched, so cost is bounded.
    int iter = 0;
    while (!g_gamex86_patched || !g_cgame_patched) {
        if (!g_gamex86_patched) try_patch_gamex86();
        if (!g_cgame_patched)   try_patch_cgame();
        if (g_gamex86_patched && g_cgame_patched) break;

        Sleep(5);
        ++iter;
    }
    logger::logf("all patches applied");
    return 0;
}

// --- LoadLibrary hooks (trampoline-free: we just import-table patch) ---
//
// We rely on the simple fact that Tech 3 / CoD1 doesn't dynamically resolve
// LoadLibrary at every call - it loads it from the IAT once. Hooking the import
// table is enough for the engine's own LoadLibrary calls. For robustness we
// also expose a polling fallback through DLL_THREAD_ATTACH (cheap, runs every
// time a thread spawns, which CoD1 does plenty of during init).

}  // namespace

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            // Tout en premier : applique une eventuelle MAJ pending
            // (renomme .new -> .dll). On le fait avant l'init de quoi
            // que ce soit pour eviter de bloquer le fichier en use.
            patches::updater_apply_pending();
            logger::init(hModule);
            logger::logf("cod1reloaded DLL_PROCESS_ATTACH (loaded as 0x%08x, version %s)",
                         (unsigned)(uintptr_t)hModule, patches::COD1RELOADED_VERSION);
            patches::load_config(hModule);

            // Prepare the avatar pk3 BEFORE the engine's FS_InitFilesystem
            // runs. Our DLL is loaded as mss32.dll proxy at process startup,
            // so DllMain runs BEFORE CoDMP.exe's main() initializes file
            // system. By writing main/z_cod1reloaded.pk3 here (synchronous,
            // blocks ~150ms on cache miss), the engine's pak scan picks it
            // up on its first scan. No game restart needed. Cached on
            // subsequent launches -> instant.
            patches::avatar_overlay_prepare_pk3_blocking();

            // Lance le check de mise a jour en background (non bloquant)
            patches::updater_start();

            // Demo upload background watcher
            patches::demo_upload_start();

            // Resolution du timer Windows a 1ms : permet que com_maxfps
            // soit respecte precisement. Doit etre tres tot car le timer
            // est process-wide.
            patches::fps_cap_init();

            // Smoothness bundle : anti-microstutter + responsive inputs.
            // On applique tout dans DllMain (sur le main thread du process)
            // pour beneficier des thread priority settings.
            patches::cpu_affinity_apply();
            patches::process_priority_apply();
            patches::working_set_apply();
            patches::fso_disable_apply();

            // Widescreen / resolution helper. Writes autoexec.cfg in main/
            // for custom res + refresh, and logs the Hor+ corrected FOV.
            // Engine-side hook for FOV is TODO (CG_CalcFov address unknown).
            patches::widescreen_fix_apply();

            // CoDMP.exe est forcement deja la (notre DLL est mappee par lui).
            // On peut patcher shortversion immediatement, avant que
            // Com_Init declare le dvar via Cvar_Get.
            patches::apply_short_version_patch();

            // Force r_fullscreen default a "0" pour que la fenetre soit
            // creee en mode windowed (puis le window_patch la rendra
            // borderless plein ecran -> alt-tab fonctionne).
            patches::apply_fullscreen_patch();

            // Frame limiter precis : remplace le spin-loop ms du moteur
            // par un wait QPC microseconde pour respecter com_maxfps exact.
            patches::apply_frame_limiter_patch();

            // Window watcher : la fenetre principale n'existe pas encore au
            // moment de DllMain, on poll dans un thread separe.
            patches::start_window_watcher();

            try_patch_gamex86();
            try_patch_cgame();
            // DLLs likely load later. Spawn a watcher thread.
            if (HANDLE thread = CreateThread(NULL, 0, patch_watcher_thread, NULL, 0, NULL)) {
                CloseHandle(thread);
            }
            break;
        case DLL_PROCESS_DETACH:
            patches::fps_cap_shutdown();
            patches::toast_shutdown();
            patches::avatar_overlay_shutdown();
            break;
    }
    return TRUE;
}
