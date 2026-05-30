/*
 * FLOAT - Observer Node (v9)
 * ===========================
 * Reads motor current via INA219, detects anomalies using EWMA + Hampel filter,
 * and communicates with the Target node over ESP-NOW (channel 13).
 *
 * Anomaly policy:
 *   MOTOR_STALL  — pump halted immediately (10× HALT burst, system_locked)
 *   DRY_RUN      — pump halted immediately (no water in system)
 *   VOLTAGE_DROP — buzzer warning only; pump keeps running
 *                  (momentary voltage dip should not interrupt filtration)
 *   TEMP_TOO_HIGH/LOW — buzzer warning only; operator must act on heater/cooler
 *
 * Z-score removed: thresholds are absolute (μ±kσ from Hampel calibration),
 * no need to recompute it on every tick.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>  // Required for advanced radio control
#include <math.h>
#include "dashboard.h"

// ── WiFi credentials ──────────────────────────────────────────────────────
// Observer joins this network as STA so the dashboard is reachable at the IP
// printed on serial at boot. The target also references this same SSID to
// discover the channel — no dedicated AP needed. To switch networks (e.g.
// home WiFi instead of hotspot) just edit these two constants in BOTH files
// in sync. If the network is not found at boot the system falls back to the
// hardcoded channel 13 used by the original v9 — ESP-NOW keeps working,
// just without dashboard.
const char* WIFI_SSID = "iPhone di Michele";
const char* WIFI_PASS = "Michele4!";

// ── Pin map ────────────────────────────────────────────────────────────────
const int I2C_SDA     = 41;
const int I2C_SCL     = 42;
const int BUZZER_PIN  = 7;

// ── Hardware ───────────────────────────────────────────────────────────────
Adafruit_INA219 ina219;
WebServer       server(80);   // HTTP server for the dashboard

// ── ESP-NOW peer (Target node MAC address) ────────────────────────────────
uint8_t targetAddress[] = {0x48, 0x27, 0xE2, 0xE2, 0xE3, 0x0C};

// ── System state ───────────────────────────────────────────────────────────
String  current_mode   = "IDLE";    // IDLE | LEARNING | MONITORING
bool    system_locked  = false;
String  anomaly_reason = "NONE";

// ── Learning / calibration ─────────────────────────────────────────────────
const int  MAX_SAMPLES = 60;
float      samples[MAX_SAMPLES];       // current samples (mA)
float      temp_samples[MAX_SAMPLES];  // temperature samples (°C) — independent counter
int        sample_idx      = 0;        // current sample count
int        temp_sample_idx = 0;        // temperature sample count (independent from current)
int        grace_period    = 0;

float baseline_mean      = 0.0f;
float baseline_std       = 0.0f;
float th_stall           = 0.0f;   // stall threshold:   μ_I + 3σ_I
float th_volt_min        = 0.0f;   // minimum healthy bus voltage (90 % of calib voltage)
float th_dry_run         = 0.0f;   // dry-run threshold: 30 % of baseline mean current
bool  is_calibrated      = false;
bool  new_temp_for_learn = false;  // set by DATA:SENSOR callback to trigger a combined I+V+T print

// ── Dynamic temperature thresholds ────────────────────────────────────────
const float TEMP_K_SIGMA      =  5.0f;  // anomaly multiplier on natural σ
const float TEMP_DELTA_MIN    =  1.5f;  // min deviation to trigger alarm (°C)
const float TEMP_ABSOLUTE_MAX = 32.0f;  // absolute backstop high
const float TEMP_ABSOLUTE_MIN = 16.0f;  // absolute backstop low

float th_temp_high = TEMP_ABSOLUTE_MAX;  // updated after calibration
float th_temp_low  = TEMP_ABSOLUTE_MIN;  // updated after calibration

// ── EWMA state ─────────────────────────────────────────────────────────────
const float EWMA_ALPHA   = 0.2f;
float       ewma_current = 0.0f;
bool        ewma_init    = false;

// ── Anomaly confirmation counters (Separated to prevent crosstalk) ─────────
int stall_confirm = 0;
int dry_confirm   = 0;
int volt_confirm  = 0;
int temp_confirm  = 0;
const int CONFIRM_NEEDED = 3;

// ── Latest sensor readings (forwarded to dashboard node) ──────────────────
float last_current  = 0.0f;
float last_voltage  = 0.0f;
float last_temp_c   = 25.0f;  // default; updated from Target DATA:SENSOR messages
float last_turb     = 0.0f;   // turbidity for dashboard (parsed from DATA:SENSOR)

// ── Event ring buffer for the dashboard ───────────────────────────────────
// Fixed char[] buffer, no heap/String — safe to write from inside the
// ESP-NOW receive callback. portMUX_TYPE guards inter-core access between
// the WiFi task (which runs the callback) and the main loop.
struct EvtEntry { uint32_t id; uint32_t ts; char type[14]; char msg[116]; };
const int EVT_MAX = 40;
EvtEntry  evt_buf[EVT_MAX];
int       evt_head = 0, evt_count = 0;
uint32_t  evt_seq  = 0;
portMUX_TYPE evt_mux = portMUX_INITIALIZER_UNLOCKED;

void push_event(const char* type, const char* msg) {
    portENTER_CRITICAL(&evt_mux);
    evt_seq++;
    EvtEntry& e = evt_buf[evt_head];
    e.id = evt_seq;
    e.ts = (uint32_t)millis();
    strncpy(e.type, type, 13); e.type[13] = '\0';
    strncpy(e.msg,  msg, 115); e.msg[115] = '\0';
    evt_head = (evt_head + 1) % EVT_MAX;
    if (evt_count < EVT_MAX) evt_count++;
    portEXIT_CRITICAL(&evt_mux);
}

// Classify Target LOG lines into dashboard categories by keyword.
void push_log_event(const char* txt) {
    const char* type = "log";
    if (strstr(txt, "[WARN]") || strstr(txt, "[HALT]") || strstr(txt, "[CRITICAL]"))
        type = "anomaly";
    push_event(type, txt);
}

// ── Helpers ────────────────────────────────────────────────────────────────

void computeStats(float* arr, int n, float& mean, float& std_dev) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += arr[i];
    mean = sum / n;

    float sq = 0.0f;
    for (int i = 0; i < n; i++) sq += powf(arr[i] - mean, 2);
    std_dev = sqrtf(sq / n);
}

// ── Hampel filter (replaces robustStats) ──────────────────────────────────
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

void espNowSend(const char* msg) {
    esp_now_send(targetAddress, (const uint8_t*)msg, strlen(msg));
}

void buzzerAlert(int times) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(150);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
}

// ── ESP-NOW receive callback ───────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    int id_pos = s.indexOf("|ID:");
    if (id_pos != -1) {
        uint32_t msg_id = (uint32_t)s.substring(id_pos + 4).toInt();
        s = s.substring(0, id_pos);   
        char ack_buf[32];
        snprintf(ack_buf, sizeof(ack_buf), "ACK:%lu", (unsigned long)msg_id);
        esp_now_send(mac, (const uint8_t*)ack_buf, strlen(ack_buf));
    }

    if (s.startsWith("LOG:")) {
        String log_text = s.substring(4);
        Serial.println(log_text);
        push_log_event(log_text.c_str());   // forward to dashboard

    } else if (s.startsWith("DATA:SENSOR:")) {
        String payload    = s.substring(12);
        int    commaIndex = payload.indexOf(',');
        if (commaIndex != -1) {
            float received_turb = payload.substring(0, commaIndex).toFloat();
            float received_temp = payload.substring(commaIndex + 1).toFloat();
            if (received_turb >= 0.0f) last_turb = received_turb;
            if (received_temp > -10.0f) {
                last_temp_c = received_temp;
                if (current_mode == "LEARNING" && temp_sample_idx < MAX_SAMPLES) {
                    temp_samples[temp_sample_idx++] = received_temp;
                    new_temp_for_learn = true;   
                }
            } else {
                Serial.printf("[OBS] WARNING: invalid temperature %.1f °C received — keeping last valid: %.1f °C\n",
                              received_temp, last_temp_c);
            }
        }

    } else if (s == "CMD:START_LEARN") {
        current_mode         = "LEARNING";
        sample_idx           = 0;
        temp_sample_idx      = 0;
        ewma_init            = false;
        new_temp_for_learn   = false;
        memset(temp_samples, 0, sizeof(temp_samples));
        Serial.println("[OBS] MODE → LEARNING");
        push_event("phase", "LEARNING — calibrating pump baseline");

    } else if (s == "CMD:START_MONITOR") {
        if (!is_calibrated) {
            Serial.println("[OBS] WARNING: not calibrated yet — monitoring skipped");
            return;
        }
        current_mode    = "MONITORING";
        
        stall_confirm = 0;
        dry_confirm   = 0;
        volt_confirm  = 0;
        temp_confirm  = 0;

        ewma_init       = false;  
        grace_period    = 4;
        Serial.println("[OBS] MODE → MONITORING");
        push_event("phase", "MONITORING — anomaly detection active");

    } else if (s == "CMD:STOP_MEASURE") {
        if (current_mode == "LEARNING" && sample_idx > 5) {
            float clean_mean, clean_std;
            hampelStats(samples, sample_idx, 3.0f, clean_mean, clean_std);

            baseline_mean = clean_mean;
            baseline_std  = clean_std;
            th_stall      = baseline_mean + (3.0f * baseline_std);
            if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;
            th_volt_min   = last_voltage * 0.90f;
            th_dry_run    = baseline_mean * 0.30f;

            if (temp_sample_idx >= 3) {
                float temp_mean, temp_std;
                hampelStats(temp_samples, temp_sample_idx, 3.0f, temp_mean, temp_std);

                float delta     = max(TEMP_K_SIGMA * temp_std, TEMP_DELTA_MIN);
                float cand_high = temp_mean + delta;
                float cand_low  = temp_mean - delta;

                th_temp_high = min(cand_high, TEMP_ABSOLUTE_MAX);
                th_temp_low  = max(cand_low,  TEMP_ABSOLUTE_MIN);

                Serial.println("\n[OBS] ── Temperature baseline ──");
                Serial.printf("   Temp samples : %d\n",         temp_sample_idx);
                Serial.printf("   Temp mean    : %.2f °C\n",    temp_mean);
                Serial.printf("   Temp std (σ) : %.2f °C\n",    temp_std);
                Serial.printf("   Delta used   : %.2f °C  (max(%.0f×σ=%.2f, min=%.1f))\n",
                              delta, TEMP_K_SIGMA, TEMP_K_SIGMA * temp_std, TEMP_DELTA_MIN);
                Serial.printf("   Alarm HIGH   : %.2f °C  (μ+delta=%.2f, backstop=%.0f)\n",
                              th_temp_high, cand_high, TEMP_ABSOLUTE_MAX);
                Serial.printf("   Alarm LOW    : %.2f °C  (μ-delta=%.2f, backstop=%.0f)\n",
                              th_temp_low,  cand_low,  TEMP_ABSOLUTE_MIN);
            } else {
                th_temp_high = TEMP_ABSOLUTE_MAX;
                th_temp_low  = TEMP_ABSOLUTE_MIN;
                Serial.printf("[OBS] WARNING: only %d temperature samples — using backstop limits (%.0f / %.0f °C)\n",
                              temp_sample_idx, TEMP_ABSOLUTE_MAX, TEMP_ABSOLUTE_MIN);
            }

            is_calibrated = true;

            Serial.println("\n[OBS] ══ Calibration complete (EWMA + Hampel) ══");
            Serial.printf("   Current samples : %d\n",               sample_idx);
            Serial.printf("   Temp samples    : %d\n",               temp_sample_idx);
            Serial.printf("   mean (μ)        : %.2f mA\n",          baseline_mean);
            Serial.printf("   std  (σ)        : %.2f mA\n",          baseline_std);
            Serial.printf("   Stall thr       : %.2f mA  (μ + 3σ)\n", th_stall);
            Serial.printf("   Dry-run         : %.2f mA  (30%% μ)\n",  th_dry_run);
            Serial.printf("   Volt min        : %.2f V\n",            th_volt_min);
            Serial.printf("   Temp alarm ↑    : %.2f °C\n",           th_temp_high);
            Serial.printf("   Temp alarm ↓    : %.2f °C\n",           th_temp_low);

            char buf[200];
            snprintf(buf, sizeof(buf), "CAL:%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                     baseline_mean, baseline_std, th_stall, th_dry_run,
                     th_temp_high, th_temp_low);
            espNowSend(buf);

            char notif[120];
            snprintf(notif, sizeof(notif),
                     "Calibration done — μ=%.1f mA  stall=%.1f  T[%.1f-%.1f]°C",
                     baseline_mean, th_stall, th_temp_low, th_temp_high);
            push_event("calibration", notif);
        }
        current_mode = "IDLE";
        push_event("phase", "IDLE — waiting for next target cycle");

    } else if (s == "CMD:RESET") {
        system_locked   = false;
        anomaly_reason  = "NONE";
        
        stall_confirm = 0;
        dry_confirm   = 0;
        volt_confirm  = 0;
        temp_confirm  = 0;

        ewma_init       = false;
        current_mode    = "IDLE";
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("[OBS] System reset by dashboard command");
    }
}

// ── HTTP handlers ─────────────────────────────────────────────────────────
// HTML is streamed in 1 KB chunks via sendContent_P to avoid a 7 KB heap
// allocation on every page load (which under load would fragment the heap
// and trigger watchdog resets).
void handleRoot() {
    size_t total = strlen(html_page);
    server.setContentLength(total);
    server.send(200, "text/html", "");
    const char* p = html_page;
    size_t left = total;
    while (left > 0) {
        size_t n = left > 1024 ? 1024 : left;
        server.sendContent_P(p, n);
        p += n;
        left -= n;
        yield();
    }
}

void handleData() {
    char buf[400];
    snprintf(buf, sizeof(buf),
        "{\"I\":%.2f,\"I_ewma\":%.2f,\"V\":%.2f,\"T\":%.2f,\"turb\":%.1f"
        ",\"mode\":\"%s\",\"locked\":%s"
        ",\"th_stall\":%.2f,\"th_dry\":%.2f,\"th_thi\":%.2f,\"th_tlo\":%.2f"
        ",\"evt_seq\":%lu}",
        last_current, ewma_current, last_voltage, last_temp_c, last_turb,
        current_mode.c_str(),
        system_locked ? "true" : "false",
        th_stall, th_dry_run, th_temp_high, th_temp_low,
        (unsigned long)evt_seq);
    server.send(200, "application/json", buf);
}

void handleEvents() {
    uint32_t last_id = server.hasArg("last")
                       ? (uint32_t)server.arg("last").toInt() : 0;
    // Snapshot under critical section, then format JSON outside it.
    static EvtEntry snap[EVT_MAX];
    int snap_count, snap_start;
    portENTER_CRITICAL(&evt_mux);
    snap_count = evt_count;
    snap_start = (evt_head - evt_count + EVT_MAX) % EVT_MAX;
    for (int i = 0; i < snap_count; i++) snap[i] = evt_buf[(snap_start + i) % EVT_MAX];
    portEXIT_CRITICAL(&evt_mux);

    static char buf[2048];
    int pos = 0, sent = 0;
    buf[pos++] = '[';
    bool first = true;
    for (int i = 0; i < snap_count && sent < 15; i++) {
        const EvtEntry& e = snap[i];
        if (e.id <= last_id) continue;
        char safe[120]; int si = 0;
        for (int k = 0; e.msg[k] && si < 116; k++) {
            if (e.msg[k] == '"' || e.msg[k] == '\\') safe[si++] = '\\';
            safe[si++] = e.msg[k];
        }
        safe[si] = '\0';
        int w = snprintf(buf + pos, (int)sizeof(buf) - pos - 4,
            "%s{\"id\":%lu,\"ts\":%lu,\"type\":\"%s\",\"msg\":\"%s\"}",
            first ? "" : ",",
            (unsigned long)e.id, (unsigned long)e.ts, e.type, safe);
        if (w <= 0 || pos + w >= (int)sizeof(buf) - 4) break;
        pos += w; first = false; sent++;
    }
    buf[pos++] = ']'; buf[pos] = '\0';
    server.send(200, "application/json", buf);
}

// smartDelay: like delay() but keeps the WebServer responsive. We replace the
// blocking delay() calls in loop() with this so HTTP polling is never
// starved while the observer waits between sensor ticks. Logic and timing
// of loop() are unchanged.
void smartDelay(unsigned long ms) {
    unsigned long t0 = millis();
    do {
        server.handleClient();
        delay(2);
    } while (millis() - t0 < ms);
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup() {
    delay(3000);
    Serial.begin(115200);
    Serial.println("\n\n[SYS] Serial connected. Booting...");

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

    // ── WiFi STA + ESP-NOW ─────────────────────────────────────────────────
    // STA-only mode (no AP) so the dashboard is reachable on the user's
    // existing network. The radio's channel is dictated by the router/hotspot;
    // we read it back and pin ESP-NOW to that same channel. The target does
    // the same scan independently, so both ESP32s converge on the same
    // channel without any dedicated discovery AP.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // disable modem sleep — critical for reliable ESP-NOW
                            // reception while connected to a WiFi AP (matches what
                            // target.cpp already does on its own setup)
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[OBS] Connecting to '%s'", WIFI_SSID);
    unsigned long t0_wifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0_wifi < 15000) {
        delay(400); Serial.print(".");
    }

    uint8_t esp_now_channel = 13;   // fallback (matches the v9 default)
    if (WiFi.status() == WL_CONNECTED) {
        uint8_t pri; wifi_second_chan_t sec;
        esp_wifi_get_channel(&pri, &sec);
        esp_now_channel = pri;
        Serial.printf("\n[OBS] WiFi OK — channel %d, dashboard at http://%s\n",
                      esp_now_channel, WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[OBS] WiFi not found — fallback channel 13 (no dashboard)");
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("[OBS] ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = esp_now_channel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // Register HTTP routes; quick 204 for /favicon.ico and other unknowns.
    server.on("/",            HTTP_GET, handleRoot);
    server.on("/data",        HTTP_GET, handleData);
    server.on("/events",      HTTP_GET, handleEvents);
    // Browsers automatically request /favicon.ico — registering an explicit
    // empty handler silences the WebServer "request handler not found" log.
    server.on("/favicon.ico", HTTP_GET, [](){ server.send(204, "text/plain", ""); });
    server.onNotFound([](){ server.send(204, "text/plain", ""); });
    server.begin();

    push_event("phase", "IDLE — system online");

    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║  FLOAT  –  Observer Node (v9)               ║");
    Serial.println("║  EWMA+Hampel | DRY_RUN | Dynamic TEMP       ║");
    Serial.println("║  VOLT/TEMP = warning only (no halt)         ║");
    Serial.println("╚══════════════════════════════════════════════╝");
}

// ── loop ───────────────────────────────────────────────────────────────────
void loop() {
    server.handleClient();   // serve dashboard HTTP traffic every iteration

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
        Serial.printf("[HALT] Locked. I=%.1f mA  V=%.2f V  T=%.1f C\n",
                      last_current, last_voltage, last_temp_c);
        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,HALTED,%s",
                 last_current, last_voltage, last_temp_c, anomaly_reason.c_str());
        espNowSend(buf);
        buzzerAlert(1);
        smartDelay(5000);
        return;
    }

    if (current_mode == "LEARNING") {
        if (last_current > 50.0f && sample_idx < MAX_SAMPLES) {
            samples[sample_idx++] = last_current;
        }

        if (new_temp_for_learn) {
            new_temp_for_learn = false;
            int row = temp_sample_idx; 
            Serial.printf("   [LEARN] #%d  I=%.2f mA  V=%.2f V  T=%.2f C\n",
                          row, last_current, last_voltage, last_temp_c);
        }
        smartDelay(300);
        return;
    }

    if (current_mode == "MONITORING" && is_calibrated) {
        if (grace_period > 0) {
            grace_period--;
            Serial.printf("   [MON] Motor inrush — alarms suppressed (%d left)  I_raw=%.1f mA\n",
                          grace_period, last_current);
            if (grace_period == 0) {
                ewma_current = last_current;
                Serial.printf("   [MON] Grace period ended — EWMA seeded at %.1f mA\n", ewma_current);
            }
            smartDelay(400);
            return;
        }

        bool stall_flag     = (ewma_current > th_stall);
        bool volt_flag      = (th_volt_min > 0.1f && last_voltage < th_volt_min);
        bool dry_flag       = (th_dry_run  > 0.1f && ewma_current < th_dry_run);
        
        bool temp_high_flag = false;
        bool temp_low_flag  = false;
        
        if (last_temp_c != -127.0f && last_temp_c > -10.0f) {
            temp_high_flag = (last_temp_c > th_temp_high);
            temp_low_flag  = (last_temp_c < th_temp_low);
        }
        bool temp_flag = temp_high_flag || temp_low_flag;

        if (stall_flag) stall_confirm++; else stall_confirm = 0;
        if (volt_flag)  volt_confirm++;  else volt_confirm = 0;
        if (dry_flag)   dry_confirm++;   else dry_confirm = 0;
        if (temp_flag)  temp_confirm++;  else temp_confirm = 0;

        bool anomaly = (stall_confirm >= CONFIRM_NEEDED) || 
                       (volt_confirm >= CONFIRM_NEEDED) || 
                       (dry_confirm >= CONFIRM_NEEDED) || 
                       (temp_confirm >= CONFIRM_NEEDED);

        Serial.printf("   [MON] I_raw=%.1f  I_ewma=%.1f  V=%.2f  T=%.1f"
                      "  thr[↑%.1f↓%.1f]  [S:%d V:%d D:%d T:%d]%s%s%s%s%s\n",
                      last_current, ewma_current, last_voltage, last_temp_c,
                      th_temp_high, th_temp_low,
                      stall_confirm, volt_confirm, dry_confirm, temp_confirm,
                      stall_flag      ? " STALL"     : "",
                      volt_flag       ? " VOLT-LOW"  : "",
                      dry_flag        ? " DRY-RUN"   : "",
                      temp_high_flag  ? " TEMP-HIGH" : "",
                      temp_low_flag   ? " TEMP-LOW"  : "");

        if (anomaly) {
            if (stall_confirm >= CONFIRM_NEEDED)      anomaly_reason = "MOTOR_STALL";
            else if (dry_confirm >= CONFIRM_NEEDED)   anomaly_reason = "DRY_RUN";
            else if (volt_confirm >= CONFIRM_NEEDED)  anomaly_reason = "VOLTAGE_DROP";
            else if (temp_high_flag)                  anomaly_reason = "TEMP_TOO_HIGH";
            else                                      anomaly_reason = "TEMP_TOO_LOW";

            Serial.printf("\n[!!!] ANOMALY CONFIRMED: %s\n", anomaly_reason.c_str());
            Serial.printf("   I_ewma=%.2f mA  V=%.2f V  T=%.1f C"
                          "  (thr ↑%.1f ↓%.1f)\n",
                          ewma_current, last_voltage, last_temp_c,
                          th_temp_high, th_temp_low);

            // MOTOR_STALL and DRY_RUN halt the system immediately — pump must stop.
            // VOLTAGE_DROP, TEMP_TOO_HIGH, TEMP_TOO_LOW are warnings:
            //   buzzer alerts the operator but the pump keeps running, because:
            //   - Low voltage may be a momentary dip (battery, wiring); stopping
            //     the pump mid-cycle could leave the aquarium unfiltered.
            //   - Temperature is outside the safe range but the pump itself is healthy;
            //     the operator needs to intervene (heater/cooler), not stop the pump.
            if (anomaly_reason == "MOTOR_STALL" || anomaly_reason == "DRY_RUN") {
                for (int i = 0; i < 10; i++) { espNowSend("HALT"); delay(5); }
                system_locked = true;
            }

            char alert[64];
            snprintf(alert, sizeof(alert), "ALERT:%s", anomaly_reason.c_str());
            espNowSend(alert);

            buzzerAlert(3);
            
            // For non-halting anomalies, reset their confirm counter so the next
            // check starts fresh (avoids a continuous stream of alerts).
            if (!system_locked) {
                volt_confirm = 0;
                temp_confirm = 0;
            }
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,MONITORING,OK",
                 last_current, last_voltage, last_temp_c);
        espNowSend(buf);

        smartDelay(400);
        return;
    }

    Serial.printf("[IDLE] I=%.2f mA  V=%.2f V  T=%.1f C\n",
                  last_current, last_voltage, last_temp_c);
    char buf[128];
    snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,IDLE,OK",
             last_current, last_voltage, last_temp_c);
    espNowSend(buf);

    smartDelay(2000);
}