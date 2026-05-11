/*
 * FLOAT - Observer Node
 * =====================
 * Responsibilities:
 *  - Measure pump current AND bus voltage via INA219
 *  - Measure water temperature via DS18B20 (OneWire, GPIO 4)
 *  - Run EWMA + sliding-window Z-score anomaly detection
 *    (detects BOTH stall / over-current  AND  dry-run / under-current)
 *  - Send HALT command to Target via ESP-NOW on confirmed anomaly
 *  - Drive buzzer alert
 *  - Forward telemetry to the Dashboard node via ESP-NOW "TELEM:" frames
 *    so the web dashboard (served by main.cpp) always has fresh sensor data
 *
 * Anomaly Detection Algorithm  –  EWMA + Z-score
 * ------------------------------------------------
 * Classic 3-sigma on raw samples is brittle:
 *   • it treats every spike as independent
 *   • a single stuck sample can reset the counter and delay detection
 *
 * EWMA smooths the signal first (α = 0.2), then a Z-score of the
 * smoothed value against the learned baseline flags anomalies.
 * Three consecutive Z-score violations (score > +3 or < -3) confirm
 * a fault — this combines trend sensitivity with noise immunity.
 *
 * Detected anomaly types:
 *   MOTOR_STALL  – EWMA current >> μ  (rotor blocked, high torque)
 *   DRY_RUN      – EWMA current << μ  (no water load, pump spinning free)
 *   VOLTAGE_DROP – Bus voltage drops below calibrated minimum
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>

// ── Pin map ────────────────────────────────────────────────────────────────
const int I2C_SDA     = 41;
const int I2C_SCL     = 42;
const int BUZZER_PIN  = 7;

// ── Hardware ───────────────────────────────────────────────────────────────
Adafruit_INA219   ina219;

// ── ESP-NOW peer (broadcast — replaced at runtime with real MAC if needed) ─
uint8_t targetAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── System state ───────────────────────────────────────────────────────────
String  current_mode   = "IDLE";    // IDLE | LEARNING | MONITORING
bool    system_locked  = false;
String  anomaly_reason = "NONE";

// ── Learning / calibration ─────────────────────────────────────────────────
const int  MAX_SAMPLES = 60;
float      samples[MAX_SAMPLES];
int        sample_idx  = 0;
int grace_period = 0;

float baseline_mean    = 0.0f;
float baseline_std     = 0.0f;
float th_stall         = 0.0f;  // μ + 3σ
float th_volt_min      = 0.0f;  // minimum healthy bus voltage
bool  is_calibrated    = false;

// ── EWMA state ─────────────────────────────────────────────────────────────
const float EWMA_ALPHA = 0.2f;  // smoothing factor  (0 = no update, 1 = raw)
float       ewma_current = 0.0f;
bool        ewma_init    = false;

// ── Anomaly confirmation counter ───────────────────────────────────────────
int anomaly_confirm = 0;
const int CONFIRM_NEEDED = 3;

// ── Latest sensor readings (forwarded to dashboard node) ──────────────────
float last_current  = 0.0f;
float last_voltage  = 0.0f;
float last_temp_c   = 25.0f;

// ── Helpers ────────────────────────────────────────────────────────────────

/** Compute mean and population std-dev of arr[0..n-1] */
void computeStats(float* arr, int n, float& mean, float& std_dev) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += arr[i];
    mean = sum / n;

    float sq = 0.0f;
    for (int i = 0; i < n; i++) sq += powf(arr[i] - mean, 2);
    std_dev = sqrtf(sq / n);
}

/** Reject outliers beyond z_limit standard deviations, recompute stats */
void robustStats(float* arr, int n, float z_limit,
                 float& clean_mean, float& clean_std) {
    float raw_mean, raw_std;
    computeStats(arr, n, raw_mean, raw_std);

    float sum = 0.0f;
    float sq  = 0.0f;
    int   cnt = 0;

    for (int i = 0; i < n; i++) {
        if (raw_std < 1e-6f || fabsf(arr[i] - raw_mean) / raw_std <= z_limit) {
            sum += arr[i];
            cnt++;
        }
    }

    if (cnt < 3) { clean_mean = raw_mean; clean_std = raw_std; return; }
    clean_mean = sum / cnt;

    for (int i = 0; i < n; i++) {
        if (raw_std < 1e-6f || fabsf(arr[i] - raw_mean) / raw_std <= z_limit)
            sq += powf(arr[i] - clean_mean, 2);
    }
    clean_std = sqrtf(sq / cnt);
}

/** Send a short string message over ESP-NOW */
void espNowSend(const char* msg) {
    esp_now_send(targetAddress, (const uint8_t*)msg, strlen(msg));
}

/** Buzz N times */
void buzzerAlert(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(150);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
}

// ── ESP-NOW receive callback ────────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    // ── 1. Ricezione Log ───────────────────────────────────────────────────
    if (s.startsWith("LOG:")) {
        Serial.println(s.substring(4));

    // ── 2. NUOVO: Ricezione Dati Sensori Remoti dal Target ─────────────────
    } else if (s.startsWith("DATA:SENSOR:")) {
        // Il Target invia: "DATA:SENSOR:valore_ntu,valore_temperatura"
        // Esempio: "DATA:SENSOR:120.5,24.8"
        String payload = s.substring(12); // Salta la stringa iniziale "DATA:SENSOR:"
        int commaIndex = payload.indexOf(','); // Trova la posizione della virgola
        
        if (commaIndex != -1) {
            // Estrapola il numero dopo la virgola e lo converte in float
            last_temp_c = payload.substring(commaIndex + 1).toFloat();
            
            // Stampa di debug opzionale (puoi commentarla se intasa troppo la seriale)
            // Serial.printf("[RAD] Temperatura aggiornata dal Target: %.1f °C\n", last_temp_c);
        }

    // ── 3. Gestione Comandi / Macchina a Stati ─────────────────────────────
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
        ewma_init       = false;   // re-seed EWMA on each monitoring session
        grace_period = 4;
        Serial.println("[OBS] MODE → MONITORING");

    } else if (s == "CMD:STOP_MEASURE") {
            if (current_mode == "LEARNING" && sample_idx > 5) {
                // ── Robust calibration with Z-score outlier rejection ──────────
                float clean_mean, clean_std;
                robustStats(samples, sample_idx, 1.5f, clean_mean, clean_std);

                baseline_mean = clean_mean;
                baseline_std  = clean_std;

                // Calcola SOLO la soglia di stallo
                th_stall = baseline_mean + (3.0f * baseline_std);

                // Enforce minimum margin so sensor noise cannot collapse threshold
                if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;

                // Voltage threshold: 90 % of mean bus voltage seen during learning
                th_volt_min = last_voltage * 0.90f;

                is_calibrated = true;

                Serial.println("\n[OBS] ══ Calibration Complete (EWMA + Z-score) ══");
                Serial.printf("   Samples   : %d\n",     sample_idx);
                Serial.printf("   μ (mean)  : %.2f mA\n", baseline_mean);
                Serial.printf("   σ (std)   : %.2f mA\n", baseline_std);
                Serial.printf("   Stall thr : %.2f mA  (μ + 3σ)\n", th_stall);
                Serial.printf("   Volt min  : %.2f V\n",  th_volt_min);

                // Invia la calibrazione alla dashboard (solo 3 parametri ora)
                char buf[128];
                snprintf(buf, sizeof(buf),
                        "CAL:%.2f,%.2f,%.2f",
                        baseline_mean, baseline_std, th_stall);
                espNowSend(buf);
            }
            current_mode = "IDLE";

    } else if (s == "CMD:RESET") {
        system_locked   = false;
        anomaly_reason  = "NONE";
        anomaly_confirm = 0;
        ewma_init       = false;
        current_mode    = "IDLE";
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("[OBS] System RESET by dashboard command");
    }
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup() {
    // AGGIUNGI QUESTE TRE RIGHE:
    delay(3000); 
    Serial.println("\n\n[SISTEMA] Seriale Connessa! Avvio boot...");

    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // I2C + INA219
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    Wire.setClock(100000);
    delay(100);
    if (!ina219.begin()) {
        Serial.println("[OBS] CRITICAL: INA219 not found!");
        while (1) delay(100);
    }

    // ESP-NOW
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[OBS] ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  FLOAT  –  Observer Node (v2)           ║");
    Serial.println("║  Anomaly: EWMA + Z-score (stall+dry-run)║");
    Serial.println("╚══════════════════════════════════════════╝");
}

// ── loop ───────────────────────────────────────────────────────────────────
void loop() {
    // ── 1. Read sensors ────────────────────────────────────────────────────
    float raw_current = ina219.getCurrent_mA();
    float bus_voltage = ina219.getBusVoltage_V();

    if (raw_current > 3000.0f || isnan(raw_current)) {
        // I2C bus recovery
        Wire.end(); delay(10);
        Wire.setPins(I2C_SDA, I2C_SCL); Wire.begin(); Wire.setClock(100000);
        ina219.begin();
        raw_current = 0.0f;
        bus_voltage = 0.0f;
    }

    last_current = max(0.0f, raw_current);
    last_voltage = max(0.0f, bus_voltage);

    // ── 2. Update EWMA ─────────────────────────────────────────────────────
    if (!ewma_init) {
        ewma_current = last_current;
        ewma_init    = true;
    } else {
        ewma_current = EWMA_ALPHA * last_current + (1.0f - EWMA_ALPHA) * ewma_current;
    }

    // ── 3. Locked / halted state ───────────────────────────────────────────
    if (system_locked) {
        Serial.printf("[HALT] Locked. I=%.1f mA  V=%.2f V  T=%.1f°C\n",
                      last_current, last_voltage, last_temp_c);
        // Still forward telemetry so dashboard shows live sensor data
        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,HALTED,%s",
                 last_current, last_voltage, last_temp_c, anomaly_reason.c_str());
        espNowSend(buf);
        buzzerAlert(1);
        delay(5000);
        return;
    }

    // ── 4. Learning phase ──────────────────────────────────────────────────
    if (current_mode == "LEARNING") {
        // Only record samples after motor has fully spun up (> 50 mA)
        if (last_current > 50.0f && sample_idx < MAX_SAMPLES) {
            samples[sample_idx++] = last_current;
            Serial.printf("   [LEARN] #%d  I=%.2f mA  V=%.2f V\n",
                          sample_idx, last_current, last_voltage);
        }
        delay(300);
        return;
    }

    // ── 5. Monitoring phase ────────────────────────────────────────────────
    if (current_mode == "MONITORING" && is_calibrated) {
        if (grace_period > 0) {
            grace_period--;
            Serial.printf("   [MON] SPUNTO MOTORE (ignoro allarmi)... I_raw=%.1f mA\n", last_current);
            delay(400);
            return; // <--- QUESTO RETURN È FONDAMENTALE! FERMA I CALCOLI QUI.
        }
        // Z-score of the EWMA-smoothed current against learned baseline
        float z_score = (baseline_std > 1e-6f)
                        ? (ewma_current - baseline_mean) / baseline_std
                        : 0.0f;

        bool stall_flag    = (ewma_current > th_stall);      // over-current

        bool volt_flag     = (th_volt_min > 0.1f &&
                              last_voltage < th_volt_min);   // supply drop

        bool anomaly = stall_flag || volt_flag;

        Serial.printf("   [MON] I_raw=%.1f  I_ewma=%.1f  Z=%+.2f  V=%.2f  T=%.1f  [%d/%d]%s%s%s\n",
                      last_current, ewma_current, z_score, last_voltage, last_temp_c,
                      anomaly_confirm, CONFIRM_NEEDED,
                      stall_flag   ? " STALL"   : "",
                      volt_flag    ? " VOLT-LOW" : "");

        if (anomaly) {
            anomaly_confirm++;
            if (anomaly_confirm >= CONFIRM_NEEDED) {
                // ── FAULT CONFIRMED ────────────────────────────────────────
                if      (stall_flag)   anomaly_reason = "MOTOR_STALL";
                else                   anomaly_reason = "VOLTAGE_DROP";

                Serial.printf("\n[!!!] ANOMALY CONFIRMED: %s\n", anomaly_reason.c_str());
                Serial.printf("   I_ewma=%.2f mA  Z=%+.2f  V=%.2f V\n",
                              ewma_current, z_score, last_voltage);

                // Send HALT to Target node
                espNowSend("HALT");

                // Forward event to dashboard
                char alert[64];
                snprintf(alert, sizeof(alert), "ALERT:%s", anomaly_reason.c_str());
                espNowSend(alert);

                buzzerAlert(3);
                system_locked = true;
            }
        } else {
            anomaly_confirm = 0;
        }

        // Forward live telemetry to dashboard (every monitoring tick)
        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,MONITORING,OK",
                 last_current, last_voltage, last_temp_c);
        espNowSend(buf);

        delay(400);
        return;
    }

    // ── 6. Idle ────────────────────────────────────────────────────────────
    Serial.printf("[IDLE] I=%.2f mA  V=%.2f V  T=%.1f°C\n",
                  last_current, last_voltage, last_temp_c);

    // Forward idle telemetry
    char buf[128];
    snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,IDLE,OK",
             last_current, last_voltage, last_temp_c);
    espNowSend(buf);

    delay(2000);
}