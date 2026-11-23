// smodel_lod.h - distance culling for the map's DETAIL static-model clusters, even under r_lodscale 0.
//
// CoDMP.exe 1.5, the static-model surface pass 0x513d10: `call 0x4e0f10` at 0x513d1f returns the LOD distance
// (|vieworg 0x199d530 - origin esi+0x44| * [0x199d674] + [0x199d670], / scale esi+0x40) that XModel_GetLodForDist
// 0x491300 turns into a LOD index, -1 = farther than the last LOD = not drawn. [0x199d674] comes from r_lodscale;
// rPAM forces r_lodscale 0, so every static model is LOD 0 at any distance and nothing is ever culled - the
// thousands of loose bricks, grass tufts and curbs of a converted map are drawn across the whole town.
// The map pipeline groups those decorations into their own clusters named <map>_dtNNN with a real LOD 0 distance
// in the xmodel header; this hook gives exactly those models the plain distance (factor 1, cvar
// cod1x_detail_scale) when the engine's factor is 0. Every other model (player models, stock maps, the map's
// props, cover, trees, buildings) gets the engine's own value, unchanged.
#ifndef COD1RELOADED_SMODEL_LOD_H
#define COD1RELOADED_SMODEL_LOD_H

namespace patches {

void smodel_lod_install();
void smodel_lod_tick();   // the 10 s line while it culls anything
void smodel_lod_frame();  // main thread, every frame: reads cod1x_detail_scale

}  // namespace patches

#endif
