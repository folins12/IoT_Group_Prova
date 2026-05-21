/*
 * FLOAT - Target Node
 * ====================
 * Misura torbidità + temperatura, pilota pompa e servo alimentatore.
 * Comunica via ESP-NOW (canale 13) con l'Observer.
 * Anomalie rilevate dall'Observer: MOTOR_STALL, DRY_RUN, VOLTAGE_DROP,
 *   TEMP_OUT_OF_RANGE, PERIODIC_STALL, COMMUNICATION_LOSS.
 *
 * Modifiche rispetto alla versione precedente:
 *   - ACK / retry con backoff sui comandi critici
 *   - readTurbidityNTU riceve la temperatura per correzione viscosità
 *   - Torbidità letta PRIMA di accendere il WiFi (ADC stabile)
 *   - Contatori RTC per anomalie ricorrenti e mancate comunicazioni
 *   - Deep-sleep alzato a 20 s → duty cycle ~9 %  (91 % sleep)
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
const float TURB_MAX_NTU       = 3000.0f;
const float TURB_THRESHOLD_NTU = 100.0f;

// ── Deep-sleep period ─────────────────────────────────────────────────────
// 20 s → active ≈ 2 s / 22 s = 9 %  → 91 % sleep ✓
const uint64_t SLEEP_US = 20ULL * 1000000ULL;

// ── RTC-persistent state (survives deep sleep) ─────────────────────────────
RTC_DATA_ATTR int      bootCount            = 0;
RTC_DATA_ATTR bool     system_halted        = false;
RTC_DATA_ATTR uint32_t last_cmd_id          = 0;   // per de-duplicazione ACK
RTC_DATA_ATTR int      consecutive_noack    = 0;   // comunicazioni fallite consecutive
RTC_DATA_ATTR int      anomaly_count        = 0;   // anomalie totali per rilevare PERIODIC_STALL

// ── Globals ────────────────────────────────────────────────────────────────
uint8_t observerAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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
    }
}

// ── sendMsg – fire and forget (telemetria, log) ────────────────────────────
void sendMsg(const String& type, const String& content) {
    String full = type + ":" + content;
    esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
    delay(150);
}

// ── sendMsgWithACK – comandi critici con retry + backoff ──────────────────
//   Aggiunge "|ID:N" al messaggio; l'Observer risponde "ACK:N".
//   Ritorna true se l'ACK è ricevuto entro (retries × timeout_ms).
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
                consecutive_noack = 0;   // reset contatore fallimenti
                return true;
            }
            delay(10);
        }
        delay(50 * (attempt + 1));   // backoff esponenziale
    }

    consecutive_noack++;
    Serial.printf("[TGT] WARNING: no ACK for '%s' after %d retries\n",
                  content.c_str(), retries);
    return false;
}

// ── readTurbidityNTU ───────────────────────────────────────────────────────
//   Legge l'ADC con WiFi OFF per evitare interferenze RF.
//   Applica correzione di viscosità in funzione della temperatura.
//   Chiamare PRIMA di WiFi.mode(WIFI_STA) / esp_now_init().
float readTurbidityNTU(float temp_c = 25.0f) {
    // Lettura ADC con media su 64 campioni
    long sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += analogRead(TURBIDITY_PIN);
        delay(2);
    }
    int adc = (int)(sum / 64);
    Serial.printf("[SENSOR] ADC Torbidità (raw): %d\n", adc);

    adc = constrain(adc, TURB_DIRTY_ADC, TURB_CLEAN_ADC);

    float ntu_raw = TURB_MAX_NTU *
                    (float)(TURB_CLEAN_ADC - adc) /
                    (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);

    // Correzione temperatura: viscosità dell'acqua cala con T.
    // La velocità di sedimentazione delle particelle aumenta → lettura NTU
    // apparentemente più bassa a T alta. Correzione lineare ±0.5 %/°C.
    float temp_correction = 1.0f + 0.005f * (temp_c - 25.0f);
    float ntu = ntu_raw / temp_correction;

    return constrain(ntu, 0.0f, TURB_MAX_NTU);
}

// ── servoMove – generazione PWM software ──────────────────────────────────
void servoMove(int us, int pulses) {
    for (int i = 0; i < pulses; i++) {
        digitalWrite(SERVO_PIN, HIGH);
        delayMicroseconds(us);
        digitalWrite(SERVO_PIN, LOW);
        delay(18);
    }
}

// ── goToSleep – helper unico per il deep sleep ────────────────────────────
void goToSleep(uint64_t duration_us = SLEEP_US) {
    bootCount++;
    sendMsg("LOG", "[SLEEP] Deep sleep " + String((unsigned long)(duration_us / 1000000UL)) + " s...");
    delay(200);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_timer_wakeup(duration_us);
    esp_deep_sleep_start();
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

    // ── Sistema bloccato per anomalia critica ──────────────────────────────
    if (system_halted) {
        Serial.println("[TGT] System halted — staying in deep sleep.");
        esp_sleep_enable_timer_wakeup(SLEEP_US);
        esp_deep_sleep_start();
    }

    // ── Lettura DS18B20 (prima del WiFi per non perdere slot conversione) ──
    tempSensor.begin();
    tempSensor.setResolution(11);
    tempSensor.setWaitForConversion(false);
    tempSensor.requestTemperatures();
    delay(400);   // attesa conversione 11 bit

    float water_temp = tempSensor.getTempCByIndex(0);
    if (water_temp == DEVICE_DISCONNECTED_C || water_temp < -10.0f)
        water_temp = 25.0f;   // fallback sicuro

    // ── Lettura torbidità con WiFi OFF ─────────────────────────────────────
    // ADC è sensibile alle interferenze RF; leggere prima di accendere il radio.
    float turbidity_ntu = readTurbidityNTU(water_temp);
    int   turbidity_pct = (int)((turbidity_ntu / TURB_MAX_NTU) * 100.0f);

    // ── Inizializzazione WiFi + ESP-NOW ────────────────────────────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
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

    delay(2000);   // pausa per stabilizzazione link ESP-NOW

    // ── Invio dati iniziali (fire and forget) ──────────────────────────────
    sendMsg("LOG", "────────────────────────────────────");
    sendMsg("LOG", "[BOOT] #" + String(bootCount));
    sendMsg("LOG", "[SENSOR] Turbidity : " + String(turbidity_ntu, 1) +
            " NTU  (" + String(turbidity_pct) + "%)");
    sendMsg("LOG", "[SENSOR] Water Temp: " + String(water_temp, 1) + " C");
    sendMsg("DATA", "SENSOR:" + String(turbidity_ntu, 1) +
            "," + String(water_temp, 1));

    // ── Rilevazione PERIODIC_STALL ─────────────────────────────────────────
    if (anomaly_count >= 3) {
        sendMsg("LOG", "[WARN] PERIODIC_STALL: anomalia ricorrente (" +
                String(anomaly_count) + " eventi). Possibile ostruzione parziale.");
        sendMsg("CMD", "PERIODIC_STALL");
    }

    // ── Rilevazione COMMUNICATION_LOSS ────────────────────────────────────
    if (consecutive_noack >= 3) {
        sendMsg("LOG", "[CRITICAL] COMMUNICATION_LOSS: Observer irraggiungibile. Safe mode.");
        // Sleep lungo in safe mode: riduce consumo senza bloccarsi
        goToSleep(60ULL * 1000000ULL);
    }

    // ══════════════════════════════════════════════════════════════════════
    // LOGICA PRINCIPALE
    // ══════════════════════════════════════════════════════════════════════

    if (bootCount == 0) {
        // ── Primo boot: fase di calibrazione ──────────────────────────────
        sendMsg("LOG", "[ACTION] First boot → calibration learning phase");

        bool ok = sendMsgWithACK("CMD", "START_LEARN");
        if (!ok) {
            sendMsg("LOG", "[WARN] Observer non ha confermato START_LEARN");
        }
        delay(300);

        // Pompa ON per 10 s con telemetria live ogni 500 ms
        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0 = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 500) {
                last_update = millis();
                float live_turb = readTurbidityNTU(water_temp);
                float live_temp = tempSensor.getTempCByIndex(0);
                tempSensor.requestTemperatures();
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else if (turbidity_ntu > TURB_THRESHOLD_NTU) {
        // ── Acqua torbida: pompa ON con monitoraggio ───────────────────────
        sendMsg("LOG", "[ACTION] Turbidity=" + String(turbidity_ntu, 1) +
                " NTU > soglia → pump ON (10 s)");

        bool ok = sendMsgWithACK("CMD", "START_MONITOR");
        if (!ok) {
            sendMsg("LOG", "[WARN] Observer non ha confermato START_MONITOR");
        }
        delay(300);

        digitalWrite(PUMP_PIN, HIGH);
        unsigned long t0 = millis();
        unsigned long last_update = millis();

        while (millis() - t0 < 10000 && !emergency_stop) {
            if (millis() - last_update >= 500) {
                last_update = millis();
                float live_turb = readTurbidityNTU(water_temp);
                float live_temp = tempSensor.getTempCByIndex(0);
                tempSensor.requestTemperatures();
                sendMsg("DATA", "SENSOR:" + String(live_turb, 1) +
                        "," + String(live_temp, 1));
            }
            delay(10);
        }
        digitalWrite(PUMP_PIN, LOW);

        sendMsgWithACK("CMD", "STOP_MEASURE");

    } else {
        // ── Acqua pulita: dispensa cibo con servo ──────────────────────────
        sendMsg("LOG", "[ACTION] Water clean (" + String(turbidity_ntu, 1) +
                " NTU) → dispensing food");
        sendMsg("LOG", "[SERVO] Open 90°");
        servoMove(1500, 35);
        delay(1000);
        sendMsg("LOG", "[SERVO] Close 0°");
        servoMove(1000, 35);
    }

    // ── Fine ciclo: deep sleep ─────────────────────────────────────────────
    goToSleep();
}

void loop() {}