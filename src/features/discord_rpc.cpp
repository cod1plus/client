// Discord IPC over \\.\pipe\discord-ipc-N.
// frame = [opcode u32 LE][len u32 LE][json utf-8]; opcode 0=handshake 1=SET_ACTIVITY.
// in-match = HUD frame ticked recently (engine_2d), not an engine-state read.

#include "features/discord_rpc.h"
#include "features/engine_2d.h"
#include "features/settings_menu.h"   // CODMP_CVAR_FINDVAR_VA
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

// ---- what the client knows about the server it is on --------------------------------
// mapname / g_gametype / sv_hostname are serverinfo keys. Whether the engine also
// exposes them as client cvars is not something the strings in the binaries can prove,
// so read them defensively and log what actually came back the first time: an empty
// answer simply degrades to the old fixed text instead of showing a blank presence.

const char* cvar_str(const char* name) {
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) return "";
    if (*(volatile int*)CODMP_CVAR_COUNT_VA <= 0) return "";
    typedef void* (__cdecl *Cvar_FindVar_t)(const char*);
    void* cv = ((Cvar_FindVar_t)CODMP_CVAR_FINDVAR_VA)(name);
    if (!cv) return "";
    const char* s = *(const char**)((char*)cv + 0x04);   // cvar_t.string
    return s ? s : "";
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
    if (strncmp(in, "mp_", 3) == 0) in += 3;
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

// false only on write failure (= discord closed)
// details = what you are playing, state = where. Falls back to the .ini strings when
// the engine gives us nothing, so the presence is never blank.
void compose(bool in_match, char* details, size_t details_size,
                            char* state_txt, size_t state_size) {
    details[0] = '\0';
    state_txt[0] = '\0';

    if (in_match) {
        char mapraw[64], gt[32], host[128];
        snprintf(mapraw, sizeof(mapraw), "%s", cvar_str("mapname"));
        snprintf(gt,     sizeof(gt),     "%s", cvar_str("g_gametype"));
        strip_colors(cvar_str("sv_hostname"), host, sizeof(host));

        static bool probed = false;
        if (!probed) {
            probed = true;
            logger::logf("discord_rpc: server info mapname='%s' g_gametype='%s' sv_hostname='%s'",
                         mapraw, gt, host);
        }

        char mapname[64];
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
    char details[192], state_txt[192];
    compose(in_match, details, sizeof(details), state_txt, sizeof(state_txt));

    if (state == g_sent_state &&
        strcmp(details,   g_sent_details)  == 0 &&
        strcmp(state_txt, g_sent_state_txt) == 0) return true;

    const DWORD now = GetTickCount();
    if (g_last_send_tick != 0 && (now - g_last_send_tick) < MIN_SEND_GAP_MS)
        return true;  // throttle, resend next tick

    char det[160], st_esc[160], limg[96], ltxt[160];
    json_escape(details,   det,    sizeof(det));
    json_escape(state_txt, st_esc, sizeof(st_esc));
    json_escape(g_discord_rpc_config.large_image,  limg,   sizeof(limg));
    json_escape(g_discord_rpc_config.large_text,   ltxt,   sizeof(ltxt));

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
