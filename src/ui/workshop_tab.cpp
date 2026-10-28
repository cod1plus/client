// workshop_tab.cpp - the Workshop tab of the 1.6X menu. See workshop_tab.h.
//
// Same flat monochrome language as the rest of the menu (modern_menu.cpp): black ground,
// hairlines, white pill = the action. The two colours the Files tab already uses for its
// downloads keep their meaning here: amber = waiting on you (update, restart), red = failed.
// The small widgets below are copies of modern_menu.cpp's (they live in its anonymous
// namespace); keep the two looking the same.

#include "ui/workshop_tab.h"
#include "ui/gl_overlay.h"
#include "features/workshop.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace patches {

namespace {

constexpr DWORD UI_LINE  = 0xFF232323;    // hairline           (as modern_menu.cpp)
constexpr DWORD UI_HINT  = 0xFF5E5E5E;    // explanatory text
constexpr DWORD UI_EYEB  = 0xFF6B7280;    // section eyebrows
constexpr DWORD WS_AMBER = 0xFFE0B252;    // the Files tab's "restart to finish"
constexpr DWORD WS_BODY  = 0xFFB9B9B9;    // long text (About tab's feature list)
const char* const DOT = "  \xC2\xB7  ";   // " · "

DWORD mixc(DWORD a, DWORD b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    DWORD r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        r |= (DWORD)(ca + (int)((cb - ca) * t)) << sh;
    }
    return r;
}
long wkey(float x, float y, int tag) { return ((long)x << 14) ^ (long)y ^ ((long)(tag & 15) << 27); }
long ikey(int i, int tag) { return 0x7A000000L + i * 8 + tag; }   // per tile: stable while scrolling

bool hit(float x, float y, float w, float h) {
    const UiInput& in = ui_input();
    return in.mx >= x && in.mx < x + w && in.my >= y && in.my < y + h;
}

// style 1 = primary (white pill), 0 = secondary, -1 = inert (a state, not an action)
bool button(float x, float y, float w, float h, const char* label, int style) {
    const bool over = style >= 0 && hit(x, y, w, h);
    const float hv = ui_smooth(wkey(x, y, 9), over ? 1.0f : 0.0f, 14.0f);
    DWORD bg = style == 1 ? mixc(UI_ACCENT, 0xFFFFFFFF, hv) : style == 0 ? mixc(UI_ROW, UI_ROW_HOT, hv) : UI_PANEL;
    ui_rect_rounded(x, y, w, h, 8, bg);
    if (style < 0) ui_rect_border(x, y, w, h, 8, 1.0f, UI_LINE);
    const float tw = ui_text_width(16, 600, label);
    ui_text(x + (w - tw) / 2, y + (h - 22) / 2, 16, 600,
            style == 1 ? 0xFF10151C : style == 0 ? UI_TEXT : UI_MUTED, label);
    return over && ui_input().clicked;
}

int segmented(float x, float y, float w, const char* const* opts, int n, int sel) {
    const float h = 40, seg = w / n, pad = 5;
    ui_rect_rounded(x, y, w, h, 8, UI_ROW);
    const float selpos = ui_smooth(wkey(x, y, 10), (float)sel, 13.0f);
    ui_rect_rounded(x + seg * selpos + pad, y + pad, seg - pad * 2, h - pad * 2, 6, UI_ACCENT);
    for (int i = 0; i < n; ++i) {
        const float sx = x + seg * i;
        if (i > 0) ui_rect(sx, y + 9, 1, h - 18, 0x22FFFFFF);
        float prox = 1.0f - (selpos > i ? selpos - i : i - selpos);
        if (prox < 0) prox = 0;
        const float tw = ui_text_width(15, 600, opts[i]);
        ui_text(sx + (seg - tw) / 2, y + 9, 15, 600, mixc(UI_TEXT, 0xFF10151C, prox), opts[i]);
        if (hit(sx, y, seg, h) && ui_input().clicked) sel = i;
    }
    return sel;
}

void section(float x, float y, float w, const char* s) {
    ui_text(x, y, 13, 600, UI_EYEB, s);
    const float tw = ui_text_width(13, 600, s);
    if (w > tw + 26) ui_rect(x + tw + 14, y + 9, w - tw - 14, 1, UI_LINE);
}

float chip(float x, float y, const char* s, DWORD fg, DWORD bg, DWORD border) {
    const float w = ui_text_width(11, 600, s) + 16;
    ui_rect_rounded(x, y, w, 22, 5, bg);
    if (border) ui_rect_border(x, y, w, 22, 5, 1.0f, border);
    ui_text(x + 8, y + 3, 11, 600, fg, s);
    return w;
}

std::string fmt_size(unsigned long long b) {
    char s[32];
    if (b >= 1024ull * 1024 * 1024) snprintf(s, sizeof(s), "%.2f GB", b / 1073741824.0);
    else if (b >= 1024ull * 1024)   snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
    else                            snprintf(s, sizeof(s), "%.0f KB", b / 1024.0);
    return s;
}

// text cut to a width with an ellipsis (one measure + one cut: no texture per prefix)
std::string fit(const std::string& s, int px, int weight, float maxw) {
    const float w = ui_text_width(px, weight, s.c_str());
    if (w <= maxw || s.empty()) return s;
    size_t keep = (size_t)(s.size() * (maxw / w)) ;
    keep = keep > 2 ? keep - 2 : 0;
    while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80) --keep;
    return s.substr(0, keep) + "\xE2\x80\xA6";
}

// word wrap, cached per text and width. Words are measured one by one: ui_text caches a
// texture per distinct string, so measuring growing prefixes would fill that cache.
const std::vector<std::string>& wrap(const std::string& text, int px, int weight, float maxw) {
    static std::map<std::string, std::vector<std::string>> cache;
    char k[32];
    snprintf(k, sizeof(k), "%d|%d|%d|", px, weight, (int)maxw);
    const std::string key = k + text;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 64) cache.clear();
    std::vector<std::string> lines;
    const float space = ui_text_width(px, weight, "x x") - ui_text_width(px, weight, "xx");
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string para = text.substr(pos, eol - pos);
        std::string line;
        float lw = 0;
        size_t i = 0;
        while (i < para.size()) {
            while (i < para.size() && para[i] == ' ') ++i;
            size_t j = i;
            while (j < para.size() && para[j] != ' ') ++j;
            if (j == i) break;
            const std::string word = para.substr(i, j - i);
            const float ww = ui_text_width(px, weight, word.c_str());
            if (!line.empty() && lw + space + ww > maxw) { lines.push_back(line); line.clear(); lw = 0; }
            if (!line.empty()) { line += ' '; lw += space; }
            line += word;
            lw += ww;
            i = j;
        }
        lines.push_back(line);
        pos = eol + 1;
    }
    return cache.emplace(key, lines).first->second;
}

void thumb(const WsItem& it, float x, float y, float w, float h) {
    if (!it.thumb_path.empty() && ui_image_cover(x, y, w, h, it.thumb_path.c_str())) return;
    ui_rect(x, y, w, h, UI_ROW);
    const char* t = it.thumb_url.empty() ? "NO PREVIEW" : "LOADING PREVIEW";
    const float tw = ui_text_width(11, 600, t);
    ui_text(x + (w - tw) / 2, y + h / 2 - 8, 11, 600, UI_HINT, t);
}

// viewfinder corners around the big picture, like the Display tab's preview
void brackets(float x, float y, float w, float h) {
    const float L = 18, T = 2, o = 8;
    const float xs[2] = { x - o, x + w + o - L }, ys[2] = { y - o, y + h + o - T };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            ui_rect(xs[i], ys[j], L, T, 0xFF9A9A9A);
            ui_rect(i ? x + w + o - T : x - o, j ? y + h + o - L : y - o, T, L, 0xFF9A9A9A);
        }
}

// what an item shows: its badge on the picture and its action button
struct Look {
    const char* badge;
    DWORD badge_fg, badge_bg;
    const char* action;
    int style;              // button(): 1 action, -1 a state
};

Look look_of(const WsItem& it) {
    switch (it.job) {
    case WSJ_QUEUED:      return { "QUEUED", UI_TEXT, 0xFF1E1E1E, "QUEUED", -1 };
    case WSJ_DOWNLOADING: return { "DOWNLOADING", UI_TEXT, 0xFF1E1E1E, "DOWNLOADING...", -1 };
    case WSJ_VERIFYING:   return { "VERIFYING", UI_TEXT, 0xFF1E1E1E, "VERIFYING...", -1 };
    case WSJ_ERROR:       return { "FAILED", 0xFFFFFFFF, UI_DANGER, "RETRY", 1 };
    default: break;
    }
    switch (it.local) {
    case WS_ABSENT:    return { nullptr, 0, 0, "DOWNLOAD", 1 };
    case WS_INSTALLED: return { "INSTALLED", 0xFF0A0A0A, UI_ACCENT, "INSTALLED", -1 };
    case WS_OUTDATED:  return { "UPDATE", 0xFF0A0A0A, WS_AMBER, "UPDATE", 1 };
    case WS_STAGED:    return { "RESTART", 0xFF0A0A0A, WS_AMBER, "RESTART TO FINISH", -1 };
    default:           return { nullptr, 0, 0, "CHECKING...", -1 };
    }
}

std::string status_line(const WsItem& it, DWORD& col) {
    col = UI_MUTED;
    if (it.job == WSJ_ERROR) { col = UI_DANGER; return it.job_text; }
    if (it.job == WSJ_QUEUED || it.job == WSJ_DOWNLOADING || it.job == WSJ_VERIFYING) { col = UI_TEXT; return it.job_text; }
    switch (it.local) {
    case WS_ABSENT:    return "Not installed  -  " + fmt_size(it.size) + " download" +
                              (it.files.size() > 1 ? " (" + std::to_string(it.files.size()) + " files)" : "");
    case WS_INSTALLED: col = UI_TEXT;
                       return it.installed_now ? "Installed and verified - restart the game to play it"
                                               : "Installed, identical to the catalog (SHA-256)";
    case WS_OUTDATED:  col = WS_AMBER;
                       return it.local_version.empty() ? "Another version is installed - v" + it.version + " available"
                                                       : "v" + it.local_version + " installed  -  v" + it.version + " available";
    case WS_STAGED:    col = WS_AMBER; return "Downloaded and verified - restart the game to finish";
    default:           return "Checking the file on disk...";
    }
}

std::string host_of(const std::string& url) {
    if (!_strnicmp(url.c_str(), "file://", 7)) return "local file";
    const size_t s = url.find("://");
    if (s == std::string::npos) return url;
    const size_t e = url.find('/', s + 3);
    return url.substr(s + 3, e == std::string::npos ? std::string::npos : e - s - 3);
}

std::string s_sel;          // selected item id
int   s_filter = 0;         // 0 all, 1 maps, 2 mods
float s_scroll = 0;

void draw_detail(const WsItem& it, float dx, float dy, float dw, float bottom) {
    // the picture gives way to the text on short screens (cover crop: it just gets wider)
    float th = dw * 9.0f / 16.0f;
    if (th > (bottom - dy) * 0.38f) th = (bottom - dy) * 0.38f;
    if (th < 150) th = 150;
    thumb(it, dx, dy, dw, th);
    brackets(dx, dy, dw, th);
    dy += th + 26;

    ui_text(dx, dy, 28, 600, UI_TEXT, fit(it.name, 28, 600, dw).c_str());
    dy += 42;
    if (!it.author.empty()) {
        ui_text(dx, dy, 15, 400, UI_MUTED, fit("by " + it.author, 15, 400, dw).c_str());
        dy += 30;
    }
    float cx = dx;
    cx += chip(cx, dy, it.type == "mod" ? "MOD" : "MAP", UI_TEXT, UI_ROW, UI_LINE) + 8;
    if (!it.version.empty()) cx += chip(cx, dy, ("v" + it.version).c_str(), UI_TEXT, UI_ROW, UI_LINE) + 8;
    cx += chip(cx, dy, fmt_size(it.size).c_str(), UI_TEXT, UI_ROW, UI_LINE) + 8;
    const Look lk = look_of(it);
    if (lk.badge) chip(cx, dy, lk.badge, lk.badge_fg, lk.badge_bg, 0);
    dy += 40;

    if (button(dx, dy, 240, 46, lk.action, lk.style) && lk.style == 1) workshop_install(it.id);
    DWORD col;
    const std::string st = status_line(it, col);
    if (dw - 258 >= 320) {                 // beside the button when the column is wide enough
        ui_text(dx + 258, dy + 13, 14, 500, col, fit(st, 14, 500, dw - 258).c_str());
        dy += 58;
    } else {
        ui_text(dx, dy + 56, 14, 500, col, fit(st, 14, 500, dw).c_str());
        dy += 84;
    }
    const bool moving = it.job == WSJ_DOWNLOADING || it.job == WSJ_VERIFYING || it.job == WSJ_QUEUED;
    if (moving) {
        float f = it.progress < 0 ? 0 : (it.progress > 1 ? 1 : it.progress);
        const float shown = ui_smooth(ikey(0x3ff, 1), f, 10.0f);
        ui_rect_rounded(dx, dy - 4, dw, 6, 3, UI_ROW);
        ui_rect_rounded(dx, dy - 4, dw * shown, 6, 3, UI_ACCENT);
    }
    dy += 18;

    // the page below the action: description, then the file facts (kept above `bottom`)
    const int nfacts = it.cvars.empty() ? 3 : 4;
    const float facts_h = nfacts * 24 + 34 + 46;
    if (!it.description.empty() && dy + 60 < bottom - facts_h) {
        section(dx, dy, dw, "ABOUT");
        dy += 28;
        const std::vector<std::string>& lines = wrap(it.description, 14, 400, dw);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (dy + 21 > bottom - facts_h) {
                ui_text(dx, dy, 14, 400, UI_HINT, "\xE2\x80\xA6");
                dy += 21;
                break;
            }
            ui_text(dx, dy, 14, 400, WS_BODY, lines[i].c_str());
            dy += 21;
        }
        dy += 16;
    }
    if (dy + facts_h - 46 > bottom) return;
    section(dx, dy, dw, "FILE");
    dy += 28;
    if (it.files.empty()) return;
    const size_t nf = it.files.size();
    const std::string where = it.folder + "\\" + it.files[0].file +
                              (nf > 1 ? "  + " + std::to_string(nf - 1) + " more" : "");
    const std::string sha = nf > 1 ? std::to_string(nf) + " files, each checked"
                                   : it.files[0].sha256.substr(0, 16) + "\xE2\x80\xA6";
    const std::string src = host_of(it.files[0].url);
    std::string sets;
    for (size_t i = 0; i < it.cvars.size(); ++i) sets += (i ? ", " : "") + it.cvars[i].first + " " + it.cvars[i].second;
    const char* labels[4] = { "Installs to", "SHA-256", "Source", "Sets" };
    const std::string* vals[4] = { &where, &sha, &src, &sets };
    for (int i = 0; i < nfacts; ++i) {
        ui_text(dx, dy, 13, 400, UI_HINT, labels[i]);
        ui_text(dx + 110, dy, 13, 600, UI_TEXT, fit(*vals[i], 13, 600, dw - 110).c_str());
        dy += 24;
    }
    dy += 10;
    if (dy + 40 > bottom) return;
    if (it.installed_now) {
        ui_text(dx, dy, 13, 400, UI_HINT, "CoD1 lists its pk3s when it starts: restart the game (or join a");
        ui_text(dx, dy + 20, 13, 400, UI_HINT, "server running it) and the new content is there.");
    } else {
        ui_text(dx, dy, 13, 400, UI_HINT, "Checked against the catalog's SHA-256 before it is placed. A pk3");
        ui_text(dx, dy + 20, 13, 400, UI_HINT, "the game holds open is swapped in at the next launch.");
    }
}

}  // namespace

void workshop_tab_draw(float x, float y, float w, float bottom) {
    static bool s_loaded = false;
    if (!s_loaded) { s_loaded = true; workshop_refresh(); }
    WsCatalogInfo info;
    std::vector<WsItem> items;
    workshop_snapshot(info, items);

    // ---- toolbar: filter, where the catalog came from, refresh
    static const char* filters[] = { "All", "Maps", "Mods" };
    s_filter = segmented(x, y, 300, filters, 3, s_filter);
    {
        const float tx = x + 324, tw = w - 324 - 150;
        const DWORD tc = info.state == WSC_ERROR ? UI_DANGER : info.state == WSC_CACHED ? WS_AMBER : UI_MUTED;
        if (!info.title.empty()) {
            ui_text(tx, y - 1, 15, 600, UI_TEXT, fit(info.title, 15, 600, tw).c_str());
            ui_text(tx, y + 20, 13, 400, tc, fit(info.text, 13, 400, tw).c_str());
        } else {
            ui_text(tx, y + 10, 13, 400, tc, fit(info.text, 13, 400, tw).c_str());
        }
    }
    const bool can_refresh = info.state != WSC_NONE && info.state != WSC_DISABLED && !info.busy;
    if (button(x + w - 130, y, 130, 40, info.busy ? "Loading..." : "Refresh", can_refresh ? 0 : -1) && can_refresh)
        workshop_refresh();
    y += 66;

    std::vector<int> vis;
    for (size_t i = 0; i < items.size(); ++i)
        if (s_filter == 0 || (s_filter == 1) == (items[i].type == "map")) vis.push_back((int)i);

    if (items.empty()) {
        // nothing to list yet: say why, and what to do about it
        const char* head = info.state == WSC_LOADING ? "Loading the catalog..."
                         : info.state == WSC_ERROR   ? "The catalog could not be loaded"
                         : info.state == WSC_READY   ? "The catalog is empty"
                                                     : "No catalog yet";
        ui_text(x, y + 10, 22, 600, UI_TEXT, head);
        ui_text(x, y + 48, 14, 400, info.state == WSC_ERROR ? UI_DANGER : UI_MUTED, info.text.c_str());
        ui_text(x, y + 84, 13, 400, UI_HINT, "The Workshop lists maps and mods from a catalog file (JSON): set workshop_manifest_url");
        ui_text(x, y + 104, 13, 400, UI_HINT, "in cod1reloaded.ini to its address - http, https or a local file:/// path.");
        return;
    }

    // ---- two panes: tiles on the left, the selected item's page on the right
    float dw = w * 0.42f;
    if (dw > 640) dw = 640;
    if (dw < 420) dw = 420;
    const float gw = w - dw - 48, gh = bottom - y;
    const float gap = 24;
    int cols = (int)((gw + gap) / (300 + gap));
    if (cols < 1) cols = 1;
    float tw = (gw - (cols - 1) * gap) / cols;
    if (tw > 360) tw = 360;
    const float th = tw * 9.0f / 16.0f, tile_h = th + 96;
    const int rows = ((int)vis.size() + cols - 1) / cols;
    const float content_h = rows * (tile_h + gap) - gap;
    const float maxs = content_h > gh ? content_h - gh : 0;
    if (hit(x, y, gw, gh)) s_scroll -= ui_input().wheel * 60;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > maxs) s_scroll = maxs;

    const WsItem* sel = nullptr;
    for (size_t k = 0; k < vis.size(); ++k) if (items[vis[k]].id == s_sel) sel = &items[vis[k]];
    if (!sel && !vis.empty()) { sel = &items[vis[0]]; s_sel = sel->id; }

    ui_clip_push(x - 2, y - 2, gw + 4, gh + 2);
    for (size_t k = 0; k < vis.size(); ++k) {
        const WsItem& it = items[vis[k]];
        const float tx = x + (k % cols) * (tw + gap);
        const float ty = y + (k / cols) * (tile_h + gap) - s_scroll;
        if (ty > bottom || ty + tile_h < y) continue;
        const bool over = hit(tx, ty, tw, tile_h) && hit(x, y, gw, gh);
        const bool is_sel = sel == &it;
        const float hv = ui_smooth(ikey(vis[k], 0), over ? 1.0f : 0.0f, 14.0f);
        ui_rect_rounded(tx, ty, tw, tile_h, 10, mixc(UI_PANEL, UI_ROW, hv));
        thumb(it, tx + 1, ty + 1, tw - 2, th);
        if (is_sel) ui_rect_border(tx, ty, tw, tile_h, 10, 1.5f, UI_ACCENT);
        else ui_rect_border(tx, ty, tw, tile_h, 10, 1.0f, mixc(UI_LINE, 0xFF3A3A3A, hv));
        const Look lk = look_of(it);
        if (lk.badge) {
            const float bw = ui_text_width(11, 600, lk.badge) + 16;
            chip(tx + tw - bw - 10, ty + 10, lk.badge, lk.badge_fg, lk.badge_bg, 0);
        }
        if (it.job == WSJ_DOWNLOADING || it.job == WSJ_VERIFYING) {
            ui_rect(tx + 1, ty + th - 3, tw - 2, 4, 0xC0000000);
            ui_rect(tx + 1, ty + th - 3, (tw - 2) * (it.progress < 0 ? 0 : it.progress > 1 ? 1 : it.progress), 4, UI_ACCENT);
        }
        ui_text(tx + 16, ty + th + 14, 17, 600, UI_TEXT, fit(it.name, 17, 600, tw - 32).c_str());
        if (!it.author.empty())
            ui_text(tx + 16, ty + th + 40, 13, 400, UI_MUTED, fit("by " + it.author, 13, 400, tw - 32).c_str());
        std::string meta = std::string(it.type == "mod" ? "MOD" : "MAP") + DOT + fmt_size(it.size);
        if (!it.version.empty()) meta += std::string(DOT) + "v" + it.version;
        ui_text(tx + 16, ty + th + 64, 12, 600, UI_EYEB, meta.c_str());
        if (over && ui_input().clicked) s_sel = it.id;
    }
    ui_clip_pop();
    if (vis.empty()) ui_text(x, y + 6, 14, 400, UI_MUTED, s_filter == 1 ? "(no map in this catalog)" : "(no mod in this catalog)");
    if (maxs > 0) {
        const float thumbh = gh * gh / content_h;
        const float thumby = y + (gh - thumbh) * (s_scroll / maxs);
        ui_rect_rounded(x + gw + 10, y, 3, gh, 1.5f, UI_ROW);
        ui_rect_rounded(x + gw + 10, thumby, 3, thumbh, 1.5f, 0xFF3A3A3A);
    }

    if (sel) draw_detail(*sel, x + gw + 48, y, dw, bottom);
}

}  // namespace patches
