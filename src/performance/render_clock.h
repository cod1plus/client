#ifndef COD1RELOADED_RENDER_CLOCK_H
#define COD1RELOADED_RENDER_CLOCK_H
// The RENDER CLOCK: the screen gets frames on its own cadence, the game keeps its own.
//
// CoD1's physics depend on the frame rate (one usercmd per frame, msec = 1000 / com_maxfps,
// no pmove_fixed), so com_maxfps stays 250 - and a 250 fps game on a 144 / 240 / 320 Hz screen
// shows some frames once and some twice: judder. With cod1x_render_clock N the frames are DRAWN
// N times a second at evenly spaced instants, while the commands (and so the physics, the
// network, everything the server sees) stay exactly at com_maxfps:
//   - CL_Frame's SCR_UpdateScreen (CoDMP 0x412915 -> 0x418b30) only draws when a render
//     instant is due; otherwise it only pumps the window messages it would have pumped;
//   - in the frame limiter's wait, a render instant that falls before the next frame is drawn
//     there ("between frames"), with cl.serverTime (0x148dca0) advanced to that instant for
//     the cgame - the other players are interpolated to the exact display time;
//   - such a frame has no new command, so the cgame's predicted view is the last command's:
//     right after CG_CalcViewValues (cgame call 0x355e9 -> 0x34bb0) the camera is moved by
//     the predicted velocity over the time since that command (<= 12 ms) and turned by the
//     mouse movement the engine has not read yet, converted exactly like CL_MouseMove
//     (0x40bd60) will convert it for the next command - what you see is what you send.
// cod1x_render_clock 0 (the default) = the engine's own behaviour, bit for bit.
#include <windows.h>

namespace patches {

void render_clock_install();                    // DllMain: CL_Frame's SCR_UpdateScreen call
void render_clock_install_cgame(HMODULE cgame); // every cgame load: the view hook
void render_clock_tick();                       // watcher thread: the cvar, a log line / 10 s

// Frame limiter: the next render instant (QPC) if one is wanted before `deadline`, with room
// to draw it; false when off / not in game. Then render_clock_draw_between() at that instant.
bool render_clock_slot_before(LONGLONG deadline, LONGLONG now, LONGLONG* slot);
void render_clock_draw_between(LONGLONG now);

}  // namespace patches

#endif
