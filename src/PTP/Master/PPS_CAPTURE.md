# PPS hardware capture experiment

Current capture path: the GPIO ISR performs a raw, direct-register snapshot
and publishes it to a 32-record single-producer/single-consumer queue. It saves
the ETM latch, captures timer entry time, reads PHC control/seconds, brackets
one PHC subsecond read with GPTimer captures, and saves control/seconds again.
No timer-driver or Ethernet ioctl calls, spinlocks, semaphore operations,
cadence checks, conversions, or servo calculations run in this ISR. A bounded
rollover fallback is described below.
The timer's low-level software-capture primitive still waits for the hardware
clock-domain capture handshake; eliminating software locks does not eliminate
peripheral synchronization time.

Queue indices are lock-free 32-bit atomics. Release/acquire publication ensures
the task only sees completed records and the ISR never reuses a slot before
the task copies it. When full, new arrivals are dropped, preserving unread
records. `PPS_QUEUE capacity=32 dropped=...` reports the cumulative drop count.
The PPS task drains the queue and sleeps one RTOS tick (currently 10 ms) when
empty. Task latency does not enter the captured timestamps; UTC-label matching
continues using the stored ISR monotonic time. Long task stalls can overflow
the queue, and sufficiently delayed processing can also lose UART candidates.

Cadence gating/calibration, seconds-rollover/update validation, binary-to-ns
conversion, edge-delay subtraction, and diagnostic publication now happen in
the PPS task. Clock-update snapshots are rejected. Timer ownership is exclusive after initialization:
no other task may call GPTimer capture/read APIs on this timer. Boot resolves
the allocated timer group/index from the ETM capture task ID using the IDF
private ETM interface; it does not assume a fixed timer allocation. This code
therefore needs review when upgrading ESP-IDF or changing chips.

The following sections retain historical observations. The older ioctl and
direct-read comparison paths are no longer used for PPS capture; compatibility
log fields for ioctl comparison/retries remain zero. Cache-off interrupt
deferral, servo quality limits and gains remain unchanged.

## Rollover fallback

The initial direct snapshot rejected every sample within +/-1 us of the PHC
second boundary. Acquisition could dwell in that band for many seconds,
freezing correction and repeatedly losing its confirmed epoch despite clean
sub-microsecond read windows. The ISR now checks only the raw subsecond band
and seconds-before/after equality after its first snapshot. If either indicates
rollover risk, it waits 3 us with the ROM delay helper and captures exactly one
replacement PHC snapshot. Digital and binary PHC counter modes are supported.

The original PPS hardware edge, ISR-entry counter, and monotonic arrival time
are retained. The replacement has its own before/after GPTimer bracket; the
3 us wait is outside that bracket and included in edge-delay subtraction.
The task still applies all original rollover, control/update, capture-age and
read-window checks to the replacement. If it is also unsafe, it is rejected;
there is no unbounded PHC retry loop. `PPS_CAPTURE tries=2` and the existing
`TIMING_STATS direct_retry_samples` count identify the fallback. Normal samples
still take one snapshot. Validate this on hardware while acquisition crosses
the PHC second boundary with web polling active.

Current trial: `PPS_CAPTURE_APPLY_CORRECTION=1`. Valid delay-subtracted PPS
timestamps now drive acquisition, initial pre-service alignment, and the locked
servo. Gains, quality thresholds and lock requirements are unchanged. New NVS
writes remain disabled. The earlier observation-mode descriptions below record
the preceding experiments; the current behavior is specified in the paired-fit
section at the end.

GPIO 20 rising edges trigger an ETM task that latches a free-running GPTimer
at 40 MHz (25 ns per tick), independently of CPU interrupt dispatch. The
existing GPIO interrupt saves that latch before making any software capture.
The timer driver holds the selected clock's power-management lock while enabled.
Initialization failures stop boot explicitly.
The 80 MHz PLL source uses a divider of 2: the P4 timer does not permit a
divider of 1, so an 80 MHz capture resolution is unsupported on this path.

The Ethernet timestamp clock is the MAC's PHC, not a separate PHY clock.
For each accepted PPS, the ISR reads (the ioctl path is the current default):

1. Hardware edge latch (`edge`).
2. Software capture near handler entry (`entry`).
3. Software capture just before the PHC subsecond read (`before`).
4. PHC time (`phc`).
5. Software capture just after that read (`after`).

The GPIO handler now uses the calibrated cadence gate described below instead
of the former 500 ms guard. Rejected edges do not reach the servo.
All logging and cross-clock rate calculations run in
`pps_task`, outside the ISR. This adds instrumentation overhead to the read
path; the logs describe this instrumented firmware.

## Software GPTimer calibration and cadence gate

The hardware counter stays free-running. Its software conversion to nanoseconds
uses the measured ticks per PPS second, independently of the PHC frequency
adjustment. Four accepted intervals establish calibration. The first interval
allows 1 ms of nominal-clock error; subsequent training intervals allow 100 us.
Measured frequency must remain within 1000 ppm of the nominal clock.

After calibration, `PPS_CADENCE_GATE_US` in `main.c` selects a fixed +/-10 us
arrival window. The prediction uses the last accepted hardware edge and the
learned period, with one, two or three elapsed seconds allowed. Rejected edges
never move the baseline, consume a UTC label, or notify the servo. For example,
an extra edge 560 ms after a real PPS is rejected and the following real PPS
remains eligible. Every eight accepted seconds, a long-baseline observation
updates the period with a 1/8 smoothing weight.

After more than 3.5 seconds without an accepted edge, the next plausible capture
starts a new training run. This avoids permanent rejection after a signal loss;
that first edge is provisional, with UTC confirmation still handled separately
by the existing label logic. Captures older than 100 ms at ISR entry are rejected.
The software scale retains its previous calibration during reacquisition.

Periodic `PPS_GATE` logs report calibration state, ticks per second, conversion
scale, cumulative accepted/rejected counts, missed slots and reacquisitions.
`error_ticks` is the latest accepted edge's prediction error; `max_error_ticks`
is the largest accepted error since calibration. Divide ticks by roughly 40 to
get microseconds. These describe cadence confidence, not the PTP servo LOCK
state. The separate GNSS_SIG rejected count also includes invalid captures.

The default window is deliberately 10 us. Evaluate prediction errors on hardware
before tightening it to 1 us. Calibration cannot recover a real edge whose
single hardware capture latch was overwritten, or distinguish a false edge
inside the valid window. No changes were made to servo gains.

## Bounded UTC-label coasting

Two consecutive fresh labels still establish the PPS-to-UTC association at
startup. Once confirmed, up to `PPS_LABEL_MAX_COAST_SECONDS` (default 5)
consecutive missing labels can be bridged by advancing the associated UTC
second by one per hardware PPS. This never derives UTC from the PHC.

Coasting requires a valid GNSS fix, valid capture, calibrated cadence, and
exactly one hardware second since the preceding processed PPS (within the
configured cadence window). Missing PPS pulses, invalid captures, loss of fix,
or exhaustion of the five-pulse allowance invalidate the association and use
the existing holdover/reconfirmation path. A returning fresh label must match
the predicted UTC second; contradictory fresh labels cannot be coasted over.
After invalidation, two consecutive fresh labels establish a new association.
Read-window quality filtering still applies independently to every servo sample.

`PPS_EPOCH` logs identify coasting pulses, recovery to fresh labels, and loss
of the confirmed association. `TIMING_STATS` includes a separate `coast` count.
Missing-message debug snapshots are retained even when coasting succeeds.

## Reading PPS_CAPTURE logs

- `edge`, `entry`, `before`, `after`: raw timer ticks.
- `phc`: uncorrected PHC nanoseconds.
- `entry_delay`: edge-to-first-software-capture delay in ns. This includes
  latch retrieval and driver overhead, so it is not exact ISR entry latency.
- `read_window`: timer interval bracketing the complete PHC read in ns.
  The sampled subsecond value occurs somewhere within this interval. The
  ioctl comparison path brackets the entire `emac_get_time()` helper instead.
  The width is a timing uncertainty indicator,
  not just the register access execution time.
- `correction`: edge-to-midpoint delay in ns.
- `candidate`: `phc - correction`, or raw `phc` if the sample is invalid.
- `rate`: relative PHC/GPTimer rate in ppb, estimated from successive paired
  midpoint samples. This includes bracket error, PHC frequency adjustments,
  and any time steps. `rate_valid=0` marks missing, gapped or grossly
  discontinuous pairs; smaller PHC steps are not reliably distinguishable
  from rate changes. This is a diagnostic, not a calibrated rate estimate.
- `valid`: monotonic capture ordering, nonzero edge, correction <=100 us,
  read window <=20 us, and safe subtraction. It does not certify edge identity
  or absolute accuracy.
- `applied`: whether the candidate was actually sent to the servo.

## Observation and correction modes

`PPS_CAPTURE_APPLY_CORRECTION` near the top of `main.c` defaults to **0**:
hardware capture and candidate timestamps are logged, while the servo keeps
using the raw PHC read. Set it to **1** to trial midpoint subtraction for
valid samples. Invalid samples invalidate the PPS label, pause servo updates,
and reset interval-jitter tracking; they do not feed a raw fallback to the servo.

For the short edge-to-read interval the candidate assumes nominal timer
nanoseconds equal PHC nanoseconds. A relative rate error of 100 ppm contributes
10 ns over the maximum accepted 100 us delay. The measured `rate` lets us
evaluate this assumption before implementing a rate-calibrated translation.
The local mapping is rebuilt on every edge, avoiding a persistent offset
mapping that becomes stale when the servo steps the PHC.

The midpoint approximation has uncertainty up to roughly half the bracket
width, plus timer quantization, GPIO/ETM propagation, clock-rate error and
register timestamp semantics. It cannot reconstruct errors caused by a PHC
step during the bracket. Ethernet restart windows are already skipped by
the ISR. Observation mode should be used to evaluate acquisition and OTA
behavior before enabling correction.

## Limitations and board validation

The timer has a single capture latch, not an edge FIFO. Software raw reads
overwrite that latch, and subsequent electrical edges can overwrite the
original edge before the ISR reads it. A burst that overlaps the ISR can
therefore produce ambiguous samples that the simple ordering checks cannot
always detect. The 500 ms software guard does not prevent hardware recapture.
The present IRQ service is still deferred during flash-cache-off windows.
There is no claim that OTA delays or input glitches are fully corrected.

On the board, collect logs in observation mode while idle, under Ethernet/web
load, during OTA and after Ethernet recovery. Check entry delay, bracket width,
invalid samples and rate stability. Compare raw and candidate timestamp
differences (and an independent timing reference if available). Then trial
correction mode and compare offset spread, bias, lock acquisition and holdover.
An externally verified accuracy improvement is required before making the
correction the default. No board testing is performed by the firmware build.

## Direct PHC snapshot and ioctl comparison

`PPS_CAPTURE_DIRECT_PHC=1` selects a P4-specific read-only snapshot using the
same SoC register definitions as ESP-IDF's HAL. Other users of `emac_get_time()`
are unchanged. Set the switch to zero to measure the previous ioctl path.
Recheck the register layout when changing ESP-IDF or chip targets.

Each of at most three attempts reads seconds, brackets one subsecond read with
GPTimer captures, then rereads seconds. The attempt must have equal seconds,
no PHC initialize/update operation visible before or after, and unchanged
digital/binary rollover mode. Binary subseconds use IDF's multiplication and
right-shift conversion. Nanoseconds within 1 us of either second boundary are
conservatively rejected and retried; exhausted attempts yield an invalid
sample rather than a guessed epoch. This is not an atomic hardware PHC latch
and cannot detect every concurrent software time step.

`PPS_CAPTURE_COMPARE_IOCTL=1` adds a later ioctl read with its own bracket.
This comparison does not extend the direct sample's bracket, but does add ISR
execution time. `ioctl_window` reports the whole-helper read window and
`ioctl_delta` is ioctl PHC minus direct PHC minus elapsed nominal timer time
between their bracket midpoints. `compare_valid` rejects malformed ordering,
comparisons over 1 ms, and gross timestamp differences; small time steps can
still contaminate it. Stable values near zero indicate agreement within bracket
uncertainty. The comparison's clock-rate error at 52 ppm over 10 us is only
about 0.52 ns; uncertainty from read timing is expected to dominate.

New fields in PPS_CAPTURE are `direct`, `tries`, `ioctl_window`, `ioctl_delta`,
and `compare_valid`. PPS_LABEL includes `raw_offset` and `candidate_offset`
against the same confirmed GNSS edge, making bias and variation directly
comparable. Offset placeholders are zero when the label is unconfirmed.
Latency correction remains disabled by default. Collect both paths under idle
and network/web load before deciding whether to enable it. Servo gains and
averaging are unchanged. Turn off ioctl comparison after diagnosis to reduce
ISR overhead. The interrupt service remains non-IRAM/cache-deferred.

The direct path and ioctl comparison now default off following board testing:
although the direct bracket narrowed to about 1.9 us, its edge-to-sample delay
varied about 8.8-16.7 us, including retry-dependent delay. With correction off,
this disturbed acquisition. The previous ioctl path is restored for normal
operation; 5 Hz fractional-second labeling remains enabled. A narrow bracket
alone does not eliminate pre-bracket delay. Do not enable the direct path for
normal service until corrected sampling and rollover behavior are validated.

## GNSS second association diagnostics

`GNSS_TIME_ASSOC_LOG` in `main.c` enables `GNSS_ASSOC` lines for successfully
parsed, checksum-verified RMC/ZDA sentences and every accepted PPS. RMC must
have valid status; ZDA logs `used=0` if RMC has not established validity.
The complete sentence preserves fractional UTC seconds, while `utc` is the
integer Unix UTC second used by the current timing code. ZDA parsing accepts
both integer and fractional-second time fields.

Sentence `rx_us` is the monotonic time immediately after reading the UART chunk
containing the newline, before configurator processing and diagnostic logging.
It is not a hardware UART arrival timestamp: UART buffering, serialization and
task scheduling can delay it. Multiple sentences in a chunk share its timestamp.
`snapshot_pps_seq`, `pps_isr_us`, and `hw_edge` identify the latest published
accepted PPS at logging time. `rx_minus_pps_isr_us` is signed; the snapshot
may already contain a newer edge if processing was delayed. Sequence zero
means no accepted PPS was available, and the delta is a placeholder zero.

PPS lines identify the hardware edge, ISR observation time in monotonic
microseconds, raw PHC nanoseconds, latest GNSS UTC/source/reception time, and
the current `sentence_offset`. `pps_isr_minus_rx_us` is positive if the
sentence was read before the ISR observation. Zero reception time means no
used sentence yet. Negative ages can occur when the PPS task observes a
sentence received after the edge; these task snapshots do not prove what
was available at the instant of the edge. `isr_us` is sampled shortly after
the initial software timer capture, not at the electrical PPS edge.

Collect from boot through lock and include consecutive RMC/ZDA and PPS lines.
Compare the integer UTC labels with the independent GPS/NTP reference; ordering
alone cannot prove whether receiver firmware announces an upcoming or previous
second. Full sentence logging now defaults off to reduce UART traffic at 5 Hz.
The association rule is described below; it no longer uses PHC-derived labels.

## 5 Hz fractional-second association

At boot the configurator sends the supplied `06 42` 5 Hz command and enables
RMC/ZDA with CFG-MSG. Commands require ACK; configuration is not saved to
receiver flash. PPS remains 1 Hz: no CFG-PPS command is sent. The command
checksum matches the supplied `61 05` bytes. Verify firmware support by
observing actual mid-second messages, not just an ACK.

Only valid time-bearing sentences with fractional epochs from `.400` through
`.600` are queued as candidates. Their integer UTC second plus one labels the
following PPS. An eight-entry queue preserves pre-edge candidates if parsing
and PPS processing overlap. Selection uses monotonic UART chunk-read time:
the newest candidate must have been read 50-650 ms before the ISR observation.
Consumed or stale pre-edge candidates are removed; post-edge candidates are
retained for the following edge. A valid hardware capture is also required.

Two consecutive labels must advance by exactly one UTC second and their PPS
observations must be 0.9-1.1 seconds apart before the reference is trusted.
Missing/stale labels invalidate the reference and freeze servo updates; after
initial lock they trigger holdover behavior. There is no PHC-inference or 1 Hz
NMEA fallback. A receiver without mid-second messages will not lock.

`PPS_LABEL` logs show the selected integer `utc`, `fraction_ms`, reception
`age_us`, `confirmed`, and `target_tai`. Expect .400/.600 candidates, positive
ages of a few hundred milliseconds, and `confirmed=1` after consecutive edges.
Acquisition steps now subtract measured error from a current PHC read,
preserving elapsed time since PPS instead of setting a stale edge timestamp
after task/logging delay. Register read/write latency remains.

This assumes UART delivery latency is comfortably below one second. Freshness
and continuity cannot detect a consistently whole-second-old UART backlog or
a receiver with different timestamp semantics. Validate against the independent
GPS/NTP reference after lock and under load. If UART saturation occurs at 5 Hz,
reduce non-time output or change baud rate rather than relaxing freshness.
Startup PTP service gating is unchanged: clients may see acquisition corrections.

## Robust acquisition and measurement quality

Normal operation still uses ioctl PHC sampling with latency correction disabled.
Diagnostic field initialization after the PHC read reduces pre-sample work.
PPS_CAPTURE, confirmed PPS_LABEL, and steady servo diagnostic logs are throttled
to once every ten seconds/edges. Warnings and acquisition-window results remain
visible. This does not make the sample an absolute edge timestamp.

Second association retains the existing broad capture-validity checks. Servo
measurement quality is separate: read windows above 5 us are rejected before
coarse acquisition, frequency learning, or steady-state averaging/integration.
Rejected reads preserve the applied frequency and increment a diagnostic counter;
they do not destroy an otherwise valid GNSS second label. Five seconds without
a clean timestamp triggers holdover behavior. Loss of confirmed second labels
continues to trigger holdover immediately.

After the initial coarse/residual steps, acquisition holds the actuator fixed
for each non-overlapping window of approximately 32 seconds. At least 24 clean
samples spanning at least 28 seconds are required. The fit uses the median of
pairwise phase slopes with baselines of at least eight seconds, then a median
phase projected to the last sample. It reports the 90th-percentile detrended
residual (`p90`). Scratch arrays live outside the servo task stack and are used
only by that task. Sparse/noisy windows hold frequency rather than integrating
uncertain phase error.

For a usable fit, frequency changes by `-0.7 * drift_ppb - phase_ns / 120`,
limited to 100,000 ppb per window and the existing absolute frequency limits.
The 120-second phase term deliberately avoids aggressive per-sample steering.
Lock requires two consecutive usable windows with absolute fitted phase below
5 us, residual drift below 100 ppb, p90 at most 5 us, and at least 80% residual
inliers within 5 us. This replaces the old nine-consecutive-sample rule, so
acquisition can take several minutes. It is not evidence of failure if a few
windows are needed to learn the crystal's approximately 52 ppm correction.
After lock the existing slow PI(D)/averaging loop resumes with its integrator
initialized from the learned frequency; read-quality gating remains active.

`ACQUIRE-FIT` logs show phase, drift, p90, sample count, good windows out of two,
applied adjustment, and cumulative read-quality rejections. During acquisition
the UI Offset is the latest fitted phase after each successful window; in lock
it remains the existing moving-average phase estimate.

Validation: ESP-IDF firmware compilation/linking/image checks pass. A separate
Python reference-plant calculation exercised a 51,600 ppb oscillator error,
200 ns noise, two 50 us outliers and five rejected samples per 32-second window.
That model converged to the lock gates in 448 seconds, and additional model
checks covered isolated outliers and a sustained 300 ppb false-lock case.
These are numerical-design checks, not execution tests of the C fit helper,
hardware timing validation, or a guaranteed board lock time. Compare actual
ACQUIRE-FIT history and independent GPS/NTP accuracy over at least an hour.

## Label failure reasons and aggregated read timing

PPS_LABEL now appends a primary `reason` on each logged edge. Unconfirmed edges
are always logged. The categories, in precedence order, are:

- `no_fix`: current GNSS time validity is false.
- `capture_invalid`: the existing hardware capture/bracket checks failed.
- `too_old`: no eligible label and a queued candidate was older than 650 ms.
- `too_close`: no eligible label and a candidate was read within 50 ms before
  the ISR observation.
- `post_edge`: only candidates received after the edge were available.
- `no_midsecond`: no queued mid-second candidate was available. This does not
  prove the receiver failed to transmit one; parsing, delivery, and queue
  overwrite can also affect availability.
- `warmup`: a candidate exists but a preceding available label is needed.
- `utc_sequence`: the selected UTC second did not increment by one.
- `pps_interval`: the candidate seconds progress but ISR observations are
  outside the 0.9-1.1 s interval requirement.
- `ok`: confirmation succeeded.

Exactly one category is counted per processed PPS. If multiple rejected
candidate types coexist, older candidates take precedence over too-close and
post-edge candidates. Reason reporting does not change selection behavior.

The low-priority GNSS report task emits and resets TIMING_STATS each reporting
cycle (normally about 30 seconds, with the existing shorter first cycle).
Reporting runs even when GSV is missing/stale. `read_ns` is an exclusive-bin
histogram: <=4 us, (4,5] us, (5,8] us, (8,12] us, (12,20] us, >20 us.
`unavailable` counts samples without a usable recorded bracket width; these
are not included in the histogram. `quality_rejects` counts measurements that
fail capture validity or exceed the separate 5 us servo read-window limit,
even if the servo already skipped them for an invalid second label.

`ioctl_retry_samples` counts primary PPS PHC reads that used the helper's
third ioctl call after its first two timestamps appeared non-monotonic.
`extra_reads` counts those extra calls. `ioctl_retry_bins` uses the same timing
boundaries, showing whether slow windows correlate with that retry. These
counters exclude PHC reads from other tasks and the optional later comparison
read. Direct-mode retry samples are reported separately. PPS_CAPTURE also
includes `ioctl_retries` for the individual logged sample.

`clean_pairs`, `raw_mean`, and `candidate_mean` compare the same capture-valid,
confirmed samples with read windows <=5 us and absolute errors below 100 ms.
Gross boot/step offsets are excluded to avoid meaningless averages and overflow.
These are means of measured phase, not absolute calibration or dispersion
estimates. Candidate correction is still observation-only.

ISR work is limited to returning the primary PHC helper's retry count along
with the existing capture record. Histogram selection, reason classification,
and counter aggregation run in pps_task; logging runs outside the ISR. Counters
are snapshotted/reset under a separate short task lock. Gains, thresholds,
second association, lock rules, and correction-mode settings are unchanged.

## Persisted frequency and staged startup

NVS namespace `ptp_freq`, key `learned`, stores the integral-only oscillator
correction in ppb. The record has a magic value, version, Ethernet MAC binding,
and bounds of +/-100,000 ppb; size/version/MAC/range mismatches are ignored.
NVS provides blob integrity checking. NVS initialization errors do not cause
automatic erasure: the GM falls back to unseeded acquisition. The seed is
applied after PHC startup, before the timing tasks run. It never establishes
UTC validity or lock by itself. Temperature/aging can make it stale.

A low-priority worker samples the eligible integral-only correction every
30 seconds. Twenty-one observations span at least ten minutes. Any observed
holdover, loss of lock, or filtered phase error >=5 us invalidates continuity
through an epoch counter, including interruptions between worker polls. The
sample range must be <=200 ppb. Save requires at least a 100 ppb change from
the saved value (or no saved value); successful writes are separated by at
least six hours within a boot. A reboot restarts that interval, while the
change and stable-lock requirements still apply. No timestamp, phase error,
or proportional/derivative correction is persisted. Power interruption during
NVS writes is handled by NVS; record rejection falls back safely on next boot.

PTP Sync/Follow_Up, Announce and Delay Responses are withheld on boot until
the first usable acquisition fit. That means roughly 32 seconds of qualifying
data after initial steps, not a guarantee of 32 wall-clock seconds or exactly
32 accepted samples (the existing minimum is 24 samples spanning >=28 seconds).
The first fit subtracts the full measured residual drift from the applied
frequency, then removes the fitted phase using a current PHC read while
service is still gated. Service opens only if both driver operations succeed.
There are no log calls between that phase read and write. Subsequent windows
use the conservative controller and the existing two-good-window lock test.

Service-ready and locked are deliberately different: early Announce remains
class 248 with unknown accuracy, but advertises the actual PTP timescale and
valid UTC offset. Time/frequency traceability is not asserted until locked.
Some clients may wait for better clock quality regardless of packet availability.
Later full reacquisition closes service before a coarse step and reopens it
at lock; brief holdover preserves frequency and continues existing service.
The first pre-service phase alignment is not repeated on normal holdover.

`FREQ_NVS` logs show seed application, record rejection, saves and failures.
`PTP service enabled after initial fit` marks initial service availability.
The first run has no seed until sustained stable lock has qualified a save.
Keep the existing gains and correction mode while validating this change.
Verify cold boot without a record, boot after a successful save, lost/stale
GNSS during startup, service packet gating and reboot accuracy on the board.

## Paired acquisition-fit experiment

`PPS_COMPARE_FITS=1` now evaluates raw and delay-subtracted timestamps over
exactly the same accepted acquisition samples. The PPS task publishes both
timestamps atomically with the selected control timestamp and GNSS reference.
The candidate fit uses the same monotonic sample times, window boundaries,
quality gate and resets as the raw fit. The shared estimator scratch is used
sequentially and each result is copied before evaluating the other fit.

`PPS_FIT_COMPARE` reports sample count and span, then phase, drift, p90 residual
and inlier count for each path. `delay` reports mean/min/max of the subtracted
delay over those samples. Insufficient-data windows report both usability
flags rather than fabricated metrics. These comparison windows run during
acquisition, including before initial service alignment; they stop when the
existing controller enters steady lock. They are not a continuous locked-mode
stability monitor.

The control path now uses corrected timestamps (`PPS_CAPTURE_APPLY_CORRECTION=1`),
including the initial frequency fit, pre-service alignment and locked servo.
A separate raw-fit buffer preserves matched comparison diagnostics independent
of the selected control input. PPS_FIT_COMPARE reports `control=corrected`;
PPS_CAPTURE reports `applied=1` on valid samples. Invalid captures invalidate
the reference and do not reach the servo; read windows above 5 us still fail
the separate servo quality gate. No automatic winner is selected: a constant phase shift alone is not evidence
of improved precision, while reduced p90 and drift variation on matched samples
would support further evaluation of the candidate. Midpoint uncertainty and
capture-latch overwrite limitations still apply.

`FREQUENCY_PERSIST_WRITES_ENABLED=0` pauses new NVS calibration saves while
comparing measurement paths. Existing validated seeds are still loaded; no
record is erased. Restore writes only after the chosen capture path is stable.
The existing gains, thresholds, service gate, and lock criteria are unchanged.
After settling, the UI Offset is an average of corrected edge-phase estimates.
Raw PHC offsets may now sit about one capture delay positive when the corrected
offset is near zero; do not interpret that raw bias as a regression. Verify
absolute client time with the independent GPS/NTP reference and gather several
acquisition comparison windows before restoring calibration writes.
No additional PHC/GPTimer read or logging is added to the ISR by this experiment.

## Missing mid-second debug snapshots

The GNSS task keeps the most recent sixteen recognized RMC/ZDA reception
records, including checksum failures, parser outcomes and current fix validity.
Each record includes the raw time field (fractions preserved), monotonic chunk
read time and UART buffered-byte count observed just after reading that chunk.
`buffered=4294967295` means the buffer query failed. This measures software
buffer occupancy, not hardware FIFO overflow, and malformed message identifiers
are not recognized as RMC/ZDA. UART buffering and parser scheduling still affect
observed arrival times.

When no eligible candidate exists, pps_task snapshots the queue BEFORE consuming
entries and the recent RX history under short task locks. Only the first event
before each periodic report is retained; later events increment
`snapshots_suppressed`. The low-priority report task emits the snapshot as
MID_DEBUG lines, even when GSV is unavailable. No snapshot text is printed from
the ISR or PPS task. Queue-overwrite counts and total RX counts are lifetime
counters. Queue timestamps and ages reveal stale, too-close and post-edge data;
RX history reveals whether .400/.600 messages were seen, parsed and valid.

Startup can produce an empty-history debug snapshot before messages arrive.
Use later snapshots accompanying no_midsecond events for diagnosis. The extra
UART-task bookkeeping and periodic logging can affect scheduling, so this is
diagnostic instrumentation, not a transparent timing measurement.

The controller still requires a fresh association each PPS in this build.
Once absolute labeling is established, a future bounded-coasting policy can
advance the UTC second on verified consecutive PPS edges and use GNSS messages
as consistency checks. It must reject ambiguous/missing/extra edges and must
not extrapolate indefinitely without fresh GNSS time. That behavior has not
been changed by these debug additions.
