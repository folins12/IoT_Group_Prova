# FLOAT - Concept Document 

## 1. Problem Statement

Home and small-scale aquariums require continuous maintenance to keep fish alive and healthy. A few failure modes are responsible for the majority of fish deaths in unattended tanks:

1. **Pump failure.** The circulation pump can mechanically **stall** (blocked impeller, foreign object) or **run dry** (water level too low). Both conditions overheat and destroy the motor within seconds to minutes, and leave the tank without oxygenation or filtration.
2. **Water-quality degradation.** Turbidity rises over time due to organic waste, uneaten food and algae. High turbidity signals a deteriorating chemical and biological environment that, if left unaddressed, leads to bacterial blooms and fatal oxygen depletion.
3. **Temperature excursions.** Water outside the species-specific safe range stresses fish even when the water looks visually clean.

Existing consumer solutions are either purely reactive (manual checks), expensive (professional monitoring rigs), or **fully cloud-dependent** (commercial smart feeders that stop working the moment the Internet drops). There is no low-cost system at the hobbyist price point that combines **motor-health monitoring** with **environmental sensing** while keeping the safety-critical decisions running locally, independent of any router or cloud.

## 2. Proposed Solution

**FLOAT** is an autonomous IoT system that monitors and reacts to aquarium conditions in real time. It is organised in **two tiers** with very different reliability requirements:

- **Edge safety tier - always autonomous.** Two ESP32 microcontrollers and one Arduino Uno cooperate at the tank to sense conditions and protect the pump. They communicate over **ESP-NOW**, a connectionless, encrypted, peer-to-peer link that needs no router, no DHCP and no IP stack. This tier detects a pump anomaly and cuts power to the pump in roughly **1.25 s**, and it keeps working with no WiFi infrastructure and no Internet.
- **Monitoring & control tier - optional, connectivity-enabled.** The Observer additionally bridges the system to a **local web dashboard**, to an **MQTT cloud broker** (HiveMQ Cloud, over authenticated TLS), and to **e-mail alerts**. This tier provides remote visibility, a device shadow (last-known state), remote commands and notifications when connectivity is available - but it is never on the safety path.

The roles of the three devices are:

- **Target node (ESP32-S3).** Deployed at the aquarium. Reads water **temperature** (DS18B20) and drives the **pump** and the **servo food dispenser**. Reads water **turbidity** from a dedicated **Arduino Uno** over a serial link. Spends most of its time in deep sleep.
- **Arduino Uno.** Hosts the analog **turbidity sensor**, sleeps on its watchdog and answers the Target on request. Keeping the analog acquisition on a separate microcontroller isolates it from the ESP32 radio noise.
- **Observer node (ESP32-S3).** Measures the pump **supply current and bus voltage** with an INA219, runs the anomaly-detection and predictive-maintenance algorithms, serves the dashboard, talks to the cloud, and issues emergency commands to the Target. A **second INA219** measures the auxiliary (Arduino Uno) supply so the system can report **total power consumption** and warn on a depleted auxiliary battery.

The key design insight is that **pump motor current is a direct proxy for motor health**: a stall causes a current surge (`MOTOR_STALL`), while running without water causes abnormally low current (`DRY_RUN`). Both are detectable in milliseconds, with no mechanical intervention and no extra sensor on the motor.

## 3. Core Ideas - Why This Is Interesting

- **Several distinct anomaly types**, each with a different physical cause, measurement signature and consequence:
  - `MOTOR_STALL` (over-current) and `DRY_RUN` (under-current) are **safety-critical** → the pump is halted immediately.
  - `VOLTAGE_DROP`, `TEMP_TOO_HIGH`, `TEMP_TOO_LOW` are **advisory** → the user is warned, but the pump keeps circulating water (stopping it would make things worse).
  - `DEGRADATION` is a **predictive-maintenance** signal raised before any hard failure.
- **Multi-sensor fusion.** Turbidity, temperature and motor current are used together. Temperature is not a standalone reading: it **compensates the turbidity measurement** (optical readings drift with water viscosity, which depends on temperature) and it has its **own adaptive baseline**, learned per-tank during calibration.
- **Robust detection.** The detector combines an **EWMA** smoother with a **Hampel filter** (median + Median Absolute Deviation) during calibration, so motor inrush spikes captured while learning do not inflate the thresholds.
- **Predictive maintenance.** A one-sided **CUSUM** accumulates the per-cycle healthy current versus the learned baseline and raises a service warning when the pump's draw creeps up consistently - catching slow degradation before it becomes a stall.
- **Security by design.** ESP-NOW frames are **AES-CCM encrypted**; the MQTT cloud bridge runs over **TLS with broker-certificate verification** and username/password authentication.
- **Quantitative evaluation.** A built-in **labelled confusion-matrix harness** (six classes) records detection outcomes and latency on the device itself, exposing accuracy, false-positive rate, false-negative rate, per-class recall and mean reaction time.
- **Two-tier resilience.** The fish stay protected even with no network at all; connectivity only adds remote monitoring and control.

## 4. System Architecture Overview

```
                              AQUARIUM
   ┌───────────────────────────────────────────────────────────────┐
   │                                                               │
   │   ┌──────────────────────┐        UART 'T'/'E'                │
   │   │  ARDUINO UNO         │◄──────── handshake ──────┐         │
   │   │  • turbidity sensor  │                          │         │
   │   │  • watchdog sleep 4s │                          │         │
   │   └──────────────────────┘                          │         │
   │                                ┌────────────────────▼───────┐ │
   │                                │     TARGET  (ESP32-S3)     │ │
   │                                │  • DS18B20 temperature     │ │
   │                                │  • pump  (GPIO47)          │ │
   │                                │  • servo (GPIO6)           │ │
   │                                │  • deep sleep 20 s / cycle │ │
   │                                └──────────────┬─────────────┘ │
   │                                               │               │
   └───────────────────────────────────────────────│───────────────┘
                                                   │
                                                ESP-NOW 
                              (AES-CCM encrypted, connectionless, no router)
                                                   │
                            ┌──────────────────────▼──────────────────────┐
                            │            OBSERVER  (ESP32-S3)             │
                            │  • INA219 #1 → pump current + voltage       │
                            │  • INA219 #2 → arduino uno                  │
                            │  • EWMA + Hampel → anomaly detection        │
                            │  • CUSUM → predictive maintenance           │
                            │  • buzzer → local alarm                     │
                            │  • WiFi STA + Web dashboard + MQTT + e-mail │
                            └───────────────────────┬─────────────────────┘
                                                    │  WiFi (router / hotspot)
              ┌─────────────────────────────────────┼───────────────────────────────┐
              │                                     │                               │
     ┌────────▼────────┐                  ┌─────────▼──────────┐          ┌─────────▼────────┐
     │ Local dashboard │                  │  MQTT cloud broker │          │  E-mail alerts   │
     │ (browser, LAN)  │                  │  HiveMQ Cloud, TLS │          │  (Apps Script)   │
     │ live chart +    │                  │  telemetry/state/  │          │   Mobile app     │
     │ controls + eval │                  │  alert/cmd topics  │          │ (IoT MQTT Panel) │
     └─────────────────┘                  └────────────────────┘          └──────────────────┘
                          
```

The **ESP-NOW safety loop is independent** of the WiFi/router/cloud blocks.

## 5. Design Choices and Trade-offs

| Choice | Rationale | Trade-off accepted |
|---|---|---|
| ESP-NOW for the Target↔Observer link | No router; sub-5 ms latency; no DHCP brownouts | Both nodes must share one radio channel |
| Edge-only safety decisions | Pump stops in ~1.25 s even with no Internet | Remote features require connectivity |
| Cloud layer over MQTT (separate from safety) | Remote dashboard, device shadow, alerts | Adds a broker/network dependency for *non-critical* features only |
| Current sensing as motor-health proxy | Reliable, no mechanical contact, ms-scale | Cannot resolve a partial blockage below threshold |
| Dedicated Arduino Uno for turbidity | Isolates the analog reading from ESP32 RF noise | Extra board + a serial handshake to maintain |
| Hampel + EWMA detector | Robust to inrush spikes; tight thresholds | Slightly more computation than a plain mean+3σ |
| Adaptive (learned) temperature band | Personalised per tank, no manual config | Requires a clean calibration window |
| Deep sleep on the Target | Cuts idle draw between cycles | Target unreachable during the sleep window |
| AES-CCM on ESP-NOW + TLS on MQTT | Confidentiality + authenticated cloud | Keys/certificate must be provisioned on both ends |

## 6. Target Users

Home and hobbyist aquarium owners who want automated, offline-first protection for their fish — without a subscription, without trusting a vendor cloud for safety, and with the option of remote monitoring when they want it. The safety features need no router; the remote features use whatever WiFi is already available.

## 7. Scope and Limitations

**In scope (delivered):**
- Automated pump anomaly detection (stall / dry-run) with emergency stop.
- Advisory warnings for voltage drop and temperature out-of-range.
- Predictive-maintenance degradation warning (CUSUM).
- Turbidity-driven pump activation and servo food dispensing.
- Adaptive, per-tank temperature baseline; temperature-compensated turbidity.
- Total power-consumption monitoring (two INA219) and auxiliary-battery low warning.
- Local real-time dashboard; MQTT cloud bridge with device shadow, remote commands and alerts; e-mail notifications.
- Encrypted ESP-NOW and authenticated TLS cloud.
- On-device labelled-confusion-matrix evaluation harness.

**Out of scope:**
- **LoRaWAN long-range uplink.** The hardware (Heltec boards) carries an unused LoRa radio; a LoRaWAN variant for a remote/outdoor tank is the most natural next step, but it was not required for an indoor, WiFi-covered deployment.
- **Additional water-chemistry sensors** (pH, dissolved oxygen, ammonia) - identified as future extensions.
- **Multi-tank / fleet deployment** - the architecture would extend to it, but it was not implemented.
