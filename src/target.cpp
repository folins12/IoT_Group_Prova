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
const int   TURB_CLEAN_ADC     = 3435;   // ADC count in distilled water (≈ 0 NTU)
const int   TURB_DIRTY_ADC     = 1200;   // ADC count at ~3000 NTU reference
const float TURB_MAX_NTU       = 3000.0f;
const float TURB_THRESHOLD_NTU = 100.0f; // NTU above which pump activates

// ── RTC-persistent state (survives deep sleep) ─────────────────────────────
RTC_DATA_ATTR int  bootCount      = 0;
RTC_DATA_ATTR bool system_halted  = false;
// Variabili di Sicurezza Anti-Replay Attack che non si cancellano nel Deep Sleep:
RTC_DATA_ATTR unsigned long last_received_seq = 0; 
RTC_DATA_ATTR unsigned long target_seq_num = 1;    

// ── Globals ────────────────────────────────────────────────────────────────
uint8_t observerAddress[] = {0xF0, 0x9E, 0x9E, 0x77, 0x73, 0x60}; // Continuiamo col Broadcast per ora
volatile bool emergency_stop = false;

// Le chiavi AES devono essere ESATTAMENTE di 16 caratteri
const char *PMK_KEY_STR = "SuperSegretoPMK!"; 
const char *LMK_KEY_STR = "ChiaveTarget1234";

OneWire           oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ── ESP-NOW callback (CON SICUREZZA ANTI-REPLAY) ───────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String msgStr = String(msg);

    // 1. Controlla se il messaggio contiene un Sequence Number (es. "HALT|SEQ:5")
    int seqIndex = msgStr.indexOf("|SEQ:");
    if (seqIndex > 0) {
        String cmdPart = msgStr.substring(0, seqIndex); // Estrae il comando vero e proprio, es: "HALT"
        unsigned long incomingSeq = msgStr.substring(seqIndex + 5).toInt(); // Estrae il numero

        // 2. Controllo Anti-Replay Attack
        if (incomingSeq <= last_received_seq) {
            Serial.printf("[SECURITY] Replay Attack Bloccato! Seq ricevuto: %lu, Atteso: > %lu\n", incomingSeq, last_received_seq);
            return; // SCARTA IL PACCHETTO FALSO/VECCHIO!
        }
        
        // 3. Se è valido, aggiorna la memoria per i prossimi pacchetti e pulisce la stringa
        last_received_seq = incomingSeq;
        msgStr = cmdPart; 
    } else {
        Serial.println("[SECURITY] Messaggio ignorato: Nessun Sequence Number trovato.");
        return; // Blocca i messaggi non formattati in modo sicuro
    }

    // 4. Esegue il comando verificato e "pulito"
    if (msgStr == "HALT") {
        digitalWrite(PUMP_PIN, LOW);
        emergency_stop = true;
        system_halted  = true;
        Serial.println("[CMD] Ricevuto comando HALT verificato e sicuro.");
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────
void sendMsg(const String& type, const String& content) {
    // Aggiungiamo il Sequence Number anche ai messaggi in uscita verso l'Observer!
    String full = type + ":" + content + "|SEQ:" + String(target_seq_num);
    
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    
    target_seq_num++; // Incrementa il contatore per non mandare cloni
    delay(150); // Mantenuto a 150 per dare respiro al Wi-Fi
}

/**
 * Read turbidity via ADC and return NTU.
 * [VERSIONE SIMULATA: Alterna Acqua Pulita e Acqua Sporca ad ogni risveglio]
 */
float readTurbidityNTU() {
    if (bootCount % 2 != 0) {
        Serial.println("[MOCK SENSOR] Generato valore: ACQUA SPORCA (500 NTU)");
        return 500.0f; 
    } else {
        Serial.println("[MOCK SENSOR] Generato valore: ACQUA PULITA (20 NTU)");
        return 20.0f;  
    }
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
    delay(400);   
    float water_temp = tempSensor.getTempCByIndex(0);
    if (water_temp == DEVICE_DISCONNECTED_C || water_temp < -10.0f)
        water_temp = 25.0f; 

    float turbidity_ntu = readTurbidityNTU();
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);

    // ── ESP-NOW SECURE INIT ────────────────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setTxPower(WIFI_POWER_2dBm);

    if (esp_now_init() == ESP_OK) {
        
        // 1. Imposta la Primary Master Key (PMK)
        esp_now_set_pmk((uint8_t *)PMK_KEY_STR); 
        
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, observerAddress, 6);
        peer.channel = 0;
        
        // 2. ATTIVAZIONE DELLA CRITTOGRAFIA AES E INSERIMENTO DELLA CHIAVE LMK
        peer.encrypt = true;
        memcpy(peer.lmk, LMK_KEY_STR, 16); 
        
        esp_now_add_peer(&peer);
    }

    delay(2000);   // allow ESP-NOW stack to settle

    // ── Log sensor readings ────────────────────────────────────────────────
    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) + " NTU  (" + String(turbidity_pct) + "%)");
    sendMsg("LOG", "[SENSOR] Water Temp: " + String(water_temp, 1) + " °C");

    String sensorPack = "SENSOR:" + String(turbidity_ntu, 1) + "," + String(water_temp, 1);
    sendMsg("DATA", sensorPack);

    // ── Decision logic ─────────────────────────────────────────────────────
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

    // ── Sleep ──────────────────────────────────────────────────────────────
    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep for 10 s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {}