// radar_map.cpp - see radar_map.h. A port of the PAM's tools/gen_radar_overviews.py
// (same thresholds, same framing), drawing at the size the radar is shown at instead of
// a fixed 256 x 256 JPG.
#include "ui/radar_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace patches {

namespace {

constexpr int L_MATERIALS = 0, L_BRUSHSIDES = 3, L_BRUSHES = 4, L_TRISOUPS = 6;
constexpr int L_DRAWVERTS = 7, L_DRAWINDEXES = 8, L_MODELS = 27, L_ENTITIES = 29;
constexpr uint32_t CONTENTS_PLAYERCLIP = 0x10000;
constexpr float BODY_LOW = 18.0f, BODY_HIGH = 56.0f;   // above the floor: lower = a step, higher = a lintel
constexpr float MAX_STEP = 44.0f;                       // height change between neighbouring floor pixels
constexpr int   WORK = 512;                             // analysis grid (framing, reach): long side
constexpr int   SS = 3;                                 // drawing: supersampling per output pixel

// ---- look (straight RGB) ----
struct RGB { float r, g, b; };
constexpr RGB FLOOR_LO = { 37, 42, 49 };
constexpr RGB FLOOR_HI = { 76, 84, 95 };
constexpr RGB WATER    = { 27, 56, 84 };
constexpr RGB WALL     = { 198, 206, 214 };
constexpr RGB VOID_    = { 17, 20, 24 };
constexpr float WALL_PX  = 1.15f;                       // wall lines, output pixels
constexpr float LEDGE_A  = 0.40f;                       // dark line where the floor steps
constexpr float LEDGE_DZ = 44.0f;

// ------------------------------------------------------------------ BSP
struct Tri { float x[3], y[3], z[3]; float nx, ny, nz, zmin, zmax; bool water; };
struct Box { float minx, maxx, miny, maxy, minz, maxz; int nsides; };
struct Pt  { float x, y, z; };

struct Bsp {
    std::vector<Tri> floors, walls;
    std::vector<Box> clips;
    std::vector<Pt>  pts;          // spawns + bombsites: where players are
    bool  site[2] = { false, false };
    float site_x[2] = { 0, 0 }, site_y[2] = { 0, 0 };
};

uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
float    f32(const unsigned char* p) { float f; memcpy(&f, p, 4); return f; }

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool skip_material(const std::string& n) {
    return n.find("common/") != std::string::npos || n.find("sky") != std::string::npos ||
           n.find("caulk") != std::string::npos || n.find("nodraw") != std::string::npos ||
           n.find("clip") != std::string::npos || n.find("trigger") != std::string::npos ||
           n.find("portal") != std::string::npos || n.find("hint") != std::string::npos;
}

std::vector<std::map<std::string, std::string>> parse_entities(const char* s, int len) {
    std::vector<std::map<std::string, std::string>> out;
    int i = 0;
    auto quoted = [&](std::string& v) -> bool {
        while (i < len && s[i] && s[i] != '"' && s[i] != '}') ++i;
        if (i >= len || s[i] != '"') return false;
        const int a = ++i;
        while (i < len && s[i] && s[i] != '"') ++i;
        v.assign(s + a, s + i);
        if (i < len) ++i;
        return true;
    };
    while (i < len && s[i]) {
        if (s[i] != '{') { ++i; continue; }
        ++i;
        std::map<std::string, std::string> e;
        for (;;) {
            while (i < len && s[i] && s[i] != '"' && s[i] != '}') ++i;
            if (i >= len || !s[i] || s[i] == '}') { ++i; break; }
            std::string k, v;
            if (!quoted(k) || !quoted(v)) break;
            e[k] = v;
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool load_bsp(const unsigned char* d, int len, Bsp& b, std::string& err) {
    if (len < 8 + 31 * 8 || memcmp(d, "IBSP", 4) != 0 || u32(d + 4) != 59) { err = "not a CoD1 v59 bsp"; return false; }
    const unsigned char* lp[31];
    int ll[31];
    for (int i = 0; i < 31; ++i) {
        ll[i] = (int)u32(d + 8 + 8 * i);
        const int off = (int)u32(d + 12 + 8 * i);
        if (ll[i] < 0 || off < 0 || off + ll[i] > len) { err = "bad lump"; return false; }
        lp[i] = d + off;
    }
    // materials: name[64], surface flags, contents
    const int nmat = ll[L_MATERIALS] / 72;
    std::vector<std::string> mname((size_t)nmat);
    std::vector<uint32_t> mcont((size_t)nmat);
    for (int i = 0; i < nmat; ++i) {
        const char* n = (const char*)lp[L_MATERIALS] + i * 72;
        mname[i] = lower(std::string(n, strnlen(n, 64)));
        mcont[i] = u32(lp[L_MATERIALS] + i * 72 + 68);
    }
    if (ll[L_MODELS] < 48) { err = "no models"; return false; }
    const unsigned char* m0 = lp[L_MODELS];
    const int first_soup = (int)u32(m0 + 24), num_soups = (int)u32(m0 + 28);
    const int first_brush = (int)u32(m0 + 40), num_brush = (int)u32(m0 + 44);

    const int nsoup = ll[L_TRISOUPS] / 16, nvert = ll[L_DRAWVERTS] / 44, nidx = ll[L_DRAWINDEXES] / 2;
    for (int s = first_soup; s < first_soup + num_soups && s < nsoup; ++s) {
        const unsigned char* sp = lp[L_TRISOUPS] + s * 16;
        const int mat = u16(sp), fv = (int)u32(sp + 4), ni = u16(sp + 10), fi = (int)u32(sp + 12);
        if (mat >= nmat || skip_material(mname[mat])) continue;
        const bool water = mname[mat].find("water") != std::string::npos || mname[mat].find("liquid") != std::string::npos;
        for (int t = 0; t + 2 < ni; t += 3) {
            Tri tr;
            float n[3] = { 0, 0, 0 };
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (fi + t + k >= nidx) { ok = false; break; }
                const int vi = fv + u16(lp[L_DRAWINDEXES] + (fi + t + k) * 2);
                if (vi < 0 || vi >= nvert) { ok = false; break; }
                const unsigned char* v = lp[L_DRAWVERTS] + vi * 44;
                tr.x[k] = f32(v); tr.y[k] = f32(v + 4); tr.z[k] = f32(v + 8);
                n[0] += f32(v + 28); n[1] += f32(v + 32); n[2] += f32(v + 36);
            }
            if (!ok) continue;
            const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-6f) continue;
            tr.nx = n[0] / nl; tr.ny = n[1] / nl; tr.nz = n[2] / nl;
            tr.zmin = std::min(tr.z[0], std::min(tr.z[1], tr.z[2]));
            tr.zmax = std::max(tr.z[0], std::max(tr.z[1], tr.z[2]));
            tr.water = water;
            if (tr.nz >= 0.35f) b.floors.push_back(tr);
            else if (fabsf(tr.nz) < 0.35f) b.walls.push_back(tr);
        }
    }

    // player-clip brushes of the world: their 6 first sides are the AABB (min/max as floats)
    const int nbrush = ll[L_BRUSHES] / 4, nside = ll[L_BRUSHSIDES] / 8;
    int side = 0;
    for (int bi = 0; bi < nbrush; ++bi) {
        const int ns = u16(lp[L_BRUSHES] + bi * 4), mat = u16(lp[L_BRUSHES] + bi * 4 + 2);
        if (bi >= first_brush && bi < first_brush + num_brush && ns >= 6 && mat < nmat &&
            (mcont[mat] & CONTENTS_PLAYERCLIP) && side + 6 <= nside) {
            Box bx;
            float v[6];
            for (int k = 0; k < 6; ++k) v[k] = f32(lp[L_BRUSHSIDES] + (side + k) * 8);
            bx.minx = v[0]; bx.maxx = v[1]; bx.miny = v[2]; bx.maxy = v[3]; bx.minz = v[4]; bx.maxz = v[5];
            bx.nsides = ns;
            b.clips.push_back(bx);
        }
        side += ns;
    }

    // entities: spawns, bombsites
    const auto ents = parse_entities((const char*)lp[L_ENTITIES], ll[L_ENTITIES]);
    const int nmodel = ll[L_MODELS] / 48;
    for (const auto& e : ents) {
        auto get = [&](const char* k) -> std::string { auto it = e.find(k); return it == e.end() ? "" : it->second; };
        const std::string cn = lower(get("classname")), tn = lower(get("targetname"));
        const std::string org = get("origin");
        float ox = 0, oy = 0, oz = 0;
        const bool has_org = !org.empty() && sscanf(org.c_str(), "%f %f %f", &ox, &oy, &oz) == 3;
        const int site = tn == "bombzone_a" ? 0 : tn == "bombzone_b" ? 1 : -1;
        if (((cn.rfind("mp_", 0) == 0 && cn.find("spawn") != std::string::npos) || site >= 0) && has_org)
            b.pts.push_back({ ox, oy, oz });
        if (site >= 0 && !b.site[site]) {
            if (has_org) { b.site[site] = true; b.site_x[site] = ox; b.site_y[site] = oy; }
            else {
                const std::string model = get("model");
                const int mi = model.size() > 1 && model[0] == '*' ? atoi(model.c_str() + 1) : -1;
                if (mi > 0 && mi < nmodel) {
                    const unsigned char* mp = lp[L_MODELS] + mi * 48;
                    b.site[site] = true;
                    b.site_x[site] = (f32(mp) + f32(mp + 12)) / 2;
                    b.site_y[site] = (f32(mp + 4) + f32(mp + 16)) / 2;
                }
            }
        }
    }
    if (b.pts.empty()) { err = "no spawn points"; return false; }
    return true;
}

// ------------------------------------------------------------------ grid
// A window of the world, square pixels of `px` units, x right, y down (world +Y up).
struct Grid {
    float px = 1, x0 = 0, y0 = 0, span_x = 1, span_y = 1;
    int nx = 8, ny = 8;
    static Grid fit(float lox, float loy, float hix, float hiy, float px) {
        Grid g;
        g.px = px;
        g.nx = std::max(8, (int)ceilf((hix - lox) / px));
        g.ny = std::max(8, (int)ceilf((hiy - loy) / px));
        const float cx = (lox + hix) / 2, cy = (loy + hiy) / 2;
        g.x0 = cx - g.nx * px / 2;
        g.y0 = cy - g.ny * px / 2;
        g.span_x = g.nx * px; g.span_y = g.ny * px;
        return g;
    }
    static Grid exact(float cx, float cy, int nx, int ny, float px) {
        Grid g;
        g.px = px; g.nx = nx; g.ny = ny;
        g.x0 = cx - nx * px / 2; g.y0 = cy - ny * px / 2;
        g.span_x = nx * px; g.span_y = ny * px;
        return g;
    }
    float tx(float x) const { return (x - x0) / px; }
    float ty(float y) const { return (y0 + span_y - y) / px; }
};

struct Layers {
    std::vector<float> height, shade;
    std::vector<unsigned char> water, walls, block;
};

void raster(const Bsp& b, const Grid& g, float zcut, float wall_foot, bool want_block, Layers& L) {
    const size_t n = (size_t)g.nx * g.ny;
    L.height.assign(n, -1e9f); L.shade.assign(n, 0); L.water.assign(n, 0); L.walls.assign(n, 0);
    L.block.assign(want_block ? n : 0, 0);
    const float lx = -0.45f, ly = 0.45f, lz = 0.77f;

    for (const Tri& t : b.floors) {
        if (t.zmin > zcut) continue;
        float sx[3], sy[3];
        for (int k = 0; k < 3; ++k) { sx[k] = g.tx(t.x[k]); sy[k] = g.ty(t.y[k]); }
        const int ix0 = std::max(0, (int)floorf(std::min(sx[0], std::min(sx[1], sx[2]))));
        const int ix1 = std::min(g.nx - 1, (int)ceilf(std::max(sx[0], std::max(sx[1], sx[2]))));
        const int iy0 = std::max(0, (int)floorf(std::min(sy[0], std::min(sy[1], sy[2]))));
        const int iy1 = std::min(g.ny - 1, (int)ceilf(std::max(sy[0], std::max(sy[1], sy[2]))));
        if (ix1 < ix0 || iy1 < iy0) continue;
        const float xa = sx[0], ya = sy[0], xb = sx[1], yb = sy[1], xc = sx[2], yc = sy[2];
        const float den = (yb - yc) * (xa - xc) + (xc - xb) * (ya - yc);
        if (fabsf(den) < 1e-12f) continue;
        const float shade = t.nx * lx + t.ny * ly + t.nz * lz;
        for (int iy = iy0; iy <= iy1; ++iy) {
            const float gy = iy + 0.5f;
            for (int ix = ix0; ix <= ix1; ++ix) {
                const float gx = ix + 0.5f;
                const float w0 = ((yb - yc) * (gx - xc) + (xc - xb) * (gy - yc)) / den;
                const float w1 = ((yc - ya) * (gx - xc) + (xa - xc) * (gy - yc)) / den;
                const float w2 = 1 - w0 - w1;
                if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
                const float z = w0 * t.z[0] + w1 * t.z[1] + w2 * t.z[2];
                const size_t i = (size_t)iy * g.nx + ix;
                if (z <= zcut && z > L.height[i]) {
                    L.height[i] = z; L.shade[i] = shade; L.water[i] = t.water;
                }
            }
        }
    }

    for (const Tri& t : b.walls) {
        if (t.zmin > zcut) continue;
        float sx[3], sy[3];
        for (int k = 0; k < 3; ++k) { sx[k] = g.tx(t.x[k]); sy[k] = g.ty(t.y[k]); }
        const int pad = (int)ceilf(wall_foot);
        const int ix0 = std::max(0, (int)floorf(std::min(sx[0], std::min(sx[1], sx[2]))) - pad);
        const int ix1 = std::min(g.nx - 1, (int)ceilf(std::max(sx[0], std::max(sx[1], sx[2]))) + pad);
        const int iy0 = std::max(0, (int)floorf(std::min(sy[0], std::min(sy[1], sy[2]))) - pad);
        const int iy1 = std::min(g.ny - 1, (int)ceilf(std::max(sy[0], std::max(sy[1], sy[2]))) + pad);
        if (ix1 < ix0 || iy1 < iy0) continue;
        const bool visible = t.zmax >= zcut - 400;
        const float f2 = wall_foot * wall_foot;
        for (int iy = iy0; iy <= iy1; ++iy) {
            const float gy = iy + 0.5f;
            for (int ix = ix0; ix <= ix1; ++ix) {
                const float gx = ix + 0.5f;
                float best = 1e9f;
                for (int k = 0; k < 3; ++k) {
                    const float ax = sx[k], ay = sy[k], bx = sx[(k + 1) % 3], by = sy[(k + 1) % 3];
                    const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                    float u = l2 < 1e-9f ? 0 : ((gx - ax) * dx + (gy - ay) * dy) / l2;
                    u = u < 0 ? 0 : u > 1 ? 1 : u;
                    const float ex = gx - ax - u * dx, ey = gy - ay - u * dy;
                    best = std::min(best, ex * ex + ey * ey);
                }
                if (best >= f2) continue;
                const size_t i = (size_t)iy * g.nx + ix;
                if (visible) L.walls[i] = 1;
                if (want_block) {
                    const float h = L.height[i];
                    if (t.zmin < h + BODY_HIGH && t.zmax > h + BODY_LOW) L.block[i] = 1;
                }
            }
        }
    }

    if (!want_block) return;
    for (const Box& c : b.clips) {
        if (c.nsides > 6 && std::min(c.maxx - c.minx, c.maxy - c.miny) >= 96) continue;   // slanted: AABB too big
        const int ix0 = std::max(0, (int)floorf(g.tx(c.minx))), ix1 = std::min(g.nx - 1, (int)ceilf(g.tx(c.maxx)));
        const int iy0 = std::max(0, (int)floorf(g.ty(c.maxy))), iy1 = std::min(g.ny - 1, (int)ceilf(g.ty(c.miny)));
        for (int iy = iy0; iy <= iy1; ++iy)
            for (int ix = ix0; ix <= ix1; ++ix) {
                const size_t i = (size_t)iy * g.nx + ix;
                const float h = L.height[i];
                if (c.minz < h + BODY_HIGH && c.maxz > h + BODY_LOW) L.block[i] = 1;
            }
    }
}

std::vector<unsigned char> reachable(const Layers& L, const Grid& g, const Bsp& b) {
    const size_t n = (size_t)g.nx * g.ny;
    std::vector<unsigned char> reach(n, 0);
    auto passable = [&](size_t i) { return L.height[i] > -1e8f && !L.water[i] && !L.block[i]; };
    std::vector<int> queue;
    for (const Pt& p : b.pts) {
        const int cx = (int)g.tx(p.x), cy = (int)g.ty(p.y);
        int best = -1, bd = 1 << 30;
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= g.nx || y >= g.ny) continue;
                const float h = L.height[(size_t)y * g.nx + x];
                if (h >= p.z - 100 && h <= p.z + 24 && dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = y * g.nx + x; }
            }
        if (best >= 0 && passable((size_t)best) && !reach[best]) { reach[best] = 1; queue.push_back(best); }
    }
    for (size_t q = 0; q < queue.size(); ++q) {
        const int i = queue[q], x = i % g.nx, y = i / g.nx;
        const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (auto& d : nb) {
            const int xx = x + d[0], yy = y + d[1];
            if (xx < 0 || yy < 0 || xx >= g.nx || yy >= g.ny) continue;
            const int j = yy * g.nx + xx;
            if (reach[j] || !passable((size_t)j)) continue;
            if (fabsf(L.height[j] - L.height[i]) > MAX_STEP) continue;
            reach[j] = 1;
            queue.push_back(j);
        }
    }
    return reach;
}

std::vector<unsigned char> dilate(const std::vector<unsigned char>& m, int nx, int ny, int r) {
    std::vector<unsigned char> cur = m, nxt;
    for (int k = 0; k < r; ++k) {
        nxt = cur;
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                const size_t i = (size_t)y * nx + x;
                if (cur[i]) continue;
                if ((x > 0 && cur[i - 1]) || (x + 1 < nx && cur[i + 1]) ||
                    (y > 0 && cur[i - nx]) || (y + 1 < ny && cur[i + nx])) nxt[i] = 1;
            }
        cur.swap(nxt);
    }
    return cur;
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    const float pos = p / 100.0f * (v.size() - 1);
    const size_t k = (size_t)pos;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    const float a = v[k];
    if (k + 1 >= v.size()) return a;
    const float bnext = *std::min_element(v.begin() + k + 1, v.end());
    return a + (bnext - a) * (pos - k);
}

float smooth01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

}  // namespace

bool radar_map_build(const unsigned char* data, int len, int max_w, int max_h, RadarMap& out) {
    out = RadarMap();
    Bsp b;
    if (!load_bsp(data, len, b, out.error)) return false;
    for (int k = 0; k < 2; ++k) { out.site[k] = b.site[k]; out.site_x[k] = b.site_x[k]; out.site_y[k] = b.site_y[k]; }

    float zcut = -1e9f, slox = 1e9f, sloy = 1e9f, shix = -1e9f, shiy = -1e9f;
    for (const Pt& p : b.pts) {
        zcut = std::max(zcut, p.z);
        slox = std::min(slox, p.x); sloy = std::min(sloy, p.y);
        shix = std::max(shix, p.x); shiy = std::max(shiy, p.y);
    }
    zcut += 150;
    const float sext = std::max(shix - slox, shiy - sloy);

    // 1. loose frame around the spawns and bombsites, where the reachable area is measured
    float pad = std::max(1200.0f, 0.35f * sext);
    float lox = slox - pad, loy = sloy - pad, hix = shix + pad, hiy = shiy + pad;
    Grid g = Grid::fit(lox, loy, hix, hiy, std::max(hix - lox, hiy - loy) / WORK);
    Layers L;
    raster(b, g, zcut, 0.7f, true, L);
    std::vector<unsigned char> reach = reachable(L, g, b);

    // 2. tight frame: the reachable area (and every spawn / bombsite), small margin. Never
    //    wider than the spawn-based frame: a map whose fences are xmodels (not in the BSP)
    //    lets the fill leak into the countryside; thin leaks are trimmed by percentiles.
    const float s_pad = std::max(700.0f, 0.22f * sext);
    lox = slox - s_pad; loy = sloy - s_pad; hix = shix + s_pad; hiy = shiy + s_pad;
    {
        std::vector<float> xs, ys;
        for (int y = 0; y < g.ny; ++y)
            for (int x = 0; x < g.nx; ++x)
                if (reach[(size_t)y * g.nx + x]) { xs.push_back((float)x); ys.push_back((float)y); }
        if (xs.size() > 50) {
            const float rx0 = g.x0 + percentile(xs, 1) * g.px;
            const float rx1 = g.x0 + (percentile(xs, 99) + 1) * g.px;
            const float ry1 = g.y0 + g.span_y - percentile(ys, 1) * g.px;
            const float ry0 = g.y0 + g.span_y - (percentile(ys, 99) + 1) * g.px;
            lox = std::max(lox, rx0); loy = std::max(loy, ry0);
            hix = std::min(hix, rx1); hiy = std::min(hiy, ry1);
        }
    }
    lox = std::min(lox, slox - 250); loy = std::min(loy, sloy - 250);
    hix = std::max(hix, shix + 250); hiy = std::max(hiy, shiy + 250);
    {
        const float margin = 0.03f * std::max(hix - lox, hiy - loy);
        lox -= margin; loy -= margin; hix += margin; hiy += margin;
        // keep the shape within 3:1 either way (a very thin radar is unreadable)
        float sx = hix - lox, sy = hiy - loy;
        const float cx = (lox + hix) / 2, cy = (loy + hiy) / 2;
        sx = std::max(sx, sy / 3.0f); sy = std::max(sy, sx / 3.0f);
        lox = cx - sx / 2; hix = cx + sx / 2; loy = cy - sy / 2; hiy = cy + sy / 2;
    }
    g = Grid::fit(lox, loy, hix, hiy, std::max(hix - lox, hiy - loy) / WORK);
    raster(b, g, zcut, 0.7f, true, L);
    reach = reachable(L, g, b);
    const std::vector<unsigned char> play = dilate(reach, g.nx, g.ny, 3);

    // 3. the drawing, at the size it is shown (SS x SS samples per output pixel)
    const float wx = hix - lox, wy = hiy - loy;
    const float fit = std::min(max_w / wx, max_h / wy);
    const int ow = std::max(16, (int)(wx * fit + 0.5f)), oh = std::max(16, (int)(wy * fit + 0.5f));
    const float vpx = std::max(wx / ow, wy / oh) / SS;
    const Grid v = Grid::exact((lox + hix) / 2, (loy + hiy) / 2, ow * SS, oh * SS, vpx);
    Layers V;
    raster(b, v, zcut, WALL_PX * SS * 0.5f, false, V);

    // play mask at drawing resolution: the analysis mask, bilinear, soft threshold
    const size_t vn = (size_t)v.nx * v.ny;
    std::vector<float> pm(vn);
    for (int y = 0; y < v.ny; ++y) {
        const float wyy = v.y0 + v.span_y - (y + 0.5f) * v.px;
        const float gy = g.ty(wyy) - 0.5f;
        for (int x = 0; x < v.nx; ++x) {
            const float wxx = v.x0 + (x + 0.5f) * v.px;
            const float gx = g.tx(wxx) - 0.5f;
            const int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
            const float fx = gx - x0, fy = gy - y0;
            float acc = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = x0 + dx, yy = y0 + dy;
                    const float m = (xx >= 0 && yy >= 0 && xx < g.nx && yy < g.ny) ? play[(size_t)yy * g.nx + xx] : 0.0f;
                    acc += m * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                }
            pm[(size_t)y * v.nx + x] = smooth01(0.3f, 0.7f, acc);
        }
    }

    // height range of the playable floor (2nd..98th percentile)
    std::vector<float> hs;
    for (size_t i = 0; i < vn; i += 3)
        if (pm[i] > 0.5f && V.height[i] > -1e8f && !V.water[i]) hs.push_back(V.height[i]);
    const float hmin = hs.size() > 20 ? percentile(hs, 2) : 0;
    const float hmax = hs.size() > 20 ? percentile(hs, 98) : 1;
    const float hr = std::max(1.0f, hmax - hmin);

    // compose (straight colour + alpha per sample), then box-filter SS x SS, premultiplied
    out.w = ow; out.h = oh;
    out.rgba.assign((size_t)ow * oh * 4, 0);
    std::vector<float> acc((size_t)ow * oh * 4, 0.0f);
    for (int y = 0; y < v.ny; ++y) {
        for (int x = 0; x < v.nx; ++x) {
            const size_t i = (size_t)y * v.nx + x;
            const float a = pm[i];
            if (a <= 0) continue;
            RGB c;
            const float h = V.height[i];
            if (V.walls[i]) c = WALL;
            else if (h <= -1e8f) c = VOID_;
            else if (V.water[i]) c = WATER;
            else {
                const float t = std::min(1.0f, std::max(0.0f, (h - hmin) / hr));
                const float sh = 0.86f + 0.20f * std::min(1.0f, std::max(0.0f, V.shade[i]));
                c = { (FLOOR_LO.r + (FLOOR_HI.r - FLOOR_LO.r) * t) * sh,
                      (FLOOR_LO.g + (FLOOR_HI.g - FLOOR_LO.g) * t) * sh,
                      (FLOOR_LO.b + (FLOOR_HI.b - FLOOR_LO.b) * t) * sh };
                // a step in the floor (ledge, stairs edge): a dark line on the lower side
                float dz = 0;
                if (x + 1 < v.nx && V.height[i + 1] > -1e8f) dz = std::max(dz, V.height[i + 1] - h);
                if (x > 0 && V.height[i - 1] > -1e8f) dz = std::max(dz, V.height[i - 1] - h);
                if (y + 1 < v.ny && V.height[i + v.nx] > -1e8f) dz = std::max(dz, V.height[i + v.nx] - h);
                if (y > 0 && V.height[i - v.nx] > -1e8f) dz = std::max(dz, V.height[i - v.nx] - h);
                if (dz > LEDGE_DZ) {
                    c.r *= 1 - LEDGE_A; c.g *= 1 - LEDGE_A; c.b *= 1 - LEDGE_A;
                }
            }
            float* o = &acc[((size_t)(y / SS) * ow + (x / SS)) * 4];
            o[0] += c.r * a; o[1] += c.g * a; o[2] += c.b * a; o[3] += a;
        }
    }
    const float nsamp = (float)(SS * SS);
    for (size_t i = 0; i < (size_t)ow * oh; ++i) {
        const float* s = &acc[i * 4];
        unsigned char* d = &out.rgba[i * 4];
        if (s[3] > 0) {
            d[0] = (unsigned char)std::min(255.0f, s[0] / s[3] + 0.5f);
            d[1] = (unsigned char)std::min(255.0f, s[1] / s[3] + 0.5f);
            d[2] = (unsigned char)std::min(255.0f, s[2] / s[3] + 0.5f);
        } else {
            d[0] = (unsigned char)FLOOR_LO.r; d[1] = (unsigned char)FLOOR_LO.g; d[2] = (unsigned char)FLOOR_LO.b;
        }
        d[3] = (unsigned char)std::min(255.0f, s[3] / nsamp * 255.0f + 0.5f);
    }
    out.x0 = v.x0; out.y0 = v.y0; out.span_x = v.span_x; out.span_y = v.span_y;
    out.ok = true;
    return true;
}

}  // namespace patches
