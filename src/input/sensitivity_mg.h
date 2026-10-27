#ifndef COD1RELOADED_SENSITIVITY_MG_H
#define COD1RELOADED_SENSITIVITY_MG_H

namespace patches {

// The mouse on a mounted MG follows its own cvar (port of cod2x's sensitivity_MG). The game
// turns a turret at a fixed speed whatever `sensitivity` says (2.5 on yaw, 2 on pitch, per
// mouse count); that factor is now `sensitivity_mg` (archived, default 1 = the game's own
// speed, 0.01 - 100), plus cl_mouseAccel like the normal path. Installed once at attach
// (CoDMP.exe); a different build is left untouched.
bool sensitivity_mg_install();
void sensitivity_mg_tick();
// On a mounted MG: the per-count factors CL_MouseMove turns with there (render_clock's view of
// the next command, cl_mouseAccel 0). False otherwise.
bool sensitivity_mg_active(float* kx, float* ky);

}  // namespace patches

#endif
