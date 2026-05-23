#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "dashboard.h"

const int PUMP_PIN = 47;
const int SERVO_PIN = 6;
const int BUZZER_PIN = 7;
const int I2C_SDA = 41;
const int I2C_SCL = 42;

const char* ssid = "WIFI";
const char* password = "PASSWORD";

WebServer server(80);
Adafruit_INA219 ina219;

// --- STATO DEL SISTEMA ---
String system_status = "IDLE"; // IDLE, LEARNING, MONITORING, HALTED
String anomaly_type = "NONE";
bool pump_on = false;
bool servo_active = false;

// --- AUTO MODE VARIABLES ---
bool auto_mode = false;
unsigned long last_auto_check = 0;
unsigned long pump_auto_stop_time = 0;
bool pump_is_auto_running = false;

// --- EDGE AI & SENSORI ---
float current_mA = 0.0;
int turbidity = 30;
float water_temp = 24.5;

const int NUM_SAMPLES = 40;
float raw_samples[NUM_SAMPLES];
int sample_idx = 0;

float baseline_mean = 0;
float th_hard_stall = 0;
float th_dry_run = 0;
int stall_confirm = 0;

// ==========================================
// FUNZIONI API SERVER
// ==========================================
void handleRoot() { server.send(200, "text/html", html_page); }

void handleData() {
    // Genera dati verosimili
    if (auto_mode) turbidity = (turbidity + random(-5, 6)); 
    if(turbidity < 0) turbidity = 0; if(turbidity > 100) turbidity = 100;
    water_temp = water_temp + random(-1, 2) / 10.0;

    String json = "{";
    json += "\"current\":" + String(current_mA) + ",";
    json += "\"pump\":" + String(pump_on ? "true" : "false") + ",";
    json += "\"status\":\"" + system_status + "\",";
    json += "\"anomaly\":\"" + anomaly_type + "\",";
    json += "\"auto_mode\":" + String(auto_mode ? "true" : "false") + ",";
    json += "\"th_stall\":" + String(th_hard_stall) + ",";
    json += "\"th_dry\":" + String(th_dry_run) + ",";
    json += "\"turbidity\":" + String(turbidity) + ",";
    json += "\"temp\":" + String(water_temp);
    json += "}";
    server.send(200, "application/json", json);
}

void handleLearn() {
    if (system_status == "HALTED") { server.send(400, "text/plain", "Reset First"); return; }
    // Forza accensione pompa per imparare
    pump_on = true;
    digitalWrite(PUMP_PIN, HIGH);
    system_status = "LEARNING";
    sample_idx = 0;
    auto_mode = false; // Disabilita l'auto durante il learning
    server.send(200, "text/plain", "Learning Started");
}

void handleAuto() {
    if (system_status == "HALTED") { server.send(400, "text/plain", "Reset First"); return; }
    auto_mode = !auto_mode;
    if (auto_mode) last_auto_check = millis() - 10000; // Forza controllo immediato
    server.send(200, "text/plain", "Auto Toggled");
}

void handlePump() {
    if (system_status == "HALTED" || system_status == "LEARNING") { server.send(400, "text/plain", "Locked"); return; }
    pump_on = !pump_on;
    digitalWrite(PUMP_PIN, pump_on ? HIGH : LOW);
    auto_mode = false; // Override manuale spegne l'auto mode
    system_status = pump_on ? "MONITORING" : "IDLE";
    server.send(200, "text/plain", "Pump Toggled");
}

void handleServo() { 
    servo_active = true; 
    auto_mode = false; // Override manuale
    server.send(200, "text/plain", "Feeding"); 
}

void handleReset() {
    system_status = "IDLE"; 
    anomaly_type = "NONE"; 
    stall_confirm = 0; 
    pump_on = false;
    auto_mode = false;
    digitalWrite(PUMP_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW); 
    server.send(200, "text/plain", "Reset OK");
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
    Serial.begin(115200);
    pinMode(PUMP_PIN, OUTPUT); pinMode(SERVO_PIN, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(PUMP_PIN, LOW); digitalWrite(SERVO_PIN, LOW); digitalWrite(BUZZER_PIN, LOW);

    // 1. PRIMA avviamo il Wi-Fi (così il picco di corrente iniziale non fa crashare l'INA219)
    Serial.print("Connessione WiFi...");
    WiFi.mode(WIFI_STA); WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nIP DASHBOARD: http://" + WiFi.localIP().toString());

    // 2. DOPO blocchiamo i pin I2C a livello hardware
    Wire.setPins(I2C_SDA, I2C_SCL); 
    Wire.begin();
    Wire.setClock(100000); // <--- AGGIUNGI QUESTA RIGA: Rallenta il bus a 100kHz per stabilità su breadboard
    delay(100);            // <--- AGGIUNGI QUESTA RIGA: Dà tempo al sensore di svegliarsi

    // 3. Avviamo l'INA219 in sicurezza
    if (!ina219.begin()) {
        Serial.println("ERRORE CRITICO: INA219 non trovato! Controlla i cavi.");
        while (1) { delay(100); } 
    }
    Serial.println("INA219 Inizializzato con successo!");

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/data", HTTP_GET, handleData);
    server.on("/api/learn", HTTP_POST, handleLearn);
    server.on("/api/auto", HTTP_POST, handleAuto);
    server.on("/api/pump", HTTP_POST, handlePump);
    server.on("/api/servo", HTTP_POST, handleServo);
    server.on("/api/reset", HTTP_POST, handleReset);
    server.begin();
}

void loop() {
    server.handleClient();
    
    // 1. GESTIONE SERVO (Non blocca il server)
    if (servo_active) {
        for(int i=0; i<30; i++) { digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(1500); digitalWrite(SERVO_PIN, LOW); delay(18); } delay(500);
        for(int i=0; i<30; i++) { digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(1000); digitalWrite(SERVO_PIN, LOW); delay(18); }
        servo_active = false;
    }

    // 2. GESTIONE AUTO MODE (Ogni 10 secondi)
    if (auto_mode && system_status != "HALTED" && system_status != "LEARNING" && !pump_is_auto_running) {
        if (millis() - last_auto_check > 10000) {
            last_auto_check = millis();
            
            if (turbidity > 50) {
                // Acqua sporca -> Pompa ON per 5 sec
                pump_on = true;
                digitalWrite(PUMP_PIN, HIGH);
                pump_auto_stop_time = millis() + 5000;
                pump_is_auto_running = true;
                system_status = "MONITORING";
                Serial.println("[AUTO] Torbidita alta, avvio pompa per 5s.");
            } else {
                // Acqua pulita -> Cibo
                servo_active = true;
                Serial.println("[AUTO] Acqua pulita, erogazione cibo.");
            }
        }
    }

    // 3. SPEGNIMENTO AUTOMATICO POMPA (Se accesa dall'Auto Mode)
    if (pump_is_auto_running && millis() > pump_auto_stop_time) {
        pump_on = false;
        digitalWrite(PUMP_PIN, LOW);
        pump_is_auto_running = false;
        if (system_status != "HALTED") system_status = "IDLE";
        Serial.println("[AUTO] Ciclo pompa completato.");
    }

    // 4. EDGE AI: Monitoraggio e Calibrazione (Ogni 200ms)
    static unsigned long last_read = 0;
    if (millis() - last_read > 200) {
        last_read = millis();
        // Lettura con Auto-Recovery
        float raw_current = ina219.getCurrent_mA();
        
        // Se la libreria fallisce catastroficamente o restituisce valori insensati
        // (spesso legge circa ~3200 o cade a 0 fisso se il bus è morto)
        if (raw_current > 3000.0 || isnan(raw_current)) {
            Serial.println("[CRITICO] Rilevato blocco I2C. Tentativo di riavvio INA219...");
            Wire.end(); // Chiude il bus bloccato
            delay(10);
            Wire.setPins(I2C_SDA, I2C_SCL);
            Wire.begin();
            ina219.begin();
            current_mA = 0.0;
        } else {
            current_mA = max(0.0f, raw_current);
        }

        // FASE DI APPRENDIMENTO MANUALE
        if (pump_on && system_status == "LEARNING") {
            if (sample_idx < NUM_SAMPLES) {
                raw_samples[sample_idx++] = current_mA;
            } else {
                // Calcolo Baseline (Z-Score Filtering)
                float sum = 0; for(int i=0; i<NUM_SAMPLES; i++) sum += raw_samples[i];
                float raw_mean = sum / NUM_SAMPLES;
                float sq_sum = 0; for(int i=0; i<NUM_SAMPLES; i++) sq_sum += pow(raw_samples[i] - raw_mean, 2);
                float raw_std = sqrt(sq_sum / NUM_SAMPLES);

                float clean_sum = 0; int clean_count = 0;
                for(int i=0; i<NUM_SAMPLES; i++) {
                    if (abs(raw_samples[i] - raw_mean) / raw_std <= 1.5) {
                        clean_sum += raw_samples[i]; clean_count++;
                    }
                }
                
                baseline_mean = clean_sum / clean_count;
                float clean_sq = 0;
                for(int i=0; i<NUM_SAMPLES; i++) {
                    if (abs(raw_samples[i] - raw_mean) / raw_std <= 1.5) clean_sq += pow(raw_samples[i] - baseline_mean, 2);
                }
                float clean_std = sqrt(clean_sq / clean_count);

                th_hard_stall = baseline_mean + (3.0 * clean_std);
                th_dry_run = baseline_mean - (3.0 * clean_std);
                if (th_hard_stall < baseline_mean + 15.0) th_hard_stall = baseline_mean + 15.0;

                // Calibrazione finita: SPEGNI LA POMPA E TORNA IN IDLE
                pump_on = false;
                digitalWrite(PUMP_PIN, LOW);
                system_status = "IDLE";
                Serial.println("Calibrazione completata. Soglie impostate.");
            }
        } 
        // FASE DI MONITORAGGIO (Sicurezza)
        else if (pump_on && system_status == "MONITORING" && th_hard_stall > 0) {
            // Salta i primissimi decimi di secondo di "spunto" del motore
            static unsigned long pump_start_time = millis();
            if (!pump_is_auto_running) pump_start_time = millis(); // Reset se manuale

            if (current_mA > th_hard_stall || (current_mA < th_dry_run && current_mA > 10.0)) {
                stall_confirm++;
                if (stall_confirm >= 3) {
                    pump_on = false; 
                    digitalWrite(PUMP_PIN, LOW);
                    auto_mode = false;
                    pump_is_auto_running = false;
                    system_status = "HALTED";
                    anomaly_type = (current_mA > th_hard_stall) ? "MOTOR STALL" : "DRY RUN";
                    digitalWrite(BUZZER_PIN, HIGH);
                }
            } else {
                stall_confirm = 0;
            }
        }
    }
}