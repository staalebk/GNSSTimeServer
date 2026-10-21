#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint64_t last, anchor;
    uint32_t nominal, period, anchor_slots, training;
    bool have_edge, calibrated;
    uint32_t accepted, rejected, missed, reacquired;
    int64_t last_error;
    uint64_t max_error;
} pps_gate_t;

static void pps_gate_init(pps_gate_t *g, uint32_t hz)
{
    memset(g, 0, sizeof(*g));
    g->nominal = g->period = hz;
}

/* ISR-owned state; all units are raw capture ticks. Rejects never move the
 * accepted baseline or learning anchor. The timer hardware stays unchanged. */
static bool pps_gate_accept(pps_gate_t *g, uint64_t edge, uint32_t gate_us)
{
    if (!g->have_edge) {
        g->last = g->anchor = edge;
        g->have_edge = true;
        g->accepted++;
        return true;
    }
    if (edge <= g->last) { g->rejected++; return false; }
    uint64_t delta = edge - g->last;
    if (delta > (uint64_t)g->period * 7 / 2) {
        /* More than three missed slots: lose phase/cadence confidence and
         * seed a fresh training run. Keep the learned frequency estimate. */
        g->calibrated = false;
        g->training = g->anchor_slots = 0;
        g->last = g->anchor = edge;
        g->reacquired++;
        g->accepted++;
        return true;
    }
    uint32_t slots = (uint32_t)((delta + g->period / 2) / g->period);
    if (!slots || slots > 3) { g->rejected++; return false; }
    int64_t error = (int64_t)delta - (int64_t)slots * g->period;
    uint64_t magnitude = error < 0 ? (uint64_t)-error : (uint64_t)error;
    uint32_t tolerance_us = g->calibrated ? gate_us : (g->training ? 100 : 1000);
    uint64_t tolerance = (uint64_t)g->period * tolerance_us / 1000000ULL;
    if (magnitude > tolerance) { g->rejected++; return false; }
    uint32_t observed = (uint32_t)((delta + slots / 2) / slots);
    /* Never learn an implausible rate (>1000 ppm from the configured clock). */
    uint32_t distance = observed > g->nominal ? observed - g->nominal : g->nominal - observed;
    if (distance > g->nominal / 1000) { g->rejected++; return false; }
    g->last_error = error;
    if (magnitude > g->max_error) g->max_error = magnitude;
    g->last = edge;
    g->accepted++;
    g->missed += slots - 1;
    g->anchor_slots += slots;
    if (!g->calibrated) {
        g->period = (uint32_t)((edge - g->anchor + g->anchor_slots / 2) / g->anchor_slots);
        if (++g->training >= 4) {
            g->calibrated = true;
            g->anchor = edge;
            g->anchor_slots = 0;
            g->max_error = 0;
        }
    } else if (g->anchor_slots >= 8) {
        /* Long-baseline observation followed by gentle rate tracking. */
        observed = (uint32_t)((edge - g->anchor + g->anchor_slots / 2) / g->anchor_slots);
        g->period = (uint32_t)(((uint64_t)g->period * 7 + observed + 4) / 8);
        g->anchor = edge;
        g->anchor_slots = 0;
    }
    return true;
}
