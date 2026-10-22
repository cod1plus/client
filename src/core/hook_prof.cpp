// hook_prof.cpp - see hook_prof.h
#include "core/hook_prof.h"

namespace patches {

LONGLONG g_prof_ticks[PROF_COUNT] = {};
long     g_prof_calls[PROF_COUNT] = {};

namespace {
LARGE_INTEGER g_freq = {};
long   g_w_frames = 0;
double g_w_sum = 0, g_w_max = 0;
int    g_w_max_slot = -1;
double g_w_slot_sum[PROF_COUNT] = {};
const char* const k_names[PROF_COUNT] = { "bone", "lean", "inspect", "overlay", "rcdraw" };
}  // namespace

const char* prof_slot_name(int slot) { return slot >= 0 && slot < PROF_COUNT ? k_names[slot] : "?"; }

void prof_frame_poll(ProfFrame* out) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    const double k = g_freq.QuadPart ? 1000.0 / (double)g_freq.QuadPart : 0.0;
    double total = 0; int lead = -1; double lead_ms = -1;
    for (int i = 0; i < PROF_COUNT; ++i) {
        const double ms = g_prof_ticks[i] * k;
        out->ms[i] = ms; out->calls[i] = g_prof_calls[i];
        total += ms;
        g_w_slot_sum[i] += ms;
        if (ms > lead_ms) { lead_ms = ms; lead = i; }
        g_prof_ticks[i] = 0; g_prof_calls[i] = 0;
    }
    out->total_ms = total;
    g_w_frames++;
    g_w_sum += total;
    if (total > g_w_max) { g_w_max = total; g_w_max_slot = lead; }
}

void prof_window_stats(double* avg_ms, double* max_ms, int* max_slot, double* slot_avg_ms, bool reset) {
    if (avg_ms) *avg_ms = g_w_frames ? g_w_sum / g_w_frames : 0.0;
    if (max_ms) *max_ms = g_w_max;
    if (max_slot) *max_slot = g_w_max_slot;
    if (slot_avg_ms) for (int i = 0; i < PROF_COUNT; ++i) slot_avg_ms[i] = g_w_frames ? g_w_slot_sum[i] / g_w_frames : 0.0;
    if (reset) {
        g_w_frames = 0; g_w_sum = 0; g_w_max = 0; g_w_max_slot = -1;
        for (int i = 0; i < PROF_COUNT; ++i) g_w_slot_sum[i] = 0;
    }
}

}  // namespace patches
