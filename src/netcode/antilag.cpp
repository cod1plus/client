// antilag (g_antilag), CoD1 server module. docs/lag_compensation_cod1_plan.md

#include "netcode/antilag.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

namespace patches {

AntilagConfig g_antilag_config = {
    /* diag_enable      */ false,
    /* diag_log_count   */ 400,
    /* fire_hook_enable */ false,
    /* capture_enable   */ false,
    /* rewind_enable    */ false,
    /* rewind_test_z    */ 0,
    /* rewind_test_self */ false,
};

// game_mp_x86.dll ASLR base, resolved at runtime
static uintptr_t g_antilag_game_base = 0;

// RE offsets (game_mp_x86.dll, imagebase 0x20000000). rva = vma - imagebase.
namespace {
constexpr uintptr_t IMAGEBASE = 0x20000000;

constexpr uintptr_t VMA_LEVEL_TIME    = 0x202cdd68;  // level.time (int ms)
constexpr uintptr_t VMA_LEVEL_FRAMET  = 0x202cdd70;  // level.frameTime
constexpr uintptr_t VMA_LEVEL_NUME    = 0x202cdd60;  // level.num_entities
constexpr uintptr_t VMA_G_ENTITIES    = 0x201756a0;  // gentity_t array base
constexpr uintptr_t VMA_TRAP_DISPATCH = 0x2006d684;  // trap(int num, ...) fn ptr

constexpr uintptr_t VMA_FIRE_TARGET     = 0x2003f9c0;  // Bullet_Fire_Extended entry
constexpr uintptr_t VMA_FIRE_RESUME     = 0x2003f9c8;  // +8, after stolen prologue
constexpr uintptr_t VMA_RUNFRAME        = 0x200294d0;  // G_RunFrame entry
constexpr uintptr_t VMA_RUNFRAME_RESUME = 0x200294d6;  // +6, after stolen prologue

constexpr uintptr_t ENT_STRIDE  = 0x31c;
constexpr uintptr_t ENT_NUMBER  = 0x000;  // s.number
constexpr uintptr_t ENT_SVFLAGS = 0x00c;  // r.svFlags
constexpr uintptr_t ENT_ORIGIN  = 0x138;  // r.currentOrigin, field we shift
constexpr uintptr_t ENT_HEALTH  = 0x238;
constexpr uintptr_t ENT_CLIENT  = 0x15c;  // r.client (gclient_t*, 0 if non-client)
constexpr uintptr_t ENT_INUSE   = 0x164;  // r.inuse (byte)

constexpr uintptr_t CL_COMMANDTIME = 0x000;  // ps.commandTime

constexpr int SYS_LINKENTITY = 0x35;  // trap_LinkEntity(gentity*)

constexpr int MAX_CLIENTS = 64;
constexpr int ONE_FRAME_MS = 25;      // ~1000/sv_fps(40)

inline uintptr_t rva(uintptr_t vma) { return vma - IMAGEBASE; }
}  // namespace

static inline uintptr_t game_ptr(uintptr_t vma) { return g_antilag_game_base + rva(vma); }
static inline uintptr_t ent_at(int i)  { return game_ptr(VMA_G_ENTITIES) + (uintptr_t)i * ENT_STRIDE; }
static inline int       level_time()   { return *(const int*)game_ptr(VMA_LEVEL_TIME); }

// derefs nothing
static inline int antilag_idx_of(uintptr_t p) {
    const uintptr_t lo = game_ptr(VMA_G_ENTITIES);
    const uintptr_t hi = lo + 1024u * ENT_STRIDE;   // MAX_GENTITIES = 1024
    if (p < lo || p >= hi || ((p - lo) % ENT_STRIDE) != 0) return -1;
    return (int)((p - lo) / ENT_STRIDE);
}

// rewind state, declared early: capture's self-heal needs it
namespace {
struct Vec3  { float x, y, z; };
struct Moved { int num; Vec3 saved; };
Moved g_moved[MAX_CLIENTS];
int   g_moved_count = 0;      // 0 = clean state
int   g_in_root     = 0;
long  g_rewind_count = 0;
long  g_restore_count = 0;
long  g_heal_count = 0;
void  antilag_restore_all_impl();   // fwd
}  // namespace

namespace {
constexpr int ANTILAG_SLOTS = 128;          // ~3.2s @ sv_fps 40
constexpr int ANTILAG_MASK  = ANTILAG_SLOTS - 1;

struct Sample { int levelTime; int valid; Vec3 origin; };
struct Ring {
    Sample slot[MAX_CLIENTS][ANTILAG_SLOTS];
    int    frameTime[ANTILAG_SLOTS];
    int    head;
};
Ring g_ring;

// at G_RunFrame entry level.time is OLD, origins are end of prev frame -> consistent pair
void antilag_capture_impl() {
    if (!g_antilag_game_base) return;

    // self-heal: if a restore was skipped, do it now before capture/rewind
    if (g_moved_count > 0) {
        antilag_restore_all_impl();
        g_in_root = 0;
        ++g_heal_count;
        static int s_heallog = 0;
        if (s_heallog < 40) {
            logger::logf("antilag SELF-HEAL: restore force (rewinds=%ld restores=%ld heals=%ld)",
                         g_rewind_count, g_restore_count, g_heal_count);
            ++s_heallog;
        }
    }

    const int now = level_time();
    const int s   = (g_ring.head + 1) & ANTILAG_MASK;
    g_ring.frameTime[s] = now;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        const uintptr_t e = ent_at(i);
        Sample* smp = &g_ring.slot[i][s];
        smp->levelTime = now;
        void* client        = *(void* const*)(e + ENT_CLIENT);
        unsigned char inuse  = *(const unsigned char*)(e + ENT_INUSE);
        int health           = *(const int*)(e + ENT_HEALTH);
        if (client && inuse && health > 0) {
            const float* o = (const float*)(e + ENT_ORIGIN);
            smp->origin.x = o[0]; smp->origin.y = o[1]; smp->origin.z = o[2];
            smp->valid = 1;
        } else {
            smp->valid = 0;
        }
    }
    g_ring.head = s;  // publish last

    static int s_logged = 0, s_frames = 0;
    const int cap = g_antilag_config.diag_log_count;
    if ((cap <= 0 || s_logged < cap) && (++s_frames % 40) == 0) {
        const Sample* c0 = &g_ring.slot[0][s];
        logger::logf("antilag CAP t=%d head=%d c0(valid=%d o=%.0f %.0f %.0f) [rw=%ld rs=%ld heal=%ld]",
                     now, s, c0->valid, c0->origin.x, c0->origin.y, c0->origin.z,
                     g_rewind_count, g_restore_count, g_heal_count);
        ++s_logged;
    }
}
}  // namespace

extern "C" void antilag_capture() { antilag_capture_impl(); }

extern "C" { void* g_antilag_frame_resume = nullptr; }

extern "C" __attribute__((naked))
void antilag_frame_hook() {
    asm(
        "pushal\n\t"
        "call _antilag_capture\n\t"
        "popal\n\t"
        // stolen prologue: sub esp,0x410 (81 ec 10 04 00 00)
        "subl $0x410, %esp\n\t"
        "jmp *_g_antilag_frame_resume\n\t"
    );
}

// detour Bullet_Fire_Extended entry. root call (depth==0): rewind OTHER players to
// their pos at shooter commandTime, swap return addr to post-hook that restores after
// the trace + all penetrations. depth>0 = passthrough.

// trap fn ptr stored at VMA_TRAP_DISPATCH (call ds:0x2006d684), cdecl
typedef int (*antilag_trap_fn)(int, ...);
static inline void antilag_linkentity(uintptr_t ent) {
    antilag_trap_fn trap = *(antilag_trap_fn*)game_ptr(VMA_TRAP_DISPATCH);
    trap(SYS_LINKENTITY, (void*)ent);   // recompute absmin/absmax + sectors
}

namespace {
int antilag_lerp(int num, int rewindTime, Vec3* out) {
    const int s = g_ring.head;
    const Sample* after = nullptr; const Sample* before = nullptr;
    int afterT = 0, beforeT = 0;
    for (int n = 0; n < ANTILAG_SLOTS; ++n) {
        const Sample* smp = &g_ring.slot[num][(s - n) & ANTILAG_MASK];
        if (!smp->valid) continue;
        if (smp->levelTime >= rewindTime) { after = smp; afterT = smp->levelTime; }
        else { before = smp; beforeT = smp->levelTime; break; }
    }
    if (!before && !after) return 0;
    if (!before) { *out = after->origin;  return 1; }
    if (!after)  { *out = before->origin; return 1; }
    const float denom = (float)(afterT - beforeT);
    const float f = denom > 0.f ? (float)(rewindTime - beforeT) / denom : 0.f;
    out->x = before->origin.x + f * (after->origin.x - before->origin.x);
    out->y = before->origin.y + f * (after->origin.y - before->origin.y);
    out->z = before->origin.z + f * (after->origin.z - before->origin.z);
    return 1;
}

float g_last_dx = 0, g_last_dy = 0, g_last_dz = 0;

void antilag_move_one(int num, uintptr_t e, const Vec3* org) {
    float* cur = (float*)(e + ENT_ORIGIN);
    Moved* m = &g_moved[g_moved_count++];
    m->num = num;
    m->saved.x = cur[0]; m->saved.y = cur[1]; m->saved.z = cur[2];
    g_last_dx = org->x - cur[0]; g_last_dy = org->y - cur[1]; g_last_dz = org->z - cur[2];
    cur[0] = org->x; cur[1] = org->y; cur[2] = org->z;
    antilag_linkentity(e);
}

int antilag_rewind_all(uintptr_t attacker) {
    g_moved_count = 0;
    const int idxA = antilag_idx_of(attacker);
    if (idxA < 0) return 0;
    void* acl = *(void* const*)(attacker + ENT_CLIENT);
    if (!acl) return 0;                               // non-client shooter (MG/turret)
    int rewindTime = *(const int*)((uintptr_t)acl + CL_COMMANDTIME);
    const int now = level_time();
    const int attackerNum = *(const int*)(attacker + ENT_NUMBER);
    const int   tz   = g_antilag_config.rewind_test_z;
    const bool  self = g_antilag_config.rewind_test_self;

    if (tz == 0) {
        int oneFrame = *(const int*)game_ptr(VMA_LEVEL_FRAMET);
        if (oneFrame <= 0) oneFrame = ONE_FRAME_MS;
        if (now - rewindTime < oneFrame) return 0;    // too recent
        if (now - rewindTime > 1000) rewindTime = now - 1000;
    }
    int moved = 0;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (i == attackerNum && !self) continue;
        const uintptr_t e = ent_at(i);
        if (!*(void* const*)(e + ENT_CLIENT)) continue;
        if (*(const unsigned char*)(e + ENT_INUSE) != 1) continue;
        if (*(const int*)(e + ENT_HEALTH) <= 0) continue;

        Vec3 target;
        if (tz != 0) {
            const float* o = (const float*)(e + ENT_ORIGIN);
            target.x = o[0]; target.y = o[1]; target.z = o[2] + (float)tz;
        } else {
            if (!antilag_lerp(i, rewindTime, &target)) continue;  // no history
        }
        antilag_move_one(i, e, &target);
        ++moved;
    }

    if (moved > 0) {
        static int s_log = 0;
        if (s_log < 60) {
            logger::logf("antilag REWIND: tireur=%d rt=%d (lag=%dms) -> %d cible(s), "
                         "recul=(%.0f %.0f %.0f)%s",
                         attackerNum, rewindTime, now - rewindTime, moved,
                         g_last_dx, g_last_dy, g_last_dz, tz ? " [test_z]" : "");
            ++s_log;
        }
    }
    return moved;
}

// reverse order
void antilag_restore_all_impl() {
    for (int k = g_moved_count - 1; k >= 0; --k) {
        const uintptr_t e = ent_at(g_moved[k].num);
        float* cur = (float*)(e + ENT_ORIGIN);
        cur[0] = g_moved[k].saved.x; cur[1] = g_moved[k].saved.y; cur[2] = g_moved[k].saved.z;
        antilag_linkentity(e);
    }
    g_moved_count = 0;
    ++g_restore_count;
}

void antilag_log_root(const uintptr_t* a) {
    static int s_count = 0;
    if (s_count >= 40) return;
    const uintptr_t attacker = a[1];
    const int idx = antilag_idx_of(attacker);
    if (idx < 0) { logger::logf("antilag FIRE root: attacker=0x%08x idx=-1", (unsigned)attacker); ++s_count; return; }
    void* client = *(void* const*)(attacker + ENT_CLIENT);
    const int now = level_time();
    if (client)
        logger::logf("antilag FIRE root: idx=%d client=0x%08x cmdTime=%d now=%d lag=%dms",
                     idx, (unsigned)(uintptr_t)client, *(const int*)client, now,
                     now - *(const int*)client);
    else
        logger::logf("antilag FIRE root: idx=%d client=NULL (non-joueur) now=%d", idx, now);
    ++s_count;
}
}  // namespace

// returns 1 if asm should swap the return addr (rewind applied)
extern "C" int antilag_prefire(void* args) {
    if (!g_antilag_game_base) return 0;
    const uintptr_t* a = (const uintptr_t*)args;     // a[0]=ret, a[1..]=args
    const int depth = (int)a[6];                     // arg6 = recursion depth
    if (depth == 0 && g_antilag_config.fire_hook_enable) antilag_log_root(a);
    if (!g_antilag_config.rewind_enable) return 0;
    if (depth != 0) return 0;                         // root only
    if (g_in_root) return 0;
    const int moved = antilag_rewind_all(a[1]);
    if (moved <= 0) return 0;
    g_in_root = 1;
    ++g_rewind_count;
    return 1;
}

extern "C" void antilag_postfire(void) {
    antilag_restore_all_impl();
    g_in_root = 0;
}

extern "C" {
    void* g_antilag_fire_resume = nullptr;
    void* g_antilag_fire_args   = nullptr;  // entry esp: [esp]=ret, +4=args
    void* g_antilag_real_ret    = nullptr;
}

extern "C" __attribute__((naked))
void antilag_fire_posthook() {
    asm(
        "pushl %eax\n\t"                 // preserve return value
        "pushfl\n\t"
        "call _antilag_postfire\n\t"
        "popfl\n\t"
        "popl %eax\n\t"
        "jmp *_g_antilag_real_ret\n\t"
    );
}

extern "C" __attribute__((naked))
void antilag_fire_hook() {
    asm(
        "movl %esp, _g_antilag_fire_args\n\t"     // entry esp: [esp]=ret, +4=arg1, +0x18=depth
        "pushal\n\t"
        "pushl _g_antilag_fire_args\n\t"
        "call _antilag_prefire\n\t"
        "addl $4, %esp\n\t"
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        // swap [entry_esp] from real ret to posthook
        "movl _g_antilag_fire_args, %ecx\n\t"
        "movl (%ecx), %eax\n\t"
        "movl %eax, _g_antilag_real_ret\n\t"
        "movl $_antilag_fire_posthook, %eax\n\t"
        "movl %eax, (%ecx)\n\t"
        "1:\n\t"
        "popal\n\t"
        // stolen prologue:
        "subl $0x50, %esp\n\t"                    // 83 ec 50
        "cmpl $0xc, 0x68(%esp)\n\t"               // 83 7c 24 68 0c, flags for following jle
        "jmp *_g_antilag_fire_resume\n\t"
    );
}

namespace {

bool g_fire_hooked  = false;
bool g_frame_hooked = false;

// 5-byte detour (E9 rel32) at entry, after verifying expected prologue
bool install_entry_detour(uintptr_t func, const uint8_t* expect, int expect_len,
                          void* trampoline, const char* tag) {
    if (memcmp((const void*)func, expect, expect_len) != 0) {
        const uint8_t* b = (const uint8_t*)func;
        logger::logf("antilag: %s ANNULE (prologue inattendu @0x%08x : "
                     "%02x %02x %02x %02x %02x %02x)",
                     tag, (unsigned)func, b[0], b[1], b[2], b[3], b[4], b[5]);
        return false;
    }
    uint8_t patch[5];
    patch[0] = 0xE9;
    const int32_t rel = (int32_t)((uintptr_t)trampoline - (func + 5));
    memcpy(patch + 1, &rel, 4);

    DWORD old = 0;
    if (!VirtualProtect((void*)func, expect_len, PAGE_EXECUTE_READWRITE, &old)) {
        logger::logf("antilag: %s ANNULE (VirtualProtect)", tag);
        return false;
    }
    memcpy((void*)func, patch, 5);  // bytes past 5 become dead
    VirtualProtect((void*)func, expect_len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)func, expect_len);
    return true;
}

void antilag_install_fire_hook(uintptr_t base) {
    if (g_fire_hooked) return;
    const uintptr_t func = base + rva(VMA_FIRE_TARGET);
    static const uint8_t expect[8] = {0x83,0xec,0x50,0x83,0x7c,0x24,0x68,0x0c};
    g_antilag_fire_resume = (void*)(base + rva(VMA_FIRE_RESUME));
    if (install_entry_detour(func, expect, 8, (void*)&antilag_fire_hook, "fire hook")) {
        g_fire_hooked = true;
        logger::logf("antilag: FIRE HOOK (entree) installe @0x%08x -> 0x%08x, reprise 0x%08x",
                     (unsigned)func, (unsigned)(uintptr_t)&antilag_fire_hook,
                     (unsigned)(base + rva(VMA_FIRE_RESUME)));
    }
}

void antilag_install_frame_hook(uintptr_t base) {
    if (g_frame_hooked) return;
    const uintptr_t func = base + rva(VMA_RUNFRAME);
    static const uint8_t expect[6] = {0x81,0xec,0x10,0x04,0x00,0x00};
    g_antilag_frame_resume = (void*)(base + rva(VMA_RUNFRAME_RESUME));
    if (install_entry_detour(func, expect, 6, (void*)&antilag_frame_hook, "frame hook")) {
        g_frame_hooked = true;
        logger::logf("antilag: FRAME HOOK (G_RunFrame) installe @0x%08x -> 0x%08x, reprise 0x%08x",
                     (unsigned)func, (unsigned)(uintptr_t)&antilag_frame_hook,
                     (unsigned)(base + rva(VMA_RUNFRAME_RESUME)));
    }
}

DWORD WINAPI antilag_watch(LPVOID) {
    // game_mp_x86.dll loads only when a hosted match starts
    HMODULE gm = nullptr;
    for (;;) { gm = GetModuleHandleA("game_mp_x86.dll"); if (gm) break; Sleep(20); }

    const uintptr_t base = (uintptr_t)gm;
    g_antilag_game_base = base;
    logger::logf("antilag: game_mp_x86.dll detecte (base=0x%08x)", (unsigned)base);

    // rewind needs both ring (capture) and fire hook
    if (g_antilag_config.capture_enable   || g_antilag_config.rewind_enable)
        antilag_install_frame_hook(base);
    if (g_antilag_config.fire_hook_enable || g_antilag_config.rewind_enable)
        antilag_install_fire_hook(base);

    if (!g_antilag_config.diag_enable) return 0;

    const int*      p_time = (const int*)(base + rva(VMA_LEVEL_TIME));
    const int*      p_nume = (const int*)(base + rva(VMA_LEVEL_NUME));
    const uintptr_t ents   = base + rva(VMA_G_ENTITIES);
    logger::logf("antilag: PHASE 0 validation lecture seule");

    int logged = 0, last_t = INT_MIN;
    const int cap = g_antilag_config.diag_log_count;
    while (cap <= 0 || logged < cap) {
        const int t = *p_time;
        if (t != last_t) {
            const int dt = (last_t == INT_MIN) ? 0 : (t - last_t);
            last_t = t;
            char line[320];
            int n = snprintf(line, sizeof(line), "antilagdiag t=%d dt=%d ne=%d",
                             t, dt, *p_nume);
            for (int i = 0; i < 2 && n < (int)sizeof(line) - 1; ++i) {
                const uintptr_t e = ents + (uintptr_t)i * ENT_STRIDE;
                const float* o = (const float*)(e + ENT_ORIGIN);
                n += snprintf(line + n, sizeof(line) - n,
                              " | c%d hp=%d sv=0x%x o=(%.1f %.1f %.1f)",
                              i, *(const int*)(e + ENT_HEALTH),
                              *(const int*)(e + ENT_SVFLAGS), o[0], o[1], o[2]);
            }
            logger::logf("%s", line);
            ++logged;
        }
        Sleep(8);
    }
    logger::logf("antilag: PHASE 0 terminee (%d frames)", logged);
    return 0;
}

}  // namespace

void antilag_start() {
    if (!g_antilag_config.diag_enable && !g_antilag_config.fire_hook_enable &&
        !g_antilag_config.capture_enable && !g_antilag_config.rewind_enable) return;
    if (HANDLE h = CreateThread(NULL, 0, antilag_watch, NULL, 0, NULL)) CloseHandle(h);
}

}  // namespace patches
