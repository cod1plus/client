// Discord IPC over \\.\pipe\discord-ipc-N.
// frame = [opcode u32 LE][len u32 LE][json utf-8]; opcode 0=handshake 1=SET_ACTIVITY.
// in-match = HUD frame ticked recently (engine_2d), not an engine-state read.

#include "features/discord_rpc.h"
#include "features/engine_2d.h"
#include "core/logger.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace patches {

DiscordRpcConfig g_discord_rpc_config;

namespace {

HANDLE        g_pipe           = INVALID_HANDLE_VALUE;
HANDLE        g_thread         = nullptr;
volatile LONG g_stop           = 0;

int           g_last_state     = -1;  // -1 unknown, 0 menu, 1 match
int           g_sent_state     = -1;
long long     g_state_epoch    = 0;   // time() at state start
DWORD         g_last_send_tick = 0;
unsigned      g_nonce          = 0;

char g_sent_details[192] = {0};   // resend when the text changes, not just the state
char g_sent_state_txt[192] = {0};
char g_sent_image[96] = {0};

// ---- what the client knows about the server it is on --------------------------------
// The cvars named mapname / sv_hostname / g_gametype are NOT the answer: the binary
// carries the server code too, so every client registers them (defaults "nomap" and
// "CoDHost") and on a pure client they keep those defaults forever. Proven twice -
// live, and statically: those two defaults are pushed right before their cvar names in
// the server's own Cvar_Get block.
//
// The real values live in cgame, which parses them out of the CS_SERVERINFO
// configstring. CG_ParseServerinfo @cgame 0x3002d040 (RE'd 2026-08-03) calls
// Info_ValueForKey(ecx = info, ebx = key) @0x300404b0 and stores:
//     gametype   [32]  RVA 0x1d5a5c   "sd"
//     hostname   [256] RVA 0x1d5a7c   the real server name
//     maxclients int   RVA 0x1d5b7c   the "of N" a party size would need
//     mapname    [64]  RVA 0x1d5b80   as "maps/mp/<name>.bsp" (fmt "maps/mp/%s.bsp")
// Each field ends exactly where the next begins, which is what confirms the layout.
constexpr uintptr_t CGS_GAMETYPE_RVA   = 0x001d5a5c;
constexpr uintptr_t CGS_HOSTNAME_RVA   = 0x001d5a7c;
constexpr uintptr_t CGS_MAXCLIENTS_RVA = 0x001d5b7c;
constexpr uintptr_t CGS_MAPNAME_RVA    = 0x001d5b80;

// cgame is loaded per map and gone in the menus, so its presence doubles as "connected".
HMODULE cgame_module() { return GetModuleHandleA("cgame_mp_x86.dll"); }

void cgs_string(HMODULE cg, uintptr_t rva, size_t field_size, char* out, size_t out_size) {
    out[0] = '\0';
    if (!cg) return;
    const char* p = (const char*)((uintptr_t)cg + rva);
    size_t n = 0;
    while (n < field_size && n + 1 < out_size && p[n]) { out[n] = p[n]; ++n; }
    out[n] = '\0';
}

int cgs_maxclients(HMODULE cg) {
    if (!cg) return 0;
    const int n = *(const int*)((uintptr_t)cg + CGS_MAXCLIENTS_RVA);
    return (n > 0 && n <= 64) ? n : 0;
}

// "maps/mp/mp_carentan.bsp" -> "mp_carentan"
void bsp_to_mapname(char* s) {
    const char* slash = strrchr(s, '/');
    if (slash) memmove(s, slash + 1, strlen(slash + 1) + 1);
    char* dot = strrchr(s, '.');
    if (dot && _stricmp(dot, ".bsp") == 0) *dot = '\0';
}

// CoD1 colour codes are '^' + one digit; they would show up literally on a profile.
void strip_colors(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_size; ++i) {
        if (in[i] == '^' && in[i + 1] >= '0' && in[i + 1] <= '9') { ++i; continue; }
        out[o++] = in[i];
    }
    while (o > 0 && out[o - 1] == ' ') --o;    // trailing padding is common in host names
    out[o] = '\0';
}

// "mp_carentan" -> "Carentan"; leaves anything unexpected alone, so custom maps like
// mp_coastal read correctly too.
void pretty_map(const char* in, char* out, size_t out_size) {
    if (!in[0]) { out[0] = '\0'; return; }
    // case-insensitive: the prefix comes from whatever the server put in serverinfo,
    // and "MP_Harbor" is as valid a spelling as "mp_harbor"
    if (_strnicmp(in, "mp_", 3) == 0) in += 3;
    snprintf(out, out_size, "%s", in);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

constexpr DWORD IN_MATCH_TIMEOUT_MS = 1500;
constexpr DWORD MIN_SEND_GAP_MS     = 4000;  // discord limits ~5 updates/20s

void pipe_close() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

bool pipe_write_frame(uint32_t opcode, const char* payload) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    const uint32_t len = (uint32_t)strlen(payload);
    char buf[1408];
    if ((size_t)len + 8 > sizeof(buf)) return false;
    memcpy(buf, &opcode, 4);
    memcpy(buf + 4, &len, 4);
    memcpy(buf + 8, payload, len);
    DWORD written = 0;
    if (!WriteFile(g_pipe, buf, len + 8, &written, nullptr)) return false;
    return written == len + 8;
}

// drain + discard replies (READY/PONG/errors), non-blocking
void pipe_drain() {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    char scratch[2048];
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD got = 0;
        DWORD want = avail < sizeof(scratch) ? avail : (DWORD)sizeof(scratch);
        if (!ReadFile(g_pipe, scratch, want, &got, nullptr) || got == 0) break;
    }
}

bool pipe_connect() {
    if (g_discord_rpc_config.client_id[0] == '\0') return false;
    for (int i = 0; i < 10; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_pipe = h;
        char hs[160];
        snprintf(hs, sizeof(hs), "{\"v\":1,\"client_id\":\"%s\"}",
                 g_discord_rpc_config.client_id);
        if (!pipe_write_frame(0, hs)) { pipe_close(); continue; }
        logger::logf("discord_rpc: connecte (%s)", name);
        return true;
    }
    return false;
}

// escape " and \, drop control chars
void json_escape(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_size; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c >= 0x20)        { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

// Is this map declared as having an art asset? Matches whole words only, so listing
// "mp_carentan" never accidentally covers "mp_carentan2".
bool map_has_asset(const char* map) {
    if (!map[0]) return false;
    const char* list = g_discord_rpc_config.map_images;
    if (list[0] == '*' && list[1] == '\0') return true;
    const size_t len = strlen(map);
    for (const char* p = list; *p; ) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        const char* start = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') ++p;
        if ((size_t)(p - start) == len && _strnicmp(start, map, len) == 0) return true;
    }
    return false;
}

// details = what you are playing, state = where. Falls back to the .ini strings when
// cgame is not loaded (i.e. in the menus), so the presence is never blank.
void compose(bool in_match, char* details, size_t details_size,
                            char* state_txt, size_t state_size,
                            char* image, size_t image_size,
                            char* image_text, size_t image_text_size) {
    details[0] = '\0';
    state_txt[0] = '\0';
    snprintf(image,      image_size,      "%s", g_discord_rpc_config.large_image);
    snprintf(image_text, image_text_size, "%s", g_discord_rpc_config.large_text);

    HMODULE cg = cgame_module();
    char mapraw[80] = {0}, gt[32] = {0}, hostraw[256] = {0}, host[160] = {0};
    if (cg) {
        cgs_string(cg, CGS_MAPNAME_RVA,  64,  mapraw,  sizeof(mapraw));
        cgs_string(cg, CGS_GAMETYPE_RVA, 32,  gt,      sizeof(gt));
        cgs_string(cg, CGS_HOSTNAME_RVA, 256, hostraw, sizeof(hostraw));
        bsp_to_mapname(mapraw);
        strip_colors(hostraw, host, sizeof(host));
    }

    // Log what cgame really held, once and again on every map change.
    {
        static char last_probe_map[80] = {0};
        static bool probed = false;
        if (!probed || strcmp(mapraw, last_probe_map) != 0) {
            probed = true;
            snprintf(last_probe_map, sizeof(last_probe_map), "%s", mapraw);
            logger::logf("discord_rpc: cgs map='%s' gametype='%s' host='%s' maxclients=%d",
                         mapraw, gt, host, cgs_maxclients(cg));
        }
    }

    if (in_match) {
        char mapname[80];
        pretty_map(mapraw, mapname, sizeof(mapname));

        // Raw gametype code, uppercased: players say "sd" and "tdm", and a table of
        // pretty names would just go stale on the first custom gametype.
        char gtup[32];
        size_t i = 0;
        for (; gt[i] && i + 1 < sizeof(gtup); ++i)
            gtup[i] = (gt[i] >= 'a' && gt[i] <= 'z') ? (char)(gt[i] - 'a' + 'A') : gt[i];
        gtup[i] = '\0';

        if (mapname[0] && gtup[0])      snprintf(details, details_size, "%s on %s", gtup, mapname);
        else if (mapname[0])            snprintf(details, details_size, "%s", mapname);
        if (host[0])                    snprintf(state_txt, state_size, "%s", host);

        // Thumbnail = the map, when an asset for it was actually uploaded. Asset keys
        // are lowercase on Discord's side.
        if (map_has_asset(mapraw)) {
            size_t k = 0;
            for (; mapraw[k] && k + 1 < image_size; ++k)
                image[k] = (mapraw[k] >= 'A' && mapraw[k] <= 'Z')
                           ? (char)(mapraw[k] - 'A' + 'a') : mapraw[k];
            image[k] = '\0';
            if (mapname[0]) snprintf(image_text, image_text_size, "%s", mapname);
        }
    }

    if (!details[0]) {
        snprintf(details, details_size, "%s",
                 in_match ? g_discord_rpc_config.details_match
                          : g_discord_rpc_config.details_menu);
    }
    if (!state_txt[0] && g_discord_rpc_config.state_text[0])
        snprintf(state_txt, state_size, "%s", g_discord_rpc_config.state_text);
}

bool update_presence(bool in_match) {
    const int state = in_match ? 1 : 0;
    if (state != g_last_state) {
        g_last_state  = state;
        g_state_epoch = (long long)time(nullptr);
    }

    // The map and the server can change without the menu/match state changing, so the
    // resend test is on the TEXT, not just on that state.
    char details[192], state_txt[192], image[96], image_text[160];
    compose(in_match, details, sizeof(details), state_txt, sizeof(state_txt),
            image, sizeof(image), image_text, sizeof(image_text));

    // The thumbnail changes with the map, so it belongs in the resend test too.
    if (state == g_sent_state &&
        strcmp(details,   g_sent_details)   == 0 &&
        strcmp(state_txt, g_sent_state_txt) == 0 &&
        strcmp(image,     g_sent_image)     == 0) return true;

    const DWORD now = GetTickCount();
    if (g_last_send_tick != 0 && (now - g_last_send_tick) < MIN_SEND_GAP_MS)
        return true;  // throttle, resend next tick

    char det[160], st_esc[160], limg[96], ltxt[160];
    json_escape(details,    det,    sizeof(det));
    json_escape(state_txt,  st_esc, sizeof(st_esc));
    json_escape(image,      limg,   sizeof(limg));
    json_escape(image_text, ltxt,   sizeof(ltxt));

    char ts[64] = "";
    if (g_discord_rpc_config.show_elapsed)
        snprintf(ts, sizeof(ts), ",\"timestamps\":{\"start\":%lld}", g_state_epoch);

    char stf[200] = "";
    if (st_esc[0])
        snprintf(stf, sizeof(stf), ",\"state\":\"%s\"", st_esc);

    // Only send an assets block when an image name is configured. large_image must be
    // the key of an asset uploaded under Rich Presence > Art Assets - the application
    // ICON is a different thing and is NOT reachable by name. Sending a key that does
    // not exist gets you no image at all, whereas sending no assets block lets Discord
    // fall back to the application icon, which is what most setups actually have.
    char assets[300] = "";
    if (limg[0]) {
        snprintf(assets, sizeof(assets),
                 ",\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"}", limg, ltxt);
    }

    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
        "\"details\":\"%s\"%s%s%s"
        "}},\"nonce\":\"%u\"}",
        (unsigned long)GetCurrentProcessId(),
        det, stf, ts, assets, ++g_nonce);

    if (!pipe_write_frame(1, payload)) return false;
    g_sent_state     = state;
    g_last_send_tick = now;
    snprintf(g_sent_details,  sizeof(g_sent_details),  "%s", details);
    snprintf(g_sent_state_txt, sizeof(g_sent_state_txt), "%s", state_txt);
    snprintf(g_sent_image, sizeof(g_sent_image), "%s", image);
    return true;
}

bool in_match_now() {
    const DWORD t = engine_2d_last_hud_tick();
    return t != 0 && (GetTickCount() - t) < IN_MATCH_TIMEOUT_MS;
}

DWORD WINAPI thread_main(LPVOID) {
    DWORD last_connect_attempt = 0;
    g_sent_state = -1;

    while (!g_stop) {
        if (g_pipe == INVALID_HANDLE_VALUE) {
            const DWORD now = GetTickCount();
            if (last_connect_attempt == 0 || (now - last_connect_attempt) > 10000) {
                last_connect_attempt = now;
                if (pipe_connect()) g_sent_state = -1;  // force resend
            }
        } else {
            pipe_drain();
            if (!update_presence(in_match_now())) {
                logger::logf("discord_rpc: ecriture echouee -> deconnecte");
                pipe_close();
                g_sent_state = -1;
            }
        }
        for (int i = 0; i < 20 && !g_stop; ++i) Sleep(100);  // ~2s, stop-responsive
    }

    pipe_close();
    return 0;
}

}  // namespace

void discord_rpc_start() {
    if (!g_discord_rpc_config.enable) {
        logger::logf("discord_rpc: desactive (discord_rpc_enable=false)");
        return;
    }
    if (g_discord_rpc_config.client_id[0] == '\0') {
        logger::logf("discord_rpc: aucun client_id -> desactive "
                     "(voir discord_rpc_client_id dans cod1reloaded.ini)");
        return;
    }
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
    logger::logf("discord_rpc: thread demarre (client_id=%s, image=%s)",
                 g_discord_rpc_config.client_id,
                 g_discord_rpc_config.large_image);
}

void discord_rpc_shutdown() {
    InterlockedExchange(&g_stop, 1);
    // no join (loader lock); closing pipe clears presence
    pipe_close();
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
}

}  // namespace patches
