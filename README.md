# FLOAT
**Framework for Local Observation of Aquatic Tanks**

Edge-IoT system that runs an aquarium (water circulation + feeding) and, while it works, watches the pump's electrical signature to catch faults before they become failures. Fully autonomous on the local network; status and alarms also mirrored to the cloud.

Demo video: https://www.youtube.com/watch?v=Wky8BkITGp4

---

## Hardware

- **Target — ESP32-S3** · actuator + sensor node: drives pump, feeding servo, temperature probe.
- **Observer — ESP32** · always-on brain + gateway: measures pump current, detects anomalies, serves the dashboard, bridges to the cloud.
- **Arduino Uno** · dedicated front-end for the analog turbidity probe.
- Peripherals: turbidity probe, **DS18B20** temperature, **water pump**, **feeding servo**, **INA219** current sensor, buzzer.

## Connections

- **Arduino Uno ↔ Target** — UART (Uno TX→GPIO4, Uno RX→GPIO5), `'T'`/`'E'` handshake.
- **DS18B20 → Target** — 1-Wire (GPIO7). **Pump** GPIO47, **Servo** GPIO6.
- **INA219 → Observer** — I²C (SDA 41 / SCL 42); measures the Target's supply current. Buzzer GPIO7.
- **Both ESP32 → WiFi hotspot** (STA); dashboard at the IP shown on boot (e.g. `http://172.20.10.5`).
- MACs — Observer `F0:9E:9E:77:73:60` · Target `48:27:E2:E2:E3:0C`.

## Communication

- **Target ↔ Observer → ESP-NOW** (not WiFi): connectionless, peer-to-peer, low-latency, low-power, no router needed. Messages: `LOG`, `DATA`, `CMD` (START_LEARN / START_MONITOR / STOP_MEASURE / HALT / mode / pump / feed / calibrate); ACK on critical commands. Both nodes auto-discover the hotspot's channel.
- **Observer ↔ phone/PC → WiFi (HTTP):** the Observer hosts the live dashboard on the LAN.
- **Observer ↔ cloud → MQTT over TLS** (HiveMQ Cloud, topics `float/aq1/...`): telemetry, retained device-shadow state, alerts, and remote commands. Plus **email alerts** on anomalies (Google Apps Script).

## Security

- **ESP-NOW encrypted** (AES-CCM, shared PMK + per-peer LMK) → no spoofed HALT/PUMP over the air.
- **MQTT authenticated** (broker username/password) + **TLS encrypted**, with optional **server-certificate verification** (root CA pinned + NTP time) → MITM-proof cloud link.
- **NVS safety latch** → pump stays halted across resets/brownouts until explicitly cleared.

## Sensing & actuation (what each is for)

- **Turbidity** → selects the cycle action: circulate/filter (pump) vs. dispense food (servo).
- **Temperature** → corrects the turbidity reading + triggers high/low temperature alarms.
- **Pump** → water circulation/filtration. **Servo** → drops food.
- **Pump current (INA219)** → the health signal used for anomaly detection.

## Anomaly detection (at the edge)

- **Calibration:** learns the pump's normal current with **EWMA + Hampel** filter (rejects motor inrush) → baseline μ, σ.
- **Detection** (confirmed over **3 consecutive samples**, after an inrush grace window):
  - **MOTOR_STALL** (I > μ+3σ) → **immediate HALT**.
  - **DRY_RUN** (I < 30% μ) → **immediate HALT**.
  - **VOLTAGE_DROP**, **TEMP_TOO_HIGH/LOW** → warning only (pump keeps running).
  - **DEGRADATION** (slow current creep, per-cycle CUSUM) → predictive "service soon" warning.
- Reaction time ≈ **1.2 s** (3 × 400 ms).

## Power

- Target runs a short cycle, then **deep-sleeps ~20 s**; reduced WiFi TX power and brownout handling.

## What the user can do

**At home — local dashboard:**
- live temperature / current / turbidity + pump-current chart with threshold lines;
- **Default mode** ON (autonomous cycle) / OFF (manual bench);
- manual controls (OFF only): run pump, dispense food, **stop feeding**, calibrate;
- **Clear HALT** to recover after a stall/dry-run; live event log + anomaly pop-ups.

**Away — cloud:**
- automatic **email** on every anomaly;
- from the phone (IoT MQTT Panel) or the HiveMQ web client: **clear the alarm** and **restart the default cycle**.

---

## Team

- Michele Libriani, 1954541 — www.linkedin.com/in/michele-libriani-805985316
- Andrea Folino, 1986019 — www.linkedin.com/in/andrea-folino-981aa5322
- Edoardo Zompanti, 1985499 — www.linkedin.com/in/edoardo-zompanti-a8678a3b4