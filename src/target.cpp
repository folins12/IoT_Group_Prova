/*
 * FLOAT - Target Node
 * ====================
 * Measures turbidity + temperature, drives pump and food servo.
 * Communicates via ESP-NOW (channel 13) with the Observer node.
 * Anomalies detected by Observer: MOTOR_STALL, DRY_RUN, VOLTAGE_DROP,
 *   TEMP_TOO_HIGH, TEMP_TOO_LOW, PERIODIC_STALL, COMMUNICATION_LOSS.
 *
 * Changes vs previous version:
 *   - Temperature guard in monitoring loop: -127 °C (DS18B20 disconnect)
 *     is discarded and replaced with last valid reading
 *   - Turbidity value is randomised on every boot (demo / test mode)
 *   - Clean water action: servo dispenses food AND pump runs for 5 s
 *     with anomaly monitoring (START_MONITOR / STOP_MEASURE)
 *   - ACK / retry with exponential backoff on critical commands
 *   - Turbidity read BEFORE WiFi init (stable ADC, no RF interference)
 *   - RTC counters for recurring anomalies and missed ACKs
 *   - Deep sleep raised to 20 s → duty cycle ~9 % (91 % sleep)
 *   - During the learning phase, DATA:SENSOR messages carry temperature
 *     every 500 ms so the Observer builds ~20 temperature samples over the
 *     10 s learning window. The Observer uses its own counter (temp_sample_idx)
 *     independent from the current sample counter, avoiding index-alignment
 *     bugs that previously caused 0.0f padding and a wrong baseline mean.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Pin map ────────────────────────────────────────────────────────────────
const int PUMP_PIN      = 47;
const int SERVO_PIN     = 6;
const int TURBIDITY_PIN = 1;
const int DS18B20_PIN   = 4;

// ── Turbidity sensor calibration ──────────────────────────────────────────
const int   TURB_CLEAN_ADC     = 3435;
const int   TURB_DIRTY_ADC     = 1200;
const float TURB_MAX_NTU       = 800.0f;
const float TURB_THRESHOLD_NTU = 50.0f;

// ── Deep-sleep period ─────────────────────────────────────────────────────
// 20 s → active ≈ 2 s / 22 s = 9 %  → 91 % sleep ✓
const uint64_t SLEEP_US = 20ULL * 1000000ULL;

// ── RTC-persistent state (survives deep sleep) ─────────────────────────────
RTC_DATA_ATTR int      bootCount         = 0;
RTC_DATA_ATTR bool     system_halted     = false;
RTC_DATA_ATTR uint32_t last_cmd_id       = 0;   // for ACK de-duplication
RTC_DATA_ATTR int      consecutive_noack = 0;   // consecutive failed ACKs
RTC_DATA_ATTR int      anomaly_count     = 0;   // total anomalies for PERIODIC_STALL
RTC_DATA_ATTR float    last_valid_temp   = 20.0f;

// ── Globals ────────────────────────────────────────────────────────────────
uint8_t observerAddress[] = {0xF0, 0x9E, 0x9E, 0x77, 0x73, 0x60};

volatile bool     emergency_stop = false;
volatile bool     ack_received   = false;
volatile uint32_t acked_id       = 0;

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ── ESP-NOW receive callback ───────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    if (s == "HALT") {
        digitalWrite(PUMP_PIN, LOW);
        emergency_stop = true;
        system_halted  = true;
        anomaly_count++;
    } else if (s.startsWith("ACK:")) {
        acked_id     = (uint32_t)s.substring(4).toInt();
        ack_received = true;
    } else if (s.startsWith("CAL:")) {
        // Format: CAL:mean,std,th_stall,th_dry,th_temp_high,th_temp_low
        // Log the calibration result for diagnostics; thresholds are enforced by Observer.
        String p  = s.substring(4);
        int c1 = p.indexOf(','), c2 = p.indexOf(',', c1+1);
        int c3 = p.indexOf(',', c2+1), c4 = p.indexOf(',', c3+1);
        int c5 = p.indexOf(',', c4+1);
        if (c5 != -1) {
            Serial.printf("[TGT] Calibration received — stall:%.1f mA  dry:%.1f mA"
                          "  T↑:%.1f °C  T↓:%.1f °C\n",
                          p.substring(c2+1, c3).toFloat(),
                          p.substring(c3+1, c4).toFloat(),
                          p.substring(c4+1, c5).toFloat(),
                          p.substring(c5+1).toFloat());
        }
    }
}

// ── sendMsg – fire and forget (telemetry, logs) ────────────────────────────
void sendMsg(const String& type, const String& content) {
    String full = type + ":" + content;
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    delay(150);
}

// ── sendMsgWithACK – critical commands with retry + backoff ───────────────
// Appends "|ID:N" to the message; Observer replies "ACK:N".
// Returns true if ACK received within (retries × timeout_ms).
bool sendMsgWithACK(const String& type, const String& content,
                    int retries = 5, int timeout_ms = 300) {
    last_cmd_id++;
    String full = type + ":" + content + "|ID:" + String(last_cmd_id);
    ack_received = false;

    for (int attempt = 0; attempt < retries; attempt++) {
        esp_now_send(observerAddress,
                     (const uint8_t*)full.c_str(), full.length());
        unsigned long t0 = millis();
        while (millis() - t0 < (unsigned long)timeout_ms) {
            if (ack_received && acked_id == last_cmd_id) {
                consecutive_noack = 0;  // reset failure counter
                return true;
            }
            delay(10);
        }
        delay(50 * (attempt + 1));  // exponential backoff
    }

    consecutive_noack++;
    Serial.printf("[TGT] WARNING: no ACK for '%s' after %d retries\n",
                  content.c_str(), retries);
    return false;
}

// ── readTurbidityNTU ───────────────────────────────────────────────────────
// Reads ADC with WiFi OFF to avoid RF interference.
// Applies a viscosity temperature correction.
// Must be called BEFORE WiFi.mode(WIFI_STA) / esp_now_init().
float readTurbidityNTU(float temp_c = 25.0f) {
    long sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += analogRead(TURBIDITY_PIN);
        delay(2);
    }
    int adc = (int)(sum / 64);
    Serial.printf("[SENSOR] Turbidity ADC (raw): %d\n", adc);

    adc = constrain(adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);

    float ntu_raw = TURB_MAX_NTU *
                    (float)(TURB_CLEAN_ADC - adc) /
                    (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);

    // Temperature correction: water viscosity decreases with temperature,
    // making particles settle faster → apparently lower NTU at high T.
    // Linear correction ±0.5 %/°C referenced to 25 °C.
    float temp_correction = 1.0f + 0.005f * (temp_c - 25.0f);
    float ntu = ntu_raw / temp_correction;

    return constrain(ntu, 0.0f, TURB_MAX_NTU);
}

// ── readTurbidityRandom ────────────────────────────────────────────────────
// Returns a random NTU value for demo / testing purposes.
// Alternates between a "dirty" range and a "clean" range each boot.
float readTurbidityRandom() {
    // Seed with a mix of boot count and a free-running timer
    randomSeed(esp_random());
    // Alternate boots: even → dirty water, odd → clean water
    float ntu;
    if (bootCount % 2 == 0) {
        // Dirty: 150–2500 NTU (above threshold → pump ON)
        ntu = random(51, 800);
    } else {
        // Clean: 10–80 NTU (below threshold → servo + pump)
        ntu = random(0, 50);
    }
    Serial.printf("[SENSOR] Turbidity (random demo): %.1f NTU\n", ntu);
    return ntu;
}

// ── servoMove – software PWM ──────────────────────────────────────────────
void servoMove(int us, int pulses) {
    for (int i = 0; i < pulses; i++) {
        digitalWrite(SERVO_PIN, HIGH);
        delayMicroseconds(us);
        digitalWrite(SERVO_PIN, LOW);
        delay(18);
    }
}

// ── goToSleep – single deep-sleep entry point ─────────────────────────────
void goToSleep(uint64_t duration_us = SLEEP_US) {
    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep " +
            String((unsigned long)(duration_us / 1000000UL)) + " s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(duration_us);
    esp_deep_sleep_start();
}

// ── safeTemp – validate DS18B20 reading ───────────────────────────────────
// Returns the reading if valid; otherwise keeps last_valid_temp unchanged
// and returns it. -127 °C is the DS18B20 disconnect sentinel.
float safeTemp(float raw) {
    if (raw == DEVICE_DISCONNECTED_C || raw < -10.0f) {
        Serial.printf("[WARN] Invalid temperature %.1f C — using last valid: %.1f C\n", raw, last_valid_temp);
        return last_valid_temp;
    }
    last_valid_temp = raw;
    return raw;
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    pinMode(PUMP_PIN,  OUTPUT);
    pinMode(SERVO_PIN, OUTPUT);
    analogReadResolution(12);

    digitalWrite(PUMP_PIN,  LOW);
    digitalWrite(SERVO_PIN, LOW);

    // ── System halted by a critical anomaly ───────────────────────────────
    if (system_halted) {
        Serial.println("[TGT] System halted — staying in deep sleep.");
        esp_sleep_enable_timer_wakeup(SLEEP_US);
        esp_deep_sleep_start();
    }

    // ── DS18B20 reading (before WiFi to avoid losing conversion window) ───
    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);  // wait for 11-bit conversion

    float water_temp = safeTemp(tempSensor.getTempCByIndex(0));

    // ── Turbidity reading with WiFi OFF ───────────────────────────────────
    // ADC is sensitive to RF noise; read before enabling the radio.
    // NOTE: using randomised value for demo purposes (see readTurbidityRandom)
    float turbidity_ntu = readTurbidityRandom();
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);

    // ── WiFi + ESP-NOW init ───────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 13;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    } else {
        Serial.println("[TGT] ESP-NOW init failed — going to sleep");
        goToSleep();
    }

    delay(2000);  // allow ESP-NOW link to stabilise

    // ── Send initial data (fire and forget) ───────────────────────────────
    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) +
            " NTU  (" + String(turbidity_pct) + "%)");
    sendMsg("LOG", "[SENSOR] Water temp: " + String(water_temp, 1) + " C");
    sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) +
            "," + String(water_temp, 1));

    // ── PERIODIC_STALL detection ──────────────────────────────────────────
    if (anomaly_count >= 3) {
        sendMsg("LOG", "[WARN] PERIODIC_STALL: recurring anomaly (" +
                String(anomaly_count) + " events). Possible partial obstruction.");
        sendMsg("CMD", "PERIODIC_STALL");
    }

    // ── COMMUNICATION_LOSS detection ──────────────────────────────────────
    if (consecutive_noack >= 3) {
        sendMsg("LOG", "[CRITICAL] COMMUNICATION_LOSS: Observer unreachable. Safe mode.");
        goToSleep(60ULL * 1000000ULL);  // long sleep, reduce power without hard-halting
    }

    // ══════════════════════════════════════════════════════════════════════
    // MAIN LOGIC
    // ══════════════════════════════════════════════════════════════════════

    if (bootCount == 0) {
        // ── First boot: calibration learning phase ────────────────────────
        sendMsg("LOG", "[ACTION] First boot → calibration learning phase");

        bool ok = sendMsgWithACK("CMD", "START_LEARN");
        if (!ok) sendMsg("LOG", "[WARN] Observer did not confirm START_LEARN");
        delay(300);

        // Pump ON for 10 s, send live telemetry every 300 ms
        // (matches Observer loop rate so sample counts stay in sync)
        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0          = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 300) {
                last_update = millis();
                tempSensor.requestTemperatures();
                float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
                float live_turb = readTurbidityRandom();
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu > TURB_THRESHOLD_NTU) {
        // ── Dirty water: pump ON with anomaly monitoring ──────────────────
        sendMsg("LOG", "[ACTION] Turbidity=" + String(turbidity_ntu, 1) +
                " NTU > threshold → pump ON (10 s)");

        bool ok = sendMsgWithACK("CMD", "START_MONITOR");
        if (!ok) sendMsg("LOG", "[WARN] Observer did not confirm START_MONITOR");
        delay(300);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0          = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 300) {
                last_update = millis();
                tempSensor.requestTemperatures();
                // Guard -127: use last valid temperature if probe glitches
                float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
                float live_turb = readTurbidityRandom();
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else {
        // ── Clean water: dispense food with servo, then run pump briefly ──
        sendMsg("LOG", "[ACTION] Water clean (" + String(turbidity_ntu, 1) + " NTU) → dispensing food + short pump cycle");

        // Servo: one full rotation (open → close)
        sendMsg("LOG", "[SERVO] Open 90° — dispensing");
        servoMove(1500, 35);   // ~630 ms at 18 ms/pulse × 35 pulses
        delay(500);
        sendMsg("LOG", "[SERVO] Close 0°");
        servoMove(1000, 35);
        delay(500);

        // Pump ON for 5 s (short circulation after feeding)
        sendMsg("LOG", "[PUMP] Short pump cycle (5 s) after feeding");

        bool ok = sendMsgWithACK("CMD", "START_MONITOR");
        if (!ok) sendMsg("LOG", "[WARN] Observer did not confirm START_MONITOR");
        delay(5);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0          = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 5000 && !emergency_stop) {
            if (millis() - last_update >= 300) {
                last_update = millis();
                tempSensor.requestTemperatures();
                float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
                float live_turb = readTurbidityRandom();
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);
        sendMsgWithACK("CMD", "STOP_MEASURE");
        sendMsg("LOG", "[PUMP] Short cycle done");
    }

    // ── End of cycle: deep sleep ───────────────────────────────────────────
    goToSleep();
}

void loop() {}