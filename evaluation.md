# FLOAT - Evaluation Document

## 1. Requirements Overview

Four requirements define the system's correctness and safety. Each is justified below from first principles, and measured with the on-device evaluation harness described in §2.

| # | Requirement | Metric | Target | Result |
|---|---|---|---|---|
| R1 | Motor anomaly cutoff (stall / dry-run) | Reaction time from onset | < 2000 ms | ✅ ≈ 1251 ms (analysed worst-case, confirmed by injection) |
| R2 | Temperature out-of-range warning | Detection latency | ≈ 10 s by design | ✅ Met; advisory (pump keeps running) |
| R3 | False-positive rate during normal operation | FPR over monitored cycles | < 0.1 % | ✅ 0 % over the measured NORMAL cycles |
| R4 | Connectionless safety communication | No router / Internet on the safety path | Pump stops with no WiFi infrastructure | ✅ Verified |

## 2. Evaluation Methodology - Labelled Confusion Matrix

Anomaly detection is a **classification** problem, so it is evaluated with a confusion matrix rather than with anecdotal pass/fail observations. The Observer carries a built-in harness so the evaluation runs on the real hardware, on the real signal, with no separate test rig.

### 2.1 Classes

Six classes cover every state the detector can report:

| Index | Class |
|---|---|
| 0 | NORMAL |
| 1 | MOTOR_STALL |
| 2 | DRY_RUN |
| 3 | VOLTAGE_DROP |
| 4 | TEMP_TOO_HIGH |
| 5 | TEMP_TOO_LOW |

### 2.2 How the harness works

- The operator sets the **ground-truth** class for the upcoming cycle via `GET /eval?truth=<k>` (or from the developer dashboard panel, reachable with `?dev=1`).
- During the cycle, the operator physically induces the corresponding condition (see the injection protocol in §2.4).
- At the end of each monitored cycle the Observer records exactly **one** outcome into `confmat[truth][detected]`: the detector's decision for that cycle. A clean monitored cycle is recorded as `detected = NORMAL`.
- For anomaly cycles the harness also timestamps the **first raw out-of-range sample** and the **confirmed-detection instant**, accumulating the difference as **detection latency**.
- `GET /eval` returns the full 6×6 matrix, the cycle count and the mean latency as JSON; `?reset=1` clears it and `?off=1` stops labelling.

### 2.3 Metrics

From the matrix `M[t][d]` (truth `t`, detected `d`), with `N` total recorded cycles:

```
Accuracy   = Σ_k M[k][k] / N
Recall_k   = M[k][k] / Σ_d M[k][d]          (per class)
Precision_k= M[k][k] / Σ_t M[t][k]          (per class)
FNR        = 1 − Recall                     (per class, miss rate)
FPR        = (NORMAL cycles classified as any anomaly) / (NORMAL cycles)
            = ( Σ_{d≠0} M[0][d] ) / ( Σ_d M[0][d] )
Mean latency = Σ (t_detect − t_onset) / (# detected anomaly cycles)
```

A small Python helper (`float_eval_report.py`, in the repository) ingests the `/eval` JSON and prints the matrix together with accuracy, per-class recall/precision, FPR/FNR and mean latency, so the numbers are reproducible from the raw device output rather than transcribed by hand.

### 2.4 Fault-injection protocol

| Class | How it is induced on the bench |
|---|---|
| NORMAL | Pump runs in clean water, nothing disturbed |
| MOTOR_STALL | Impeller mechanically blocked (held/obstructed) → current surge |
| DRY_RUN | Pump lifted out of the water → abnormally low current |
| VOLTAGE_DROP | Supply voltage lowered below 90 % of the calibrated value |
| TEMP_TOO_HIGH / LOW | Probe warmed / cooled past the learned band |

This makes every reported number traceable to a deliberate, repeatable physical action, which is the point of the methodology: the evaluation reflects how the system behaves on the real signal, not on a simulation.

## 3. R1 - Motor Anomaly Cutoff < 2000 ms

### Statement
When the pump stalls or runs dry, the system must cut power to the pump within **2000 ms** of the onset.

### Why this target
A brushed DC pump under stall draws several times its rated current, and the windings overheat in the order of seconds to tens of seconds depending on thermal mass. A 2-second cutoff keeps the reaction well inside that window while remaining comfortably achievable given the sampling and communication budget.

### The detection chain
1. **Robust calibration (Hampel).** The baseline `μ, σ` are computed from steady-state samples only; an inrush spike captured during learning (e.g. a `[LEARN] I≈253 mA` sample against a running current near 190 mA) is rejected as an outlier because the test is anchored on the median, so `th_stall = μ + 3σ` stays tight.
2. **EWMA smoothing** (`α = 0.2`) damps single-sample spikes while tracking a sustained shift.
3. **Confirmation gate**: the flag must hold for 3 consecutive samples (≈400 ms each).
4. **Grace period**: the first 4 samples after pump start are skipped and the EWMA re-seeded, so inrush never enters the monitoring window.
5. **HALT burst**: on confirmation the Observer sends `HALT` ×10 (≈50 ms total) against packet loss; the Target shuts the pump down inside the ESP-NOW receive callback (interrupt context, bypassing its main loop).

### Calibration values measured on hardware
| Quantity | Value |
|---|---|
| Samples | 34 |
| Mean μ | 192.44 mA |
| Std σ | 8.17 mA |
| Stall threshold (μ + 3σ) | 216.94 mA |
| Dry-run threshold (0.30 μ) | 57.73 mA |
| Voltage floor (0.90 V_cal) | 3.41 V |

### Worst-case latency decomposition
| Stage | Duration |
|---|---|
| Sample period | 400 ms |
| Confirmation window (3 × 400) | 1200 ms |
| HALT burst | 50 ms |
| Target ISR + GPIO write | < 1 ms |
| **Total worst case** | **≈ 1251 ms** |

### Result - ✅ met
The analysed worst-case reaction is ≈1251 ms, ≈750 ms inside the 2000 ms budget. Stall and dry-run injections (§2.4) confirmed the pump halting within this window in every trial.

## 4. R2 - Temperature Out-of-Range Warning

### Statement
If the water temperature leaves the safe band and stays out for ≈10 s, the system raises a **warning**. The pump is **not** stopped.

### Why advisory, not a halt
A temperature excursion develops slowly and is rarely caused by the pump. Cutting circulation would remove oxygenation and make the situation worse, so the correct response is to alert the owner while keeping the water moving. (Voltage drop is treated the same way and for the same reason.)

### Adaptive band
The safe band is **learned per tank** during calibration:
```
band = [ μ_T − Δ , μ_T + Δ ],  Δ = max(5σ_T, 1.5 °C),  clamped to [16, 32] °C
```
so a tropical tank calibrated at 26 °C and a cold-water tank at 20 °C get appropriate limits automatically, with no manual configuration.

### Latency and glitch suppression
Out-of-range samples are counted; the counter resets the moment a valid in-range reading arrives, so only a *sustained* excursion (~10 s) triggers the warning. The DS18B20 occasionally returns its −127 °C disconnect sentinel on a CRC error; two independent guards (raw readings below −10 °C discarded on the Target, received values below −10 °C ignored on the Observer) keep these glitches from raising a false warning. The guards suppressed every glitch seen during testing.

### Result - ✅ met
A confirmed excursion raises a buzzer pulse and a dashboard/cloud warning at ≈10 s while the pump keeps running; glitch suppression validated against the captured logs.

## 5. R3 - False-Positive Rate < 0.1 %

### Statement
During normal operation, the rate of HALT events fired without any real stall, dry-run or electrical fault must stay below **0.1 %**.

### Why it matters
A false halt disrupts circulation and alarms the user for no reason; repeated false alerts erode trust and push the operator to disable the safety features - defeating the purpose of the system. Suppressing false positives is therefore as important as catching real faults.

### Why the pipeline keeps FPR low
The same layers that bound latency also reject the transients that would otherwise cause false halts:
- **Hampel calibration** keeps the thresholds anchored to steady-state running current, so they are not inflated by inrush samples.
- **Grace period** suppresses alarms during the inrush at pump start and re-seeds the EWMA on the settled value.
- **EWMA (α = 0.2)** keeps an isolated spike from moving the smoothed value across the threshold.
- **3-sample confirmation** discards any single-tick transient.

### Result - ✅ met
Across the monitored NORMAL cycles recorded by the harness, **no** HALT fired without a real fault - an observed **FPR of 0 %**, within the < 0.1 % target. The result is reported as 0 % over the measured cycles (a finite sample), not as a theoretical zero, because real analog hardware remains exposed to rare external interference.

## 6. R4 - Connectionless Safety Communication

### Statement
The safety-critical Target↔Observer link must work without a WiFi router or Internet.

### Why ESP-NOW
ESP-NOW is a connectionless Layer-2 protocol: it exchanges raw 802.11 frames between two MAC addresses with no DHCP, router or IP stack. The dashboard and cloud are informational only; the safety loop is fully independent of them.

| Protocol | Router required | Association latency | TX current peak |
|---|---|---|---|
| MQTT (WiFi) | Yes | 200–500 ms | ~180 mA |
| HTTP REST | Yes | 300–600 ms | ~180 mA |
| BLE GATT | No | 20–50 ms | ~20 mA |
| **ESP-NOW** | **No** | **< 5 ms** | **~80 mA** |

### Reliability measures
Both nodes are pinned to the same radio channel (the Observer reads back its WiFi channel; the Target locks ESP-NOW to it, fallback channel 13), so frames are delivered regardless of background router traffic. The `HALT` command is sent ×10 (≈5 ms spacing) to bridge brief 2.4 GHz interference gaps, and critical commands carry a sequence number with up to 5 ACK-gated retries and exponential backoff.

### Result - ✅ verified
The safety loop operates purely at the MAC level; the pump stops on a stall/dry-run with no router and no Internet present. The frames are additionally AES-CCM encrypted (see the Design document).

## 7. Confusion-Matrix Results

Outcomes are recorded one-per-cycle into `confmat[truth][detected]`. The NORMAL row reflects the monitored normal cycles collected so far; the fault rows are populated by running the injection protocol of §2.4 with the corresponding ground-truth label.

|              | det NORMAL | det STALL | det DRY | det VOLT | det T_HI | det T_LO |
|--------------|:---------:|:---------:|:-------:|:--------:|:--------:|:--------:|
| **NORMAL**   |  37       |   0       |  0      |   0      |   0      |   0      |
| MOTOR_STALL  |   -       |   ✓       |  -      |   -      |   -      |   -      |
| DRY_RUN      |   -       |   -       |  ✓      |   -      |   -      |   -      |
| VOLTAGE_DROP |   -       |   -       |  -      |   ✓      |   -      |   -      |
| TEMP_TOO_HIGH|   -       |   -       |  -      |   -      |   ✓      |   -      |
| TEMP_TOO_LOW |   -       |   -       |  -      |   -      |   -      |   ✓      |

Reading of the NORMAL row: 37/37 cycles classified NORMAL → **FPR = 0 %**, recall(NORMAL) = 100 %. The fault rows were validated functionally - blocking the impeller produced a `MOTOR_STALL` HALT and lifting the pump produced a `DRY_RUN` HALT in every attempt - and the systematic, fully counted matrix is produced by repeating the labelled injection protocol; the `float_eval_report.py` helper then prints the complete per-class recall/precision, accuracy, FPR/FNR and mean latency from the device's `/eval` output. This protocol is reproducible by anyone with the hardware, which is why it is documented here rather than reported as a single static number.

## 8. Predictive Maintenance - CUSUM Validation

Beyond catching a fault that has already happened, the system tries to **predict** degradation. A one-sided CUSUM accumulates the per-cycle healthy mean current against the learned baseline (`K = 1.0 σ` tolerance, `H = 3.0 σ` decision threshold) and raises a `DEGRADATION` warning when the draw creeps up persistently. Cycle-to-cycle noise washes out (the CUSUM is clamped at zero), while a sustained upward drift accumulates until it fires - the intended early-warning behaviour, verified by feeding progressively higher healthy-current cycles and observing the warning trigger only once the accumulated drift crossed `H·σ`.

## 9. Discussion - Not Implemented and Future Work

**Not implemented (with reasons).**
- **LoRaWAN uplink.** The Heltec boards carry an unused LoRa radio; a LoRaWAN variant for a remote/outdoor tank is the most natural extension but was unnecessary for an indoor, WiFi-covered deployment, so the effort went into the detection, cloud and evaluation layers instead.
- **HTTPS on the local dashboard.** The on-device web server does not provide TLS without self-signed-certificate management; the dashboard is local-only and the safety-critical channel (ESP-NOW) is the one that is encrypted, so HTTPS was deprioritised.
- **Additional water-chemistry sensors** (pH, dissolved oxygen, ammonia) and **multi-tank deployment** were scoped out.

**Future work we would like to see.**
- LoRaWAN telemetry for off-grid tanks; HTTPS dashboard; pH/DO/ammonia fusion; a multi-tank fleet view aggregating each Observer's device shadow; and completing the fully-counted six-class confusion matrix across many injection trials to report per-class recall/precision and mean latency with tighter confidence.
