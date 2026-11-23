// use_pickup.cpp - a weapon on the ground is not predicted as picked up on contact. See
// use_pickup.h. Client half of cod1plus.so's use_pickup.c (cod1plushookserver).
//
// The server (game.mp.i386.so + use_pickup.c): walking over a weapon used to take the ammo of
// a weapon of the same type the player carries (Touch_Item_Auto with touched = 1); it now does
// nothing, the use key takes it. The client predicts that pickup on its own:
//
// Main/cgame_mp_x86.dll (md5 075e2af1, image base 0x30000000):
//   +0x2a7a0  CG_TouchTriggerPrediction: for every trigger entity of type 3 (item), `call
//             0x3002a710` at +0x2a7f1 (its only call site), edi = the centity;
//   +0x2a710  CG_TouchItem(edi = cent): cg_predictItems.integer [0x3029c5ac] set,
//             BG_PlayerTouchesItem, cent->miscTime (+0x1f4) != cg.time, then
//             BG_CanItemBeGrabbed(es, ps 0x3020af14, touched 1) -> es->eFlags |= 0x100 (the item
//             is not drawn) and EV_ITEM_PICKUP (0x92) queued in the predicted playerState with
//             the item index as parm: the pickup sound, for this player only;
//   +0x5840   BG_CanItemBeGrabbed: item = bg_itemlist (0x30078328) + es->modelindex (+0x8c) *
//             0x30, index 1..0x45 (else Com_Error), giType +0x20 (1 = weapon, filled when the
//             weapons register; 2 = ammo box, 3 = health), giTag +0x24 = the weapon. A weapon
//             the player carries -> yes while it has ammo to give, touched or not; one he does
//             not carry -> no when touched. Same function as the server's.
// So for a weapon the prediction only ever does what the server no longer does.
//
// THE FIX = the call site +0x2a7f1 points at a stub: an item whose giType is 1 returns at once,
// everything else (and every weapon while cod1x_use_pickup is 0) enters CG_TouchItem with the
// registers as they were. eax / ecx / edx are free there (CG_TouchItem overwrites them before
// reading anything but edi; the caller reloads them after the call). The absolute operands
// checked below are compared with the load's delta added (cgame can be relocated, see
// anim_clamp.cpp).

#include "gameplay/use_pickup.h"
#include "core/logger.h"
#include "netcode/protocol_patch.h"      // CODMP_CVAR_GET_VA, CVAR_ARCHIVE

#include <cstdint>
#include <cstring>

namespace patches {

namespace {

constexpr uintptr_t CGAME_IMAGE_BASE = 0x30000000;
constexpr uintptr_t CALL_RVA         = 0x2a7f1;   // CG_TouchTriggerPrediction -> CG_TouchItem (E8 rel32)
constexpr uintptr_t TARGET_RVA       = 0x2a710;   // CG_TouchItem: a1 <cg_predictItems.integer> 85 c0 0f 84
constexpr uintptr_t PREDICT_ITEMS_VA = 0x3029c5ac;
constexpr uintptr_t ITEMS_LEA_RVA    = 0x5877;    // BG_CanItemBeGrabbed: lea eax,[eax+eax*2]; shl eax,4; add eax,<bg_itemlist>
constexpr uintptr_t ITEMLIST_VA      = 0x30078328;
constexpr uintptr_t RANGE_RVA        = 0x584c;    // cmp eax,1; jl; cmp eax,0x46 - the index range the stub keeps
const uint8_t ITEMS_LEA[7] = { 0x8d, 0x04, 0x40, 0xc1, 0xe0, 0x04, 0x05 };
const uint8_t RANGE[8]     = { 0x83, 0xf8, 0x01, 0x7c, 0x05, 0x83, 0xf8, 0x46 };

constexpr int CV_INT = 0x20;
typedef void* (__cdecl* Cvar_Get_t)(const char*, const char*, int);

volatile LONG g_on = 1;               // cod1x_use_pickup
uintptr_t g_itemlist = 0;             // &bg_itemlist[0] of this cgame load
void* g_orig = nullptr;               // CG_TouchItem of this cgame load
void* g_cv = nullptr;

// In place of `call CG_TouchItem` (edi = the item's centity, which starts with its entityState).
__attribute__((naked)) void stub() {
    __asm__ volatile(
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "movl 0x8c(%%edi), %%eax\n\t"      /* es->modelindex = the item's index */
        "leal -1(%%eax), %%ecx\n\t"
        "cmpl $0x45, %%ecx\n\t"            /* outside 1..0x45: the game's own path (its error) */
        "jae 1f\n\t"
        "leal (%%eax,%%eax,2), %%eax\n\t"
        "shll $4, %%eax\n\t"
        "addl %1, %%eax\n\t"
        "cmpl $1, 0x20(%%eax)\n\t"         /* giType: 1 = weapon */
        "jne 1f\n\t"
        "ret\n"
        "1:\n\t"
        "jmp *%2\n\t"
        : : "m"(g_on), "m"(g_itemlist), "m"(g_orig));
}

}  // namespace

bool use_pickup_install(HMODULE cgame) {
    if (!cgame) return false;
    const uintptr_t base = (uintptr_t)cgame, delta = base - CGAME_IMAGE_BASE;
    const uintptr_t site = base + CALL_RVA, target = base + TARGET_RVA;
    if (*(const uint8_t*)site != 0xE8) { logger::logf("use_pickup: call site opcode 0x%02x, expected E8 - not installed", *(const uint8_t*)site); return false; }
    const uintptr_t cur = site + 5 + (intptr_t)*(const int32_t*)(site + 1);
    if (cur == (uintptr_t)&stub) return true;
    const uint8_t* t = (const uint8_t*)target;
    const uint8_t* lea = (const uint8_t*)(base + ITEMS_LEA_RVA);
    if (cur != target || t[0] != 0xA1 || *(const uint32_t*)(t + 1) != PREDICT_ITEMS_VA + delta ||
        t[5] != 0x85 || t[6] != 0xC0 || t[7] != 0x0F || t[8] != 0x84 ||
        memcmp(lea, ITEMS_LEA, sizeof(ITEMS_LEA)) != 0 || *(const uint32_t*)(lea + 7) != ITEMLIST_VA + delta ||
        memcmp((const void*)(base + RANGE_RVA), RANGE, sizeof(RANGE)) != 0) {
        logger::logf("use_pickup: call site targets 0x%x (expected 0x%x), or CG_TouchItem / bg_itemlist differ - different cgame build? not installed",
                     (unsigned)cur, (unsigned)target);
        return false;
    }
    g_itemlist = base + (ITEMLIST_VA - CGAME_IMAGE_BASE);
    g_orig = (void*)target;
    const int32_t rel = (int32_t)((uintptr_t)&stub - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(site + 1) = rel;
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    logger::logf("use_pickup: weapons on the ground no longer predicted as picked up on contact (cgame+0x%x -> CG_TouchItem), cod1x_use_pickup %ld",
                 (unsigned)CALL_RVA, g_on);
    return true;
}

void use_pickup_tick() {
    if (!g_cv) {
        g_cv = ((Cvar_Get_t)CODMP_CVAR_GET_VA)("cod1x_use_pickup", "1", CVAR_ARCHIVE);
        if (!g_cv) return;
    }
    const LONG on = *(const int*)((const uint8_t*)g_cv + CV_INT) ? 1 : 0;
    if (on != g_on) {
        InterlockedExchange(&g_on, on);
        logger::logf("use_pickup: %s", on ? "weapons on the ground not predicted on contact" : "the game's prediction (cod1x_use_pickup 0)");
    }
}

}  // namespace patches
