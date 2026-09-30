// hitbox_client.cpp - see hitbox_client.h
//
// The client carries its own copy of the per-bone hit boxes (same xmodelparts files, same
// structure as the server, read live: XModel -> [+4] -> [] -> [+4] = parts, numBones =
// short at **parts, 40-byte entries { mins, maxs, centre, r^2 } at *(parts + 8), hit
// location class per bone at *(parts + 0x14)) and uses it for what it predicts locally -
// the blood / impact of your own bullets. cod1plus.so rewrites the server's copy
// (hitbox_fix.c); left alone, the client would still test the VANILLA boxes: blood on a
// hip edge the server no longer counts, no blood on a hand it now does, and a head box
// turned by the head controller where the server follows the skull.
// Same table (gameplay/hitbox_table.h = the server's hitbox_data.h), same fingerprint
// guard, same head-box move: what the client predicts is what the server tests.
#include "gameplay/hitbox_client.h"
#include "gameplay/hitbox_table.h"
#include "core/logger.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace patches {
namespace {
struct Entry { float mins[3], maxs[3], center[3], r2; };      // 40 bytes
struct Seen  { const void* parts; Entry first; };
Seen g_seen[64];
int  g_nseen = 0;
bool g_verified = false, g_disabled = false;
int  g_unknown = 0;

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}
unsigned fnv1a(const unsigned char* p, size_t n, unsigned h) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}
void set_entry(Entry* e, const float* mins, const float* maxs) {
    float h[3];
    memcpy(e->mins, mins, sizeof(e->mins));
    memcpy(e->maxs, maxs, sizeof(e->maxs));
    for (int a = 0; a < 3; ++a) {
        e->center[a] = 0.5f * (mins[a] + maxs[a]);
        h[a] = 0.5f * (maxs[a] - mins[a]);
    }
    e->r2 = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
}
const hb_set_t* set_for(const char* name) {
    if (!name) return &HB_UNIVERSAL;
    for (unsigned k = 0; k < HB_NUM_LOOKUP; ++k)
        if (!_stricmp(HB_LOOKUP[k].xmodel, name)) return &HB_SETS[HB_LOOKUP[k].set];
    return &HB_UNIVERSAL;
}
}  // namespace

void hitbox_client_patch(const void* dobj) {
    if (g_disabled || !dobj) return;
    const uint8_t* model = *(const uint8_t* const*)((const uint8_t*)dobj + 0x1c);     // the body XModel
    if (!readable(model, 0x10)) return;
    const uint8_t* a = *(const uint8_t* const*)(model + 4);
    if (!readable(a, 4)) return;
    const uint8_t* b = *(const uint8_t* const*)a;
    if (!readable(b, 8)) return;
    uint8_t* parts = *(uint8_t* const*)(b + 4);
    if (!readable(parts, 0x18)) return;
    Entry* e = *(Entry* const*)(parts + 8);
    if (!readable(e, sizeof(Entry))) return;
    for (int i = 0; i < g_nseen; ++i)
        if (g_seen[i].parts == parts && memcmp(&g_seen[i].first, &e[0], sizeof(Entry)) == 0) return;

    const uint8_t* hp = *(const uint8_t* const*)parts;
    if (!readable(hp, 4)) return;
    const uint8_t* hdr = *(const uint8_t* const*)hp;
    if (!readable(hdr, 2)) return;
    const int nbones = *(const short*)hdr;
    if (nbones <= 0 || nbones > 256 || !readable(e, sizeof(Entry) * (size_t)nbones)) return;
    unsigned fp = 0x811c9dc5u;
    for (int i = 0; i < nbones; ++i) fp = fnv1a((const unsigned char*)&e[i], 24, fp);
    if (fp == HB_SHIPPED_FP && nbones == HB_NBONES) {
        const char* name = *(const char* const*)model;
        if (!readable(name, 48)) name = nullptr;
        if (!g_verified) {
            // prove the { mins, maxs, centre, r2 } layout on the first bone with a box
            int k = 0;
            while (k < nbones && e[k].r2 == 0.0f) ++k;
            Entry chk;
            if (k < nbones) set_entry(&chk, e[k].mins, e[k].maxs);
            if (k >= nbones || memcmp(chk.center, e[k].center, sizeof(chk.center)) != 0 ||
                (chk.r2 - e[k].r2) * (chk.r2 - e[k].r2) > 1e-3f * chk.r2 * chk.r2) {
                logger::logf("hitbox_client: bone table layout is not { mins, maxs, centre, r2 } - client boxes left vanilla");
                g_disabled = true;
                return;
            }
            g_verified = true;
        }
        const hb_set_t* set = set_for(name);
        for (unsigned k = 0; k < set->nfix; ++k) {
            const hb_fix_t* f = &set->fix[k];
            if (f->bone < (unsigned)nbones) set_entry(&e[f->bone], f->mins, f->maxs);
        }
        // the head box goes on the bone the head mesh rides (server: hitbox_fix.c)
        int b_head = -1, b_bip = -1;
        for (int k = 0; k < HB_NBONES; ++k) {
            if (!strcmp(HB_BONE_NAMES[k], "head")) b_head = k;
            if (!strcmp(HB_BONE_NAMES[k], "bip01 head")) b_bip = k;
        }
        bool moved = false;
        const uint8_t* cls = *(const uint8_t* const*)(parts + 0x14);
        if (b_head >= 0 && b_bip >= 0 && e[b_bip].r2 == 0.0f && readable(cls, HB_NBONES) && cls[b_bip] == cls[b_head]) {
            e[b_bip] = e[b_head];
            memset(&e[b_head], 0, sizeof(Entry));
            moved = true;
        }
        logger::logf("hitbox_client: %s - %u boxes set as the server tests them (set %s)%s", name ? name : "(no name)",
                     set->nfix, set->parts, moved ? ", head box on bip01 head" : "");
    } else if (nbones == HB_NBONES && g_unknown < 4) {
        ++g_unknown;
        logger::logf("hitbox_client: 80-bone table %p is not the vanilla one (fp 0x%08x) - left alone", (void*)parts, fp);
    }
    Seen* s = &g_seen[g_nseen < 64 ? g_nseen++ : (g_nseen++ % 64)];
    if (g_nseen > 128) g_nseen = 64;
    s->parts = parts;
    s->first = e[0];
}
}  // namespace patches
