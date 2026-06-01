/*
 * FLOAT - Target Node (v9)
 * ========================
 * Measures turbidity + temperature, drives pump and food servo.
 * Communicates via ESP-NOW (channel 13) with the Observer node.
 *
 * Turbidity is now read from a real sensor connected to an Arduino Uno,
 * which communicates with this ESP32 via a UART serial link (Serial1).
 * Protocol (same as the old standalone ESP32 in main.cpp):
 *   ESP32 → Arduino: 'T'          (request a reading)
 *   Arduino → ESP32: "<raw_adc>\n" (raw ADC value 0-1023)
 *   ESP32 → Arduino: "E<value>\n" (echo confirmation)
 * The Arduino Uno sleeps via watchdog every 4 s and listens for 150 ms
 * on wake. The ESP32 retries for up to 4.5 s to catch an Arduino wake cycle.
 * If no response arrives the system falls back to turbidity = 0 NTU (safe: clean).
 *
 * Anomalies detected by Observer: MOTOR_STALL, DRY_RUN, VOLTAGE_DROP (warning),
 *   TEMP_TOO_HIGH (warning), TEMP_TOO_LOW (warning), PERIODIC_STALL,
 *   COMMUNICATION_LOSS.
 */

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

// ── Preferences (NVS) ──────────────────────────────────────────────────────
// RTC_DATA_ATTR survives deep sleep but NOT power glitches (a pump-stall
// brownout will wipe it). NVS lives in flash and survives everything,
// so we mirror critical state (halted, default-mode) here too.
Preferences prefs;
bool autoCycleEnabled = true;   // mirrored in NVS as "auto"
volatile bool restart_requested = false;   // set by AUTO_ON_RESET in callback;
                                            // ESP.restart() is performed outside
                                            // the callback (in loop() / end-of-cycle).

// ── Manual controls (default-mode OFF only; all RAM, never persisted) ───────
// These exist only while the system is paused in idle. They are wiped whenever
// the user switches back to default-mode ON (the AUTO_ON_RESET handler clears
// them and reboots), and they do NOT survive a power cycle. This makes manual
// mode a maintenance/test bench, not a second persistent operating mode.
volatile bool          manualPumpOn       = false;  // pump currently driven by user
volatile unsigned long manualPumpOffAt    = 0;      // millis() deadline; 0 = infinite
volatile bool          manualFeedNow      = false;  // do one servo feed asap
volatile unsigned long feedIntervalMs     = 0;      // 0 = one-shot (no repeat)
volatile unsigned long nextFeedAt         = 0;      // millis() of next scheduled feed
volatile bool          manualCalibrateReq = false;  // run a learning sample from idle
float                  lastTurbidityNtu   = -1.0f;   // last turbidity, cached for
                                                     // loop()/manual calibration use

// ── Pin map ────────────────────────────────────────────────────────────────
const int PUMP_PIN      = 47;
const int SERVO_PIN     = 6;
const int DS18B20_PIN   = 7;

// ── Serial1 link to Arduino Uno (turbidity sensor) ────────────────────────
// Arduino Uno TX → ESP32 pin 4 (RXD1)
// Arduino Uno RX → ESP32 pin 5 (TXD1)
const int TURB_RXD = 4;
const int TURB_TXD = 5;

// ── Turbidity sensor calibration (raw ADC from Arduino Uno → NTU) ─────────
// These constants must match the physical sensor calibration.
// TURB_CLEAN_ADC : ADC value in perfectly clean water (high voltage = clear)
// TURB_DIRTY_ADC : ADC value in very dirty water (low voltage = turbid)
const int   TURB_CLEAN_ADC     = 750;  // ← adjust after physical calibration
const int   TURB_DIRTY_ADC     = 10;
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
        manualPumpOn    = false;   // manual run aborted by the halt
        manualPumpOffAt = 0;
        prefs.putBool("halted", true);   // persist across power loss
    } else if (s == "CMD:CLEAR_HALT") {
        // Recovery from a halt (dashboard "Clear HALT" button). Clears the latch.
        // In manual mode we return to a clean idle state (pump off; the user
        // re-enables it). In automatic mode we reboot into a fresh cycle, just
        // like AUTO_ON_RESET, so the system resumes unattended operation.
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
        if (autoCycleEnabled) {
            bootCount         = 0;          // fresh learning cycle after recovery
            consecutive_noack = 0;
            restart_requested = true;
            Serial.println("[TGT] HALT cleared by user — restarting fresh (auto)");
        } else {
            Serial.println("[TGT] HALT cleared by user — back to idle (manual)");
        }
    } else if (s == "CMD:AUTO_OFF") {
        // Dashboard toggle moved to OFF → stop the default cycle.
        // Effect takes hold from the NEXT wake (current action completes).
        if (autoCycleEnabled) {
            autoCycleEnabled = false;
            prefs.putBool("auto", false);
            Serial.println("[TGT] Default mode → OFF (system will idle next wake)");
        }
    } else if (s == "CMD:PUMP_ON") {
        // Manual pump ON, infinite (no duration given). Ignored while halted.
        if (!system_halted) {
            manualPumpOn    = true;
            manualPumpOffAt = 0;
            digitalWrite(PUMP_PIN, HIGH);
        }
    } else if (s.startsWith("CMD:PUMP_ON:")) {
        // Manual pump ON with a duration in seconds (0 = infinite).
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
    } else if (s.startsWith("CMD:FEED")) {
        // CMD:FEED  or  CMD:FEED:N  (N = repeat interval in seconds, 0/absent = once)
        long sec = 0;
        int  c   = s.indexOf(':', 4);       // second colon, if any
        if (c != -1) sec = s.substring(c + 1).toInt();
        feedIntervalMs = (sec > 0) ? (unsigned long)sec * 1000UL : 0;
        manualFeedNow  = true;              // first feed happens immediately in loop()
    } else if (s == "CMD:CALIBRATE") {
        // Run a manual learning sample. The actual work is done in loop()
        // (too long for the receive callback). Pump is left in whatever state
        // the user set — we deliberately don't force it on or off.
        manualCalibrateReq = true;
    } else if (s == "CMD:AUTO_ON") {
        // Dashboard toggle confirmed ON. No state reset — just remember.
        if (!autoCycleEnabled) {
            autoCycleEnabled = true;
            prefs.putBool("auto", true);
            Serial.println("[TGT] Default mode → ON");
        }
    } else if (s == "CMD:AUTO_ON_RESET") {
        // Dashboard toggle moved OFF → ON: full reset for a clean cycle.
        //
        // Key trick: we set bootCount = 0 here and trigger a software restart.
        // RTC_DATA_ATTR (bootCount) survives ESP.restart() but not POWERON, so
        // after the restart setup() runs again with bootCount = 0 → learning.
        //
        // Guard: only act while we're still in manual mode (autoCycleEnabled
        // false) and no restart is already pending. The Observer sends this
        // several times for reliability and re-echoes it on every heartbeat;
        // without the guard each copy that lands during boot would re-trigger a
        // restart, producing the BOOT#1→BOOT#0 loop seen in the logs.
        if (!autoCycleEnabled && !restart_requested) {
            autoCycleEnabled  = true;
            bootCount         = 0;
            system_halted     = false;
            anomaly_count     = 0;
            consecutive_noack = 0;
            // Wipe any manual controls — switching to ON always starts clean.
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
            restart_requested = true;
            Serial.println("[TGT] Default mode → ON + RESET (restart pending)");
        }
    } else if (s == "CMD:AUTO_ON_KEEP") {
        // Dashboard toggle OFF → ON *with a manual calibration already done*.
        // Keep the Observer's thresholds (it doesn't reset them) and skip the
        // learning phase by starting at bootCount = 1, so the target goes
        // straight into the monitored pump cycle using the user's baseline.
        // Same duplicate-restart guard as AUTO_ON_RESET.
        if (!autoCycleEnabled && !restart_requested) {
            autoCycleEnabled  = true;
            bootCount         = 1;          // 1 = skip learning, run monitored cycle
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
            restart_requested = true;
            Serial.println("[TGT] Default mode → ON, keeping manual calibration (boot 1)");
        }
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

// ── readTurbidityFromArduino ───────────────────────────────────────────────
// Queries the Arduino Uno via Serial1 using the 'T' / 'E' handshake protocol.
//
// The Arduino Uno sleeps via watchdog (4 s cycles) and listens for 150 ms on
// each wake. We retry for up to MAX_TURB_WAIT_MS (4500 ms) so we are guaranteed
// to catch at least one Arduino wake cycle regardless of phase offset.
//
// Must be called BEFORE WiFi.mode() / esp_now_init() because the ADC on the
// Arduino is read while the ESP32 radio is still off — RF interference from
// the ESP32 could slightly affect the Uno's analog reading over the PCB traces.
//
// Returns the turbidity in NTU, or 0.0 (clean-water safe default) on timeout.
//
// Temperature correction: water viscosity drops with temperature, so particles
// settle faster and the sensor reads lower NTU than it should. We apply a
// ±0.5 %/°C linear correction around 25 °C reference.

const unsigned long MAX_TURB_WAIT_MS = 4500; // covers one full 4-s Arduino sleep cycle

float readTurbidityFromArduino(float temp_c = 25.0f) {
    Serial1.begin(9600, SERIAL_8N1, TURB_RXD, TURB_TXD);
    delay(100); // allow Serial1 to stabilise before sending

    int   raw_adc   = 0;
    bool  got_value = false;
    unsigned long search_start = millis();

    // Keep sending 'T' until Arduino responds or timeout expires.
    // The Arduino discards non-'T' bytes, so repeated sends are harmless.
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
                // Non-numeric lines (Arduino debug prints) are silently ignored.
            }
        }
        if (got_value) break;
    }

    if (!got_value) {
        Serial.println("[SENSOR] WARNING: Arduino Uno did not respond — turbidity = 0 NTU (safe default)");
        Serial1.end();
        return 0.0f;
    }

    // Send echo confirmation so the Arduino knows the value was received.
    Serial1.print('E');
    Serial1.println(raw_adc);
    Serial1.flush();
    Serial1.end();

    Serial.printf("[SENSOR] Turbidity ADC from Arduino: %d\n", raw_adc);

    // Convert raw ADC to NTU with temperature viscosity correction.
    int   adc_clamped = constrain(raw_adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);
    float ntu_raw = TURB_MAX_NTU *
                (float)(adc_clamped - TURB_DIRTY_ADC) /
                (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);
    float correction  = 1.0f + 0.005f * (temp_c - 25.0f);
    return constrain(ntu_raw / correction, 0.0f, TURB_MAX_NTU);
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

// ── WiFi channel discovery ─────────────────────────────────────────────────
// The Observer joins this WiFi network and ESP-NOW lands on whatever channel
// the router/hotspot picks. We scan for the same SSID and pin our radio to
// the channel found, so both ESP32s talk on the same channel without any
// dedicated discovery AP. MUST MATCH the WIFI_SSID in observer.cpp.
const char* WIFI_SSID = "iPhone di Michele";

uint8_t findChannel() {
    for (int attempt = 0; attempt < 5; attempt++) {
        Serial.printf("[TGT] Scanning for '%s' (try %d/5)...\n", WIFI_SSID, attempt + 1);
        int n = WiFi.scanNetworks(false, false);   // sync, visible only
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
    Serial.println("[TGT] Network not found — fallback channel 13 (v9 default)");
    return 13;
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

    // ── Load persistent state from NVS ────────────────────────────────────
    // RTC vars survive deep sleep but a brownout from pump stall wipes them.
    // NVS lives in flash and survives any reset, so it's our source of truth
    // for halted state and default-mode toggle. The actual decision to sleep
    // (when halted) or skip the cycle (when auto=false) is taken LATER, after
    // ESP-NOW is up and we've had a chance to receive AUTO_ON_RESET — without
    // that grace window the target would deep-sleep before ever hearing the
    // dashboard, and the user could never recover from a HALT.
    prefs.begin("float", false);
    if (!system_halted) system_halted = prefs.getBool("halted", false);
    autoCycleEnabled = prefs.getBool("auto", true);

    // ── Fresh-power-on halt handling ──────────────────────────────────────
    // The NVS "halted" latch protects against re-running the pump after a
    // stall, even across a power loss. But a deliberate power-on must be able
    // to start fresh (the user expects a new learning cycle); otherwise a
    // stale latch from a previous test session blocks startup forever — which
    // is exactly the "learning never starts" symptom in the logs.
    //
    // We can't tell a clean power-on from a stall-induced brownout by reset
    // reason alone (both wipe RTC and report POWERON with brown-out disabled).
    // So we keep an NVS "storm" counter: every RTC-wiping reset bumps it; a
    // clean monitored cycle (or a dashboard Clear HALT / mode reset) clears it.
    //  - storm ≤ 3: treat as a genuine (re)start — clear any stale halt latch so
    //    the system learns/runs fresh instead of being stuck from a prior session.
    //  - storm > 3: the device keeps resetting without ever finishing a clean
    //    cycle — typically a pump stall that browns out the supply, looping the
    //    learning phase forever. Force a halt (and stay awake, handled below) so
    //    the dashboard shows HALTED and the user can fix the pump + Clear HALT.
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {   // RTC wiped: power-on / brownout / SW reset
        int storm = prefs.getInt("storm", 0) + 1;
        prefs.putInt("storm", storm);
        if (storm > 3) {
            system_halted = true;
            prefs.putBool("halted", true);
            Serial.printf("[TGT] Reset storm=%d — fault loop suspected, halting for safety\n", storm);
        } else if (system_halted) {
            system_halted = false;
            prefs.putBool("halted", false);
            Serial.printf("[TGT] Power-on (storm=%d) — cleared stale halt, starting fresh\n", storm);
        }
    }

    // ── DS18B20 reading (before WiFi to avoid losing conversion window) ───
    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);  // wait for 11-bit conversion

    float water_temp = safeTemp(tempSensor.getTempCByIndex(0));

    // ── Turbidity reading via Arduino Uno (Serial1, WiFi still OFF) ───────
    // Serial1 communication happens here, before WiFi init, for two reasons:
    //   1. The Arduino Uno's analog read is less susceptible to RF noise
    //      when the ESP32 radio is off.
    //   2. The 4.5-second timeout fits naturally in the boot sequence without
    //      blocking the ESP-NOW link (which hasn't been created yet).
    float turbidity_ntu = readTurbidityFromArduino(water_temp);
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);
    lastTurbidityNtu    = turbidity_ntu;   // cache for loop()/manual calibration

    // ── WiFi + ESP-NOW init ───────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // Discover the WiFi channel by scanning for WIFI_SSID (the same network
    // the Observer joins). ESP-NOW will then run on that channel — no need
    // for a hardcoded value, works with any router/hotspot.
    uint8_t obs_channel = findChannel();

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(obs_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = obs_channel;
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
            " NTU  (" + String(turbidity_pct) + "%)  [Arduino Uno sensor]");
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

    // ── Wait briefly for any state command from the Observer ──────────────
    // Observer piggy-backs HALT (if it's locked), CMD:AUTO_OFF/AUTO_ON/
    // AUTO_ON_RESET (toggle changes) on every incoming message from us. The
    // DATA:SENSOR we just sent triggers it, so a short delay lets the callback
    // land and update system_halted / autoCycleEnabled before we decide what
    // to do next.
    delay(800);

    // ── Decision after grace window ───────────────────────────────────────

    // A restart was requested by AUTO_ON_RESET / AUTO_ON_KEEP — perform it
    // cleanly here, before doing any cycle work that an imminent reboot wastes.
    if (restart_requested) {
        Serial.println("[TGT] Restart requested — rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    // Default mode OFF (manual): do NOT deep-sleep. Return from setup() so
    // loop() takes over and the device stays awake on ESP-NOW. This holds even
    // when halted: in manual mode a HALT must leave the device awake so the
    // user can press "Clear HALT" on the dashboard (in automatic mode, by
    // contrast, a halt sleeps and is recovered via the OFF/ON toggle below).
    // A HALT keeps the device AWAKE in BOTH modes (it does not deep-sleep), so
    // the dashboard shows HALTED and the "Clear HALT" button can reach the
    // target over ESP-NOW. This makes automatic-mode HALT behave exactly like
    // manual-mode HALT. Recovery via Clear HALT: in auto mode it reboots into a
    // fresh cycle, in manual mode it returns to idle (handled in CMD:CLEAR_HALT).
    if (system_halted) {
        sendMsg("LOG", "[HALT] Halted — pump locked. "
                       "Press 'Clear HALT' on the dashboard to recover.");
        return;
    }

    // Default mode OFF (manual) and NOT halted: stay awake so loop() can service
    // the manual controls and heartbeat. Automatic mode falls through to the cycle.
    if (!autoCycleEnabled) {
        sendMsg("LOG", "[IDLE] Default mode OFF — staying awake, no boots until toggled ON");
        return;
    }

    // ══════════════════════════════════════════════════════════════════════
    // MAIN LOGIC
    // ══════════════════════════════════════════════════════════════════════

    // ── SAFETY LATCH ON ───────────────────────────────────────────────────
    // Write halted=true to NVS BEFORE any pump activation. If something goes
    // wrong during the cycle — observer's HALT lost due to radio drop, voltage
    // brownout from stall current, target spontaneously resetting, anything —
    // the NVS state will keep the target halted on its next boot. The latch
    // is cleared further down ONLY if the cycle completes cleanly (no anomaly).
    // This is the primary fix for the "pump turns on after HALT" bug, since
    // it doesn't rely on HALT reaching the target during the noisiest moment
    // (high pump current + voltage sag).
    prefs.putBool("halted", true);

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
                float live_turb = turbidity_ntu;  // re-use boot reading; turbidity changes slowly
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu <= TURB_THRESHOLD_NTU) {
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
                float live_turb = turbidity_ntu;  // re-use boot reading; turbidity changes slowly
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu >  TURB_THRESHOLD_NTU) {
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
                float live_turb = turbidity_ntu;  // re-use boot reading; turbidity changes slowly
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);
        sendMsgWithACK("CMD", "STOP_MEASURE");
        sendMsg("LOG", "[PUMP] Short cycle done");
    }

    // ── End of cycle ──────────────────────────────────────────────────────

    // SAFETY LATCH OFF: only clear halted=true if the cycle completed without
    // any anomaly. The 500ms wait gives the Observer one last chance to push
    // a HALT we might have missed earlier (e.g., its first HALT was lost
    // during the high-current transient).
    delay(500);
    if (!emergency_stop && !system_halted) {
        prefs.putBool("halted", false);
        prefs.putInt("storm", 0);   // a clean cycle proves we're not in a stall loop
    } else {
        sendMsg("LOG", "[LATCH] Anomaly detected during cycle — halted state persisted");
    }

    // Halted by an anomaly during this cycle: stay AWAKE (don't deep-sleep) so
    // the dashboard shows HALTED and Clear HALT can reach us — same as manual
    // mode. loop() takes over and keeps sending heartbeats.
    if (system_halted) {
        sendMsg("LOG", "[HALT] Halted — pump locked. "
                       "Press 'Clear HALT' on the dashboard to recover.");
        return;
    }

    // AUTO_OFF received mid-cycle: stay awake instead of sleeping. loop() will
    // hold the device until the user toggles ON again.
    if (!autoCycleEnabled) {
        sendMsg("LOG", "[IDLE] Default mode switched OFF during cycle — staying awake");
        return;
    }

    // AUTO_ON_RESET received mid-cycle: do a clean reboot so the next setup()
    // sees bootCount=0 from RTC and runs a fresh learning cycle.
    if (restart_requested) {
        Serial.println("[TGT] Restart requested — rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    goToSleep();
}

// ── doFeed – one servo open/close to drop food ───────────────────────────────
// Same motion the automatic clean-water branch uses. Safe to call from loop().
void doFeed() {
    sendMsg("LOG", "[MANUAL] Feed — servo open 90°");
    servoMove(1500, 35);   // open
    delay(500);
    servoMove(1000, 35);   // close
    sendMsg("LOG", "[MANUAL] Feed — servo closed");
}

// ── runManualCalibration – learn a baseline from the CURRENT pump state ──────
// Triggered by the dashboard "Calibrate" button while idle. Deliberately does
// NOT touch the pump: it samples whatever the user has set up. If the pump is
// off the baseline will be the idle current (~13 mA) and the resulting stall
// threshold will be far too low — that's the user's responsibility, and the
// dashboard warns about it. Reuses the Observer's existing LEARNING machinery:
// START_LEARN → stream temp/turb for 10 s → STOP_MEASURE → Observer computes
// EWMA+Hampel thresholds exactly as in the automatic first-boot calibration.
void runManualCalibration() {
    sendMsg("LOG", String("[MANUAL] Calibration start (pump ") +
            (manualPumpOn ? "ON" : "OFF — baseline will be idle, thresholds invalid!") + ")");

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
            float live_turb = lastTurbidityNtu;
            sendMsg("DATA", "SENSOR:" + String(live_turb, 1) + "," + String(live_temp, 1));
        }
        delay(10);
    }
    sendMsgWithACK("CMD", "STOP_MEASURE");
    sendMsg("LOG", "[MANUAL] Calibration done — thresholds updated on Observer");
}

// ── loop ───────────────────────────────────────────────────────────────────
// Reached only in default-mode OFF (setup() returns early). The device stays
// awake on ESP-NOW. This is the manual / maintenance bench: it services the
// pump duration timer, the food scheduler and calibration requests, and pings
// the Observer with its pump state so OFF→ON reconciliation keeps working.
void loop() {
    if (restart_requested) {
        Serial.println("[TGT] Restart requested — rebooting for fresh state");
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    unsigned long now = millis();

    // ── Manual pump monitoring ────────────────────────────────────────────
    // While the user runs the pump manually we put the Observer in MONITORING
    // so its stall / dry-run detection (and HALT) stay active exactly as in the
    // automatic cycle. The Observer samples current on its own timer; we just
    // tell it when to start/stop and stream temp/turbidity for its temp alarms
    // and the dashboard. NOTE: the Observer only detects current anomalies when
    // calibrated, so manual HALT requires a prior calibration with the pump on.
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

    // Manual pump timed auto-off (signed compare handles millis() wrap).
    if (manualPumpOn && manualPumpOffAt != 0 && (long)(now - manualPumpOffAt) >= 0) {
        manualPumpOn    = false;
        manualPumpOffAt = 0;
        digitalWrite(PUMP_PIN, LOW);
        sendMsg("LOG", "[MANUAL] Pump duration elapsed — OFF");
    }

    // Manual calibration request (long-running; runs outside the RX callback).
    if (manualCalibrateReq) {
        manualCalibrateReq = false;
        runManualCalibration();
        monitorActive = false;   // calibration left Observer in IDLE; if the
                                 // pump is still on, re-arm MONITORING next pass
    }

    // Food: first feed immediately, then repeat at the chosen interval (if any).
    if (manualFeedNow) {
        manualFeedNow = false;
        doFeed();
        nextFeedAt = (feedIntervalMs > 0) ? (now + feedIntervalMs) : 0;
    } else if (nextFeedAt != 0 && (long)(now - nextFeedAt) >= 0) {
        doFeed();
        nextFeedAt = millis() + feedIntervalMs;   // re-read millis(): doFeed() blocks ~1.6s
    }

    // Heartbeat every 2 s, reporting pump + halt state as two digits:
    // "HB:<pump><halt>" e.g. "HB:00" idle, "HB:10" pumping, "HB:01" halted.
    // Lets the Observer reconcile OFF→ON, light the dashboard pump control,
    // and — crucially — surface a target-side halt even when the Observer
    // itself was reset and lost its own lock, so "Clear HALT" stays reachable.
    static unsigned long last_hb = 0;
    if (now - last_hb >= 2000) {
        last_hb = now;
        String hb = String(manualPumpOn ? "1" : "0") + String(system_halted ? "1" : "0");
        sendMsg("HB", hb);
    }

    delay(50);
}