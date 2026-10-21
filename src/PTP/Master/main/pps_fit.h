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
static int pps_fit_evaluate(const pps_fit_t *fit, pps_fit_result_t *out)
{
    /* Servo-task-only scratch; avoid consuming its ISR-independent stack. */
    static double slopes[PPS_FIT_CAPACITY * (PPS_FIT_CAPACITY - 1) / 2];
    static double residuals[PPS_FIT_CAPACITY];
    unsigned count = 0;
    if (fit->n < 24 || fit->t[fit->n - 1] - fit->t[0] < 28)
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
        if (residuals[i] <= 5000) out->inliers++;
    }
    qsort(residuals, fit->n, sizeof(*residuals), pps_fit_compare);
    out->p90 = residuals[(9 * fit->n - 1) / 10];
    return 1;
}
