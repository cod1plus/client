// trace_cull.cpp - see trace_cull.h
//
// CoDMP.exe 1.5, CM_BoxTrace 0x426430 (stack: results [esp0+4], start [esp0+8], end [esp0+0xc], model handle
// [esp0+0x10], brushmask [esp0+0x14], capsule [esp0+0x18]; eax = sphere struct (always passed: 9 dwords copied at
// 0x426578: +0 use, +4 radius, +8 halfheight, +0xc offset[3]), ecx = mins, edx = maxs, NULL = vec3_origin
// 0x580c4c; prologue `sub esp, 0x124` 81 EC 24 01 00 00).
// ecx = MAXS and edx = MINS (not the other way round - a 2026-10-26 build had them swapped and the server's box
// traces answered "no hit" against the floor and the walls, the player walked through everything): the body does
// ebx = ecx, edi = edx, then size[1] = [ebx] - c is what the radius is taken from (min of x and z, positive) and
// size[0] = [edi] - c is what bounds[0] adds to min(start, end); CM_TransformedBoxTrace 0x426a80 agrees:
//   tw (traceWork_t, 0x118 bytes, memset 0 by 0x434890) is the local at esp+0x1c after the four pushes;
//   tw.start = start + c, tw.end = end + c with c = (mins + maxs) / 2;
//   sphere.use == 0 (0x4265e2): bounds[0] = min(start, end) + mins, bounds[1] = max(start, end) + maxs
//     (0x426768..0x426827);
//   sphere.use != 0: bounds[0] = min(tw.start, tw.end) - |offset| - radius, bounds[1] = max + |offset| + radius
//     (0x4266ea..0x42675c); bounds at tw+0xa4 / tw+0xb0, contents tw+0xbc, trace_t at tw+0xc4 (12 dwords:
//   fraction, endpos[3], ... copied to results by the `rep movsd` at 0x426a37; the fraction starts as the
//   caller's results->fraction, endpos = start + fraction * (end - start) at 0x4269df).
//   The model branch: 0x4269aa `mov esi, [esp+0x18]` (the cmodel_t from CM_ClipHandleToModel 0x41c1a0 =
//   [0x18e2c04] + handle * 40 for handle < [0x18e2c00] numSubModels - `lea eax,[esi+esi*4]; lea eax,[ecx+eax*8]`;
//   mins +0, maxs +0xc, leaf +0x18 = 4 ints: a cmodel_t is 40 bytes, NOT 72 - the 2026-10-27 00:29 audit caught the entry test
//   reading another model's bounds and answering 'no hit' against walls),
//   `add esi, 0x18`, `lea eax, [esp+0x1c]`, 0x4269b5 `call 0x425770` CM_TraceThroughLeaf(eax = tw, esi = leaf).
// CM_PointContents 0x423e00 (ecx = point, eax = model handle; prologue 83 EC 08 85 C0 = `sub esp, 8; test eax,
//   eax`): model -> ClipHandleToModel + 0x18 = its leaf, then every leaf brush's bounds; returns eax = contents,
//   0 when nothing holds the point (0x423ee2 `mov eax, edx` with edx = 0). No stack arguments.
//
// Measured on mp_liberation (151 func_static): 8-12 million inline-model traces per 10 s = ~5000 per frame,
// 97-98 % of them against a model whose bounds the trace misses. Three hooks, in the order they pay off:
//   1. CM_BoxTrace ENTRY: handle in [1, numSubModels) -> the engine's own bounds formula (box or capsule)
//      against the model's bounds; disjoint -> write the "no hit" trace_t the engine would have produced
//      (zeros, the caller's fraction, endpos = start + fraction * delta) and return: the memset, the 60 x87
//      ops of the bounds setup and the leaf walk are all spared.
//   2. the leaf call 0x4269b5 (whatever the entry let through): tw bounds vs model.
//   3. CM_PointContents ENTRY: point outside the model's bounds -> 0, like the leaf walk would answer.
#include "performance/trace_cull.h"
#include "core/logger.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

// C symbols for the asm below (MinGW 32-bit prefixes them with an underscore)
extern "C" {
volatile long cod1x_tc_calls = 0;          // CM_BoxTrace entries for an inline model
volatile long cod1x_tc_entry_skipped = 0;  // of which the entry bounds test answered "no hit"
volatile long cod1x_tc_leaf_calls = 0;     // leaf traces that reached the call site
volatile long cod1x_tc_leaf_skipped = 0;   // of which the tw-bounds test spared
volatile long cod1x_tc_pc_calls = 0;       // CM_PointContents entries for an inline model
volatile long cod1x_tc_pc_skipped = 0;     // of which the point lay outside the model's bounds
volatile long cod1x_tc_entry_on = 1;       // cvar cod1x_trace_entry (not archived): 1 = the entry test answers (default), 0 = only the leaf test
volatile long cod1x_tc_audit_this = 0;     // 1 while an audit trace runs (set at the entry, read at the leaf)
volatile long cod1x_tc_entry_verdict = 0;  // the entry test's answer on an audit trace: 1 = "no hit", 0 = overlap
volatile long cod1x_tc_entry_wrong = 0;    // audit traces the entry called "no hit" whose engine bounds overlap the model
long cod1x_tc_scratch[32];                 // audit trace inputs: cmodel, start[3], end[3], mins[3], maxs[3], use, radius, offset[3], my bounds lo.x hi.x lo.y hi.y lo.z hi.z
long cod1x_tc_ewrong_rec[4 * 40];          // the first 4 disagreements: leaf, engine tw bounds[6], then the scratch (32)
volatile long cod1x_tc_audit_ctr = 0;      // every inline-model trace; the 64th is an audit trace (see below)
volatile long cod1x_tc_audit_checked = 0;  // audit traces whose leaf skip was double-checked by the engine
volatile long cod1x_tc_audit_wrong = 0;    // of which the engine found a hit the bounds test had called impossible
long cod1x_tc_audit_rec[4 * 13];           // the first 4 wrong ones: leaf, bounds[6] of tw, fraction before/after, pad
const void* cod1x_tc_leaf_target = (const void*)0x00425770;
const void* cod1x_tc_box_resume = (const void*)0x00426436;   // after the displaced `sub esp, 0x124`
const void* cod1x_tc_pc_resume = (const void*)0x00423e05;    // after the displaced `sub esp, 8; test eax, eax`
float cod1x_tc_half = 0.5f;               // not const: a const at namespace scope has internal linkage, the asm needs the symbol
}

namespace patches {
namespace {

constexpr uintptr_t VA_BOXTRACE   = 0x00426430;
constexpr uintptr_t VA_LEAF_SITE  = 0x004269b5;   // E8 rel32 -> 0x425770, inside CM_BoxTrace
constexpr uintptr_t VA_LEAF       = 0x00425770;   // CM_TraceThroughLeaf(eax = tw, esi = leaf)
constexpr uintptr_t VA_POINTCONT  = 0x00423e00;
const uint8_t BOXTRACE_PROLOGUE[6]  = {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00};
const uint8_t POINTCONT_PROLOGUE[5] = {0x83, 0xEC, 0x08, 0x85, 0xC0};

bool g_installed = false;
long g_rep[6] = {0, 0, 0, 0, 0, 0};
ULONGLONG g_last_log = 0;

// comiss leaves CF/ZF like an unsigned compare: `ja` = the left operand is greater; NaN gives
// "unordered" (not above), so a NaN never skips anything. The engine's x87 code never touches
// xmm0-7; ebx/ebp/esi/edi are preserved around every path.

// 1. CM_BoxTrace entry (reached by the jmp written over its prologue; the stack is the caller's call frame)
__attribute__((naked)) void boxtrace_entry() {
    __asm__ volatile(
        "pushl %%eax\n\t"                        // sphere
        "pushl %%ecx\n\t"                        // maxs
        "pushl %%edx\n\t"                        // mins
        // [esp]=mins [+4]=maxs [+8]=sphere [+0xc]=ret [+0x10]=results [+0x14]=start [+0x18]=end [+0x1c]=model
        "movl 0x1c(%%esp), %%eax\n\t"
        "testl %%eax, %%eax\n\t"
        "jz 9f\n\t"
        "cmpl 0x18e2c00, %%eax\n\t"
        "jae 9f\n\t"
        "incl _cod1x_tc_calls\n\t"
        "incl _cod1x_tc_audit_ctr\n\t"
        "movl $0, _cod1x_tc_audit_this\n\t"
        "movl $0, _cod1x_tc_entry_verdict\n\t"
        "testb $63, _cod1x_tc_audit_ctr\n\t"
        "jnz 6f\n\t"
        "movl $1, _cod1x_tc_audit_this\n\t"      // audit trace: the test runs for its verdict only, the engine runs it in full
        "jmp 5f\n\t"
        "6:\n\t"
        "cmpl $0, _cod1x_tc_entry_on\n\t"
        "je 9f\n\t"                              // entry test off: the engine (the leaf hook still tests)
        "5:\n\t"
        "leal (%%eax,%%eax,4), %%eax\n\t"
        "shll $3, %%eax\n\t"
        "addl 0x18e2c04, %%eax\n\t"              // eax = cmodel_t
        "movl 4(%%esp), %%ecx\n\t"               // ecx = maxs (NULL -> vec3_origin)
        "testl %%ecx, %%ecx\n\t"
        "jnz 1f\n\t"
        "movl $0x580c4c, %%ecx\n\t"
        "1:\n\t"
        "movl (%%esp), %%edx\n\t"                // edx = mins (NULL -> vec3_origin)
        "testl %%edx, %%edx\n\t"
        "jnz 2f\n\t"
        "movl $0x580c4c, %%edx\n\t"
        "2:\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "pushl %%ebx\n\t"
        // [esp]=ebx [+4]=edi [+8]=esi [+0xc]=mins [+0x10]=maxs [+0x14]=sphere [+0x18]=ret [+0x1c]=results [+0x20]=start [+0x24]=end
        "movl 0x20(%%esp), %%esi\n\t"            // start
        "movl 0x24(%%esp), %%edi\n\t"            // end
        "movl 0x14(%%esp), %%ebx\n\t"            // sphere
        "cmpl $0, _cod1x_tc_audit_this\n\t"
        "je 12f\n\t"
        "movl %%eax, _cod1x_tc_scratch\n\t"
        "movss (%%esi), %%xmm7\n\t"  "movss %%xmm7, _cod1x_tc_scratch+4\n\t"
        "movss 4(%%esi), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+8\n\t"
        "movss 8(%%esi), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+12\n\t"
        "movss (%%edi), %%xmm7\n\t"  "movss %%xmm7, _cod1x_tc_scratch+16\n\t"
        "movss 4(%%edi), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+20\n\t"
        "movss 8(%%edi), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+24\n\t"
        "movss (%%edx), %%xmm7\n\t"  "movss %%xmm7, _cod1x_tc_scratch+28\n\t"   // mins (edx)
        "movss 4(%%edx), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+32\n\t"
        "movss 8(%%edx), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+36\n\t"
        "movss (%%ecx), %%xmm7\n\t"  "movss %%xmm7, _cod1x_tc_scratch+40\n\t"   // maxs (ecx)
        "movss 4(%%ecx), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+44\n\t"
        "movss 8(%%ecx), %%xmm7\n\t" "movss %%xmm7, _cod1x_tc_scratch+48\n\t"
        "xorps %%xmm7, %%xmm7\n\t"
        "movss %%xmm7, _cod1x_tc_scratch+52\n\t" "movss %%xmm7, _cod1x_tc_scratch+56\n\t" "movss %%xmm7, _cod1x_tc_scratch+60\n\t"
        "movss %%xmm7, _cod1x_tc_scratch+64\n\t" "movss %%xmm7, _cod1x_tc_scratch+68\n\t"
        "movss %%xmm7, _cod1x_tc_scratch+72\n\t" "movss %%xmm7, _cod1x_tc_scratch+76\n\t" "movss %%xmm7, _cod1x_tc_scratch+80\n\t"
        "movss %%xmm7, _cod1x_tc_scratch+84\n\t" "movss %%xmm7, _cod1x_tc_scratch+88\n\t" "movss %%xmm7, _cod1x_tc_scratch+92\n\t"
        "testl %%ebx, %%ebx\n\t"
        "jz 12f\n\t"
        "movl (%%ebx), %%eax\n\t"    "movl %%eax, _cod1x_tc_scratch+52\n\t"
        "movl 4(%%ebx), %%eax\n\t"   "movl %%eax, _cod1x_tc_scratch+56\n\t"
        "movl 0xc(%%ebx), %%eax\n\t" "movl %%eax, _cod1x_tc_scratch+60\n\t"
        "movl 0x10(%%ebx), %%eax\n\t" "movl %%eax, _cod1x_tc_scratch+64\n\t"
        "movl 0x14(%%ebx), %%eax\n\t" "movl %%eax, _cod1x_tc_scratch+68\n\t"
        "movl _cod1x_tc_scratch, %%eax\n\t"      // eax = cmodel again
        "12:\n\t"
        "testl %%ebx, %%ebx\n\t"
        "jz 3f\n\t"
        "cmpl $0, (%%ebx)\n\t"
        "jne 4f\n\t"
        "3:\n\t"                                 // box: lo = min(start, end) + mins (edx), hi = max + maxs (ecx)
        "movss (%%esi), %%xmm0\n\t"   "minss (%%edi), %%xmm0\n\t"   "addss (%%edx), %%xmm0\n\t"   "movss %%xmm0, _cod1x_tc_scratch+72\n\t" "comiss 0xc(%%eax), %%xmm0\n\t"  "ja 8f\n\t"
        "movss (%%esi), %%xmm0\n\t"   "maxss (%%edi), %%xmm0\n\t"   "addss (%%ecx), %%xmm0\n\t"   "movss %%xmm0, _cod1x_tc_scratch+76\n\t" "movss (%%eax), %%xmm1\n\t"      "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        "movss 4(%%esi), %%xmm0\n\t"  "minss 4(%%edi), %%xmm0\n\t"  "addss 4(%%edx), %%xmm0\n\t"  "movss %%xmm0, _cod1x_tc_scratch+80\n\t" "comiss 0x10(%%eax), %%xmm0\n\t" "ja 8f\n\t"
        "movss 4(%%esi), %%xmm0\n\t"  "maxss 4(%%edi), %%xmm0\n\t"  "addss 4(%%ecx), %%xmm0\n\t"  "movss %%xmm0, _cod1x_tc_scratch+84\n\t" "movss 4(%%eax), %%xmm1\n\t"     "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        "movss 8(%%esi), %%xmm0\n\t"  "minss 8(%%edi), %%xmm0\n\t"  "addss 8(%%edx), %%xmm0\n\t"  "movss %%xmm0, _cod1x_tc_scratch+88\n\t" "comiss 0x14(%%eax), %%xmm0\n\t" "ja 8f\n\t"
        "movss 8(%%esi), %%xmm0\n\t"  "maxss 8(%%edi), %%xmm0\n\t"  "addss 8(%%ecx), %%xmm0\n\t"  "movss %%xmm0, _cod1x_tc_scratch+92\n\t" "movss 8(%%eax), %%xmm1\n\t"     "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        "jmp 7f\n\t"
        "4:\n\t"                                 // capsule: c = (mins + maxs) / 2, lo = min + c - |off| - r, hi = max + c + |off| + r
        "movss 4(%%ebx), %%xmm4\n\t"             // radius
        // x
        "movss (%%ecx), %%xmm3\n\t"   "addss (%%edx), %%xmm3\n\t"   "mulss _cod1x_tc_half, %%xmm3\n\t"
        "movss 0xc(%%ebx), %%xmm5\n\t" "xorps %%xmm6, %%xmm6\n\t"    "subss %%xmm5, %%xmm6\n\t"  "maxss %%xmm5, %%xmm6\n\t"   // |off|
        "movss (%%esi), %%xmm0\n\t"   "minss (%%edi), %%xmm0\n\t"   "addss %%xmm3, %%xmm0\n\t"    "subss %%xmm6, %%xmm0\n\t" "subss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+72\n\t" "comiss 0xc(%%eax), %%xmm0\n\t" "ja 8f\n\t"
        "movss (%%esi), %%xmm0\n\t"   "maxss (%%edi), %%xmm0\n\t"   "addss %%xmm3, %%xmm0\n\t"    "addss %%xmm6, %%xmm0\n\t" "addss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+76\n\t" "movss (%%eax), %%xmm1\n\t" "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        // y
        "movss 4(%%ecx), %%xmm3\n\t"  "addss 4(%%edx), %%xmm3\n\t"  "mulss _cod1x_tc_half, %%xmm3\n\t"
        "movss 0x10(%%ebx), %%xmm5\n\t" "xorps %%xmm6, %%xmm6\n\t"   "subss %%xmm5, %%xmm6\n\t"  "maxss %%xmm5, %%xmm6\n\t"
        "movss 4(%%esi), %%xmm0\n\t"  "minss 4(%%edi), %%xmm0\n\t"  "addss %%xmm3, %%xmm0\n\t"    "subss %%xmm6, %%xmm0\n\t" "subss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+80\n\t" "comiss 0x10(%%eax), %%xmm0\n\t" "ja 8f\n\t"
        "movss 4(%%esi), %%xmm0\n\t"  "maxss 4(%%edi), %%xmm0\n\t"  "addss %%xmm3, %%xmm0\n\t"    "addss %%xmm6, %%xmm0\n\t" "addss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+84\n\t" "movss 4(%%eax), %%xmm1\n\t" "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        // z
        "movss 8(%%ecx), %%xmm3\n\t"  "addss 8(%%edx), %%xmm3\n\t"  "mulss _cod1x_tc_half, %%xmm3\n\t"
        "movss 0x14(%%ebx), %%xmm5\n\t" "xorps %%xmm6, %%xmm6\n\t"   "subss %%xmm5, %%xmm6\n\t"  "maxss %%xmm5, %%xmm6\n\t"
        "movss 8(%%esi), %%xmm0\n\t"  "minss 8(%%edi), %%xmm0\n\t"  "addss %%xmm3, %%xmm0\n\t"    "subss %%xmm6, %%xmm0\n\t" "subss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+88\n\t" "comiss 0x14(%%eax), %%xmm0\n\t" "ja 8f\n\t"
        "movss 8(%%esi), %%xmm0\n\t"  "maxss 8(%%edi), %%xmm0\n\t"  "addss %%xmm3, %%xmm0\n\t"    "addss %%xmm6, %%xmm0\n\t" "addss %%xmm4, %%xmm0\n\t" "movss %%xmm0, _cod1x_tc_scratch+92\n\t" "movss 8(%%eax), %%xmm1\n\t" "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"
        "7:\n\t"                                 // overlapping: the engine's CM_BoxTrace
        "popl %%ebx\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "jmp 9f\n\t"
        "8:\n\t"                                 // disjoint: the "no hit" trace_t, as CM_BoxTrace would leave it
        "popl %%ebx\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "cmpl $0, _cod1x_tc_audit_this\n\t"
        "je 10f\n\t"
        "movl $1, _cod1x_tc_entry_verdict\n\t"   // audit trace: verdict recorded, the engine runs it anyway
        "jmp 9f\n\t"
        "10:\n\t"
        "movl 0x10(%%esp), %%eax\n\t"            // results
        "movss (%%eax), %%xmm2\n\t"              // the caller's initial fraction stays
        "xorl %%ecx, %%ecx\n\t"
        "movl %%ecx, 0x10(%%eax)\n\t"
        "movl %%ecx, 0x14(%%eax)\n\t"
        "movl %%ecx, 0x18(%%eax)\n\t"
        "movl %%ecx, 0x1c(%%eax)\n\t"
        "movl %%ecx, 0x20(%%eax)\n\t"
        "movl %%ecx, 0x24(%%eax)\n\t"
        "movl %%ecx, 0x28(%%eax)\n\t"
        "movl %%ecx, 0x2c(%%eax)\n\t"
        "movl 0x14(%%esp), %%ecx\n\t"            // start
        "movl 0x18(%%esp), %%edx\n\t"            // end
        "movss (%%edx), %%xmm0\n\t"   "subss (%%ecx), %%xmm0\n\t"   "mulss %%xmm2, %%xmm0\n\t" "addss (%%ecx), %%xmm0\n\t"  "movss %%xmm0, 4(%%eax)\n\t"
        "movss 4(%%edx), %%xmm0\n\t"  "subss 4(%%ecx), %%xmm0\n\t"  "mulss %%xmm2, %%xmm0\n\t" "addss 4(%%ecx), %%xmm0\n\t" "movss %%xmm0, 8(%%eax)\n\t"
        "movss 8(%%edx), %%xmm0\n\t"  "subss 8(%%ecx), %%xmm0\n\t"  "mulss %%xmm2, %%xmm0\n\t" "addss 8(%%ecx), %%xmm0\n\t" "movss %%xmm0, 0xc(%%eax)\n\t"
        "incl _cod1x_tc_entry_skipped\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%eax\n\t"
        "ret\n\t"
        "9:\n\t"                                 // the engine's CM_BoxTrace, with its displaced prologue
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%eax\n\t"
        "subl $0x124, %%esp\n\t"
        "jmp *_cod1x_tc_box_resume\n\t"
        : : : "memory");
}

// 2. the leaf call inside CM_BoxTrace: eax = tw, esi = leaf = cmodel + 0x18
__attribute__((naked)) void leaf_hook() {
    __asm__ volatile(
        "incl _cod1x_tc_leaf_calls\n\t"
        "movss 0xa4(%%eax), %%xmm0\n\t"   "comiss -0xc(%%esi), %%xmm0\n\t"  "ja 1f\n\t"
        "movss 0xa8(%%eax), %%xmm0\n\t"   "comiss -0x8(%%esi), %%xmm0\n\t"  "ja 1f\n\t"
        "movss 0xac(%%eax), %%xmm0\n\t"   "comiss -0x4(%%esi), %%xmm0\n\t"  "ja 1f\n\t"
        "movss -0x18(%%esi), %%xmm0\n\t"  "comiss 0xb0(%%eax), %%xmm0\n\t"  "ja 1f\n\t"
        "movss -0x14(%%esi), %%xmm0\n\t"  "comiss 0xb4(%%eax), %%xmm0\n\t"  "ja 1f\n\t"
        "movss -0x10(%%esi), %%xmm0\n\t"  "comiss 0xb8(%%eax), %%xmm0\n\t"  "ja 1f\n\t"
        "cmpl $0, _cod1x_tc_audit_this\n\t"
        "je 3f\n\t"
        "cmpl $1, _cod1x_tc_entry_verdict\n\t"
        "jne 3f\n\t"
        "incl _cod1x_tc_entry_wrong\n\t"          // the entry said "no hit", the engine's own bounds overlap the model
        "movl _cod1x_tc_entry_wrong, %%edx\n\t"
        "cmpl $4, %%edx\n\t"
        "ja 3f\n\t"
        "decl %%edx\n\t"
        "imull $160, %%edx, %%edx\n\t"
        "addl $_cod1x_tc_ewrong_rec, %%edx\n\t"
        "movl %%esi, (%%edx)\n\t"
        "movl 0xa4(%%eax), %%ecx\n\t" "movl %%ecx, 4(%%edx)\n\t"
        "movl 0xa8(%%eax), %%ecx\n\t" "movl %%ecx, 8(%%edx)\n\t"
        "movl 0xac(%%eax), %%ecx\n\t" "movl %%ecx, 12(%%edx)\n\t"
        "movl 0xb0(%%eax), %%ecx\n\t" "movl %%ecx, 16(%%edx)\n\t"
        "movl 0xb4(%%eax), %%ecx\n\t" "movl %%ecx, 20(%%edx)\n\t"
        "movl 0xb8(%%eax), %%ecx\n\t" "movl %%ecx, 24(%%edx)\n\t"
        "pushl %%esi\n\t"
        "xorl %%esi, %%esi\n\t"
        "13:\n\t"
        "movl _cod1x_tc_scratch(,%%esi,4), %%ecx\n\t"
        "movl %%ecx, 28(%%edx,%%esi,4)\n\t"
        "incl %%esi\n\t"
        "cmpl $24, %%esi\n\t"
        "jb 13b\n\t"
        "popl %%esi\n\t"
        "3:\n\t"
        "jmp *_cod1x_tc_leaf_target\n\t"
        "1:\n\t"
        "incl _cod1x_tc_leaf_skipped\n\t"
        "testb $63, _cod1x_tc_audit_ctr\n\t"
        "jnz 2f\n\t"
        // audit: the bounds test said "no brush can be hit" - let the engine walk the leaf and compare the fraction
        "incl _cod1x_tc_audit_checked\n\t"
        "pushl %%esi\n\t"
        "pushl %%eax\n\t"
        "pushl 0xc4(%%eax)\n\t"                  // trace.fraction before
        "call *_cod1x_tc_leaf_target\n\t"        // eax = tw, esi = leaf, as the engine calls it
        "popl %%ecx\n\t"
        "popl %%eax\n\t"
        "popl %%esi\n\t"
        "cmpl %%ecx, 0xc4(%%eax)\n\t"
        "je 2f\n\t"
        "incl _cod1x_tc_audit_wrong\n\t"
        "movl _cod1x_tc_audit_wrong, %%edx\n\t"
        "cmpl $4, %%edx\n\t"
        "ja 2f\n\t"
        "decl %%edx\n\t"
        "imull $52, %%edx, %%edx\n\t"
        "addl $_cod1x_tc_audit_rec, %%edx\n\t"
        "movl %%esi, (%%edx)\n\t"
        "movl 0xa4(%%eax), %%esi\n\t" "movl %%esi, 4(%%edx)\n\t"
        "movl 0xa8(%%eax), %%esi\n\t" "movl %%esi, 8(%%edx)\n\t"
        "movl 0xac(%%eax), %%esi\n\t" "movl %%esi, 12(%%edx)\n\t"
        "movl 0xb0(%%eax), %%esi\n\t" "movl %%esi, 16(%%edx)\n\t"
        "movl 0xb4(%%eax), %%esi\n\t" "movl %%esi, 20(%%edx)\n\t"
        "movl 0xb8(%%eax), %%esi\n\t" "movl %%esi, 24(%%edx)\n\t"
        "movl %%ecx, 28(%%edx)\n\t"
        "movl 0xc4(%%eax), %%esi\n\t" "movl %%esi, 32(%%edx)\n\t"
        "movl (%%edx), %%esi\n\t"                // esi back to the leaf (the engine's code after the call does not use it)
        "2:\n\t"
        "ret\n\t"
        : : : "memory");
}

// 3. CM_PointContents entry: ecx = point, eax = model handle
__attribute__((naked)) void pointcontents_entry() {
    __asm__ volatile(
        "pushl %%edx\n\t"
        "testl %%eax, %%eax\n\t"
        "jz 9f\n\t"
        "cmpl 0x18e2c00, %%eax\n\t"
        "jae 9f\n\t"
        "incl _cod1x_tc_pc_calls\n\t"
        "leal (%%eax,%%eax,4), %%edx\n\t"
        "shll $3, %%edx\n\t"
        "addl 0x18e2c04, %%edx\n\t"              // edx = cmodel_t, eax keeps the handle
        "movss (%%ecx), %%xmm0\n\t"   "movss (%%edx), %%xmm1\n\t"    "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"  "comiss 0xc(%%edx), %%xmm0\n\t"  "ja 8f\n\t"
        "movss 4(%%ecx), %%xmm0\n\t"  "movss 4(%%edx), %%xmm1\n\t"   "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"  "comiss 0x10(%%edx), %%xmm0\n\t" "ja 8f\n\t"
        "movss 8(%%ecx), %%xmm0\n\t"  "movss 8(%%edx), %%xmm1\n\t"   "comiss %%xmm0, %%xmm1\n\t" "ja 8f\n\t"  "comiss 0x14(%%edx), %%xmm0\n\t" "ja 8f\n\t"
        "jmp 9f\n\t"
        "8:\n\t"
        "incl _cod1x_tc_pc_skipped\n\t"
        "popl %%edx\n\t"
        "xorl %%eax, %%eax\n\t"
        "ret\n\t"
        "9:\n\t"
        "popl %%edx\n\t"
        "subl $8, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "jmp *_cod1x_tc_pc_resume\n\t"
        : : : "memory");
}

bool patch_callsite(uintptr_t site, uintptr_t expect, uintptr_t to) {
    uint8_t* p = (uint8_t*)site;
    if (p[0] != 0xE8) return false;
    int32_t rel; memcpy(&rel, p + 1, 4);
    if ((uintptr_t)(site + 5 + rel) != expect) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = (int32_t)(to - (site + 5));
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool patch_entry(uintptr_t va, const uint8_t* expect, size_t n, uintptr_t to) {
    uint8_t* p = (uint8_t*)va;
    if (memcmp(p, expect, n) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t buf[8];
    memset(buf, 0x90, sizeof buf);
    buf[0] = 0xE9;
    int32_t rel = (int32_t)(to - (va + 5));
    memcpy(buf + 1, &rel, 4);
    memcpy(p, buf, n);                        // jmp rel32 + nops over the displaced prologue
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

}  // namespace

void trace_cull_install() {
    if (g_installed) return;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) {
        logger::logf("trace_cull: CoDMP.exe not at its preferred base - not installed");
        return;
    }
    const bool leaf = patch_callsite(VA_LEAF_SITE, VA_LEAF, (uintptr_t)&leaf_hook);
    const bool entry = patch_entry(VA_BOXTRACE, BOXTRACE_PROLOGUE, sizeof BOXTRACE_PROLOGUE, (uintptr_t)&boxtrace_entry);
    const bool pc = patch_entry(VA_POINTCONT, POINTCONT_PROLOGUE, sizeof POINTCONT_PROLOGUE, (uintptr_t)&pointcontents_entry);
    g_installed = leaf || entry || pc;
    logger::logf("trace_cull: inline-model collision bounds tests - CM_BoxTrace entry %s (box and capsule), leaf call %s, CM_PointContents entry %s "
                 "(a model whose bounds the trace misses is answered 'no hit' without walking its brushes; same hits)",
                 entry ? "ON" : "not patched (bytes differ)", leaf ? "ON" : "not patched", pc ? "ON" : "not patched");
}

typedef void* (__cdecl* TcCvarGet_t)(const char*, const char*, int);
static void* g_cv_entry = nullptr;

void trace_cull_tick() {
    if (!g_installed) return;
    if (!g_cv_entry) g_cv_entry = ((TcCvarGet_t)0x0043b880)("cod1x_trace_entry", "1", 0);   // Cvar_Get, CVAR_ARCHIVE
    if (g_cv_entry) cod1x_tc_entry_on = *(const int*)((const uint8_t*)g_cv_entry + 0x20) ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (g_last_log && now - g_last_log < 10000) return;
    const long cur[6] = {cod1x_tc_calls, cod1x_tc_entry_skipped, cod1x_tc_leaf_calls, cod1x_tc_leaf_skipped,
                         cod1x_tc_pc_calls, cod1x_tc_pc_skipped};
    long d[6];
    for (int i = 0; i < 6; ++i) { d[i] = cur[i] - g_rep[i]; g_rep[i] = cur[i]; }
    if (g_last_log && (d[0] > 0 || d[4] > 0)) {
        logger::logf("trace_cull: last 10 s - %ld inline-model traces: %ld answered at the entry (%.1f%%), %ld reached the leaf "
                     "(%ld skipped there), %ld walked; point contents %ld, %ld outside the model | audit: %ld skips double-checked "
                     "by the engine since launch, %ld WRONG (a hit the bounds test called impossible); entry test %s (cod1x_trace_entry), "
                     "%ld entry-vs-engine disagreements",
                     d[0], d[1], d[0] ? 100.0 * d[1] / d[0] : 0.0, d[2], d[3], d[2] - d[3], d[4], d[5],
                     cod1x_tc_audit_checked, cod1x_tc_audit_wrong, cod1x_tc_entry_on ? "ON" : "OFF (leaf test only)", cod1x_tc_entry_wrong);
        static long elogged = 0;
        for (long i = elogged; i < cod1x_tc_entry_wrong && i < 4; ++i, ++elogged) {
            const long* r = cod1x_tc_ewrong_rec + i * 40;
            const uintptr_t cmod = (uintptr_t)r[0] - 0x18;
            const long idx = (long)((cmod - *(uintptr_t*)0x18e2c04) / 40);
            const float* mb = (const float*)cmod;
            const float* tb = (const float*)(r + 1);
            const float* in = (const float*)(r + 8);      // start[3] end[3] mins[3] maxs[3] use radius off[3] mylo.x myhi.x mylo.y myhi.y mylo.z myhi.z
            logger::logf("trace_cull: ENTRY DISAGREEMENT #%ld: model *%ld (cmodel %08x, entry saw %08x) bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f), engine trace bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f)",
                         i + 1, idx, (unsigned)cmod, (unsigned)r[7], mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], tb[0], tb[1], tb[2], tb[3], tb[4], tb[5]);
            logger::logf("trace_cull:   inputs: start (%.1f %.1f %.1f) end (%.1f %.1f %.1f) mins (%.1f %.1f %.1f) maxs (%.1f %.1f %.1f) sphere use %ld radius %.2f offset (%.2f %.2f %.2f); "
                         "my bounds x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f (0 = axis not reached)",
                         in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7], in[8], in[9], in[10], in[11], r[8 + 12], in[13], in[14], in[15], in[16],
                         in[17], in[18], in[19], in[20], in[21], in[22]);
        }
        static long logged = 0;
        for (long i = logged; i < cod1x_tc_audit_wrong && i < 4; ++i, ++logged) {
            const long* r = cod1x_tc_audit_rec + i * 13;
            const uintptr_t leaf = (uintptr_t)r[0];
            const uintptr_t cmod = leaf - 0x18;
            const long idx = (long)((cmod - *(uintptr_t*)0x18e2c04) / 40);
            const float* mb = (const float*)cmod;
            const float* tb = (const float*)(r + 1);
            logger::logf("trace_cull: WRONG skip #%ld: model *%ld bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f), trace bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f), fraction %.4f -> %.4f",
                         i + 1, idx, mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], tb[0], tb[1], tb[2], tb[3], tb[4], tb[5],
                         *(const float*)(r + 7), *(const float*)(r + 8));
        }
    }
    g_last_log = now;
}

}  // namespace patches
