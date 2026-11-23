// demo_scan.cpp - runs src/features/demo_index.cpp on demo files, outside the game.
//   g++ -std=c++17 -O2 -I src tools/demo_scan.cpp src/features/demo_index.cpp -o build/demo_scan
//   build/demo_scan [-v] demo0107.dm_2 [more.dm_2 ...]
// Prints, per demo: map, recorder, duration, the kills (time, killer -> victim, weapon);
// a summary line at the end (files parsed cleanly / with an error). -v lists every kill.
#include "features/demo_index.h"

#include <cstdio>
#include <cstring>
#include <chrono>

using namespace patches;

int main(int argc, char** argv) {
    bool verbose = false;
    int ok = 0, bad = 0, total_kills = 0, unnamed = 0;
    double total_ms = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) { verbose = true; continue; }
        DemoInfo d;
        auto t0 = std::chrono::steady_clock::now();
        demo_index_file(argv[i], &d);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total_ms += ms;
        const char* base = strrchr(argv[i], '/');
        if (!base) base = strrchr(argv[i], '\\');
        base = base ? base + 1 : argv[i];
        int mine = 0;
        for (auto& k : d.kills) {
            if (k.attacker == k.pov && k.victim != k.pov) ++mine;
            if (k.weapon_name.empty()) ++unnamed;
        }
        printf("%-16s %-14s %-4s rec=%2d %-20.20s %3d:%02d  msgs=%5d snaps=%5d kills=%3d pov-kills=%3d  %6.0f ms %s%s\n",
               base, d.map.c_str(), d.gametype.c_str(), d.recorder, d.recorder_name.c_str(),
               d.duration_ms / 60000, (d.duration_ms / 1000) % 60, d.messages, d.snapshots,
               (int)d.kills.size(), mine, ms, d.error.empty() ? "" : "ERR: ", d.error.c_str());
        if (d.error.empty() && d.ok) ++ok; else ++bad;
        total_kills += (int)d.kills.size();
        if (verbose) {
            for (auto& k : d.kills) {
                printf("    %3d:%02d.%03d  [pov %2d] %2d %-20.20s -> %2d %-20.20s  %-14s %s%s\n",
                       k.t_ms / 60000, (k.t_ms / 1000) % 60, k.t_ms % 1000, k.pov,
                       k.attacker, k.attacker_name.c_str(), k.victim, k.victim_name.c_str(),
                       k.weapon_name.c_str(), k.mod >= 0 ? demo_mod_label(k.mod) : "",
                       (k.attacker == k.pov && k.victim != k.pov) ? "   <== POV KILL" : "");
            }
        }
    }
    printf("---- %d clean, %d with an error, %d kills (%d without a weapon name), %.0f ms total\n",
           ok, bad, total_kills, unnamed, total_ms);
    return bad ? 1 : 0;
}
