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
bool update_presence(bool in_match) {
    const int state = in_match ? 1 : 0;
    if (state != g_last_state) {
        g_last_state  = state;
        g_state_epoch = (long long)time(nullptr);
    }

    if (state == g_sent_state) return true;

    const DWORD now = GetTickCount();
    if (g_last_send_tick != 0 && (now - g_last_send_tick) < MIN_SEND_GAP_MS)
        return true;  // throttle, resend next tick

    char det[160], st_esc[160], limg[96], ltxt[160];
    json_escape(in_match ? g_discord_rpc_config.details_match
                         : g_discord_rpc_config.details_menu,
                det, sizeof(det));
    json_escape(g_discord_rpc_config.state_text,   st_esc, sizeof(st_esc));
    json_escape(g_discord_rpc_config.large_image,  limg,   sizeof(limg));
    json_escape(g_discord_rpc_config.large_text,   ltxt,   sizeof(ltxt));

    char ts[64] = "";
    if (g_discord_rpc_config.show_elapsed)
        snprintf(ts, sizeof(ts), ",\"timestamps\":{\"start\":%lld}", g_state_epoch);

    char stf[200] = "";
    if (st_esc[0])
        snprintf(stf, sizeof(stf), ",\"state\":\"%s\"", st_esc);

    char payload[1024];
    snprintf(payload, sizeof(payload),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
        "\"details\":\"%s\"%s%s,"
        "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"}"
        "}},\"nonce\":\"%u\"}",
        (unsigned long)GetCurrentProcessId(),
        det, stf, ts, limg, ltxt, ++g_nonce);

    if (!pipe_write_frame(1, payload)) return false;
    g_sent_state     = state;
    g_last_send_tick = now;
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
