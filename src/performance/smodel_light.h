// smodel_light.h - DIAGNOSTIC: how much static-model vertex lighting the renderer recomputes.
//
// CoDMP.exe 1.5 (disassembled 2026-10-26, sampling profiler on mp_liberation): the frame-time spikes on a
// quick turn are spent in 0x506510 (one vertex's colour from the entity's light list, a VectorNormalize
// 0x42fc00 per light) called from 0x506ae0 = "light this surface of this static model and cache the
// colours": 0x513ded calls it for every drawn surface of an entity whose cache slot ([ent+0x2b0][surf] is
// empty), only for surfaces of at most 0x200 vertices, from a pool of 128 entries (0x1448ae8, 0x1c8 bytes
// each). A turn brings new models in: their surfaces are lit vertex by vertex, the pool evicts, the next
// turn back relights. This hooks the call at 0x513ded (`call 0x506ae0`, eax/ecx + 3 stack arguments) to
// count calls and vertices per 10 s and the worst frame, and logs them; nothing else changes.
#ifndef COD1RELOADED_SMODEL_LIGHT_H
#define COD1RELOADED_SMODEL_LIGHT_H

namespace patches {

void smodel_light_install();
void smodel_light_tick();

}  // namespace patches

#endif
