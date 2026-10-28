// hud_editor.cpp - see hud_editor.h
#include "ui/hud_editor.h"
#include "ui/gl_overlay.h"
#include "ui/minimap.h"
#include "ui/modern_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace patches {

namespace {

constexpr DWORD UI_LINE  = 0xFF232323;
constexpr DWORD UI_EYEB  = 0xFF6B7280;
constexpr DWORD GUIDE    = 0xB0FFFFFF;

bool g_on = false;
MinimapLayout g_edit;                 // the layout being edited (the cvars follow it)
int   g_drag = 0;                     // 0 none, 1 move, 2..5 resize from a corner (TL TR BR BL)
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

// The live minimap box: dragged by its body, resized by a corner, snapped to the screen's
// edges (16 px at 1080p) and centre lines.
void edit_box(float sw, float sh, bool input) {
    const UiInput& in = ui_input();
    float bx, by, bw, bh;
    minimap_box(g_edit, sw, sh, &bx, &by, &bw, &bh);
    const float k = bh / std::max(1.0f, bw);
    const float S = std::max(0.5f, sh / 1080.0f);
    const float m = roundf(16 * S);

    // corners: TL TR BR BL (the handles sit 6 px outside the box)
    const float cx[4] = { bx - 6, bx + bw + 6, bx + bw + 6, bx - 6 }, cy[4] = { by - 6, by - 6, by + bh + 6, by + bh + 6 };
    int hot_corner = -1;
    if (input)
        for (int c = 0; c < 4; ++c)
            if (fabsf(in.mx - cx[c]) < 12 && fabsf(in.my - cy[c]) < 12) hot_corner = c;
    const bool over = input && in.mx >= bx && in.mx < bx + bw && in.my >= by && in.my < by + bh;

    if (input && in.clicked && !g_drag) {
        if (hot_corner >= 0) g_drag = 2 + hot_corner;
        else if (over) { g_drag = 1; g_grab_x = in.mx - bx; g_grab_y = in.my - by; }
    }
    if (g_drag && !in.down) {                       // released: saved
        g_drag = 0;
        minimap_layout_set(g_edit);
    }
    g_snap_x = g_snap_y = g_snap_cx = g_snap_cy = false;
    if (g_drag == 1) {
        float nx = in.mx - g_grab_x, ny = in.my - g_grab_y;
        const float snap = 10;
        if (fabsf(nx - m) < snap) { nx = m; g_snap_x = true; g_guide_x = m; }
        else if (fabsf(nx + bw - (sw - m)) < snap) { nx = sw - m - bw; g_snap_x = true; g_guide_x = sw - m; }
        else if (fabsf(nx + bw / 2 - sw / 2) < snap) { nx = sw / 2 - bw / 2; g_snap_cx = true; }
        if (fabsf(ny - m) < snap) { ny = m; g_snap_y = true; g_guide_y = m; }
        else if (fabsf(ny + bh - (sh - m)) < snap) { ny = sh - m - bh; g_snap_y = true; g_guide_y = sh - m; }
        else if (fabsf(ny + bh / 2 - sh / 2) < snap) { ny = sh / 2 - bh / 2; g_snap_cy = true; }
        g_edit.x = clampf(nx, 0, sw - bw) / std::max(1.0f, sw - bw);
        g_edit.y = clampf(ny, 0, sh - bh) / std::max(1.0f, sh - bh);
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

    minimap_box(g_edit, sw, sh, &bx, &by, &bw, &bh);
    if (!g_edit.on) ui_alpha(0.4f);
    minimap_draw_box(bx, by, bw, bh, sw, sh, g_edit);
    ui_alpha(1.0f);

    // selection frame, handles, size chip
    const bool hot = over || hot_corner >= 0 || g_drag;
    ui_aa_stroke_rect(bx - 6, by - 6, bw + 12, bh + 12, 6, 1.5f, hot ? 0xFFFFFFFF : 0x99FFFFFF);
    const float hx[4] = { bx - 6, bx + bw + 6, bx + bw + 6, bx - 6 }, hy[4] = { by - 6, by - 6, by + bh + 6, by + bh + 6 };
    for (int c = 0; c < 4; ++c) handle(hx[c], hy[c], hot_corner == c || g_drag == 2 + c);
    char chip[64];
    if (g_edit.on) snprintf(chip, sizeof(chip), "MINIMAP   %d PX", g_edit.size);
    else snprintf(chip, sizeof(chip), "MINIMAP   OFF");
    const float tw = ui_text_tracked_width(11, 700, chip, 1.2f, UI_FONT_SANS) + 18;
    const float chx = bx - 6;
    float chy = by + bh + 16;
    if (chy + 24 > sh) chy = by - 44;
    ui_aa_rect(chx, chy, tw, 24, 5, 0xE6FFFFFF);
    ui_text_tracked(chx + 9, chy + 4, 11, 700, 0xFF0A0A0A, chip, 1.2f, UI_FONT_SANS);

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

// The game's own HUD in faint outlines (in its 640x480 frame, the sides anchored to the
// screen's edges), so the miniature reads as a screen: crosshair, obituaries, compass,
// stance, health, ammo.
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

void screen_size(float* sw, float* sh) {
    RECT rc;
    *sw = 1920; *sh = 1080;
    if (GetClientRect(overlay_game_window(), &rc) && rc.right > 0 && rc.bottom > 0) { *sw = (float)rc.right; *sh = (float)rc.bottom; }
}

}  // namespace

bool hud_editor_active() { return g_on; }

void hud_editor_open(bool on) {
    if (on && !g_on) g_edit = minimap_layout();
    if (!on && g_on && g_drag) minimap_layout_set(g_edit);
    g_on = on;
    g_drag = 0;
}

float hud_editor_block(float x, float y, float w) {
    const float y0 = y;
    menu_section(x, y, w, "HUD"); y += 26;
    MinimapLayout l = minimap_layout();

    // the screen in miniature, the minimap live at its place; a click opens the editor
    float sw, sh;
    screen_size(&sw, &sh);
    const float mw = roundf(std::min(300.0f, w * 0.55f)), mh = roundf(mw * sh / sw);
    const bool hover = menu_hit(x, y, mw, mh);
    ui_aa_rect_vgrad(x, y, mw, mh, 6, 0xFF16181B, 0xFF0A0A0A);
    hud_ghosts(x, y, mw, mh);
    {
        float bx, by, bw, bh;
        minimap_box(l, sw, sh, &bx, &by, &bw, &bh);
        const float s = mw / sw;
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
    menu_hint(cx, y + 100, "CoD4 radar, zoomed on you,");
    menu_hint(cx, y + 118, "turning with your view.");
    const char* src = minimap_source();
    char line[96];
    if (src[0]) snprintf(line, sizeof(line), "%s  -  %s", minimap_map(), src);
    else snprintf(line, sizeof(line), "drawn from the map in a game");
    menu_hint(cx, y + 142, line);
    y += std::max(mh, 162.0f);
    return y - y0;
}

void hud_editor_draw(float sw, float sh) {
    const UiInput& in = ui_input();
    // the game stays visible: a veil, a faint grid in twelfths, the centre lines brighter
    ui_rect(0, 0, sw, sh, 0x73000000);
    for (int i = 1; i < 12; ++i) {
        ui_rect(roundf(sw * i / 12), 0, 1, sh, i == 6 ? 0x30FFFFFF : 0x12FFFFFF);
        ui_rect(0, roundf(sh * i / 12), sw, 1, i == 6 ? 0x30FFFFFF : 0x12FFFFFF);
    }

    // the side panel, on the other half of the screen than the minimap
    float bx, by, bw, bh;
    minimap_box(g_edit, sw, sh, &bx, &by, &bw, &bh);
    const float pw = 400, ph = 672;                     // = the rows below, top to bottom
    const float pxl = (bx + bw / 2 < sw / 2) ? sw - pw - 48 : 48;
    const float pyl = roundf(std::max(20.0f, (sh - ph) / 2));
    const bool over_panel = in.mx >= pxl && in.mx < pxl + pw && in.my >= pyl && in.my < pyl + ph;
    edit_box(sw, sh, !over_panel || g_drag != 0);

    ui_shadow(pxl, pyl, pw, ph, 14, 24, 0xA0000000);
    ui_aa_rect(pxl, pyl, pw, ph, 14, 0xF20B0B0B);
    ui_aa_stroke_rect(pxl, pyl, pw, ph, 14, 1.0f, 0xFF2A2A2A);
    const float x = pxl + 28, w = pw - 56;
    float y = pyl + 26;
    ui_text(x, y, 24, 600, UI_TEXT, "HUD editor"); y += 38;
    ui_text(x, y, 13, 400, UI_MUTED, "Drag the minimap, pull a corner to resize it."); y += 18;
    ui_text(x, y, 13, 400, UI_MUTED, "Saved as you go. ESC: back to the menu."); y += 30;

    MinimapLayout l = g_edit;
    const bool on = menu_toggle(x, y, "Minimap", l.on != 0);
    l.on = on;
    y += 44;

    menu_section(x, y, w, "SHAPE"); y += 22;
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

    // (a bigger box keeps its place in the room left: anchored to its edge, it grows inwards)
    const bool changed = l.on != g_edit.on || l.shape != g_edit.shape || l.rotate != g_edit.rotate ||
                         l.range != g_edit.range || l.size != g_edit.size || l.alpha != g_edit.alpha ||
                         l.mates != g_edit.mates;
    if (changed && !g_drag) { g_edit = l; minimap_layout_set(g_edit); }

    const char* src = minimap_source();
    char line[96];
    if (src[0]) snprintf(line, sizeof(line), "Radar: %s  -  %s", minimap_map(), src);
    else snprintf(line, sizeof(line), "No map loaded: join a game to see it live.");
    y += 30;
    ui_text(x, y, 12, 400, 0xFF5E5E5E, line);
    y += 28;

    const float by2 = y;
    if (menu_button(x, by2, (w - 12) / 2, 46, "Reset", false)) {
        g_edit = minimap_layout_default();
        minimap_layout_set(g_edit);
    }
    if (menu_button(x + (w - 12) / 2 + 12, by2, (w - 12) / 2, 46, "Done", true)) hud_editor_open(false);
}

}  // namespace patches
