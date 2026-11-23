// bone_probe.h - DEV DIAGNOSTIC (2026-09-29): the world position of a few bones of every
// player model the client DRAWS, logged once a second per model, to compare with the
// server's bone dump of the same player (hitmap .bones). See bone_probe.cpp.
#pragma once
namespace patches {
// Redirects the render call `call DObjCalcSkel` at CoDMP.exe 0x403d53 through a stub that
// runs the original and then logs. Returns false (and touches nothing) when the bytes are
// not the ones expected.
bool bone_probe_install();
}
