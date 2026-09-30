// hitbox_view.h - draw the SERVER's tested hit boxes over the player you look at.
//
// cod1plus.so (server cvar cod1x_drawhitbox 1) streams, 5 times a second, the boxes the
// server tests for the player nearest your line of sight, posed exactly as its hit test
// poses them: server command
//     hb <cn> <part> <nparts> <ox> <oy> <oz> <bone> <bone> ...
//     bone = id,px,py,pz,r00,r01,r02,r10,r11,r12,r20,r21,r22,mnx,mny,mnz,mxx,mxy,mxz
// This intercepts it in cgame's server-command dispatcher (CG_ServerCommand, cgame+0x2f660,
// its single call site cgame+0x2fc96) and, with the client cvar cod1x_drawhitbox 1, draws
// every box as a wireframe projected with the game's own camera. A gap between the drawn
// body and the boxes is the desync, on screen.
#pragma once
#include <windows.h>
namespace patches {
bool hitbox_view_install(HMODULE cgame);      // idempotent, at every cgame load
void hitbox_view_frame();                     // cvar registration / poll, ~4/s
bool hitbox_view_active();                    // cod1x_drawhitbox != 0 and fresh data
void hitbox_view_draw(float vw, float vh);    // inside a 2D overlay pass
// the client's own copy of a player's position/yaw (es.pos, es.apos) each time it poses him
void hitbox_view_note_entity(int cn, const float* pos, float yaw);
// the client's own rendered skeleton of one model, in WORLD (bone_probe): pelvis, back_up, neck, head
void hitbox_view_note_skeleton(const float* pelvis, const float* backup, const float* neck, const float* head);
// every frame, for every player drawn (bone_probe): his 80 bone frames in WORLD (12 floats
// each: rows = bone axes, then position) and his body model name -> the tested boxes are
// drawn on the body the client draws, instantly, for everyone on screen (cod1x_drawhitbox 1)
void hitbox_view_note_pose(int cn, const char* model, int nbones, const float* frames);
}
