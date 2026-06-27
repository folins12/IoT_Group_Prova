# FLOAT

**Framework for Local Observation of Aquatic Tanks**

| | |
|---|---|
| **Team** | Michele Libriani (1954541) · Andrea Folino (1986019) · Edoardo Zompanti (1985499) |
| **Course** | Internet of Things - Sapienza University of Rome |
| **Repository** | https://github.com/erdod/FLOAT |
| **Demo / video** | `<INSERT YOUTUBE LINK>` |

FLOAT is an edge-first IoT system that monitors and manages aquariums with low-power embedded devices. Its safety-critical loop detects pump motor anomalies and shuts the pump down in under 2 seconds, with no router and no Internet - while an optional cloud layer adds remote monitoring, control and alerts.

ARCHITECTURE
The system is built around three devices:
1) Target Node (ESP32-S3) - at the tank: reads water temperature (DS18B20), drives the pump and the servo food dispenser, and reads water turbidity from a dedicated Arduino Uno. Deep-sleeps between cycles.
2) Arduino Uno - hosts the real analog turbidity sensor and answers the Target over a UART handshake, isolating the analog reading from radio noise.
3) Observer Node (ESP32-S3) - measures pump current and bus voltage with an INA219, runs the anomaly-detection and predictive-maintenance algorithms, and acts as the gateway to the dashboard and cloud. A second INA219 measures the auxiliary supply for total power consumption.

Communication between the two ESP32 nodes uses ESP-NOW: connectionless, low-power, and AES-CCM encrypted.

TECHNICAL FEATURES
- Edge-to-edge ESP-NOW link (encrypted, no router needed)
- Anomaly detection: motor stall (μ + 3σ on an EWMA-smoothed signal), dry-run, voltage drop, and adaptive (learned) temperature limits
- Robust calibration with a Hampel filter (Median Absolute Deviation) to exclude motor inrush current
- CUSUM predictive maintenance: catches slow pump degradation before a hard failure
- Real-time current and total-power monitoring (two INA219 sensors)
- Emergency motor cutoff on a confirmed stall or dry-run
- MQTT cloud bridge over TLS with a retained device shadow, plus a local web dashboard and email alerts
- Deep-sleep duty cycling on the Target node for energy efficiency

PERFORMANCE METRICS
- Motor cutoff reaction time: ~1250 ms (requirement: less than 2000 ms)
- False-positive rate: 0% over the measured normal cycles, evaluated with a labelled 6-class confusion matrix
- Target node deep-sleeps 20 s between cycles

FUTURE WORK
- LoRaWAN uplink for remote / off-grid tanks
- HTTPS on the local dashboard
- Additional water-chemistry sensors (pH, dissolved oxygen, ammonia)
- Multi-tank deployment and a complete, fully-counted confusion matrix

Built as part of the IoT course at Sapienza University of Rome.

GitHub Repository: https://github.com/erdod/FLOAT