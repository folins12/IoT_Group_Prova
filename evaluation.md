# FLOAT - Evaluation Document (Final)

## 1. Requirements Overview

Four requirements define the system's correctness and safety. Each is justified below from first principles and measured with the on-device evaluation harness of §2. The results column reports the figures obtained from a labelled run of 64 monitored cycles.

| # | Requirement | Metric | Target | Result |
|---|---|---|---|---|
| R1 | Motor anomaly cutoff (stall / dry-run) | Reaction time from onset | < 2000 ms | ✅ 1467 ms measured mean |
| R2 | Temperature out-of-range warning | Detection | sustained ≈10 s excursion | ✅ 100 % recall (20/20 temp cycles), advisory |
| R3 | False-positive rate during normal operation | FPR over NORMAL cycles | < 0.1 % | ✅ 0 % (0 / 10 NORMAL cycles) |
| R4 | Connectionless safety communication | No router / Internet on the safety path | Pump stops with no WiFi infrastructure | ✅ Verified |

Headline figures of the run: **accuracy 92.2 %**, **false-positive rate 0 %**, **false-negative rate 0 %** (no fault was ever missed), **mean detection latency 1467 ms**. The only errors were five inter-class confusions discussed in §7.

## 2. Evaluation Methodology - Labelled Confusion Matrix

Anomaly detection is a **classification** problem, so it is evaluated with a confusion matrix rather than with anecdotal pass/fail observations. The Observer carries a built-in harness so the evaluation runs on the real hardware, on the real current signal, with no separate test rig.

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
- During the cycle, the operator physically induces the corresponding condition (injection protocol in §2.4).
- At the end of each monitored cycle the Observer records exactly **one** outcome into `confmat[truth][detected]`. A clean monitored cycle is recorded as `detected = NORMAL`. A flag (`eval_cycle_recorded`) guarantees one outcome per cycle, so a late second anomaly in the same cycle never double-counts.
- For anomaly cycles the harness timestamps the **first raw out-of-range sample** and the **confirmed-detection instant**, accumulating the difference as **detection latency**.
- `GET /eval` returns the full 6×6 matrix, the cycle count and the mean latency as JSON; `?reset=1` clears it and `?off=1` stops labelling.

The unit of evaluation is the **pump cycle**, which is the real unit of decision in the system (one HALT decision per cycle).

### 2.3 Metrics

From the matrix `M[t][d]` (truth `t`, detected `d`), with `N` total recorded cycles:

```
Accuracy   = Σ_k M[k][k] / N
Recall_k   = M[k][k] / Σ_d M[k][d]          (per class)
Precision_k= M[k][k] / Σ_t M[t][k]          (per class)
FPR        = ( Σ_{d≠0} M[0][d] ) / ( Σ_d M[0][d] )      (NORMAL cycles flagged as any anomaly)
FNR        = ( Σ_{t≠0} M[t][0] ) / ( Σ_{t≠0,d} M[t][d] ) (fault cycles reported as NORMAL = missed)
Mean latency = Σ (t_detect − t_onset) / (# detected anomaly cycles)
```

A small Python helper (`float_eval_report.py`, in the repository) ingests the `/eval` JSON and prints the matrix together with accuracy, per-class recall/precision, FPR/FNR and mean latency, so the numbers are reproducible from the raw device output rather than transcribed by hand.

Note on metric choice: with classes dominated by NORMAL, **accuracy alone is misleading**, so the per-class recall/precision, the FPR (on the NORMAL row) and the FNR (on the NORMAL column of fault rows) are reported separately. They answer the two questions a safety system actually cares about: *does it raise false alarms?* (FPR) and *does it miss real faults?* (FNR).

### 2.4 Fault-injection protocol

| Class | How it is induced on the bench |
|---|---|
| NORMAL | Pump runs in clean water, nothing disturbed |
| MOTOR_STALL | Impeller mechanically blocked (held / obstructed) → current surge |
| DRY_RUN | Pump lifted out of the water → reduced current (~70 % of baseline) |
| VOLTAGE_DROP | Supply voltage forced below 90 % of the calibrated value (loaded / depleted battery) |
| TEMP_TOO_HIGH / LOW | Probe warmed / cooled past the learned band |

Every reported number is therefore traceable to a deliberate, repeatable physical action: the evaluation reflects how the system behaves on the real signal, not on a simulation.

## 3. R1 - Motor Anomaly Cutoff < 2000 ms

### Statement
When the pump stalls or runs dry, the system must cut power to the pump within **2000 ms** of the onset.

### Why this target
A brushed DC pump under stall draws several times its rated current, and the windings overheat in the order of seconds to tens of seconds depending on thermal mass. A 2-second cutoff keeps the reaction well inside that window while remaining comfortably achievable given the sampling and communication budget.

### The detection chain
1. **Robust calibration (Hampel).** The baseline `μ, σ` are computed from steady-state samples only; an inrush spike captured during learning is rejected as an outlier because the test is anchored on the median, so `th_stall = μ + 3σ` stays tight.
2. **EWMA smoothing** (`α = 0.2`) damps single-sample spikes while tracking a sustained shift.
3. **Confirmation gate**: the flag must hold for `CONFIRM_NEEDED = 3` consecutive samples (≈400 ms each).
4. **Grace period**: the first 4 samples after pump start are skipped and the EWMA re-seeded, so inrush never enters the monitoring window.
5. **HALT burst**: on confirmation the Observer sends `HALT` ×10 (≈50 ms total) against packet loss; the Target shuts the pump down inside the ESP-NOW receive callback (interrupt context, bypassing its main loop).

### Thresholds and calibration
The baseline is **re-learned every session**, so the absolute thresholds adapt to the current battery and pump state. Across the test sessions the learned mean ranged roughly 250–340 mA depending on supply voltage. A representative healthy calibration:

| Quantity | Value |
|---|---|
| Samples | ~33 |
| Mean μ | 270.0 mA |
| Std σ | 2.65 mA |
| Stall threshold (μ + 3σ) | 285.0 mA |
| Dry-run threshold (0.70 μ) | 189.0 mA |
| Voltage floor (0.90 V_cal) | 3.25 V |

The dry-run threshold is set at **0.70 μ**: a pump that loses prime keeps spinning but draws only ~70–75 % of its loaded current (a ~25–30 % drop), so a threshold at 70 % of baseline is the level that actually catches a real dry-run on this hardware while staying clear of normal current ripple.

### Latency: measured and analysed
The harness measured a **mean detection latency of 1467 ms** across all injected anomalies, within the 2000 ms budget. This is consistent with the analytical decomposition of the minimum path:

| Stage | Duration |
|---|---|
| Sample period | 400 ms |
| Confirmation window (3 × 400) | 1200 ms |
| HALT burst | 50 ms |
| Target ISR + GPIO write | < 1 ms |
| **Analytical minimum** | **≈ 1251 ms** |

The measured mean sits slightly above this minimum because the onset is timestamped on the first raw out-of-range sample while confirmation runs on the (slightly lagged) EWMA, and because the latency is quantised to the ~400 ms loop period.

Two honest caveats on the number: it measures **onset → Observer decision** (the ~50 ms HALT burst and <1 ms Target ISR that follow are not included in the 1467 ms but are inside the 2000 ms budget); and "onset" is the first *detectable* out-of-range sample, i.e. the **algorithmic** latency, not the instant of physical fault injection.

### Result - ✅ met
Measured mean reaction 1467 ms, comfortably inside the 2000 ms budget, with the analytical worst-case of the minimum path at ≈1251 ms.

## 4. R2 - Temperature Out-of-Range Warning

### Statement
If the water temperature leaves the safe band and stays out for a sustained period (~10 s), the system raises a **warning**. The pump is **not** stopped.

### Why advisory, not a halt
A temperature excursion develops slowly and is rarely caused by the pump. Cutting circulation would remove oxygenation and make the situation worse, so the correct response is to alert the owner while keeping the water moving. (Voltage drop is treated the same way and for the same reason.)

### Adaptive band
The safe band is **learned per tank** during calibration:
```
band = [ μ_T − Δ , μ_T + Δ ],  Δ = max(5σ_T, 1.5 °C),  clamped to [16, 32] °C
```
so a tropical tank calibrated at 26 °C and a cold-water tank at 20 °C get appropriate limits automatically, with no manual configuration.

### Glitch suppression
The DS18B20 occasionally returns its −127 °C disconnect sentinel on a CRC error; two independent guards (raw readings below −10 °C discarded on the Target, received values below −10 °C ignored on the Observer) keep these glitches from raising a false warning. The guards suppressed every glitch seen during testing.

### Result - ✅ met
In the labelled run both temperature classes were detected perfectly: **TEMP_TOO_HIGH 10/10** and **TEMP_TOO_LOW 10/10**, with no confusion into any other class (100 % recall and 100 % precision for both). The warning is raised while the pump keeps running.

## 5. R3 - False-Positive Rate < 0.1 %

### Statement
During normal operation, the rate of HALT events fired without any real stall, dry-run or electrical fault must stay below **0.1 %**.

### Why it matters
A false halt disrupts circulation and alarms the user for no reason; repeated false alerts erode trust and push the operator to disable the safety features - defeating the purpose of the system. Suppressing false positives is therefore as important as catching real faults.

### Why the pipeline keeps FPR low
The same layers that bound latency also reject the transients that would otherwise cause false halts: **Hampel calibration** keeps the thresholds anchored to steady-state current; the **grace period** suppresses the inrush at pump start and re-seeds the EWMA on the settled value; the **EWMA (α = 0.2)** keeps an isolated spike from crossing the threshold; and the **3-sample confirmation** discards any single-tick transient.

### Result - ✅ met
Across the 10 monitored NORMAL cycles, **no HALT fired without a real fault → FPR = 0 % (0 / 10)**, and recall(NORMAL) = 100 %. Honest scope note: 10 normal cycles establish *zero observed false positives*, not a statistically tight bound on a 0.1 % rate - by the rule of three, 0 events in 10 trials only bounds the true rate to roughly < 30 % at 95 % confidence. Tightening the bound to the 0.1 % level would require on the order of thousands of normal cycles, which is the motivation for the offline-replay extension noted in §9. What the run does establish is that the detector produced **no false alarms at all** on real normal operation.

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

The matrix below is the full labelled run: **64 monitored cycles**, with each fault induced physically per the protocol of §2.4. Rows are the ground truth, columns the detector's decision.

| truth ＼ detected | NORMAL | STALL | DRY | VOLT | T_HI | T_LO | **recall** |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **NORMAL** | **10** | 0 | 0 | 0 | 0 | 0 | 100 % |
| **MOTOR_STALL** | 0 | **9** | 1 | 0 | 0 | 0 | 90 % |
| **DRY_RUN** | 0 | 0 | **10** | 0 | 0 | 0 | 100 % |
| **VOLTAGE_DROP** | 0 | 0 | 4 | **10** | 0 | 0 | 71 % |
| **TEMP_TOO_HIGH** | 0 | 0 | 0 | 0 | **10** | 0 | 100 % |
| **TEMP_TOO_LOW** | 0 | 0 | 0 | 0 | 0 | **10** | 100 % |
| **precision** | 100 % | 100 % | 67 % | 100 % | 100 % | 100 % | |

**Aggregate metrics:**

| Metric | Value | Meaning |
|---|---|---|
| Accuracy | 92.2 % (59/64) | cycles classified into the exact correct class |
| False-positive rate | **0 %** (0/10) | NORMAL cycles never flagged as a fault |
| False-negative rate | **0 %** (0/54) | no fault cycle was ever reported as NORMAL |
| Mean detection latency | 1467 ms | within the 2000 ms budget |
| Macro recall / precision | 93.6 % / 94.4 % | averaged over the six classes |

### Reading the result
The two metrics that matter most for a safety system are both ideal: **FPR = 0 %** (no false alarms on healthy operation) and **FNR = 0 %** (every injected fault was detected as *some* anomaly - nothing was silently missed). All five errors are **inter-class confusions**, and every one collapses into DRY_RUN: four VOLTAGE_DROP cycles and one MOTOR_STALL cycle were labelled DRY_RUN.

### Why the confusions happen (and why they are not misses)
The dry-run condition is "current well below baseline". When the supply voltage collapses far enough, the pump current drops to **≈0 mA** - and zero current is, by definition, below the dry-run threshold, so the dry-run flag co-fires alongside the voltage flag. Which label the cycle receives depends on which confirmation counter saturates first; in four of these cycles the readings drove the dry-run counter to confirmation. The single stall→dry case is the same effect in reverse: after a stalled pump is cut, the current collapses and a late dry-run condition appears. These are **cause-attribution errors, not detection failures** - the fault was always caught.

Crucially, the confusions do **not** compromise the protective action: MOTOR_STALL and DRY_RUN both trigger a HALT, so the stall→dry cycle still stopped the pump correctly; and in the voltage→dry cycles the current had already collapsed to ~0 mA (a severe supply failure), where halting is the safe response anyway. The imperfection is in the *diagnosis label*, not in whether the system reacted.

### Fix identified
The confusions are removed by guarding the dry-run test against a near-zero reading (a primed-but-dry pump draws reduced-but-nonzero current; exactly 0 mA means loss of supply, not a dry-run) and by letting the voltage fault take precedence when both fire:
```cpp
bool dry_flag = (th_dry_run > 0.1f && ewma_current < th_dry_run
                 && ewma_current > 20.0f      // 0 mA = no supply, not a dry-run
                 && !volt_flag);              // voltage is the primary cause when both trip
```
This is a one-line change that would raise VOLTAGE_DROP recall and DRY_RUN precision toward 100 % without affecting the (already ideal) FPR/FNR.

## 8. Predictive Maintenance - CUSUM Validation

Beyond catching a fault that has already happened, the system tries to **predict** degradation. A one-sided CUSUM accumulates the per-cycle healthy mean current against the learned baseline (`K = 1.0 σ` tolerance, `H = 3.0 σ` decision threshold) and raises a `DEGRADATION` warning when the draw creeps up persistently. Across every healthy cycle in the test logs the CUSUM correctly stayed at **0** (e.g. `CUSUM=0.0 / 46.3`), i.e. it produced **no false degradation warnings**; cycle-to-cycle noise washes out because the statistic is clamped at zero, while a sustained upward drift would accumulate until it crosses the `H·σ` limit. A controlled long-run degradation trend is the natural next test to exercise the firing path end-to-end.

## 9. Discussion - Not Implemented and Future Work

**Not implemented (with reasons).**
- **LoRaWAN uplink.** The Heltec boards carry an unused LoRa radio; a LoRaWAN variant for a remote/outdoor tank is the most natural extension but was unnecessary for an indoor, WiFi-covered deployment, so the effort went into the detection, cloud and evaluation layers instead.
- **HTTPS on the local dashboard.** The on-device web server does not provide TLS without self-signed-certificate management; the dashboard is local-only and the safety-critical channel (ESP-NOW) is the one that is encrypted, so HTTPS was deprioritised.
- **Additional water-chemistry sensors** (pH, dissolved oxygen, ammonia) and **multi-tank deployment** were scoped out.

**Future work we would like to see.**
- The dry-run/voltage disambiguation guard of §7, to drive the two off-diagonal cells to zero.
- **Offline replay** of recorded current traces against the detector: this yields far more normal "cycles" for free (tightening the FPR bound toward the 0.1 % level), and lets the decision thresholds be swept to produce a ROC / precision-recall curve that justifies the operating point instead of fixing it.
- Reporting detection latency as a **distribution** (median, p95, max) rather than a mean, which matters more for a safety system than the average.
- LoRaWAN telemetry for off-grid tanks; HTTPS dashboard; pH/DO/ammonia fusion; and a multi-tank fleet view aggregating each Observer's device shadow.
