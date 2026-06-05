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
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>  // Required for advanced radio control
#include <math.h>
#include "dashboard.h"

// ── WiFi credentials ───────────────────────────────────────────────────────
// Observer joins this network as STA so the dashboard is reachable at the IP
// printed on serial at boot. The target also references this same SSID to
// discover the channel — no dedicated AP needed. To switch networks (e.g.
// home WiFi instead of hotspot) just edit these two constants in BOTH files
// in sync. If the network is not found at boot the system falls back to the
// hardcoded channel 13 used by the original v9 — ESP-NOW keeps working,
// just without dashboard.
const char* WIFI_SSID = "iPhone di Michele";
const char* WIFI_PASS = "Michele4!";

// ── MQTT / cloud bridge (Device-Shadow "reported" + Rules-Engine alerts) ───
// The observer is the always-on WiFi STA, so it doubles as the MQTT client that
// bridges FLOAT to a broker. It publishes telemetry, a retained "reported" state
// (the device-shadow snapshot) and anomaly alerts (which a broker Rules Engine
// can turn into Telegram/email). Remote commands ("desired") come in step 1b.
// Broker-agnostic; the topic layout maps cleanly onto ThingsBoard / AWS IoT.
// Set MQTT_ENABLED to 0 to compile the firmware without the cloud bridge (and
// without needing the PubSubClient library).
#define MQTT_ENABLED    1
#define MQTT_TLS         1    // 1 = MQTT over TLS (encrypted link); 0 = plaintext
#define MQTT_TLS_VERIFY  0    // 1 = also validate the broker certificate (needs NTP time +
                             //     the broker root CA in MQTT_ROOT_CA below)
                             // 0 = encrypt only, skip the server-cert check (quick test, any broker)
// ── Cloud broker: HiveMQ Cloud Serverless (free, private, authenticated) ────
// Create a free cluster at https://console.hivemq.cloud (no credit card), then
// under "Access Management" add a username/password credential. Paste the
// cluster URL and that credential below. Authentication closes the open-broker
// hole: only clients with these credentials can publish/subscribe.
#define MQTT_HOST      "cd328eb2e9234476a04403e82faba091.s1.eu.hivemq.cloud"  // your HiveMQ Cloud cluster URL
#if MQTT_TLS
  #define MQTT_PORT    8883                  // TLS port (HiveMQ Cloud requires TLS)
#else
  #define MQTT_PORT    1883                  // plaintext port (only for a local broker)
#endif
#define MQTT_USER      "Float_User" // credential created in HiveMQ Cloud
#define MQTT_PASS      "Float1234"
#define MQTT_CLIENT_ID "float-observer"
#define MQTT_BASE      "float/aq1"           // topics: <base>/telemetry|state|alert|cmd
#if MQTT_ENABLED
  #include <PubSubClient.h>
  #if MQTT_TLS
    #include <WiFiClientSecure.h>
    WiFiClientSecure mqttNet;
    #if MQTT_TLS_VERIFY
      #include <time.h>
      // Broker root CA (PEM). Default target is Let's Encrypt ISRG Root X1, which
      // backs broker.hivemq.com and many public brokers. If your broker uses a
      // different CA, paste its root here (get it with:
      //   openssl s_client -connect <host>:8883 -showcerts ).
      static const char* MQTT_ROOT_CA = R"CERT(
-----BEGIN CERTIFICATE-----
PASTE_BROKER_ROOT_CA_HERE
-----END CERTIFICATE-----
)CERT";
    #endif
  #else
    WiFiClient mqttNet;
  #endif
  PubSubClient mqtt(mqttNet);
  uint32_t     mqtt_last_pub = 0;            // last periodic publish
  uint32_t     mqtt_last_try = 0;            // last (re)connect attempt
#endif

// ── Email alerts on anomalies (free, via a Google Apps Script web app) ──────
// IFTTT Webhooks is no longer free, so we use Google Apps Script (free with any
// Google account): a tiny script with MailApp.sendEmail, deployed as a Web App
// ("Execute as: Me", "Who has access: Anyone"). Paste its /exec URL below; the
// device GETs it with ?reason=&severity=&v= and the script emails you.
#define EMAIL_ALERTS_ENABLED 1
#define EMAIL_WEBHOOK_URL "https://script.google.com/macros/s/AKfycbwqgXFfeVU0x2Wg0LbV6Y3OX_cs2e2xW5ThtuZp2KUCADMGFY1AHphpS_XoToIrBsl9/exec"
#if EMAIL_ALERTS_ENABLED
  #include <WiFiClientSecure.h>
  #include <HTTPClient.h>
#endif

// ── Default-mode (dashboard toggle) ────────────────────────────────────────
// autoMode = true  → the predefined cycle (learning → sleep → pump/feeding →
//                    sleep → …) runs autonomously, exactly as in v9.
// autoMode = false → the cycle is suspended; system sits in IDLE awaiting
//                    explicit commands from the dashboard (buttons TBD).
// The state is persisted in NVS (survives any reset). When the user toggles
// OFF→ON the Observer resets its own state AND tells the target to wipe its
// own counters so the next cycle starts fresh from learning.
Preferences   obsPrefs;
bool          autoMode            = true;
const char*   pendingTargetCmd    = nullptr;   // ESP-NOW string to forward
int           pendingTargetCount  = 0;          // send N times for reliability
bool          targetPumpOn        = false;     // last pump state from idle heartbeat
bool          targetHalted        = false;     // last halt state from idle heartbeat

// ── Pin map ────────────────────────────────────────────────────────────────
const int I2C_SDA     = 41;
const int I2C_SCL     = 42;
const int BUZZER_PIN  = 7;

// ── Hardware ───────────────────────────────────────────────────────────────
Adafruit_INA219 ina219;
WebServer       server(80);   // HTTP server for the dashboard

// ── ESP-NOW peer (Target node MAC address) ────────────────────────────────
uint8_t targetAddress[] = {0x48, 0x27, 0xE2, 0xE2, 0xE3, 0x0C};

// ESP-NOW link encryption (AES-CCM). MUST match target.cpp byte-for-byte.
static const uint8_t ESPNOW_PMK[16] = {'F','L','O','A','T','-','p','m','k','-','v','9','-','a','q','1'};
static const uint8_t ESPNOW_LMK[16] = {'F','L','O','A','T','-','l','m','k','-','v','9','-','a','q','1'};

// ── System state ───────────────────────────────────────────────────────────
String  current_mode   = "IDLE";    // IDLE | LEARNING | MONITORING
bool    system_locked  = false;
String  anomaly_reason = "NONE";
String   last_anomaly_pushed = "NONE";  // de-duplicate dashboard toast events
uint32_t last_anomaly_push_ms = 0;

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
bool  calFromManual      = false;   // true if current calibration came from a
                                    // manual Calibrate (OFF mode). Decides whether
                                    // OFF→ON keeps the baseline (boot 1) or relearns
                                    // (boot 0), and survives only until ON→OFF.
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

// ── Predictive degradation (per-cycle CUSUM on healthy operating current) ──
// Each monitored cycle yields a mean "healthy" current (stall/dry samples
// excluded). We accumulate, via a one-sided CUSUM, how much that per-cycle
// mean sits ABOVE the learned baseline μ beyond a tolerance band. A sustained
// upward creep (impeller fouling, bearing friction) pushes the CUSUM past a
// limit and raises a DEGRADATION warning — a heads-up to service the pump
// BEFORE a hard stall, which a single-sample μ+3σ test cannot anticipate.
// Tolerance and limit are expressed in σ so they scale with baseline noise.
const float DRIFT_K_SIGMA = 1.0f;   // allowance: ignore drift under μ + 1σ
const float DRIFT_H_SIGMA = 3.0f;   // fire after this many σ accumulate
float drift_cusum     = 0.0f;       // one-sided cumulative sum (mA·cycles)
float drift_cycle_sum = 0.0f;       // Σ healthy ewma_current in current cycle
int   drift_cycle_n   = 0;          // count of healthy samples in current cycle

// ── Evaluation harness (labelled confusion matrix + detection latency) ─────
// Lets us quantify detector quality with ground-truth labels set from the
// dashboard. Classes: 0 NORMAL, 1 MOTOR_STALL, 2 DRY_RUN, 3 VOLTAGE_DROP,
// 4 TEMP_TOO_HIGH, 5 TEMP_TOO_LOW. For each monitored cycle we record one
// outcome: confmat[truth][detected]++. From the matrix the dashboard derives
// accuracy, false-positive rate, false-negative rate and per-class recall.
// Latency = time from the first raw out-of-range sample to ANOMALY CONFIRMED.
const int EVAL_CLASSES = 6;
int      confmat[EVAL_CLASSES][EVAL_CLASSES] = {{0}};
int      eval_truth         = -1;       // -1 = evaluation off; else 0..5
int      eval_count         = 0;        // total recorded cycles
float    eval_lat_sum       = 0.0f;     // Σ detection latency (ms)
int      eval_lat_n         = 0;        // number of latency samples
bool     eval_cycle_recorded = false;   // one record per cycle
uint32_t eval_onset_ms      = 0;        // first raw-abnormal sample this cycle

int reasonToClass(const String& r) {
    if (r == "MOTOR_STALL")   return 1;
    if (r == "DRY_RUN")       return 2;
    if (r == "VOLTAGE_DROP")  return 3;
    if (r == "TEMP_TOO_HIGH") return 4;
    if (r == "TEMP_TOO_LOW")  return 5;
    return 0;
}

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

#if MQTT_ENABLED
// Forward declarations: MQTT "desired" commands reuse the exact same logic as
// the HTTP handlers (single source of truth for mode transition / halt clear).
void applyMode(bool on);
void applyClearHalt();
bool applyManualCmd(const String& a, long sec);   // pump/feed/calibrate, shared HTTP+MQTT
// ── MQTT cloud bridge ──────────────────────────────────────────────────────
// Time-series telemetry → <base>/telemetry
void mqttPublishTelemetry() {
    if (!mqtt.connected()) return;
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"I\":%.2f,\"I_ewma\":%.2f,\"V\":%.2f,\"T\":%.2f,\"turb\":%.1f}",
        last_current, ewma_current, last_voltage, last_temp_c, last_turb);
    mqtt.publish(MQTT_BASE "/telemetry", buf);
}

// Retained "reported" device state → <base>/state. Retain means a freshly
// connected subscriber (or the cloud after a restart) immediately gets the last
// known mode/halt/calibration/thresholds — the device-shadow snapshot.
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
    mqtt.publish(MQTT_BASE "/state", buf, true);   // retained
}

// Anomaly alert → <base>/alert (a broker Rules Engine routes this to a
// notification, e.g. Telegram/email). Severity mirrors the dashboard toast.
void mqttPublishAlert(const char* reason, const char* severity) {
    if (!mqtt.connected()) return;
    char buf[176];
    snprintf(buf, sizeof(buf),
        "{\"severity\":\"%s\",\"reason\":\"%s\",\"V\":%.2f,\"T\":%.1f,\"ts\":%lu}",
        severity, reason, last_voltage, last_temp_c, (unsigned long)millis());
    mqtt.publish(MQTT_BASE "/alert", buf);
}

// Non-blocking keepalive. Reconnects at most every 5 s and never stalls the
// dashboard or ESP-NOW if the broker is unreachable. Publishes telemetry+state
// on a 5 s cadence once connected.
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
            mqtt.subscribe(MQTT_BASE "/cmd");   // listen for desired-state commands
            mqttPublishState();           // push reported state on (re)connect
        } else {
            Serial.printf("[MQTT] connect failed (rc=%d) — will retry\n", mqtt.state());
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

// Incoming "desired" command on <base>/cmd. Full remote control, mirroring the
// dashboard. Always allowed: mode_on | mode_off | clear_halt. Allowed only when
// the automatic cycle is OFF (same gate as the dashboard): pump_on[:sec] |
// pump_off | feed[:sec] | calibrate. After applying, state is republished so
// the shadow's reported side converges with the desired command.
void mqttCallback(char* topic, byte* payload, unsigned int len) {
    char msg[24];
    unsigned int n = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, payload, n);
    msg[n] = '\0';
    while (n > 0 && (msg[n-1] == '\n' || msg[n-1] == '\r' || msg[n-1] == ' ')) msg[--n] = '\0';
    String c = String(msg); c.toLowerCase();
    Serial.printf("[MQTT] cmd received: '%s'\n", c.c_str());

    // Mode + halt clear: valid in any mode.
    if      (c == "mode_on"    || c == "mode:on"  || c == "on")  { applyMode(true);  mqttPublishState(); return; }
    else if (c == "mode_off"   || c == "mode:off" || c == "off") { applyMode(false); mqttPublishState(); return; }
    else if (c == "clear_halt" || c == "clearhalt")              { applyClearHalt(); mqttPublishState(); return; }

    // Manual actuator commands: parse optional duration ("pump_on:10", "feed:5").
    String act = c; long sec = 0;
    int colon = c.indexOf(':');
    if (colon > 0) { act = c.substring(0, colon); sec = c.substring(colon + 1).toInt(); }

    if (autoMode) {   // same safety gate as the dashboard
        Serial.println("[MQTT] ignored: auto mode on (manual control only in OFF)");
        return;
    }
    if (applyManualCmd(act, sec)) mqttPublishState();
    else Serial.println("[MQTT] unknown command — ignored");
}
#endif

#if EMAIL_ALERTS_ENABLED
// Send an email on an anomaly via a free Google Apps Script web app (one short
// HTTPS GET). To stay within RAM on the plain ESP32, the MQTT TLS session is
// dropped for the duration of the call (the alert has already been published to
// the broker by this point) and reconnects on the next loop. Failure is non-fatal.
void sendEmailAlert(const char* reason, const char* severity) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (strncmp(EMAIL_WEBHOOK_URL, "PASTE", 5) == 0) return;   // not configured yet
#if MQTT_ENABLED
    if (mqtt.connected()) mqtt.disconnect();           // free the TLS context
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
            https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // Apps Script 302s to googleusercontent.com
            https.setConnectTimeout(5000);
            https.setTimeout(8000);                                  // script cold-start can be slow
            int code = https.GET();
            Serial.printf("[EMAIL] webhook -> %d\n", code);
            https.end();
        } else {
            Serial.println("[EMAIL] request setup failed");
        }
    }
#if MQTT_ENABLED
    mqtt_last_try = 0;   // reconnect MQTT immediately on the next loop
#endif
}
#endif

// ── ESP-NOW receive callback ───────────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    // ── HALT echo (safety net) ───────────────────────────────────────────
    // If we're locked we keep telling the target so on every single message
    // it sends us. This covers the scenario in which the original HALT
    // (sent right at stall confirmation) was lost because of the voltage
    // sag / radio drop caused by the stall itself. On any subsequent target
    // boot, the very first message (a [BOOT] log) triggers this echo and
    // the target sets its NVS halted flag, going back to sleep at the next
    // gate in setup(). The check is BEFORE the pending-cmd block because
    // an AUTO_ON_RESET (which clears system_locked first) must not be
    // shadowed by a stale HALT.
    if (system_locked) {
        esp_now_send(mac, (const uint8_t*)"HALT", 4);
    }

    // ── Piggy-back pending default-mode command to the target ────────────
    // When the dashboard toggle changes, we don't know when the target will
    // be awake. Instead of polling, we ride along on any message the target
    // sends us and reply with the current state command. pendingTargetCount
    // (default 5) ensures we keep sending for several incoming messages so
    // at least one reliably reaches the target.
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
        // Idle heartbeat from a target paused in default-mode OFF. Payload is
        // two digits "<pump><halt>": pump state then halt state ("HB:00" idle,
        // "HB:10" pumping, "HB:01" halted). Three jobs:
        //  1) reconcile desired vs actual — if the dashboard wants ON but the
        //     target is still OFF, push AUTO_ON_RESET so it reboots into a
        //     fresh cycle (makes OFF→ON work while idle, and re-syncs after a
        //     physical reset when the transient pendingTargetCmd was lost);
        //  2) record pump state so /data lights the dashboard pump control;
        //  3) record target-side halt so the dashboard can show "Clear HALT"
        //     even when this Observer was reset and lost its own lock.
        targetPumpOn   = (s.length() > 3 && s.charAt(3) == '1');
        targetHalted   = (s.length() > 4 && s.charAt(4) == '1');
        if (autoMode && !targetHalted) {
            // Bring the idle target online with a fresh learning cycle
            // (AUTO_ON_RESET → boot 0). Skipped while the target is halted: it
            // stays awake to be recovered with Clear HALT and must not be
            // restarted out from under the halt.
            const char* cmd = "CMD:AUTO_ON_RESET";
            esp_now_send(mac, (const uint8_t*)cmd, strlen(cmd));
        }
        return;
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
        // Remember who triggered this learn: a manual Calibrate (autoMode=false)
        // produces a baseline that should survive the next OFF→ON; the automatic
        // first-boot learn (autoMode=true) does not.
        calFromManual        = !autoMode;
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
        eval_cycle_recorded = false;   // new monitored cycle → arm one eval record
        eval_onset_ms       = 0;
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
            drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;  // new μ → restart drift

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

        // End of a MONITORING cycle: fold this cycle's healthy mean into the
        // degradation CUSUM. Skipped after a learning cycle (no drift data) and
        // when the cycle was halted (system_locked) or too short to be reliable.
        if (current_mode == "MONITORING" && is_calibrated &&
            !system_locked && drift_cycle_n >= 4 && baseline_mean > 1.0f) {
            float cycle_mean = drift_cycle_sum / drift_cycle_n;
            float k = DRIFT_K_SIGMA * baseline_std;
            float dev = (cycle_mean - baseline_mean) - k;        // beyond tolerance
            drift_cusum += dev; if (drift_cusum < 0.0f) drift_cusum = 0.0f;
            float limit = DRIFT_H_SIGMA * baseline_std;
            Serial.printf("[DRIFT] cycle μ=%.1f mA (baseline %.1f, +%.1f)  CUSUM=%.1f / %.1f\n",
                          cycle_mean, baseline_mean, cycle_mean - baseline_mean,
                          drift_cusum, limit);
            if (drift_cusum > limit) {
                Serial.println("[WARN] DEGRADATION: pump current creeping up — service recommended.");
                push_event("warn", "DEGRADATION");
                buzzerAlert(2);
#if MQTT_ENABLED
                mqttPublishAlert("DEGRADATION", "warn");
#endif
#if EMAIL_ALERTS_ENABLED
                sendEmailAlert("DEGRADATION", "warn");
#endif
                drift_cusum = 0.0f;   // re-arm for the next sustained creep
            }
        }
        drift_cycle_sum = 0.0f; drift_cycle_n = 0;   // reset accumulator for next cycle

        // Evaluation: a monitored cycle that ended without a confirmed anomaly
        // counts as "detected NORMAL" — a true negative if truth was NORMAL, a
        // false negative (missed fault) if truth was a fault class.
        if (current_mode == "MONITORING" && eval_truth >= 0 && !eval_cycle_recorded) {
            confmat[eval_truth][0]++;
            eval_count++;
            eval_cycle_recorded = true;
            Serial.printf("[EVAL] truth=%d detected=0 (no anomaly)  (n=%d)\n",
                          eval_truth, eval_count);
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
    char buf[460];
    snprintf(buf, sizeof(buf),
        "{\"I\":%.2f,\"I_ewma\":%.2f,\"V\":%.2f,\"T\":%.2f,\"turb\":%.1f"
        ",\"mode\":\"%s\",\"locked\":%s,\"auto\":%s,\"pump\":%s,\"thalt\":%s"
        ",\"th_stall\":%.2f,\"th_dry\":%.2f,\"th_thi\":%.2f,\"th_tlo\":%.2f"
        ",\"evt_seq\":%lu}",
        last_current, ewma_current, last_voltage, last_temp_c, last_turb,
        current_mode.c_str(),
        system_locked ? "true" : "false",
        autoMode      ? "true" : "false",
        targetPumpOn  ? "true" : "false",
        targetHalted  ? "true" : "false",
        th_stall, th_dry_run, th_temp_high, th_temp_low,
        (unsigned long)evt_seq);
    server.send(200, "application/json", buf);
}

// Evaluation endpoint.
//   /eval?truth=K  (K=0..5)  → label the upcoming monitored cycles
//   /eval?off=1               → stop labelling
//   /eval?reset=1             → clear the confusion matrix and latency
//   /eval                     → JSON: truth, count, mean latency, flat 6×6 matrix
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
// Clear a halt latch. Shared by HTTP (/cmd?a=clear_halt) and MQTT. Valid in both
// modes: unlock the observer and tell the target to clear its latch (in auto mode
// the target reboots into a fresh cycle; in manual mode it returns to idle).
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

// target is awake in its idle loop and is a registered ESP-NOW peer, so we
// send straight to it, repeated a few times for reliability like the HALT echo.
//   /cmd?a=pump_on&sec=N   (N=0 or absent → infinite)
//   /cmd?a=pump_off
//   /cmd?a=feed&sec=N      (N=0 or absent → one-shot; N>0 → repeat every N s)
//   /cmd?a=calibrate
// Manual actuator commands shared by HTTP /cmd and MQTT. Builds the ESP-NOW
// command, updates local state/events, and sends it to the target. Returns
// false for an unknown action. (clear_halt and mode are handled separately.)
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
    } else if (a == "calibrate") {
        snprintf(cmd, sizeof(cmd), "CMD:CALIBRATE");
        push_event("phase", "Manual calibration requested");
    } else {
        return false;
    }
    for (int i = 0; i < 5; i++) { espNowSend(cmd); delay(8); }
    return true;
}

void handleCmd() {
    String a = server.hasArg("a") ? server.arg("a") : "";
    // Clear HALT must work in BOTH modes so an automatic-mode halt is
    // recoverable from the dashboard, exactly like a manual-mode halt. Every
    // other manual control is disabled while the automatic cycle owns the pump.
    if (autoMode && a != "clear_halt") {
        server.send(409, "application/json", "{\"err\":\"auto mode on\"}");
        return;
    }
    if (a == "clear_halt") {
        applyClearHalt();   // shared with the MQTT path
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

// /mode?set=on  → enable default cycle  (and reset state if coming from OFF)
// /mode?set=off → suspend default cycle (system goes to IDLE)
// Response: {"auto":true|false}
// Apply a default-mode change (shared by HTTP /mode and MQTT). Persists the new
// state to NVS and, on a real transition, runs the OFF→ON (fresh relearn) or
// ON→OFF (pause + discard calibration) reset sequence.
void applyMode(bool on) {
    bool was = autoMode;
    autoMode = on;
    obsPrefs.putBool("auto", autoMode);

    if (autoMode && !was) {
        // OFF → ON transition: ALWAYS start a fresh learning cycle. Any
        // previous calibration (manual or automatic) is discarded so the
        // baseline always matches the current setup. Target relearns at
        // boot 0; the dashboard threshold lines are cleared until the new
        // calibration completes.
        system_locked     = false;
        anomaly_reason    = "NONE";
        last_anomaly_pushed = "NONE";
        current_mode      = "IDLE";
        stall_confirm = volt_confirm = dry_confirm = temp_confirm = 0;
        ewma_init         = false;
        grace_period      = 0;
        targetPumpOn      = false;   // manual pump state wiped on reset
        is_calibrated     = false;
        calFromManual     = false;
        th_stall = th_dry_run = th_temp_high = th_temp_low = 0.0f;  // clear chart lines
        drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;
        pendingTargetCmd   = "CMD:AUTO_ON_RESET";
        pendingTargetCount = 5;
        push_event("phase", "Default mode ON — fresh learning starting");
        Serial.println("[OBS] Default mode → ON (fresh learning)");
    } else if (!autoMode && was) {
        // ON → OFF transition: pause the cycle and DISCARD any calibration.
        // The user re-calibrates manually in OFF (or it relearns on next ON).
        current_mode  = "IDLE";
        stall_confirm = volt_confirm = dry_confirm = temp_confirm = 0;
        ewma_init     = false;
        targetPumpOn  = false;   // target enters idle with pump off
        is_calibrated = false;   // wipe calibration
        calFromManual = false;
        anomaly_reason = "NONE";
        last_anomaly_pushed = "NONE";
        th_stall = th_dry_run = th_temp_high = th_temp_low = 0.0f;   // clear chart lines
        drift_cusum = 0.0f; drift_cycle_sum = 0.0f; drift_cycle_n = 0;
        pendingTargetCmd   = "CMD:AUTO_OFF";
        pendingTargetCount = 5;
        push_event("phase", "Default mode OFF — system paused, calibration cleared");
        Serial.println("[OBS] Default mode → OFF (calibration cleared)");
    }
    // No transition → no command needed
}

// /mode?set=on  → enable default cycle  (and reset state if coming from OFF)
// /mode?set=off → suspend default cycle (system goes to IDLE)
// Response: {"auto":true|false}
void handleMode() {
    if (server.hasArg("set")) {
        String v = server.arg("set"); v.toLowerCase();
        applyMode(v == "on" || v == "1" || v == "true");
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"auto\":%s}", autoMode ? "true" : "false");
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
#if MQTT_ENABLED
        mqtt.setServer(MQTT_HOST, MQTT_PORT);
        mqtt.setBufferSize(512);   // our state/telemetry JSON exceeds the 256-byte default
        mqtt.setCallback(mqttCallback);   // handle desired-state commands
  #if MQTT_TLS
    #if MQTT_TLS_VERIFY
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // TLS needs real time for cert dates
        mqttNet.setCACert(MQTT_ROOT_CA);                    // authenticate the broker
    #else
        mqttNet.setInsecure();   // TLS encryption without server-certificate validation
    #endif
  #endif
#endif
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
    esp_now_set_pmk(ESPNOW_PMK);              // shared primary master key

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, targetAddress, 6);
    // channel 0 = "use the current radio channel". As a STA the observer's radio
    // follows the hotspot if it hops channels; pinning the peer to the channel
    // captured at boot would break ESP-NOW on the next hop, so we send on the
    // current channel instead.
    peer.channel = 0;
    memcpy(peer.lmk, ESPNOW_LMK, 16);         // per-peer key → AES-CCM
    peer.encrypt = true;
    esp_now_add_peer(&peer);

    // Register HTTP routes; quick 204 for /favicon.ico and other unknowns.
    server.on("/",            HTTP_GET, handleRoot);
    server.on("/data",        HTTP_GET, handleData);
    server.on("/events",      HTTP_GET, handleEvents);
    server.on("/mode",        HTTP_GET, handleMode);   // dashboard toggle
    server.on("/cmd",         HTTP_GET, handleCmd);    // manual controls (OFF only)
    server.on("/eval",        HTTP_GET, handleEval);   // evaluation harness
    // Browsers automatically request /favicon.ico — registering an explicit
    // empty handler silences the WebServer "request handler not found" log.
    server.on("/favicon.ico", HTTP_GET, [](){ server.send(204, "text/plain", ""); });
    server.onNotFound([](){ server.send(204, "text/plain", ""); });
    server.begin();

    // Load persistent default-mode toggle from NVS (defaults to ON first boot)
    obsPrefs.begin("float-obs", false);
    autoMode = obsPrefs.getBool("auto", true);
    Serial.printf("[OBS] Default mode at boot: %s\n", autoMode ? "ON" : "OFF");

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
#if MQTT_ENABLED
    mqttTask();              // cloud keepalive + periodic telemetry/state publish
#endif

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

        // Evaluation: timestamp the first RAW out-of-range sample of the cycle.
        // Latency is measured from here to ANOMALY CONFIRMED, so it captures the
        // full EWMA-smoothing + confirmation delay, not just a fixed count.
        if (eval_truth >= 0 && eval_onset_ms == 0) {
            bool raw_bad = (last_current > th_stall) ||
                           (th_dry_run  > 0.1f && last_current < th_dry_run) ||
                           (th_volt_min > 0.1f && last_voltage < th_volt_min) ||
                           temp_flag;
            if (raw_bad) eval_onset_ms = (uint32_t)millis();
        }

        if (stall_flag) stall_confirm++; else stall_confirm = 0;
        if (volt_flag)  volt_confirm++;  else volt_confirm = 0;
        if (dry_flag)   dry_confirm++;   else dry_confirm = 0;
        if (temp_flag)  temp_confirm++;  else temp_confirm = 0;

        // Degradation tracking: collect only healthy operating samples (a stall
        // or dry-run spike must not pollute the cycle's representative current).
        if (!stall_flag && !dry_flag) {
            drift_cycle_sum += ewma_current;
            drift_cycle_n++;
        }

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

            // Evaluation: record this cycle's outcome once (detected = reason),
            // with detection latency from the first raw-abnormal sample.
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

            // Dashboard pop-up: push a typed event the UI turns into a toast.
            // "halt" = red (stops the pump), "warn" = orange (advisory only).
            // Throttled so a persistent temp/voltage condition doesn't spam:
            // re-push only when the reason changes or after 15 s.
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
                targetHalted  = true;
            }

            char alert[64];
            snprintf(alert, sizeof(alert), "ALERT:%s", anomaly_reason.c_str());
            espNowSend(alert);

            buzzerAlert(3);

            // Email LAST: it's a blocking HTTPS call (~seconds), so it must run only
            // after the pump has already been commanded to stop (HALT above).
#if EMAIL_ALERTS_ENABLED
            if (notify_now) sendEmailAlert(anomaly_reason.c_str(), isHalt ? "halt" : "warn");
#endif
            
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