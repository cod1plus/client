// smodel_fast.h - the scene list's overflow without the engine's O(N) sqrt scan per model.
//
// CoDMP.exe 1.5 keeps 1022 refEntity slots per frame (RE_AddRefEntityToScene 0x4e0800).
// A heavy view - coastal, hurtgen, carentan from the right spot - sends more static models
// than that, and for EVERY model past the limit the engine calls 0x4e06e0: a walk over the
// whole list with a sqrt per slot to find the farthest replaceable entry (reType not 0/1),
// which the new model then overwrites, whatever its own distance. 1700 extra models x 1022
// slots = 1.7 M sqrt per frame: the frame-time spikes of the "too many visible models"
// maps come from there, not from the GPU (bilan: GPU 0.9 ms while frames hit 60-130 ms).
//
// This repoints that one call (0x4e086f) at a replacement that keeps a max-heap of the
// slots' squared distances, built once on the first overflow of a frame (O(N)) and then
// popped / pushed per overflow (O(log N)): the same slot the engine would have picked, for
// a few microseconds a frame instead of milliseconds. Two cvars, both archived:
//   cod1x_smodel_fast      1 (default) = the heap; 0 = the engine's own scan, timed
//   cod1x_smodel_keepnear  1 (default) = a model farther than everything in the list is
//                          not added at all (the engine replaced the farthest with it, so a
//                          nearby prop could vanish for a distant one); 0 = exactly vanilla
// The bilan gets a "scene" line whenever a window had frames over the limit.
#ifndef COD1RELOADED_SMODEL_FAST_H
#define COD1RELOADED_SMODEL_FAST_H

#include <cstddef>

namespace patches {

void smodel_fast_install();                                    // after the frame limiter, CoDMP at 0x400000 only
void smodel_fast_tick();                                       // cvar poll, ~1/s (watcher)
void smodel_fast_report(char* buf, size_t n, bool reset);      // "" when no frame overflowed
void smodel_fast_poll_frame(long* calls, double* ms);         // since the last poll (stall_trace)

}  // namespace patches

#endif
