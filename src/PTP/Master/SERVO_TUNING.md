# Locked-servo tuning experiment

The baseline firmware uses Kp=0.05, Ki=0.001, D disabled, and the 32-sample
phase average. `SERVO_TUNING_TRIAL=1` near the top of main.c selects Ki=0.00025.
The trial has been disabled following comparison: the baseline gains are active.
The lower-Ki run reduced frequency movement but increased measured phase error
while the required frequency correction was changing.
Acquisition and PPS capture are unchanged. The acquisition-to-LOCK transition
already sets the integral to `-applied_ppb / SERVO_KI_SLOW`, so either build
preserves the acquired frequency contribution. This is a build-time selection,
not a live gain change. NVS stores ppb, not gain-dependent integral state.

## Recording on hardware

Keep web polling and board conditions similar. After lock settles, record at
least ten minutes of serial output. Each accepted locked-servo sample is
buffered once per PPS. The low-priority 30-second reporter emits `SERVO_TUNE`
lines with the original `sample_us`, corrected raw phase, averaged phase,
actually applied adjustment, and P/I/D contributions. The serial prefix is the
later print time; use `sample_us` for analysis. `SERVO_TUNE_STATS` records gains
and cumulative dropped trace samples. The 64-entry SPSC queue cannot block the
servo. Disable tracing afterward with `SERVO_TUNING_LOG_ENABLED=0`.

Holdover, rejected samples and trace overflow create gaps; the analyzer reports
them. Analyze one boot and preferably one uninterrupted locked segment at a
time. Trace output is diagnostic overhead, so use identical logging for both
gain profiles. The existing sparse SERVO messages remain available.

From the Master directory, run:

```text
python tools/servo-tuning.py --out build/servo-tuning
python tools/servo-tuning.py --log capture.txt --out build/servo-baseline
```

The first command writes simulation CSVs and summary.json. The second writes
samples.csv and summary.json with measured phase mean/RMS/p95/peak, frequency
peak-to-peak range, sample gaps and an exploratory frequency cycle estimate.
Cycle estimation removes a linear trend and searches 40–300 seconds, requiring
at least two cycles within the recording. It is descriptive, not a statistical
test for oscillation. Compare phase metrics as well as adjustment amplitude.

## Initial simulation results

This is a deterministic controller model, not measured oscillator behavior.
It models clean 1 Hz updates, full 32-sample averaging, integer averaging and
ppb output, integral clamp, and 100 ppb/s slew limit. It excludes holdover,
rejected samples, hardware timestamp bias and clock updates. The four scenarios
are a 1 us phase step, a 20 ppb frequency step, a 0.02 ppb/s frequency ramp,
and seeded 50 ns RMS measurement noise with approximate 25 ns bins.

| Ki (Kp=0.05) | Phase-step undershoot | Settle within 100 ns after phase step | Ramp phase RMS after 600 s |
|---|---:|---:|---:|
| 0.001 | 700 ns | 145 s | 22.7 ns |
| 0.0005 | 468 ns | 97 s | 42.2 ns |
| 0.00025 | 344 ns | 91 s | 81.0 ns |
| 0.0001 | 264 ns | 84 s | 178.1 ns |

Lower Ki reduces phase-step ringing but weakens frequency-drift tracking.
Ki=0.00025 is the proposed comparison profile, not an established optimum.
Use the hardware baseline before deciding whether reduced integral action
improves actual phase error. A synthetic 600-s, 90-s-period trace verifies the
analyzer's period detection; gain-rescaling checks are included in simulation.
