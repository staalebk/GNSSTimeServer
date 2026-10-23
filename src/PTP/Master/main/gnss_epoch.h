#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Compare NMEA whole seconds against an independently advancing monotonic
 * projection of the last PPS-confirmed date. Allow fractional-message timing,
 * UART latency and a leap-second boundary, never a changed calendar era. */
static inline bool gnss_epoch_consistent(int64_t utc, int64_t rx_us,
                                         int64_t anchor_utc, int64_t anchor_us,
                                         int64_t *expected, int64_t *delta)
{
    int64_t elapsed_us = rx_us - anchor_us;
    *expected = anchor_utc + elapsed_us / 1000000LL;
    *delta = utc - *expected;
    /* Allow 1000 ppm monotonic-clock error across a long receiver outage. */
    int64_t tolerance = 2 + (elapsed_us < 0 ? -elapsed_us : elapsed_us) / 1000000000LL;
    return !anchor_us || (*delta >= -tolerance && *delta <= tolerance);
}
