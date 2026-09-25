#ifndef COD1RELOADED_RADAR_MAP_H
#define COD1RELOADED_RADAR_MAP_H
// The radar drawing of a map, computed by the client from the map's own BSP (CoD1 v59),
// at the exact pixel size it is shown at: any map, custom ones included, nothing to ship.
//
// Same method as the PAM's tools/gen_radar_overviews.py (validated on every stock map):
//   - the world surfaces seen from above, nothing higher than the playable height
//     (spawns and bombsites + 150 units), so roofs are cut away and interiors show;
//   - the part players can reach: flood fill of the floor from every spawn / bombsite,
//     stopped by walls and player clips at body height and by water. The image is framed
//     on it, and only it is drawn - the rest stays transparent.
// Pure code (no engine, no GL, no globals): runs on a worker thread, and in the preview.
#include <string>
#include <vector>

namespace patches {

struct RadarMap {
    bool ok = false;
    std::string error;
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;     // rows top-down, bytes R,G,B,A (straight alpha)
    // world rectangle of the image: x from x0 (left) to x0 + span_x, y from y0 (bottom)
    // to y0 + span_y (top) - north up
    float x0 = 0, y0 = 0, span_x = 1, span_y = 1;
    bool  site[2] = { false, false };    // SD bombsites A / B (bombzone_A / bombzone_B)
    float site_x[2] = { 0, 0 }, site_y[2] = { 0, 0 };
};

// `max_w` x `max_h`: the box the radar is shown in, in screen pixels. The image fits it
// with the playable area's own shape.
bool radar_map_build(const unsigned char* bsp, int len, int max_w, int max_h, RadarMap& out);

}  // namespace patches

#endif
