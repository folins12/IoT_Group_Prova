# FLOAT
uint8_t observerAddress[] = {0xF0, 0x9E, 0x9E, 0x77, 0x73, 0x60};
uint8_t targetAddress[] = {0x48, 0x27, 0xE2, 0xE2, 0xE3, 0x0C};
**Framework for Local Observation of Aquatic Tanks**

FLOAT is an Edge IoT system designed to monitor and control aquatic tanks using low-power embedded devices.
The system performs local monitoring, anomaly detection and actuator control without relying on cloud infrastructure.

The goal of the project is to demonstrate how **edge devices can autonomously detect abnormal behaviors (e.g., pump stall)** and react in real-time while minimizing energy consumption.

---

# System Architecture

The system is composed of two ESP32 nodes communicating through **ESP-NOW**, a low-power connectionless wireless protocol.

### Target Node

The Target Node is responsible for:

• Simulating turbidity sensing
• Controlling the **water pump**
• Controlling the **feeding servo motor**
• Sending system logs and commands to the Observer Node

### Observer Node

The Observer Node performs monitoring and analysis:

• Measures current consumption using an **INA219 power sensor**
• Learns the normal behavior of the pump
• Detects anomalies using a **statistical 3-Sigma rule**
• Sends an **emergency HALT command** if abnormal current is detected

Communication between the nodes is implemented using **ESP-NOW**, allowing direct device-to-device communication without WiFi infrastructure.

---

# Edge AI Monitoring

The system implements a lightweight **statistical anomaly detection algorithm at the edge**.

During the **learning phase**, the Observer Node collects samples of the pump current consumption.

The following values are computed:

μ = mean current
σ = standard deviation

The anomaly threshold is then calculated as:

Threshold = μ + 3σ

If the current exceeds the threshold for **three consecutive samples**, the system identifies a **mechanical stall** and immediately stops the pump.

This approach enables **real-time fault detection without cloud processing**.

---

# Requirements and Metrics

The system has been designed to satisfy the following requirements.

### Real-time Safety Reaction

Requirement
Motor cutoff time < **2000 ms**

Metric
Measured reaction time ≈ **1200 ms**

Explanation
Three consecutive samples are collected every **400 ms**, producing a maximum detection latency of approximately **1.2 seconds**.

---

### Power Monitoring Reliability

Requirement
Prevent false positives while detecting mechanical stalls.

Metric
Statistical threshold based on **3-Sigma rule (μ + 3σ)**.

Explanation
This method adapts the detection threshold to the real operating conditions of the pump.

---

### Low Power Operation

Requirement
Maximize battery lifetime.

Metric

Duty Cycle:

10 seconds active
10 seconds deep sleep

Additional energy optimizations:

• WiFi TX power reduction
• Deep Sleep mode
• Brownout detector protection

---

# Communication

The system uses **ESP-NOW**, which offers:

• Connectionless communication
• Very low latency
• Low energy consumption

Two message types are exchanged between nodes:

LOG → status information
CMD → system commands (START_LEARN, START_MONITOR, STOP_MEASURE, HALT)

---

# Experiments

Several experiments were performed to validate the system behavior.

### Experiment 1 – Pump Current Learning

Goal
Determine the normal operating current of the pump.

Procedure
The pump runs for **10 seconds**, while the Observer Node collects current samples using the INA219 sensor.

Result
The system computes the mean current and standard deviation.

Outcome
The dynamic anomaly threshold is generated automatically using the **3-Sigma rule**.

---

### Experiment 2 – Stall Detection

Goal
Verify that the system can detect abnormal motor behavior.

Procedure
Artificial high current conditions are simulated.

Result
The Observer Node detects three consecutive anomalies and sends a **HALT command**.

Measured Reaction Time
≈ **1200 ms**

Outcome
The system successfully stops the pump within the required time.

---

### Experiment 3 – Energy Efficiency

Goal
Verify the energy optimization strategy.

Procedure
The device alternates between active operation and **Deep Sleep mode**.

Configuration

10 seconds active
10 seconds sleep

Outcome
The duty cycle significantly reduces the average power consumption.

---

# Demo Video

Project demonstration video: https://www.youtube.com/watch?v=Wky8BkITGp4

The video shows:

• system architecture
• learning phase
• anomaly detection
• automatic pump shutdown

---

# Future Work

Several improvements are planned for the final delivery.

• Integration of a **real turbidity sensor**
• Implementation of **IoT security mechanisms**
• Improved power optimization strategies
• Extended testing with real aquatic environments

---

# Team Members

Michele Libriani, 1954541 - www.linkedin.com/in/michele-libriani-805985316
Andrea Folino, 1986019 - www.linkedin.com/in/andrea-folino-981aa5322
Edoardo Zompanti, 1985499 - www.linkedin.com/in/edoardo-zompanti-a8678a3b4