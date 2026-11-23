// trace_cull.h - a bounds test before the collision trace against an inline model's brushes.
//
// CoDMP.exe 1.5 (disassembled 2026-10-26, sampling profiler on mp_liberation): CM_BoxTrace (0x426430)
// traces an inline model (func_static, a bmodel '*N') by handing the model's single leaf to
// CM_TraceThroughLeaf (0x425770) at 0x4269b5 WITHOUT comparing the trace's bounds with the model's
// bounds - every brush of the model goes through CM_TraceThroughBrush (0x424fc0), which has only the
// per-brush axial early-out. The client's prediction and the local server trace against every solid
// bmodel entity, so a converted map with 12 000 detail brushes in func_statics spends 45-50 % of its
// main thread in that one function (120-170 fps on liberation), whatever the grouping of the brushes.
//
// This repoints that one call at a check of tw->bounds (tw+0xa4 / tw+0xb0, computed by CM_BoxTrace
// before the branch, in model space like the trace itself) against the model's mins / maxs (the
// cmodel_t at leaf-0x18 / leaf-0xc): disjoint -> the leaf trace is skipped, the trace keeps the
// "no hit" result CM_BoxTrace initialised; else the engine's function runs unchanged. What a trace
// can hit is identical: a brush outside the model's bounds cannot be touched by a trace whose box
// misses those bounds. One log line every 10 s with the counts while it skips anything.
#ifndef COD1RELOADED_TRACE_CULL_H
#define COD1RELOADED_TRACE_CULL_H

namespace patches {

void trace_cull_install();                                     // after the frame limiter, CoDMP at 0x400000 only
void trace_cull_tick();                                        // ~1/s (watcher): the 10 s log line

}  // namespace patches

#endif
