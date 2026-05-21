/*
 * FLOAT - Observer Node + Dashboard Web Server
 * ============================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <math.h>
#include "dashboard.h"

// ── Credenziali AP Dashboard ───────────────────────────────────────────────
const char* AP_SSID = "FLOAT-Dashboard";
const char* AP_PASS = "float1234";

WebServer server(80);
WiFiClient sseClient;
bool sseActive = false;

// ── Pin map ────────────────────────────────────────────────────────────────
const int I2C_SDA     = 41;
const int I2C_SCL     = 42;
const int BUZZER_PIN  = 7;

Adafruit_INA219 ina219;
uint8_t targetAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── System state ───────────────────────────────────────────────────────────
String  current_mode   = "IDLE";    
bool    system_locked  = false;
String  anomaly_reason = "NONE";

const int  MAX_SAMPLES = 60;
float      samples[MAX_SAMPLES];
int        sample_idx  = 0;
int grace_period = 0;

float baseline_mean    = 0.0f;
float baseline_std     = 0.0f;
float th_stall         = 0.0f;
float th_dry           = 0.0f;  // DRY_RUN threshold
float th_volt_min      = 0.0f;  
bool  is_calibrated    = false;

const float EWMA_ALPHA = 0.2f;  
float       ewma_current = 0.0f;
bool        ewma_init    = false;

int anomaly_confirm = 0;
const int CONFIRM_NEEDED = 2;   // Abbassato per latenza < 2s

float last_current  = 0.0f;
float last_voltage  = 0.0f;
float last_temp_c   = 25.0f;

// Limiti termici
const float TEMP_MIN_C = 18.0f;
const float TEMP_MAX_C = 32.0f;

// ── Hampel Filter (Sostituzione 3-Sigma Rule) ──────────────────────────────
float arrayMedian(float* arr, int n) {
    float buf[MAX_SAMPLES];
    memcpy(buf, arr, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float key = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = key;
    }
    return (n % 2 == 0) ? (buf[n/2-1] + buf[n/2]) / 2.0f : buf[n/2];
}

void hampelStats(float* arr, int n, float k_sigma, float& clean_mean, float& clean_std) {
    if (n < 3) return;

    float med = arrayMedian(arr, n);
    float devs[MAX_SAMPLES];
    for (int i = 0; i < n; i++) devs[i] = fabsf(arr[i] - med);

    float mad = arrayMedian(devs, n);
    float sigma_h = 1.4826f * mad;  

    float sum = 0.0f; float sq = 0.0f; int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (sigma_h < 1e-6f || fabsf(arr[i] - med) <= k_sigma * sigma_h) {
            sum += arr[i]; cnt++;
        }
    }
    if (cnt < 3) { clean_mean = med; clean_std = sigma_h; return; }
    
    clean_mean = sum / cnt;
    for (int i = 0; i < n; i++) {
        if (sigma_h < 1e-6f || fabsf(arr[i] - med) <= k_sigma * sigma_h)
            sq += powf(arr[i] - clean_mean, 2);
    }
    clean_std = sqrtf(sq / cnt);
}

// ── Helpers Comunicazione ──────────────────────────────────────────────────
void espNowSend(const char* msg) {
    esp_now_send(targetAddress, (const uint8_t*)msg, strlen(msg));
}

void buzzerAlert(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(150);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
}

// ── SSE Push alla Dashboard Web ────────────────────────────────────────────
void pushSSE() {
    if (!sseActive || !sseClient.connected()) { sseActive = false; return; }
    StaticJsonDocument<256> doc;
    doc["current"]   = last_current;
    doc["ewma"]      = ewma_current;
    doc["voltage"]   = last_voltage;
    doc["temp"]      = last_temp_c;
    doc["th_stall"]  = th_stall;
    doc["th_dry"]    = th_dry;
    
    if (system_locked) {
        doc["status"] = "HALTED";
    } else {
        doc["status"] = current_mode;
    }
    
    doc["anomaly"]   = anomaly_reason;
    doc["pump"]      = false; 
    doc["timestamp"] = millis(); 
    
    String body; serializeJson(doc, body);
    sseClient.print("data: " + body + "\n\n");
}

// ── Web Server Handlers ────────────────────────────────────────────────────
void handleRoot()   { server.send(200, "text/html", html_page); }
void handleEvents() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/event-stream", "");
    sseClient = server.client();
    sseActive = true;
}
void handleReset() {
    system_locked   = false;
    anomaly_reason  = "NONE";
    anomaly_confirm = 0;
    ewma_init       = false;
    current_mode    = "IDLE";
    digitalWrite(BUZZER_PIN, LOW);
    espNowSend("CMD:RESET");
    server.send(200, "text/plain", "OK");
}
void handleLearn() {
    espNowSend("CMD:START_LEARN");
    server.send(200, "text/plain", "OK");
}
void handlePump() {
    server.send(200, "text/plain", "OK");
}

// ── Wrapper ritardo per mantenere vivo il WebServer ────────────────────────
void smartDelay(uint32_t ms) {
    uint32_t start = millis();
    while (millis() - start < ms) {
        server.handleClient();
        static unsigned long last_push = 0;
        if (millis() - last_push >= 400) {
            last_push = millis();
            pushSSE();
        }
        delay(1);
    }
}

// ── ESP-NOW Callback ───────────────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    // Sistema ACK (Sincronizzazione)
    uint32_t msg_id = 0;
    int id_pos = s.indexOf("|ID:");
    if (id_pos != -1) {
        msg_id = s.substring(id_pos + 4).toInt();
        s = s.substring(0, id_pos);
        char ack_buf[32];
        snprintf(ack_buf, sizeof(ack_buf), "ACK:%lu", msg_id);
        esp_now_send(mac, (const uint8_t*)ack_buf, strlen(ack_buf));
    }

    if (s.startsWith("LOG:")) {
        Serial.println(s.substring(4));
    } else if (s.startsWith("DATA:SENSOR:")) {
        String payload = s.substring(12);
        int commaIndex = payload.indexOf(','); 
        if (commaIndex != -1) {
            last_temp_c = payload.substring(commaIndex + 1).toFloat();
        }
    } else if (s == "CMD:START_LEARN") {
        current_mode  = "LEARNING";
        sample_idx    = 0;
        ewma_init     = false;
        Serial.println("[OBS] MODE → LEARNING");

    } else if (s == "CMD:START_MONITOR") {
        if (!is_calibrated) {
            Serial.println("[OBS] WARNING: Not calibrated yet — monitoring skipped");
            return;
        }
        current_mode    = "MONITORING";
        anomaly_confirm = 0;
        ewma_init       = false; 
        grace_period    = 4;
        Serial.println("[OBS] MODE → MONITORING");

    } else if (s == "CMD:STOP_MEASURE") {
        if (current_mode == "LEARNING" && sample_idx > 5) {
            float clean_mean, clean_std;
            
            // Hampel Filter applicato qui (k = 2.5 per sensibilità ottima)
            hampelStats(samples, sample_idx, 2.5f, clean_mean, clean_std);

            baseline_mean = clean_mean;
            baseline_std  = clean_std;
            
            // Soglie ricalcolate
            th_stall = baseline_mean + (3.0f * baseline_std);
            if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;
            
            th_dry = baseline_mean * 0.30f; // 30% della media = pompa vuota
            th_volt_min = last_voltage * 0.90f;
            
            is_calibrated = true;

            Serial.println("\n[OBS] ══ Calibration Complete (Hampel Filter) ══");
            Serial.printf("   μ (mean)  : %.2f mA\n", baseline_mean);
            Serial.printf("   Stall thr : %.2f mA\n", th_stall);
            Serial.printf("   Dry thr   : %.2f mA\n", th_dry);
        }
        current_mode = "IDLE";
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    delay(2000); 
    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin(); Wire.setClock(100000);
    delay(100);
    if (!ina219.begin()) {
        Serial.println("[OBS] CRITICAL: INA219 not found!");
        while (1) delay(100);
    }

    // Modalità Access Point per Dashboard Web + Station per ESP-NOW
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[DASHBOARD] Connettiti al Wi-Fi: %s (Pass: %s)\n", AP_SSID, AP_PASS);
    Serial.printf("[DASHBOARD] Apri il browser all'IP: %s\n", WiFi.softAPIP().toString().c_str());

    WiFi.setTxPower(WIFI_POWER_19_5dBm); 
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE); 
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = 13; 
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // Registrazione Endpoint Web
    server.on("/",           handleRoot);
    server.on("/api/events", handleEvents);
    server.on("/api/reset",  HTTP_POST, handleReset);
    server.on("/api/learn",  HTTP_POST, handleLearn);
    server.on("/api/pump",   HTTP_POST, handlePump);
    server.begin();

    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║  FLOAT  –  Observer Node + Web Dashboard Attiva ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    // Gestisce il traffico Web e i push dati
    server.handleClient();
    static unsigned long last_push = 0;
    if (millis() - last_push >= 400) {
        last_push = millis();
        pushSSE();
    }

    float raw_current = ina219.getCurrent_mA();
    float bus_voltage = ina219.getBusVoltage_V();

    if (raw_current > 3000.0f || isnan(raw_current)) {
        Wire.end(); delay(10);
        Wire.setPins(I2C_SDA, I2C_SCL); Wire.begin(); Wire.setClock(100000);
        ina219.begin(); raw_current = 0.0f; bus_voltage = 0.0f;
    }

    last_current = max(0.0f, raw_current);
    last_voltage = max(0.0f, bus_voltage);

    if (!ewma_init) {
        ewma_current = last_current; ewma_init = true;
    } else {
        ewma_current = EWMA_ALPHA * last_current + (1.0f - EWMA_ALPHA) * ewma_current;
    }

    if (system_locked) {
        buzzerAlert(1);
        smartDelay(5000);
        return;
    }

    if (current_mode == "LEARNING") {
        if (last_current > 50.0f && sample_idx < MAX_SAMPLES) {
            samples[sample_idx++] = last_current;
            Serial.printf("   [LEARN] #%d  I=%.2f mA  V=%.2f V\n", sample_idx, last_current, last_voltage);
        }
        smartDelay(300);
        return;
    }

    if (current_mode == "MONITORING" && is_calibrated) {
        if (grace_period > 0) {
            grace_period--;
            smartDelay(200);
            return; 
        }
        
        float z_score = (baseline_std > 1e-6f) ? (ewma_current - baseline_mean) / baseline_std : 0.0f;
        
        bool stall_flag = (ewma_current > th_stall);
        bool volt_flag  = (th_volt_min > 0.1f && last_voltage < th_volt_min);
        bool dry_flag   = (last_current > 10.0f && ewma_current < th_dry);
        bool temp_flag  = (last_temp_c < TEMP_MIN_C || last_temp_c > TEMP_MAX_C);
        
        bool anomaly = stall_flag || volt_flag || dry_flag || temp_flag;

        Serial.printf("   [MON] I_ewma=%.1f  Z=%+.2f  V=%.2f  T=%.1f  [%d/%d]%s%s%s%s\n",
                      ewma_current, z_score, last_voltage, last_temp_c,
                      anomaly_confirm, CONFIRM_NEEDED, 
                      stall_flag ? " STALL" : "", volt_flag ? " VOLT" : "", 
                      dry_flag ? " DRY" : "", temp_flag ? " TEMP" : "");

        if (anomaly) {
            anomaly_confirm++;
            if (anomaly_confirm >= CONFIRM_NEEDED) {
                if      (stall_flag) anomaly_reason = "MOTOR_STALL";
                else if (dry_flag)   anomaly_reason = "DRY_RUN";
                else if (temp_flag)  anomaly_reason = "TEMP_OUT_OF_RANGE";
                else                 anomaly_reason = "VOLTAGE_DROP";

                Serial.printf("\n[!!!] ANOMALY CONFIRMED: %s\n", anomaly_reason.c_str());

                for (int i = 0; i < 5; i++) {
                    espNowSend("HALT");
                    delay(5);
                }

                buzzerAlert(3);
                system_locked = true;
                pushSSE(); // Forza un push d'emergenza
            }
        } else {
            anomaly_confirm = 0;
        }

        // Ridotto a 200ms per migliorare la reattività
        smartDelay(200);
        return;
    }

    smartDelay(1000);
}