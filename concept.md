# FLOAT - Concept Document (Final)



## 1. Problem Statement

Home and small-scale aquariums require continuous maintenance to keep fish alive and healthy. Two failure modes are responsible for the majority of fish deaths in unattended tanks:

1. **Pump failure** - the water circulation pump can mechanically stall (blocked impeller, foreign object) or run dry (water level too low). Both conditions destroy the motor within minutes and leave the tank without oxygenation and filtration.
2. **Water quality degradation** - turbidity rises over time due to organic waste, uneaten food, and algae. High turbidity signals a deteriorating chemical and biological environment. Left unaddressed, it leads to bacterial blooms and fatal oxygen depletion.
3. **Temperature excursions** - water temperature outside the species-specific safe range (typically 18–30 °C) stresses fish even if the water appears visually clean.

Existing consumer solutions are either purely reactive (manual checks), extremely expensive (professional monitoring systems), or cloud-dependent (commercial smart feeders that stop working without Internet). No low-cost, fully offline, edge-capable system that combines motor health monitoring with environmental sensing exists on the market at the hobbyist price point.



## 2. Proposed Solution

**FLOAT** (Floating Low-power Observant Aquarium Technology) is an autonomous, edge-only IoT system that monitors and responds to aquarium conditions in real time, without requiring any cloud infrastructure or Internet connectivity.

The system is built around two ESP32 microcontrollers communicating via **ESP-NOW** (a connectionless, low-latency peer-to-peer WiFi protocol):

- **Target node** - deployed at the aquarium: measures water turbidity and temperature, drives the water pump and servo-actuated food dispenser.
- **Observer node** - monitors motor health via current sensing (INA219), runs anomaly detection algorithms, serves a local web dashboard, and issues emergency commands to the Target.

The key design insight is that **pump motor current is a direct proxy for motor health**: a stall causes a current surge (MOTOR_STALL), while running without water causes abnormally low current (DRY_RUN). Both can be detected in milliseconds without any mechanical intervention.

Turbidity and temperature are integrated into the same pipeline: turbidity determines whether to filter or feed, temperature provides both an independent anomaly signal and a calibration factor for the turbidity sensor (since water viscosity varies with temperature, affecting optical readings).



## 3. Why This Is Interesting

The final system directly addresses both points:

- **Multiple anomaly types** are now detected: MOTOR_STALL (over-current), DRY_RUN (under-current) and TEMP_OUT_OF_RANGE (water temperature). Each has a distinct physical cause, a different measurement signature, and a different consequence for the aquarium.
- **Multi-sensor fusion** is implemented: turbidity, temperature and motor current are all used together. Temperature is not a standalone sensor - it compensates the turbidity reading (viscosity correction), contributes to anomaly context, and is learned during calibration to establish a **personalized baseline** for each aquarium.
- **The anomaly detection algorithm was improved**: the classic 3-sigma rule (used in the mid-term version) was replaced by an **EWMA + Hampel filter** combination. The Hampel filter uses Median Absolute Deviation (MAD), which is robust to outliers in the calibration phase - a problem the 3-sigma rule suffers from when motor inrush spikes contaminate the learning window.



## 4. System Architecture Overview

```
┌───────────────────────────────────────────────────────────────────┐
│                         AQUARIUM                                  │
│                                                                   │
│  ┌──────────────────────────────────┐                             │
│  │         TARGET NODE (ESP32)      │                             │
│  │                                  │                             │
│  │  DS18B20 ──► Temperature         │                             │
│  │  Turbidity sensor ──► NTU        │     ESP-NOW (ch 13)         │
│  │  Pump ◄── PWM                    │◄───────────────────────────►│
│  │  Servo ◄── PWM                   │                             │
│  │  Deep sleep 20 s cycle           │                             │
│  └──────────────────────────────────┘                             │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
                                │
                   ESP-NOW (connectionless,
                   no router needed, ~1 ms)
                                │
┌───────────────────────────────────────────────────────────────────┐
│                       OBSERVER NODE (ESP32)                       │
│                                                                   │
│  INA219 ──► Current + Voltage                                     │
│  EWMA + Hampel ──► Anomaly detection                              │
│  WebServer ──► Dashboard (WiFi AP, 192.168.4.1)                   │
│  Buzzer ──► Local alert                                           │
└───────────────────────────────────────────────────────────────────┘
                                │
                   WiFi AP (no Internet)
                                │
                    ┌──────────────────────┐
                    │   Phone / Laptop     │
                    │   Browser            │
                    │   http://192.168.4.1 │
                    └──────────────────────┘
```



## 5. Design Choices and Trade-offs

| Choice | Rationale | Trade-off accepted |
|---|---|---|
| ESP-NOW instead of MQTT/WiFi | No router required; latency < 5 ms vs ~200 ms MQTT | No cloud integration in base version |
| Edge-only anomaly detection | Pump can be stopped within ~1.2 s even if Internet is down | Dashboard requires local WiFi connection |
| Current sensing as proxy for motor health | Indirect but extremely reliable; no mechanical contact needed | Cannot detect partial blockage below the threshold |
| Deep sleep on Target | 91% sleep duty cycle; extends battery life to ~3 days on 2000 mAh | Target not reachable during sleep window |
| Hampel filter instead of 3-sigma | Robust to inrush spikes during calibration | Slightly more computation (insertion sort) |
| DS18B20 for temperature | 1-Wire, cheap, accurate to ±0.5 °C | Occasional -127 °C glitches require software filtering |



## 6. Target Users

Home aquarium owners who want automated, offline protection for their fish without cloud dependency or subscription fees. The system requires no Wi-Fi router - the Observer creates its own access point.



## 7. Scope and Limitations

**In scope:**
- Automated pump anomaly detection and emergency stop
- Turbidity-driven pump activation and food dispensing
- Water temperature monitoring with adaptive baseline
- Local real-time dashboard accessible from any device on the local AP

**Out of scope (acknowledged):**
- pH, dissolved oxygen, ammonia sensors (identified as future extensions)
- Cloud data logging and remote notifications over the Internet
- Multi-tank deployment (MCS extension identified but not implemented)