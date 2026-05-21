# FLOAT – Guida all'Implementazione dei Task Mancanti

> Documento tecnico di riferimento per il completamento del progetto.
> Basato sull'analisi di `target.cpp`, `observer.cpp`, `dashboard.h` e delle slide L21.

---

## 1. Filtro di Hampel (sostituzione della 3σ rule)

### Perché cambiare?
La regola 3σ standard è sensibile agli outlier stessi che vuole rilevare: se durante il learning
entrano picchi anomali, `baseline_mean` e `baseline_std` vengono distorte, alzando la soglia
`th_stall` verso valori non rappresentativi. Il filtro di Hampel usa la **Mediana Assoluta delle
Deviazioni (MAD)**, robusta agli outlier per costruzione.

### Teoria
```
MAD = mediana( |xᵢ - mediana(x)| )
σ_hampel ≈ 1.4826 × MAD          (fattore di consistenza per distribuzione normale)
outlier se |xᵢ - med| > k × σ_hampel    (k = 3 tipicamente)
```

### Implementazione in `observer.cpp`

Sostituire la funzione `robustStats` con le seguenti due funzioni:

```cpp
// ── Hampel helpers ────────────────────────────────────────────────────────

// Median of an array (modifies a copy internally)
float arrayMedian(float* arr, int n) {
    // Simple insertion sort on a copy
    float buf[n];
    memcpy(buf, arr, n * sizeof(float));
    for (int i = 1; i < n; i++) {
        float key = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = key;
    }
    return (n % 2 == 0) ? (buf[n/2-1] + buf[n/2]) / 2.0f : buf[n/2];
}

// Hampel robust stats: fills mean and std_dev equivalent
void hampelStats(float* arr, int n, float k_sigma,
                 float& clean_mean, float& clean_std) {
    if (n < 3) { computeStats(arr, n, clean_mean, clean_std); return; }

    float med = arrayMedian(arr, n);

    // Compute deviations |xᵢ - med|
    float devs[n];
    for (int i = 0; i < n; i++) devs[i] = fabsf(arr[i] - med);

    float mad = arrayMedian(devs, n);
    float sigma_h = 1.4826f * mad;          // consistent estimator of σ

    // Filter: keep only inliers
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
```

Nel blocco `STOP_MEASURE` di `OnDataRecv`, sostituire la chiamata:
```cpp
// Prima:
robustStats(samples, sample_idx, 1.5f, clean_mean, clean_std);

// Dopo:
hampelStats(samples, sample_idx, 3.0f, clean_mean, clean_std);
```

Il parametro `k = 3.0` è il punto di partenza standard; abbassarlo a `2.5` per rendere la
rilevazione più sensibile.

---

## 2. Sincronizzazione Observer ↔ Target

### Problema attuale
I due nodi usano lo stesso indirizzo broadcast `FF:FF:FF:FF:FF:FF` e si affidano ai messaggi
per coordinarsi, ma non c'è nessun handshake: se l'Observer non ha ancora caricato quando il
Target trasmette `START_LEARN`, il comando va perso.

### Soluzione: ACK e retry con backoff

**In `target.cpp`** – aggiungere stato RTC e logica di retry:

```cpp
// ── In cima al file, dopo le variabili RTC esistenti ─────────────────────
RTC_DATA_ATTR uint32_t last_cmd_id = 0;   // incrementale per de-duplicazione

// ── Funzione sendMsgWithACK (sostituisce sendMsg per i CMD critici) ───────
volatile bool ack_received = false;
volatile uint32_t acked_id  = 0;

// Nel callback OnDataRecv aggiungere:
} else if (s.startsWith("ACK:")) {
    acked_id     = s.substring(4).toInt();
    ack_received = true;
}

bool sendMsgWithACK(const String& type, const String& content,
                    int retries = 5, int timeout_ms = 300) {
    last_cmd_id++;
    String full = type + ":" + content + "|ID:" + String(last_cmd_id);
    ack_received = false;
    for (int i = 0; i < retries; i++) {
        esp_now_send(observerAddress, (const uint8_t*)full.c_str(), full.length());
        unsigned long t0 = millis();
        while (millis() - t0 < timeout_ms) {
            if (ack_received && acked_id == last_cmd_id) return true;
            delay(10);
        }
        delay(50 * (i + 1));   // exponential backoff
    }
    Serial.printf("[TGT] WARNING: no ACK for CMD %s after %d retries\n",
                  content.c_str(), retries);
    return false;
}
```

**In `observer.cpp`** – rispondere con ACK:

```cpp
// Nella funzione OnDataRecv, modificare il parsing dei CMD:
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1];
    memcpy(msg, data, len);
    msg[len] = '\0';
    String s(msg);

    // Estrai ID se presente (formato "TYPE:CONTENT|ID:NNN")
    uint32_t msg_id = 0;
    int id_pos = s.indexOf("|ID:");
    if (id_pos != -1) {
        msg_id = s.substring(id_pos + 4).toInt();
        s = s.substring(0, id_pos);   // tronca l'ID dalla stringa
        // Invia ACK
        char ack_buf[32];
        snprintf(ack_buf, sizeof(ack_buf), "ACK:%lu", msg_id);
        esp_now_send(targetAddress, (const uint8_t*)ack_buf, strlen(ack_buf));
    }
    // ... resto del parsing invariato
}
```

Usare `sendMsgWithACK` solo per i comandi critici (`START_LEARN`, `START_MONITOR`,
`STOP_MEASURE`). Per telemetria e log continuare a usare `sendMsg` senza ACK.

---

## 3. Dashboard – Completamento

La dashboard HTML è già ben strutturata in `dashboard.h`. Mancano lato **server** (il file
`main.cpp` del nodo Dashboard non è allegato ma va costruito così):

### 3a. `main.cpp` del nodo Dashboard (ESP32 con WiFi AP)

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "dashboard.h"

// ── AP credentials ─────────────────────────────────────────────────────────
const char* AP_SSID = "FLOAT-Dashboard";
const char* AP_PASS = "float1234";       // min 8 chars per WPA2

WebServer server(80);

// ── Shared state (updated by ESP-NOW callback) ──────────────────────────
struct DashState {
    float current   = 0, ewma = 0, voltage = 0;
    float temp      = 0, turbidity = 0;
    float th_stall  = 0, th_dry   = 0;
    String status   = "IDLE";
    String anomaly  = "NONE";
    bool pump       = false;
    bool auto_mode  = false;
} state;

uint8_t observerMAC[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};  // aggiornare con MAC reale

// ── SSE helpers ────────────────────────────────────────────────────────────
WiFiClient sseClient;
bool sseActive = false;

void pushSSE() {
    if (!sseActive || !sseClient.connected()) { sseActive = false; return; }
    StaticJsonDocument<256> doc;
    doc["current"]   = state.current;
    doc["ewma"]      = state.ewma;
    doc["voltage"]   = state.voltage;
    doc["temp"]      = state.temp;
    doc["turbidity"] = state.turbidity;
    doc["th_stall"]  = state.th_stall;
    doc["th_dry"]    = state.th_dry;
    doc["status"]    = state.status;
    doc["anomaly"]   = state.anomaly;
    doc["pump"]      = state.pump;
    doc["auto_mode"] = state.auto_mode;
    String body; serializeJson(doc, body);
    sseClient.print("data: " + body + "\n\n");
}

// ── ESP-NOW receive (from Observer) ────────────────────────────────────────
void OnDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    char msg[len + 1]; memcpy(msg, data, len); msg[len] = '\0';
    String s(msg);

    if (s.startsWith("TELEM:")) {
        // Format: TELEM:current,voltage,temp,STATUS,anomaly
        String payload = s.substring(6);
        int c1 = payload.indexOf(',');
        int c2 = payload.indexOf(',', c1+1);
        int c3 = payload.indexOf(',', c2+1);
        int c4 = payload.indexOf(',', c3+1);
        state.current = payload.substring(0, c1).toFloat();
        state.voltage = payload.substring(c1+1, c2).toFloat();
        state.temp    = payload.substring(c2+1, c3).toFloat();
        state.status  = payload.substring(c3+1, c4);
        state.anomaly = payload.substring(c4+1);
    } else if (s.startsWith("CAL:")) {
        // Format: CAL:mean,std,th_stall
        String p = s.substring(4);
        int c1 = p.indexOf(','), c2 = p.indexOf(',', c1+1);
        state.th_stall = p.substring(c2+1).toFloat();
    } else if (s.startsWith("DATA:SENSOR:")) {
        String p = s.substring(12);
        int ci = p.indexOf(',');
        state.turbidity = p.substring(0, ci).toFloat();
        state.temp      = p.substring(ci+1).toFloat();
    } else if (s.startsWith("ALERT:")) {
        state.anomaly = s.substring(6);
        state.status  = "HALTED";
    }
}

// ── HTTP handlers ──────────────────────────────────────────────────────────
void handleRoot()   { server.send(200, "text/html", html_page); }

void handleEvents() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/event-stream", "");
    sseClient = server.client();
    sseActive = true;
}

void handleReset() {
    state.status = "IDLE"; state.anomaly = "NONE";
    esp_now_send(observerMAC, (const uint8_t*)"CMD:RESET", 9);
    server.send(200, "text/plain", "OK");
}

void handlePump() {
    state.pump = !state.pump;
    const char* cmd = state.pump ? "PUMP:ON" : "PUMP:OFF";
    esp_now_send(observerMAC, (const uint8_t*)cmd, strlen(cmd));
    server.send(200, "text/plain", "OK");
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[DASH] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    esp_wifi_set_channel(13, WIFI_SECOND_CHAN_NONE);

    esp_now_init();
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peer; memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, observerMAC, 6);
    peer.channel = 13; peer.encrypt = false;
    esp_now_add_peer(&peer);

    server.on("/",           handleRoot);
    server.on("/api/events", handleEvents);
    server.on("/api/reset",  HTTP_POST, handleReset);
    server.on("/api/pump",   HTTP_POST, handlePump);
    server.begin();
}

void loop() {
    server.handleClient();
    static unsigned long last_push = 0;
    if (millis() - last_push >= 400) {
        last_push = millis();
        pushSSE();
    }
}
```

### 3b. Sicurezza della Dashboard (IoT Security)

Secondo le slide L21, la sicurezza è un tema esplicito del corso. Aggiungere:

**Autenticazione HTTP Basic** (leggera, adatta a ESP32):
```cpp
bool authenticate() {
    if (!server.authenticate("admin", "float2025")) {
        server.requestAuthentication();
        return false;
    }
    return true;
}
// In ogni handler: if (!authenticate()) return;
```

**Rate limiting** sui comandi POST (anti-spam da browser):
```cpp
unsigned long last_cmd_time = 0;
const unsigned long CMD_COOLDOWN_MS = 2000;

bool rateLimitOK() {
    if (millis() - last_cmd_time < CMD_COOLDOWN_MS) {
        server.send(429, "text/plain", "Too Many Requests");
        return false;
    }
    last_cmd_time = millis();
    return true;
}
```

**HTTPS** non è supportato nativamente da `WebServer.h` su ESP32 senza certificati.
Per il prototipo, il WiFi AP con password WPA2 è sufficiente. Per produzione,
valutare `WiFiClientSecure` con certificato self-signed.

---

## 4. Integrazione Sensore di Temperatura

### Utilità del sensore
La temperatura dell'acqua è rilevante per il progetto per due motivi:

1. **Anomalia termica**: temperatura > 32°C o < 15°C indica malfunzionamento del riscaldatore
   dell'acquario o contaminazione da fonte esterna — da segnalare come anomaly separata.
2. **Correzione della torbidità**: i sensori ottici di torbidità sono dipendenti dalla temperatura
   (la viscosità dell'acqua cambia). Una correzione lineare migliora l'accuratezza:

```cpp
// In target.cpp — funzione readTurbidityNTU() — aggiungere correzione T:
float readTurbidityNTU(float temp_c) {
    // ... ADC reading invariato ...
    float ntu_raw = TURB_MAX_NTU * (float)(TURB_CLEAN_ADC - adc)
                    / (float)(TURB_CLEAN_ADC - TURB_DIRTY_ADC);
    // Correzione temperatura: compensazione lineare ±0.5% per °C rispetto a 25°C
    float temp_correction = 1.0f + 0.005f * (temp_c - 25.0f);
    return constrain(ntu_raw / temp_correction, 0.0f, TURB_MAX_NTU);
}
```

3. **Nuova anomalia in Observer** – aggiungere rilevazione temperatura fuori range:

```cpp
// In observer.cpp, nel blocco MONITORING:
const float TEMP_MIN_C = 18.0f;
const float TEMP_MAX_C = 32.0f;
bool temp_flag = (last_temp_c < TEMP_MIN_C || last_temp_c > TEMP_MAX_C);

bool anomaly = stall_flag || volt_flag || temp_flag;
// ...
if (temp_flag) anomaly_reason = "TEMP_OUT_OF_RANGE";
```

---

## 5. Sensore di Torbidità – Alternativa

### Problema
Il sensore analogico attuale (ADC su pin 1) non è affidabile per diversi motivi:
- ADC dell'ESP32 è notoriamente non lineare sopra ~3.1 V
- Il segnale è sensibile a interferenze da WiFi attivo
- La calibrazione TURB_CLEAN_ADC / TURB_DIRTY_ADC va rifatta per ogni campione d'acqua

### Alternativa 1: DFRobot SEN0189 (stesso sensore, lettura migliorata)
Leggere in modalità analogica **con WiFi spento** (prima di `esp_now_init`):
```cpp
// In target.cpp, PRIMA dell'init WiFi:
float readTurbidityNTU_prewifi() {
    WiFi.mode(WIFI_OFF);  // silenzia radio durante lettura ADC
    delay(50);
    long sum = 0;
    for (int i = 0; i < 64; i++) { sum += analogRead(TURBIDITY_PIN); delay(2); }
    int adc = (int)(sum / 64);
    WiFi.mode(WIFI_STA);  // ripristina
    // ... rest invariato
}
```

### Alternativa 2: Sensore I2C AS7341 (spettrofotometro)
Misura la trasmittanza della luce su 10 lunghezze d'onda. Molto più robusto.
Libreria: `adafruit/Adafruit AS7341`
```cpp
#include <Adafruit_AS7341.h>
Adafruit_AS7341 as7341;
// setup: as7341.begin();
// lettura: as7341.readAllChannels(); float clear = as7341.getChannel(AS7341_CHANNEL_CLEAR);
// NTU ∝ 1/clear (calibrare empiricamente)
```

### Alternativa 3: Approccio MCS dalla Lezione (validazione esterna)
Come suggerito nelle slide L21 (*"To validate your IoT solution you are allowed to use
'more advanced' non-IoT system providing you the ground truth"*):
usare una **fotografia del serbatoio** scattata dallo smartphone tramite app web
(Generic Sensor API + camera) per stimare la torbidità visiva come ground truth,
confrontandola col sensore analogico per costruire la curva di calibrazione.

---

## 6. Latenza e Consumo Energetico

### Latenza – Analisi dell'esistente
Il percorso critico è: Target misura → ESP-NOW → Observer analizza → ESP-NOW → blocco pompa.

| Fase | Tempo stimato |
|------|--------------|
| ADC turbidity (16 campioni × 2 ms) | ~32 ms |
| DS18B20 conversione (11 bit) | ~375 ms |
| ESP-NOW TX + RX callback | ~5 ms |
| Observer EWMA + Z-score | < 1 ms |
| CONFIRM_NEEDED × loop delay (3 × 400 ms) | ~1200 ms |
| ESP-NOW HALT burst (10 × 5 ms) | ~50 ms |
| **Totale worst case** | **~1662 ms** |

La dashboard mostra già `< 2000 ms ✓`. Per ridurre ulteriormente:
- Abbassare `delay(400)` in MONITORING a `delay(200)` (raddoppia il campionamento)
- Ridurre `CONFIRM_NEEDED` da 3 a 2 (accetta più falsi positivi)

### Consumo – Deep Sleep (già al 90% per dichiarazione del progetto)

Il Target usa già `esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL)`.
Ciclo di vita per wakeup da 10 s:

```
Active time ≈ 2 s (boot + sense + trasmissione)
Sleep time  ≈ 10 s
Duty cycle  = 2 / 12 = 16.7%  →  sleep è ~83% del tempo

Consumo medio ≈ I_active × 0.167 + I_sleep × 0.833
             ≈ 160 mA × 0.167  +  0.01 mA × 0.833
             ≈ 26.7 mA
Batteria 2000 mAh → autonomia ≈ 75 ore ≈ 3 giorni
```

Per avvicinarsi all'obiettivo "90% sleep":
```cpp
// Aumentare il periodo di sleep a 20 s:
esp_sleep_enable_timer_wakeup(20ULL * 1000000ULL);
// Active 2s / 22s = 9% active → 91% sleep ✓
```

---

## 7. Nuove Anomalie oltre il Blocco Pompa

Anomalie già implementate: `MOTOR_STALL`, `VOLTAGE_DROP`.

### Anomalie aggiuntive proposte:

**A. DRY_RUN** – pompa accesa ma nessun consumo (no acqua nel tubo):
```cpp
// th_dry = baseline_mean * 0.3  (30% del consumo normale)
// Durante MONITORING:
bool dry_flag = (ewma_current < th_dry && state.pump_on);
if (dry_flag) anomaly_reason = "DRY_RUN";
```
Aggiungere `float th_dry_run = 0.0f;` alle variabili globali e calcolare in `STOP_MEASURE`:
```cpp
th_dry_run = baseline_mean * 0.30f;
```

**B. TEMP_OUT_OF_RANGE** – vedere sezione 4.

**C. PERIODIC_STALL** – la pompa si blocca periodicamente (ostruzione intermittente):
```cpp
// Contatore di anomalie per ciclo:
RTC_DATA_ATTR int anomaly_count = 0;
// In target.cpp dopo ogni wakeup con anomalia ricevuta:
anomaly_count++;
if (anomaly_count >= 3) {
    sendMsg("LOG", "[WARN] Recurring stall — possible partial obstruction");
}
```

**D. COMMUNICATION_LOSS** – Target non riceve risposta dall'Observer:
```cpp
// Già coperto parzialmente da sendMsgWithACK (sezione 2)
// Aggiungere in target.cpp:
RTC_DATA_ATTR int consecutive_noack = 0;
if (!sendMsgWithACK("CMD", "START_LEARN")) {
    consecutive_noack++;
    if (consecutive_noack >= 3) {
        sendMsg("LOG", "[CRITICAL] Observer unreachable — entering safe mode");
        esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);  // sleep lungo
        esp_deep_sleep_start();
    }
}
```

---

## 8. Requisiti e Motivazioni (per la relazione)

### Requisiti funzionali
| ID | Requisito | Implementazione |
|----|-----------|-----------------|
| R1 | Misura torbidità ogni 10 s | Target, `readTurbidityNTU()` |
| R2 | Misura temperatura acqua | Target, DS18B20 |
| R3 | Rilevazione anomalia motore (stall) | Observer, EWMA + Hampel |
| R4 | Blocco d'emergenza pompa < 2 s | Observer → HALT burst |
| R5 | Dashboard real-time | ESP32 WebServer + SSE |
| R6 | Notifiche push anomalia | Browser Notification API |
| R7 | Modalità di calibrazione automatica | Observer, boot count = 0 |

### Motivazioni del progetto
Il sistema FLOAT affronta il problema reale della **gestione non presidiata di acquari**:
- Pompe che si bloccano (stall) danneggiano i motori e privano il serbatoio di circolazione
- Acqua torbida indica infezioni batteriche o decomposizione
- Temperature anomale causano mortalità dei pesci
- La soluzione IoT edge-first (ESP-NOW senza internet) garantisce **latenza < 2 s** anche
  in assenza di connettività cloud, critica per la protezione del sistema.

### Collegamento con Mobile Crowd Sensing (L21)
Come richiesto dalla lezione, è possibile estendere il progetto con MCS:
- **Validazione ground truth**: app web su smartphone (Generic Sensor API) fotografa
  il serbatoio e invia l'immagine a un endpoint REST per stimare la torbidità visiva,
  confrontandola col sensore per la calibrazione continua.
- **Monitoraggio distribuito**: più acquari in una pet-shop condividono dati anonimi
  di torbidità/temperatura via MQTT su broker pubblico (Opportunistic Sensing).
- **Allerta partecipativa**: i proprietari segnalano manualmente eventi (cambio acqua,
  malattia) tramite form web, arricchendo il dataset per l'apprendimento del modello.

---

## 9. Integrazione Lezione L21 nel Progetto

### Generic Sensor API per la Dashboard (web app sul telefono)
La dashboard ESP32 è già una web app. Aggiungere al JavaScript:

```javascript
// Aggiungere nella sezione <script> di dashboard.h:
// Accelerometro per rilevare urti all'acquario (vibrazione anomala)
if ('Accelerometer' in window) {
  try {
    const accel = new Accelerometer({ frequency: 10 });
    accel.addEventListener('reading', () => {
      const mag = Math.sqrt(accel.x**2 + accel.y**2 + accel.z**2);
      if (mag > 15.0) {  // soglia shock in m/s²
        addLog('WARN', `Vibrazione rilevata: ${mag.toFixed(1)} m/s²`);
        // Opzionalmente inviare al server:
        fetch('/api/vibration', { method: 'POST',
          body: JSON.stringify({ magnitude: mag }),
          headers: {'Content-Type':'application/json'} });
      }
    });
    accel.start();
  } catch(e) { console.warn('Accelerometer:', e); }
}
```

Questo è il pattern esatto della slide "Combine feature detection, and defensive
programming" della lezione: try/catch + addEventListener('error').

### MQTT over WebSockets (per futura integrazione cloud)
La lezione mostra esplicitamente MQTT over WebSockets come canale preferito.
Per espandere il progetto verso il cloud, aggiungere in `main.cpp` del Dashboard:
```cpp
// Libreria: knolleary/PubSubClient
// Broker pubblico per test: broker.hivemq.com:8884 (WebSocket TLS)
// Topic: float/<device_id>/telem, float/<device_id>/alert
```

---

## Priorità di Implementazione Suggerita

| Priorità | Task | Effort | Impatto |
|----------|------|--------|---------|
| 🔴 Alta | Filtro Hampel (§1) | 30 min | Alto – migliora rilevazione |
| 🔴 Alta | main.cpp Dashboard (§3a) | 2 h | Alto – manca il codice |
| 🟡 Media | ACK sincrono (§2) | 1 h | Medio – robustezza |
| 🟡 Media | Anomalia DRY_RUN (§7A) | 30 min | Medio – nuova safety |
| 🟡 Media | Correzione T torbidità (§4) | 20 min | Medio – accuracy |
| 🟢 Bassa | Anomalia TEMP (§4) | 20 min | Bassa – già hanno T |
| 🟢 Bassa | Accelerometro MCS (§9) | 30 min | Bassa – dimostrativo |
| 🟢 Bassa | Sleep 20 s (§6) | 5 min | Bassa – tradeoff latenza |