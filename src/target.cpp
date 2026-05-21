/*
 * FLOAT - Target Node
 * ====================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h> // <--- AGGIUNTA: Necessaria per il controllo avanzato della radio
#include <OneWire.h>
#include <DallasTemperature.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Pin map ────────────────────────────────────────────────────────────────
const int PUMP_PIN       = 47;
const int SERVO_PIN      = 6;  // <--- MODIFICA 4: Spostato dal 6 al 18 per evitare crash di memoria Flash!
const int TURBIDITY_PIN  = 1;   
const int DS18B20_PIN    = 4;   

// ── Turbidity sensor calibration ──────────────────────────────────────────
const int   TURB_CLEAN_ADC     = 3435;   
const int   TURB_DIRTY_ADC     = 1200;   

const float TURB_MAX_NTU       = 3000.0f;
const float TURB_THRESHOLD_NTU = 100.0f; 

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

float readTurbidityNTU() {
    long sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(TURBIDITY_PIN);
        delay(2);
    }
    int adc = (int)(sum / 16);

    Serial.printf("[SENSOR] Valore ADC Grezzo (da copiare in alto): %d\n", adc);
    adc = constrain(adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);
    float ntu = TURB_MAX_NTU * (float)(TURB_CLEAN_ADC - adc) / (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);           
    return constrain(ntu, 0.0f, TURB_MAX_NTU);
}

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
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   

    Serial.begin(115200);
    pinMode(PUMP_PIN,  OUTPUT);
    pinMode(SERVO_PIN, OUTPUT);
    analogReadResolution(12);                      

    digitalWrite(PUMP_PIN,  LOW);
    digitalWrite(SERVO_PIN, LOW);

    if (system_halted) {
        Serial.println("[TGT] System halted — staying in deep sleep.");
        esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);   
    
    float water_temp = tempSensor.getTempCByIndex(0);
    if (water_temp == DEVICE_DISCONNECTED_C || water_temp < -10.0f)
        water_temp = 25.0f;   

    float turbidity_ntu = readTurbidityNTU();
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);

    // --- MODIFICA 1 E 2: BLOCCO CANALE E POTENZA MASSIMA ---
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Potenza Massima
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE); // Tunnel sul canale 13
    esp_wifi_set_promiscuous(false);
    // -------------------------------------------------------

    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 13; // <-- Sincronizzato con il Canale 13
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    delay(2000);   

    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) + " NTU  (" + String(turbidity_pct) + "%)");
    sendMsg("LOG", "[SENSOR] Water Temp: " + String(water_temp, 1) + " °C");

    String sensorPack = "SENSOR:" + String(turbidity_ntu, 1) + "," + String(water_temp, 1);
    sendMsg("DATA", sensorPack);

    if (bootCount == 0) {
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
        sendMsg("LOG", "[ACTION] Turbidity=" + String(turbidity_ntu, 1) + " NTU > threshold → pump ON (10 s)");
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
        sendMsg("LOG", "[ACTION] Water clean (" + String(turbidity_ntu, 1) + " NTU) → dispensing food");
        sendMsg("LOG", "[SERVO] Open 90°");
        servoMove(1500, 35);   
        delay(1000);
        sendMsg("LOG", "[SERVO] Close 0°");
        servoMove(1000, 35);   
    }

    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep for 10 s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}