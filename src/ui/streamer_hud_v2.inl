// streamer_hud_v2.inl - the streamer overlay, LAN look "War Room 1944" (2026-11-22).
// Included by streamer_hud.cpp inside its anonymous namespace: it shares its state (g_feed,
// g_slot, g_score...). Layout in 1080p units (S = window height / 1080), as the canvas
// "HUD streamer CoD1.6X", row V2 LAN: scorebug + round history + event strip top centre, radar
// top left, kill feed top right, the two teams bottom left / right, the followed player bottom
// centre; round over (winner, player of the round, the round's kills), kill replay (letterbox,
// timeline), TAB (the whole match), winning screen at the end of the map.
// Faces embedded in the DLL (ui/lan_assets.h): Big Shoulders (display), Big Shoulders Stencil
// (scores), Barlow Semi Condensed (small text). Weapons: silhouettes from the 3D models.

// ------------------------------------------------------------------ type
constexpr int F_DISP = UI_FONT_DISPLAY, F_BLACK = UI_FONT_DISPLAY_BLACK, F_STEN = UI_FONT_STENCIL,
              F_BODY = UI_FONT_BODY, F_BOLD = UI_FONT_BODY_BOLD;

float vt_w(int font, float size, const char* s, float track = 0) {
    return track > 0 ? ui_text_tracked_width(px(size), 400, s, track * S, font)
                     : ui_text_font_width(px(size), 400, s, font);
}
// text with its caps and figures centred on cy; returns the width. GDI's line box (win ascent +
// descent) puts the caps below its middle by (ascent - descent - cap height) / 2: 0.071 em for
// Big Shoulders (1.307 / 0.365 / 0.800), 0.064 em for Barlow (1.089 / 0.261 / 0.700)
float vt(float x, float cy, int font, float size, DWORD col, const char* s, int align = -1, float track = 0) {
    if (!s || !*s) return 0;
    const int p = px(size);
    const float h = ui_text_font_height(p, 400, font);
    const float w = vt_w(font, size, s, track);
    const float x0 = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    const float y0 = cy - h / 2 - p * (font == F_BODY || font == F_BOLD ? 0.064f : 0.071f);
    if (track > 0) ui_text_tracked(x0, y0, p, 400, col, s, track * S, font);
    else           ui_text_font(x0, y0, p, 400, col, s, font);
    return w;
}

// the radar and the X-ray still call these
float ctext_w(float size, int weight, const char* s, float tracking = 0) {
    (void)weight;
    return vt_w(F_DISP, size, s, tracking);
}
float ctext(float x, float cy, float size, int weight, DWORD col, const char* s, int align = -1, float tracking = 0) {
    (void)weight;
    return vt(x, cy, F_DISP, size, col, s, align, tracking);
}

// ------------------------------------------------------------------ shapes
// a plate with cut corners: tl / tr / br / bl = length of each cut (0 = square)
int plate_pts(float x, float y, float w, float h, float tl, float tr, float br, float bl, float* p) {
    int n = 0;
    auto add = [&](float a, float b) { p[n++] = a; p[n++] = b; };
    if (tl > 0) { add(x, y + tl); add(x + tl, y); } else add(x, y);
    if (tr > 0) { add(x + w - tr, y); add(x + w, y + tr); } else add(x + w, y);
    if (br > 0) { add(x + w, y + h - br); add(x + w - br, y + h); } else add(x + w, y + h);
    if (bl > 0) { add(x + bl, y + h); add(x, y + h - bl); } else add(x, y + h);
    return n / 2;
}
void plate(float x, float y, float w, float h, float tl, float tr, float br, float bl, DWORD top, DWORD bottom) {
    float p[16];
    const int n = plate_pts(x, y, w, h, tl, tr, br, bl, p);
    ui_aa_poly_vgrad(p, n, top, bottom);
}
void plate_stroke(float x, float y, float w, float h, float tl, float tr, float br, float bl, float th, DWORD c) {
    float p[16];
    const int n = plate_pts(x, y, w, h, tl, tr, br, bl, p);
    ui_aa_poly_stroke(p, n, th, c);
}
// a camera's corners: an L at each corner of the frame
void viewfinder(float x, float y, float w, float h, float arm, float th, DWORD c) {
    ui_rect(x, y, arm, th, c);             ui_rect(x, y, th, arm, c);
    ui_rect(x + w - arm, y, arm, th, c);   ui_rect(x + w - th, y, th, arm, c);
    ui_rect(x, y + h - th, arm, th, c);    ui_rect(x, y + h - arm, th, arm, c);
    ui_rect(x + w - arm, y + h - th, arm, th, c); ui_rect(x + w - th, y + h - arm, th, arm, c);
}

// ------------------------------------------------------------------ glyphs
void glyph_skull(float cx, float cy, float s, DWORD c, DWORD hole) {
    ui_aa_disc(cx, cy - s * 0.08f, s * 0.36f, c);
    ui_aa_rect(cx - s * 0.19f, cy + s * 0.12f, s * 0.38f, s * 0.26f, s * 0.06f, c);
    ui_aa_disc(cx - s * 0.13f, cy - s * 0.07f, s * 0.095f, hole);
    ui_aa_disc(cx + s * 0.13f, cy - s * 0.07f, s * 0.095f, hole);
}
void glyph_headshot(float cx, float cy, float r, DWORD c) {       // a crosshair on the head
    const float th = std::max(1.0f, r * 0.22f);
    ui_aa_ring(cx, cy, r * 0.72f, th, c);
    ui_aa_disc(cx, cy, r * 0.22f, c);
    ui_aa_line(cx - r * 1.25f, cy, cx - r * 0.72f, cy, th, c);
    ui_aa_line(cx + r * 0.72f, cy, cx + r * 1.25f, cy, th, c);
    ui_aa_line(cx, cy - r * 1.25f, cx, cy - r * 0.72f, th, c);
    ui_aa_line(cx, cy + r * 0.72f, cx, cy + r * 1.25f, th, c);
}
void glyph_bomb(float cx, float cy, float s, DWORD c) {
    ui_aa_disc(cx - s * 0.06f, cy + s * 0.1f, s * 0.31f, c);
    ui_aa_line(cx + s * 0.16f, cy - s * 0.15f, cx + s * 0.3f, cy - s * 0.29f, std::max(1.0f, s * 0.1f), c);
    ui_aa_disc(cx + s * 0.34f, cy - s * 0.33f, s * 0.07f, c);
}
// how a round was won: elimination (skull), bomb, defuse (cutters), time (clock)
void glyph_reason(float cx, float cy, float s, char r, DWORD c, DWORD bg) {
    const float th = std::max(1.0f, s * 0.12f);
    switch (r) {
    case 'e': glyph_skull(cx, cy, s, c, bg); break;
    case 'b': glyph_bomb(cx, cy, s, c); break;
    case 'd':
        ui_aa_line(cx - s * 0.3f, cy - s * 0.3f, cx + s * 0.3f, cy + s * 0.3f, th, c);
        ui_aa_line(cx + s * 0.3f, cy - s * 0.3f, cx - s * 0.3f, cy + s * 0.3f, th, c);
        ui_aa_disc(cx - s * 0.3f, cy - s * 0.3f, s * 0.12f, c);
        ui_aa_disc(cx + s * 0.3f, cy - s * 0.3f, s * 0.12f, c);
        break;
    default:
        ui_aa_ring(cx, cy, s * 0.34f, th, c);
        ui_aa_line(cx, cy, cx, cy - s * 0.2f, th, c);
        ui_aa_line(cx, cy, cx + s * 0.15f, cy + s * 0.1f, th, c);
        break;
    }
}

// ------------------------------------------------------------------ words
const char* team_word(int team) { return team == 2 ? "AXIS" : "ALLIES"; }
// the PAM's team name (a clan tag when both sides have one), else ALLIES / AXIS
std::string team_label(int side) {
    const std::string nm = upper(g_name[side]);
    return nm.empty() ? team_word(g_team_side[side]) : nm;
}
// "mp40_mp" / "mp40" -> "MP40"
std::string weapon_label(const std::string& w) {
    std::string s = w;
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "_mp") == 0) s.resize(s.size() - 3);
    for (char& c : s) if (c == '_') c = ' ';
    return upper(s);
}
// the weapon file of a kill feed name (the PAM sends it without "_mp")
std::string weapon_file(const std::string& w) {
    if (w.size() > 3 && w.compare(w.size() - 3, 3, "_mp") == 0) return w;
    return w + "_mp";
}
// the team's mark in its tile: "TEAM 1" -> "T1", "ALLIES" -> "AL", "BIG FROGS" -> "BF"
std::string monogram(const std::string& name) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_' || c == '.') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else if (isalnum((unsigned char)c)) cur += (char)toupper((unsigned char)c);
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty()) return "?";
    if (words.size() == 1) return words[0].substr(0, std::min<size_t>(3, words[0].size()));
    const std::string& last = words.back();
    const bool num = isdigit((unsigned char)last[0]) != 0;
    return words[0].substr(0, 1) + (num ? last.substr(0, 2) : last.substr(0, 1));
}
std::string map_label() {
    std::string m = g_io->cvar("mapname");
    if (m.rfind("mp_", 0) == 0) m = m.substr(3);
    for (char& c : m) if (c == '_') c = ' ';
    return upper(m);
}
// the grenade a team throws (its silhouette)
const char* team_nade(int team) { return team == 2 ? "stielhandgranate_mp" : "fraggrenade_mp"; }

// who killed slot k this round (the last kill of him in the feed): attacker slot, weapon, hs
const KillEv* killed_by(int k) {
    for (int i = (int)g_feed.kills.size() - 1; i >= 0; --i)
        if (g_feed.kills[i].vic == k) return &g_feed.kills[i];
    return nullptr;
}

// a team's tile: the monogram over diagonal stripes in its colour
void team_tile(float x, float y, float s, DWORD tc, const std::string& mark, float cut = 0) {
    if (cut > 0) plate(x, y, s, s, cut, 0, cut, 0, with_alpha(tc, 0.14f), with_alpha(tc, 0.08f));
    else ui_rect(x, y, s, s, with_alpha(tc, 0.10f));
    ui_clip_push(x, y, s, s);
    stripes(x, y, s, s, 0.16f, tc);
    ui_clip_pop();
    if (cut > 0) plate_stroke(x, y, s, s, cut, 0, cut, 0, std::max(1.0f, 2 * S), tc);
    else ui_aa_stroke_rect(x, y, s, s, 0, std::max(1.0f, 1 * S), with_alpha(tc, 0.55f));
    vt(x + s / 2, y + s / 2, F_BLACK, s * 0.45f / S, tc, mark.c_str(), 0, 1);
}

// the clock in tenths, as text ("1:32", "9.4" for the last ten seconds of the bomb)
void clock_text(int tenths, bool bomb, char* out, int n) {
    if (tenths < 0) { snprintf(out, n, "-:--"); return; }
    const int secs = (tenths + 9) / 10;
    if (bomb && tenths < 100) snprintf(out, n, "%d.%d", tenths / 10, tenths % 10);
    else snprintf(out, n, "%d:%02d", secs / 60, secs % 60);
}

// ------------------------------------------------------------------ round history
// one cell per round of the match: winner colour + how; halftime gap; the round being played
// outlined. Returns the bottom.
float draw_history_strip(float x, float y, float W, float H, bool numbers) {
    const int played = (int)g_feed.hist.size();
    int total = g_feed.rounds > 0 ? g_feed.rounds : played + 1;
    total = std::max(total, played + (g_feed.mapover ? 0 : 1));
    if (total <= 0) return y;
    const bool has_half = g_feed.half > 0 && g_feed.half < total;
    const float gap = 3 * S, halfgap = 10 * S;
    const float bw = (W - (total - 1) * gap - (has_half ? halfgap : 0)) / total;
    const int now = g_feed.mapover ? -1 : played;
    float xx = x;
    for (int i = 0; i < total; ++i) {
        if (i < played) {
            // the PAM writes the history in the sides of the first half (S field): flipped back
            // by the switches since, each round wears the colour its winner wears NOW - one team,
            // one colour along the whole strip (enzo 2026-11-23: mixed after the half-time)
            char win = g_feed.hist[i].first;
            if (g_feed.swaps & 1) win = win == 'a' ? 'x' : win == 'x' ? 'a' : win;
            const DWORD c = win == 'x' ? COL_AXIS : win == 'a' ? COL_ALLIES : COL_MUTED;
            ui_rect(xx, y, bw, H, with_alpha(c, 0.17f));
            ui_rect(xx, y + H - 2 * S, bw, 2 * S, c);
            glyph_reason(xx + bw / 2, y + H / 2 - 1 * S, std::min(bw, H) * 0.56f, g_feed.hist[i].second, c, COL_INK);
        } else {
            ui_rect(xx, y, bw, H, i == now ? with_alpha(COL_TEXT, 0.10f) : with_alpha(COL_DEEP, 0.6f));
            if (i == now) {
                ui_aa_stroke_rect(xx, y, bw, H, 0, std::max(1.0f, 1 * S), COL_TEXT);
                char r[8];
                snprintf(r, sizeof(r), "%d", i + 1);
                vt(xx + bw / 2, y + H / 2, F_DISP, std::min(13.0f, H / S * 0.45f), COL_TEXT, r, 0);
            }
        }
        if (numbers && i != now) {
            char r[8];
            snprintf(r, sizeof(r), "%d", i + 1);
            vt(xx + 4 * S, y + 8 * S, F_BOLD, 9.5f, with_alpha(COL_TEXT, 0.4f), r);
        }
        xx += bw + gap;
        if (has_half && i + 1 == g_feed.half) xx += halfgap;
    }
    return y + H;
}

// the PAM's event line (scr_streamer_text*, one piece per "|") and the map; returns the bottom
float draw_event_strip(float cx, float y) {
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : g_event_line) {
            if (c == '|') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) parts.push_back(cur);
    }
    if (parts.empty()) return y;
    parts.push_back(map_label());
    const float h = 26 * S, sep = 4 * S, gap = 12 * S, pad = 18 * S;
    float w = 2 * pad;
    for (size_t i = 0; i < parts.size(); ++i)
        w += vt_w(F_BOLD, 13, upper(clean(parts[i].c_str())).c_str(), 3) + (i ? gap * 2 + sep : 0);
    const float x = cx - w / 2;
    const float p[8] = { x + 10 * S, y, x + w - 10 * S, y, x + w, y + h, x, y + h };
    ui_aa_poly(p, 4, with_alpha(COL_DEEP, 0.8f));
    float xx = x + pad;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            xx += gap;
            ui_rect(xx, y + h / 2 - sep / 2, sep, sep, COL_MUTED);
            xx += sep + gap;
        }
        xx += vt(xx, y + h / 2, F_BOLD, 13, i == 0 ? COL_TEXT : COL_MUTED, upper(clean(parts[i].c_str())).c_str(), -1, 3);
    }
    return y + h;
}

// ------------------------------------------------------------------ scorebug
// [tile, team, side + alive | score] [phase, clock, round] [score | team, side + alive, tile],
// the round history under it, the event strip under that. Returns the bottom.
float draw_scorebug(float vw) {
    const float top = 16 * S, h = 88 * S, tw = 420 * S, cw = 240 * S, W = 2 * tw + cw;
    const float cx = vw / 2, x0 = cx - W / 2;
    const bool bomb = g_feed.clock_kind == 'b';
    const bool over = g_feed.res.valid;
    ui_shadow(x0, top, W, h, 0, 24 * S, 0x8C000000);
    for (int side = 0; side < 2; ++side) {
        const int team = g_team_side[side];
        const DWORD tc = team_col(team);
        const float bx = side == 0 ? x0 : cx + cw / 2;
        plate(bx, top, tw, h, side == 0 ? 26 * S : 0, side == 1 ? 26 * S : 0, 0, 0, COL_PANEL_T, COL_PANEL_B);
        ui_rect(bx, top + h - 4 * S, tw, 4 * S, tc);
        // the score at the inner edge
        const float sz = 104 * S, szx = side == 0 ? bx + tw - sz : bx;
        if (side == 0) ui_rect_hgrad(szx, top, sz, h - 4 * S, with_alpha(tc, 0.0f), with_alpha(tc, 0.14f));
        else           ui_rect_hgrad(szx, top, sz, h - 4 * S, with_alpha(tc, 0.14f), with_alpha(tc, 0.0f));
        const float sage = (GetTickCount() - g_score_at[side]) / 1000.0f;
        if (g_score_at[side] && sage < 1.4f)
            ui_shadow(szx + 16 * S, top + 14 * S, sz - 32 * S, h - 30 * S, 6 * S, 26 * S, with_alpha(tc, 0.8f * (1 - sage / 1.4f)));
        const char* sc = g_score[side].empty() ? "0" : g_score[side].c_str();
        vt(szx + sz / 2, top + h / 2 - 2 * S, F_STEN, 76, COL_TEXT, sc, 0);
        // tile, name, side + alive
        const float ts = 58 * S, tx = side == 0 ? bx + 22 * S : bx + tw - 22 * S - ts, ty = top + (h - 4 * S - ts) / 2;
        const std::string nm = team_label(side);
        team_tile(tx, ty, ts, tc, monogram(nm));
        const float nx = side == 0 ? tx + ts + 16 * S : tx - 16 * S;
        const int al = side == 0 ? -1 : 1;
        const float lim = side == 0 ? szx - 10 * S - nx : nx - (szx + sz + 10 * S);
        ui_clip_push(side == 0 ? nx - 6 * S : nx - lim, top, std::max(0.0f, lim) + 6 * S, h);
        vt(nx, top + 32 * S, F_DISP, 30, COL_TEXT, nm.c_str(), al, 2);
        ui_clip_pop();
        const float sw = vt(nx, top + 62 * S, F_BOLD, 13, tc, team_word(team), al, 3.5f);
        const int alive = alive_count(side), total = std::max(5, present_count(side));
        const float pw = 6 * S, pg = 3 * S, ph = 14 * S, pws = total * pw + (total - 1) * pg;
        const float pxx = side == 0 ? nx + sw + 10 * S : nx - sw - 10 * S - pws;
        for (int n = 0; n < total; ++n)
            ui_rect(pxx + n * (pw + pg), top + 62 * S - ph / 2, pw, ph, n < alive ? tc : with_alpha(COL_TEXT, 0.16f));
    }
    // the clock
    const float ccx = cx - cw / 2;
    if (bomb) ui_rect_vgrad(ccx, top, cw, h, 0xFF3A1010, 0xFF1C0808);
    else      ui_rect(ccx, top, cw, h, COL_DEEP);
    stripes(ccx, top, cw, h, 0.035f, COL_TEXT);
    const int tenths = clock_tenths_now();
    char t[16];
    clock_text(tenths, bomb, t, sizeof(t));
    std::string phase = "LIVE";
    DWORD pc = COL_TEXT;
    if (bomb) {
        phase = "BOMB PLANTED";
        pc = COL_DANGER;
        float best = 1e18f;
        int site = -1;
        for (int k = 0; k < 2; ++k) {
            bool have = g_radar_ok && g_radar.site[k];
            float sxw = have ? g_radar.site_x[k] : 0, syw = have ? g_radar.site_y[k] : 0;
            if (g_win.valid && g_win.site[k]) { have = true; sxw = g_win.site_x[k]; syw = g_win.site_y[k]; }
            if (!have || !g_feed.bomb) continue;
            const float d = (sxw - g_feed.bx) * (sxw - g_feed.bx) + (syw - g_feed.by) * (syw - g_feed.by);
            if (d < best) { best = d; site = k; }
        }
        if (site >= 0) phase += site ? " \xC2\xB7 B" : " \xC2\xB7 A";
        if (g_bomb_total > 0 && tenths >= 0) {
            const float fr = std::min(1.0f, std::max(0.0f, (float)tenths / g_bomb_total));
            ui_rect(ccx, top + h - 4 * S, cw, 4 * S, 0xFF3A1212);
            ui_rect(ccx, top + h - 4 * S, cw * fr, 4 * S, COL_DANGER);
        }
    } else if (over) {
        phase = "ROUND OVER";
    } else if (g_feed.clock_kind == 's') {
        phase = "STRAT TIME";
    } else {
        const std::string info = upper(g_info);
        if (info == "READY-UP" || info == "BASH" || info == "FINAL" || info == "TIMEOUT") phase = info;
    }
    {
        const float lw = vt_w(F_BOLD, 12, phase.c_str(), 4);
        const float dot = 3.5f * S, total = dot * 2 + 8 * S + lw, lx0 = cx - total / 2;
        const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(GetTickCount() * 0.004f));
        ui_aa_disc(lx0 + dot, top + 17 * S, dot, with_alpha(pc, phase == "LIVE" || bomb ? pulse : 0.6f));
        vt(lx0 + dot * 2 + 8 * S, top + 17 * S, F_BOLD, 12, pc, phase.c_str(), -1, 4);
    }
    if (over) {
        const char* why = g_feed.res.reason == 'e' ? "ELIMINATION" : g_feed.res.reason == 'b' ? "BOMB" : g_feed.res.reason == 'd' ? "DEFUSED" : "TIME";
        const DWORD wc = g_feed.res.winner == 'x' ? COL_AXIS : g_feed.res.winner == 'a' ? COL_ALLIES : COL_MUTED;
        vt(cx, top + 49 * S, F_BLACK, 34, wc, why, 0, 2);
    } else {
        const DWORD tcol = tenths < 0 ? 0xFF4A4F55 : bomb ? 0xFFFF6A60 : (g_feed.clock_kind == 'r' && tenths < 100) ? COL_DANGER : COL_TEXT;
        vt(cx, top + 47 * S, F_BLACK, 46, tcol, t, 0, 1);
    }
    {
        char b[48] = "";
        if (over && g_feed.round > 0) snprintf(b, sizeof(b), "NEXT \xC2\xB7 ROUND %d", g_feed.round + 1);
        else if (g_feed.round > 0 && g_feed.rounds > 1) snprintf(b, sizeof(b), "ROUND %d / %d", g_feed.round, g_feed.rounds);
        else if (g_feed.round > 0) snprintf(b, sizeof(b), "ROUND %d", g_feed.round);
        if (b[0]) vt(cx, top + 77 * S, F_BODY, 12, COL_MUTED, b, 0, 3.5f);
    }
    float y = top + h + 6 * S;
    if (!g_feed.hist.empty() || g_feed.rounds > 0) y = draw_history_strip(x0, y, W, 30 * S, false);
    if (!g_event_line.empty()) y = draw_event_strip(cx, y + 6 * S);
    return y;
}

// ------------------------------------------------------------------ kill feed
void draw_killfeed(float vw, float vh) {
    const DWORD now = GetTickCount();
    const float right = vw - 24 * S, h = 40 * S, gap = 6 * S, pad = 14 * S, ih = 20 * S;
    const float avail = std::max(220 * S, right - (vw / 2 + 540 * S + 14 * S));   // never into the scorebug
    const float bottom = vh - 24 * S - 5 * (56 + 5) * S - 16 * S;                    // above the right team
    float y = 24 * S;
    for (int i = (int)g_feed.kills.size() - 1; i >= 0 && y + h <= bottom; --i) {
        const KillEv& k = g_feed.kills[i];
        const auto seen = g_kill_seen.find(k.id);
        const float age = seen == g_kill_seen.end() ? 1.0f : (now - seen->second) / 1000.0f;
        const float a = std::min(1.0f, age / 0.15f);
        const float slide = age < 0.3f ? (1 - age / 0.3f) * (1 - age / 0.3f) * 64 * S : 0;
        const float flash = age < 0.8f ? 1 - age / 0.8f : 0;
        const bool att_ok = k.att >= 0 && k.att < 10 && g_slot[k.att].present;
        const bool vic_ok = k.vic >= 0 && k.vic < 10 && g_slot[k.vic].present;
        const std::string an = att_ok ? upper(g_slot[k.att].name) : "";
        const std::string vn = vic_ok ? upper(g_slot[k.vic].name) : "?";
        const DWORD ac = att_ok ? team_col(slot_team(k.att)) : COL_MUTED;
        const DWORD vc = vic_ok ? team_col(slot_team(k.vic)) : COL_MUTED;
        const Icon* wi = nullptr;
        std::string wl;
        float iw = 0;
        if (k.weapon != "-" && k.weapon != "melee") {
            wi = &weapon_icon(weapon_file(k.weapon));
            if (wi->ok) iw = std::min(118 * S, ih * wi->aspect);
        }
        if (iw <= 0) {
            wl = k.weapon == "melee" ? "BASH" : k.weapon == "-" ? "DIED" : weapon_label(k.weapon);
            iw = vt_w(F_BOLD, 12, wl.c_str(), 2) + 12 * S;
        }
        const float hsw = k.hs ? 26 * S : 0;
        float naw = an.empty() ? 0 : vt_w(F_DISP, 19, an.c_str(), 1.2f);
        float nvw = vt_w(F_DISP, 19, vn.c_str(), 1.2f);
        const float fixed = 2 * pad + iw + hsw + (an.empty() ? 12 * S : 24 * S);
        if (fixed + naw + nvw > avail) {                       // long names: clipped, never over the scorebug
            const float room = std::max(40 * S, (avail - fixed) / (an.empty() ? 1 : 2));
            naw = std::min(naw, room);
            nvw = std::min(nvw, room);
        }
        const float w = fixed + naw + nvw;
        const float x = right - w + slide, cy = y + h / 2;
        const bool mine = g_feed.cam >= 0 && (k.att == g_feed.cam || k.vic == g_feed.cam);
        const bool newest = i == (int)g_feed.kills.size() - 1;
        if (flash > 0) ui_shadow(x, y, w, h, 0, 16 * S, with_alpha(k.hs ? COL_DANGER : COL_TEXT, 0.4f * flash));
        const DWORD bg = mine && newest ? with_alpha(0xFF2A2D31, 0.94f * a) : with_alpha(COL_INK, 0.88f * a);
        plate(x, y, w, h, 0, 0, 0, 10 * S, bg, bg);
        if (mine) plate_stroke(x + 1 * S, y + 1 * S, w - 2 * S, h - 2 * S, 0, 0, 0, 9 * S, 2 * S, with_alpha(COL_TEXT, (newest ? 1.0f : 0.55f) * a));
        float xx = x + pad;
        if (naw > 0) {
            ui_clip_push(xx - 4 * S, y, naw + 8 * S, h);
            vt(xx, cy, F_DISP, 19, with_alpha(ac, a), an.c_str(), -1, 1.2f);
            ui_clip_pop();
            xx += naw + 12 * S;
        }
        if (wl.empty() && wi) draw_icon(*wi, xx, cy - ih / 2, iw, ih, -1, with_alpha(0xF2EEEAE2, a));
        else {
            ui_rect(xx, y + 9 * S, iw, h - 18 * S, with_alpha(0x18FFFFFF, a));
            vt(xx + iw / 2, cy, F_BOLD, 12, with_alpha(COL_SOFT, a), wl.c_str(), 0, 2);
        }
        xx += iw + 12 * S;
        if (k.hs) { glyph_headshot(xx + 9 * S, cy, 8 * S, with_alpha(COL_DANGER, a)); xx += hsw; }
        ui_clip_push(xx - 4 * S, y, nvw + 8 * S, h);
        vt(xx, cy, F_DISP, 19, with_alpha(vc, a), vn.c_str(), -1, 1.2f);
        ui_clip_pop();
        y += h + gap;
    }
}

// ------------------------------------------------------------------ teams
// one player's row (right = the right-hand team, mirrored)
void draw_player_row(float x, float y, float w, float h, int k, bool right) {
    const Slot& s = g_slot[k];
    const FeedPlayer& f = g_feed.p[k];
    const int team = slot_team(k);
    const DWORD tc = team_col(team);
    const bool followed = s.followed || g_feed.cam == k;
    const bool alive = s.alive;
    const float cy = y + h / 2, dir = right ? -1.0f : 1.0f;
    const float prev_alpha = ui_alpha_get();
    if (!alive) ui_alpha(prev_alpha * 0.62f);

    ui_rect(x, y, w, h, with_alpha(COL_INK, 0.9f));
    if (alive) {
        const float shown = ui_smooth(0x57B0 + k, (float)s.hp, 9.0f);
        const float ghost = std::max(shown, ui_smooth(0x57D0 + k, (float)s.hp, 1.4f));
        const float fw = w * std::min(1.0f, std::max(0.0f, shown / 100.0f));
        const float gw = w * std::min(1.0f, std::max(0.0f, (ghost - shown) / 100.0f));
        if (!right) {
            ui_rect_hgrad(x, y, fw, h, with_alpha(tc, 0.30f), with_alpha(tc, 0.10f));
            ui_rect(x + fw, y, gw, h, with_alpha(COL_TEXT, 0.22f));
            ui_rect(x, y + h - 3 * S, fw, 3 * S, s.hp <= 25 ? COL_DANGER : tc);
        } else {
            ui_rect_hgrad(x + w - fw, y, fw, h, with_alpha(tc, 0.10f), with_alpha(tc, 0.30f));
            ui_rect(x + w - fw - gw, y, gw, h, with_alpha(COL_TEXT, 0.22f));
            ui_rect(x + w - fw, y + h - 3 * S, fw, 3 * S, s.hp <= 25 ? COL_DANGER : tc);
        }
    } else {
        ui_rect(x, y, w, h, 0x0A000000);
    }
    // the number key
    const float kw = 30 * S, kx = right ? x + w - kw : x;
    ui_rect(kx, y, kw, h, alive ? tc : COL_DIMKEY);
    char num[4];
    snprintf(num, sizeof(num), "%d", (k + 1) % 10);
    vt(kx + kw / 2, cy, F_BLACK, 18, COL_DEEP, num, 0);
    // health, far side
    const float hpx = right ? x + 14 * S : x + w - 14 * S;
    if (alive) {
        char hp[8];
        snprintf(hp, sizeof(hp), "%d", s.hp);
        vt(hpx, cy, F_BLACK, 26, s.hp <= 25 ? COL_DANGER : COL_TEXT, hp, right ? -1 : 1);
    }
    // gear: grenades, then the gun in hand - from the health column INWARD, toward the name
    // (the health sits at the far edge: inward = -dir)
    const float in = -dir;
    float gx = hpx + in * 50 * S;                            // the inner edge of the health column
    if (alive) {
        const int nn = std::min(3, f.nades >= 0 ? f.nades : s.nades);
        const Icon& ni = weapon_icon(team_nade(team));
        const float nh = 18 * S, nw = ni.ok ? nh * ni.aspect : 0;
        for (int n = 0; n < nn && ni.ok; ++n) {
            draw_icon(ni, right ? gx : gx - nw, y + (h - nh) / 2 + 1 * S, nw, nh, 0, 0xE6EEEAE2);
            gx += in * (nw + 3 * S);
        }
        if (nn > 0) gx += in * 7 * S;
        if (!f.weapon.empty()) {
            const Icon& wi = weapon_icon(weapon_file(f.weapon));
            if (wi.ok) {
                const float iw = std::min(118 * S, 22 * S * wi.aspect);
                draw_icon(wi, right ? gx : gx - iw, cy - 11 * S, iw, 22 * S, right ? -1 : 1, 0xF0EEEAE2, right);
                gx += in * (iw + 10 * S);
            }
        }
    }
    // name + the line under it, clipped to what is left (a margin on the anchored side: the
    // last glyph's ink may pass its advance by a pixel or two)
    const float tx = right ? kx - 12 * S : kx + kw + 12 * S;
    const int al = right ? 1 : -1;
    const float lo = right ? gx : tx - 6 * S, hi = right ? tx + 6 * S : gx;
    ui_clip_push(lo, y, std::max(0.0f, hi - lo), h);
    float nx = tx;
    if (!alive) {
        glyph_skull(nx + dir * 8 * S, y + h * 0.36f, 16 * S, COL_MUTED, COL_INK);
        nx += dir * 21 * S;
    }
    const std::string nm = upper(s.name);
    const float nw = vt(nx, y + h * 0.36f, F_DISP, 20, alive ? COL_TEXT : COL_MUTED, nm.c_str(), al, 1.2f);
    if (followed) {
        const float bw = vt_w(F_BOLD, 11, "POV", 2.5f) + 10 * S, bx = right ? nx - nw - 8 * S - bw : nx + nw + 8 * S;
        ui_rect(bx, y + h * 0.36f - 8 * S, bw, 16 * S, COL_TEXT);
        vt(bx + bw / 2, y + h * 0.36f, F_BOLD, 11, COL_DEEP, "POV", 0, 2.5f);
    }
    char line[96] = "";
    if (alive) {
        if (f.ktot >= 0) snprintf(line, sizeof(line), "K %d \xC2\xB7 D %d \xC2\xB7 ADR %d", f.ktot, std::max(0, f.deaths), std::max(0, f.adr));
        else if (!f.weapon.empty()) snprintf(line, sizeof(line), "%s", weapon_label(f.weapon).c_str());
    } else {
        const KillEv* kb = killed_by(k);
        if (kb && kb->att >= 0 && kb->att < 10 && g_slot[kb->att].present)
            snprintf(line, sizeof(line), "KILLED BY %s%s", upper(clean(g_slot[kb->att].name.c_str())).c_str(), kb->hs ? " \xC2\xB7 HS" : "");
        else snprintf(line, sizeof(line), "DEAD");
    }
    vt(tx, y + h * 0.72f, F_BODY, 12, COL_MUTED, line, al, 1.5f);
    ui_clip_pop();
    // hit / death flashes
    {
        const DWORD now = GetTickCount();
        const float hit = g_hit_at[k] ? (now - g_hit_at[k]) / 1000.0f : 9;
        const float die = g_death_at[k] ? (now - g_death_at[k]) / 1000.0f : 9;
        if (hit < 0.45f && alive) ui_rect(x, y, w, h, with_alpha(COL_DANGER, 0.35f * (1 - hit / 0.45f)));
        if (die < 0.8f) ui_rect(x, y, w, h, with_alpha(COL_TEXT, 0.45f * (1 - die / 0.8f)));
    }
    if (!alive) ui_alpha(prev_alpha);
    if (followed) ui_aa_stroke_rect(x, y, w, h, 0, 2 * S, COL_TEXT);
}

void draw_columns(float vw, float vh) {
    const float w = 456 * S, h = 56 * S, gap = 5 * S, bottom = vh - 24 * S;
    for (int side = 0; side < 2; ++side) {
        int n = 0;
        for (int i = 0; i < 5; ++i) if (g_slot[side * 5 + i].present) ++n;
        if (!n) continue;
        float y = bottom - n * h - (n - 1) * gap;
        const float x = side == 0 ? 24 * S : vw - 24 * S - w;
        for (int i = 0; i < 5; ++i) {
            const int k = side * 5 + i;
            if (!g_slot[k].present) continue;
            draw_player_row(x, y, w, h, k, side == 1);
            y += h + gap;
        }
    }
}

// ------------------------------------------------------------------ the followed player
void draw_followed(float vw, float vh) {
    const int k = g_feed.cam;
    if (k < 0 || k >= 10 || !g_slot[k].present) return;
    const Slot& s = g_slot[k];
    const FeedPlayer& f = g_feed.p[k];
    const int team = slot_team(k);
    const DWORD tc = team_col(team);
    const float w = 780 * S, h = 144 * S, x = vw / 2 - w / 2, y = vh - 24 * S - h;
    ui_shadow(x, y, w, h, 0, 26 * S, 0x8C000000);
    plate(x, y, w, h, 24 * S, 24 * S, 0, 0, COL_PANEL_T, COL_PANEL_B);
    ui_rect(x + 24 * S, y, w - 48 * S, 4 * S, tc);

    // health
    const float hx = x + 30 * S;
    if (s.alive) {
        char hp[8];
        snprintf(hp, sizeof(hp), "%d", s.hp);
        const DWORD hc = s.hp <= 25 ? COL_DANGER : COL_TEXT;
        const float hw = vt(hx, y + 56 * S, F_BLACK, 76, hc, hp);
        vt(hx + hw + 8 * S, y + 72 * S, F_BOLD, 13, COL_MUTED, "HP", -1, 3);
        const float bw = 140 * S, by = y + 104 * S;
        ui_rect(hx, by, bw, 8 * S, 0xFF232933);
        const float shown = ui_smooth(0x57C0 + k, (float)s.hp, 9.0f);
        const float ghost = std::max(shown, ui_smooth(0x57E0 + k, (float)s.hp, 1.4f));
        const float fr = std::min(1.0f, std::max(0.0f, shown / 100.0f)), gr = std::min(1.0f, std::max(0.0f, ghost / 100.0f));
        ui_rect(hx + bw * fr, by, bw * (gr - fr), 8 * S, with_alpha(COL_DANGER, 0.75f));
        ui_rect(hx, by, bw * fr, 8 * S, hc);
    } else {
        vt(hx, y + 60 * S, F_BLACK, 44, COL_MUTED, "DEAD", -1, 3);
    }
    ui_rect(x + 190 * S, y + 18 * S, std::max(1.0f, 1 * S), h - 36 * S, with_alpha(COL_TEXT, 0.08f));

    // identity, this round, the match
    const float ix = x + 214 * S, iw = w - 214 * S - 290 * S;
    ui_clip_push(ix, y, iw, h);
    const std::string nm = upper(s.name);
    vt(ix, y + 38 * S, F_DISP, 42, COL_TEXT, nm.c_str(), -1, 2.5f);
    {
        std::string tl = team_label(team == g_team_side[0] ? 0 : 1);
        if (tl != team_word(team)) tl += std::string(" \xC2\xB7 ") + team_word(team);
        float lx = ix + vt(ix, y + 69 * S, F_BOLD, 13, tc, tl.c_str(), -1, 3.5f) + 16 * S;
        if (f.kills > 0) {
            for (int n = 0; n < std::min(f.kills, 5); ++n) { glyph_skull(lx + 8 * S, y + 69 * S, 17 * S, COL_TEXT, COL_INK); lx += 19 * S; }
            int rhs = 0;
            for (const KillEv& e : g_feed.kills) if (e.att == k && e.hs) ++rhs;
            if (rhs) { char b[16]; snprintf(b, sizeof(b), "%d HS", rhs); vt(lx + 6 * S, y + 69 * S, F_BOLD, 12, COL_TEXT, b, -1, 2); }
        } else if (!s.alive) {
            const KillEv* kb = killed_by(k);
            if (kb && kb->att >= 0 && kb->att < 10 && g_slot[kb->att].present) {
                char b[96];
                snprintf(b, sizeof(b), "KILLED BY %s", upper(clean(g_slot[kb->att].name.c_str())).c_str());
                vt(lx, y + 69 * S, F_BOLD, 12, COL_MUTED, b, -1, 2);
            }
        }
    }
    if (f.ktot >= 0) {
        char v[4][16];
        const char* l[4] = { "K", "D", "ADR", "HS" };
        snprintf(v[0], 16, "%d", f.ktot);
        snprintf(v[1], 16, "%d", std::max(0, f.deaths));
        snprintf(v[2], 16, "%d", std::max(0, f.adr));
        if (f.hs >= 0 && f.ktot > 0) snprintf(v[3], 16, "%d%%", (int)(100.0f * f.hs / f.ktot + 0.5f)); else snprintf(v[3], 16, "-");
        float sx = ix;
        for (int i = 0; i < 4; ++i) {
            sx += vt(sx, y + 110 * S, F_DISP, 26, COL_TEXT, v[i]) + 6 * S;
            sx += vt(sx, y + 114 * S, F_BOLD, 11, COL_MUTED, l[i], -1, 3) + 24 * S;
        }
    }
    ui_clip_pop();

    // the gun, the ammo, the rest of the kit
    if (s.alive) {
        const float rx = x + w - 30 * S;
        if (!f.weapon.empty()) {
            const Icon& wi = weapon_icon(weapon_file(f.weapon));
            if (wi.ok) draw_icon(wi, rx - 240 * S, y + 18 * S, 240 * S, 50 * S, 1, 0xFFEEEAE2);
            else vt(rx, y + 40 * S, F_BOLD, 13, COL_MUTED, weapon_label(f.weapon).c_str(), 1, 3);
        }
        if (f.clip >= 0) {
            char c[12], rs[16];
            snprintf(c, sizeof(c), "%d", f.clip);
            snprintf(rs, sizeof(rs), "/ %d", std::max(0, f.reserve));
            const float rw = vt_w(F_DISP, 20, rs);
            vt(rx, y + 92 * S, F_DISP, 20, COL_MUTED, rs, 1);
            vt(rx - rw - 6 * S, y + 88 * S, F_BLACK, 40, f.clip == 0 ? COL_DANGER : COL_TEXT, c, 1);
        }
        // pistol / second gun, then the grenades (right aligned, under the ammo)
        float gx = rx;
        const int nn = std::min(3, f.nades >= 0 ? f.nades : s.nades);
        const Icon& ni = weapon_icon(team_nade(team));
        if (ni.ok) {
            const float nh = 24 * S, nw = nh * ni.aspect;
            for (int n = 0; n < nn; ++n) { draw_icon(ni, gx - nw, y + 116 * S, nw, nh, 0, 0xF0EEEAE2); gx -= nw + 4 * S; }
        }
        if (nn) gx -= 8 * S;
        const std::string* others[2] = { &f.pistol, &f.second };
        for (int o = 0; o < 2; ++o) {
            const std::string& g = *others[o];
            if (g.empty() || weapon_file(g) == weapon_file(f.weapon)) continue;
            const Icon& gi = weapon_icon(weapon_file(g));
            if (!gi.ok) continue;
            const float gh = 18 * S, gw = std::min(90 * S, gh * gi.aspect);
            draw_icon(gi, gx - gw, y + 119 * S, gw, gh, 1, 0xB4EEEAE2);
            gx -= gw + 10 * S;
        }
    }
}

// ------------------------------------------------------------------ round over
// the winner large, the player of the round, every kill of the round (the game's picture dimmed)
void draw_result(float vw, float vh) {
    const RoundResult& r = g_feed.res;
    const float t = std::min(1.0f, (GetTickCount() - g_res_at) / 350.0f);
    const float ease = 1 - (1 - t) * (1 - t);
    const int wteam = r.winner == 'x' ? 2 : r.winner == 'a' ? 1 : 0;
    const DWORD wc = wteam == 2 ? COL_AXIS : wteam == 1 ? COL_ALLIES : COL_MUTED;
    ui_rect(0, 0, vw, vh, with_alpha(COL_DEEP, 0.55f * t));
    ui_shadow(vw / 2 - 360 * S, 230 * S, 720 * S, 180 * S, 90 * S, 220 * S, with_alpha(wc, 0.14f * t));

    // banner
    const float rise = (1 - ease) * 26 * S;
    const float by = 168 * S + rise;
    const char* why = r.reason == 'e' ? "ELIMINATION" : r.reason == 'b' ? "BOMB EXPLODED" : r.reason == 'd' ? "BOMB DEFUSED" : "TIME RAN OUT";
    char head[64];
    if (g_feed.round > 0) snprintf(head, sizeof(head), "ROUND %d \xC2\xB7 %s", g_feed.round, why);
    else snprintf(head, sizeof(head), "%s", why);
    {
        const float hw = vt_w(F_BOLD, 16, head, 8);
        vt(vw / 2, by, F_BOLD, 16, with_alpha(COL_TEXT, t), head, 0, 8);
        ui_rect(vw / 2 - hw / 2 - 96 * S, by - 1 * S, 80 * S, 2 * S, with_alpha(wc, t));
        ui_rect(vw / 2 + hw / 2 + 16 * S, by - 1 * S, 80 * S, 2 * S, with_alpha(wc, t));
    }
    std::string who = "DRAW";
    if (wteam) who = team_label(wteam == g_team_side[0] ? 0 : 1);
    vt(vw / 2, by + 96 * S, F_STEN, 168, with_alpha(wc, t), who.c_str(), 0, 6);
    if (wteam) vt(vw / 2, by + 196 * S, F_DISP, 44, with_alpha(COL_TEXT, t), "TAKES THE ROUND", 0, 14);

    // the player of the round
    const float cy0 = 520 * S + (1 - ease) * 30 * S;
    if (r.mvp >= 0 && r.mvp < 10 && g_slot[r.mvp].present) {
        const int m = r.mvp;
        const int mteam = slot_team(m);
        const DWORD mc = team_col(mteam);
        const float cw = 760 * S, ch = 260 * S, cx = vw / 2 - 630 * S;
        // his gun of the round: the one he killed most with, else the one in hand
        std::string gun;
        {
            std::unordered_map<std::string, int> n;
            int best = 0;
            for (const KillEv& e : g_feed.kills)
                if (e.att == m && e.weapon != "-" && e.weapon != "melee" && ++n[e.weapon] > best) { best = n[e.weapon]; gun = e.weapon; }
            if (gun.empty()) gun = g_feed.p[m].weapon;
        }
        ui_shadow(cx, cy0, cw, ch, 0, 30 * S, with_alpha(0x8C000000, t));
        plate(cx, cy0, cw, ch, 30 * S, 0, 30 * S, 0, with_alpha(COL_PANEL_T, t), with_alpha(COL_PANEL_B, t));
        ui_rect(cx, cy0 + 30 * S, 6 * S, ch - 30 * S, with_alpha(mc, t));
        const Icon* gi = gun.empty() ? nullptr : &weapon_icon(weapon_file(gun));
        if (gi && gi->ok) {
            ui_clip_push(cx, cy0, cw, ch);
            draw_icon(*gi, cx + cw - 420 * S, cy0 + 34 * S, 460 * S, 190 * S, 1, with_alpha(COL_TEXT, 0.10f * t));
            ui_clip_pop();
        }
        const float lx = cx + 44 * S;
        {
            const float mw = vt_w(F_BOLD, 13, "MVP", 4) + 18 * S;
            ui_rect(lx, cy0 + 30 * S, mw, 22 * S, with_alpha(mc, t));
            vt(lx + mw / 2, cy0 + 41 * S, F_BOLD, 13, with_alpha(COL_DEEP, t), "MVP", 0, 4);
            vt(lx + mw + 10 * S, cy0 + 41 * S, F_BOLD, 13, with_alpha(COL_MUTED, t), "PLAYER OF THE ROUND", -1, 4);
        }
        vt(lx, cy0 + 92 * S, F_BLACK, 72, with_alpha(COL_TEXT, t), upper(g_slot[m].name).c_str(), -1, 3);
        {
            std::string tl = team_label(mteam == g_team_side[0] ? 0 : 1);
            if (tl != team_word(mteam)) tl += std::string(" \xC2\xB7 ") + team_word(mteam);
            vt(lx, cy0 + 132 * S, F_BOLD, 14, with_alpha(mc, t), tl.c_str(), -1, 3.5f);
        }
        char v[3][16];
        const char* l[3] = { "KILLS", "HEADSHOTS", "DAMAGE" };
        snprintf(v[0], 16, "%d", r.kills);
        snprintf(v[1], 16, "%d", r.hs);
        snprintf(v[2], 16, "%d", r.dmg);
        float sx = lx;
        for (int i = 0; i < 3; ++i) {
            vt(sx, cy0 + ch - 64 * S, F_BLACK, 52, with_alpha(COL_TEXT, t), v[i]);
            vt(sx, cy0 + ch - 30 * S, F_BOLD, 12, with_alpha(COL_MUTED, t), l[i], -1, 3.5f);
            if (i < 2) ui_rect(sx + 150 * S - 1 * S, cy0 + ch - 90 * S, std::max(1.0f, 1 * S), 64 * S, with_alpha(COL_TEXT, 0.1f * t));
            sx += 174 * S;
        }
        if (gi && gi->ok) {
            draw_icon(*gi, cx + cw - 40 * S - 200 * S, cy0 + ch - 86 * S, 200 * S, 46 * S, 1, with_alpha(COL_TEXT, t));
            vt(cx + cw - 40 * S, cy0 + ch - 30 * S, F_BOLD, 12, with_alpha(COL_MUTED, t), weapon_label(gun).c_str(), 1, 3.5f);
        }
    }

    // the kills of the round, in order
    if (!g_feed.kills.empty()) {
        const float kx = vw / 2 + 160 * S, kw = 470 * S, rh = 34 * S;
        vt(kx, cy0 + 8 * S, F_BOLD, 13, with_alpha(COL_MUTED, t), "KILLS OF THE ROUND", -1, 4);
        float ky = cy0 + 26 * S;
        int n = 0;
        for (const KillEv& e : g_feed.kills) {
            if (ky + rh > vh - 40 * S) break;
            ++n;
            const bool att_ok = e.att >= 0 && e.att < 10 && g_slot[e.att].present;
            const bool vic_ok = e.vic >= 0 && e.vic < 10 && g_slot[e.vic].present;
            ui_rect(kx, ky, kw, rh, with_alpha(COL_INK, 0.86f * t));
            if (att_ok && e.att == r.mvp) ui_aa_stroke_rect(kx, ky, kw, rh, 0, std::max(1.0f, 1 * S), with_alpha(team_col(slot_team(e.att)), 0.7f * t));
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", n);
            vt(kx + 12 * S, ky + rh / 2, F_DISP, 14, with_alpha(COL_MUTED, t), nb);
            ui_clip_push(kx + 32 * S, ky, 112 * S, rh);
            if (att_ok) vt(kx + 32 * S, ky + rh / 2, F_DISP, 17, with_alpha(team_col(slot_team(e.att)), t), upper(g_slot[e.att].name).c_str(), -1, 1);
            ui_clip_pop();
            if (e.weapon != "-" && e.weapon != "melee") {
                const Icon& wi = weapon_icon(weapon_file(e.weapon));
                if (wi.ok) draw_icon(wi, kx + 150 * S, ky + 9 * S, (e.hs ? 150 : 170) * S, 16 * S, 0, with_alpha(COL_TEXT, t));
            } else {
                vt(kx + kw / 2, ky + rh / 2, F_BOLD, 11, with_alpha(COL_MUTED, t), e.weapon == "melee" ? "BASH" : "DIED", 0, 2);
            }
            if (e.hs) glyph_headshot(kx + 312 * S, ky + rh / 2, 6.5f * S, with_alpha(COL_DANGER, t));
            ui_clip_push(kx + kw - 124 * S, ky, 112 * S, rh);
            if (vic_ok) vt(kx + kw - 12 * S, ky + rh / 2, F_DISP, 17, with_alpha(team_col(slot_team(e.vic)), t), upper(g_slot[e.vic].name).c_str(), 1, 1);
            ui_clip_pop();
            ky += rh + 6 * S;
        }
    }
}

// ------------------------------------------------------------------ kill replay
void draw_replay(float vw, float vh) {
    const DWORD now = GetTickCount();
    const float bar = 84 * S;
    ui_rect(0, 0, vw, bar, COL_DEEP);
    ui_rect(0, vh - bar, vw, bar, COL_DEEP);
    viewfinder(40 * S, bar + 20 * S, vw - 80 * S, vh - 2 * bar - 40 * S, 46 * S, 3 * S, COL_TEXT);

    const ReplayInfo& rp = g_feed.rp;
    const int att = rp.att >= 0 && rp.att < 10 && g_slot[rp.att].present ? rp.att : -1;
    // the kills of the replayed player in the replayed window (all of his this round if unknown)
    std::vector<const KillEv*> marks;
    for (const KillEv& e : g_feed.kills) {
        if (att < 0 || e.att != att) continue;
        if (rp.len > 0 && e.t >= 0 && (e.t < rp.from || e.t > rp.from + rp.len)) continue;
        marks.push_back(&e);
    }
    const float el = rp.len > 0 ? std::min((float)rp.len, rp.el + (now - g_rp_at) / 100.0f) : 0;

    // top: the tag, what it is, the score
    {
        const float cy = bar / 2;
        const float tw = vt_w(F_BLACK, 26, "KILL REPLAY", 5) + 50 * S, th = 44 * S, tx = 44 * S;
        plate(tx, cy - th / 2, tw, th, 12 * S, 0, 0, 0, COL_DANGER, COL_DANGER);
        const float pulse = 0.25f + 0.75f * (0.5f + 0.5f * sinf(now * 0.009f));
        ui_aa_disc(tx + 20 * S, cy, 6 * S, with_alpha(COL_DEEP, pulse));
        vt(tx + 34 * S, cy, F_BLACK, 26, COL_DEEP, "KILL REPLAY", -1, 5);
        char info[128];
        if (att >= 0) {
            if (g_feed.round > 0) snprintf(info, sizeof(info), "ROUND %d \xC2\xB7 POV %s \xC2\xB7 %d KILL%s", g_feed.round, upper(clean(g_slot[att].name.c_str())).c_str(), (int)marks.size(), marks.size() == 1 ? "" : "S");
            else snprintf(info, sizeof(info), "POV %s \xC2\xB7 %d KILL%s", upper(clean(g_slot[att].name.c_str())).c_str(), (int)marks.size(), marks.size() == 1 ? "" : "S");
        } else if (g_feed.round > 0) snprintf(info, sizeof(info), "ROUND %d", g_feed.round);
        else info[0] = 0;
        vt(tx + tw + 22 * S, cy, F_BOLD, 15, COL_MUTED, info, -1, 4);
        // score, right
        float rx = vw - 44 * S;
        for (int side = 1; side >= 0; --side) {
            const char* sc = g_score[side].empty() ? "0" : g_score[side].c_str();
            if (side == 1) {
                rx -= vt(rx, cy, F_DISP, 22, team_col(g_team_side[1]), team_label(1).c_str(), 1, 2) + 14 * S;
                rx -= vt(rx, cy, F_STEN, 34, COL_TEXT, sc, 1) + 12 * S;
                ui_rect(rx - 14 * S, cy - 1 * S, 14 * S, 2 * S, COL_MUTED);
                rx -= 26 * S;
            } else {
                rx -= vt(rx, cy, F_STEN, 34, COL_TEXT, sc, 1) + 14 * S;
                vt(rx, cy, F_DISP, 22, team_col(g_team_side[0]), team_label(0).c_str(), 1, 2);
            }
        }
    }

    // the kill on screen: the last one passed (the first one before it comes)
    if (att >= 0 && !marks.empty()) {
        int passed = 0;
        for (const KillEv* e : marks)
            if (rp.len <= 0 || e->t < 0 || e->t - rp.from <= el + 1) ++passed;
        const int idx = std::max(0, std::min((int)marks.size() - 1, passed - 1));
        const KillEv& e = *marks[idx];
        const bool vic_ok = e.vic >= 0 && e.vic < 10 && g_slot[e.vic].present;
        const DWORD ac = team_col(slot_team(att)), vc = vic_ok ? team_col(slot_team(e.vic)) : COL_MUTED;
        const std::string an = upper(g_slot[att].name), vn = vic_ok ? upper(g_slot[e.vic].name) : "?";
        const float anw = vt_w(F_BLACK, 54, an.c_str(), 2), vnw = vt_w(F_BLACK, 54, vn.c_str(), 2);
        const Icon* wi = (e.weapon != "-" && e.weapon != "melee") ? &weapon_icon(weapon_file(e.weapon)) : nullptr;
        const float iw = wi && wi->ok ? std::min(260 * S, 54 * S * wi->aspect) : vt_w(F_BOLD, 14, "BASH", 3);
        const float cw = 80 * S + anw + vnw + iw + 56 * S, ch = 104 * S;
        const float cx = vw / 2 - cw / 2, cyy = vh - bar - 46 * S - ch;
        char k[32];
        snprintf(k, sizeof(k), "KILL %d OF %d", idx + 1, (int)marks.size());
        const float kw = vt_w(F_BOLD, 14, k, 6) + 28 * S;
        ui_rect(vw / 2 - kw / 2, cyy - 38 * S, kw, 26 * S, with_alpha(COL_DEEP, 0.78f));
        vt(vw / 2, cyy - 25 * S, F_BOLD, 14, COL_TEXT, k, 0, 6);
        plate(cx, cyy, cw, ch, 24 * S, 0, 24 * S, 0, 0xF0171C25, 0xF00B0E13);
        float xx = cx + 40 * S;
        vt(xx, cyy + 46 * S, F_BLACK, 54, ac, an.c_str(), -1, 2);
        vt(xx, cyy + 82 * S, F_BOLD, 12, COL_MUTED, team_label(slot_team(att) == g_team_side[0] ? 0 : 1).c_str(), -1, 3.5f);
        xx += anw + 28 * S;
        if (wi && wi->ok) draw_icon(*wi, xx, cyy + 18 * S, iw, 54 * S, 0, COL_TEXT);
        else vt(xx + iw / 2, cyy + 46 * S, F_BOLD, 14, COL_MUTED, e.weapon == "melee" ? "BASH" : "-", 0, 3);
        if (e.hs) {
            glyph_headshot(xx + iw / 2 - 40 * S, cyy + 84 * S, 7 * S, COL_DANGER);
            vt(xx + iw / 2 - 28 * S, cyy + 84 * S, F_BOLD, 12, COL_DANGER, "HEADSHOT", -1, 3);
        }
        xx += iw + 28 * S;
        vt(xx, cyy + 46 * S, F_BLACK, 54, vc, vn.c_str(), -1, 2);
        if (vic_ok) vt(xx, cyy + 82 * S, F_BOLD, 12, COL_MUTED, team_label(slot_team(e.vic) == g_team_side[0] ? 0 : 1).c_str(), -1, 3.5f);
    }

    // bottom: the timeline of the replay, its kills, where it is
    if (rp.len > 0) {
        const float cy = vh - bar / 2;
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d:%02d", (int)(el / 10) / 60, (int)(el / 10) % 60);
        snprintf(b, sizeof(b), "%d:%02d", (rp.len / 10) / 60, (rp.len / 10) % 60);
        vt(44 * S, cy, F_DISP, 20, COL_TEXT, a);
        vt(vw - 44 * S, cy, F_DISP, 20, COL_MUTED, b, 1);
        const float lx = 44 * S + 96 * S, lw = vw - 2 * lx;
        ui_rect(lx, cy - 1 * S, lw, 2 * S, with_alpha(COL_TEXT, 0.16f));
        const float fr = std::min(1.0f, el / rp.len);
        ui_rect(lx, cy - 2 * S, lw * fr, 4 * S, COL_TEXT);
        for (const KillEv* e : marks) {
            if (e->t < 0) continue;
            const float mf = std::min(1.0f, std::max(0.0f, (float)(e->t - rp.from) / rp.len));
            const bool done = e->t - rp.from <= el + 1;
            const DWORD mc = e->hs ? COL_DANGER : COL_TEXT;
            glyph_skull(lx + lw * mf, cy - 13 * S, 18 * S, with_alpha(mc, done ? 1.0f : 0.45f), COL_DEEP);
            const bool vic_ok = e->vic >= 0 && e->vic < 10 && g_slot[e->vic].present;
            if (vic_ok) {
                std::string lab = upper(clean(g_slot[e->vic].name.c_str()));
                if (e->hs) lab += " \xC2\xB7 HS";
                vt(lx + lw * mf, cy + 16 * S, F_BOLD, 11, with_alpha(mc, done ? 1.0f : 0.45f), lab.c_str(), 0, 1);
            }
        }
        ui_rect(lx + lw * fr - 2 * S, cy - 10 * S, 4 * S, 20 * S, COL_DANGER);
    }
}

// ------------------------------------------------------------------ TAB
void draw_overview(float vw, float vh) {
    if (strcmp(g_io->cvar("ui_streamersystem_tab"), "1") != 0) return;
    ui_rect(0, 0, vw, vh, with_alpha(COL_DEEP, 0.8f));
    const float pw = 1460 * S, x = vw / 2 - pw / 2;
    float y = 56 * S;
    // header
    const float hh = 120 * S, cw = 260 * S, half = (pw - cw) / 2;
    plate(x, y, pw, hh, 30 * S, 30 * S, 0, 0, COL_PANEL_T, COL_PANEL_B);
    for (int side = 0; side < 2; ++side) {
        const int team = g_team_side[side];
        const DWORD tc = team_col(team);
        const float bx = side == 0 ? x : x + half + cw;
        ui_rect(bx, y + hh - 4 * S, half, 4 * S, tc);
        const std::string nm = team_label(side);
        const float ts = 72 * S, tx = side == 0 ? bx + 40 * S : bx + half - 40 * S - ts;
        team_tile(tx, y + (hh - ts) / 2 - 2 * S, ts, tc, monogram(nm));
        const float nx = side == 0 ? tx + ts + 20 * S : tx - 20 * S;
        const int al = side == 0 ? -1 : 1;
        vt(nx, y + 50 * S, F_DISP, 40, COL_TEXT, nm.c_str(), al, 3);
        char sub[48];
        snprintf(sub, sizeof(sub), "%s \xC2\xB7 %d ALIVE", team_word(team), alive_count(side));
        vt(nx, y + 84 * S, F_BOLD, 14, tc, sub, al, 4);
        const char* sc = g_score[side].empty() ? "0" : g_score[side].c_str();
        vt(side == 0 ? bx + half - 34 * S : bx + 34 * S, y + hh / 2 - 2 * S, F_STEN, 104, COL_TEXT, sc, side == 0 ? 1 : -1);
    }
    ui_rect(x + half, y, cw, hh, COL_DEEP);
    {
        const float ccx = x + half + cw / 2;
        vt(ccx, y + 26 * S, F_BOLD, 13, COL_MUTED, map_label().c_str(), 0, 5);
        char t[16];
        clock_text(clock_tenths_now(), g_feed.clock_kind == 'b', t, sizeof(t));
        vt(ccx, y + 64 * S, F_BLACK, 46, g_feed.clock_kind == 'b' ? COL_DANGER : COL_TEXT, t, 0, 1);
        char r[48] = "";
        if (g_feed.round > 0 && g_feed.rounds > 1) snprintf(r, sizeof(r), "ROUND %d / %d \xC2\xB7 MR%d", g_feed.round, g_feed.rounds, g_feed.rounds / 2);
        else if (g_feed.round > 0) snprintf(r, sizeof(r), "ROUND %d", g_feed.round);
        vt(ccx, y + 98 * S, F_BODY, 13, COL_MUTED, r, 0, 3.5f);
    }
    y += hh + 14 * S;
    if (!g_feed.hist.empty() || g_feed.rounds > 0) y = draw_history_strip(x, y, pw, 38 * S, true) + 14 * S;

    // the two teams
    struct Col { const char* lab; float w; };
    const Col cols[] = { { "K", 70 }, { "D", 70 }, { "A", 70 }, { "+/\xE2\x88\x92", 80 }, { "ADR", 90 }, { "HS%", 90 }, { "PLANT / DEF", 110 } };
    const float namew = 340 * S, loadw = 260 * S, rowh = 54 * S, pad = 18 * S;
    for (int side = 0; side < 2; ++side) {
        const int team = g_team_side[side];
        const DWORD tc = team_col(team);
        ui_rect(x, y, pw, 34 * S, with_alpha(COL_DEEP, 0.85f));
        ui_rect(x, y + 32 * S, pw, 2 * S, tc);
        std::string title = team_label(side);
        if (title != team_word(team)) title += std::string(" \xC2\xB7 ") + team_word(team);
        vt(x + pad, y + 17 * S, F_DISP, 16, tc, title.c_str(), -1, 3);
        float cxp = x + pad + namew;
        vt(cxp, y + 17 * S, F_BOLD, 12, COL_MUTED, "LOADOUT", -1, 3);
        cxp += loadw;
        for (const Col& c : cols) { vt(cxp + c.w * S / 2, y + 17 * S, F_BOLD, 12, COL_MUTED, c.lab, 0, 3); cxp += c.w * S; }
        vt(x + pw - pad, y + 17 * S, F_BOLD, 12, COL_MUTED, "THIS ROUND", 1, 3);
        y += 37 * S;
        // best ADR of the team, in its colour
        int best_adr = -1;
        for (int i = 0; i < 5; ++i) if (g_slot[side * 5 + i].present) best_adr = std::max(best_adr, g_feed.p[side * 5 + i].adr);
        for (int i = 0; i < 5; ++i) {
            const int k = side * 5 + i;
            const Slot& s = g_slot[k];
            if (!s.present) continue;
            const FeedPlayer& f = g_feed.p[k];
            const bool followed = s.followed || g_feed.cam == k;
            const float prev_alpha = ui_alpha_get();
            if (!s.alive) ui_alpha(prev_alpha * 0.6f);
            ui_rect(x, y, pw, rowh, followed ? with_alpha(COL_TEXT, 0.12f) : with_alpha(COL_INK, 0.86f));
            ui_rect(x, y, 4 * S, rowh, s.alive ? tc : COL_DIMKEY);
            const float cy = y + rowh / 2;
            char key[4];
            snprintf(key, sizeof(key), "%d", (k + 1) % 10);
            ui_rect(x + pad, cy - 13 * S, 26 * S, 26 * S, s.alive ? tc : COL_DIMKEY);
            vt(x + pad + 13 * S, cy, F_BLACK, 16, COL_DEEP, key, 0);
            ui_clip_push(x + pad + 38 * S, y, namew - 38 * S - 56 * S, rowh);
            float nxx = x + pad + 38 * S;
            nxx += vt(nxx, cy, F_DISP, 22, COL_TEXT, upper(s.name).c_str(), -1, 1.5f) + 10 * S;
            if (followed) {
                const float bw = vt_w(F_BOLD, 11, "POV", 2.5f) + 10 * S;
                ui_rect(nxx, cy - 8 * S, bw, 16 * S, COL_TEXT);
                vt(nxx + bw / 2, cy, F_BOLD, 11, COL_DEEP, "POV", 0, 2.5f);
            }
            ui_clip_pop();
            if (s.alive) {
                char hp[8];
                snprintf(hp, sizeof(hp), "%d", s.hp);
                vt(x + pad + namew - 12 * S, cy, F_DISP, 20, s.hp <= 25 ? COL_DANGER : COL_TEXT, hp, 1);
            }
            float lx = x + pad + namew + 24 * S;
            if (s.alive) {
                if (!f.weapon.empty()) {
                    const Icon& wi = weapon_icon(weapon_file(f.weapon));
                    if (wi.ok) lx += draw_icon(wi, lx, cy - 10 * S, 150 * S, 20 * S, -1, 0xF0EEEAE2) + 10 * S;
                }
                const int nn = std::min(3, f.nades >= 0 ? f.nades : s.nades);
                const Icon& ni = weapon_icon(team_nade(team));
                for (int n = 0; n < nn && ni.ok; ++n) lx += draw_icon(ni, lx, cy - 8 * S, 40 * S, 17 * S, -1, 0xE6EEEAE2) + 3 * S;
            } else {
                vt(lx, cy, F_BOLD, 12, COL_MUTED, "DEAD", -1, 3);
            }
            char v[7][16];
            auto num = [](char* o, int val) { if (val < 0) snprintf(o, 16, "-"); else snprintf(o, 16, "%d", val); };
            num(v[0], f.ktot); num(v[1], f.deaths); num(v[2], f.assists);
            if (f.ktot >= 0 && f.deaths >= 0) snprintf(v[3], 16, "%+d", f.ktot - f.deaths); else snprintf(v[3], 16, "-");
            num(v[4], f.adr);
            if (f.hs >= 0 && f.ktot > 0) snprintf(v[5], 16, "%d%%", (int)(100.0f * f.hs / f.ktot + 0.5f)); else snprintf(v[5], 16, f.hs >= 0 ? "0%%" : "-");
            if (f.plants >= 0) snprintf(v[6], 16, "%d / %d", f.plants, std::max(0, f.defuses)); else snprintf(v[6], 16, "-");
            float cxp2 = x + pad + namew + loadw;
            for (int c = 0; c < 7; ++c) {
                DWORD col = c == 0 ? COL_TEXT : COL_SOFT;
                if (c == 3 && f.ktot >= 0 && f.deaths >= 0) col = f.ktot - f.deaths > 0 ? 0xFF7FD08A : f.ktot - f.deaths < 0 ? 0xFFFF8A80 : COL_MUTED;
                if (c == 4 && f.adr >= 0 && f.adr == best_adr) col = tc;
                vt(cxp2 + cols[c].w * S / 2, cy, F_DISP, c == 3 || c == 6 ? 22.0f : 24.0f, col, v[c], 0);
                cxp2 += cols[c].w * S;
            }
            float sk = x + pw - pad;
            for (int n = 0; n < std::min(f.kills, 5); ++n) { glyph_skull(sk - 9 * S, cy, 17 * S, COL_TEXT, COL_INK); sk -= 20 * S; }
            if (!s.alive) ui_alpha(prev_alpha);
            if (followed) ui_aa_stroke_rect(x, y, pw, rowh, 0, 2 * S, COL_TEXT);
            y += rowh + 3 * S;
        }
        y += 11 * S;
    }
    // the series: "name.s1.s2" per map, ";" between maps (the PAM's sv_mapN_* history)
    if (!g_series.empty()) {
        std::vector<std::string> maps;
        std::string cur;
        for (char c : g_series) { if (c == ';') { if (!cur.empty()) maps.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) maps.push_back(cur);
        const int n = (int)maps.size() + 1;
        const float gap = 6 * S, bw = (pw - (n - 1) * gap) / n, bh = 44 * S;
        float bx = x;
        for (int i = 0; i < n; ++i) {
            const bool live = i == n - 1;
            ui_rect(bx, y, bw, bh, live ? with_alpha(COL_TEXT, 0.10f) : with_alpha(COL_DEEP, 0.85f));
            if (live) ui_aa_stroke_rect(bx, y, bw, bh, 0, std::max(1.0f, 1 * S), with_alpha(COL_TEXT, 0.5f));
            char lab[24];
            snprintf(lab, sizeof(lab), live ? "MAP %d \xC2\xB7 LIVE" : "MAP %d", i + 1);
            float xx = bx + 18 * S;
            xx += vt(xx, y + bh / 2, F_BOLD, 12, live ? COL_TEXT : COL_MUTED, lab, -1, 3) + 14 * S;
            std::string name = live ? map_label() : maps[i];
            int s1 = -1, s2 = -1;
            if (!live) {
                const size_t a = name.find('.');
                if (a != std::string::npos) { sscanf(name.c_str() + a + 1, "%d.%d", &s1, &s2); name.resize(a); }
                name = upper(name);
            } else { s1 = atoi(g_score[0].c_str()); s2 = atoi(g_score[1].c_str()); }
            vt(xx, y + bh / 2, F_DISP, 18, COL_TEXT, name.c_str(), -1, 2);
            if (s1 >= 0 && s2 >= 0) {
                char sc[24];
                snprintf(sc, sizeof(sc), "%d \xE2\x80\x93 %d", s1, s2);
                const DWORD c = s1 > s2 ? team_col(g_team_side[0]) : s2 > s1 ? team_col(g_team_side[1]) : COL_TEXT;
                vt(bx + bw - 18 * S, y + bh / 2, F_DISP, 20, c, sc, 1);
            }
            bx += bw + gap;
        }
    }
}

// ------------------------------------------------------------------ winning screen
void draw_winner(float vw, float vh) {
    const float t = std::min(1.0f, (GetTickCount() - g_mapover_at) / 600.0f);
    const float ease = 1 - (1 - t) * (1 - t);
    const int s0 = atoi(g_score[0].c_str()), s1 = atoi(g_score[1].c_str());
    const int wside = s0 > s1 ? 0 : s1 > s0 ? 1 : -1;
    const int wteam = wside >= 0 ? g_team_side[wside] : 0;
    const DWORD wc = wside >= 0 ? team_col(wteam) : COL_TEXT;
    // near-opaque: the PAM's own scoreboard opens at the intermission underneath
    ui_rect(0, 0, vw, vh, with_alpha(COL_DEEP, 0.93f * t));
    // the winner's glow rising from the bottom (a gradient: ui_shadow this large draws rays)
    ui_rect_vgrad(0, vh * 0.5f, vw, vh * 0.5f, with_alpha(wc, 0.0f), with_alpha(wc, 0.24f * t));
    stripes(0, 0, vw, vh, 0.018f * t, COL_TEXT);
    if (!g_event_line.empty()) draw_event_strip(vw / 2, 36 * S);

    // hero: tile, MAP WINNER, the name, the score
    const float rise = (1 - ease) * 30 * S;
    const float hy = 104 * S + rise;
    const std::string wname = wside >= 0 ? team_label(wside) : "DRAW";
    const float ts = 168 * S;
    const float namew = std::max(vt_w(F_STEN, 150, wname.c_str(), 8), vt_w(F_DISP, 34, ("WINS " + map_label()).c_str(), 16));
    char big[8], small_[8];
    snprintf(big, sizeof(big), "%d", std::max(s0, s1));
    snprintf(small_, sizeof(small_), "%d", std::min(s0, s1));
    const float scw = vt_w(F_STEN, 170, big) + 44 * S + vt_w(F_STEN, 120, small_);
    const float rowW = ts + 44 * S + namew + 44 * S + 40 * S + scw;
    float xx = vw / 2 - rowW / 2;
    team_tile(xx, hy, ts, wc, monogram(wname), 28 * S);
    xx += ts + 44 * S;
    {
        const float cw = vt_w(F_BOLD, 14, "MAP WINNER", 5) + 24 * S;
        ui_rect(xx, hy + 8 * S, cw, 24 * S, with_alpha(wc, t));
        vt(xx + cw / 2, hy + 20 * S, F_BOLD, 14, COL_DEEP, "MAP WINNER", 0, 5);
        char sub[64];
        snprintf(sub, sizeof(sub), "%s \xC2\xB7 %d ROUNDS", wside >= 0 ? team_word(wteam) : "", (int)g_feed.hist.size());
        vt(xx + cw + 12 * S, hy + 20 * S, F_BOLD, 14, COL_MUTED, sub, -1, 5);
        vt(xx, hy + 96 * S, F_STEN, 150, with_alpha(wc, t), wname.c_str(), -1, 8);
        vt(xx, hy + 168 * S, F_DISP, 34, with_alpha(COL_TEXT, t), ("WINS " + map_label()).c_str(), -1, 16);
    }
    xx += namew + 44 * S;
    ui_rect(xx, hy + 20 * S, std::max(1.0f, 1 * S), ts - 40 * S, with_alpha(COL_TEXT, 0.14f));
    xx += 40 * S;
    xx += vt(xx, hy + ts / 2, F_STEN, 170, with_alpha(wc, t), big) + 18 * S;
    ui_rect(xx, hy + ts / 2 - 2 * S, 26 * S, 5 * S, COL_MUTED);
    vt(xx + 44 * S, hy + ts / 2 + 8 * S, F_STEN, 120, with_alpha(0xFF6C747F, t), small_);

    // the winners, the player of the match in the middle
    if (wside < 0) return;
    std::vector<int> team;
    for (int i = 0; i < 5; ++i) if (g_slot[wside * 5 + i].present) team.push_back(wside * 5 + i);
    std::sort(team.begin(), team.end(), [](int a, int b) {
        const FeedPlayer& A = g_feed.p[a]; const FeedPlayer& B = g_feed.p[b];
        if (A.ktot != B.ktot) return A.ktot > B.ktot;
        return A.adr > B.adr;
    });
    const int order5[5] = { 3, 1, 0, 2, 4 };                  // ranks laid out left to right
    std::vector<int> laid;
    for (int r : order5) if (r < (int)team.size()) laid.push_back(team[r]);
    const float cwid = 300 * S, gap = 18 * S, base = 860 * S;
    float cx = vw / 2 - (laid.size() * cwid + (laid.size() - 1) * gap) / 2;
    for (size_t i = 0; i < laid.size(); ++i) {
        const int k = laid[i];
        const bool mvp = !team.empty() && k == team[0];
        const FeedPlayer& f = g_feed.p[k];
        const float ch = (mvp ? 440 : 380) * S;
        const float ci = std::min(1.0f, std::max(0.0f, (t * 600.0f - 150 - i * 70) / 450.0f));
        const float cyy = base - ch + (1 - ci) * 30 * S;
        const float prev_alpha = ui_alpha_get();
        ui_alpha(prev_alpha * ci);
        plate(cx, cyy, cwid, ch, 22 * S, 0, 22 * S, 0, mvp ? mix(COL_PANEL_T, wc, 0.16f) : COL_PANEL_T, COL_PANEL_B);
        ui_rect(cx + 22 * S, cyy, cwid - 22 * S, 4 * S, wc);
        // his gun of the map: the one he killed most with (as far as this client saw)
        std::string gun = f.weapon;
        int gk = 0;
        {
            auto it = g_tally.find(g_slot[k].name);
            if (it != g_tally.end())
                for (const auto& kv : it->second) if (kv.second > gk) { gk = kv.second; gun = kv.first; }
        }
        const Icon* gi = gun.empty() ? nullptr : &weapon_icon(weapon_file(gun));
        if (gi && gi->ok) {
            ui_clip_push(cx, cyy, cwid, ch);
            draw_icon(*gi, cx + cwid - 250 * S, cyy + 64 * S, 290 * S, 120 * S, 1, with_alpha(COL_TEXT, mvp ? 0.16f : 0.07f));
            ui_clip_pop();
        }
        float ty = cyy + 22 * S;
        if (mvp) {
            const float mw = vt_w(F_BOLD, 13, "MATCH MVP", 4) + 20 * S;
            ui_rect(cx + 24 * S, ty, mw, 22 * S, COL_TEXT);
            vt(cx + 24 * S + mw / 2, ty + 11 * S, F_BOLD, 13, COL_DEEP, "MATCH MVP", 0, 4);
            ty += 32 * S;
        }
        char key[4];
        snprintf(key, sizeof(key), "%d", (k + 1) % 10);
        ui_rect(cx + 24 * S, ty, 26 * S, 26 * S, wc);
        vt(cx + 37 * S, ty + 13 * S, F_BLACK, 16, COL_DEEP, key, 0);
        ui_clip_push(cx + 60 * S, ty - 6 * S, cwid - 80 * S, 40 * S);
        vt(cx + 60 * S, ty + 14 * S, F_BLACK, 34, COL_TEXT, upper(g_slot[k].name).c_str(), -1, 2);
        ui_clip_pop();
        // K - D, ADR / HS / round MVPs, most kills with
        const float by = cyy + ch - 24 * S;
        if (gi && gi->ok) {
            const float used = draw_icon(*gi, cx + 24 * S, by - 20 * S, 140 * S, 20 * S, -1, COL_TEXT);
            char wl[64];
            if (gk > 0) snprintf(wl, sizeof(wl), "MOST KILLS \xC2\xB7 %d", gk);
            else snprintf(wl, sizeof(wl), "%s", weapon_label(gun).c_str());
            vt(cx + 24 * S + used + 10 * S, by - 10 * S, F_BODY, 11, COL_MUTED, wl, -1, 2.5f);
        }
        const float sy = by - 44 * S;
        ui_rect(cx + 24 * S, sy - 48 * S, cwid - 48 * S, std::max(1.0f, 1 * S), with_alpha(COL_TEXT, 0.12f));
        char v[3][16];
        const char* l[3] = { "ADR", "HS", "ROUND MVP" };
        snprintf(v[0], 16, "%d", std::max(0, f.adr));
        if (f.hs >= 0 && f.ktot > 0) snprintf(v[1], 16, "%d%%", (int)(100.0f * f.hs / f.ktot + 0.5f)); else snprintf(v[1], 16, "-");
        if (f.mvps >= 0) snprintf(v[2], 16, "%d", f.mvps); else snprintf(v[2], 16, "-");
        for (int c = 0; c < 3; ++c) {
            const float colx = cx + 24 * S + c * (cwid - 48 * S) / 3;
            vt(colx, sy - 22 * S, F_DISP, 26, c == 0 && mvp ? wc : COL_TEXT, v[c]);
            vt(colx, sy, F_BOLD, 11, COL_MUTED, l[c], -1, 3);
        }
        char kd[16], dd[16];
        snprintf(kd, sizeof(kd), "%d", std::max(0, f.ktot));
        snprintf(dd, sizeof(dd), "\xE2\x80\x93 %d", std::max(0, f.deaths));
        const float kw = vt(cx + 24 * S, sy - 86 * S, F_BLACK, 64, COL_TEXT, kd);
        vt(cx + 24 * S + kw + 10 * S, sy - 80 * S, F_DISP, 30, COL_MUTED, dd);
        vt(cx + cwid - 24 * S, sy - 80 * S, F_BOLD, 12, COL_MUTED, "K \xE2\x80\x93 D", 1, 3);
        ui_alpha(prev_alpha);
        cx += cwid + gap;
    }

    // the losers, one line
    const int lside = 1 - wside;
    const float ly = vh - 36 * S - 52 * S, lx0 = vw / 2 - 795 * S, lw = 1590 * S;
    const float prev_alpha = ui_alpha_get();
    ui_alpha(prev_alpha * std::min(1.0f, std::max(0.0f, (t * 600.0f - 400) / 300.0f)));
    float bx = lx0;
    {
        const float w0 = 230 * S;
        ui_rect(bx, ly, w0, 52 * S, with_alpha(COL_DEEP, 0.85f));
        ui_rect(bx, ly, 4 * S, 52 * S, team_col(g_team_side[lside]));
        ui_clip_push(bx, ly, w0 - 50 * S, 52 * S);
        vt(bx + 18 * S, ly + 26 * S, F_DISP, 22, COL_TEXT, team_label(lside).c_str(), -1, 2);
        ui_clip_pop();
        vt(bx + w0 - 18 * S, ly + 26 * S, F_STEN, 26, 0xFF6C747F, g_score[lside].empty() ? "0" : g_score[lside].c_str(), 1);
        bx += w0 + 6 * S;
    }
    std::vector<int> lose;
    for (int i = 0; i < 5; ++i) if (g_slot[lside * 5 + i].present) lose.push_back(lside * 5 + i);
    std::sort(lose.begin(), lose.end(), [](int a, int b) { return g_feed.p[a].ktot > g_feed.p[b].ktot; });
    const float cw = lose.empty() ? 0 : (lx0 + lw - bx - (lose.size() - 1) * 6 * S) / lose.size();
    for (int k : lose) {
        const FeedPlayer& f = g_feed.p[k];
        ui_rect(bx, ly, cw, 52 * S, with_alpha(COL_DEEP, 0.72f));
        char kd[24], adr[24];
        snprintf(kd, sizeof(kd), "%d \xE2\x80\x93 %d", std::max(0, f.ktot), std::max(0, f.deaths));
        snprintf(adr, sizeof(adr), "ADR %d", std::max(0, f.adr));
        const float aw = vt_w(F_BOLD, 12, adr, 2), kdw = vt_w(F_DISP, 18, kd);
        vt(bx + cw - 16 * S, ly + 26 * S, F_BOLD, 12, COL_MUTED, adr, 1, 2);
        vt(bx + cw - 16 * S - aw - 10 * S, ly + 26 * S, F_DISP, 18, COL_TEXT, kd, 1);
        ui_clip_push(bx, ly, std::max(0.0f, cw - aw - kdw - 40 * S), 52 * S);
        vt(bx + 16 * S, ly + 26 * S, F_DISP, 18, COL_SOFT, upper(g_slot[k].name).c_str(), -1, 1);
        ui_clip_pop();
        bx += cw + 6 * S;
    }
    ui_alpha(prev_alpha);
}

// ------------------------------------------------------------------ pause
// A timeout (the calling team, big) or the half-time, over the dimmed picture: the countdown to
// the resume (or "waiting for players"), the score and its rounds, who is ready. The PAM's
// ready-up: Q field (_streamer_native::feedPause), the ready marks of the top bar's names.
void draw_pause(float vw, float vh) {
    const PauseInfo& q = g_feed.pause;
    const DWORD now = GetTickCount();
    const float t = std::min(1.0f, (now - g_pause_at) / 450.0f);
    const float ease = 1 - (1 - t) * (1 - t);
    const bool timeout = q.kind == 't';
    const int cteam = q.team == 'a' ? 1 : q.team == 'x' ? 2 : 0;
    const int cside = cteam ? (g_team_side[0] == cteam ? 0 : 1) : -1;
    const DWORD cc = cteam ? team_col(cteam) : COL_TEXT;
    const float cx = vw / 2, rise = (1 - ease) * 24 * S;
    ui_rect(0, 0, vw, vh, with_alpha(COL_DEEP, 0.86f * t));
    ui_rect_vgrad(0, vh * 0.55f, vw, vh * 0.45f, with_alpha(cc, 0.0f), with_alpha(cc, 0.18f * t));
    stripes(0, 0, vw, vh, 0.018f * t, COL_TEXT);
    const float prev_alpha = ui_alpha_get();
    ui_alpha(prev_alpha * t);
    if (!g_event_line.empty()) draw_event_strip(cx, 36 * S);

    // the chip, then the headline: the calling team, or HALF-TIME
    float y = 150 * S + rise;
    {
        const char* chip = timeout ? (cteam ? "TACTICAL TIMEOUT" : "TIMEOUT") : "END OF THE HALF";
        const float cw = vt_w(F_BOLD, 16, chip, 6) + 36 * S, ch = 34 * S;
        plate(cx - cw / 2, y, cw, ch, 10 * S, 0, 10 * S, 0, cc, cc);
        vt(cx, y + ch / 2, F_BOLD, 16, COL_DEEP, chip, 0, 6);
        y += ch + 34 * S;
    }
    if (timeout && cside >= 0) {
        const std::string nm = team_label(cside);
        const float ts = 118 * S, nw = vt_w(F_STEN, 118, nm.c_str(), 6);
        const float row = ts + 36 * S + nw, x0 = cx - row / 2;
        team_tile(x0, y, ts, cc, monogram(nm), 22 * S);
        vt(x0 + ts + 36 * S, y + ts / 2, F_STEN, 118, cc, nm.c_str(), -1, 6);
        y += ts + 26 * S;
        char sub[64];
        const int left = cteam == 1 ? q.to[0] : q.to[1];
        if (left >= 0) snprintf(sub, sizeof(sub), "CALLED A TIMEOUT \xC2\xB7 %d LEFT", left);
        else snprintf(sub, sizeof(sub), "CALLED A TIMEOUT");
        vt(cx, y + 10 * S, F_BOLD, 17, COL_SOFT, sub, 0, 6);
    } else {
        vt(cx, y + 62 * S, F_STEN, 132, COL_TEXT, timeout ? "TIMEOUT" : "HALF-TIME", 0, 8);
        y += 144 * S;
        vt(cx, y + 10 * S, F_BOLD, 17, COL_SOFT, timeout ? "THE MATCH IS PAUSED" : "TEAMS SWITCH SIDES", 0, 6);
    }
    y += 64 * S;

    // the countdown to the resume
    {
        float rem = -1;
        if (q.left >= 0) rem = std::max(0.0f, q.left - (now - g_pause_left_at) / 1000.0f);
        if (rem > 0) {
            char c[16];
            const int r = (int)ceilf(rem);
            snprintf(c, sizeof(c), "%d:%02d", r / 60, r % 60);
            const bool last = rem <= 10.0f;
            const float pulse = last ? 0.55f + 0.45f * (0.5f + 0.5f * sinf(now * 0.008f)) : 1.0f;
            vt(cx, y, F_BOLD, 14, COL_MUTED, "RESUMES IN", 0, 6);
            vt(cx, y + 74 * S, F_BLACK, 112, with_alpha(last ? COL_DANGER : COL_TEXT, pulse), c, 0, 2);
        } else {
            vt(cx, y, F_BOLD, 14, COL_MUTED, "RESUMES WHEN BOTH TEAMS ARE READY", 0, 6);
            vt(cx, y + 60 * S, F_DISP, 46, COL_TEXT, "WAITING FOR PLAYERS", 0, 6);
        }
        y += 150 * S;
    }

    // the score and its rounds
    {
        const float W = 980 * S, h = 84 * S, x0 = cx - W / 2, half = W / 2;
        plate(x0, y, W, h, 20 * S, 20 * S, 0, 0, COL_PANEL_T, COL_PANEL_B);
        for (int side = 0; side < 2; ++side) {
            const int team = g_team_side[side];
            const DWORD tc = team_col(team);
            const float bx = side == 0 ? x0 : x0 + half;
            ui_rect(bx, y + h - 4 * S, half, 4 * S, tc);
            const std::string nm = team_label(side);
            const float ts = 52 * S, tx = side == 0 ? bx + 24 * S : bx + half - 24 * S - ts;
            team_tile(tx, y + (h - 4 * S - ts) / 2, ts, tc, monogram(nm));
            const float nx = side == 0 ? tx + ts + 16 * S : tx - 16 * S;
            vt(nx, y + 32 * S, F_DISP, 28, COL_TEXT, nm.c_str(), side == 0 ? -1 : 1, 2);
            vt(nx, y + 58 * S, F_BOLD, 12, tc, team_word(team), side == 0 ? -1 : 1, 3.5f);
            const char* sc = g_score[side].empty() ? "0" : g_score[side].c_str();
            vt(side == 0 ? bx + half - 46 * S : bx + 46 * S, y + h / 2 - 2 * S, F_STEN, 70, COL_TEXT, sc, 0);
        }
        ui_rect(cx - 1 * S, y + 14 * S, 2 * S, h - 32 * S, with_alpha(COL_TEXT, 0.14f));
        y += h + 8 * S;
        if (!g_feed.hist.empty() || g_feed.rounds > 0) y = draw_history_strip(x0, y, W, 28 * S, true);
        y += 28 * S;
    }

    // who is ready
    {
        char head[48];
        if (q.total > 0) snprintf(head, sizeof(head), "READY  %d / %d", q.ready, q.total);
        else snprintf(head, sizeof(head), "READY");
        vt(cx, y, F_BOLD, 14, COL_MUTED, head, 0, 5);
        y += 20 * S;
        const float colw = 460 * S, rh = 30 * S, gap = 60 * S;
        for (int side = 0; side < 2; ++side) {
            const float x = side == 0 ? cx - gap / 2 - colw : cx + gap / 2;
            float ry = y;
            for (int i = 0; i < 5; ++i) {
                const Slot& s = g_slot[side * 5 + i];
                if (!s.present) continue;
                const bool rd = s.ready == 1;
                ui_rect(x, ry, colw, rh - 4 * S, with_alpha(COL_INK, 0.75f));
                ui_rect(side == 0 ? x : x + colw - 3 * S, ry, 3 * S, rh - 4 * S, team_col(slot_team(side * 5 + i)));
                const std::string nm = upper(s.name);
                const float tagw = 96 * S;
                ui_clip_push(x + 14 * S, ry, colw - tagw - 28 * S, rh);
                vt(side == 0 ? x + 16 * S : x + colw - tagw - 16 * S, ry + (rh - 4 * S) / 2, F_DISP, 17, rd ? COL_TEXT : COL_MUTED,
                   nm.c_str(), side == 0 ? -1 : 1, 1.2f);
                ui_clip_pop();
                const float tx = side == 0 ? x + colw - tagw - 6 * S : x + 6 * S;
                ui_rect(tx, ry + 4 * S, tagw, rh - 12 * S, rd ? with_alpha(COL_READY, 0.9f) : with_alpha(COL_TEXT, 0.08f));
                vt(tx + tagw / 2, ry + (rh - 4 * S) / 2, F_BOLD, 11, rd ? COL_DEEP : COL_MUTED, rd ? "READY" : "NOT READY", 0, 2.5f);
                ry += rh;
            }
        }
    }
    ui_alpha(prev_alpha);
}

// a warm-up ready-up (no timeout, no half-time): a small banner under the scorebug, the live
// picture stays
void draw_readyup_banner(float vw, float y) {
    const PauseInfo& q = g_feed.pause;
    char t[48];
    if (q.total > 0) snprintf(t, sizeof(t), "READY-UP \xC2\xB7 %d / %d READY", q.ready, q.total);
    else snprintf(t, sizeof(t), "READY-UP");
    const float w = vt_w(F_BOLD, 13, t, 4) + 40 * S, h = 30 * S, x = vw / 2 - w / 2;
    y += 8 * S;
    plate(x, y, w, h, 0, 0, 8 * S, 8 * S, 0xE60B0E13, 0xE60B0E13);
    ui_rect(x, y, w, 2 * S, COL_READY);
    vt(vw / 2, y + h / 2 + 1 * S, F_BOLD, 13, COL_TEXT, t, 0, 4);
}

// ------------------------------------------------------------------ hitmarker
// The followed player's hits, the way recent CoDs show them: four diagonal strokes around the
// centre with a dark rim, popping in (1.35x -> 1x in 70 ms, ease-out), held, then faded. A head
// hit is a little bigger; a kill is red, thicker, lasts longer and drifts outward as it fades; a
// headshot kill flashes white at its core first. Every new hit restarts it.
void draw_hitmarker(float vw, float vh) {
    if (!g_hm.at || g_feed.cam < 0) return;
    const float t = (GetTickCount() - g_hm.at) / 1000.0f;
    const float life = g_hm.kill ? 0.60f : 0.34f;
    if (t < 0 || t > life) return;
    const float p = std::min(1.0f, t / 0.07f);
    const float pop = 1.0f + (g_hm.kill ? 0.55f : g_hm.head ? 0.45f : 0.35f) * (1 - p) * (1 - p);
    const float hold = life * (g_hm.kill ? 0.5f : 0.4f);
    const float a = t < hold ? 1.0f : std::max(0.0f, 1 - (t - hold) / (life - hold));
    const float cx = vw / 2, cy = vh / 2;
    float gap = (g_hm.kill ? 9.0f : 7.5f) * S * pop;
    const float len = (g_hm.kill ? 14.0f : g_hm.head ? 12.0f : 10.0f) * S * pop;
    const float th = (g_hm.kill ? 3.4f : 2.4f) * S;
    if (g_hm.kill) gap += t * 16 * S;                       // the kill's strokes drift apart
    const float d = 0.70710678f;
    float seg[4][4];
    for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
        seg[i][0] = cx + sx * gap * d;          seg[i][1] = cy + sy * gap * d;
        seg[i][2] = cx + sx * (gap + len) * d;  seg[i][3] = cy + sy * (gap + len) * d;
    }
    for (int i = 0; i < 4; ++i)                                // dark rim: readable on snow and sky
        ui_aa_line(seg[i][0], seg[i][1], seg[i][2], seg[i][3], th + 2.2f * S, with_alpha(0xFF000000, 0.5f * a));
    DWORD col = g_hm.kill ? 0xFFFF3D3D : 0xFFFFFFFF;
    if (g_hm.hskill && t < 0.09f) col = 0xFFFFFFFF;            // headshot kill: a white flash first
    for (int i = 0; i < 4; ++i)
        ui_aa_line(seg[i][0], seg[i][1], seg[i][2], seg[i][3], th, with_alpha(col, a));
}

// ------------------------------------------------------------------ moments
// strat time over: "ROUND n" (1.8 s)
void draw_intro(float vw, float vh) {
    if (!g_intro_at || g_feed.replay) return;
    const float age = (GetTickCount() - g_intro_at) / 1000.0f;
    if (age > 1.8f) return;
    const float a = std::min(1.0f, age / 0.15f) * (age > 1.4f ? 1 - (age - 1.4f) / 0.4f : 1.0f);
    char t[32];
    if (g_feed.round > 0) snprintf(t, sizeof(t), "ROUND %d", g_feed.round);
    else snprintf(t, sizeof(t), "LIVE");
    const float cy = vh * 0.32f;
    const float ww = vt_w(F_BLACK, 64, t, 8) + 120 * S, hh = 124 * S;
    plate(vw / 2 - ww / 2, cy - hh / 2, ww, hh, 24 * S, 0, 24 * S, 0, with_alpha(0xF0171C25, a), with_alpha(0xF00B0E13, a));
    vt(vw / 2, cy - 12 * S, F_BLACK, 64, with_alpha(COL_TEXT, a), t, 0, 8);
    const float lw = vt_w(F_BOLD, 15, "LIVE", 6);
    ui_aa_disc(vw / 2 - lw / 2 - 8 * S, cy + 36 * S, 5 * S, with_alpha(COL_DANGER, a));
    vt(vw / 2 + 6 * S, cy + 36 * S, F_BOLD, 15, with_alpha(COL_DANGER, a), "LIVE", 0, 6);
}

// in a player's eyes: his team mates near him (1500 u): name and health over the head
void draw_mates(float vw, float vh) {
    const int cam = g_feed.cam;
    if (cam < 0 || cam >= 10 || !g_io->camera) return;
    float fovx, fovy, org[3], ax[9];
    if (!g_io->camera(&fovx, &fovy, org, ax)) return;
    if (!(fovx > 1 && fovx < 179 && fovy > 1 && fovy < 179)) return;
    const float tx = tanf(fovx * 3.14159265f / 360.0f), ty = tanf(fovy * 3.14159265f / 360.0f);
    const int team = slot_team(cam);
    const DWORD tc = team_col(team);
    for (int k = 0; k < 10; ++k) {
        if (k == cam || slot_team(k) != team) continue;
        const FeedPlayer& p = g_feed.p[k];
        const Slot& s = g_slot[k];
        if (!p.present || !p.alive || !s.present || !s.alive) continue;
        float wx, wy, wz;
        predicted(k, &wx, &wy, &wz);
        const float head = p.stance == 'p' ? 20.0f : p.stance == 'c' ? 50.0f : 70.0f;
        const float d[3] = { wx - org[0], wy - org[1], wz + head + 14 - org[2] };
        const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > 1500) continue;
        const float xf = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
        const float yl = d[0] * ax[3] + d[1] * ax[4] + d[2] * ax[5];
        const float zu = d[0] * ax[6] + d[1] * ax[7] + d[2] * ax[8];
        if (xf < 32) continue;
        const float sx = vw / 2 - (yl / xf) / tx * (vw / 2), sy = vh / 2 - (zu / xf) / ty * (vh / 2);
        if (sx < 0 || sx > vw || sy < 0 || sy > vh) continue;
        const float fade = dist > 1200 ? (1500 - dist) / 300 : 1.0f;
        const std::string nm = upper(s.name);
        char hp[8];
        snprintf(hp, sizeof(hp), "%d", s.hp);
        const float nw = vt_w(F_DISP, 15, nm.c_str(), 1.2f), hw = vt_w(F_BOLD, 13, hp);
        const float bw = 3 * S + 8 * S + nw + 8 * S + hw + 9 * S, bh = 24 * S;
        const float bx = sx - bw / 2, by = sy - bh - 10 * S;
        plate(bx, by, bw, bh, 0, 0, 0, 7 * S, with_alpha(0xE00B0E13, fade), with_alpha(0xE00B0E13, fade));
        ui_rect(bx, by, 3 * S, bh, with_alpha(tc, fade));
        vt(bx + 11 * S, by + bh / 2, F_DISP, 15, with_alpha(COL_TEXT, fade), nm.c_str(), -1, 1.2f);
        vt(bx + bw - 9 * S, by + bh / 2, F_BOLD, 13, with_alpha(s.hp <= 25 ? COL_DANGER : tc, fade), hp, 1);
        const float tri[6] = { sx - 6 * S, by + bh, sx + 6 * S, by + bh, sx, by + bh + 7 * S };
        ui_aa_poly(tri, 3, with_alpha(0xE00B0E13, fade));
    }
}

// nobody followed: the streamer flies free
void draw_free_camera(float vw, float vh) {
    if (g_feed.cam >= 0) return;
    const char* t = "FREE CAMERA";
    const float w = vt_w(F_BOLD, 15, t, 5) + 48 * S, h = 36 * S;
    const float x = vw / 2 - w / 2, y = vh - 24 * S - h;
    plate(x, y, w, h, 10 * S, 10 * S, 0, 0, 0xE60B0E13, 0xE60B0E13);
    vt(vw / 2, y + h / 2, F_BOLD, 15, COL_MUTED, t, 0, 5);
}
