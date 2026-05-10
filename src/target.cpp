#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── Pin map ────────────────────────────────────────────────────────────────
const int PUMP_PIN    = 47;
const int SERVO_PIN   = 6;
const int DS18B20_PIN = 4;   // Sensore temperatura 

// Nuovi Pin per i dispositivi climatici simulati (LEDs)
const int COOLER_PIN  = 8;   // Metti un LED Blu qui
const int HEATER_PIN  = 9;   // Metti un LED Rosso qui

// ── Variabili salvate in Deep Sleep ────────────────────────────────────────
RTC_DATA_ATTR int  bootCount     = 0;
RTC_DATA_ATTR bool system_halted = false;

uint8_t observerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
volatile bool emergency_stop = false;

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ── Funzioni di Base ───────────────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len); msg[len] = '\0';
    if (String(msg) == "HALT") {
        digitalWrite(PUMP_PIN, LOW); 
        emergency_stop = true;
        system_halted  = true;
    }
}

void sendMsg(const String& type, const String& content) {
    String full = type + ":" + content;
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    delay(50); 
}

void servoMove(int us, int pulses) {
    for (int i = 0; i < pulses; i++) {
        digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(us);
        digitalWrite(SERVO_PIN, LOW);  delay(18);
    }
}

// ── Setup (Logica Principale) ──────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 

    Serial.begin(115200);
    pinMode(PUMP_PIN,   OUTPUT);
    pinMode(SERVO_PIN,  OUTPUT);
    pinMode(COOLER_PIN, OUTPUT);
    pinMode(HEATER_PIN, OUTPUT);
    
    digitalWrite(PUMP_PIN,   LOW);
    digitalWrite(SERVO_PIN,  LOW);
    digitalWrite(COOLER_PIN, LOW);
    digitalWrite(HEATER_PIN, LOW);

    // Blocco hardware della pompa (se l'Observer ha rilevato stallo o dry-run)
    if (system_halted) {
        Serial.println("[TGT] Pompa bloccata da Observer. Ritorno in deep sleep.");
        esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
        esp_deep_sleep_start();
    }

    // ── 1. Lettura Temperatura Fresca ──
    tempSensor.begin();
    tempSensor.setResolution(9); 
    tempSensor.requestTemperatures(); 
    float water_temp = tempSensor.getTempCByIndex(0);
    
    // Fail-safe: se la sonda è rotta, simulo 25 gradi per far girare la pompa in sicurezza
    if (water_temp == DEVICE_DISCONNECTED_C || water_temp < -10.0f) water_temp = 25.0f;

    // ── 2. Inizializzazione Radio ESP-NOW ──
    WiFi.mode(WIFI_STA); WiFi.disconnect(); WiFi.setTxPower(WIFI_POWER_2dBm);
    if (esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer; memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 0; peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    delay(100); 

    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount) + " | Temp: " + String(water_temp, 1) + " C");
    sendMsg("DATA", "SENSOR:0.0," + String(water_temp, 1));

    // ── 3. GESTIONE CLIMATICA ED EVENTI ──
    
    if (water_temp > 45.0f) {
        // CASO A: Troppo Caldo
        sendMsg("LOG", "[CLIMA] Temp > 45°C. Rischio surriscaldamento. Pompa OFF.");
        sendMsg("LOG", "[ACTION] Avvio sistema di RAFFREDDAMENTO (10s)");
        digitalWrite(COOLER_PIN, HIGH); // Accende LED Blu
        delay(10000);                   // Simula il lavoro del raffreddatore per 10s
        digitalWrite(COOLER_PIN, LOW);  // Spegne LED
    } 
    else if (water_temp <= 0.0f) {
        // CASO B: Troppo Freddo / Gelo
        sendMsg("LOG", "[CLIMA] Temp <= 0°C. Rischio ghiaccio nelle tubature. Pompa OFF.");
        sendMsg("LOG", "[ACTION] Avvio sistema di RISCALDAMENTO (10s)");
        digitalWrite(HEATER_PIN, HIGH); // Accende LED Rosso
        delay(10000);                   // Simula il lavoro del riscaldatore per 10s
        digitalWrite(HEATER_PIN, LOW);  // Spegne LED
    }
    else {
        // CASO C: Temperatura Normale. Esegue le normali operazioni della pompa e del servo.
        if (bootCount == 0) {
            sendMsg("LOG", "[ACTION] Avvio Fase di Calibrazione (Learning)");
            sendMsg("CMD", "START_LEARN"); 
            digitalWrite(PUMP_PIN, HIGH);
            unsigned long t0 = millis();
            while (millis() - t0 < 10000 && !emergency_stop) { delay(10); }
            digitalWrite(PUMP_PIN, LOW);
            sendMsg("CMD", "STOP_MEASURE");
        } 
        else if (bootCount % 5 == 0) {
            sendMsg("LOG", "[ACTION] Manutenzione: Rilascio mangime / Movimento Servo");
            sendMsg("LOG", "[SERVO] Open 90°");
            servoMove(1500, 35); delay(1000);
            sendMsg("LOG", "[SERVO] Close 0°");
            servoMove(1000, 35);
        } 
        else {
            sendMsg("LOG", "[ACTION] Avvio Pompa per filtraggio (10s)");
            sendMsg("CMD", "START_MONITOR"); 
            digitalWrite(PUMP_PIN, HIGH);
            unsigned long t0 = millis();
            while (millis() - t0 < 10000 && !emergency_stop) { delay(10); }
            digitalWrite(PUMP_PIN, LOW);
            sendMsg("CMD", "STOP_MEASURE");
        }
    }

    // ── 4. Sleep ──
    bootCount++;
    sendMsg("LOG", "[SLEEP] Torno in Deep Sleep per 10s...");
    delay(100);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}