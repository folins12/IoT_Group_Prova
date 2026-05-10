# FLOAT v2 - Edge Predictive Maintenance
*Focus attuale: Diagnostica pompa all'Edge (torbidità temporaneamente esclusa).*

**I Files:**
- `target.cpp`: Attuatore. Legge DS18B20, controlla pompa, servo (cibo) e LED climatici.
- `observer.cpp`: Cervello. Usa INA219. Calcola Latenza Edge e AI (EWMA + Z-Score).
- `dashboard.h`: UI Web in real-time. Calcola Latenza di Rete (Jitter).

**Manutenzione Predittiva & Calcoli:**
Usa la formula `µ ± 3σ` sulla corrente filtrata (EWMA). 
Se I > µ+3σ (Stallo/Detriti) o I < µ-3σ (Dry-run/Svuotamento) -> Invia HALT d'emergenza.

**Controllo Temperatura:**
- `>45°C`: Pompa OFF -> Accende LED Blu (Raffreddamento).
- `<=0°C`: Pompa OFF -> Accende LED Rosso (Riscaldamento).
- Failsafe: Se sonda rotta, imposta finti 25°C per non bloccare il sistema.

**Sviluppi Futuri (Sensor Fusion):**
- *Accelerometro (MPU6050):* Rileva vibrazioni anomale prima dello stallo.
- *Flussimetro:* Incrocia corrente e acqua mossa per rilevare filtri intasati.
- *Torbidità DIY:* Tubo trasparente + LED bianco + Fotoresistenza (LDR) ai lati.