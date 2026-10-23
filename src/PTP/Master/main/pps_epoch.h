#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool pps_epoch_label_matches(int64_t previous_utc, int64_t utc,
                                           unsigned slots, int64_t elapsed_us)
{
    return previous_utc > 0 && slots >= 1 && slots <= 3 &&
           utc - previous_utc == (int64_t)slots &&
           elapsed_us >= (int64_t)slots * 1000000 - 100000 &&
           elapsed_us <= (int64_t)slots * 1000000 + 100000;
}

/* Short gaps retain the established epoch only when both independent counters
 * agree. Rejects cannot extend this beyond three seconds (two missed pulses). */
static inline unsigned pps_epoch_slots(uint64_t previous, uint64_t current,
                                       uint32_t period, uint32_t gate_us,
                                       bool calibrated, int64_t elapsed_us)
{
    if (!previous || current <= previous || !period || !calibrated) return 0;
    uint64_t delta = current - previous;
    if (delta > (uint64_t)period * 3 + (uint64_t)period * gate_us / 1000000)
        return 0;
    unsigned slots = (unsigned)((delta + period / 2) / period);
    if (slots < 1 || slots > 3) return 0;
    int64_t error = (int64_t)delta - (int64_t)slots * period;
    int64_t tolerance = (uint64_t)period * gate_us / 1000000;
    int64_t expected_us = (int64_t)slots * 1000000;
    return error >= -tolerance && error <= tolerance &&
           elapsed_us >= expected_us - 100000 &&
           elapsed_us <= expected_us + 100000 ? slots : 0;
}
