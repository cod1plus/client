// Discord IPC over \\.\pipe\discord-ipc-N.
// frame = [opcode u32 LE][len u32 LE][json utf-8]; opcode 0=handshake 1=SET_ACTIVITY.
// in-match = HUD frame ticked recently (engine_2d), not an engine-state read.

#include "features/discord_rpc.h"
#include "features/engine_2d.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA, CODMP_CBUF_EXECTEXT_VA
#include "netcode/protocol_patch.h"   // CODMP_CVAR_COUNT_VA
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
bool g_party_rejected = false;   // discord refused the join block: stop sending it

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

const char* cvar_str(const char* name) {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return "";
    if (*(volatile int*)CODMP_CVAR_COUNT_VA <= 0) return "";
    typedef void* (__cdecl *Cvar_FindVar_t)(const char*);
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(name);
    if (!cv) return "";
    const char* s = *(const char**)((char*)cv + 0x04);   // cvar_t.string
    return s ? s : "";
}

// ---- join secret --------------------------------------------------------------------
// The secret is an opaque string Discord only carries: it leaves one player's client
// and is handed to whoever clicks Join. We put the server address in it, and the
// receiving mod runs `connect <secret>`.
//
// Which means the secret is ATTACKER-CONTROLLED TEXT arriving from another Discord
// user, and it ends up in the console command buffer. A secret containing ';' or a
// newline would run arbitrary console commands on the machine that clicks Join. So
// nothing goes to Cbuf unless it looks exactly like an address and nothing else.
bool is_safe_address(const char* s) {
    if (!s || !*s) return false;
    size_t n = 0, colons = 0;
    for (; s[n]; ++n) {
        if (n >= 64) return false;                     // no address is this long
        const char c = s[n];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';
        if (!ok) return false;                         // kills ; \n " and every separator
        if (c == ':') ++colons;
    }
    return n >= 7 && colons <= 1;                      // "1.2.3.4" at the very least
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

// Pull one flat JSON string value out of a frame. Enough for the two fields we care
// about; the payloads Discord sends here have no nesting in the way and no escapes.
bool json_str(const char* json, const char* key, char* out, size_t out_size) {
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

// Read whatever Discord sent and act on ACTIVITY_JOIN. Everything else (READY, PONG,
// command acks, errors) is still discarded - we only ever needed one event.
// Frames are [opcode u32][len u32][json], and a frame can straddle two reads, so the
// leftover is kept rather than parsed from a single buffer.
char  g_rx[8192];
size_t g_rx_len = 0;

void on_join(const char* secret) {
    if (!is_safe_address(secret)) {
        logger::logf("discord_rpc: join secret rejected (not an address): refusing to "
                     "run it as a console command");
        return;
    }
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "connect %s\n", secret);
    typedef void (__cdecl *Cbuf_ExecuteText_t)(int, const char*);
    ((Cbuf_ExecuteText_t)CODMP_CBUF_EXECTEXT_VA)(2 /* EXEC_APPEND */, cmd);
    logger::logf("discord_rpc: ACTIVITY_JOIN -> connect %s", secret);
}

void pipe_drain() {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        if (g_rx_len >= sizeof(g_rx) - 1) { g_rx_len = 0; }   // desync: start clean
        DWORD want = (DWORD)(sizeof(g_rx) - 1 - g_rx_len);
        if (avail < want) want = avail;
        DWORD got = 0;
        if (!ReadFile(g_pipe, g_rx + g_rx_len, want, &got, nullptr) || got == 0) break;
        g_rx_len += got;

        // consume every complete frame currently buffered
        for (;;) {
            if (g_rx_len < 8) break;
            uint32_t len;
            memcpy(&len, g_rx + 4, 4);
            if (len > sizeof(g_rx) - 8) { g_rx_len = 0; break; }   // absurd: resync
            if (g_rx_len < 8 + len) break;                          // wait for the rest
            char body[4096];
            const size_t n = len < sizeof(body) - 1 ? len : sizeof(body) - 1;
            memcpy(body, g_rx + 8, n);
            body[n] = '\0';
            if (strstr(body, "ACTIVITY_JOIN")) {
                char secret[96];
                if (json_str(body, "secret", secret, sizeof(secret))) on_join(secret);
            }
            // Discord answers every command. A rejected SET_ACTIVITY leaves the
            // PREVIOUS presence in place, which looks exactly like the mod never
            // updating - so the refusal has to be visible, with its reason.
            if (strstr(body, "\"evt\":\"ERROR\"") || strstr(body, "\"code\":")) {
                char msg[192];
                if (!json_str(body, "message", msg, sizeof(msg)))
                    snprintf(msg, sizeof(msg), "%.180s", body);
                if (!g_party_rejected) {
                    // Fall back to a presence without the join block rather than let a
                    // rejected activity sit there. A refusal keeps the PREVIOUS
                    // presence alive, so the map and server would stay stuck on
                    // whatever was showing before - which reads as the mod being
                    // broken, when only the newest field is at fault.
                    g_party_rejected = true;
                    g_sent_state = -1;          // force a resend without party/secrets
                    logger::logf("discord_rpc: REFUS de discord -> %s "
                                 "| join desactive pour la session, presence renvoyee "
                                 "sans party/secrets", msg);
                } else {
                    logger::logf("discord_rpc: REFUS de discord -> %s", msg);
                }
            }
            memmove(g_rx, g_rx + 8 + len, g_rx_len - (8 + len));
            g_rx_len -= 8 + len;
        }
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
        // Without this subscription Discord never tells us that someone clicked Join.
        pipe_write_frame(1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\","
                            "\"args\":{},\"nonce\":\"sub-join\"}");
        g_rx_len = 0;
        logger::logf("discord_rpc: connecte (%s), abonne a ACTIVITY_JOIN", name);
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

    // Join button. Discord only offers one when party.id AND secrets.join are both
    // present. The server address serves as both: as a party id it puts everyone on
    // the same server in the same party, and as a secret it is exactly what the other
    // client needs to connect. Sent only while in a match and only when the address
    // passes the same validation the receiving side applies - no point advertising a
    // secret our own join handler would refuse.
    char party[224] = "";
    if (in_match && !g_party_rejected) {
        const char* addr = cvar_str("cl_currentServerAddress");
        static char last_logged[80] = {0};
        if (strcmp(addr, last_logged) != 0) {
            snprintf(last_logged, sizeof(last_logged), "%s", addr);
            logger::logf("discord_rpc: cl_currentServerAddress='%s' -> join %s",
                         addr, is_safe_address(addr) ? "offert" : "indisponible");
        }
        if (is_safe_address(addr)) {
            char a[96];
            json_escape(addr, a, sizeof(a));
            // Discord rejects the whole activity with "secrets cannot match the party
            // id" when the two are equal - and a rejected activity leaves the previous
            // presence frozen, so this took the map and the server down with it. The
            // party id is prefixed: it still groups everyone on one server, while the
            // secret stays the bare address the receiving side connects to and
            // validates.
            snprintf(party, sizeof(party),
                     ",\"party\":{\"id\":\"srv-%s\"},\"secrets\":{\"join\":\"%s\"}", a, a);
        }
    }

    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
        "\"details\":\"%s\"%s%s%s%s"
        "}},\"nonce\":\"%u\"}",
        (unsigned long)GetCurrentProcessId(),
        det, stf, ts, assets, party, ++g_nonce);

    if (!pipe_write_frame(1, payload)) return false;
    g_sent_state     = state;
    g_last_send_tick = now;
    snprintf(g_sent_details,  sizeof(g_sent_details),  "%s", details);
    snprintf(g_sent_state_txt, sizeof(g_sent_state_txt), "%s", state_txt);
    snprintf(g_sent_image, sizeof(g_sent_image), "%s", image);
    return true;
}

// "Am I on a server?" used to mean "the HUD drew a frame in the last 1.5 s", which
// reported the menus while connected: the HUD does not tick during a map load, and
// anything that stops drawing it - the ESC menu, the scoreboard, a stall - looked
// exactly like a disconnect.
//
// cgame is loaded per map and unloaded on disconnect, and it only holds a map name
// once the server info has been parsed, so the pair answers the question directly
// instead of inferring it from a side effect. The HUD tick stays as a fallback for
// the case where cgame is loaded but its layout differs from what we expect.
bool in_match_now() {
    HMODULE cg = cgame_module();
    char m[80] = {0};
    if (cg) cgs_string(cg, CGS_MAPNAME_RVA, 64, m, sizeof(m));
    const DWORD t = engine_2d_last_hud_tick();
    const bool hud = (t != 0) && (GetTickCount() - t) < IN_MATCH_TIMEOUT_MS;
    const bool res = (cg && m[0]) || hud;

    // Say out loud what the decision was made on: this test has now been wrong twice,
    // and its inputs are the only thing that can settle which one is lying.
    static int last = -1;
    if ((int)res != last) {
        last = (int)res;
        logger::logf("discord_rpc: in_match=%d (cgame=%p mapname='%s' hud_tick=%lu ago=%lu)",
                     (int)res, (void*)cg, m, (unsigned long)t,
                     (unsigned long)(t ? GetTickCount() - t : 0));
    }
    return res;
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
