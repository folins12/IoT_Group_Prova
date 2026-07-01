// FLOAT - Target Node 

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// NVS-backed state 
Preferences prefs;
bool autoCycleEnabled = true;
volatile bool restart_requested = false;   

// Manual controls 
volatile bool          manualPumpOn       = false;
volatile unsigned long manualPumpOffAt    = 0;      
volatile bool          manualFeedNow      = false;
volatile unsigned long feedIntervalMs     = 0;      
volatile unsigned long nextFeedAt         = 0;
volatile bool          manualCalibrateReq = false;
float                  lastTurbidityNtu   = -1.0f;

// Pins
const int PUMP_PIN      = 47;
const int SERVO_PIN     = 6;
const int DS18B20_PIN   = 7;

// Link to Arduino Uno 
const int TURB_RXD = 4;
const int TURB_TXD = 5;

// Turbidity 
const int   TURB_CLEAN_ADC     = 750;
const int   TURB_DIRTY_ADC     = 10;
const float TURB_MAX_NTU       = 800.0f;
const float TURB_THRESHOLD_NTU = 50.0f;

// Deep-sleep 
const uint64_t SLEEP_US = 20ULL * 1000000ULL;

// RTC-persistent state 
RTC_DATA_ATTR int      bootCount         = 0;
RTC_DATA_ATTR bool     system_halted     = false;
RTC_DATA_ATTR uint32_t last_cmd_id       = 0;
RTC_DATA_ATTR int      consecutive_noack = 0;
RTC_DATA_ATTR int      anomaly_count     = 0;
RTC_DATA_ATTR float    last_valid_temp   = 20.0f;

// ESP-NOW + AES-CCM keys 
uint8_t observerAddress[] = {0xF0, 0x9E, 0x9E, 0x77, 0x73, 0x60};
static const uint8_t ESPNOW_PMK[16] = {'F','L','O','A','T','-','p','m','k','-','v','9','-','a','q','1'};
static const uint8_t ESPNOW_LMK[16] = {'F','L','O','A','T','-','l','m','k','-','v','9','-','a','q','1'};

volatile bool     emergency_stop = false;
volatile bool     ack_received   = false;
volatile uint32_t acked_id       = 0;
const char*       haltCause      = nullptr;   

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ESP-NOW receive callback 
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
        manualPumpOn    = false;
        manualPumpOffAt = 0;
        prefs.putBool("halted", true);
    } else if (s == "CMD:CLEAR_HALT") {
        system_halted   = false;
        emergency_stop  = false;
        anomaly_count   = 0;
        manualPumpOn    = false;
        manualPumpOffAt = 0;
        manualFeedNow   = false;
        feedIntervalMs  = 0;
        nextFeedAt      = 0;
        digitalWrite(PUMP_PIN, LOW);
        prefs.putBool("halted", false);
        prefs.putInt("storm", 0);
        prefs.putBool("pumping", false);
        if (autoCycleEnabled) {
            bootCount         = 0;
            consecutive_noack = 0;
            restart_requested = true;
            Serial.println("[TGT] HALT cleared by user - restarting fresh (auto)");
        } else {
            Serial.println("[TGT] HALT cleared by user - back to idle (manual)");
        }
    } else if (s == "CMD:AUTO_OFF") {
        if (autoCycleEnabled) {
            autoCycleEnabled = false;
            prefs.putBool("auto", false);
            Serial.println("[TGT] Default mode -> OFF (idle next wake)");
        }
    } else if (s == "CMD:PUMP_ON") {
        if (!system_halted) {
            manualPumpOn    = true;
            manualPumpOffAt = 0;
            digitalWrite(PUMP_PIN, HIGH);
        }
    } else if (s.startsWith("CMD:PUMP_ON:")) {
        if (!system_halted) {
            long sec = s.substring(12).toInt();
            manualPumpOn    = true;
            manualPumpOffAt = (sec > 0) ? (millis() + (unsigned long)sec * 1000UL) : 0;
            digitalWrite(PUMP_PIN, HIGH);
        }
    } else if (s == "CMD:PUMP_OFF") {
        manualPumpOn    = false;
        manualPumpOffAt = 0;
        digitalWrite(PUMP_PIN, LOW);
    } else if (s == "CMD:FEED_STOP") {
        manualFeedNow  = false;
        feedIntervalMs = 0;
        nextFeedAt     = 0;
        Serial.println("[TGT] Feeding stopped");
    } else if (s.startsWith("CMD:FEED")) {
        long sec = 0;
        int  c   = s.indexOf(':', 4);
        if (c != -1) sec = s.substring(c + 1).toInt();
        feedIntervalMs = (sec > 0) ? (unsigned long)sec * 1000UL : 0;
        manualFeedNow  = true;
    } else if (s == "CMD:CALIBRATE") {
        manualCalibrateReq = true;
    } else if (s == "CMD:AUTO_ON") {
        if (!autoCycleEnabled) {
            autoCycleEnabled = true;
            prefs.putBool("auto", true);
            Serial.println("[TGT] Default mode -> ON");
        }
    } else if (s == "CMD:AUTO_ON_RESET") {
        if (!autoCycleEnabled && !restart_requested) {
            autoCycleEnabled  = true;
            bootCount         = 0;
            system_halted     = false;
            anomaly_count     = 0;
            consecutive_noack = 0;
            manualPumpOn       = false;
            manualPumpOffAt    = 0;
            manualFeedNow      = false;
            feedIntervalMs     = 0;
            nextFeedAt         = 0;
            manualCalibrateReq = false;
            digitalWrite(PUMP_PIN,  LOW);
            digitalWrite(SERVO_PIN, LOW);
            prefs.putBool("auto",   true);
            prefs.putBool("halted", false);
            prefs.putInt("storm", 0);
            prefs.putBool("pumping", false);
            restart_requested = true;
            Serial.println("[TGT] Default mode -> ON + RESET (restart pending)");
        }
    } else if (s == "CMD:AUTO_ON_KEEP") {
        if (!autoCycleEnabled && !restart_requested) {
            autoCycleEnabled  = true;
            bootCount         = 1;
            system_halted     = false;
            anomaly_count     = 0;
            consecutive_noack = 0;
            manualPumpOn       = false;
            manualPumpOffAt    = 0;
            manualFeedNow      = false;
            feedIntervalMs     = 0;
            nextFeedAt         = 0;
            manualCalibrateReq = false;
            digitalWrite(PUMP_PIN,  LOW);
            digitalWrite(SERVO_PIN, LOW);
            prefs.putBool("auto",   true);
            prefs.putBool("halted", false);
            prefs.putInt("storm", 0);
            prefs.putBool("pumping", false);
            restart_requested = true;
            Serial.println("[TGT] Default mode -> ON, keeping manual calibration (boot 1)");
        }
    } else if (s.startsWith("ACK:")) {
        acked_id     = (uint32_t)s.substring(4).toInt();
        ack_received = true;
    } else if (s.startsWith("CAL:")) {
        String p  = s.substring(4);
        int c1 = p.indexOf(','), c2 = p.indexOf(',', c1+1);
        int c3 = p.indexOf(',', c2+1), c4 = p.indexOf(',', c3+1);
        int c5 = p.indexOf(',', c4+1);
        if (c5 != -1) {
            Serial.printf("[TGT] Calibration received - stall:%.1f mA  dry:%.1f mA  Tup:%.1f C  Tdn:%.1f C\n",
                          p.substring(c2+1, c3).toFloat(),
                          p.substring(c3+1, c4).toFloat(),
                          p.substring(c4+1, c5).toFloat(),
                          p.substring(c5+1).toFloat());
        }
    }
}

// Fire-and-forget message 
void sendMsg(const String& type, const String& content) {
    String full = type + ":" + content;
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    delay(150);
}

// Critical message with retry + ACK
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
                consecutive_noack = 0;
                return true;
            }
            delay(10);
        }
        delay(50 * (attempt + 1));  
    }

    consecutive_noack++;
    Serial.printf("[TGT] WARNING: no ACK for '%s' after %d retries\n",
                  content.c_str(), retries);
    return false;
}

// Turbidity 
const unsigned long MAX_TURB_WAIT_MS = 4500;

float readTurbidityFromArduino(float temp_c = 25.0f) {
    Serial1.begin(9600, SERIAL_8N1, TURB_RXD, TURB_TXD);
    delay(100);
    sendMsg("LOG", "[TGT] Starting reading turbidity - sent 'T'");
    int   raw_adc   = 0;
    bool  got_value = false;
    unsigned long search_start = millis();

    while (millis() - search_start < MAX_TURB_WAIT_MS) {
        Serial1.print('T');

        unsigned long wait_start = millis();
        while (millis() - wait_start < 150) {
            if (Serial1.available()) {
                String line = Serial1.readStringUntil('\n');
                line.trim();
                int val = line.toInt();
                if (line.length() > 0 && val > 0) {
                    raw_adc   = val;
                    got_value = true;
                    break;
                }
            }
        }
        if (got_value) break;
    }

    if (!got_value) {
        Serial.println("[SENSOR] WARNING: Arduino Uno did not respond - turbidity = 0 NTU (safe default)");
        Serial1.end();
        return 0.0f;
    }

    Serial1.print('E');
    Serial1.println(raw_adc);
    Serial1.flush();
    Serial1.end();

    Serial.printf("[SENSOR] Turbidity ADC from Arduino: %d\n", raw_adc);

    int   adc_clamped = constrain(raw_adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);
    float ntu_raw = TURB_MAX_NTU *
                (float)(adc_clamped - TURB_DIRTY_ADC) /
                (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);
    float correction  = 1.0f + 0.005f * (temp_c - 25.0f);
    return constrain(ntu_raw / correction, 0.0f, TURB_MAX_NTU);
}

// Servo
void servoMove(int us, int pulses) {
    for (int i = 0; i < pulses; i++) {
        digitalWrite(SERVO_PIN, HIGH);
        delayMicroseconds(us);
        digitalWrite(SERVO_PIN, LOW);
        delay(18);
    }
}

// Deep-sleep 
void goToSleep(uint64_t duration_us = SLEEP_US) {
    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep " +
            String((unsigned long)(duration_us / 1000000UL)) + " s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(duration_us);
    esp_deep_sleep_start();
}

// Temperature 
float safeTemp(float raw) {
    if (raw == DEVICE_DISCONNECTED_C || raw < -10.0f) {
        Serial.printf("[WARN] Invalid temperature %.1f C - using last valid: %.1f C\n", raw, last_valid_temp);
        return last_valid_temp;
    }
    last_valid_temp = raw;
    return raw;
}

// WiFi
const char* WIFI_SSID = "Pixel_3478";

uint8_t findChannel() {
    for (int attempt = 0; attempt < 5; attempt++) {
        Serial.printf("[TGT] Scanning for '%s' (try %d/5)...\n", WIFI_SSID, attempt + 1);
        int n = WiFi.scanNetworks(false, false);
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == WIFI_SSID) {
                uint8_t ch = WiFi.channel(i);
                Serial.printf("[TGT] Found '%s' on channel %d\n", WIFI_SSID, ch);
                WiFi.scanDelete();
                return ch;
            }
        }
        WiFi.scanDelete();
        delay(400);
    }
    Serial.println("[TGT] Network not found - fallback channel 13");
    return 13;
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   

    Serial.begin(115200);
    pinMode(PUMP_PIN,  OUTPUT);
    pinMode(SERVO_PIN, OUTPUT);
    analogReadResolution(12);

    digitalWrite(PUMP_PIN,  LOW);
    digitalWrite(SERVO_PIN, LOW);

    prefs.begin("float", false);
    if (!system_halted) system_halted = prefs.getBool("halted", false);
    autoCycleEnabled = prefs.getBool("auto", true);

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        bool wasPumping = prefs.getBool("pumping", false);
        prefs.putBool("pumping", false);
        if (wasPumping) {
            int storm = prefs.getInt("storm", 0) + 1;
            prefs.putInt("storm", storm);
            if (storm > 5) {
                system_halted = true;
                prefs.putBool("halted", true);
                haltCause = "reset loop under pump load - check power supply";
                Serial.printf("[TGT] Pump-load reset storm=%d - halting for safety\n", storm);
            } else {
                if (system_halted) { system_halted = false; prefs.putBool("halted", false); }
                Serial.printf("[TGT] Pump-load reset (storm=%d) - retrying fresh\n", storm);
            }
        } else if (system_halted) {
            system_halted = false;
            prefs.putBool("halted", false);
            Serial.println("[TGT] Power-on (idle) - cleared stale halt, starting fresh");
        }
    }

    // Temperature (before WiFi)
    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);
    float water_temp = safeTemp(tempSensor.getTempCByIndex(0));

    // Turbidity (before WiFi)
    float turbidity_ntu = readTurbidityFromArduino(water_temp);
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);
    lastTurbidityNtu    = turbidity_ntu;

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    uint8_t obs_channel = findChannel();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(obs_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_set_pmk(ESPNOW_PMK);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 0;                 
        memcpy(peer.lmk, ESPNOW_LMK, 16);
        peer.encrypt = true;
        esp_now_add_peer(&peer);
    } else {
        Serial.println("[TGT] ESP-NOW init failed - going to sleep");
        goToSleep();
    }

    delay(2000);

    // Initial data 
    sendMsg("LOG", "------------------------------------");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) +
            " NTU  (" + String(turbidity_pct) + "%)  [Arduino Uno sensor]");
    sendMsg("LOG", "[SENSOR] Water temp: " + String(water_temp, 1) + " C");
    sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) +
            "," + String(water_temp, 1));

    // PERIODIC_STALL
    if (anomaly_count >= 3) {
        sendMsg("LOG", "[WARN] PERIODIC_STALL: recurring anomaly (" +
                String(anomaly_count) + " events). Possible partial obstruction.");
        sendMsg("CMD", "PERIODIC_STALL");
    }

    // COMMUNICATION_LOSS
    if (consecutive_noack >= 3) {
        sendMsg("LOG", "[CRITICAL] COMMUNICATION_LOSS: Observer unreachable. Safe mode.");
        goToSleep(60ULL * 1000000ULL);
    }

    // Grace window
    delay(800);

    if (restart_requested) {
        Serial.println("[TGT] Restart requested - rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    // System halted
    if (system_halted) {
        String hmsg = "[HALT] Halted - pump locked. Press 'Clear HALT' on the dashboard to recover.";
        if (haltCause) hmsg += String("  (cause: ") + haltCause + ")";
        sendMsg("LOG", hmsg);
        return;
    }

    // Default mode OFF
    if (!autoCycleEnabled) {
        sendMsg("LOG", "[IDLE] Default mode OFF - staying awake, no boots until toggled ON");
        return;
    }

    // ===== MAIN AUTO CYCLE =====

    prefs.putBool("halted", true);
    prefs.putBool("pumping", true);

    if (bootCount == 0) {
        // Learning
        sendMsg("LOG", "[ACTION] First boot -> calibration learning phase");

        bool ok = sendMsgWithACK("CMD", "START_LEARN");
        if (!ok) sendMsg("LOG", "[WARN] Observer did not confirm START_LEARN");
        delay(300);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0          = millis();
        unsigned long last_update = millis();
        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 300) {
                last_update = millis();
                tempSensor.requestTemperatures();
                float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
                sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) + "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);
        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu <= TURB_THRESHOLD_NTU) {
        sendMsg("LOG", "[ACTION] Turbidity=" + String(turbidity_ntu, 1) + " NTU -> pump ON (10 s)");

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
                float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
                sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);
        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu >  TURB_THRESHOLD_NTU) {
        sendMsg("LOG", "[ACTION] Water clean (" + String(turbidity_ntu, 1) + " NTU) -> feed + short pump");

        sendMsg("LOG", "[SERVO] Open - dispensing");
        servoMove(1500, 35);
        delay(500);
        sendMsg("LOG", "[SERVO] Close");
        servoMove(1000, 35);
        delay(500);

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
                sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);
        sendMsgWithACK("CMD", "STOP_MEASURE");
        sendMsg("LOG", "[PUMP] Short cycle done");
    }

    // End of cycle
    prefs.putBool("pumping", false);   
    delay(500);
    if (!emergency_stop && !system_halted) {
        prefs.putBool("halted", false);
        prefs.putInt("storm", 0);     
    } else {
        sendMsg("LOG", "[LATCH] Anomaly during cycle - halted state persisted");
    }

    if (system_halted) {
        String hmsg = "[HALT] Halted - pump locked. Press 'Clear HALT' on the dashboard to recover.";
        if (haltCause) hmsg += String("  (cause: ") + haltCause + ")";
        sendMsg("LOG", hmsg);
        return;
    }

    if (!autoCycleEnabled) {
        sendMsg("LOG", "[IDLE] Default mode switched OFF during cycle - staying awake");
        return;
    }

    if (restart_requested) {
        Serial.println("[TGT] Restart requested - rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    goToSleep();
}

// Servo open/close
void doFeed() {
    sendMsg("LOG", "[MANUAL] Feed - servo open");
    servoMove(1500, 35);
    delay(500);
    servoMove(1000, 35);
    sendMsg("LOG", "[MANUAL] Feed - servo closed");
}

// Manual calibration
void runManualCalibration() {
    sendMsg("LOG", String("[MANUAL] Calibration start (pump ") +
            (manualPumpOn ? "ON" : "OFF - baseline will be idle, thresholds invalid!") + ")");

    bool ok = sendMsgWithACK("CMD", "START_LEARN");
    if (!ok) sendMsg("LOG", "[WARN] Observer did not confirm START_LEARN");
    delay(300);

    unsigned long t0          = millis();
    unsigned long last_update = millis();
    while (millis() - t0 < 10000) {
        if (millis() - last_update >= 300) {
            last_update = millis();
            tempSensor.requestTemperatures();
            float live_temp = safeTemp(tempSensor.getTempCByIndex(0));
            sendMsg("DATA", "SENSOR:" + String(lastTurbidityNtu, 1) + "," + String(live_temp, 1));
        }
        delay(10);
    }
    sendMsgWithACK("CMD", "STOP_MEASURE");
    sendMsg("LOG", "[MANUAL] Calibration done - thresholds updated on Observer");
}

// loop() - default-mode OFF
void loop() {
    if (restart_requested) {
        Serial.println("[TGT] Restart requested - rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    unsigned long now = millis();

    // Manual pump 
    static bool monitorActive = false;
    static unsigned long last_stream = 0;
    if (manualPumpOn && !monitorActive) {
        sendMsgWithACK("CMD", "START_MONITOR");
        monitorActive = true;
    } else if (!manualPumpOn && monitorActive) {
        sendMsgWithACK("CMD", "STOP_MEASURE");
        monitorActive = false;
    }
    if (manualPumpOn && (now - last_stream >= 300)) {
        last_stream = now;
        tempSensor.requestTemperatures();
        float t = safeTemp(tempSensor.getTempCByIndex(0));
        sendMsg("DATA", "SENSOR:" + String(lastTurbidityNtu, 1) + "," + String(t, 1));
    }

    // Pump timed auto-off
    if (manualPumpOn && manualPumpOffAt != 0 && (long)(now - manualPumpOffAt) >= 0) {
        manualPumpOn    = false;
        manualPumpOffAt = 0;
        digitalWrite(PUMP_PIN, LOW);
        sendMsg("LOG", "[MANUAL] Pump duration elapsed - OFF");
    }

    // Calibration request 
    if (manualCalibrateReq) {
        manualCalibrateReq = false;
        runManualCalibration();
        monitorActive = false;
    }

    // Food
    if (manualFeedNow) {
        manualFeedNow = false;
        doFeed();
        nextFeedAt = (feedIntervalMs > 0) ? (now + feedIntervalMs) : 0;
    } else if (nextFeedAt != 0 && (long)(now - nextFeedAt) >= 0) {
        doFeed();
        nextFeedAt = millis() + feedIntervalMs;
    }

    // Heartbeat
    static unsigned long last_hb = 0;
    if (now - last_hb >= 2000) {
        last_hb = now;
        String hb = String(manualPumpOn ? "1" : "0") + String(system_halted ? "1" : "0");
        sendMsg("HB", hb);
    }

    delay(50);
}