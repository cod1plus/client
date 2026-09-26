// net_diag.cpp - see net_diag.h
#include "netcode/net_diag.h"
#include "core/iat.h"
#include "core/logger.h"
#include "performance/frame_limiter.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

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
constexpr int    PEER_PPS      = 10;        // this many packets a second TO one address = the game server
constexpr int    MAX_LOG_LINES = 40;        // per session; the bilan keeps counting after that
constexpr unsigned RING = 64;

sendto_t      g_orig_sendto   = nullptr;
recvfrom_t    g_orig_recvfrom = nullptr;
LARGE_INTEGER g_freq = {};
int           g_lines = 0;

// The socket also carries the server browser (one getinfo per server, a reply each)
// and the master queries: only the address the client streams usercmds to counts.
uint64_t g_peer = 0;                        // ip << 16 | port of the game server, 0 = none yet
struct PeerCount { uint64_t key; LONGLONG since; int n; };
PeerCount g_cand[8] = {};

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

// sockaddr_in: family(2) port(2, network order) addr(4)
uint64_t addr_key(const void* sa, int len) {
    if (!sa || len < 8) return 0;
    const unsigned char* b = (const unsigned char*)sa;
    if (b[0] != 2 || b[1] != 0) return 0;                        // AF_INET only
    uint32_t ip;
    memcpy(&ip, b + 4, 4);
    return ((uint64_t)ip << 16) | (uint64_t)((b[2] << 8) | b[3]);
}

void reset_dir(Dir& d) {
    const char* w = d.what;
    d = Dir();
    d.what = w;
}

// a destination we send 10+ packets a second to becomes the peer
void count_send(uint64_t key, LONGLONG t) {
    if (!key || key == g_peer) return;
    PeerCount* slot = nullptr;
    PeerCount* oldest = &g_cand[0];
    for (PeerCount& c : g_cand) {
        if (c.key == key) { slot = &c; break; }
        if (c.since < oldest->since) oldest = &c;
    }
    if (!slot) { slot = oldest; slot->key = key; slot->since = t; slot->n = 0; }
    if (ms(t - slot->since) > 1000.0) { slot->since = t; slot->n = 0; }
    if (++slot->n < PEER_PPS) return;
    g_peer = key;
    reset_dir(g_rx);                                              // a new server: new history
    reset_dir(g_tx);
    const uint32_t ip = (uint32_t)(key >> 16);
    const unsigned char* o = (const unsigned char*)&ip;
    logger::logf("net_diag: game server %u.%u.%u.%u:%u", o[0], o[1], o[2], o[3], (unsigned)(key & 0xffff));
}

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

// the other direction kept a game's pace through the gap (a disconnect leaves a stray
// packet or two, a dead inbound link leaves the client streaming usercmds)
bool other_alive(long other, double gap_ms) {
    return other >= 3 && other >= (long)(gap_ms / 1000.0 * IN_GAME_PPS * 0.5);
}
// under 20 fps across the gap: the game itself stopped (load, alt-tab, hang)
bool was_frozen(long total, long frames, double gap_ms) {
    return total > 0 && frames < (long)(gap_ms / 1000.0 * 20.0);
}

// the silence of d just ended (its next packet is here): one line on what happened.
// Both directions silent while the game kept rendering = a disconnect / reconnect,
// not a network problem: nothing to say.
void explain_gap(Dir& d, const Dir& o, double gap_ms) {
    const long total = frame_limiter_total_frames();
    const long frames = total - d.frames_at_last;
    const long other  = o.total - d.other_at_last;
    const bool frozen = was_frozen(total, frames, gap_ms);
    if (!frozen && !other_alive(other, gap_ms)) return;
    if (g_lines >= MAX_LOG_LINES) return;
    ++g_lines;
    if (total <= 0) {
        logger::logf("net: %.0f ms sans paquet %s (%ld paquets %s pendant ce temps)", gap_ms, d.what, other, o.what);
    } else if (frozen) {
        logger::logf("net: %.0f ms sans paquet %s et le jeu n'a rendu que %ld frame(s) pendant ce trou -> jeu gele "
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
    if (r > 0) {
        const LONGLONG t = now_qpc();
        const uint64_t key = addr_key(to, tolen);
        if (key && key == g_peer) on_packet(g_tx, g_rx, r, t);
        else count_send(key, t);
    }
    return r;
}

int WINAPI hk_recvfrom(UINT_PTR s, char* buf, int len, int flags, void* from, int* fromlen) {
    const int r = g_orig_recvfrom(s, buf, len, flags, from, fromlen);
    if (r > 0 && g_peer && addr_key(from, fromlen ? *fromlen : 0) == g_peer)
        on_packet(g_rx, g_tx, r, now_qpc());
    return r;
}

// watcher: a silence still open after GAP_OPEN_MS gets its line now (the connection may
// never come back) - but only one-sided silences or a frozen game, as above
void check_open(Dir& d, const Dir& o, LONGLONG t) {
    if (!d.last || d.warned) return;
    const double gap_ms = ms(t - d.last);
    if (gap_ms < GAP_OPEN_MS || gap_ms > GAP_MAX_MS || !game_was_on()) return;
    const long total = frame_limiter_total_frames();
    const long frames = total > 0 ? total - d.frames_at_last : 0;
    const long other = o.total - d.other_at_last;
    if (!was_frozen(total, frames, gap_ms) && !other_alive(other, gap_ms)) return;
    d.warned = true;
    if (g_lines >= MAX_LOG_LINES) return;
    ++g_lines;
    logger::logf("net: aucun paquet %s depuis %.0f ms (pendant ce temps: %ld frames rendues, %ld paquets %s)",
                 d.what, gap_ms, frames, other, o.what);
}

double window_max_gap(const Dir& d, const Dir& o, LONGLONG t) {
    LONGLONG g = d.win_max_gap;
    // a silence still open counts only while the other direction is alive
    if (d.last && d.win_packets && game_was_on() && t - d.last > g &&
        other_alive(o.total - d.other_at_last, ms(t - d.last))) g = t - d.last;
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
    logger::logf("net_diag: WSOCK32 recvfrom/sendto hooked (ordinals 17/20): the game server's packets in the bilan line");
}

void net_diag_stats(NetDiagStats* out, bool reset) {
    const LONGLONG t = now_qpc();
    if (out) {
        out->rx_packets = g_rx.win_packets; out->rx_bytes = g_rx.win_bytes;
        out->tx_packets = g_tx.win_packets; out->tx_bytes = g_tx.win_bytes;
        out->rx_max_gap_ms = window_max_gap(g_rx, g_tx, t);
        out->tx_max_gap_ms = window_max_gap(g_tx, g_rx, t);
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
