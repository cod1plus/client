#ifndef COD1RELOADED_NET_DIAG_H
#define COD1RELOADED_NET_DIAG_H

// net_diag - what the game's UDP socket did, for the 'bilan' line and the log.
//
// A 999 in the scoreboard is the server seeing no acknowledgement from a client over
// its last 32 snapshots (~0.8 s at snaps 40). From the client's log alone it was
// impossible to say whether the game had frozen, had stopped sending, or had stopped
// receiving. This hooks WSOCK32 sendto/recvfrom (the only UDP path of the engine) and:
//   - counts packets/bytes per direction for the session report;
//   - measures the longest silence per direction;
//   - when a silence of 0.8 s or more ends while a game was on (both directions were
//     above 10 packets/s the second before), writes ONE line that says what the game was
//     doing meanwhile: frames rendered (frame limiter's counter) and packets of the other
//     direction. Frozen game, dead inbound, or a client that stopped sending: each reads
//     differently.
// The hooks are pass-through: the return value and the socket error are untouched.

namespace patches {

struct NetDiagStats {
    long   rx_packets, tx_packets;       // in the window
    long   rx_bytes,   tx_bytes;
    double rx_max_gap_ms, tx_max_gap_ms; // longest silence in the window (an open one counts if a game was on)
};

void net_diag_start();                               // DllMain: hook the two imports
void net_diag_stats(NetDiagStats* out, bool reset);  // session report
void net_diag_tick();                                // watcher: warns while a silence is still open

}  // namespace patches

#endif
