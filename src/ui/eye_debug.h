#ifndef COD1RELOADED_EYE_DEBUG_H
#define COD1RELOADED_EYE_DEBUG_H

// eye_debug - `player_debugEyePosition 1` (cod2x's name and idea, animation.cpp): a cross
// at the local player's REAL eye point - where the camera sits and where bullets leave -
// drawn over the game, so that in third person (`cg_thirdPerson 1`) the eye can be
// compared with the body and head, e.g. with the engine's own collision boxes
// (`r_xdebug`), while leaning left / right, standing / crouched.
//
// The eye is rebuilt exactly as cgame builds the camera (CG_OffsetFirstPersonView,
// cgame 0x30034463..0x30034484): cg.predictedPlayerState.origin + viewHeightCurrent, then
// AddLeanToPosition(pos, yaw, leanf, 16, 20). In first person the result is compared with
// the real camera (refdef.vieworg) and the difference is printed: that is the proof the
// cross sits where the engine's eye is.
//
// Local server only (sv_running 1): a debug view for testing, never online.

namespace patches {

void eye_debug_frame();                       // every frame, main thread: cvar, state
bool eye_debug_active();
void eye_debug_draw(float vw, float vh);      // inside ui_begin_2d / ui_end_2d

}  // namespace patches

#endif
