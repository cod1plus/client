// smodel_path.h - DIAGNOSTIC: static-model surfaces that take the CPU path (RB_SurfaceStaticModel) and why.
#ifndef COD1RELOADED_SMODEL_PATH_H
#define COD1RELOADED_SMODEL_PATH_H

namespace patches {

void smodel_path_install();
void smodel_path_tick();
void smodel_path_frame();   // main thread, each frame: cod1x_sp_exp

}  // namespace patches

#endif
