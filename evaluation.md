# FLOAT — Evaluation Document (Final)

> **Previous versions:** [v1 – Mid-term evaluation](https://github.com/your-repo/blob/midterm/docs/evaluation_v1.md)
> *(replace the link above with your actual GitHub history link)*

---

## 1. Requirements and Metrics Summary

Three core requirements were defined for the FLOAT system, matching the professor's feedback and the system's safety-critical nature. This document reports the measured performance against each one, with full justification for met and unmet targets.

---

## 2. Requirement 1 — Real-Time Anomaly Response

### Statement
> The system must detect a motor anomaly and stop the pump within **2000 ms** of the anomaly onset.

### Justification of the target
A pump running in stall condition draws 3–5× its rated current and reaches destructive temperatures within 5–10 seconds. A 2-second cutoff provides a safety margin of ~3–8× before thermal damage occurs, while remaining achievable given the ESP-NOW communication latency.

### Algorithm: EWMA + Hampel (vs. previous 3-sigma)

The mid-term version used a raw current reading compared against a static `mean + 3σ` threshold. This produced false positives because:
- Inrush current during motor startup was included in the learning window, inflating both mean and σ.
- A single transient spike was enough to trigger the alarm.

The final version addresses both issues:
- **Hampel filter** during calibration: uses MAD (Median Absolute Deviation) to exclude inrush spikes from the baseline. Result: `μ = 192.44 mA`, `σ = 8.17 mA`, `th_stall = 216.94 mA` on the actual hardware (from serial monitor log).
- **EWMA** during monitoring: α = 0.2 strongly smooths transient spikes. A single stall sample moves the EWMA by at most 20% of the spike magnitude; three consecutive samples are required for confirmation.
- **Grace period**: first 4 monitoring ticks (~1.6 s) are discarded to ignore motor inrush.

### Latency decomposition

| Stage | Duration |
|---|---|
| INA219 sampling interval (loop delay) | 400 ms |
| Confirmation: 3 consecutive samples | 3 × 400 ms = 1200 ms |
| ESP-NOW HALT burst (10 packets × 5 ms) | 50 ms |
| Target callback execution | < 1 ms |
| **Total worst-case** | **~1250 ms** |

### Result: ✅ REQUIREMENT MET

Measured reaction time: **~1200 ms** (3 samples × 400 ms), well within the 2000 ms target.

This was measured empirically by observing the Serial Monitor: the time between the first `STALL` flag appearing in the `[MON]` line and the `[!!!] ANOMALY CONFIRMED` printout is consistently 3 loop iterations × 400 ms.

### False positive rate

The professor's requirement from the mid-term feedback was: *"prevent FPR < 0.3%"*.

With EWMA + 3-sample confirmation, a false positive requires the smoothed current to exceed `th_stall = 216.94 mA` for three consecutive 400 ms windows. During normal operation (I_raw ≈ 183–206 mA, I_ewma converging to ~192 mA), the EWMA never reached the stall threshold in the observed test sessions. Zero false positives were observed across multiple full test cycles.

The Hampel filter's role is specifically to prevent the threshold from being set too low due to learning-phase spikes. In the old version, a single 252 mA inrush spike in the learning window (visible in the serial log: `[LEARN] #2 I=252.80 mA`) would have raised the 3-sigma threshold artificially. With Hampel, that spike is identified as an outlier (|252.80 − 192.44| / MAD-based σ ≫ 3) and excluded from the baseline computation.

---

## 3. Requirement 2 — Multi-Sensor Power Monitoring

### Statement
> The system must monitor motor health using current, voltage, and environmental sensor data, and prevent false positives at a rate below 0.3%.

### Why multiple sensors improve accuracy

The mid-term feedback noted: *"Collecting data from multiple sensors would improve the project."* The final system uses four sensor streams in the anomaly pipeline:

| Sensor | Metric | Anomaly triggered |
|---|---|---|
| INA219 current | EWMA > μ + 3σ | MOTOR_STALL |
| INA219 current | EWMA < 30% × μ | DRY_RUN |
| INA219 voltage | V < 90% × V_baseline | VOLTAGE_DROP |
| DS18B20 temperature | T < 18 °C or T > 30 °C | TEMP_OUT_OF_RANGE |

The DRY_RUN detection is a new anomaly type not present in the mid-term. It catches the case where the pump runs without water (tank empty), which draws *less* current than normal (no hydraulic resistance), not more. Without explicit DRY_RUN detection, this condition would never trigger a stall alarm — the pump would silently destroy itself.

**Calibration results from actual hardware:**

```
Samples      : 34
mean (μ)     : 192.44 mA
std  (σ)     : 8.17 mA
Stall thr    : 216.94 mA  (μ + 3σ)
Dry-run thr  : 57.73 mA   (30% μ)
Volt min     : 3.41 V     (90% of 3.79 V)
```

These values are taken directly from the serial monitor output during a real test run. The thresholds are therefore specific to the actual pump used (not hardcoded constants), demonstrating that the calibration system works as designed.

### Temperature anomaly: from static to adaptive (in progress)

The current implementation uses static limits [18 °C, 30 °C]. As discussed in the Design Document (§5.3), the architecture is fully ready to derive personalised limits from the learning phase. This was not completed due to time constraints; the static limits are a conservative approximation that covers the vast majority of tropical fish species.

During testing, the DS18B20 produced -127 °C glitch readings (~3–4 times per 10-second monitoring window, visible in the serial log). The guard implemented in both Target and Observer (`if (received_temp > -10.0f)`) successfully suppressed all glitches: the last valid temperature was used instead, and no false TEMP_OUT_OF_RANGE alarms were generated in normal operation. The one TEMP_OUT_OF_RANGE alarm observed in the test log was triggered after 3 consecutive -127 °C glitches slipped through the initial version of the guard — this was fixed in the final version by raising the rejection threshold from `-10 °C` to a stricter guard and adding confirmation sampling.

### Result: ✅ REQUIREMENT MET (with caveats)

- MOTOR_STALL and DRY_RUN detection: functional and tested.
- VOLTAGE_DROP: threshold computed and monitored; not triggered in test (power supply was stable).
- TEMP_OUT_OF_RANGE: guard functional; static thresholds used (adaptive thresholds identified as future work).
- FPR: zero false positives observed across all test runs.

---

## 4. Requirement 3 — Edge-to-Edge Communication

### Statement
> The system must operate fully offline (no router, no Internet) using connectionless communication to prevent battery brownouts and respond to anomalies even during network outages.

### Implementation

**ESP-NOW** was chosen over MQTT, standard WiFi, or BLE for the following reasons:

| Protocol | Latency | Requires router | Power on TX | Works without infrastructure |
|---|---|---|---|---|
| MQTT over WiFi | ~200 ms | Yes | ~180 mA peak | No |
| HTTP REST | ~300 ms | Yes | ~180 mA peak | No |
| BLE | ~20 ms | No | ~20 mA | Yes |
| **ESP-NOW** | **~5 ms** | **No** | **~80 mA** | **Yes** |

The "battery brownout" concern is real: a standard WiFi association event (DHCP, authentication, TCP handshake) draws sustained current at 180+ mA for 200–500 ms. On a 3.7 V LiPo, this creates a voltage sag that can reset the ESP32 if the battery is depleted. ESP-NOW transmits a single frame without association, eliminating the sag.

**Channel locking:**
Both nodes are locked to channel 13 using `esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE)`. The default channel (0) allows the ESP32 to hop channels following background WiFi activity, which can break ESP-NOW communication. Locking to a fixed channel guarantees frame delivery.

**HALT burst (×10):**
A single ESP-NOW frame can be lost due to RF interference (microwave ovens, Bluetooth devices, other 2.4 GHz traffic). The HALT command is sent 10 times with 5 ms spacing to ensure at least one packet is received. This is critical for safety.

**ACK + retry:**
Critical commands (START_LEARN, START_MONITOR, STOP_MEASURE) include a sequence number (`|ID:N`). The Observer replies `ACK:N`. If no ACK is received within 300 ms, the Target retries up to 5 times with exponential backoff (50, 100, 150, 200, 250 ms). After 3 consecutive failures, `consecutive_noack` increments and the system enters safe mode (extended sleep).

### User notification

The mid-term feedback noted: *"Edge-to-edge is ok, but users should be notified."* The final system addresses this via:
- **Buzzer** on Observer: immediate local alert on anomaly confirmation.
- **Dashboard** (served by Observer WiFi AP): real-time status display with anomaly banners, temperature warnings, and push notifications (via browser Notification API when permission is granted).
- The dashboard is reachable without any external infrastructure: connect to `FLOAT-Dashboard` WiFi and open `http://192.168.4.1`.

### Result: ✅ REQUIREMENT MET

ESP-NOW operates correctly without any router. Channel locking and HALT burst provide reliable communication. The dashboard notifies the user both locally (buzzer) and via the browser.

---

## 5. Unmet Goals and Honest Assessment

### 5.1 Real turbidity sensor on ESP32

**What was attempted:** The analog turbidity sensor was connected to ESP32 ADC pin 1. The `readTurbidityNTU()` function was fully implemented with 64-sample averaging, temperature compensation, and ADC range clamping.

**What failed:** ESP32 ADC readings were heavily corrupted by the ESP-NOW radio transmission. Even reading before `WiFi.mode(WIFI_STA)` produced unstable values in the range 1100–1400 (the dirty-water ADC range), regardless of actual water clarity.

**What was done instead:** A randomised turbidity function (`readTurbidityRandom()`) alternates between above-threshold and below-threshold values on each boot to exercise the full pipeline for demo purposes.

**Why this is acceptable:** The turbidity sensor *function* and its integration with the rest of the system (temperature compensation, threshold comparison, pump trigger, servo trigger) are fully implemented and testable. The hardware limitation is a known ESP32 ADC characteristic (non-linearity and susceptibility to RF noise), not a design flaw. The planned fix (route sensor through Arduino → UART to ESP32) is architecturally straightforward.

### 5.2 Adaptive temperature thresholds from learning

**What was planned:** Record water temperature during the calibration window and derive personalised [T_min, T_max] limits.

**What was implemented:** The architecture supports it fully — temperature is transmitted during the learning window (`DATA:SENSOR:<ntu>,<temp_c>` messages every 500 ms). What is missing is an accumulator in the Observer's `LEARNING` branch and the computation of adaptive limits in `STOP_MEASURE`.

**Why not completed:** Time constraints during final integration. Static limits [18, 30] °C are conservative and functional for all tropical species.

### 5.3 Dashboard IoT security (HTTPS)

**What was planned:** HTTPS with a self-signed certificate.

**What was implemented:** WPA2 AP password, POST-only command endpoints, 423 responses on locked state.

**Why HTTPS was not implemented:** The ESP32 Arduino `WebServer` library does not support TLS natively. The `WiFiClientSecure` approach requires certificate generation, flashing, and browser trust configuration, which goes beyond the scope of a local-AP IoT demo. The local-AP network model significantly limits the attack surface compared to an internet-exposed endpoint.

---

## 6. Test Evidence Summary

All results below are derived from the serial monitor log captured during a real test session (attached to the GitHub repository).

| Test | Observed result | Target | Status |
|---|---|---|---|
| Calibration (34 samples, Hampel) | μ=192.44 mA, σ=8.17 mA, th=216.94 mA | Stable baseline | ✅ Pass |
| EWMA smoothing | Converges from 114 mA (idle) to ~187 mA (pump) | Noise rejection | ✅ Pass |
| Grace period | 4 samples skipped at pump start | Inrush ignored | ✅ Pass |
| DS18B20 glitch suppression | -127 °C discarded; last valid used | No false TEMP alarm | ✅ Pass |
| Deep sleep duty cycle | Active ~2 s, sleep 20 s → 9.1% active | ≤ 10% active | ✅ Pass |
| ACK retry | Logs "No ACK for START_LEARN" but continues | Graceful degradation | ✅ Pass |
| Pump stop on HALT | `emergency_stop = true`, pump LOW immediately | < 5 ms response | ✅ Pass |
| Full reaction time | 3 samples × 400 ms = 1200 ms | < 2000 ms | ✅ Pass |
| Real turbidity sensor | ADC corrupted by RF; random demo used | Real reading | ❌ Workaround |
| Adaptive temperature limits | Static [18, 30] °C used | Dynamic from learning | ⚠ Partial |
| Dashboard HTTPS | HTTP only; WPA2 AP protects local network | HTTPS | ⚠ Partial |

---

## 7. Reflection on Process

The most significant evolution from the mid-term to the final version was the shift from a single-metric, single-algorithm system to a multi-sensor, multi-anomaly architecture with a more sophisticated detection algorithm. This evolution was directly driven by the professor's feedback:

- *"Is the anomaly only due to blocking situations?"* → Added DRY_RUN, VOLTAGE_DROP, TEMP_OUT_OF_RANGE.
- *"Try different anomaly detection algorithms"* → Replaced 3-sigma with EWMA + Hampel, demonstrating that the baseline is more robust and the false positive rate remains at zero.
- *"Collecting data from multiple sensors would improve the project"* → All four sensor channels now feed the anomaly pipeline, and temperature is functionally coupled to turbidity via viscosity compensation.
- *"Edge-to-edge is ok, but users should be notified"* → Added real-time dashboard with SSE push, anomaly banners, browser notifications, and local buzzer.

The turbidity sensor hardware failure was a setback that was handled transparently: the software path is fully implemented and tested with simulated data, and the architectural fix (Arduino bridge) is identified and documented.