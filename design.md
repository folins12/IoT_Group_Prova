# FLOAT - Design Document 

## 1. Hardware Components

| Component | Node | Role |
|---|---|---|
| ESP32-S3 (Heltec WiFi LoRa 32 V3) | Target | Sensing/actuation; ESP-NOW; deep sleep |
| ESP32-S3 (Heltec WiFi LoRa 32 V3) | Observer | Detection; WiFi STA; dashboard + cloud gateway; INA219 host |
| Arduino Uno | Turbidity | Hosts the analog turbidity sensor; serves readings over UART |
| INA219 #1 (I2C) | Observer | Pump supply current + bus voltage |
| INA219 #2 (I2C, separate bus) | Observer | Arduino Uno supply current + voltage |
| DS18B20 waterproof probe (1-Wire) | Target | Water temperature |
| Analog turbidity sensor | Arduino Uno | Water clarity (raw ADC → NTU) |
| DC pump (3–5 V) | Target | Filtration / circulation |
| SG90 micro-servo | Target | Food-dispenser gate |
| Active buzzer | Observer | Local audible alarm |
| Li-ion/LiPo 3.7 V + bulk capacitor | Target | Power |
| Regulated 5 V source | Arduino Uno | Power|

**Pin map (Target):** pump `GPIO47`, servo `GPIO6`, DS18B20 `GPIO7`, UART to Arduino `RXD1=GPIO4 / TXD1=GPIO5`.
**Pin map (Observer):** INA219 #1 on I2C bus 0 `SDA=GPIO41 / SCL=GPIO42`; INA219 #2 on I2C bus 1 `SDA=GPIO47 / SCL=GPIO48`; buzzer `GPIO7`.

## 2. Network Architecture

The Target↔Observer link uses **ESP-NOW** (IEEE 802.11 connectionless frames between two MAC addresses): no router, no DHCP, no IP. Both nodes are pinned to the **same radio channel**: the Observer joins the local WiFi as a station and reads back its channel; the Target scans for the same SSID and locks ESP-NOW to that channel (fallback to channel 13 if the network is not found). ESP-NOW frames are **AES-CCM encrypted**.

The Observer simultaneously acts as the gateway to the outside world over its WiFi station interface: it serves the dashboard on the local network, bridges telemetry/alerts to an MQTT cloud broker over TLS, and sends e-mail notifications.

```
  Arduino Uno ──UART──► Target (ESP32-S3) ──ESP-NOW (encrypted, ch = WiFi ch)──► Observer (ESP32-S3)
   • turbidity          • temp, pump, servo                                       • detection + CUSUM
                                                                                  • 2× INA219
                                                                                  • WiFi STA gateway
                                                                                          │
                            ┌──────────────────────────────┌──────────────────────────────┤
                            ▼                              ▼                              ▼
                   Local dashboard (LAN)        MQTT broker (HiveMQ, TLS)        E-mail (Apps Script)
                   http://<observer-ip>/        float/aq1/{telemetry,state,      HTTPS webhook on alert
                                                alert,cmd}
```

## 3. Communication Protocols

### 3.1 Target ↔ Observer (ESP-NOW)

| Direction | Message | Meaning |
|---|---|---|
| T → O | `LOG:<text>` | Human-readable status line |
| T → O | `DATA:SENSOR:<ntu>,<temp_c>` | Turbidity + temperature |
| T → O | `HB:<pump><halt>` | Idle heartbeat (used for OFF→ON reconciliation) |
| T → O | `CMD:<name>\|ID:<n>` | Reliable command; Observer replies `ACK:<n>` |
| O → T | `CMD:START_LEARN` / `START_MONITOR` / `STOP_MEASURE` | Drive the detection phase per cycle |
| O → T | `HALT` | Emergency pump stop (sent ×10 in a burst against RF loss) |
| O → T | `CMD:CLEAR_HALT`, `CMD:AUTO_ON_RESET`, `CMD:AUTO_OFF`, `CMD:PUMP_*`, `CMD:FEED*`, `CMD:CALIBRATE` | Mode / manual control |
| O → T | `CAL:<μ,σ,th_stall,th_dry,th_hi,th_lo>` | Calibration result (logged on the Target) |
| O → T | `ACK:<n>` | Handshake reply |

Critical commands carry a sequence number and are retried up to 5 times with exponential backoff until acknowledged.

### 3.2 Target ↔ Arduino Uno (UART)

A request/echo handshake on `Serial1`: the Target sends `'T'`, the Uno replies with the raw ADC value terminated by newline, and the Target confirms with `'E'<value>`. The Uno sleeps on its watchdog (~4 s) and listens briefly on each wake; the Target repeats `'T'` long enough to always catch a wake window. On timeout the Target falls back to a safe default reading. The acquisition runs **before** the ESP32 radio is enabled, to keep RF noise off the analog line.

### 3.3 Dashboard → Observer (HTTP, polling)

| Endpoint | Action |
|---|---|
| `GET /` | Serve the dashboard (streamed in 1 KB chunks) |
| `GET /data` | Live JSON (current, EWMA, voltage, temperature, turbidity, mode, thresholds, auxiliary supply) |
| `GET /events?last=<id>` | Incremental event feed |
| `GET /mode?set=on\|off` | Switch autonomous / manual mode |
| `GET /cmd?a=<action>&sec=<n>` | Manual control (`clear_halt` always; pump/feed/calibrate only in OFF mode) |
| `GET /eval?...` | Evaluation harness (set ground-truth class, reset, read confusion matrix) |

### 3.4 Observer ↔ Cloud (MQTT over TLS, base `float/aq1`)

| Topic | Direction | Purpose |
|---|---|---|
| `…/telemetry` | publish (~5 s) | Live current/EWMA/voltage/temperature/turbidity |
| `…/state` | publish, **retained** | Device shadow: mode, auto, halted, calibrated, μ, thresholds, drift |
| `…/alert` | publish on event | Anomaly reason + severity + context |
| `…/cmd` | subscribe | Remote commands (mode, clear-halt, and OFF-mode manual actions) |

The retained `…/state` topic is the **device shadow**: any client that connects later immediately receives the last-known system state.

## 4. Software Architecture

### 4.1 Target Node

The autonomous cycle lives entirely inside `setup()` (one full cycle per wake), so the Target can deep-sleep between cycles. `loop()` runs **only** in manual (OFF) mode, where it acts as a bench: it services the manual pump/feed/calibrate requests and emits a heartbeat.

```
            boot (RTC + NVS state restored)
                          │
      reset-storm filter (only resets that occurred
      while the pump was driving load count toward a
      safety halt; idle/sleep brownouts are ignored)
                          │
      read DS18B20 temperature   ──►   read turbidity (Arduino UART)
                          │
      scan WiFi channel · init encrypted ESP-NOW
                          │
      send boot data · grace window for piggy-backed HALT / AUTO_*
                          │
        ┌─────────────────┼───────────────────────────────┐
        │ system halted?  │ default-mode OFF?             │ else: AUTO CYCLE
        ▼                 ▼                               ▼
  stay awake,       stay awake, run loop()        set NVS safety latch
  show HALTED       as a manual bench             (halted=true, pumping=true)
  on dashboard                                            │
                                        ┌─────────────────┼───────────────────┐
                                        │ bootCount == 0? │ turbidity ≤ 50?   │ else
                                        ▼                 ▼                   ▼
                                  LEARN (pump 10 s,   MONITOR (pump 10 s,  feed (servo) +
                                  START_LEARN)        START_MONITOR)       short pump 5 s
                                        └─────────────────┼───────────────────┘
                                                          │
                                           clean cycle → clear safety latch
                                                          │
                                                   deep sleep 20 s
```

**State persistence.** `RTC_DATA_ATTR` variables (boot counter, etc.) survive deep sleep but not a power loss. **NVS** flags (`halted`, `pumping`, `storm`, `auto`) survive *everything*, including brown-outs, and are the source of truth for safety. The `halted` flag is written **before** any pump activation and cleared only on a fully clean cycle, so a brown-out during a stall leaves the system safely halted on the next boot.

### 4.2 Observer Node

The Observer runs two concurrent contexts that share global state under a critical-section spinlock: the main `loop()` (measure and decide) and the **ESP-NOW receive callback** (parse Target messages, run calibration, talk back).

```
                 loop()
                   │
      read INA219 #1 (current, voltage) · update EWMA
      read INA219 #2 every 2 s (auxiliary current, battery-low warning)
                   │
   ┌───────────────┼───────────────────────┐
   │system locked? │ mode == LEARNING?     │ mode == MONITORING & calibrated?
   ▼               ▼                       ▼
 buzz + HALT    collect current     grace period (skip inrush) → flags:
 telemetry,     samples             STALL  : EWMA > μ + 3σ
 hold                               DRY_RUN: EWMA < 0.30 μ
                                    VOLT   : V < 0.90 V_cal
                                    TEMP   : T outside learned band
                                           │
                                           │ confirm 3× consecutive
                                           ▼
                                    classify by priority → act
                                           │
                                           ▼
                                      else → IDLE
```

The receive callback handles `START_LEARN` / `START_MONITOR` / `STOP_MEASURE`. On `STOP_MEASURE` after a learning cycle it runs the Hampel calibration; after a monitoring cycle it folds the healthy mean into the CUSUM and records the evaluation outcome.

### 4.3 Operating Modes

- **Autonomous (ON):** the Target runs the sense → sleep → act cycle on its own.
- **Manual (OFF):** the Target stays awake; the operator can run the pump (timed), dispense food (once or on an interval), or force a calibration, from the dashboard or via MQTT. 

A mode change always discards the calibration and forces a fresh relearn, so thresholds are never applied to a stale baseline.

## 5. Anomaly Detection Algorithm

### 5.1 Calibration (Hampel filter)

On the first boot of a cycle set, the Observer collects pump-current samples while the pump runs normally, then computes a **robust** baseline:

```
MAD   = median( |x_i − median(x)| )
σ_H   = 1.4826 × MAD
keep x_i  if  |x_i − median(x)| ≤ 3 × σ_H
μ, σ  = mean and std of the kept (steady-state) samples only
```

The factor `1.4826` makes `σ_H` a consistent estimator of the standard deviation for normally distributed data. Because the test is anchored on the **median**, an inrush spike captured during learning is rejected as an outlier instead of inflating the thresholds.

Derived thresholds:

```
th_stall    = μ + 3σ          (floored at μ + 15 mA)
th_dry_run  = 0.30 × μ
th_volt_min = 0.90 × V_cal
temperature band = [ μ_T − Δ , μ_T + Δ ],  Δ = max(5σ_T, 1.5 °C),  clamped to [16, 32] °C
```

The temperature band is **learned per tank** from the temperature samples collected during the same learning window: a tropical tank calibrated at 26 °C gets different limits than a cold-water tank at 20 °C, with no manual configuration.

### 5.2 Monitoring (EWMA + confirmation + grace)

```
ewma_t = α · I_raw + (1 − α) · ewma_{t−1}        α = 0.2
```

- **EWMA** damps single-sample spikes (one spike moves the average by ≤20 % of its magnitude) while tracking a sustained shift.
- **Grace period:** the first 4 samples after the pump starts are skipped to let the motor reach steady state; the EWMA is then re-seeded with the settled value so inrush never leaks into the monitoring window.
- **Confirmation gate:** an anomaly flag must hold for `CONFIRM_NEEDED = 3` consecutive samples (sample period ≈ 400 ms) before it is acted upon.

### 5.3 Anomaly Types, Actions and Priority

| Anomaly | Condition (on EWMA / reading) | Physical cause | Action |
|---|---|---|---|
| `MOTOR_STALL` | `ewma > μ + 3σ` | Impeller blocked / mechanical fault | **HALT** (10× burst), lock, buzzer |
| `DRY_RUN` | `ewma < 0.30 μ` | No water, pump running dry | **HALT** (10× burst), lock, buzzer |
| `VOLTAGE_DROP` | `V < 0.90 V_cal` | Supply sag / brown-out | **Warning** only (pump keeps running) |
| `TEMP_TOO_HIGH` | `T > th_temp_high` | Heater fault / overheating | **Warning** only |
| `TEMP_TOO_LOW` | `T < th_temp_low` | Cold draft / heater failure | **Warning** only |
| `DEGRADATION` | CUSUM exceeds limit (§6) | Slow wear / partial fouling | **Warning** (predictive maintenance) |

Priority when several flags are active at once: `MOTOR_STALL` > `DRY_RUN` > `VOLTAGE_DROP` > `TEMP`. Only the two safety-critical anomalies stop the pump; the others are advisory, because cutting circulation on a voltage dip or a temperature excursion would harm the fish rather than help.

## 6. Predictive Maintenance - CUSUM

At the end of each healthy monitoring cycle, the mean current of that cycle is compared to the learned baseline and accumulated in a one-sided CUSUM:

```
dev      = (cycle_mean − μ) − K·σ            K = 1.0   (tolerance band)
cusum   += dev,   clamped at 0
fire if   cusum > H·σ                        H = 3.0   (decision threshold)
```

A small but **persistent** upward drift in the pump's healthy current accumulates until it crosses the limit and raises a `DEGRADATION` service warning, while ordinary cycle-to-cycle noise washes out. This catches gradual wear or partial fouling before it becomes a hard stall - the predictive-maintenance goal.

## 7. Multi-Sensor Fusion

Turbidity and temperature interact. Optical turbidity readings depend on water viscosity, which falls as temperature rises (particles settle faster), so the same suspended load reads differently at 20 °C and 30 °C. The Target applies a linear correction before transmitting:

```
NTU_corrected = NTU_raw / (1 + 0.005 × (T − 25))
```

Without it, a warming tank would appear to "self-clean", masking a real water-quality problem. Temperature additionally drives its own anomaly band (§5.1) and is recorded during calibration to personalise that band.

## 8. Power-Consumption Monitoring (two INA219)

INA219 #1 measures the current and voltage of the Target node, including the pump and the sensors connected to it, and provides the measurements used by the anomaly detection algorithms. INA219 #2 monitors the power consumption of the Arduino Uno, which is dedicated to acquiring turbidity sensor readings. 
For the dashboard, the currents measured by both INA219 sensors are summed to display the system's total instantaneous power consumption.

## 9. Dashboard, Cloud and Remote Control

- **Local dashboard:** a single-page interface served by the Observer over the local network, with a live consumption chart, system state, event feed, manual controls (in OFF mode), and a developer-only evaluation panel.
- **MQTT cloud bridge:** publishes telemetry, a retained device-shadow state, and alerts to HiveMQ Cloud, and subscribes to a command topic for remote control (a phone MQTT client can switch mode, clear a halt, or drive manual actions).
- **E-mail alerts:** on a confirmed anomaly the Observer triggers an HTTPS webhook (a Google Apps Script web app) that sends a notification e-mail.

## 10. IoT Security

| Layer | Measure | Rationale |
|---|---|---|
| ESP-NOW (Target↔Observer) | **AES-CCM encryption** (PMK + per-peer LMK) | Confidentiality + authenticity of the safety link |
| MQTT (Observer↔cloud) | **TLS** with **broker-certificate verification** and username/password auth | Encrypted, server-authenticated, access-controlled cloud channel |
| E-mail webhook | HTTPS | Encrypted transport to the notification endpoint |

## 11. Deep Sleep and Energy

The Target deep-sleeps for 20 s between cycles, which removes its idle draw during the gap. Within an active cycle the dominant cost is the **pump run itself** (the unavoidable work), plus a short sensing/transmit phase; the brown-out detector is disabled and a bulk capacitor absorbs the pump/servo inrush so the cycle completes on battery. Battery life therefore depends mainly on how often and how long the pump runs and on the cell capacity, rather than on idle consumption. The Observer runs continuously (it must serve the dashboard and receive ESP-NOW at any time) and is intended to be mains- or power-bank-powered.

## 12. Evaluation Instrumentation

An on-device **confusion-matrix harness** records, for every monitored cycle, the ground-truth class (set by the operator while inducing a fault) against the class the detector reported, and measures detection latency. It is exposed on the `…/eval` endpoint and on a developer-only dashboard panel. The methodology, metrics and results are described in the Evaluation document.
