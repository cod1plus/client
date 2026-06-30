// gamex86.dll / cgame_mp_x86.dll load lazily, so patch via watcher thread, not DllMain.
// mss32 proxy not implemented yet; inject manually.

#include <windows.h>
#include <cstring>

#include "core/patches.h"
#include "gameplay/viewheight_fix.h"
#include "gameplay/lean_fix.h"
#include "core/logger.h"
#include "netcode/version_patch.h"
#include "netcode/protocol_patch.h"
#include "netcode/version_gate.h"
#include "video/window_patch.h"
#include "video/fullscreen_patch.h"
#include "performance/fps_cap.h"
#include "performance/frame_limiter.h"
#include "features/updater.h"
#include "features/demo_upload.h"
#include "core/toast.h"
#include "performance/cpu_affinity.h"
#include "performance/process_priority.h"
#include "performance/working_set.h"
#include "performance/fso_disable.h"
#include "video/widescreen_fix.h"
#include "features/avatar_overlay.h"
#include "features/engine_2d.h"
#include "features/discord_rpc.h"
#include "netcode/antilag.h"

namespace {

bool    g_gamex86_patched = false;
HMODULE g_cgame_applied   = nullptr;  // cgame base we last successfully patched

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

bool apply_cgame_patches(HMODULE cgame, bool fresh) {
    if (fresh) {
        logger::logf("cgame_mp_x86.dll detected (base=0x%08x)", (unsigned)(uintptr_t)cgame);
        if (patches::g_lean_fix_config.enable)
            logger::logf("  applying lean fix (diag_fix=%d ctrl_smooth=%d in_stand=%d)...",
                         patches::g_lean_fix_config.move_diag_fix,
                         patches::g_lean_fix_config.ctrl_smooth_enable,
                         patches::g_lean_fix_config.apply_in_stand);
    } else {
        logger::logf("cgame_mp_x86.dll reloaded (map change) -> re-applying patches");
    }

    // CG_Draw2D hook = discord in-match heartbeat (engine_2d_last_hud_tick)
    if (patches::g_discord_rpc_config.enable &&
        patches::g_discord_rpc_config.client_id[0] != '\0') {
        patches::engine_2d_install_hook(cgame);
    }

    const bool ok = patches::apply_to_cgame(cgame);
    logger::logf("  [%s] cgame patches applied (lean=%d)", ok ? "OK" : "RETRY",
                 patches::g_lean_fix_config.enable);
    if (ok && fresh) patches::avatar_overlay_show_test();
    return ok;
}

// Re-applies cgame patches on first load AND after a map-change reload (the engine
// reloads cgame_mp_x86.dll, reverting our hooks). Only latches the base on success
// so a mid-load detection retries.
void monitor_cgame() {
    HMODULE cgame = GetModuleHandleA("cgame_mp_x86.dll");
    if (!cgame) { g_cgame_applied = nullptr; return; }
    const bool fresh = (cgame != g_cgame_applied);
    if (fresh || patches::cgame_needs_reapply(cgame)) {
        if (apply_cgame_patches(cgame, fresh))
            g_cgame_applied = cgame;
    }
}

DWORD WINAPI patch_watcher_thread(LPVOID) {
    // Runs for the whole process. cgame_mp_x86.dll must be caught within a few ms of
    // load (else CG_Init -> CG_RegisterCgameShaders runs before hooks are in), AND it
    // is reloaded on every map change, so we keep monitoring + re-applying forever.
    for (;;) {
        if (!g_gamex86_patched) try_patch_gamex86();
        // register the version userinfo cvar as soon as the engine cvar system is up
        if (*(volatile int*)patches::CODMP_CVAR_COUNT_VA > 0)
            patches::register_client_version_cvar();
        monitor_cgame();
        Sleep(5);
    }
    return 0;
}

}  // namespace

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            // apply pending update (rename .new -> .dll) before file gets locked
            patches::updater_apply_pending();
            logger::init(hModule);
            logger::logf("cod1reloaded DLL_PROCESS_ATTACH (loaded as 0x%08x, version %s)",
                         (unsigned)(uintptr_t)hModule, patches::COD1RELOADED_VERSION);
            patches::load_config(hModule);

            // must run before FS_InitFilesystem so the first pak scan picks it up (~150ms sync on cache miss)
            patches::avatar_overlay_prepare_pk3_blocking();

            patches::updater_start();
            patches::demo_upload_start();
            patches::discord_rpc_start();

            // phase 0: read-only offset validation; no-op unless antilag_diag_enable. see docs/lag_compensation_cod1_plan.md
            patches::antilag_start();

            // server-side: reject clients running an older cod1reloaded (ClientConnect gate)
            patches::version_gate_start();

            // 1ms timer res for precise com_maxfps; process-wide so set early
            patches::fps_cap_init();

            patches::cpu_affinity_apply();
            patches::process_priority_apply();
            patches::working_set_apply();
            patches::fso_disable_apply();

            // FOV engine hook still TODO (CG_CalcFov addr unknown)
            patches::widescreen_fix_apply();

            // patch before Com_Init declares the dvar via Cvar_Get
            patches::apply_short_version_patch();

            // protocol 6 -> 10 (separate from vanilla) + repoint master server
            patches::apply_protocol_patch();

            // r_fullscreen default "0"; window_patch then makes it borderless (alt-tab works)
            patches::apply_fullscreen_patch();

            patches::apply_frame_limiter_patch();

            // window doesn't exist at DllMain yet
            patches::start_window_watcher();

            try_patch_gamex86();
            monitor_cgame();
            if (HANDLE thread = CreateThread(NULL, 0, patch_watcher_thread, NULL, 0, NULL)) {
                CloseHandle(thread);
            }
            break;
        case DLL_PROCESS_DETACH:
            patches::fps_cap_shutdown();
            patches::toast_shutdown();
            patches::avatar_overlay_shutdown();
            patches::discord_rpc_shutdown();
            break;
    }
    return TRUE;
}
