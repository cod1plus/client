#ifndef COD1RELOADED_PLAYER_PREVIEW_H
#define COD1RELOADED_PLAYER_PREVIEW_H

// The REAL in-game player model in the settings preview, replacing the old
// rectangle soldier (2026-08-21). The mesh is character_soviet_coat3 + head +
// helmet baked offline with engine-exact FK (xanim_tool/export_model.py), the
// same export that powers the 1.5-vs-1.6X web comparisons. Rendered CPU-side
// (project + painter sort) into the overlay's 2D ortho pass - no engine
// renderer involved, so it works frozen-world inside the Ctrl+M overlay.

namespace patches {

// cx  screen x of the model's centre line
// gy  screen y of the ground (feet baseline)
// h   target model height in pixels (head-to-toe)
// st  horizontal stretch factor, applied in SCREEN space - exactly what GPU
//     stretching does to the real game image, which is the preview's argument
// t   animation clock in seconds; pass judder-quantised time so a low refresh
//     rate visibly steps the run cycle like it steps the sway
// dir run/facing direction: >= 0 runs right, < 0 runs left
void player_preview_draw(float cx, float gy, float h, float st, float t, float dir);

}  // namespace patches

#endif
