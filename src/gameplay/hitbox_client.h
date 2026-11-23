// hitbox_client.h - the client's own per-bone hit boxes, set to what the server tests.
//
// The client predicts the blood / impact of your own bullets with its copy of the boxes.
// cod1plus.so rewrites the server's copy; this rewrites the client's the same way, from the
// same table, so a predicted impact and a counted hit agree. See hitbox_client.cpp.
#pragma once
namespace patches {
// Called for every player DObj the client builds (bone_probe): patches that body model's
// bone table once (fingerprint-guarded: only the vanilla 80-bone table is touched).
void hitbox_client_patch(const void* dobj);
}
