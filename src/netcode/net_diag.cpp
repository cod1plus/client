// net_diag.cpp - see net_diag.h
#include "netcode/net_diag.h"
#include "core/iat.h"
#include "core/logger.h"
#include "performance/frame_limiter.h"

#include <windows.h>
#include <cstdio>

namespace patches {

namespace {

// winsock 1.1 prototypes, without pulling <winsock2.h> next to <windows.h>
typedef int (WINAPI* sendto_t)(UINT_PTR s, const char* buf, int len, int flags, const void* to, int tolen);
typedef int (WINAPI* recvfrom_t)(UINT_PTR s, char* buf, int len, int flags, void* from, int* fromlen);

constexpr WORD WSOCK32_ORD_RECVFROM = 17;
constexpr WORD WSOCK32_ORD_SENDTO   = 20;
constexpr double GAP_MIN_MS    = 800.0;     // what a 999 needs at snaps 40
constexpr double GAP_OPEN_MS   = 1000.0;    // the watcher warns about a silence this long
constexpr double GAP_MAX_MS    = 120000.0;  // longer = menu / reconnect, not a hiccup
constexpr int    IN_GAME_PPS   = 10;        // both directions above this the second before = a game was on
constexpr int    MAX_LOG_LINES = 40;        // per session; the bilan keeps counting after that
constexpr unsigned RING = 64;

sendto_t      g_orig_sendto   = nullptr;
recvfrom_t    g_orig_recvfrom = nullptr;
LARGE_INTEGER g_freq = {};
int           g_lines = 0;

struct Dir {
    const char* what;               // "du serveur" / "au serveur"
    LONGLONG last = 0;              // QPC of the last packet
    long     total = 0;             // packets since the hook
    long     frames_at_last = 0;    // frame_limiter_total_frames() at the last packet
    long     other_at_last = 0;     // the other direction's total at the last packet
    LONGLONG ring[RING] = {};       // timestamps of the last packets (in-game test)
    unsigned ring_pos = 0;
    long     win_packets = 0;       // session-report window
    long     win_bytes = 0;
    LONGLONG win_max_gap = 0;
    bool     warned = false;        // the watcher already wrote about the open silence
};

Dir g_rx = { "du serveur" };
Dir g_tx = { "au serveur" };

double ms(LONGLONG ticks) { return g_freq.QuadPart ? (double)ticks * 1000.0 / (double)g_freq.QuadPart : 0.0; }
LONGLONG now_qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// packets of d in the second before its last one
int pps_before_last(const Dir& d) {
    if (!d.last || !g_freq.QuadPart) return 0;
    const LONGLONG from = d.last - g_freq.QuadPart;
    int n = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (d.ring[i] && d.ring[i] >= from && d.ring[i] <= d.last) ++n;
    return n;
}

bool game_was_on() {
    return pps_before_last(g_rx) >= IN_GAME_PPS && pps_before_last(g_tx) >= IN_GAME_PPS;
}

// the silence of d just ended (its next packet is here): one line on what happened
void explain_gap(Dir& d, const Dir& o, double gap_ms) {
    if (g_lines >= MAX_LOG_LINES) return;
    ++g_lines;
    const long total = frame_limiter_total_frames();
    const long frames = total - d.frames_at_last;
    const long other  = o.total - d.other_at_last;
    if (total <= 0) {
        logger::logf("net: %.0f ms sans paquet %s (%ld paquets %s pendant ce temps)", gap_ms, d.what, other, o.what);
    } else if (frames <= 2) {
        logger::logf("net: %.0f ms sans paquet %s et le jeu n'a rendu que %ld frame pendant ce trou -> jeu gele "
                     "(chargement, alt-tab, blocage)", gap_ms, d.what, frames);
    } else if (&d == &g_rx) {
        logger::logf("net: %.0f ms sans paquet du serveur alors que le jeu tournait (%ld frames) et envoyait "
                     "(%ld paquets) -> reseau entrant ou serveur", gap_ms, frames, other);
    } else {
        logger::logf("net: %.0f ms sans envoi au serveur alors que le jeu tournait (%ld frames) et recevait "
                     "(%ld paquets) -> anomalie client", gap_ms, frames, other);
    }
}

void on_packet(Dir& d, const Dir& o, int n, LONGLONG t) {
    if (d.last) {
        const LONGLONG gap = t - d.last;
        if (gap > d.win_max_gap) d.win_max_gap = gap;
        const double gap_ms = ms(gap);
        if (gap_ms >= GAP_MIN_MS && gap_ms <= GAP_MAX_MS && game_was_on()) explain_gap(d, o, gap_ms);
    }
    d.last = t;
    ++d.total;
    d.frames_at_last = frame_limiter_total_frames();
    d.other_at_last = o.total;
    d.ring[d.ring_pos++ % RING] = t;
    ++d.win_packets;
    d.win_bytes += n;
    d.warned = false;
}

int WINAPI hk_sendto(UINT_PTR s, const char* buf, int len, int flags, const void* to, int tolen) {
    const int r = g_orig_sendto(s, buf, len, flags, to, tolen);
    if (r > 0) on_packet(g_tx, g_rx, r, now_qpc());
    return r;
}

int WINAPI hk_recvfrom(UINT_PTR s, char* buf, int len, int flags, void* from, int* fromlen) {
    const int r = g_orig_recvfrom(s, buf, len, flags, from, fromlen);
    if (r > 0) on_packet(g_rx, g_tx, r, now_qpc());
    return r;
}

// watcher: a silence still open after GAP_OPEN_MS gets its line now (the connection may never come back)
void check_open(Dir& d, const Dir& o, LONGLONG t) {
    if (!d.last || d.warned) return;
    const double gap_ms = ms(t - d.last);
    if (gap_ms < GAP_OPEN_MS || gap_ms > GAP_MAX_MS || !game_was_on()) return;
    d.warned = true;
    if (g_lines >= MAX_LOG_LINES) return;
    ++g_lines;
    const long total = frame_limiter_total_frames();
    logger::logf("net: aucun paquet %s depuis %.0f ms (pendant ce temps: %ld frames rendues, %ld paquets %s)",
                 d.what, gap_ms, total > 0 ? total - d.frames_at_last : 0, o.total - d.other_at_last, o.what);
}

double window_max_gap(const Dir& d, LONGLONG t) {
    LONGLONG g = d.win_max_gap;
    if (d.last && d.win_packets && game_was_on() && t - d.last > g) g = t - d.last;   // still open
    return ms(g);
}

}  // namespace

void net_diag_start() {
    QueryPerformanceFrequency(&g_freq);
    g_orig_recvfrom = (recvfrom_t)iat_hook_ordinal("WSOCK32.dll", WSOCK32_ORD_RECVFROM, (void*)hk_recvfrom);
    g_orig_sendto   = (sendto_t)iat_hook_ordinal("WSOCK32.dll", WSOCK32_ORD_SENDTO, (void*)hk_sendto);
    if (!g_orig_recvfrom || !g_orig_sendto) {
        logger::logf("net_diag: WSOCK32 recvfrom/sendto not in the import table (recv=%p send=%p) - no network figures",
                     (void*)g_orig_recvfrom, (void*)g_orig_sendto);
        return;
    }
    logger::logf("net_diag: WSOCK32 recvfrom/sendto hooked (ordinals 17/20): packets and silences in the bilan line");
}

void net_diag_stats(NetDiagStats* out, bool reset) {
    const LONGLONG t = now_qpc();
    if (out) {
        out->rx_packets = g_rx.win_packets; out->rx_bytes = g_rx.win_bytes;
        out->tx_packets = g_tx.win_packets; out->tx_bytes = g_tx.win_bytes;
        out->rx_max_gap_ms = window_max_gap(g_rx, t);
        out->tx_max_gap_ms = window_max_gap(g_tx, t);
    }
    if (reset) {
        g_rx.win_packets = g_rx.win_bytes = 0; g_rx.win_max_gap = 0;
        g_tx.win_packets = g_tx.win_bytes = 0; g_tx.win_max_gap = 0;
    }
}

void net_diag_tick() {
    if (!g_orig_recvfrom || !g_orig_sendto) return;
    static DWORD s_last = 0;
    const DWORD now = GetTickCount();
    if (s_last && now - s_last < 1000) return;
    s_last = now;
    const LONGLONG t = now_qpc();
    check_open(g_rx, g_tx, t);
    check_open(g_tx, g_rx, t);
}

}  // namespace patches
