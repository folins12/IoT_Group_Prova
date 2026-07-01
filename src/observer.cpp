// FLOAT - Observer Node (ESP32)
// Measures the Target's supply current with an INA219, detects anomalies (SPC: Shewhart + CUSUM),
// serves the local dashboard, and bridges to the cloud over MQTT + email alerts.
// Anomaly policy: MOTOR_STALL / DRY_RUN -> halt; VOLTAGE_DROP / TEMP_TOO_HIGH/LOW -> warning only.
//
// Detection design (Statistical Process Control):
//   MOTOR_STALL  : upper Shewhart control limit  mu + 3*sigma          (large, abrupt jump)
//   DRY_RUN      : lower one-sided CUSUM on pump current               (sustained under-current)
//   DEGRADATION  : cumulative CUSUM on per-cycle healthy mean          (slow drift, predictive)
//   TEMP high/low: adaptive control band  mu_T +/- max(5*sigma_T,1.5)  (learned per tank)
//   VOLTAGE_DROP : PHYSICAL battery cutoff  (NOT a statistical limit: voltage drifts with SoC)

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>
#include "dashboard.h"

// WiFi (edit in sync with target.cpp; fallback channel 13 if not found)
const char* WIFI_SSID = "Pixel_3478";
const char* WIFI_PASS = "sounciocco";

// MQTT cloud bridge (HiveMQ Cloud, private + authenticated, TLS)
#define MQTT_ENABLED    1
#define MQTT_TLS         1    // 1 = TLS (encrypted); 0 = plaintext
#define MQTT_TLS_VERIFY  1    // 1 = also validate the broker cert (needs NTP + MQTT_ROOT_CA); 0 = encrypt only
#define MQTT_HOST      "cd328eb2e9234476a04403e82faba091.s1.eu.hivemq.cloud"
#if MQTT_TLS
  #define MQTT_PORT    8883
#else
  #define MQTT_PORT    1883
#endif
#define MQTT_USER      "Float_User"
#define MQTT_PASS      "Float1234"
#define MQTT_CLIENT_ID "float-observer"
#define MQTT_BASE      "float/aq1"           // <base>/telemetry | state | alert | cmd
#if MQTT_ENABLED
  #include <PubSubClient.h>
  #if MQTT_TLS
    #include <WiFiClientSecure.h>
    WiFiClientSecure mqttNet;
    #if MQTT_TLS_VERIFY
      #include <time.h>
      // Broker root CA (PEM). HiveMQ Cloud uses Let's Encrypt ISRG Root X1.
      static const char* MQTT_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)CERT";
    #endif
  #else
    WiFiClient mqttNet;
  #endif
  PubSubClient mqtt(mqttNet);
  uint32_t     mqtt_last_pub = 0;
  uint32_t     mqtt_last_try = 0;
#endif

// Email alerts via a free Google Apps Script web app (HTTPS GET with ?reason=&severity=&v=)
#define EMAIL_ALERTS_ENABLED 1
#define EMAIL_WEBHOOK_URL "https://script.google.com/macros/s/AKfycbyJJLdBmr7H1WGy-DhsXkxaTLGi_KEOzmZNDy5yu_Lrdq13BcbJ_TE9X9nILWsZYLVF/exec"
#if EMAIL_ALERTS_ENABLED
  #include <WiFiClientSecure.h>
  #include <HTTPClient.h>
#endif

// Default-mode toggle (persisted in NVS). ON = autonomous cycle, OFF = manual bench.
Preferences   obsPrefs;
bool          autoMode            = true;
const char*   pendingTargetCmd    = nullptr;   // ESP-NOW string to forward to the target
int           pendingTargetCount  = 0;
bool          targetPumpOn        = false;
bool          targetHalted        = false;

// Pins
const int I2C_SDA     = 41;
const int I2C_SCL     = 42;
const int I2C2_SDA    = 47;   // INA #2 (Uno 9V)  - bus 1 (Wire1), due pin NUOVI
const int I2C2_SCL    = 48;
const int BUZZER_PIN  = 7;

Adafruit_INA219 ina219;
Adafruit_INA219 ina219_uno;
WebServer       server(80);

// ESP-NOW peer (Target MAC) + AES-CCM keys (must match target.cpp byte-for-byte)
uint8_t targetAddress[] = {0x48, 0x27, 0xE2, 0xE2, 0xE3, 0x0C};
static const uint8_t ESPNOW_PMK[16] = {'F','L','O','A','T','-','p','m','k','-','v','9','-','a','q','1'};
static const uint8_t ESPNOW_LMK[16] = {'F','L','O','A','T','-','l','m','k','-','v','9','-','a','q','1'};

// System state
String  current_mode   = "IDLE";    // IDLE | LEARNING | MONITORING
bool    system_locked  = false;
String  anomaly_reason = "NONE";
String   last_anomaly_pushed = "NONE";
uint32_t last_anomaly_push_ms = 0;

// Learning / calibration
const int  MAX_SAMPLES = 60;
float      samples[MAX_SAMPLES];
float      temp_samples[MAX_SAMPLES];
int        sample_idx      = 0;
int        temp_sample_idx = 0;
int        grace_period    = 0;

float baseline_mean      = 0.0f;
float baseline_std       = 0.0f;
float th_stall           = 0.0f;   // mu + 3*sigma (upper Shewhart limit)
float th_volt_min        = 0.0f;   // physical battery cutoff OR 90% of cal voltage
float th_dry_run         = 0.0f;   // lower control-limit reference (display/onset); detection via lower CUSUM
bool  is_calibrated      = false;
bool  calFromManual      = false;
bool  new_temp_for_learn = false;

// Dynamic temperature thresholds
const float TEMP_K_SIGMA      =  5.0f;
const float TEMP_DELTA_MIN    =  1.5f;
const float TEMP_ABSOLUTE_MAX = 32.0f;
const float TEMP_ABSOLUTE_MIN = 16.0f;
float th_temp_high = TEMP_ABSOLUTE_MAX;
float th_temp_low  = TEMP_ABSOLUTE_MIN;

// EWMA
const float EWMA_ALPHA   = 0.2f;
float       ewma_current = 0.0f;
bool        ewma_init    = false;

// Anomaly confirmation counters
int stall_confirm = 0;
int dry_confirm   = 0;
int volt_confirm  = 0;
int temp_confirm  = 0;
const int CONFIRM_NEEDED = 3;

// Predictive degradation: one-sided CUSUM on the per-cycle healthy current vs baseline
const float DRIFT_K_SIGMA = 1.0f;   // tolerance band (sigma)
const float DRIFT_H_SIGMA = 3.0f;   // fire after this many sigma accumulate
float drift_cusum     = 0.0f;
float drift_cycle_sum = 0.0f;
int   drift_cycle_n   = 0;

// Dry-run: within-cycle LOWER CUSUM on pump current (sustained under-current, NOT an on/off switch).
// K and H are floored to a fraction of baseline because steady-state sigma is tiny while a dry
// pump drops current by a fraction of its LOADED value.
const float DRY_K_SIGMA     = 0.5f;    // CUSUM slack (sigma)
const float DRY_H_SIGMA     = 5.0f;    // CUSUM decision (sigma)
const float DRY_K_FRAC      = 0.10f;   // ... or >= 10% of baseline
const float DRY_H_FRAC      = 0.20f;   // ... or >= 20% of baseline
const float DRY_MIN_CURRENT = 20.0f;   // below this = no supply, NOT a dry-run
float dry_cusum = 0.0f;                // lower-CUSUM accumulator (reset each monitoring cycle)

// Voltage: PHYSICAL battery cutoff, not a statistical control limit (voltage drifts with state of charge).
const float BATT_CUTOFF_V = 3.30f;     // LiPo safe cutoff under load

// Evaluation harness: labelled confusion matrix + detection latency.
// Classes: 0 NORMAL, 1 MOTOR_STALL, 2 DRY_RUN, 3 VOLTAGE_DROP, 4 TEMP_TOO_HIGH, 5 TEMP_TOO_LOW.
const int EVAL_CLASSES = 6;
int      confmat[EVAL_CLASSES][EVAL_CLASSES] = {{0}};
int      eval_truth         = -1;       // -1 = off; else 0..5
int      eval_count         = 0;
float    eval_lat_sum       = 0.0f;
int      eval_lat_n         = 0;
bool     eval_cycle_recorded = false;
uint32_t eval_onset_ms      = 0;

int reasonToClass(const String& r) {
    if (r == "MOTOR_STALL")   return 1;
    if (r == "DRY_RUN")       return 2;
    if (r == "VOLTAGE_DROP")  return 3;
    if (r == "TEMP_TOO_HIGH") return 4;
    if (r == "TEMP_TOO_LOW")  return 5;
    return 0;
}

// Latest sensor readings
// Arduino Uno 9V supply (INA #2, separate I2C bus)
float    last_uno_V = 0.0f;
float    last_uno_I = 0.0f;
bool     ina_uno_ok = false;
uint32_t uno_read_last = 0;
const float UNO_BATT_WARN_V = 7.0f;
bool     uno_batt_warned = false;
float last_current  = 0.0f;
float last_voltage  = 0.0f;
float last_temp_c   = 25.0f;
float last_turb     = 0.0f;

// Event ring buffer for the dashboard (fixed buffer, callback-safe)
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

void push_log_event(const char* txt) {
    const char* type = "log";
    if (strstr(txt, "[WARN]") || strstr(txt, "[HALT]") || strstr(txt, "[CRITICAL]"))
        type = "anomaly";
    push_event(type, txt);
}

// Stats helpers
void computeStats(float* arr, int n, float& mean, float& std_dev) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += arr[i];
    mean = sum / n;
    float sq = 0.0f;
    for (int i = 0; i < n; i++) sq += powf(arr[i] - mean, 2);
    std_dev = sqrtf(sq / n);
}

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

// Hampel filter: robust mean/std (rejects outliers via MAD)
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

#if MQTT_ENABLED
// Forward decls: MQTT commands reuse the same logic as the HTTP handlers (single source of truth)
void applyMode(bool on);
void applyClearHalt();
bool applyManualCmd(const String& a, long sec);

// MQTT publish: telemetry
void mqttPublishTelemetry() {
    if (!mqtt.connected()) return;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"I\":%.2f,\"I_ewma\":%.2f,\"V\":%.2f,\"T\":%.2f,\"turb\":%.1f}",
        last_current, ewma_current, last_voltage, last_temp_c, last_turb);
    mqtt.publish(MQTT_BASE "/telemetry", buf);
}

// MQTT publish: retained reported state (device-shadow snapshot)
void mqttPublishState() {
    if (!mqtt.connected()) return;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"auto\":%s,\"halted\":%s,\"calibrated\":%s,"
        "\"mu\":%.1f,\"th_stall\":%.1f,\"th_dry\":%.1f,\"drift\":%.1f}",
        current_mode.c_str(),
        autoMode      ? "true" : "false",
        system_locked ? "true" : "false",
        is_calibrated ? "true" : "false",
        baseline_mean, th_stall, th_dry_run, drift_cusum);
    mqtt.publish(MQTT_BASE "/state", buf, true);
}

// MQTT publish: anomaly alert
void mqttPublishAlert(const char* reason, const char* severity) {
    if (!mqtt.connected()) return;
    char buf[176];
    snprintf(buf, sizeof(buf),
        "{\"severity\":\"%s\",\"reason\":\"%s\",\"V\":%.2f,\"T\":%.1f,\"ts\":%lu}",
        severity, reason, last_voltage, last_temp_c, (unsigned long)millis());
    mqtt.publish(MQTT_BASE "/alert", buf);
}

// Non-blocking keepalive: reconnect <=5 s, publish telemetry+state every 5 s
void mqttTask() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!mqtt.connected()) {
        uint32_t now = millis();
        if (now - mqtt_last_try < 5000) return;
        mqtt_last_try = now;
#if MQTT_TLS && MQTT_TLS_VERIFY
        if (time(nullptr) < 1700000000UL) {   // wait for NTP before the TLS handshake
            Serial.println("[MQTT] waiting for NTP time (cert validation)...");
            return;
        }
#endif
        bool ok = (strlen(MQTT_USER) > 0)
                ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
                : mqtt.connect(MQTT_CLIENT_ID);
        if (ok) {
            Serial.println("[MQTT] connected");
            mqtt.subscribe(MQTT_BASE "/cmd");
            mqttPublishState();
        } else {
            Serial.printf("[MQTT] connect failed (rc=%d) - will retry\n", mqtt.state());
            return;
        }
    }
    mqtt.loop();
    uint32_t now = millis();
    if (now - mqtt_last_pub >= 5000) {
        mqtt_last_pub = now;
        mqttPublishTelemetry();
        mqttPublishState();
    }
}

// Incoming command on <base>/cmd. Always allowed: mode_on | mode_off | clear_halt.
// OFF-mode only (same gate as the dashboard): pump_on[:sec] | pump_off | feed[:sec] | feed_stop | calibrate.
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    char msg[24];
    unsigned int n = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, payload, n);
    msg[n] = '\0';
    while (n > 0 && (msg[n-1] == '\n' || msg[n-1] == '\r' || msg[n-1] == ' ')) msg[--n] = '\0';
    String c = String(msg); c.toLowerCase();
    Serial.printf("[MQTT] cmd received: '%s'\n", c.c_str());

    if      (c == "mode_on"    || c == "mode:on"  || c == "on")  { applyMode(true);  mqttPublishState(); return; }
    else if (c == "mode_off"   || c == "mode:off" || c == "off") { applyMode(false); mqttPublishState(); return; }
    else if (c == "clear_halt" || c == "clearhalt")              { applyClearHalt(); mqttPublishState(); return; }

    String act = c; long sec = 0;
    int colon = c.indexOf(':');
    if (colon > 0) { act = c.substring(0, colon); sec = c.substring(colon + 1).toInt(); }

    if (autoMode) {
        Serial.println("[MQTT] ignored: auto mode on (manual control only in OFF)");
        return;
    }
    if (applyManualCmd(act, sec)) mqttPublishState();
    else Serial.println("[MQTT] unknown command - ignored");
}
#endif

#if EMAIL_ALERTS_ENABLED
// Email on anomaly via Apps Script (HTTPS GET). MQTT TLS is dropped during the call to free RAM.
void sendEmailAlert(const char* reason, const char* severity) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (strncmp(EMAIL_WEBHOOK_URL, "PASTE", 5) == 0) return;
#if MQTT_ENABLED
    if (mqtt.connected()) mqtt.disconnect();
#endif
    {
        WiFiClientSecure cli;
        cli.setInsecure();
        HTTPClient https;
        char url[300];
        snprintf(url, sizeof(url),
            "%s?reason=%s&severity=%s&v=%.2f",
            EMAIL_WEBHOOK_URL, reason, severity, last_voltage);
        if (https.begin(cli, url)) {
            https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            https.setConnectTimeout(5000);
            https.setTimeout(8000);
            int code = https.GET();
            Serial.printf("[EMAIL] webhook -> %d\n", code);
            https.end();
        } else {
            Serial.println("[EMAIL] request setup failed");
        }
    }
#if MQTT_ENABLED
    mqtt_last_try = 0;
#endif
}
#endif

// ESP-NOW receive callback (messages from the Target)
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    // HALT echo: if locked, re-assert HALT on every message (covers a lost original HALT)
    if (system_locked) {
        esp_now_send(mac, (const uint8_t*)"HALT", 4);
    }

    // Piggy-back a pending default-mode command on any incoming message
    if (pendingTargetCmd != nullptr && pendingTargetCount > 0) {
        esp_now_send(mac, (const uint8_t*)pendingTargetCmd, strlen(pendingTargetCmd));
        pendingTargetCount--;
        if (pendingTargetCount == 0) pendingTargetCmd = nullptr;
    }

    int id_pos = s.indexOf("|ID:");
    if (id_pos != -1) {
        uint32_t msg_id = (uint32_t)s.substring(id_pos + 4).toInt();
        s = s.substring(0, id_pos);
        char ack_buf[32];
        snprintf(ack_buf, sizeof(ack_buf), "ACK:%lu", (unsigned long)msg_id);
        esp_now_send(mac, (const uint8_t*)ack_buf, strlen(ack_buf));
    }

    if (s.startsWith("HB:")) {
        // Idle heartbeat "<pump><halt>": reconcile OFF->ON, light pump control, surface target halt
        targetPumpOn   = (s.length() > 3 && s.charAt(3) == '1');
        targetHalted   = (s.length() > 4 && s.charAt(4) == '1');
        if (autoMode && !targetHalted) {
            const char* cmd = "CMD:AUTO_ON_RESET";
            esp_now_send(mac, (const uint8_t*)cmd, strlen(cmd));
        }
        return;
    }

    if (s.startsWith("LOG:")) {
        String log_text = s.substring(4);
        Serial.println(log_text);
        push_log_event(log_text.c_str());

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
                Serial.printf("[OBS] WARNING: invalid temperature %.1f C - keeping last valid: %.1f C\n",
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
        calFromManual        = !autoMode;
        Serial.println("[OBS] MODE -> LEARNING");
        push_event("phase", "LEARNING - calibrating pump baseline");

    } else if (s == "CMD:START_MONITOR") {
        if (!is_calibrated) {
            // Not calibrated. In auto mode this means a desync (e.g. a remote
            // OFF->ON wiped calibration while the target was asleep, so the
            // target never relearned). Self-heal: learn from THIS pump cycle
            // instead of skipping monitoring forever. In OFF mode keep the
            // skip + warning (the user must press Calibrate first).
            if (autoMode) {
                current_mode       = "LEARNING";
                sample_idx         = 0;
                temp_sample_idx    = 0;
                ewma_init          = false;
                new_temp_for_learn = false;
                memset(temp_samples, 0, sizeof(temp_samples));
                calFromManual      = false;
                Serial.println("[OBS] not calibrated -> learning from this cycle");
                push_event("phase", "LEARNING - calibrating from current cycle");
            } else {
                Serial.println("[OBS] WARNING: not calibrated yet - monitoring skipped");
            }
            return;
        }
        current_mode    = "MONITORING";
        stall_confirm = 0;
        dry_confirm   = 0;
        volt_confirm  = 0;
        temp_confirm  = 0;
        ewma_init       = false;
        grace_period    = 4;
        dry_cusum           = 0.0f;   // reset the lower CUSUM for this monitoring cycle
        eval_cycle_recorded = false;
        eval_onset_ms       = 0;
        Serial.println("[OBS] MODE -> MONITORING");
        push_event("phase", "MONITORING - anomaly detection active");

    } else if (s == "CMD:STOP_MEASURE") {
        if (current_mode == "LEARNING" && sample_idx > 5) {
            float clean_mean, clean_std;
            hampelStats(samples, sample_idx, 3.0f, clean_mean, clean_std);

            baseline_mean = clean_mean;
            baseline_std  = clean_std;
            th_stall      = baseline_mean + (3.0f * baseline_std);
            if (th_stall < baseline_mean + 15.0f) th_stall = baseline_mean + 15.0f;
            // Voltage: physical battery cutoff OR 10% below the calibration voltage (whichever is higher)
            th_volt_min   = max(BATT_CUTOFF_V, last_voltage * 0.90f);
            // Dry-run reference (lower control limit): mu - max(3 sigma, 15% of mu). Detection is the lower CUSUM.
            th_dry_run    = baseline_mean - max(3.0f * baseline_std, 0.15f * baseline_mean);

            if (temp_sample_idx >= 3) {
                float temp_mean, temp_std;
                hampelStats(temp_samples, temp_sample_idx, 3.0f, temp_mean, temp_std);
                float delta     = max(TEMP_K_SIGMA * temp_std, TEMP_DELTA_MIN);
                float cand_high = temp_mean + delta;
                float cand_low  = temp_mean - delta;
                th_temp_high = min(cand_high, TEMP_ABSOLUTE_MAX);
                th_temp_low  = max(cand_low,  TEMP_ABSOLUTE_MIN);

                Serial.println("\n[OBS] -- Temperature baseline --");
                Serial.printf("   Temp samples : %d\n",         temp_sample_idx);
                Serial.printf("   Temp mean    : %.2f C\n",     temp_mean);
                Serial.printf("   Temp std     : %.2f C\n",     temp_std);
                Serial.printf("   Alarm HIGH   : %.2f C\n",     th_temp_high);
                Serial.printf("   Alarm LOW    : %.2f C\n",     th_temp_low);
            } else {
                th_temp_high = TEMP_ABSOLUTE_MAX;
                th_temp_low  = TEMP_ABSOLUTE_MIN;
                Serial.printf("[OBS] WARNING: only %d temp samples - using backstop limits\n", temp_sample_idx);
            }

            is_calibrated = true;
            drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;

            Serial.println("\n[OBS] == Calibration complete (SPC: Shewhart + CUSUM) ==");
            Serial.printf("   mean (mu)  : %.2f mA\n",          baseline_mean);
            Serial.printf("   std (sigma): %.2f mA\n",          baseline_std);
            Serial.printf("   Stall thr  : %.2f mA  (mu+3sigma)\n", th_stall);
            Serial.printf("   Dry-run ref: %.2f mA  (lower ctrl limit; CUSUM detect)\n", th_dry_run);
            Serial.printf("   Volt min   : %.2f V   (battery cutoff)\n", th_volt_min);

            char buf[200];
            snprintf(buf, sizeof(buf), "CAL:%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                     baseline_mean, baseline_std, th_stall, th_dry_run,
                     th_temp_high, th_temp_low);
            espNowSend(buf);

            char notif[120];
            snprintf(notif, sizeof(notif),
                     "Calibration done - mu=%.1f mA  stall=%.1f  T[%.1f-%.1f]C",
                     baseline_mean, th_stall, th_temp_low, th_temp_high);
            push_event("calibration", notif);
        }

        // End of a MONITORING cycle: fold healthy mean into the degradation CUSUM
        if (current_mode == "MONITORING" && is_calibrated &&
            !system_locked && drift_cycle_n >= 4 && baseline_mean > 1.0f) {
            float cycle_mean = drift_cycle_sum / drift_cycle_n;
            float k = DRIFT_K_SIGMA * baseline_std;
            float dev = (cycle_mean - baseline_mean) - k;
            drift_cusum += dev; if (drift_cusum < 0.0f) drift_cusum = 0.0f;
            float limit = DRIFT_H_SIGMA * baseline_std;
            Serial.printf("[DRIFT] cycle mu=%.1f mA (baseline %.1f, +%.1f)  CUSUM=%.1f / %.1f\n",
                          cycle_mean, baseline_mean, cycle_mean - baseline_mean,
                          drift_cusum, limit);
            if (drift_cusum > limit) {
                Serial.println("[WARN] DEGRADATION: pump current creeping up - service recommended.");
                push_event("warn", "DEGRADATION");
                buzzerAlert(2);
#if MQTT_ENABLED
                mqttPublishAlert("DEGRADATION", "warn");
#endif
#if EMAIL_ALERTS_ENABLED
                sendEmailAlert("DEGRADATION", "warn");
#endif
                drift_cusum = 0.0f;
            }
        }
        drift_cycle_sum = 0.0f; drift_cycle_n = 0;

        // Evaluation: a clean monitored cycle counts as "detected NORMAL"
        if (current_mode == "MONITORING" && eval_truth >= 0 && !eval_cycle_recorded) {
            confmat[eval_truth][0]++;
            eval_count++;
            eval_cycle_recorded = true;
            Serial.printf("[EVAL] truth=%d detected=0 (no anomaly)  (n=%d)\n",
                          eval_truth, eval_count);
        }

        current_mode = "IDLE";
        push_event("phase", "IDLE - waiting for next target cycle");

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

// HTTP: serve the dashboard (streamed in 1 KB chunks)
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
    // Single consumption graph = INSTANTANEOUS SUM of both INA219 (Target + Uno).
    // Detection stays internal on the Target current only; here we just shift the
    // displayed values by the Uno draw so the one chart is consistent.
    float i_total  = last_current + last_uno_I;                                   // Raw line
    float ie_total = ewma_current + last_uno_I;                                   // EWMA line
    float ths_disp = (th_stall   > 0.1f) ? (th_stall   + last_uno_I) : 0.0f;      // stall line
    float thd_disp = (th_dry_run > 0.1f) ? (th_dry_run + last_uno_I) : 0.0f;      // dry line
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"I\":%.2f,\"I_ewma\":%.2f,\"V\":%.2f,\"T\":%.2f,\"turb\":%.1f"
        ",\"mode\":\"%s\",\"locked\":%s,\"auto\":%s,\"pump\":%s,\"thalt\":%s"
        ",\"th_stall\":%.2f,\"th_dry\":%.2f,\"th_thi\":%.2f,\"th_tlo\":%.2f"
        ",\"uno_v\":%.2f,\"uno_i\":%.1f,\"uno_ok\":%s"
        ",\"evt_seq\":%lu}",
        i_total, ie_total, last_voltage, last_temp_c, last_turb,
        current_mode.c_str(),
        system_locked ? "true" : "false",
        autoMode      ? "true" : "false",
        targetPumpOn  ? "true" : "false",
        targetHalted  ? "true" : "false",
        ths_disp, thd_disp, th_temp_high, th_temp_low,
        last_uno_V, last_uno_I, ina_uno_ok ? "true" : "false",
        (unsigned long)evt_seq);
    server.send(200, "application/json", buf);
}

// HTTP: evaluation harness. /eval?truth=K | /eval?off=1 | /eval?reset=1 | /eval (JSON: truth,count,lat,6x6 matrix)
void handleEval() {
    if (server.hasArg("reset")) {
        memset(confmat, 0, sizeof(confmat));
        eval_count = 0; eval_lat_sum = 0.0f; eval_lat_n = 0;
        Serial.println("[EVAL] matrix reset");
    }
    if (server.hasArg("off")) {
        eval_truth = -1;
        Serial.println("[EVAL] labelling OFF");
    } else if (server.hasArg("truth")) {
        int t = server.arg("truth").toInt();
        if (t >= 0 && t < EVAL_CLASSES) {
            eval_truth = t;
            Serial.printf("[EVAL] ground truth set to class %d\n", t);
        }
    }

    char buf[360];
    int n = snprintf(buf, sizeof(buf),
        "{\"truth\":%d,\"count\":%d,\"lat\":%.0f,\"m\":[",
        eval_truth, eval_count,
        eval_lat_n > 0 ? (eval_lat_sum / eval_lat_n) : 0.0f);
    for (int i = 0; i < EVAL_CLASSES; i++)
        for (int j = 0; j < EVAL_CLASSES; j++)
            n += snprintf(buf + n, sizeof(buf) - n, "%s%d",
                          (i == 0 && j == 0) ? "" : ",", confmat[i][j]);
    snprintf(buf + n, sizeof(buf) - n, "]}");
    server.send(200, "application/json", buf);
}

// Clear a halt latch (shared by HTTP and MQTT). Valid in both modes.
void applyClearHalt() {
    system_locked       = false;
    anomaly_reason      = "NONE";
    last_anomaly_pushed = "NONE";
    current_mode        = "IDLE";
    stall_confirm = volt_confirm = dry_confirm = temp_confirm = 0;
    targetPumpOn        = false;
    targetHalted        = false;
    push_event("phase", "HALT cleared");
    for (int i = 0; i < 5; i++) { espNowSend("CMD:CLEAR_HALT"); delay(8); }
}

// Manual actuator commands shared by HTTP /cmd and MQTT (OFF mode only).
// pump_on[:sec] | pump_off | feed[:sec] | feed_stop | calibrate
bool applyManualCmd(const String& a, long sec) {
    if (sec < 0) sec = 0;
    char cmd[40];
    if (a == "pump_on") {
        snprintf(cmd, sizeof(cmd), "CMD:PUMP_ON:%ld", sec);
        targetPumpOn = true;
        push_event("phase", sec > 0 ? "Manual pump ON (timed)" : "Manual pump ON");
    } else if (a == "pump_off") {
        snprintf(cmd, sizeof(cmd), "CMD:PUMP_OFF");
        targetPumpOn = false;
        push_event("phase", "Manual pump OFF");
    } else if (a == "feed") {
        snprintf(cmd, sizeof(cmd), "CMD:FEED:%ld", sec);
        push_event("phase", sec > 0 ? "Manual feed scheduled" : "Manual feed (once)");
    } else if (a == "feed_stop") {
        snprintf(cmd, sizeof(cmd), "CMD:FEED_STOP");
        push_event("phase", "Manual feeding stopped");
    } else if (a == "calibrate") {
        snprintf(cmd, sizeof(cmd), "CMD:CALIBRATE");
        push_event("phase", "Manual calibration requested");
    } else {
        return false;
    }
    for (int i = 0; i < 5; i++) { espNowSend(cmd); delay(8); }
    return true;
}

// HTTP: manual controls. Clear HALT works in both modes; everything else only in OFF.
void handleCmd() {
    String a = server.hasArg("a") ? server.arg("a") : "";
    if (autoMode && a != "clear_halt") {
        server.send(409, "application/json", "{\"err\":\"auto mode on\"}");
        return;
    }
    if (a == "clear_halt") {
        applyClearHalt();
        server.send(200, "application/json", "{\"ok\":true,\"pump\":false}");
        return;
    }
    long sec = server.hasArg("sec") ? server.arg("sec").toInt() : 0;
    if (!applyManualCmd(a, sec)) {
        server.send(400, "application/json", "{\"err\":\"bad action\"}");
        return;
    }
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"pump\":%s}", targetPumpOn ? "true" : "false");
    server.send(200, "application/json", resp);
}

// Apply a default-mode change (shared by HTTP /mode and MQTT). On a real transition,
// OFF->ON forces a fresh relearn, ON->OFF pauses and discards calibration.
void applyMode(bool on) {
    bool was = autoMode;
    autoMode = on;
    obsPrefs.putBool("auto", autoMode);

    if (autoMode && !was) {
        // OFF -> ON: always relearn (discard any calibration)
        system_locked     = false;
        anomaly_reason    = "NONE";
        last_anomaly_pushed = "NONE";
        current_mode      = "IDLE";
        stall_confirm = volt_confirm = dry_confirm = temp_confirm = 0;
        ewma_init         = false;
        grace_period      = 0;
        targetPumpOn      = false;
        is_calibrated     = false;
        calFromManual     = false;
        th_stall = th_dry_run = th_temp_high = th_temp_low = 0.0f;
        drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;
        dry_cusum = 0.0f;
        pendingTargetCmd   = "CMD:AUTO_ON_RESET";
        pendingTargetCount = 5;
        push_event("phase", "Default mode ON - fresh learning starting");
        Serial.println("[OBS] Default mode -> ON (fresh learning)");
    } else if (!autoMode && was) {
        // ON -> OFF: pause and discard calibration
        current_mode  = "IDLE";
        stall_confirm = volt_confirm = dry_confirm = temp_confirm = 0;
        ewma_init     = false;
        targetPumpOn  = false;
        is_calibrated = false;
        calFromManual = false;
        anomaly_reason = "NONE";
        last_anomaly_pushed = "NONE";
        th_stall = th_dry_run = th_temp_high = th_temp_low = 0.0f;
        drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;
        dry_cusum = 0.0f;
        pendingTargetCmd   = "CMD:AUTO_OFF";
        pendingTargetCount = 5;
        push_event("phase", "Default mode OFF - system paused, calibration cleared");
        Serial.println("[OBS] Default mode -> OFF (calibration cleared)");
    }
}

// HTTP: /mode?set=on|off
void handleMode() {
    if (server.hasArg("set")) {
        String v = server.arg("set"); v.toLowerCase();
        applyMode(v == "on" || v == "1" || v == "true");
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"auto\":%s}", autoMode ? "true" : "false");
    server.send(200, "application/json", buf);
}

// HTTP: event feed as JSON (incremental via ?last=ID)
void handleEvents() {
    uint32_t last_id = server.hasArg("last")
                       ? (uint32_t)server.arg("last").toInt() : 0;
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

// Like delay() but keeps the WebServer responsive
void smartDelay(unsigned long ms) {
    unsigned long t0 = millis();
    do {
        server.handleClient();
        delay(2);
    } while (millis() - t0 < ms);
}

void setup() {
    delay(3000);
    Serial.begin(115200);
    Serial.println("\n\n[SYS] Serial connected. Booting...");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

// I2C bus 0 - INA #1 (Target)
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();
    Wire.setClock(100000);
    delay(100);
    if (!ina219.begin()) {
        Serial.println("[OBS] CRITICAL: INA219 (Target) not found!");
        while (1) delay(100);
    }
    // I2C bus 1 - INA #2 (Arduino Uno 9V): bus separato, stesso 0x40
    Wire1.begin(I2C2_SDA, I2C2_SCL, 100000);
    if (ina219_uno.begin(&Wire1)) {
        ina_uno_ok = true;
        Serial.println("[OBS] INA219 (Uno 9V) detected on I2C bus 1");
    } else {
        Serial.println("[OBS] WARNING: INA219 (Uno 9V) not found on bus 1 - battery monitor off");
    }

    // WiFi (STA only) + read back channel for ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[OBS] Connecting to '%s'", WIFI_SSID);
    unsigned long t0_wifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0_wifi < 15000) {
        delay(400); Serial.print(".");
    }

    uint8_t esp_now_channel = 13;
    if (WiFi.status() == WL_CONNECTED) {
        uint8_t pri; wifi_second_chan_t sec;
        esp_wifi_get_channel(&pri, &sec);
        esp_now_channel = pri;
        Serial.printf("\n[OBS] WiFi OK - channel %d, dashboard at http://%s\n",
                      esp_now_channel, WiFi.localIP().toString().c_str());
#if MQTT_ENABLED
        mqtt.setServer(MQTT_HOST, MQTT_PORT);
        mqtt.setBufferSize(512);
        mqtt.setCallback(mqttCallback);
  #if MQTT_TLS
    #if MQTT_TLS_VERIFY
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        mqttNet.setCACert(MQTT_ROOT_CA);
    #else
        mqttNet.setInsecure();   // TLS without server-cert validation
    #endif
  #endif
#endif
    } else {
        Serial.println("\n[OBS] WiFi not found - fallback channel 13 (no dashboard)");
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);
    }

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[OBS] ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_set_pmk(ESPNOW_PMK);

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    peer.channel = 0;                 // use current radio channel
    memcpy(peer.lmk, ESPNOW_LMK, 16);
    peer.encrypt = true;
    esp_now_add_peer(&peer);

    // HTTP routes
    server.on("/",            HTTP_GET, handleRoot);
    server.on("/data",        HTTP_GET, handleData);
    server.on("/events",      HTTP_GET, handleEvents);
    server.on("/mode",        HTTP_GET, handleMode);
    server.on("/cmd",         HTTP_GET, handleCmd);
    server.on("/eval",        HTTP_GET, handleEval);
    server.on("/favicon.ico", HTTP_GET, [](){ server.send(204, "text/plain", ""); });
    server.onNotFound([](){ server.send(204, "text/plain", ""); });
    server.begin();

    // Default-mode toggle from NVS
    obsPrefs.begin("float-obs", false);
    autoMode = obsPrefs.getBool("auto", true);
    Serial.printf("[OBS] Default mode at boot: %s\n", autoMode ? "ON" : "OFF");

    push_event("phase", "IDLE - system online");

    Serial.println("\n== FLOAT Observer Node ==");
    Serial.println("SPC: Shewhart(stall) + lower CUSUM(dry) + CUSUM(degrade) | adaptive TEMP | battery cutoff(volt) | VOLT/TEMP = warning");
}

void loop() {
    server.handleClient();
#if MQTT_ENABLED
    mqttTask();
#endif

    // INA219 read (recover the bus on a bad reading)
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

// --- Arduino Uno 9V monitor (INA #2, bus 1; additivo, NON tocca la v9) ---
    if (ina_uno_ok && (millis() - uno_read_last >= 2000)) {
        uno_read_last = millis();
        float uv = ina219_uno.getBusVoltage_V();
        float ui = ina219_uno.getCurrent_mA();
        if (!isnan(uv)) last_uno_V = max(0.0f, uv);
        if (!isnan(ui)) last_uno_I = ui;
        Serial.printf("[UNO] battery %.2f V  draw %.1f mA\n", last_uno_V, last_uno_I);
        if (last_uno_V > 1.0f && last_uno_V < UNO_BATT_WARN_V && !uno_batt_warned) {
            uno_batt_warned = true;
            Serial.printf("[WARN] Arduino 9V battery low (%.2f V) - replace soon\n", last_uno_V);
            push_event("warn", "Arduino 9V battery low - replace soon");
            buzzerAlert(2);
#if MQTT_ENABLED
            mqttPublishAlert("UNO_BATTERY_LOW", "warn");
#endif
        } else if (last_uno_V > UNO_BATT_WARN_V + 0.5f) {
            uno_batt_warned = false;
        }
    }

    // Halted: keep buzzing + telemetry until Clear HALT
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

    // LEARNING: collect current samples (pump-load only)
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

    // MONITORING: detect anomalies (SPC) + confirmation counters
    if (current_mode == "MONITORING" && is_calibrated) {
        if (grace_period > 0) {
            grace_period--;
            Serial.printf("   [MON] Motor inrush - alarms suppressed (%d left)  I_raw=%.1f mA\n",
                          grace_period, last_current);
            if (grace_period == 0) {
                ewma_current = last_current;
                dry_cusum    = 0.0f;   // start the lower CUSUM clean once the motor has settled
                Serial.printf("   [MON] Grace period ended - EWMA seeded at %.1f mA\n", ewma_current);
            }
            smartDelay(400);
            return;
        }

        // MOTOR_STALL: upper Shewhart control limit (large abrupt jump).
        bool stall_flag = (ewma_current > th_stall);

        // VOLTAGE_DROP: physical battery cutoff (voltage is not a stationary process).
        bool volt_flag  = (th_volt_min > 0.1f && last_voltage < th_volt_min);

        // DRY_RUN: lower one-sided CUSUM on the raw pump current (sustained under-current).
        // K/H are floored to a fraction of the baseline (steady-state sigma is tiny).
        // A supply collapse (current ~0) or an active voltage fault is NOT a dry-run:
        // it is excluded here and handled by the voltage branch.
        float dryK = max(DRY_K_SIGMA * baseline_std, DRY_K_FRAC * baseline_mean);
        float dryH = max(DRY_H_SIGMA * baseline_std, DRY_H_FRAC * baseline_mean);
        if (last_current > DRY_MIN_CURRENT) {
            dry_cusum += (baseline_mean - dryK) - last_current;   // grows as current sags below (mu - K)
            if (dry_cusum < 0.0f) dry_cusum = 0.0f;
        } else {
            dry_cusum = 0.0f;                                     // no supply -> not a dry-run
        }
        bool dry_flag = (dry_cusum > dryH) && !volt_flag;

        // TEMP: adaptive control band.
        bool temp_high_flag = false;
        bool temp_low_flag  = false;
        if (last_temp_c != -127.0f && last_temp_c > -10.0f) {
            temp_high_flag = (last_temp_c > th_temp_high);
            temp_low_flag  = (last_temp_c < th_temp_low);
        }
        bool temp_flag = temp_high_flag || temp_low_flag;

        // Evaluation: timestamp first raw out-of-range sample (for latency)
        if (eval_truth >= 0 && eval_onset_ms == 0) {
            bool raw_bad = (last_current > th_stall) ||
                           (last_current > DRY_MIN_CURRENT && last_current < th_dry_run) ||
                           (th_volt_min > 0.1f && last_voltage < th_volt_min) ||
                           temp_flag;
            if (raw_bad) eval_onset_ms = (uint32_t)millis();
        }

        if (stall_flag) stall_confirm++; else stall_confirm = 0;
        if (volt_flag)  volt_confirm++;  else volt_confirm = 0;
        dry_confirm = dry_flag ? CONFIRM_NEEDED : 0;   // CUSUM crossing IS the confirmation
        if (temp_flag)  temp_confirm++;  else temp_confirm = 0;

        // Degradation: accumulate only healthy samples
        if (!stall_flag && !dry_flag) {
            drift_cycle_sum += ewma_current;
            drift_cycle_n++;
        }

        bool anomaly = (stall_confirm >= CONFIRM_NEEDED) ||
                       (volt_confirm >= CONFIRM_NEEDED) ||
                       (dry_confirm >= CONFIRM_NEEDED) ||
                       (temp_confirm >= CONFIRM_NEEDED);

        Serial.printf("   [MON] I_raw=%.1f  I_ewma=%.1f  V=%.2f  T=%.1f"
                      "  Tthr[H%.1f L%.1f]  dryCUSUM=%.0f/%.0f  [S:%d V:%d D:%d T:%d]%s%s%s%s%s\n",
                      last_current, ewma_current, last_voltage, last_temp_c,
                      th_temp_high, th_temp_low, dry_cusum, dryH,
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

            // Evaluation: record outcome (detected = reason) + latency
            if (eval_truth >= 0 && !eval_cycle_recorded) {
                int det = reasonToClass(anomaly_reason);
                confmat[eval_truth][det]++;
                eval_count++;
                if (eval_onset_ms != 0) {
                    eval_lat_sum += (float)((uint32_t)millis() - eval_onset_ms);
                    eval_lat_n++;
                }
                eval_cycle_recorded = true;
                Serial.printf("[EVAL] truth=%d detected=%d  (n=%d)\n",
                              eval_truth, det, eval_count);
            }

            // Dashboard toast + cloud alert (throttled: re-push on reason change or after 15 s)
            bool isHalt = (anomaly_reason == "MOTOR_STALL" || anomaly_reason == "DRY_RUN");
            uint32_t nowm = (uint32_t)millis();
            bool notify_now = false;
            if (anomaly_reason != last_anomaly_pushed ||
                (nowm - last_anomaly_push_ms) > 15000) {
                push_event(isHalt ? "halt" : "warn", anomaly_reason.c_str());
                last_anomaly_pushed  = anomaly_reason;
                last_anomaly_push_ms = nowm;
#if MQTT_ENABLED
                mqttPublishAlert(anomaly_reason.c_str(), isHalt ? "halt" : "warn");
#endif
                notify_now = true;
            }

            // MOTOR_STALL / DRY_RUN -> halt the pump; VOLTAGE_DROP / TEMP -> warning only
            if (anomaly_reason == "MOTOR_STALL" || anomaly_reason == "DRY_RUN") {
                for (int i = 0; i < 10; i++) { espNowSend("HALT"); delay(5); }
                system_locked = true;
                targetHalted  = true;
            }

            char alert[64];
            snprintf(alert, sizeof(alert), "ALERT:%s", anomaly_reason.c_str());
            espNowSend(alert);

            buzzerAlert(3);

            // Email last (blocking HTTPS), only after the pump has been told to stop
#if EMAIL_ALERTS_ENABLED
            if (notify_now) sendEmailAlert(anomaly_reason.c_str(), isHalt ? "halt" : "warn");
#endif

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

    // IDLE
    Serial.printf("[IDLE] I=%.2f mA  V=%.2f V  T=%.1f C\n",
                  last_current, last_voltage, last_temp_c);
    char buf[128];
    snprintf(buf, sizeof(buf), "TELEM:%.2f,%.2f,%.2f,IDLE,OK",
             last_current, last_voltage, last_temp_c);
    espNowSend(buf);

    smartDelay(2000);
}
