# FLOAT - Design Document (Final)



## 1. Hardware Components

| Component | Node | Role |
|---|---|---|
| ESP32 (Heltec WiFi LoRa 32 V3) | Target | Main MCU, WiFi/ESP-NOW, deep sleep |
| ESP32 (Heltec WiFi LoRa 32 V3) | Observer | Main MCU, WiFi AP, INA219 host |
| INA219 current/voltage sensor (I2C) | Observer | Motor current and bus voltage measurement |
| DS18B20 waterproof temperature probe | Target | Real-time water temperature |
| Turbidity sensor (analog, 0–5 V) | Target (via ADC) | Water clarity in NTU |
| Submersible water pump (3-5 V DC) | Target | Water filtration and circulation |
| SG90 micro servo | Target | Food dispenser gate actuation |
| Active buzzer | Observer | Local audible alarm on anomaly |



## 2. Network Architecture

The system uses **ESP-NOW** (IEEE 802.11 connectionless protocol, no router needed) on channel 13. This choice eliminates battery brownouts caused by DHCP association and dramatically reduces communication latency compared to standard WiFi or MQTT.

```
   Target (ESP32)  ──ESP-NOW ch13──►  Observer (ESP32)    ──WiFi AP──►  Browser
  • Sensor data                      • Anomaly detection              • Dashboard
  • Status commands                  • Emergency HALT                 • http://192.168.4.1
  • ACK replies                      • Dashboard serving
```

The Observer runs in **WIFI_AP_STA** mode simultaneously: AP mode serves the dashboard, STA mode is required for ESP-NOW. Both interfaces share channel 13.



## 3. Communication Protocol

### 3.1 Target → Observer (ESP-NOW)

| Message | Format | Description |
|---|---|---|
| Log | `LOG:<text>` | Human-readable status line, printed on Observer Serial |
| Sensor data | `DATA:SENSOR:<ntu>,<temp_c>` | Turbidity (NTU) + temperature (°C) |
| Command (reliable) | `CMD:<name>\|ID:<n>` | Critical command; Observer replies `ACK:<n>` |

### 3.2 Observer → Target (ESP-NOW)

| Message | Description |
|---|---|
| `HALT` | Emergency pump stop; sent ×10 in burst to overcome RF interference |
| `ACK:<n>` | Handshake reply to a `CMD:\|ID:<n>` message |

### 3.3 Dashboard → Observer (HTTP POST)

| Endpoint | Action |
|---|---|
| `POST /api/pump` | Toggle pump state |
| `POST /api/servo` | Dispense food |
| `POST /api/reset` | Unlock system after anomaly |
| `POST /api/learn` | Force new calibration cycle |
| `POST /api/auto` | Toggle auto mode |
| `GET /api/events` | Server-Sent Events stream (JSON, every 400 ms) |



## 4. Software Architecture

### 4.1 Target Node State Machine

```
           boot
             │
             ▼
    ┌─────────────────┐
    │  system_halted? │──YES──► deep sleep (20 s, reset)
    └────────┬────────┘
             │ NO
             ▼
    read DS18B20 temperature
    read turbidity (NTU)
    init ESP-NOW
             │
    ┌────────▼────────┐
    │  bootCount==0?  │──YES──► START_LEARN → STOP_MEASURE
    └────────┬────────┘
             │ NO
    ┌────────▼─────────────┐
    │  turbidity < 50 NTU? │──YES──► START_MONITOR + pump 10 s → STOP_MEASURE
    └────────┬─────────────┘
             │ NO
             ▼
    dispense food (servo) + short pump 5 s → STOP_MEASURE
             │
             ▼
    goToSleep (20 s)
```

### 4.2 Observer Node State Machine

```
              loop()
                │
                ▼
    read INA219 (current, voltage)
    update EWMA
    push SSE every 400 ms
                │
    ┌───────────▼───────────┐
    │   system_locked?      │──YES──► buzzer, HALT TELEM, delay 5 s
    └───────────┬───────────┘
                │ NO
    ┌───────────▼───────────┐
    │  mode == LEARNING?    │──YES──► collect current samples
    └───────────┬───────────┘
                │ NO
    ┌───────────▼───────────────────────────────────────────────┐                  ____________________________________
    │  mode == MONITORING  AND  is_calibrated?                  │                 | mode == MONITORNG AND is_calibrated|
    │                                                           │                 |                                    |
    │  grace_period > 0 → skip (motor inrush)                   │                 |  TEMP: T < 18 °C or T > 30 °C ?    |
    │  grace_period == 0 → check:                               │_________________|                                    |
    │    STALL   : EWMA > μ + 3σ                                │                 |                                    |
    │    DRY_RUN : EWMA < 30 % μ                                │                 | anomaly confirmed ( x consecutive) |
    │    VOLT    : V < 90 % V_baseline                          │                 |                                    |
    │                                                           │                 |   ->    buzzer                     |
    │                                                           │                 |                                    |
    │  anomaly confirmed (3 consecutive) → HALT burst + lock    │                 |___________________________________ |
    └───────────┬───────────────────────────────────────────────┘                  
                │ NO
                ▼
        IDLE (delay 2000 ms)
```



## 5. Anomaly Detection Algorithm

### 5.1 From 3-Sigma to EWMA + Hampel

The mid-term version used a plain mean + 3σ threshold computed over learning samples. This approach has a known weakness: if the motor inrush current (a transient spike at startup) is captured during learning, it inflates both the mean and the standard deviation, raising the stall threshold above levels that would actually indicate a stall.

The final version uses two techniques together:

**EWMA (Exponential Weighted Moving Average):**
```
ewma_t = α × I_raw + (1 − α) × ewma_{t-1}     α = 0.2
```
Applied to the raw current reading before anomaly comparison. With α = 0.2, transient spikes are damped: a single spike only moves the EWMA by 20% of its magnitude. A sustained stall (which persists) will raise the EWMA consistently over several samples, triggering detection after `CONFIRM_NEEDED = 3` consecutive ticks.

**Hampel Filter (calibration phase):**
```
MAD  = median( |x_i − median(x)| )
σ_H  = 1.4826 × MAD
Keep x_i  if  |x_i − median(x)| ≤ k × σ_H     k = 3.0
```
The factor 1.4826 makes `σ_H` a consistent estimator of the standard deviation under a normal distribution. Outliers (inrush spikes) are excluded from the baseline computation because they deviate far from the median. The result: `baseline_mean` and `baseline_std` are computed only from steady-state running current, making `th_stall = μ + 3σ` a tight and accurate threshold.

**Grace period:**
After the pump starts, the first 4 samples (~1.6 s) are discarded to let the motor reach steady state. At the end of the grace period, the EWMA is re-seeded with the current raw value, so inrush current does not carry over into the monitoring window.

### 5.2 Anomaly Types and Thresholds

| Anomaly | Condition | Physical cause | Action |
|---|---|---|---|
| MOTOR_STALL | `ewma > μ + 3σ` | Impeller blocked, mechanical failure | HALT burst, buzzer ×3, system lock |
| DRY_RUN | `ewma < 0.30 × μ` | Tank empty, pump running without water | HALT burst, buzzer ×3, system lock |
| VOLTAGE_DROP | `V < 0.90 × V_baseline` | Power supply degradation or brownout | HALT burst, buzzer ×3, system lock |
| TEMP_OUT_OF_RANGE | `T < 18 °C or T > 30 °C` | Heater failure, cold draft, overheating | HALT burst, buzzer ×3, system lock |

Priority order (when multiple flags active simultaneously): STALL > DRY_RUN > VOLTAGE_DROP > TEMP.

### 5.3 Temperature in the Learning Phase (planned extension)

The current implementation uses static temperature thresholds [18 °C, 30 °C]. A natural improvement - identified as the next development step - is to **record the water temperature during the calibration learning phase** and derive an adaptive baseline:

```
T_baseline = mean(T_during_learning)
T_min = T_baseline − ΔT_low    (e.g. ΔT_low  = 3 °C)
T_max = T_baseline + ΔT_high   (e.g. ΔT_high = 4 °C)
```

This would make the temperature anomaly detection **personalised to each aquarium**: a tropical tank calibrated at 26 °C would have different limits than a cold-water tank calibrated at 20 °C, without requiring any manual configuration. The architecture is fully in place to support this (temperature is already transmitted during the learning window); it requires only adding an accumulator in the Observer's `LEARNING` branch.



## 6. How Turbidity and Temperature Work Together

The two sensors on the Target are not independent; they interact in the viscosity compensation.
Water turbidity sensors measure optical backscattering. At higher temperatures, water viscosity decreases, causing suspended particles to settle faster. The same physical quantity of suspended material produces a lower ADC reading at 30 °C than at 20 °C. The Target applies a linear correction before transmitting:
```
NTU_corrected = NTU_raw / (1 + 0.005 × (T − 25))
```
Without this correction, a warming tank would appear to "self-clean" in sensor readings, masking actual water quality issues.



## 7. Dashboard and IoT Security

### 7.1 Access
The Observer creates a WiFi Access Point:
- **SSID:** `FLOAT-Dashboard`
- **Password:** `float1234`
- **URL:** `http://192.168.4.1`

No router, Internet connection, or mobile data is required.

### 7.2 Security measures implemented

| Measure | Implementation | Rationale |
|---|---|---|
| WPA2 AP password | `WiFi.softAP(AP_SSID, AP_PASS)` | Prevents unauthorized access to the dashboard and command endpoints |
| HTTP POST-only for commands | All `/api/*` commands require POST | Dashboard actions cannot be triggered by a passive page load or URL visit |
| 423 Locked response | All command handlers check `system_locked` | Prevents issuing pump commands while a critical anomaly is active |
| Servo blocked during pump | JavaScript + server-side guard | Prevents simultaneous actuator commands that could cause mechanical conflict |
| Rate limiting (planned) | Identified but not yet implemented | Would prevent command flooding from a compromised browser |

### 7.3 Security limitations acknowledged

HTTP (not HTTPS) is used because the ESP32 `WebServer` library does not support TLS without self-signed certificate management, which would significantly complicate deployment. Since the AP is local-only (no Internet), the attack surface is limited to devices physically present on the local network. HTTPS with a self-signed certificate is identified as the primary future security improvement.



## 8. Deep Sleep and Energy Budget

The Target node uses ESP32 deep sleep between measurement cycles.

| Parameter | Value |
|---|---|
| Active time per cycle | ~10 s (boot + sense + transmit) |
| Sleep time per cycle | 20 s |
| Active duty cycle | 2/22 = **9.1%** |
| Sleep duty cycle | 20/22 = **90.9%** |
| Estimated current (active) | ~195 mA (ESP32 + pump startup) |
| Estimated current (sleep) | ~14 mA (deep sleep RTC only) |
| Mean current (2000 mAh battery) | ~14.6 mA → **~137 h ≈ 5.7 days** |

The Observer node runs continuously (no deep sleep) since it must serve the dashboard and receive ESP-NOW messages at any time.



## 9. Evolution from Mid-term Version

| Feature | Mid-term (v1) | Final (v4) |
|---|---|---|
| Anomaly detection | 3-sigma rule, stall only | EWMA + Hampel; 4 anomaly types |
| Temperature | Not used | DS18B20, glitch-filtered, used for turbidity compensation and anomaly detection |
| Communication reliability | Fire-and-forget | ACK + retry with exponential backoff for critical commands |
| Dashboard | Not present | Real-time HTML dashboard on ESP32 AP, SSE push at 400 ms |
| ESP-NOW channel | Default (0) | Fixed channel 13, max TX power |
| Deep sleep | 10 s cycle | 20 s cycle (91% sleep) |
| Turbidity | Simulated | Real ADC function implemented; random demo mode pending hardware fix |
| Anomaly burst | Single HALT packet | ×10 burst to overcome RF interference |
| Grace period | Not present | 4 samples (~1.6 s) after pump start |
