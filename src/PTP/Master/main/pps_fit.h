#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#define PPS_FIT_CAPACITY 32
typedef struct { double t[PPS_FIT_CAPACITY], y[PPS_FIT_CAPACITY]; unsigned n; } pps_fit_t;
typedef struct { double slope, phase, p90; unsigned inliers; } pps_fit_result_t;
static int pps_fit_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double pps_fit_median(double *v, unsigned n)
{
    qsort(v, n, sizeof(*v), pps_fit_compare);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
/* Caller holds the applied frequency constant throughout each fit window.
 * Long-baseline median pair slopes reject isolated timestamp spikes. */
static int pps_fit_evaluate_window(const pps_fit_t *fit, pps_fit_result_t *out,
                                   unsigned minimum_samples, double minimum_span,
                                   double inlier_ns)
{
    /* Servo-task-only scratch; avoid consuming its ISR-independent stack. */
    static double slopes[PPS_FIT_CAPACITY * (PPS_FIT_CAPACITY - 1) / 2];
    static double residuals[PPS_FIT_CAPACITY];
    unsigned count = 0;
    if (fit->n < minimum_samples || fit->n > PPS_FIT_CAPACITY ||
        fit->t[fit->n - 1] - fit->t[0] < minimum_span)
        return 0;
    for (unsigned i = 0; i < fit->n; ++i)
        for (unsigned j = i + 1; j < fit->n; ++j)
            if (fit->t[j] - fit->t[i] >= 8)
                slopes[count++] = (fit->y[j] - fit->y[i]) / (fit->t[j] - fit->t[i]);
    if (!count) return 0;
    out->slope = pps_fit_median(slopes, count);
    for (unsigned i = 0; i < fit->n; ++i)
        residuals[i] = fit->y[i] + out->slope * (fit->t[fit->n - 1] - fit->t[i]);
    out->phase = pps_fit_median(residuals, fit->n);
    out->inliers = 0;
    for (unsigned i = 0; i < fit->n; ++i) {
        residuals[i] = fabs(fit->y[i] + out->slope * (fit->t[fit->n - 1] - fit->t[i]) - out->phase);
        if (residuals[i] <= inlier_ns) out->inliers++;
    }
    qsort(residuals, fit->n, sizeof(*residuals), pps_fit_compare);
    out->p90 = residuals[(9 * fit->n - 1) / 10];
    return 1;
}

static int pps_fit_evaluate(const pps_fit_t *fit, pps_fit_result_t *out)
{
    return pps_fit_evaluate_window(fit, out, 24, 28, 5000);
}

/* Startup seed verification only. Preserve the >=8-second slope baseline;
 * require a span >=14 s and much tighter residuals than the normal fit. */
static int pps_fit_evaluate_seed(const pps_fit_t *fit, pps_fit_result_t *out)
{
    return pps_fit_evaluate_window(fit, out, 12, 14, 500);
}

/* Only after saved-seed validation and startup phase recovery. Failed checks
 * return acquisition to two full windows, without changing the actuator. */
static inline int pps_fit_short_verify_ok(const pps_fit_result_t *fit, unsigned samples)
{
    return samples >= 12 && samples <= PPS_FIT_CAPACITY &&
           fit->p90 <= 500.0 && fit->inliers * 5 >= samples * 4 &&
           fabs(fit->phase) < 1000.0 && fabs(fit->slope) < 100.0;
}
