#ifndef COD1RELOADED_DEMO_INDEX_H
#define COD1RELOADED_DEMO_INDEX_H

// demo_index - reads a CoD1 demo (.dm_*) WITHOUT the engine and lists its kills.
//
// A demo is the stream of server messages the client received, framed Quake 3 style
// ([int32 sequence][int32 length][bytes], ended by -1 -1). CoD1 changed the payload,
// all of it RE'd from CoDMP.exe 1.5 (docs in demo_index.cpp): 4 raw bytes, then the
// rest Huffman-compressed (Quake 3's static tree), then commands mixing byte-aligned
// reads and a bit cursor; snapshots carry a playerState (with stats / ammo / hudelems),
// the entities and a CoD-only list of clientStates (team + name).
//
// A kill is the obituary temp entity the gametype script spawns (Scr_Obituary in
// game_mp_x86.dll): eType 12 + 201, otherEntityNum = victim, attackerEntityNum =
// killer (1022 = the world), eventParm = weapon index, or 0x80 | means-of-death for
// melee / headshot / suicide / falling / crush / water / slime. It is counted once, on
// the first snapshot it appears in - what cgame does with an event entity.
//
// Pure computation: no engine call, no global state, safe on any thread.

#include <string>
#include <vector>

namespace patches {

struct DemoKill {
    int  server_time = 0;        // server time of the snapshot that carried it (ms)
    int  t_ms = 0;               // since the demo's first snapshot
    int  attacker = -1;          // client number, 1022 = the world
    int  victim = -1;
    int  weapon = -1;            // weapon index (eventParm) when not a flagged death
    int  mod = -1;               // means of death when flagged (8 = headshot, 7 = melee ...)
    int  pov = -1;               // whose eyes the demo shows at that moment (ps.clientNum)
    int  pov_weapon = -1;        // the weapon in that player's hands (ps.weapon), same index space
    int  segment = 0;            // which gamestate (map) of the demo it happened in: maps[segment]
    std::string attacker_name, victim_name, weapon_name;
};

struct DemoInfo {
    bool ok = false;
    std::string error;                   // why it stopped, when !ok (kills up to there are kept)
    std::string map, hostname, gametype; // map = the FIRST one (the one playback loads first)
    std::vector<std::string> maps;       // one per gamestate: a map change inside the demo adds one
    int  recorder = -1;                  // clientNum from the gamestate (who recorded)
    std::string recorder_name;
    int  first_time = 0, last_time = 0;  // server times of the first / last snapshot
    int  duration_ms = 0;
    int  messages = 0, snapshots = 0;
    int  gamestates = 0;                 // > 1: map_restart / map change inside the demo
    bool time_went_back = false;         // a snapshot older than the one before (never seen so far)
    std::vector<DemoKill> kills;         // every obituary, in order
};

// Parses the whole file. Returns false (and fills error) only when nothing usable came out.
bool demo_index_file(const char* path, DemoInfo* out);

// Same, from memory.
bool demo_index_buffer(const unsigned char* data, size_t size, DemoInfo* out);

// Display name of a weapon file name ("kar98k_sniper_mp" -> "Kar98k Sniper").
std::string demo_weapon_label(const char* weapon_file);

// Human label for a means of death (MOD_HEAD_SHOT -> "headshot"), "" for none.
const char* demo_mod_label(int mod);

}  // namespace patches

#endif
