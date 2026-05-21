/*
 * FLOAT - Observer Node
 * =====================
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h> // <--- AGGIUNTA: Necessaria per il controllo avanzato della radio
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
float th_dry_run       = 0.0f;  // §7A: 30% di baseline_mean
bool  is_calibrated    = false;

// ── Soglie temperatura (§4) ────────────────────────────────────────────────
const float TEMP_MIN_C = 18.0f;
const float TEMP_MAX_C = 32.0f;

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

void computeStats(float* arr, int n, float& mean, float& std_dev) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += arr[i];
    mean = sum / n;

    float sq = 0.0f;
    for (int i = 0; i < n; i++) sq += powf(arr[i] - mean, 2);
    std_dev = sqrtf(sq / n);
}

// ── Hampel helpers (§1 – sostituisce robustStats) ─────────────────────────
// Usa MAD (Median Absolute Deviation) invece della 3σ classica:
// robusta agli outlier per costruzione, non distorce la baseline.

float arrayMedian(float* arr, int n) {
    float buf[n];
    memcpy(buf, arr, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float key = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = key;
    }
    return (n % 2 == 0) ? (buf[n/2-1] + buf[n/2]) / 2.0f : buf[n/2];
}

void hampelStats(float* arr, int n, float k_sigma,
                 float& clean_mean, float& clean_std) {
    if (n < 3) { computeStats(arr, n, clean_mean, clean_std); return; }

    float med = arrayMedian(arr, n);

    float devs[n];
    for (int i = 0; i < n; i++) devs[i] = fabsf(arr[i] - med);

    float mad     = arrayMedian(devs, n);
    float sigma_h = 1.4826f * mad;   // stimatore consistente di σ

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

void espNowSend(const char* msg) {
    esp_now_send(targetAddress, (const uint8_t*)msg, strlen(msg));
}

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

    // §2 – ACK handshake: se il messaggio contiene |ID:N|, risponde ACK:N al mittente
    int id_pos = s.indexOf("|ID:");
    if (id_pos != -1) {
        uint32_t msg_id = (uint32_t)s.substring(id_pos + 4).toInt();
        s = s.substring(0, id_pos);   // tronca il suffisso prima del parsing
        char ack_buf[32];
        snprintf(ack_buf, sizeof(ack_buf), "ACK:%lu", (unsigned long)msg_id);
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
        grace_period = 4;
        Serial.println("[OBS] MODE → MONITORING");

    } else if (s == "CMD:STOP_MEASURE") {
            if (current_mode == "LEARNING" && sample_idx > 5) {
                float clean_mean, clean_std;
                hampelStats(samples, sample_idx, 3.0f, clean_mean, clean_std); // §1

                baseline_mean = clean_mean;
                baseline_std  = clean_std;
                th_stall    = baseline_mean + (3.0f * baseline_std);
                if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;
                th_volt_min = last_voltage * 0.90f;
                th_dry_run  = baseline_mean * 0.30f;  // §7A: 30% del consumo normale
                is_calibrated = true;

                Serial.println("\n[OBS] ══ Calibration Complete (EWMA + Hampel) ══");
                Serial.printf("   Samples   : %d\n",              sample_idx);
                Serial.printf("   μ (mean)  : %.2f mA\n",         baseline_mean);
                Serial.printf("   σ (std)   : %.2f mA\n",         baseline_std);
                Serial.printf("   Stall thr : %.2f mA  (μ + 3σ)\n", th_stall);
                Serial.printf("   Dry-run   : %.2f mA  (30%% μ)\n",  th_dry_run);
                Serial.printf("   Volt min  : %.2f V\n",           th_volt_min);

                char buf[160];
                snprintf(buf, sizeof(buf), "CAL:%.2f,%.2f,%.2f,%.2f",
                         baseline_mean, baseline_std, th_stall, th_dry_run);
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
    delay(3000); 
    Serial.println("\n\n[SISTEMA] Seriale Connessa! Avvio boot...");

    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    Wire.setClock(100000);
    delay(100);
    if (!ina219.begin()) {
        Serial.println("[OBS] CRITICAL: INA219 not found!");
        while (1) delay(100);
    }

    // --- MODIFICA 1 E 2: BLOCCO CANALE E POTENZA MASSIMA ---
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // Potenza massima
    
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE); // Tunnel sul canale 13
    esp_wifi_set_promiscuous(false);
    // -------------------------------------------------------

    if (esp_now_init() != ESP_OK) {
        Serial.println("[OBS] ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = 13; // <-- Deve seguire il canale forzato
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║  FLOAT  –  Observer Node (v3)               ║");
    Serial.println("║  Anomaly: EWMA + Hampel + DRY_RUN + TEMP    ║");
    Serial.println("╚══════════════════════════════════════════════╝");
}

// ── loop ───────────────────────────────────────────────────────────────────
void loop() {
    float raw_current = ina219.getCurrent_mA();
    float bus_voltage = ina219.getBusVoltage_V();

    if (raw_current > 3000.0f || isnan(raw_current)) {
        Wire.end(); delay(10);
        Wire.setPins(I2C_SDA, I2C_SCL); Wire.begin(); Wire.setClock(100000);
        ina219.begin();
        raw_current = 0.0f;
        bus_voltage = 0.0f;
    }

    last_current = max(0.0f, raw_current);
    last_voltage = max(0.0f, bus_voltage);

    if (!ewma_init) {
        ewma_current = last_current;
        ewma_init    = true;
    } else {
        ewma_current = EWMA_ALPHA * last_current + (1.0f - EWMA_ALPHA) * ewma_current;
    }

    if (system_locked) {
        Serial.printf("[HALT] Locked. I=%.1f mA  V=%.2f V  T=%.1f°C\n", last_current, last_voltage, last_temp_c);
        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,HALTED,%s", last_current, last_voltage, last_temp_c, anomaly_reason.c_str());
        espNowSend(buf);
        buzzerAlert(1);
        delay(5000);
        return;
    }

    if (current_mode == "LEARNING") {
        if (last_current > 50.0f && sample_idx < MAX_SAMPLES) {
            samples[sample_idx++] = last_current;
            Serial.printf("   [LEARN] #%d  I=%.2f mA  V=%.2f V\n", sample_idx, last_current, last_voltage);
        }
        delay(300);
        return;
    }

    if (current_mode == "MONITORING" && is_calibrated) {
        if (grace_period > 0) {
            grace_period--;
            Serial.printf("   [MON] SPUNTO MOTORE (ignoro allarmi)... I_raw=%.1f mA\n", last_current);
            delay(400);
            return; 
        }
        float z_score   = (baseline_std > 1e-6f) ? (ewma_current - baseline_mean) / baseline_std : 0.0f;
        bool stall_flag = (ewma_current > th_stall);
        bool volt_flag  = (th_volt_min > 0.1f && last_voltage < th_volt_min);
        bool dry_flag   = (th_dry_run  > 0.1f && ewma_current < th_dry_run);   // §7A
        bool temp_flag  = (last_temp_c < TEMP_MIN_C || last_temp_c > TEMP_MAX_C); // §4
        bool anomaly    = stall_flag || volt_flag || dry_flag || temp_flag;

        Serial.printf("   [MON] I_raw=%.1f  I_ewma=%.1f  Z=%+.2f  V=%.2f  T=%.1f  [%d/%d]%s%s%s%s\n",
                      last_current, ewma_current, z_score, last_voltage, last_temp_c,
                      anomaly_confirm, CONFIRM_NEEDED,
                      stall_flag ? " STALL"    : "",
                      volt_flag  ? " VOLT-LOW" : "",
                      dry_flag   ? " DRY-RUN"  : "",
                      temp_flag  ? " TEMP-ERR" : "");

        if (anomaly) {
            anomaly_confirm++;
            if (anomaly_confirm >= CONFIRM_NEEDED) {
                // Priorità: stall > dry-run > tensione > temperatura
                if      (stall_flag) anomaly_reason = "MOTOR_STALL";
                else if (dry_flag)   anomaly_reason = "DRY_RUN";
                else if (volt_flag)  anomaly_reason = "VOLTAGE_DROP";
                else                 anomaly_reason = "TEMP_OUT_OF_RANGE";

                Serial.printf("\n[!!!] ANOMALY CONFIRMED: %s\n", anomaly_reason.c_str());
                Serial.printf("   I_ewma=%.2f mA  Z=%+.2f  V=%.2f V  T=%.1f°C\n",
                              ewma_current, z_score, last_voltage, last_temp_c);

                // --- MODIFICA 3: RAFFICA DI EMERGENZA (Anti-Interferenze) ---
                for (int i = 0; i < 10; i++) {
                    espNowSend("HALT");
                    delay(5);
                }
                // ------------------------------------------------------------

                char alert[64];
                snprintf(alert, sizeof(alert), "ALERT:%s", anomaly_reason.c_str());
                espNowSend(alert);

                buzzerAlert(3);
                system_locked = true;
            }
        } else {
            anomaly_confirm = 0;
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,MONITORING,OK", last_current, last_voltage, last_temp_c);
        espNowSend(buf);

        delay(400);
        return;
    }

    Serial.printf("[IDLE] I=%.2f mA  V=%.2f V  T=%.1f°C\n", last_current, last_voltage, last_temp_c);
    char buf[128];
    snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,IDLE,OK", last_current, last_voltage, last_temp_c);
    espNowSend(buf);

    delay(2000);
}