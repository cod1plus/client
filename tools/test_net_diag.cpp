// test_net_diag.cpp - replays traffic patterns through net_diag's hooks, in real time
// (the module times packets with QPC): the server browser must stay silent, a one-way
// silence during a game must be reported with its cause, a disconnect must not be.
//   g++ -std=c++17 -O2 -I src tools/test_net_diag.cpp -o build/test_net_diag && build/test_net_diag
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

// ---- the module's dependencies, faked
namespace logger {
std::vector<std::string> lines;
void logf(const char* fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    lines.push_back(b);
    printf("    log: %s\n", b);
}
void init(HMODULE) {}
}
namespace patches {
long g_frames = 0;
long frame_limiter_total_frames() { return g_frames; }
void* iat_hook_ordinal(const char*, WORD, void*) { return nullptr; }
}

#include "../src/netcode/net_diag.cpp"

using namespace patches;

static int WINAPI fake_sendto(UINT_PTR, const char*, int len, int, const void*, int) { return len; }
static int WINAPI fake_recvfrom(UINT_PTR, char*, int len, int, void*, int*) { return len; }

static unsigned char addr[16];
static const void* sa(unsigned ip_last, unsigned port) {
    memset(addr, 0, sizeof(addr));
    addr[0] = 2;                                   // AF_INET
    addr[2] = (unsigned char)(port >> 8); addr[3] = (unsigned char)port;
    addr[4] = 10; addr[5] = 0; addr[6] = 0; addr[7] = (unsigned char)ip_last;
    return addr;
}
static void send_to(unsigned ip, unsigned port) { hk_sendto(0, "x", 40, 0, sa(ip, port), 16); }
static void recv_from(unsigned ip, unsigned port) {
    int l = 16;
    unsigned char from[16];
    memcpy(from, sa(ip, port), 16);
    hk_recvfrom(0, (char*)"x", 400, 0, from, &l);
}

// ms of game: frames at 250 fps, usercmds / snapshots at 40 Hz each way (when enabled)
static void run(int ms, bool tx, bool rx, bool frames, unsigned ip = 1, unsigned port = 28960) {
    for (int t = 0; t < ms; t += 25) {
        if (tx) send_to(ip, port);
        if (rx) recv_from(ip, port);
        if (frames) g_frames += 6;
        Sleep(25);
        net_diag_tick();
    }
}

static int g_failed = 0;
static void check(bool ok, const char* what) { printf("  [%s] %s\n", ok ? "OK" : "FAIL", what); if (!ok) ++g_failed; }
static int count(const char* needle) {
    int n = 0;
    for (auto& l : logger::lines) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

int main() {
    QueryPerformanceFrequency(&g_freq);
    g_orig_sendto = fake_sendto;
    g_orig_recvfrom = fake_recvfrom;

    printf("1. server browser: 150 servers, one getinfo + one reply each, over 1.5 s\n");
    for (int i = 0; i < 150; ++i) { send_to(100 + i % 100, 28960 + i); recv_from(100 + i % 100, 28960 + i); g_frames += 2; Sleep(10); net_diag_tick(); }
    run(1500, false, false, true);
    check(count("net:") == 0, "no net line for the browser");
    check(count("game server") == 0, "no game server picked from the browser");

    printf("2. join a server, play 2 s, then 1.3 s with nothing coming in while we keep sending\n");
    run(2000, true, true, true);
    check(count("game server 10.0.0.1:28960") == 1, "the game server is picked");
    run(1300, true, false, true);
    run(500, true, true, true);
    check(count("reseau entrant ou serveur") == 1, "inbound silence reported once, as network/server");

    printf("3. play 2 s, then disconnect: both directions stop, the menu keeps rendering\n");
    logger::lines.clear();
    run(2000, true, true, true);
    run(2500, false, false, true);
    check(count("net:") == 0, "a disconnect is not reported");

    printf("4. play 2 s, then the game freezes 1.2 s (no frame, no packet), then resumes\n");
    logger::lines.clear();
    run(2000, true, true, true);
    Sleep(1200);
    net_diag_tick();
    run(300, true, true, true);
    check(count("jeu gele") >= 1, "a freeze is reported as a frozen game");

    printf("%s\n", g_failed ? "FAILED" : "ALL OK");
    return g_failed ? 1 : 0;
}
