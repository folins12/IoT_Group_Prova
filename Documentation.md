# FLOAT Aquarium — Technical Documentation

A self-contained, low-power **predictive-maintenance** system for a small aquarium.
It does the routine work (water circulation + fish feeding) **and**, while it works,
watches the pump's electrical signature to catch faults *before* they become failures.
It runs fully on a local network, and mirrors its status + alarms to the cloud so the
owner can be warned and react when away from home.

---

## 1. System architecture

The system is built from **three microcontrollers**, each with one clear job, plus a
local web dashboard and an optional cloud path.

```
                          ESP-NOW (encrypted, AES-CCM)
   ┌──────────────┐  <───────────────────────────────>  ┌────────────────┐
   │  ARDUINO UNO │       UART ('T'/'E')                 │  OBSERVER ESP32 │
   │  turbidity   │ <───────────────────  ┌───────────┐  │  INA219 current │
   │  sensor      │                        │  TARGET   │  │  + buzzer       │
   └──────────────┘                        │  ESP32-S3 │  │  + WiFi/HTTP    │
                                           │  pump,    │  │  + MQTT + email │
                                           │  servo,   │  └───────┬────────┘
                                           │  DS18B20  │          │ WiFi (STA)
                                           └───────────┘          │
                                                          ┌───────┴────────┐
                                                          │ Local dashboard │  (phone/PC on the hotspot)
                                                          │ HiveMQ Cloud    │  (MQTT, TLS)
                                                          │ Email alerts    │  (Apps Script)
                                                          └─────────────────┘
```

* **Target (ESP32-S3)** — the *actuator + sensor* node. Reads turbidity (through the
  Arduino Uno) and water temperature (DS18B20), drives the **pump** and the **food
  servo**. It runs the operating cycle and spends most of its time in **deep sleep**.
* **Observer (ESP32)** — the *brain + gateway* node. Always on. Measures the Target's
  **supply current** with an INA219, runs the anomaly-detection maths, serves the local
  **dashboard**, and bridges everything to the cloud (**MQTT** + **email**).
* **Arduino Uno** — a tiny dedicated front-end for the analog turbidity probe. It sleeps
  on its watchdog and answers the Target's `'T'` request with a raw ADC value.

The two ESP32s talk over **ESP-NOW** (a connectionless, low-latency Espressif radio
protocol) with **AES-CCM encryption**. They also both join the same WiFi network so the
Observer can host the dashboard and reach the internet.

### What the user can do

**At home (local network):** open the dashboard at `http://<observer-ip>/` (e.g.
`http://172.20.10.5`). It shows:
* live **temperature, current and turbidity** cards;
* a live **pump-current chart** with the learned **stall** and **dry-run** threshold lines;
* a **Default mode** switch (ON = the system runs its cycle autonomously; OFF = manual bench);
* **manual controls** (only in OFF): run the pump for a chosen time, dispense food once or
  on a repeating interval, **Stop feeding**, and re-calibrate;
* a **live event log** and pop-up **toasts** when an anomaly fires;
* a **Clear HALT** button to recover after a stall/dry-run lockout.

**Away from home (cloud):** the owner does not keep the dashboard open. Instead:
* an **email** arrives automatically on every anomaly (stall, dry-run, voltage, temperature,
  degradation);
* from a phone (the *IoT MQTT Panel* app) or the **HiveMQ web client** they can send a few
  commands to recover the system — clear the alarm and restart the default cycle.

This is a deliberate **two-tier design**: a rich, low-latency HMI on the LAN, and a lean,
notification-first path for remote use. See §4.10.

---

## 2. The three source files

### `target.cpp` — Target node firmware
Responsible for sensing and actuation, and for the autonomous cycle.
Structure:
* **State**: NVS-backed (`halted`, `auto`, `storm`, `pumping`) + RTC-backed (`bootCount`, …)
  so the right things survive deep sleep *and* power loss.
* **`OnDataRecv`**: the ESP-NOW command parser — `HALT`, `CLEAR_HALT`, `AUTO_ON/OFF/RESET/KEEP`,
  `PUMP_ON[:sec]`, `PUMP_OFF`, `FEED[:N]`, `FEED_STOP`, `CALIBRATE`, plus `ACK`/`CAL` replies.
* **Sensing**: `readTurbidityFromArduino` (UART handshake) and `safeTemp` (DS18B20 with a
  disconnect guard).
* **Radio**: `findChannel` (scan for the hotspot, pin ESP-NOW to its channel), `sendMsg`
  (fire-and-forget) and `sendMsgWithACK` (retry + acknowledge for critical commands).
* **`setup()`**: the whole operating cycle (it runs once per wake, then sleeps).
* **`loop()`**: only runs in Default-mode OFF (the manual bench: pump timer, food scheduler,
  calibration, heartbeat).

### `observer.cpp` — Observer node firmware
Responsible for measurement, detection, the dashboard and the cloud.
Structure:
* **Config block**: WiFi credentials, MQTT (HiveMQ Cloud, TLS), email webhook.
* **Detection state**: calibration thresholds, EWMA, confirmation counters, CUSUM, the
  evaluation harness (confusion matrix).
* **`OnDataRecv`**: handles Target messages (`LOG`, `DATA:SENSOR`, mode transitions,
  `START_LEARN/MONITOR`, `STOP_MEASURE`), runs calibration on `STOP_MEASURE`, and echoes a
  pending command / `HALT` back to the Target.
* **MQTT** (`mqttTask`, `mqttPublish*`, `mqttCallback`) and **email** (`sendEmailAlert`).
* **HTTP handlers** (`handleRoot/Data/Events/Mode/Cmd/Eval`) — the dashboard API.
* **`loop()`**: read the INA219, update EWMA, and — when MONITORING — run the anomaly checks.

### `dashboard.h` — the local web UI
A single self-contained HTML/CSS/JS page stored as a C++ raw string and served by the
Observer. It polls `/data` (≈0.6 s) and `/events` (≈1.5 s), renders the chart/cards/toasts,
and sends commands to `/cmd`, `/mode` and `/eval`. The **evaluation panel is developer-only**
and appears only when the URL contains `?dev` (e.g. `http://172.20.10.5/?dev=1`).

---

## 3. Core logic and **why** it is built this way

### 3.1 Why two ESP32 nodes
The node that *causes* the current (the Target, switching the pump) should not be the same
node that *measures* it, and the measuring node must be **always awake** to catch a fault the
instant it happens. The Target therefore sleeps to save power, while the Observer stays on,
samples current continuously during a pump run, and can issue an immediate `HALT`. Splitting
the roles also keeps each firmware small and testable.

### 3.2 Why duty-cycled deep sleep
Continuous pumping is unnecessary and power-hungry. The Target wakes, acts (~2 s), and sleeps
20 s → roughly a **9 % duty cycle**. Deep sleep wipes RAM, which is *exploited* on purpose:
critical state lives in RTC memory (survives sleep) and NVS/flash (survives everything), so a
wake is a clean, predictable restart.

### 3.3 The sensing pipeline
* **Turbidity** is read by a dedicated **Arduino Uno** and sent over UART. It is read **before**
  the WiFi radio is switched on, because the analog probe is sensitive to RF noise from the
  ESP32. A timeout falls back to *0 NTU* (treated as a safe condition) so a missing sensor never
  stalls the cycle.
* **Temperature** (DS18B20) is validated against the −127 °C "disconnected" sentinel; a bad
  read reuses the last valid value rather than poisoning the logic.

### 3.4 Calibration: EWMA + Hampel
On the very first boot the pump runs for ~10 s while the Observer collects current samples and
computes a **baseline** with a **Hampel filter** (median + MAD). Hampel is used instead of a
plain mean/σ because the **inrush spikes** of a DC motor are exactly the kind of outliers that
would otherwise inflate the baseline and hide real faults — Hampel rejects them robustly.
From the clean baseline the Observer derives:
* **stall threshold** = `μ + 3σ` (with a 15 mA floor),
* **dry-run threshold** = `30 % of μ`,
* **min voltage** = `90 % of the calibration voltage`,
* **temperature alarm band** = `μ_T ± max(5σ_T, 1.5 °C)` clamped to absolute backstops.

During monitoring the live current is smoothed with an **EWMA (α = 0.2)** so that single noisy
samples do not trip an alarm, while a genuine sustained shift is tracked quickly.

### 3.5 Anomaly detection and the halt vs. warning policy
Each tick computes four flags (stall / dry / under-voltage / out-of-temp). A flag must persist
for **3 consecutive ticks** (`CONFIRM_NEEDED`) before it is *confirmed* — this is the key
debounce that turns a noisy signal into a reliable decision. A **grace period** of 4 ticks at
the start of every pump run absorbs the motor inrush (the EWMA is re-seeded afterwards).

Confirmed anomalies are split by severity, on purpose:
* **MOTOR_STALL** and **DRY_RUN → immediate HALT.** A blocked impeller or a pump running dry can
  burn the motor; the system stops the pump (a 10× `HALT` burst for reliability) and latches.
* **VOLTAGE_DROP, TEMP_TOO_HIGH/LOW → warning only.** A momentary voltage dip should not leave
  the tank unfiltered, and an out-of-range temperature is a problem with the heater/water, not
  the pump — the operator must act, but stopping the pump would not help. These raise the buzzer,
  a toast and an email, but the pump keeps running.

### 3.6 Predictive degradation (CUSUM)
A single `μ + 3σ` test is reactive — it only fires once a fault is already present. To get
*ahead* of failure, each monitored cycle's mean **healthy** current (stall/dry samples excluded)
is fed into a one-sided **CUSUM**. A slow upward creep (impeller fouling, bearing friction)
accumulates and, past a `3σ` limit, raises a **DEGRADATION** warning — a "service the pump soon"
heads-up that a single-sample threshold cannot anticipate. Tolerance and limit are expressed in
σ so they scale with the pump's own noise.

### 3.7 Safety: the NVS latch and the reset-storm filter
The hardest failure mode is a stall whose own current sag **resets the board** before the `HALT`
lands. Two mechanisms close that hole:
* **NVS safety latch** — `halted=true` is written to flash **before** the pump is ever energised,
  and cleared **only** if the cycle completes with no anomaly. So even if the Target browns out
  mid-stall, it wakes up still halted and will not re-energise a blocked pump.
* **Reset-storm filter** — an NVS `pumping` marker is set only while the pump drives load. At
  boot, a reset is counted toward a safety halt **only if it happened while pumping**; idle/sleep
  brownouts are ignored. This was the fix for the earlier "phantom halt / learning never starts"
  symptom: weak-supply idle glitches no longer accumulate into a forced halt, while genuine
  pump-load reset loops (>5) still halt and report the cause (`reset loop under pump load`).

> Note: the *root* cause of those resets was an under-sized supply. It was fixed in hardware with
> bulk capacitors (≈400 µF electrolytic + a 100 nF ceramic across the rails on the load side of
> the INA219). The firmware filter remains as a safety backstop.

### 3.8 ESP-NOW security (AES-CCM)
Every Target↔Observer frame is authenticated and encrypted with **AES-CCM** (a shared PMK and a
per-peer LMK, identical on both nodes). Without it, anyone nearby could inject a spoofed `HALT`
or `PUMP_ON` over the air. The keys are 16 bytes each and **must match byte-for-byte** in both
files; they should be changed before any real deployment.

### 3.9 WiFi channel discovery
ESP-NOW must run on the same radio channel as the WiFi link. Rather than hard-coding a channel,
both nodes **scan for the hotspot SSID** and pin their radio to whatever channel the router/phone
chose (peer channel `0` = "use the current channel"), so the system works on any network and
survives the hotspot hopping channels. If the SSID is not found, both fall back to channel 13.

### 3.10 Cloud bridge: MQTT shadow + alerts
The Observer (the always-on node) is also the MQTT client. It publishes to **HiveMQ Cloud**
(free, private, **authenticated**, **TLS**) under `float/aq1/...`:
* `/telemetry` — periodic current/voltage/temperature/turbidity;
* `/state` — a **retained** device-shadow snapshot (mode, halted, calibration, thresholds), so a
  subscriber that connects later immediately sees the last known state;
* `/alert` — one message per anomaly (a broker rule can route it to a notification);
* `/cmd` — **subscribed**: remote "desired" commands. Incoming commands reuse the *exact same*
  handlers as the dashboard (`applyMode`, `applyClearHalt`, `applyManualCmd`) — a single source of
  truth for local and remote control. The keepalive is non-blocking, so an unreachable broker
  never stalls the dashboard or the radio.

### 3.11 Email alerts
On every anomaly the Observer also fires a single HTTPS GET to a free **Google Apps Script** web
app, which sends an email. To stay within the plain ESP32's RAM, the MQTT TLS session is briefly
dropped during the call (the alert has already been published) and reconnects on the next loop.

### 3.12 Two-tier remote control (the design choice)
**At home** the user has the full dashboard. **Away**, the essential remote workflow is just:
*email arrives → clear the alarm → restart the default cycle.* That maps to three commands —
**`clear_halt`, `mode_off`, `mode_on`** — which are the only ones that should be used *blind*
(without eyes on the tank). The manual **start** actions (pump-on, feed, calibrate) are kept on
the **local** dashboard only: starting an actuator you cannot see risks overfeeding or running the
pump dry, and they are gated to OFF mode anyway. The only safe extra remote commands are the
**stop** actions (`pump_off`, `feed_stop`) — they can only ever *stop* something. See §5 for the
app-button recommendation.

---

## 4. Evaluation with the confusion matrix

The detector is validated quantitatively with a **labelled confusion matrix** built into the
firmware. Six classes are tracked:

| Index | Class           | Outcome |
|:-----:|-----------------|---------|
| 0 | NORMAL          | true negative when no anomaly |
| 1 | MOTOR_STALL     | halt |
| 2 | DRY_RUN         | halt |
| 3 | VOLTAGE_DROP    | warning |
| 4 | TEMP_TOO_HIGH   | warning |
| 5 | TEMP_TOO_LOW    | warning |

For each monitored cycle the firmware records exactly one outcome:
`confmat[truth][detected]++`. Rows are the **ground truth** you set; columns are what the detector
**actually decided**. The diagonal is "correct"; everything off-diagonal is an error.

### How to run an evaluation
1. Open the dashboard in developer mode: `http://<observer-ip>/?dev=1`.
2. In the **Evaluation** panel, click the class you are about to *induce* (this calls
   `/eval?truth=K`). Run with Default mode **ON** so cycles repeat automatically.
3. Physically create that condition and let cycles run; each one adds a record:
   * **NORMAL** — run the pump normally, in clean conditions.
   * **MOTOR_STALL** — pinch/block the pump outlet so current rises above `μ + 3σ`.
   * **DRY_RUN** — run the pump out of water so current falls below `30 % μ`.
   * **VOLTAGE_DROP** — weaken the supply (e.g. series resistance) so the bus drops below 90 %.
   * **TEMP_TOO_HIGH / LOW** — warm/cool the DS18B20 probe past its dynamic threshold.
4. Switch class for the next scenario, or click **Stop labelling** when done. **Reset matrix**
   clears all counts.

### Metrics the panel reports
* **Accuracy** = Σ diagonal / Σ all cycles.
* **False-positive rate (FPR)** = of NORMAL cycles, the fraction flagged as *any* fault
  (row 0, off-diagonal ÷ row 0 total). This is the "nuisance alarm" rate.
* **False-negative rate (FNR)** = of fault cycles, the fraction reported as NORMAL
  (fault rows, column 0 ÷ all fault rows). This is the "missed fault" rate — the one that matters
  most for a safety system.
* **Per-class recall** = `confmat[k][k] ÷ row k` (read directly from the diagonal vs. its row).
* **Mean detection latency** = average time from the first raw out-of-range sample to
  *ANOMALY CONFIRMED* (it captures the full EWMA-smoothing + 3-tick confirmation delay).

The clean run currently in the logs (≈20 consecutive NORMAL cycles, `confmat[0][0]` only,
CUSUM ≈ 0) is an example of the NORMAL column being validated end-to-end: 100 % accuracy and
0 % false positives on that segment.

### Should the matrix live on the user's dashboard? **No.**
The confusion matrix is a **developer / validation instrument**, not an operator feature. An
aquarium owner has no use for ground-truth labelling, FPR or FNR, and exposing it would only add
confusion (and a small extra polling load on the Observer). That is exactly why the panel is
**dev-gated**: a normal user gets a clean dashboard, and a developer opens `?dev=1` when they need
it. The evaluation *harness* stays compiled in the firmware (it is harmless and free when no truth
is set) — only its **UI** is hidden.

### How we (developers) use it to produce the report
The matrix turns "it seems to work" into **numbers**. For the documentation/report we:
1. run each labelled scenario above for a handful of cycles,
2. read off the **table** and the four headline metrics (accuracy, FPR, FNR, mean latency),
3. present the table as the quantitative evaluation of the predictive-maintenance detector, and
4. pair it with the **CUSUM degradation** result as the *predictive* metric — i.e. how much
   lead-time the system gives before a hard stall. Together they answer both "does it detect
   faults correctly?" (matrix) and "does it warn early?" (CUSUM).

---

## 5. Practical notes

### IoT MQTT Panel — which buttons to keep
The full command set is `mode_on`, `mode_off`, `clear_halt`, `pump_on[:sec]`, `pump_off`,
`feed[:sec]`, `feed_stop`, `calibrate`. For the **remote** panel, keep only the **essentials**:
* **`clear_halt`** — clear the alarm,
* **`mode_off`** then **`mode_on`** — restart a fresh default cycle.

Optionally add the two **safe-stop** buttons (`pump_off`, `feed_stop`) since they can only stop
things. Leave the manual **start** buttons (`pump_on`, `feed`, `calibrate`) out of the remote panel
— use them on the local dashboard, where you can see the tank.

* A **Publish** button *sends* a command to `float/aq1/cmd` (use the **exact** topic, not a
  wildcard). The payload is the command word, e.g. `clear_halt`.
* A **Subscribe** button does the opposite: it **receives and displays** messages on a topic
  (e.g. subscribe to `float/aq1/telemetry` or `/state` to watch live values on the phone). It does
  **not** send anything. So: *subscribe = read status remotely, publish = command*.
* Use a **unique MQTT Client ID** in the app (e.g. `float-phone`) — it must differ from the
  firmware's `float-observer`, or the two will keep kicking each other off the broker.

### MQTT certificate verification (`MQTT_TLS_VERIFY`) — should you enable it?
The link is already encrypted (`MQTT_TLS 1`). The remaining question is whether the device also
**checks the broker's identity**.
* `MQTT_TLS_VERIFY 0` (current) — TLS encrypts the traffic, but the device does **not** verify the
  broker certificate. Encryption is real, but in principle a man-in-the-middle with a fake
  certificate could impersonate the broker.
* `MQTT_TLS_VERIFY 1` — the device validates the broker certificate against the **root CA**
  (HiveMQ Cloud uses *Let's Encrypt ISRG Root X1*) and needs **NTP time** (for the certificate
  date check). This closes the MITM gap.

**Recommendation:** enable it for the final/production build — it is a free security upgrade and
makes the cloud leg genuinely authenticated. It is left at `0` in the shipped files so the working
setup is not broken. To turn it on: set `#define MQTT_TLS_VERIFY 1` and paste the ISRG Root X1 PEM
into `MQTT_ROOT_CA` (obtain it from letsencrypt.org or with
`openssl s_client -connect <host>:8883 -showcerts`). The hooks (NTP `configTime`, `setCACert`,
the time-wait before connecting) are already in the code. Cost: a slightly slower first connect,
and the device will not connect until it has the time from NTP.

### Re-flashing
All three components changed in this revision (Target firmware, Observer firmware, and the
dashboard, which is compiled into the Observer). **Re-flash both ESP32s.** The Arduino Uno is
unchanged.