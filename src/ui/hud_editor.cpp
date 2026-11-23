// hud_editor.cpp - see hud_editor.h
#include "ui/hud_editor.h"
#include "ui/gl_overlay.h"
#include "ui/hud_layout.h"
#include "ui/minimap.h"
#include "ui/modern_menu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace patches {

namespace {

constexpr DWORD UI_LINE  = 0xFF232323;
constexpr DWORD UI_EYEB  = 0xFF6B7280;
constexpr DWORD GUIDE    = 0xB0FFFFFF;

// The editor's elements: EL_MINIMAP, then the HUD_* of ui/hud_layout.h.
constexpr int EL_MINIMAP = -1;
constexpr int EL_FIRST = EL_MINIMAP, EL_END = HUD_COUNT;

bool g_on = false;
MinimapLayout g_edit;                 // the minimap's layout being edited (the cvars follow it)
int   g_sel = EL_MINIMAP;             // the element the panel shows
int   g_drag = 0;                     // 0 none, 1 move, 2..5 resize the minimap from a corner (TL TR BR BL)
int   g_drag_el = EL_MINIMAP;
float g_grab_x = 0, g_grab_y = 0;     // move: cursor minus the box's corner
bool  g_snap_x = false, g_snap_y = false, g_snap_cx = false, g_snap_cy = false;
float g_guide_x = 0, g_guide_y = 0;

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

const char* rule_text() {
    switch (minimap_server_rule()) {
    case 0:  return "This server turns the minimap off.";
    case 1:  return "This server shows only you (competitive rules).";
    default: return "Teammates in your snapshot are shown, never an enemy.";
    }
}

const char* el_label(int el) { return el == EL_MINIMAP ? "Minimap" : hud_elem_label(el); }
bool el_on(int el) { return el == EL_MINIMAP ? g_edit.on != 0 : hud_elem_get(el).on != 0; }
void el_set_on(int el, bool on) {
    if (el == EL_MINIMAP) { g_edit.on = on; minimap_layout_set(g_edit); return; }
    HudState s = hud_elem_get(el);
    s.on = on;
    hud_elem_set(el, s);
}
// the game's own fps counter / lagometer stay where the game draws them
bool el_movable(int el) { return el == EL_MINIMAP || hud_elem_game(el) || hud_elem_get(el).style != 0; }
bool el_box(int el, float sw, float sh, float* x, float* y, float* w, float* h) {
    if (el == EL_MINIMAP) { minimap_box(g_edit, sw, sh, x, y, w, h); return true; }
    return hud_elem_box(el, sw, sh, x, y, w, h);
}
void el_reset(int el) {
    if (el == EL_MINIMAP) {
        const int on = g_edit.on;
        g_edit = minimap_layout_default();
        g_edit.on = on;
        minimap_layout_set(g_edit);
        return;
    }
    HudState d = hud_elem_default(el);
    const HudState s = hud_elem_get(el);
    d.on = s.on;
    d.style = s.style;
    hud_elem_set(el, d);
    if (el == HUD_COMPASS) hud_compass_size_set(1.0f);
}

void handle(float x, float y, bool hot) {
    const float s = hot ? 12 : 10;
    ui_aa_rect(x - s / 2, y - s / 2, s, s, 2, 0xFF0B0B0B);
    ui_aa_stroke_rect(x - s / 2, y - s / 2, s, s, 2, 1.5f, 0xFFFFFFFF);
}

void dashed_v(float x, float y0, float y1) {
    for (float y = y0; y < y1; y += 14) ui_rect(roundf(x), y, 1, std::min(7.0f, y1 - y), GUIDE);
}
void dashed_h(float y, float x0, float x1) {
    for (float x = x0; x < x1; x += 14) ui_rect(x, roundf(y), std::min(7.0f, x1 - x), 1, GUIDE);
}

void chip(float x, float y, const char* t, bool light) {
    const float tw = ui_text_tracked_width(11, 700, t, 1.2f, UI_FONT_SANS) + 18;
    ui_aa_rect(x, y, tw, 24, 5, light ? 0xE6FFFFFF : 0xE60B0B0B);
    ui_text_tracked(x + 9, y + 4, 11, 700, light ? 0xFF0A0A0A : 0xFFE6E6E6, t, 1.2f, UI_FONT_SANS);
}

void upper(const char* s, char* out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; ++i) out[i] = (char)toupper((unsigned char)s[i]);
    out[i] = 0;
}

// Edges (16 px at 1080p) and centre lines: a box near one lands on it.
void snap(float sw, float sh, float bw, float bh, float* nx, float* ny) {
    const float S = std::max(0.5f, sh / 1080.0f);
    const float m = roundf(16 * S), d = 10;
    g_snap_x = g_snap_y = g_snap_cx = g_snap_cy = false;
    if (fabsf(*nx - m) < d) { *nx = m; g_snap_x = true; g_guide_x = m; }
    else if (fabsf(*nx + bw - (sw - m)) < d) { *nx = sw - m - bw; g_snap_x = true; g_guide_x = sw - m; }
    else if (fabsf(*nx + bw / 2 - sw / 2) < d) { *nx = sw / 2 - bw / 2; g_snap_cx = true; }
    if (fabsf(*ny - m) < d) { *ny = m; g_snap_y = true; g_guide_y = m; }
    else if (fabsf(*ny + bh - (sh - m)) < d) { *ny = sh - m - bh; g_snap_y = true; g_guide_y = sh - m; }
    else if (fabsf(*ny + bh / 2 - sh / 2) < d) { *ny = sh / 2 - bh / 2; g_snap_cy = true; }
}

// The element under the cursor: the selected one first, then the smallest box.
int pick(float mx, float my, float sw, float sh) {
    int best = EL_END;
    float best_area = 1e12f;
    for (int el = EL_FIRST; el < EL_END; ++el) {
        float x, y, w, h;
        if (!el_box(el, sw, sh, &x, &y, &w, &h)) continue;
        const float g = 4;
        if (mx < x - g || mx >= x + w + g || my < y - g || my >= y + h + g) continue;
        if (el == g_sel) return el;
        if (w * h < best_area) { best_area = w * h; best = el; }
    }
    return best;
}

// The live screen: every element at its place, dragged by its body; the minimap also
// resized by a corner. `input` = the cursor is not over the panel.
void edit_screen(float sw, float sh, bool input) {
    const UiInput& in = ui_input();
    const float S = std::max(0.5f, sh / 1080.0f);

    // minimap corners (selected only): TL TR BR BL, the handles 6 px outside the box
    float bx, by, bw, bh;
    minimap_box(g_edit, sw, sh, &bx, &by, &bw, &bh);
    const float k = bh / std::max(1.0f, bw);
    const float cx[4] = { bx - 6, bx + bw + 6, bx + bw + 6, bx - 6 }, cy[4] = { by - 6, by - 6, by + bh + 6, by + bh + 6 };
    int hot_corner = -1;
    if (input && g_sel == EL_MINIMAP)
        for (int c = 0; c < 4; ++c)
            if (fabsf(in.mx - cx[c]) < 12 && fabsf(in.my - cy[c]) < 12) hot_corner = c;
    const int hot = input && hot_corner < 0 ? pick(in.mx, in.my, sw, sh) : (hot_corner >= 0 ? EL_MINIMAP : EL_END);

    if (input && in.clicked && !g_drag) {
        if (hot_corner >= 0) { g_drag = 2 + hot_corner; g_drag_el = EL_MINIMAP; }
        else if (hot != EL_END) {
            g_sel = hot;
            if (el_movable(hot)) {
                float x, y, w, h;
                el_box(hot, sw, sh, &x, &y, &w, &h);
                g_drag = 1; g_drag_el = hot; g_grab_x = in.mx - x; g_grab_y = in.my - y;
            }
        }
    }
    if (g_drag && !in.down) {                       // released: saved
        if (g_drag_el == EL_MINIMAP) minimap_layout_set(g_edit);
        g_drag = 0;
    }
    g_snap_x = g_snap_y = g_snap_cx = g_snap_cy = false;
    if (g_drag == 1) {
        float x, y, w, h;
        if (el_box(g_drag_el, sw, sh, &x, &y, &w, &h)) {
            float nx = in.mx - g_grab_x, ny = in.my - g_grab_y;
            snap(sw, sh, w, h, &nx, &ny);
            nx = clampf(nx, 0, sw - w); ny = clampf(ny, 0, sh - h);
            if (g_drag_el == EL_MINIMAP) {
                g_edit.x = nx / std::max(1.0f, sw - w);
                g_edit.y = ny / std::max(1.0f, sh - h);
            } else {
                hud_elem_set(g_drag_el, hud_elem_moved(g_drag_el, sw, sh, nx, ny));
            }
        }
    } else if (g_drag >= 2) {
        const int c = g_drag - 2;
        const float ox[4] = { bx, bx + bw, bx + bw, bx }, oy[4] = { by, by, by + bh, by + bh };
        const float ax = ox[(c + 2) % 4], ay = oy[(c + 2) % 4];        // the opposite corner stays
        float side = std::max(fabsf(in.my - ay), fabsf(in.mx - ax) * k);
        side = clampf(side, 140 * S, std::min(640 * S, sh * 0.8f));
        g_edit.size = (int)(side / S + 0.5f);
        float nx, ny, nw, nh;
        minimap_box(g_edit, sw, sh, &nx, &ny, &nw, &nh);
        const float left = (c == 0 || c == 3) ? ax - nw : ax, top = (c == 0 || c == 1) ? ay - nh : ay;
        g_edit.x = clampf(left, 0, sw - nw) / std::max(1.0f, sw - nw);
        g_edit.y = clampf(top, 0, sh - nh) / std::max(1.0f, sh - nh);
    }

    // the elements: ours drawn here (the game draws its own under the veil)
    for (int el = EL_FIRST; el < EL_END; ++el) {
        float x, y, w, h;
        if (!el_box(el, sw, sh, &x, &y, &w, &h)) continue;
        const bool on = el_on(el), sel = el == g_sel, over = el == hot || (g_drag && el == g_drag_el);
        if (el == EL_MINIMAP) {
            if (!on) ui_alpha(0.4f);
            minimap_draw_box(x, y, w, h, sw, sh, g_edit);
            ui_alpha(1.0f);
        } else if (!hud_elem_game(el)) {
            if (!on) ui_alpha(0.4f);
            hud_layout_draw_elem(el, sw, sh);
            ui_alpha(1.0f);
        } else if (!on) {                            // hidden: the game draws nothing there
            ui_aa_rect(x, y, w, h, 4, 0x26FFFFFF);
        }
        const float g = el == EL_MINIMAP ? 6 : 3;
        const DWORD col = sel ? 0xFFFFFFFF : over ? 0xCCFFFFFF : 0x4DFFFFFF;
        ui_aa_stroke_rect(x - g, y - g, w + 2 * g, h + 2 * g, el == EL_MINIMAP ? 6 : 4, sel ? 1.5f : 1.0f, col);
        if (sel && el == EL_MINIMAP) {
            for (int c = 0; c < 4; ++c) handle(cx[c], cy[c], hot_corner == c || g_drag == 2 + c);
        }
        if (sel || over) {
            char t[64], u[48];
            upper(el_label(el), u, sizeof(u));
            if (el == EL_MINIMAP && on) snprintf(t, sizeof(t), "%s   %d PX", u, g_edit.size);
            else snprintf(t, sizeof(t), "%s%s", u, on ? "" : "   HIDDEN");
            float chy = y + h + g + 8;
            if (chy + 24 > sh) chy = y - g - 32;
            const float tw = ui_text_tracked_width(11, 700, t, 1.2f, UI_FONT_SANS) + 18;
            chip(clampf(x - g, 4, sw - tw - 4), clampf(chy, 4, sh - 28), t, sel);
        }
    }

    if (g_snap_x) dashed_v(g_guide_x, 0, sh);
    if (g_snap_y) dashed_h(g_guide_y, 0, sw);
    if (g_snap_cx) dashed_v(sw / 2, 0, sh);
    if (g_snap_cy) dashed_h(sh / 2, 0, sw);
}

// a labelled slider: name on the left, value on the right, the slider under them
int slider_row(float x, float y, float w, const char* name, const char* value, int lo, int hi, int v) {
    ui_text(x, y, 13, 600, UI_EYEB, name);
    const float vw = ui_text_width(14, 600, value);
    ui_text(x + w - vw, y - 1, 14, 600, UI_TEXT, value);
    return menu_slider(x, y + 16, w, lo, hi, v);
}

// one cell of the element list: a small switch (shown / hidden) and the name (picks it)
void element_cell(float x, float y, float w, int el) {
    const bool sel = el == g_sel, on = el_on(el);
    const float h = 32;
    const bool over = menu_hit(x, y, w, h);
    ui_rect_rounded(x, y, w, h, 7, sel ? 0xFF262626 : over ? 0xFF1C1C1C : 0xFF141414);
    if (sel) ui_rect_border(x, y, w, h, 7, 1.0f, 0xFF5A5A5A);
    const float tx = x + 8, ty = y + 8;
    const bool over_sw = menu_hit(tx - 4, y, 38, h);
    ui_rect_rounded(tx, ty, 30, 16, 8, on ? UI_ACCENT : over_sw ? 0xFF3A3A3A : 0xFF2C2C2C);
    ui_rect_rounded(on ? tx + 16 : tx + 2, ty + 2, 12, 12, 6, on ? 0xFF0B0B0B : 0xFF8A8A8A);
    ui_text(x + 48, y + 7, 14, sel ? 600 : 400, on ? UI_TEXT : UI_MUTED, el_label(el));
    if (ui_input().clicked) {
        if (over_sw) el_set_on(el, !on);
        else if (over) g_sel = el;
    }
}

float options_height(int el) {
    if (el == EL_MINIMAP) return 54 + 22 + 56 + 64 + 64 + 68 + 36 + 18 + 26;   // = minimap_options
    if (el == HUD_COMPASS) return 64 + 26;
    if (!hud_elem_game(el)) return 54 + 26;
    return 26;
}

void minimap_options(float x, float& y, float w, float sw, float sh) {
    MinimapLayout l = g_edit;
    static const char* shapes[] = { "Rounded square", "Round" };
    l.shape = menu_segmented(x, y, w, shapes, 2, l.shape); y += 54;
    menu_section(x, y, w, "ORIENTATION"); y += 22;
    static const char* orient[] = { "Turns with you", "North up" };
    l.rotate = menu_segmented(x, y, w, orient, 2, l.rotate ? 0 : 1) == 0; y += 56;
    char v[32];
    snprintf(v, sizeof(v), "%d units", l.range);
    const int range = slider_row(x, y, w, "ZOOM  (from you to the edge)", v, 800, 3200, l.range);
    if (range != l.range) l.range = (range + 25) / 50 * 50;
    y += 64;
    snprintf(v, sizeof(v), "%d px", l.size);
    l.size = slider_row(x, y, w, "SIZE  (at 1080p)", v, 140, 480, l.size); y += 64;
    snprintf(v, sizeof(v), "%d %%", l.alpha);
    l.alpha = slider_row(x, y, w, "OPACITY", v, 30, 100, l.alpha); y += 68;
    l.mates = menu_toggle(x, y, "Teammates on the radar", l.mates != 0);
    y += 36;
    menu_hint(x, y, rule_text());
    y += 18;
    // (a bigger box keeps its place in the room left: anchored to its edge, it grows inwards)
    const bool changed = l.shape != g_edit.shape || l.rotate != g_edit.rotate || l.range != g_edit.range ||
                         l.size != g_edit.size || l.alpha != g_edit.alpha || l.mates != g_edit.mates;
    if (changed && !g_drag) { g_edit = l; minimap_layout_set(g_edit); }
    const char* src = minimap_source();
    char line[96];
    if (src[0]) snprintf(line, sizeof(line), "Radar: %s  -  %s", minimap_map(), src);
    else snprintf(line, sizeof(line), "No map loaded: join a game to see it live.");
    ui_text(x, y + 6, 12, 400, 0xFF5E5E5E, line);
    y += 26;
    (void)sw; (void)sh;
}

void element_options(int el, float x, float& y, float w) {
    char line[96];
    if (el == HUD_COMPASS) {
        const int pc = (int)roundf(hud_compass_size() * 100);
        char v[16];
        snprintf(v, sizeof(v), "%d %%", pc);
        const int n = slider_row(x, y, w, "SIZE", v, 50, 160, pc);
        if (n != pc) hud_compass_size_set(n / 100.0f);
        y += 64;
    }
    if (!hud_elem_game(el)) {
        const char* const* names = nullptr;
        const int n = hud_elem_styles(el, &names);
        HudState s = hud_elem_get(el);
        const int st = menu_segmented(x, y, w, names, n, s.style);
        if (st != s.style) {
            const bool was_on = s.on != 0;
            s.style = st;
            s.on = was_on;
            hud_elem_set(el, s);
        }
        y += 54;
        menu_hint(x, y, s.style == 0 ? "The game's own: it stays where the game draws it."
                                     : "Drag it anywhere. The game's one is turned off.");
        y += 26;
        return;
    }
    float bx, by, bw, bh;
    if (!hud_elem_box(el, 1920, 1080, &bx, &by, &bw, &bh)) {
        menu_hint(x, y, "Not on screen now: it shows in a game, alive.");
    } else {
        const HudState s = hud_elem_get(el);
        if (s.x == 0 && s.y == 0) snprintf(line, sizeof(line), "At its usual place. Drag it to move it.");
        else snprintf(line, sizeof(line), "Moved by %.1f, %.1f (of 640 x 480).", s.x, s.y);
        menu_hint(x, y, line);
    }
    y += 26;
}

void screen_size(float* sw, float* sh) {
    RECT rc;
    *sw = 1920; *sh = 1080;
    if (GetClientRect(overlay_game_window(), &rc) && rc.right > 0 && rc.bottom > 0) { *sw = (float)rc.right; *sh = (float)rc.bottom; }
}

// The game's own HUD in faint outlines (in its 640x480 frame, the sides anchored to the
// screen's edges), so the miniature reads as a screen out of a game: crosshair,
// obituaries, compass, stance, health, ammo.
void hud_ghosts(float x, float y, float mw, float mh) {
    const float q = mh / 480.0f;
    auto L = [&](float vx) { return x + vx * q; };
    auto R = [&](float vx) { return x + mw - (640 - vx) * q; };
    auto Y = [&](float vy) { return y + vy * q; };
    const DWORD c = 0x2EFFFFFF, f = 0x14FFFFFF;
    const float cx = x + mw / 2, cy = y + mh / 2, g = std::max(2.0f, 5 * q), t = std::max(2.0f, 9 * q);
    ui_rect(cx - g - t, cy, t, 1, 0x55FFFFFF); ui_rect(cx + g, cy, t, 1, 0x55FFFFFF);
    ui_rect(cx, cy - g - t, 1, t, 0x55FFFFFF); ui_rect(cx, cy + g, 1, t, 0x55FFFFFF);
    ui_aa_rect(L(12), Y(318), 104 * q, 7 * q, 2, f);
    ui_aa_rect(L(12), Y(332), 78 * q, 7 * q, 2, f);
    ui_aa_ring(L(70), Y(414), 46 * q, 1.0f, c);
    ui_aa_disc(L(70), Y(414) - 46 * q, std::max(1.5f, 3 * q), c);
    ui_aa_rect(L(128), Y(424), 14 * q, 34 * q, 3, f);
    ui_aa_rect(R(520), Y(462), 100 * q, 5 * q, 2, f);
    ui_aa_rect(R(568), Y(432), 52 * q, 20 * q, 3, f);
    ui_aa_rect(R(540), Y(440), 22 * q, 12 * q, 2, f);
}

}  // namespace

bool hud_editor_active() { return g_on; }

void hud_editor_open(bool on) {
    if (on && !g_on) g_edit = minimap_layout();
    if (!on && g_on && g_drag && g_drag_el == EL_MINIMAP) minimap_layout_set(g_edit);
    g_on = on;
    g_drag = 0;
}

void hud_editor_select(int el) { if (el >= EL_FIRST && el < EL_END) g_sel = el; }

float hud_editor_block(float x, float y, float w) {
    const float y0 = y;
    menu_section(x, y, w, "HUD"); y += 26;
    MinimapLayout l = minimap_layout();

    // the screen in miniature, the HUD at its place (the minimap live); a click opens the editor
    float sw, sh;
    screen_size(&sw, &sh);
    const float mw = roundf(std::min(300.0f, w * 0.55f)), mh = roundf(mw * sh / sw);
    const bool hover = menu_hit(x, y, mw, mh);
    ui_aa_rect_vgrad(x, y, mw, mh, 6, 0xFF16181B, 0xFF0A0A0A);
    const float s = mw / sw;
    int boxes = 0;
    for (int e = 0; e < HUD_COUNT; ++e) {             // in a game: the real places
        float bx, by, bw, bh;
        if (!hud_elem_box(e, sw, sh, &bx, &by, &bw, &bh)) continue;
        ++boxes;
        const bool on = hud_elem_get(e).on != 0;
        ui_aa_rect(x + bx * s, y + by * s, std::max(2.0f, bw * s), std::max(2.0f, bh * s), 1.5f, on ? 0x33FFFFFF : 0x0FFFFFFF);
    }
    if (!boxes) hud_ghosts(x, y, mw, mh);
    {
        float bx, by, bw, bh;
        minimap_box(l, sw, sh, &bx, &by, &bw, &bh);
        bx = x + bx * s; by = y + by * s; bw = std::max(8.0f, bw * s); bh = std::max(8.0f, bh * s);
        if (!l.on) ui_alpha(0.35f);
        minimap_draw_box(bx, by, bw, bh, sw, sh, l);
        ui_alpha(1.0f);
        const float fx0 = std::max(x + 2, bx - 3), fy0 = std::max(y + 2, by - 3);     // kept inside
        const float fx1 = std::min(x + mw - 2, bx + bw + 3), fy1 = std::min(y + mh - 2, by + bh + 3);
        ui_aa_stroke_rect(fx0, fy0, fx1 - fx0, fy1 - fy0, 3, 1.0f, hover ? 0xCCFFFFFF : 0x40FFFFFF);
    }
    ui_rect_border(x, y, mw, mh, 6, 1.0f, hover ? 0xFF3A3A3A : UI_LINE);
    if (hover) {
        const char* t = "CLICK TO EDIT";
        const float tw = ui_text_tracked_width(10, 700, t, 1.2f, UI_FONT_SANS) + 16;
        ui_aa_rect(x + (mw - tw) / 2, y + mh / 2 - 11, tw, 22, 5, 0xE6FFFFFF);
        ui_text_tracked(x + (mw - tw) / 2 + 8, y + mh / 2 - 7, 10, 700, 0xFF0A0A0A, t, 1.2f, UI_FONT_SANS);
    }
    if (hover && ui_input().clicked) hud_editor_open(true);

    // beside it: on / off, the editor
    const float cx = x + mw + 22, cw = w - mw - 22;
    const bool on = menu_toggle(cx, y + 2, "Minimap", l.on != 0);
    if (on != (l.on != 0)) { l.on = on; minimap_layout_set(l); }
    if (menu_button(cx, y + 44, std::min(220.0f, cw), 42, "Edit HUD layout", true)) hud_editor_open(true);
    menu_hint(cx, y + 100, "Minimap, compass, ammo, fps,");
    menu_hint(cx, y + 118, "lagometer: move, hide, restyle.");
    const char* src = minimap_source();
    char line[96];
    if (src[0]) snprintf(line, sizeof(line), "%s  -  %s", minimap_map(), src);
    else snprintf(line, sizeof(line), "radar drawn from the map in a game");
    menu_hint(cx, y + 142, line);
    y += std::max(mh, 162.0f);
    return y - y0;
}

void hud_editor_draw(float sw, float sh) {
    const UiInput& in = ui_input();
    hud_layout_preview();                         // the weapon list kept on screen meanwhile
    // the game stays visible: a veil, a faint grid in twelfths, the centre lines brighter
    ui_rect(0, 0, sw, sh, 0x59000000);
    for (int i = 1; i < 12; ++i) {
        ui_rect(roundf(sw * i / 12), 0, 1, sh, i == 6 ? 0x30FFFFFF : 0x12FFFFFF);
        ui_rect(0, roundf(sh * i / 12), sw, 1, i == 6 ? 0x30FFFFFF : 0x12FFFFFF);
    }

    // the side panel, on the other half of the screen than the selected element
    float bx = sw * 0.75f, by = 0, bw = 0, bh = 0;
    el_box(g_sel, sw, sh, &bx, &by, &bw, &bh);
    const float pw = 400;
    const float top_h = 24 + 36 + 28 + 22 + 5 * 36 + 10 + 22, bottom_h = 16 + 46 + 26;
    const float ph = top_h + options_height(g_sel) + bottom_h;
    const float ph_max = top_h + options_height(EL_MINIMAP) + bottom_h;  // the top stays put
    const float pxl = (bx + bw / 2 < sw / 2) ? sw - pw - 48 : 48;
    const float pyl = roundf(std::max(20.0f, (sh - ph_max) / 2));
    const bool over_panel = in.mx >= pxl && in.mx < pxl + pw && in.my >= pyl && in.my < pyl + ph;
    edit_screen(sw, sh, !over_panel || g_drag != 0);

    ui_shadow(pxl, pyl, pw, ph, 14, 24, 0xA0000000);
    ui_aa_rect(pxl, pyl, pw, ph, 14, 0xF20B0B0B);
    ui_aa_stroke_rect(pxl, pyl, pw, ph, 14, 1.0f, 0xFF2A2A2A);
    const float x = pxl + 28, w = pw - 56;
    float y = pyl + 24;
    ui_text(x, y, 24, 600, UI_TEXT, "HUD editor"); y += 36;
    ui_text(x, y, 13, 400, UI_MUTED, "Click an element, drag it. Saved as you go."); y += 28;

    menu_section(x, y, w, "ELEMENTS"); y += 22;
    const float cw = (w - 10) / 2;
    for (int el = EL_FIRST, i = 0; el < EL_END; ++el, ++i)
        element_cell(x + (i % 2) * (cw + 10), y + (i / 2) * 36, cw, el);
    y += 5 * 36 + 10;

    char sec[48];
    upper(el_label(g_sel), sec, sizeof(sec));
    menu_section(x, y, w, sec); y += 22;
    if (g_sel == EL_MINIMAP) minimap_options(x, y, w, sw, sh);
    else element_options(g_sel, x, y, w);

    y += 16;
    const float bw3 = (w - 20) / 3;
    if (menu_button(x, y, bw3, 46, "Reset", false)) el_reset(g_sel);
    if (menu_button(x + bw3 + 10, y, bw3, 46, "Reset all", false))
        for (int el = EL_FIRST; el < EL_END; ++el) el_reset(el);
    if (menu_button(x + 2 * (bw3 + 10), y, bw3, 46, "Done", true)) hud_editor_open(false);
}

}  // namespace patches
