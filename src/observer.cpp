#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>

const int I2C_SDA    = 41;
const int I2C_SCL    = 42;
const int BUZZER_PIN = 7;

Adafruit_INA219 ina219;
uint8_t targetAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

String current_mode  = "IDLE";
bool   system_locked = false;
String anomaly_reason= "NONE";

// ── Statistica Edge AI ──
const int MAX_SAMPLES = 60;
float samples[MAX_SAMPLES];
int   sample_idx = 0;

float baseline_mean = 0.0f;
float baseline_std  = 0.0f;
float th_stall      = 0.0f;
float th_dry        = 0.0f;
bool  is_calibrated = false;

const float EWMA_ALPHA = 0.2f;
float ewma_current = 0.0f;
bool  ewma_init    = false;
int   anomaly_confirm = 0;

float last_current = 0.0f;
float last_voltage = 0.0f;

// ── Latenza e Sincronizzazione ──
unsigned long sync_timer = 0;
bool latency_measured = false;

// ── Funzioni Matematiche ──
void computeStats(float* arr, int n, float& mean, float& std_dev) {
    float sum = 0.0f; for (int i = 0; i < n; i++) sum += arr[i];
    mean = sum / n;
    float sq = 0.0f; for (int i = 0; i < n; i++) sq += powf(arr[i] - mean, 2);
    std_dev = sqrtf(sq / n);
}

void robustStats(float* arr, int n, float z_limit, float& clean_mean, float& clean_std) {
    float raw_mean, raw_std; computeStats(arr, n, raw_mean, raw_std);
    float sum = 0.0f, sq = 0.0f; int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (raw_std < 1e-6f || fabsf(arr[i] - raw_mean) / raw_std <= z_limit) { sum += arr[i]; cnt++; }
    }
    if (cnt < 3) { clean_mean = raw_mean; clean_std = raw_std; return; }
    clean_mean = sum / cnt;
    for (int i = 0; i < n; i++) {
        if (raw_std < 1e-6f || fabsf(arr[i] - raw_mean) / raw_std <= z_limit) sq += powf(arr[i] - clean_mean, 2);
    }
    clean_std = sqrtf(sq / cnt);
}

void espNowSend(const char* msg) { esp_now_send(targetAddress, (const uint8_t*)msg, strlen(msg)); }

void buzzerAlert(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(150);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
}

// ── Ricezione Radio ──
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1]; memcpy(msg, data, len); msg[len] = '\0';
    String s(msg);

    if (s.startsWith("LOG:")) {
        Serial.println(s.substring(4));
    } else if (s == "CMD:START_LEARN") {
        current_mode = "LEARNING";
        sample_idx = 0; ewma_init = false;
        sync_timer = millis(); // [SYNC] Faccio partire il cronometro latenza
        latency_measured = false;
        Serial.println("[OBS] MODE → LEARNING (Sincronizzato)");
    } else if (s == "CMD:START_MONITOR") {
        if (!is_calibrated) return;
        current_mode = "MONITORING";
        anomaly_confirm = 0; ewma_init = false;
        sync_timer = millis(); // [SYNC] Faccio partire il cronometro latenza
        latency_measured = false;
        Serial.println("[OBS] MODE → MONITORING (Sincronizzato)");
    } else if (s == "CMD:STOP_MEASURE") {
        if (current_mode == "LEARNING" && sample_idx > 5) {
            float clean_mean, clean_std;
            robustStats(samples, sample_idx, 1.5f, clean_mean, clean_std);
            baseline_mean = clean_mean; baseline_std = clean_std;
            th_stall = baseline_mean + (3.0f * baseline_std);
            th_dry   = baseline_mean - (3.0f * baseline_std);
            if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;
            if (th_dry   < 0.0f) th_dry = 0.0f;
            is_calibrated = true;
            Serial.printf("\n[OBS] ══ AI Calibration Complete ══\n μ: %.2f mA | σ: %.2f mA | Stall: %.2f mA | Dry: %.2f mA\n", 
                          baseline_mean, baseline_std, th_stall, th_dry);
        }
        current_mode = "IDLE";
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin(); Wire.setClock(100000); delay(100);
    if (!ina219.begin()) { Serial.println("[OBS] CRITICAL: INA219 non trovato!"); while (1) delay(100); }

    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer; memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = 0; peer.encrypt = false;
    esp_now_add_peer(&peer);
}

void loop() {
    float raw_current = ina219.getCurrent_mA();
    float bus_voltage = ina219.getBusVoltage_V();
    
    // Ripristino I2C in caso di blocco hardware
    if (raw_current > 3000.0f || isnan(raw_current)) {
        Wire.end(); delay(10); Wire.setPins(I2C_SDA, I2C_SCL); Wire.begin(); Wire.setClock(100000);
        ina219.begin(); raw_current = 0.0f; bus_voltage = 0.0f;
    }

    last_current = max(0.0f, raw_current);
    last_voltage = max(0.0f, bus_voltage);

    // Filtro EWMA
    if (!ewma_init) { ewma_current = last_current; ewma_init = true; } 
    else { ewma_current = EWMA_ALPHA * last_current + (1.0f - EWMA_ALPHA) * ewma_current; }

    // ── CALCOLO LATENZA EDGE ──
    // Se siamo nei primi istanti di monitoraggio e la corrente supera il Noise Gate (50mA)
    if ((current_mode == "LEARNING" || current_mode == "MONITORING") && last_current > 50.0f && !latency_measured) {
        unsigned long edge_latency = millis() - sync_timer;
        Serial.printf("[METRIC] LATENZA EDGE (Avvio Elettrico -> Rilevamento): %lu ms\n", edge_latency);
        latency_measured = true; // Calcola solo una volta per ciclo
    }

    // ── GESTIONE STATI ──
    if (system_locked) {
        Serial.printf("[HALT] Sistema Bloccato. I=%.1f mA\n", last_current);
        char buf[128]; snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,HALTED,%s,%lu", last_current, last_voltage, anomaly_reason.c_str(), millis());
        espNowSend(buf);
        buzzerAlert(1); delay(5000); return;
    }

    if (current_mode == "LEARNING") {
        if (last_current > 50.0f && sample_idx < MAX_SAMPLES) {
            samples[sample_idx++] = last_current;
            Serial.printf("   [LEARN] #%d  I=%.2f mA\n", sample_idx, last_current);
        }
        delay(300); return;
    }

    if (current_mode == "MONITORING" && is_calibrated) {
        float z_score = (baseline_std > 1e-6f) ? (ewma_current - baseline_mean) / baseline_std : 0.0f;
        
        bool stall_flag   = (ewma_current > th_stall);
        bool dry_run_flag = (last_current > 10.0f && ewma_current < th_dry);
        bool anomaly = stall_flag || dry_run_flag;

        Serial.printf("   [MON] I_raw=%.1f  I_ewma=%.1f  Z=%+.2f  [%d/%d]%s%s\n",
                      last_current, ewma_current, z_score, anomaly_confirm, 3,
                      stall_flag ? " STALLO" : "", dry_run_flag ? " DRY-RUN" : "");

        if (anomaly) {
            anomaly_confirm++;
            if (anomaly_confirm >= 3) {
                if (stall_flag) anomaly_reason = "MOTOR_STALL";
                else anomaly_reason = "DRY_RUN";

                Serial.printf("\n[!!!] ANOMALIA CONFERMATA: %s\n", anomaly_reason.c_str());
                espNowSend("HALT"); // Kill-switch radio
                buzzerAlert(3);
                system_locked = true;
            }
        } else { anomaly_confirm = 0; }

        // Invio dati alla dashboard con TIMESTAMP
        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,MONITORING,OK,%lu", last_current, last_voltage, millis());
        espNowSend(buf);

        delay(400); return;
    }

    // Se il target dorme (IDLE)
    char buf[128]; snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,IDLE,OK,%lu", last_current, last_voltage, millis());
    espNowSend(buf);
    delay(1000);
}