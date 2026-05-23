# FLOAT — Evaluation Document (Final)

> **Previous versions:** [v1 – Mid-term evaluation](https://github.com/your-repo/blob/midterm/docs/evaluation_v1.md)
> *(replace the link above with your actual GitHub history link)*

---

## 1. Requirements Overview

Four requirements define the FLOAT system's correctness and safety. They were refined
from the mid-term version based on the professor's feedback and on what turned out to
be measurable during actual hardware testing. Each is stated precisely below, followed
by a full account of how it was measured, what result was obtained, and — where the
target was not fully met — what was done and why.

| # | Requirement | Metric | Target | Result |
|---|---|---|---|---|
| R1 | Motor anomaly cutoff | Reaction time | < 2000 ms | ✅ ~1250 ms |
| R2 | Temperature out-of-range notification | Detection latency | < 10 s (sustained) | ✅ ~10 s by design |
| R3 | False positive rate | FPR over monitoring window | < 0.3 % | ✅ 0 % observed |
| R4 | Connectionless edge-to-edge communication | No router required for core safety | Pump stops without WiFi infra | ✅ Verified |

---

## 2. Requirement 1 — Motor Anomaly Cutoff < 2000 ms

### Statement
> When the pump motor enters a stall or dry-run condition, the system must cut power
> to the pump within **2000 ms** of the anomaly onset.

### Why this target

A brushed DC pump running under stall draws 3–5× its rated current. At that load,
motor windings reach destructive temperatures in 5–15 seconds depending on thermal
mass. A 2-second cutoff provides a safety margin of at least 3× before irreversible
damage, while remaining achievable given the communication and sampling constraints.

### How the algorithm works

**Step 1 — Calibration with Hampel filter.**
During the first boot (learning phase), the Observer collects motor current samples
while the pump runs normally. The mid-term version computed `mean + 3σ` over all
samples, which was corrupted by motor inrush spikes. The final version applies the
**Hampel filter** instead:

```
MAD  = median( |x_i − median(x)| )
σ_H  = 1.4826 × MAD
Inliers: x_i  where  |x_i − median(x)| ≤ 3 × σ_H
baseline_mean, baseline_std = stats of inliers only
th_stall   = baseline_mean + 3 × baseline_std
th_dry_run = 0.30 × baseline_mean
```

The factor 1.4826 makes `σ_H` a consistent estimator of σ under a normal distribution.
Because it uses the *median* rather than the mean, a single inrush spike (e.g.,
`[LEARN] #2 I=252.80 mA` in the serial log) has zero influence on the result: the
median of 34 samples remains near the true running current, and the spike is flagged
as an outlier and excluded.

**Calibration results from actual hardware (serial log):**
```
Samples      : 34
mean (μ)     : 192.44 mA
std  (σ)     :   8.17 mA
Stall thr    : 216.94 mA   (μ + 3σ)
Dry-run thr  :  57.73 mA   (30 % μ)
Volt min     :   3.41 V    (90 % of 3.79 V baseline)
```

**Step 2 — EWMA smoothing during monitoring.**
Raw current readings are smoothed with an Exponential Weighted Moving Average before
comparison against the threshold:

```
ewma_t = α × I_raw_t + (1 − α) × ewma_{t−1}     α = 0.2
```

With α = 0.2, a single-sample spike moves the EWMA by at most 20 % of its magnitude.
A genuine stall — where current stays elevated — raises the EWMA steadily over
successive samples.

**Step 3 — Confirmation gate.**
The anomaly flag must remain set for `CONFIRM_NEEDED = 3` consecutive loop iterations
before the system halts. Each iteration has a 400 ms delay. This prevents any
transient (one-tick) spike from triggering a false alarm.

**Step 4 — Grace period.**
The first 4 loop ticks after the pump starts (~1.6 s) are unconditionally skipped.
At the end of the grace period the EWMA is re-seeded from the current raw reading,
so inrush current cannot carry over into the monitoring window.

**Step 5 — Emergency HALT burst.**
Once the anomaly is confirmed, the Observer sends the `HALT` command **10 times in
rapid succession** (5 ms spacing), totalling ~50 ms. This guards against packet loss
from RF interference (microwave ovens, Bluetooth, other 2.4 GHz traffic). The Target
executes `digitalWrite(PUMP_PIN, LOW)` inside the ESP-NOW receive callback, which
runs at interrupt priority — no loop iteration is needed on the Target side.

### Latency decomposition

| Stage | Duration |
|---|---|
| INA219 sample period (loop delay) | 400 ms |
| Confirmation window: 3 × sample | 3 × 400 ms = 1200 ms |
| ESP-NOW HALT burst (10 × 5 ms) | 50 ms |
| Target ISR + GPIO write | < 1 ms |
| **Total worst-case** | **≈ 1251 ms** |

### Measurement method

The reaction time was measured from the Serial Monitor: the timestamp of the first
`[MON] ... STALL` flag in the monitoring log was compared to the timestamp of the
`[!!!] ANOMALY CONFIRMED` line. The distance is consistently exactly 3 loop
iterations (3 × 400 ms = 1200 ms), matching the analytical model. The HALT burst
adds a fixed 50 ms overhead that does not depend on the sample rate.

### Result: ✅ REQUIREMENT MET

**Measured reaction time: ~1250 ms**, well within the 2000 ms target. The 750 ms
margin provides headroom for any real-world jitter in the ESP-NOW link or the I2C
bus recovery routine.

---

## 3. Requirement 2 — Temperature Out-of-Range Notification < 10 s

### Statement
> If the water temperature remains outside the safe range **[18 °C, 30 °C]
> continuously for 10 seconds**, the system must activate the buzzer and push a
> warning notification to the dashboard. The pump is **not stopped** — temperature
> excursions are advisory warnings, not hard faults.

### Why this design choice

Unlike motor stall (which destroys hardware in seconds), temperature excursions
develop slowly and are rarely caused by the aquarium system itself (they typically
reflect heater failure or room temperature changes). Stopping the pump in response to
a temperature alert would actually worsen the fish's situation by removing circulation
and oxygenation. The correct response is to alert the owner while keeping the system
running.

This is fundamentally different from MOTOR_STALL and DRY_RUN, which justify an
immediate hard halt because continuing to run the pump causes irreversible damage.

### Detection logic

Temperature readings arrive from the Target every 500 ms during monitoring. The
Observer tracks a **dedicated confirmation counter** (`temp_warn_confirm`) that
increments on each sample where the temperature is outside [18, 30] °C and resets
to zero as soon as a valid in-range reading arrives.

```
Sampling period during monitoring   : 400 ms (Observer loop delay)
Confirmation threshold              : TEMP_WARN_NEEDED = 25 samples
Detection latency                   : 25 × 400 ms = 10 000 ms = 10 s
```

The threshold of 25 was chosen so that:
- Brief sensor glitches (which last 1–3 samples, ~0.4–1.2 s) never trigger the warning.
- A real excursion that lasts 10 s — long enough to confirm it is not a transient — always triggers it.

### DS18B20 glitch suppression

The DS18B20 sensor occasionally returns −127 °C (the `DEVICE_DISCONNECTED_C`
sentinel from the DallasTemperature library) when the 1-Wire conversion is not ready
or a CRC error occurs. This is a **known hardware behaviour**, not a firmware bug.

Two independent guards prevent it from causing false temperature warnings:

1. **Target side** (`safeTemp()` function): if the raw reading is below −10 °C, the
   function discards it and returns `last_valid_temp` instead. The corrected value
   (not the glitch) is what gets transmitted in the `DATA:SENSOR:` message.

2. **Observer side** (`OnDataRecv`): if the received temperature value is below
   −10 °C, it is silently discarded and `last_temp_c` is not updated.

Both guards are in the final firmware. In the serial log from the test session, the
`[WARN] Invalid temperature -127.0 C` printout appears several times, confirming that
the guard fires and prevents the glitch from propagating.

### On-action when triggered

When `temp_warn_confirm` reaches 25:
- The buzzer emits one short pulse (distinct from the 3-pulse pattern used for hard faults).
- The SSE JSON pushed to the dashboard includes `"temp_warn": true`.
- The dashboard displays an amber warning banner: *"Water temperature outside safe
  range [18°C – 32°C]. System continues."*
- A browser push notification is sent if the user has granted permission.
- The pump continues running.
- The counter resets to zero so the warning fires again if the excursion persists.

### Result: ✅ REQUIREMENT MET

The detection latency is **exactly 10 s by design** (25 samples × 400 ms). The
glitch suppression was validated against the test log, where multiple −127 °C
readings appeared without triggering false warnings.

---

## 4. Requirement 3 — False Positive Rate < 0.3 %

### Statement
> During normal pump operation (no mechanical fault), the rate of incorrectly
> triggered HALT events must remain **below 0.3 %** of monitoring samples.

### What a false positive means here

A false positive is a HALT event that fires while the pump is running normally — no
stall, no dry run, no actual voltage drop. It is the most dangerous failure mode
because it stops the pump unnecessarily, disrupting water circulation and alarming
the user without cause. Repeated false positives would make the system untrustworthy
and cause users to disable the monitoring feature.

### Why EWMA + Hampel achieves a low FPR

**The mid-term 3-sigma approach and its failure mode.**
In the old version, `th_stall = mean + 3σ` was computed over the raw learning
samples. The learning window included the motor inrush spike: `[LEARN] #2 I=252.80 mA`
(visible in the serial log). With plain 3-sigma, this spike inflates both the mean
and σ, but in an unpredictable direction: if only a few spikes are present, they
raise the mean slightly but inflate σ more, which can *lower* the effective threshold
margin and increase FPR. Alternatively, if the spike is large enough to raise the
mean substantially, the threshold becomes too high and real stalls are missed (false
negatives). Neither outcome is acceptable.

**How the Hampel filter fixes the calibration.**
By computing the baseline on *inliers only* (samples within `3 × σ_H` of the
median), the Hampel filter guarantees that the 252.80 mA spike — which is more than
7 standard deviations above the median in the normal-running distribution — is
excluded. The resulting baseline is computed purely from steady-state current values
(the ~180–206 mA range), giving a tight and accurate threshold at 216.94 mA.

**How EWMA prevents transient spikes from triggering false positives.**
Even with a perfect threshold, raw current readings fluctuate. The observed
steady-state range in the serial log is 179–206 mA, with occasional peaks near
218–222 mA (a single-sample fluctuation). With EWMA at α = 0.2, a single spike
to 220 mA moves the EWMA from its nominal value of ~192 mA by only:
```
Δ_ewma = 0.2 × (220 − 192) = 5.6 mA   →   ewma after spike ≈ 197.6 mA
```
This is still 19.3 mA below `th_stall = 216.94 mA`. The spike cannot trigger the
threshold on its own. Only a sustained elevation — the kind caused by a real stall —
can push the EWMA above the threshold.

**The 3-sample confirmation gate as a final safety net.**
Even if the EWMA briefly crossed the threshold due to an unusually large transient,
it would need to stay above it for 3 consecutive 400 ms samples (1.2 s total). A
single-sample transient lasting 400 ms cannot satisfy this condition.

### Quantitative FPR calculation

During the test session, the monitoring window contained the following samples
(from the serial log, after the grace period ended):

```
Samples in monitoring window   : 25 (approx.)
HALT events triggered          : 1  (TEMP_OUT_OF_RANGE, caused by -127 glitch
                                      in an earlier firmware version)
HALT events in final firmware  : 0
False positives (motor-related): 0
```

**Observed FPR (final firmware) = 0 / 25 = 0.0 %**, which satisfies the < 0.3 %
requirement.

To place the 0.3 % target in perspective: with a 400 ms sampling rate, 0.3 % of
samples over a 10-second monitoring window (25 samples) corresponds to 0.075
samples — effectively, the requirement demands zero false HALT events in any single
10-second run. Our system achieves this by combining three independent suppression
mechanisms (Hampel calibration, EWMA smoothing, confirmation gate), providing defence
in depth against false triggers.

### Result: ✅ REQUIREMENT MET

**Observed FPR: 0.0 %** across all test runs with the final firmware.
The combination of Hampel-filtered calibration, EWMA monitoring, grace period, and
3-sample confirmation provides robust false-positive suppression that improves on the
mid-term 3-sigma approach in a principled and measurable way.

---

## 5. Requirement 4 — Connectionless Edge-to-Edge Communication

### Statement
> The safety-critical communication between Target and Observer (pump stop commands,
> anomaly detection, sensor telemetry) must operate **without requiring any WiFi
> router or Internet infrastructure**. The system must respond to motor anomalies
> even when no external network is available.
>
> Note: WiFi is still used by the system — the Observer serves the dashboard over a
> local WiFi Access Point. The requirement is that the core safety loop
> (Target ↔ Observer) does not depend on it.

### Why this distinction matters

This requirement is often misunderstood. FLOAT uses WiFi extensively:
- The Observer creates a WiFi AP (`FLOAT-Dashboard`) that users connect to.
- The dashboard is served over HTTP on this AP.
- The browser-to-Observer communication (POST commands, SSE stream) uses TCP/IP over
  this AP.

None of this means the system is cloud-dependent or router-dependent. The critical
clarification is: **ESP-NOW and WiFi infrastructure are orthogonal**.

ESP-NOW is a connectionless Layer-2 protocol that transmits raw 802.11 frames between
two MAC addresses. It does not use DHCP, does not require an AP association, does not
go through a router, and does not need an IP address on either side. Both nodes
communicate directly, at the hardware frame level.

The Observer operates in `WIFI_AP_STA` mode: the STA interface is used exclusively
for ESP-NOW (no association to any external AP), while the AP interface serves the
dashboard. The two functions share the same radio hardware but are independent at the
protocol level.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    OBSERVER  (WIFI_AP_STA mode)                     │
│                                                                      │
│  STA interface ◄──── ESP-NOW (MAC-level, no router) ────► Target    │
│                         Channel 13, no DHCP, no IP                  │
│                                                                      │
│  AP  interface ◄──── WiFi (TCP/IP) ────► Browser (phone/laptop)     │
│                         192.168.4.1, HTTP, SSE                      │
└─────────────────────────────────────────────────────────────────────┘
```

If the user's phone is not connected, or if no phone is present at all, the
Observer still detects motor anomalies and sends HALT to the Target. The dashboard
is informational; the safety loop is autonomous.

### Why ESP-NOW instead of standard WiFi / MQTT

| Protocol | Router required | Association latency | TX current peak | Packet loss risk |
|---|---|---|---|---|
| MQTT over WiFi | Yes | 200–500 ms | ~180 mA | Low, but retried |
| HTTP REST | Yes | 300–600 ms | ~180 mA | Low |
| BLE GATT | No | 20–50 ms | ~20 mA | Moderate |
| **ESP-NOW** | **No** | **< 5 ms** | **~80 mA** | **Low with burst** |

The WiFi association process (DHCP, authentication, TCP handshake) draws sustained
current at ~180 mA. On a 3.7 V LiPo under partial depletion, this creates a voltage
sag that can brown-out the ESP32 and reset it — the opposite of what a safety system
should do when detecting a fault. ESP-NOW transmits a single 250-byte frame with no
prior association, eliminating the sag entirely.

### Channel locking

Both nodes are forced to channel 13 via `esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE)`
before `esp_now_init()`. The default behaviour (channel 0) allows the ESP32 radio to
drift to whatever channel background WiFi activity is using, which silently breaks
ESP-NOW communication. Fixed-channel operation guarantees frame delivery regardless
of nearby router activity.

### HALT burst and ACK reliability

**HALT (safety-critical, one-directional):** sent 10 times with 5 ms spacing.
A single 2.4 GHz interference event lasts typically 1–20 ms. Ten packets spaced 5 ms
apart ensures that at least several are transmitted in the interference gaps, making
it statistically near-certain that the Target receives at least one HALT.

**ACK + retry (command reliability):** critical commands (START_LEARN,
START_MONITOR, STOP_MEASURE) include a sequence number (`|ID:N`). The Observer
replies `ACK:N`. The Target retries up to 5 times with exponential backoff (50, 100,
150, 200, 250 ms). If 3 consecutive commands go unacknowledged across boots,
`consecutive_noack` increments and the Target enters safe mode (60 s deep sleep),
avoiding runaway operation without oversight.

In the test session, `[WARN] No ACK for START_LEARN` appears once in the log,
demonstrating that the retry mechanism is exercised and that the system continues
operating gracefully without the ACK (it logs the warning but proceeds).

### Result: ✅ REQUIREMENT MET

The Target–Observer safety loop operates at the MAC level via ESP-NOW and requires
no router, no DHCP, no IP address, and no Internet. The dashboard WiFi AP is additive
— it enriches the user experience without being part of the safety-critical path.
The system was tested with no external WiFi router present and performed identically
to tests conducted in the presence of a router.

---

## 6. Unmet Goals and Honest Assessment

### 6.1 Real turbidity sensor on ESP32

The analog turbidity sensor was connected to ESP32 ADC pin 1. The full reading
function (`readTurbidityNTU()`) was implemented with 64-sample averaging and
temperature compensation. In testing, ADC readings were corrupted by the ESP-NOW
radio emission regardless of whether readings were taken before or after `WiFi.mode()`.
The current firmware uses `readTurbidityRandom()` for demo purposes.

The software integration — threshold comparison, pump trigger, servo trigger,
temperature compensation — is complete and exercised via the simulated values. The
hardware fix (routing the sensor through an Arduino that forwards values over UART or
I2C) is identified and would require one additional component and a minor firmware
change.

### 6.2 Adaptive temperature thresholds

The current threshold [18, 30] °C is static. The architecture fully supports deriving
personalised limits from the learning phase (temperature is already transmitted every
500 ms during the learning window). The accumulator and adaptive threshold computation
in `CMD:STOP_MEASURE` were not completed due to time constraints. Static limits are
functionally correct for tropical species and do not affect the correctness of the
other requirements.

### 6.3 Dashboard HTTPS

HTTP is used rather than HTTPS. The local-AP network model limits the attack surface
to devices physically present in the room. WPA2 authentication on the AP, POST-only
command endpoints, and locked-state 423 responses are implemented. TLS on the ESP32
`WebServer` library requires self-signed certificate management and was identified
as a future improvement rather than a blocker for the current deployment context.

---

## 7. Test Evidence Summary

All rows reference observable evidence from the serial monitor log captured during
real hardware testing.

| Test | Evidence | Target | Status |
|---|---|---|---|
| Hampel calibration (34 samples) | μ=192.44, σ=8.17, th=216.94 | Stable, spike-free baseline | ✅ |
| Inrush spike excluded | Sample #2 (252.80 mA) not in baseline | Hampel filter working | ✅ |
| EWMA convergence | ewma rises 114→187 mA over grace period | Smooth tracking | ✅ |
| Grace period | 4 ticks logged as "Motor inrush — alarms suppressed" | Inrush ignored | ✅ |
| EWMA re-seed at grace end | "EWMA seeded at X mA" log line | No inrush carry-over | ✅ |
| Motor anomaly reaction time | 3 `STALL` ticks × 400 ms = 1200 ms | < 2000 ms | ✅ |
| DS18B20 glitch suppression | "[WARN] Invalid temperature -127.0 C" suppressed | No false TEMP alarm | ✅ |
| Temperature warning (not halt) | Pump continues on TEMP event | System not stopped | ✅ |
| FPR (final firmware) | 0 false HALT events across all runs | < 0.3 % | ✅ |
| ESP-NOW without router | Tested with no external AP present | Connectionless | ✅ |
| ACK retry | "No ACK for START_LEARN" + system continues | Graceful degradation | ✅ |
| Deep sleep duty cycle | Active ~2 s / 22 s = 9.1 % | ≤ 10 % active | ✅ |
| Real turbidity sensor | ADC corrupted by RF; random demo used | Real reading | ❌ workaround |
| Adaptive temperature limits | Static [18, 30] °C used | Dynamic from learning | ⚠ partial |
| Dashboard HTTPS | HTTP + WPA2 only | HTTPS | ⚠ partial |

---

## 8. Reflection on the Evaluation Process

The most valuable aspect of building this evaluation was being forced to make each
requirement *measurable*. The motor cutoff requirement (< 2000 ms) could be computed
analytically (3 × 400 ms) and verified empirically from the serial log. The
temperature notification requirement needed a concrete definition of "sustained" — we
chose 10 s / 25 samples, which eliminates glitch noise while remaining responsive to
real excursions. The FPR requirement demanded a precise definition of what counts as
a false positive and a clear explanation of *why* EWMA + Hampel achieves a lower rate
than the mid-term 3-sigma approach.

Where requirements were not fully met (turbidity hardware, adaptive temperature,
HTTPS), the evaluation explains the concrete technical reason, what was done instead,
and what the minimal change would be to close the gap. This is intentional: an
evaluation that only reports successes is not trustworthy.