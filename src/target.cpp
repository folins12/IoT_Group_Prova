/*
 * FLOAT - Target Node
 * ====================
 * Responsibilities:
 * - Read real turbidity via analog sensor (e.g. DFRobot SEN0189 / generic TSD-10)
 * connected to GPIO 1 (ADC1_CH0).  Output is 0–4095 → converted to NTU.
 * - Read water temperature via DS18B20 on GPIO 4 (shared OneWire bus)
 * - Control the water pump (GPIO 47)
 * - Control the servo feeder (GPIO 6, bit-bang PWM)
 * - Send structured LOG and CMD messages to Observer via ESP-NOW
 * - Receive HALT from Observer and enter deep sleep
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Pin map ────────────────────────────────────────────────────────────────
const int PUMP_PIN       = 47;
const int SERVO_PIN      = 6;
const int TURBIDITY_PIN  = 1;   // ADC1_CH0 — analog turbidity sensor output
const int DS18B20_PIN    = 4;   // OneWire (4.7 kΩ pull-up to 3.3 V)

// ── Turbidity sensor calibration ──────────────────────────────────────────
// DOPO AVER LETTO IL VALORE GREZZO SUL MONITOR, AGGIORNA QUESTI DUE NUMERI:
const int   TURB_CLEAN_ADC     = 3435;   // ADC in acqua pulita (valore alto)
const int   TURB_DIRTY_ADC     = 1200;   // ADC in acqua molto sporca (valore basso)

const float TURB_MAX_NTU       = 3000.0f;
const float TURB_THRESHOLD_NTU = 100.0f; // NTU oltre il quale si attiva la pompa

// ── RTC-persistent state (survives deep sleep) ─────────────────────────────
RTC_DATA_ATTR int  bootCount      = 0;
RTC_DATA_ATTR bool system_halted  = false;

// ── Globals ────────────────────────────────────────────────────────────────
uint8_t observerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
volatile bool emergency_stop = false;

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ── ESP-NOW callback ───────────────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    if (String(msg) == "HALT") {
        digitalWrite(PUMP_PIN, LOW);
        emergency_stop = true;
        system_halted  = true;
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────
void sendMsg(const String& type, const String& content) {
    String full = type + ":" + content;
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    delay(150);
}

/**
 * Read turbidity via ADC and return NTU.
 * [VERSIONE REALE: Legge il modulo rosso collegato al Pin 1]
 */
float readTurbidityNTU() {
    // Fa 16 letture veloci e calcola la media per filtrare i "rumori" elettrici dell'acqua
    long sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(TURBIDITY_PIN);
        delay(2);
    }
    int adc = (int)(sum / 16);

    // ── STAMPA IL VALORE GREZZO PER LA CALIBRAZIONE MANUALE ──
    Serial.printf("[SENSOR] Valore ADC Grezzo (da copiare in alto): %d\n", adc);

    // Evita che il valore esca dai limiti imposti in alto
    adc = constrain(adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);

    // Interpolazione lineare: ADC più basso = Acqua più sporca (più NTU)
    float ntu = TURB_MAX_NTU *
                (float)(TURB_CLEAN_ADC - adc) /
                (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);
                
    return constrain(ntu, 0.0f, TURB_MAX_NTU);
}

/** Bit-bang servo: send `pulses` PWM pulses of `us` microseconds. */
void servoMove(int us, int pulses) {
    for (int i = 0; i < pulses; i++) {
        digitalWrite(SERVO_PIN, HIGH);
        delayMicroseconds(us);
        digitalWrite(SERVO_PIN, LOW);
        delay(18);
    }
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // disable brownout reset

    Serial.begin(115200);
    pinMode(PUMP_PIN,  OUTPUT);
    pinMode(SERVO_PIN, OUTPUT);
    analogReadResolution(12);                      // 12-bit ADC (0–4095)

    digitalWrite(PUMP_PIN,  LOW);
    digitalWrite(SERVO_PIN, LOW);

    // If a HALT was received last cycle, stay asleep
    if (system_halted) {
        Serial.println("[TGT] System halted — staying in deep sleep.");
        esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // ── Sensors ────────────────────────────────────────────────────────────
    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);   // allow DS18B20 conversion
    
    float water_temp = tempSensor.getTempCByIndex(0);
    if (water_temp == DEVICE_DISCONNECTED_C || water_temp < -10.0f)
        water_temp = 25.0f;   // fallback if probe absent

    // ── LEGGE IL SENSORE VERO ──
    float turbidity_ntu = readTurbidityNTU();
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);

    // ── ESP-NOW ────────────────────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setTxPower(WIFI_POWER_2dBm);

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    delay(2000);   // allow ESP-NOW stack to settle

    // ── Log sensor readings ────────────────────────────────────────────────
    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) + " NTU  (" + String(turbidity_pct) + "%)");
    sendMsg("LOG", "[SENSOR] Water Temp: " + String(water_temp, 1) + " °C");

    // Forward sensor pack to observer so dashboard stays in sync
    String sensorPack = "SENSOR:" +
                        String(turbidity_ntu, 1) + "," +
                        String(water_temp, 1);
    sendMsg("DATA", sensorPack);

// ── Decision logic ─────────────────────────────────────────────────────
    if (bootCount == 0) {
        // ── First boot: calibration learning phase ─────────────────────────
        sendMsg("LOG", "[ACTION] First boot → calibration learning phase");
        sendMsg("CMD", "START_LEARN");
        delay(500);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0 = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 500) {
                last_update = millis();
                float live_turb = readTurbidityNTU();
                float live_temp = tempSensor.getTempCByIndex(0);
                tempSensor.requestTemperatures(); 
                
                String livePack = "SENSOR:" + String(live_turb, 1) + "," + String(live_temp, 1);
                sendMsg("DATA", livePack);
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsg("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu > TURB_THRESHOLD_NTU) {
        // ── Water dirty: run pump under monitoring ─────────────────────────
        sendMsg("LOG", "[ACTION] Turbidity=" + String(turbidity_ntu, 1) +
                       " NTU > threshold → pump ON (10 s)");
        sendMsg("CMD", "START_MONITOR");
        delay(500);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0 = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 500) {
                last_update = millis();
                float live_turb = readTurbidityNTU();
                float live_temp = tempSensor.getTempCByIndex(0);
                tempSensor.requestTemperatures(); 
                
                String livePack = "SENSOR:" + String(live_turb, 1) + "," + String(live_temp, 1);
                sendMsg("DATA", livePack);
            }
            delay(10); 
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsg("CMD", "STOP_MEASURE");

    } else {
        // ── Water clean: dispense food ─────────────────────────────────────
        sendMsg("LOG", "[ACTION] Water clean (" +
                       String(turbidity_ntu, 1) + " NTU) → dispensing food");

        sendMsg("LOG", "[SERVO] Open 90°");
        servoMove(1500, 35);   // 1500 µs ≈ 90°
        delay(1000);

        sendMsg("LOG", "[SERVO] Close 0°");
        servoMove(1000, 35);   // 1000 µs ≈ 0°
    }

    // ── Sleep ──────────────────────────────────────────────────────────────
    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep for 10 s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}