/* GNSS-referenced clock quality. Single writer; publish snapshots separately. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define CQ_BUCKETS 1440 /* one day of minute summaries; RAM only */
#define CQ_HORIZONS 4
#define CQ_MIN_TRIALS 30
#define CQ_HOLDOVER_SPEC_NS 100000.0 /* local policy: 100 us, not certification */
#define CQ_FALLBACK_PPB 1000.0
static const unsigned cq_horizon_s[CQ_HORIZONS] = {60, 300, 900, 3600};

typedef struct {
    double phase, integral, correction;
    int64_t epoch;
    uint32_t segment;
} cq_point_t;
typedef struct {
    cq_point_t points[CQ_BUCKETS];
    unsigned head, count, minute_samples;
    uint32_t segment;
    int64_t epoch;
    double previous_phase, previous2_phase, previous_correction;
    double integral, minute_peak, initial_error;
    double variance_sum;
    unsigned contiguous, variance_count;
    double variance_ring[1024];
    unsigned variance_head;
    double rate[CQ_HORIZONS];
    unsigned trials[CQ_HORIZONS];
    uint16_t variance;
} clock_quality_t;

static inline uint16_t cq_encode_variance(double seconds_squared)
{
    if (!(seconds_squared > 0) || !isfinite(seconds_squared)) return 0xffff;
    double v = 32768.0 + 256.0 * log2(seconds_squared);
    if (v < 0) v = 0;
    if (v > 65534) v = 65534;
    return (uint16_t)lround(v);
}

/* Break continuity without discarding past qualified trials. */
static inline void cq_gap(clock_quality_t *q)
{
    if (q->contiguous) ++q->segment;
    q->contiguous = q->minute_samples = 0;
    q->integral = q->minute_peak = 0;
    q->variance_sum = 0;
    q->variance_count = q->variance_head = 0;
    q->variance = 0xffff;
}

/* phase in ns; epoch is GNSS seconds, correction is actual PHC ppb.
 * Call before changing that correction. The previous correction approximates
 * the applied correction throughout the preceding one-second interval. */
static inline void cq_sample(clock_quality_t *q, int64_t epoch,
                             double phase, double correction)
{
    if (q->contiguous && epoch != q->epoch + 1) cq_gap(q);
    if (q->contiguous) q->integral += q->previous_correction;
    if (q->contiguous >= 2) {
        double d = phase - 2 * q->previous_phase + q->previous2_phase;
        double square = d * d;
        if (q->variance_count == 1024)
            q->variance_sum -= q->variance_ring[q->variance_head];
        else ++q->variance_count;
        q->variance_ring[q->variance_head] = square;
        q->variance_head = (q->variance_head + 1) % 1024;
        q->variance_sum += square;
        /* PTPVAR(tau=1s) = mean(second phase difference squared)/6. */
        if (q->variance_count >= 256) {
            uint16_t v = cq_encode_variance(fmax(q->variance_sum, 0) /
                                           (6.0 * q->variance_count) * 1e-18);
            /* One log2 unit hysteresis limits Announce churn. */
            if (q->variance == 0xffff || abs((int)v - (int)q->variance) >= 256)
                q->variance = v;
        }
    }
    q->previous2_phase = q->previous_phase;
    q->previous_phase = phase;
    q->previous_correction = correction;
    q->epoch = epoch;
    if (q->contiguous < 3) ++q->contiguous;
    q->minute_peak = fmax(q->minute_peak, fabs(phase));
    q->initial_error = fmax(1000.0, q->minute_peak);
    if (++q->minute_samples < 60) return;
    q->minute_samples = 0;
    q->minute_peak = 0;
    q->points[q->head] = (cq_point_t){phase, q->integral, correction, epoch, q->segment};
    q->head = (q->head + 1) % CQ_BUCKETS;
    if (q->count < CQ_BUCKETS) ++q->count;
    while (q->count && epoch - q->points[(q->head + CQ_BUCKETS - q->count) % CQ_BUCKETS].epoch > 86400)
        --q->count;

    /* Recompute rolling-day maxima from non-overlapping trials. A trial
     * reconstructs phase(t)-phase(0)-integral(applied)+frozenCorrection*t.
     * No assumption of independent one-second servo observations. */
    for (unsigned h = 0; h < CQ_HORIZONS; ++h) {
        unsigned minutes = cq_horizon_s[h] / 60;
        q->rate[h] = 0; q->trials[h] = 0;
        for (unsigned age = 0; age + minutes < q->count; age += minutes) {
            const cq_point_t *end = &q->points[(q->head + CQ_BUCKETS - 1 - age) % CQ_BUCKETS];
            const cq_point_t *start = &q->points[(q->head + CQ_BUCKETS - 1 - age - minutes) % CQ_BUCKETS];
            if (end->segment != start->segment ||
                end->epoch - start->epoch != cq_horizon_s[h]) continue;
            double error = end->phase - start->phase -
                (end->integral - start->integral) + start->correction * cq_horizon_s[h];
            q->rate[h] = fmax(q->rate[h], fabs(error) / cq_horizon_s[h]);
            ++q->trials[h];
        }
    }
}

static inline double cq_error_ns(const clock_quality_t *q, double age_s)
{
    double rate = 0;
    bool qualified = false;
    for (unsigned h = 0; h < CQ_HORIZONS; ++h) {
        if (q->trials[h] < CQ_MIN_TRIALS) continue;
        rate = fmax(rate, 2.0 * q->rate[h]);
        if (age_s <= cq_horizon_s[h]) qualified = true;
    }
    /* Never extrapolate a short trained horizon into a long outage. */
    rate = fmax(rate, qualified ? 100.0 : CQ_FALLBACK_PPB);
    return fmax(q->initial_error, 5000.0) + fmax(age_s, 0) * rate;
}

static inline uint8_t cq_accuracy(double error_ns)
{
    static const double limits[] = {25,100,250,1000,2500,10000,25000,
        100000,250000,1000000,2500000,10000000,25000000,100000000,
        250000000,1000000000,10000000000.0};
    if (!isfinite(error_ns)) return 0xfe;
    for (unsigned i = 0; i < sizeof(limits)/sizeof(limits[0]); ++i)
        if (error_ns <= limits[i]) return (uint8_t)(0x20 + i);
    return 0x31;
}
