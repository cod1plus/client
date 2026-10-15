// pacing_stats.cpp - see pacing_stats.h
//
// Both notes come from the game thread (the limiter's wait and the SwapBuffers hook run on
// it); session_report reads and resets once a minute from the patch watcher thread, so one
// critical section guards the two series - uncontended on the game thread, and the reader
// holds it for the few microseconds of a 2000-bin pass. A histogram, not a sample list: 0.02 ms bins up to 40 ms, the rest in
// one overflow bin (its max is kept exactly), so the percentile is exact to a bin and the
// memory is fixed whatever the frame rate.
#include "performance/pacing_stats.h"

#include <cmath>
#include <cstring>

namespace patches {

namespace {

constexpr int    BINS = 2000;
constexpr double BIN_MS = 0.02;

struct Series {
    LONGLONG last = 0;
    long     hist[BINS + 1];
    long     n = 0;
    double   sum = 0, sum2 = 0, max = 0;
    Series() { memset(hist, 0, sizeof(hist)); }

    void note(LONGLONG qpc, LONGLONG freq) {
        if (last && qpc > last && freq) {
            const double ms = (double)(qpc - last) * 1000.0 / (double)freq;
            if (ms < 1000.0) {            // a load / an alt-tab is not a frame interval
                int b = (int)(ms / BIN_MS);
                if (b > BINS) b = BINS;
                ++hist[b];
                ++n;
                sum += ms;
                sum2 += ms * ms;
                if (ms > max) max = ms;
            }
        }
        last = qpc;
    }

    void read(PacingStats* out) const {
        memset(out, 0, sizeof(*out));
        out->n = n;
        if (!n) return;
        out->mean_ms = sum / n;
        const double var = sum2 / n - out->mean_ms * out->mean_ms;
        out->sd_ms = var > 0 ? std::sqrt(var) : 0;
        out->max_ms = max;
        const long want = n - n / 100;    // 99th percentile
        const double over_ms = out->mean_ms * 1.25;
        long acc = 0;
        bool p99_set = false;
        for (int b = 0; b <= BINS; ++b) {
            acc += hist[b];
            if (!p99_set && acc >= want) { out->p99_ms = b < BINS ? (b + 1) * BIN_MS : max; p99_set = true; }
            if (b * BIN_MS > over_ms) out->over += hist[b];
        }
    }

    void reset() {
        memset(hist, 0, sizeof(hist));
        n = 0; sum = sum2 = max = 0;          // `last` kept: the next interval is still real
    }
};

Series   g_frames, g_presents;
LONGLONG g_freq = 0;

struct Lock {
    CRITICAL_SECTION cs;
    Lock() { InitializeCriticalSection(&cs); }
} g_lock;

LONGLONG freq() {
    if (!g_freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_freq = f.QuadPart; }
    return g_freq;
}

}  // namespace

void pacing_note_frame(LONGLONG qpc) {
    const LONGLONG f = freq();
    EnterCriticalSection(&g_lock.cs);
    g_frames.note(qpc, f);
    LeaveCriticalSection(&g_lock.cs);
}

void pacing_note_present(LONGLONG qpc) {
    const LONGLONG f = freq();
    EnterCriticalSection(&g_lock.cs);
    g_presents.note(qpc, f);
    LeaveCriticalSection(&g_lock.cs);
}

void pacing_stats(PacingStats* frames, PacingStats* presents, bool reset) {
    EnterCriticalSection(&g_lock.cs);
    if (frames) g_frames.read(frames);
    if (presents) g_presents.read(presents);
    if (reset) { g_frames.reset(); g_presents.reset(); }
    LeaveCriticalSection(&g_lock.cs);
}

}  // namespace patches
