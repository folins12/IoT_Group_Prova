#ifndef DASHBOARD_H
#define DASHBOARD_H

// Static dashboard served by the Observer. Connects to the observer over WiFi
// (the iPhone hotspot network) and polls /data + /events. No buttons — read-only
// live view: motor-current chart with stall & dry-run dashed thresholds, plus
// live cards for temperature, current and turbidity.
// Chart.js is loaded from CDN; this works because the phone hotspot has internet.

const char* html_page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FLOAT Aquarium</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --deep:#020e1a;--surface:#062540;--card:#0a3356;
  --border:rgba(56,178,255,.14);--accent:#38b2ff;--teal:#00e5c8;
  --txt:#c8e6fa;--dim:#5a8caa;--faint:#2a5070;
  --ok:#00e5a0;--warn:#f5c542;--danger:#ff4d6a;
  --r:14px;--sh:0 6px 28px rgba(0,0,0,.5);
}
html,body{min-height:100%;background:var(--deep);color:var(--txt);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:14px}
body::before{content:"";position:fixed;inset:0;pointer-events:none;z-index:0;
  background:
    radial-gradient(ellipse 70% 50% at 15% 5%,rgba(56,178,255,.07),transparent),
    radial-gradient(ellipse 60% 40% at 85% 95%,rgba(0,229,200,.05),transparent)}
.app{position:relative;z-index:1;max-width:1040px;margin:0 auto;padding:18px 14px;
  display:flex;flex-direction:column;gap:14px}

/* Header */
header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;
  gap:10px;padding:16px 20px;background:var(--surface);border:1px solid var(--border);
  border-radius:var(--r);box-shadow:var(--sh)}
.brand{display:flex;align-items:center;gap:12px}
.bi{width:38px;height:38px;border-radius:50%;
  background:linear-gradient(135deg,var(--accent),var(--teal));
  display:flex;align-items:center;justify-content:center;font-size:18px;
  box-shadow:0 0 18px rgba(56,178,255,.4)}
.bn{font-size:1.2rem;font-weight:700;
  background:linear-gradient(90deg,var(--accent),var(--teal));
  -webkit-background-clip:text;-webkit-text-fill-color:transparent}
.bs{font-size:.7rem;color:var(--dim);margin-top:1px}
.hr{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.phase{padding:5px 13px;border-radius:99px;font-size:.72rem;font-weight:700;
  letter-spacing:.6px;border:1px solid var(--faint);color:var(--dim);transition:all .35s}
.phase.LEARNING  {border-color:var(--teal);color:var(--teal)}
.phase.MONITORING{border-color:var(--ok);color:var(--ok)}
.phase.HALTED    {border-color:var(--danger);color:var(--danger);animation:blink 1s infinite}
.conn{display:flex;align-items:center;gap:6px;padding:5px 12px;border-radius:99px;
  border:1px solid var(--border);background:var(--card);font-size:.72rem;font-weight:600}
.dot{width:7px;height:7px;border-radius:50%;background:var(--faint);transition:background .4s}
.dot.on {background:var(--ok);box-shadow:0 0 7px var(--ok)}
.dot.err{background:var(--danger);box-shadow:0 0 7px var(--danger)}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.25}}

/* Default mode toggle */
.toggle-wrap{display:flex;align-items:center;gap:8px;padding:4px 11px;border-radius:99px;
  border:1px solid var(--border);background:var(--card);font-size:.72rem;font-weight:600}
.toggle-lbl{color:var(--dim);text-transform:uppercase;letter-spacing:.6px;font-size:.62rem}
.toggle-state{font-weight:700;letter-spacing:.5px;min-width:24px;text-align:center;
  transition:color .25s}
.toggle-state.on {color:var(--ok)}
.toggle-state.off{color:var(--dim)}
.switch{position:relative;display:inline-block;width:34px;height:18px;cursor:pointer}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;inset:0;background:var(--faint);border-radius:18px;
  transition:.3s}
.slider::before{content:"";position:absolute;height:14px;width:14px;left:2px;top:2px;
  background:#fff;border-radius:50%;transition:.3s}
.switch input:checked + .slider{background:linear-gradient(90deg,var(--accent),var(--teal))}
.switch input:checked + .slider::before{transform:translateX(16px)}
.switch input:disabled + .slider{opacity:.5;cursor:not-allowed}

/* Manual controls (enabled only when default mode is OFF) */
.manual{background:var(--card);border:1px solid var(--border);border-radius:var(--r);
  padding:16px;box-shadow:var(--sh);transition:opacity .3s}
.manual.locked{opacity:.45;pointer-events:none}
.manual-t{font-size:.7rem;font-weight:700;letter-spacing:1px;text-transform:uppercase;
  color:var(--dim);margin-bottom:4px}
.manual-sub{font-size:.66rem;color:var(--faint);margin-bottom:14px}
.mrow{display:flex;align-items:center;gap:10px;flex-wrap:wrap;
  padding:11px 0;border-top:1px solid var(--border)}
.mrow:first-of-type{border-top:none}
.mbtn{appearance:none;border:1px solid var(--faint);background:var(--surface);
  color:var(--txt);font-size:.8rem;font-weight:700;letter-spacing:.4px;
  padding:9px 16px;border-radius:9px;cursor:pointer;transition:all .2s;min-width:128px}
.mbtn:hover:not(:disabled){border-color:var(--accent);box-shadow:0 0 12px rgba(56,178,255,.25)}
.mbtn:disabled{opacity:.5;cursor:not-allowed}
.mbtn.on{background:linear-gradient(90deg,var(--accent),var(--teal));border-color:transparent;color:#021019}
.mbtn.warn{border-color:var(--warn);color:var(--warn)}
.mfields{display:flex;align-items:center;gap:5px;flex-wrap:wrap}
.mfields input{width:64px;background:var(--deep);border:1px solid var(--faint);
  color:var(--txt);border-radius:7px;padding:7px 9px;font-size:.8rem;
  font-variant-numeric:tabular-nums}
.mfields select{background:var(--deep);border:1px solid var(--faint);color:var(--txt);
  border-radius:7px;padding:7px 6px;font-size:.78rem}
.mfields input:disabled,.mfields select:disabled{opacity:.5}
.mhint{font-size:.64rem;color:var(--faint);flex:1;min-width:120px;line-height:1.35}
.mwarn{font-size:.64rem;color:var(--warn);flex-basis:100%;line-height:1.35;
  display:none}
.mwarn.show{display:block}

/* Cards */
.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
@media(max-width:560px){.cards{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--border);border-radius:var(--r);
  padding:16px;box-shadow:var(--sh);position:relative;overflow:hidden}
.card::after{content:"";position:absolute;top:-35%;right:-15%;width:70%;height:70%;
  border-radius:50%;opacity:.08;filter:blur(20px)}
.card.temp::after{background:#f5c542}
.card.cur ::after{background:#38b2ff}
.card.turb::after{background:#a78bfa}
.cl{font-size:.66rem;font-weight:600;letter-spacing:1.2px;text-transform:uppercase;
  color:var(--dim);margin-bottom:9px}
.cv{font-size:2rem;font-weight:700;line-height:1;font-variant-numeric:tabular-nums}
.cu{font-size:.68rem;color:var(--dim);margin-top:3px}
.t-yel{color:#f5c542}.t-blue{color:#38b2ff}.t-pur{color:#a78bfa}

/* Chart */
.chart-box{background:var(--card);border:1px solid var(--border);border-radius:var(--r);
  padding:18px;box-shadow:var(--sh)}
.ch{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;
  flex-wrap:wrap;gap:8px}
.ct{font-size:.7rem;font-weight:700;letter-spacing:1px;text-transform:uppercase;color:var(--dim)}
.legend{display:flex;gap:13px;flex-wrap:wrap}
.li{display:flex;align-items:center;gap:5px;font-size:.66rem;color:var(--dim)}
.lb{width:16px;height:3px;border-radius:2px;display:inline-block}
.lb.dash{height:0;border-top:2px dashed}
canvas{display:block;width:100%!important;height:260px!important}

/* Events */
.evts{background:var(--card);border:1px solid var(--border);border-radius:var(--r);
  padding:16px;box-shadow:var(--sh)}
.evts-t{font-size:.7rem;font-weight:700;letter-spacing:1px;text-transform:uppercase;
  color:var(--dim);margin-bottom:10px}
.elist{display:flex;flex-direction:column;gap:5px;max-height:180px;overflow-y:auto}
.elist::-webkit-scrollbar{width:3px}
.elist::-webkit-scrollbar-thumb{background:var(--faint);border-radius:2px}
.ei{display:flex;gap:10px;align-items:flex-start;padding:8px 10px;border-radius:8px;
  background:var(--surface);border-left:3px solid var(--faint);font-size:.76rem;
  animation:drop .2s ease}
@keyframes drop{from{opacity:0;transform:translateY(-5px)}to{opacity:1;transform:none}}
.ei.phase{border-color:#38b2ff}.ei.calibration{border-color:#00e5c8}
.ei.anomaly{border-color:#ff4d6a}.ei.log{border-color:#3a6880}
.ets{color:var(--faint);font-size:.65rem;white-space:nowrap;padding-top:1px}
.em{flex:1;color:var(--txt);line-height:1.4}

#lost{display:none;position:fixed;top:0;left:0;right:0;z-index:99;padding:9px;
  text-align:center;background:var(--danger);color:#fff;font-weight:700;font-size:.83rem}
footer{text-align:center;font-size:.65rem;color:var(--faint);padding:4px}
#toasts{position:fixed;top:14px;right:14px;z-index:1000;display:flex;flex-direction:column;gap:8px;max-width:300px}
.toast{padding:11px 13px;border-radius:10px;border-left:4px solid;background:#1b2130;color:#e8edf6;
  box-shadow:0 6px 20px rgba(0,0,0,.45);font-size:13px;line-height:1.35;cursor:pointer;
  animation:tin .22s ease-out;display:flex;gap:9px;align-items:flex-start}
.toast .ti{font-size:16px;line-height:1.1}
.toast .tt{font-weight:700;display:block;margin-bottom:1px}
.toast.halt{border-left-color:#ff4d6a;background:#2a1a20}
.toast.warn{border-left-color:#f5a623;background:#2a2418}
.toast.fade{opacity:0;transform:translateX(12px);transition:all .35s ease}
@keyframes tin{from{opacity:0;transform:translateX(20px)}to{opacity:1;transform:translateX(0)}}
.eval-stats{font-size:12.5px;color:var(--muted,#9fb0c8);margin:8px 0;line-height:1.5}
.eval-tab{border-collapse:collapse;font-size:11px;width:100%;margin-top:4px}
.eval-tab th,.eval-tab td{border:1px solid #2a3346;padding:3px 5px;text-align:center}
.eval-tab th{color:#9fb0c8;font-weight:600}
.eval-tab td.diag{background:rgba(0,229,200,.14);font-weight:700}
.eval-tab td.hit{color:#e8edf6}
.eval-tab td.zero{color:#566}
.mbtn.act{outline:2px solid #00e5c8}
</style>
</head>
<body>
<div id="toasts"></div>
<div id="lost">⚠ Connessione persa — riprovo…</div>
<div class="app">

  <header>
    <div class="brand">
      <div class="bi">🌊</div>
      <div><div class="bn">FLOAT Aquarium</div><div class="bs">Monitor live — Observer</div></div>
    </div>
    <div class="hr">
      <div class="toggle-wrap">
        <span class="toggle-lbl">Default mode</span>
        <label class="switch">
          <input type="checkbox" id="tg-auto" checked onchange="setAutoMode(this.checked)">
          <span class="slider"></span>
        </label>
        <span id="tg-state" class="toggle-state on">ON</span>
      </div>
      <span id="phase" class="phase">IDLE</span>
      <div class="conn"><div id="dot" class="dot"></div><span id="conn">Connecting…</span></div>
    </div>
  </header>

  <div class="cards">
    <div class="card temp"><div class="cl">Temperature</div>
      <div id="v-temp" class="cv t-yel">—</div><div class="cu">°C</div></div>
    <div class="card cur"><div class="cl">Current</div>
      <div id="v-cur" class="cv t-blue">—</div><div class="cu">mA</div></div>
    <div class="card turb"><div class="cl">Turbidity</div>
      <div id="v-turb" class="cv t-pur">—</div><div class="cu">NTU</div></div>
  </div>

  <div id="manual" class="manual locked">
    <div class="manual-t">Manual controls</div>
    <div class="manual-sub">Available only while Default mode is OFF. All settings are temporary and are wiped when you switch back to ON.</div>

    <div class="mrow">
      <button id="btn-pump" class="mbtn" onclick="togglePump()">Pump: OFF</button>
      <div class="mfields">
        <input id="pump-val" type="number" min="0" placeholder="∞">
        <select id="pump-unit">
          <option value="1">sec</option>
          <option value="60">min</option>
          <option value="3600">hours</option>
        </select>
      </div>
      <span class="mhint">Duration the pump stays on. Leave empty = stays on indefinitely.</span>
    </div>

    <div class="mrow">
      <button id="btn-feed" class="mbtn" onclick="dispenseFood()">Dispense food</button>
      <div class="mfields">
        <input id="feed-val" type="number" min="0" placeholder="once">
        <select id="feed-unit">
          <option value="1">sec</option>
          <option value="60">min</option>
          <option value="3600">hours</option>
        </select>
      </div>
      <span class="mhint">Repeat interval for the servo. Leave empty = dispense once.</span>
    </div>

    <div class="mrow">
      <button id="btn-cal" class="mbtn warn" onclick="calibrate()">Calibrate</button>
      <span class="mhint">Learns the pump's normal current baseline (~10 s).</span>
      <span id="cal-warn" class="mwarn">⚠ Turn the pump ON first, then calibrate — otherwise the baseline is idle current and the pump will trip a false stall the moment it runs.</span>
    </div>
  </div>

  <!-- HALT recovery lives OUTSIDE the manual panel so it stays clickable even
       in automatic mode (where the manual panel is locked / non-interactive). -->
  <div id="halt-row" class="manual" style="display:none;margin-top:10px">
    <div class="mrow">
      <button id="btn-clrhalt" class="mbtn" style="border-color:var(--danger);color:var(--danger)" onclick="clearHalt()">Clear HALT</button>
      <span class="mhint">The pump was halted by a stall / dry-run anomaly. Clear to recover — in automatic mode this restarts a fresh cycle; in manual mode it returns to idle. Investigate the pump before running again.</span>
    </div>
  </div>

  <div class="chart-box">
    <div class="ch">
      <div class="ct">Current pump</div>
      <div class="legend">
        <div class="li"><span class="lb" style="background:#38b2ff"></span>Raw</div>
        <div class="li"><span class="lb" style="background:#00e5c8"></span>EWMA</div>
        <div class="li"><span class="lb dash" style="border-color:#ff4d6a"></span>Stall</div>
        <div class="li"><span class="lb dash" style="border-color:#f5c542"></span>Dry-run</div>
      </div>
    </div>
    <canvas id="cCur"></canvas>
  </div>

  <div class="evts">
    <div class="evts-t">Live events</div>
    <div id="elist" class="elist">
      <div class="ei log"><span class="ets">—</span><span class="em">Waiting for new events…</span></div>
    </div>
  </div>

  <div class="evts" id="evalpanel">
    <div class="evts-t">Valutazione — matrice di confusione</div>
    <div class="manual-sub">Imposta la verità del ciclo che stai per eseguire, poi fai girare cicli monitorati (più semplice in Default mode ON). Ogni ciclo registra un esito.</div>
    <div class="mrow" id="eval-btns"></div>
    <div id="eval-stats" class="eval-stats">Etichettatura disattivata — scegli una classe per iniziare.</div>
    <table id="eval-tab" class="eval-tab"></table>
    <div class="mrow" style="margin-top:8px">
      <button class="mbtn" onclick="evalOff()">Stop etichettatura</button>
      <button class="mbtn warn" onclick="evalReset()">Reset matrice</button>
    </div>
  </div>

  <footer>FLOAT Aquarium Control · polling live</footer>
</div>

<script>
const N=80;
let cCur=null, thStall=null, thDry=null, lastEvtId=0, failing=0, toastsArmed=false;
let togglePending=false;   // ignore /data sync while a toggle is in flight
let pumpOn=false;          // last known pump state from /data
let autoOn=true;           // last known default-mode state
let halted=false;          // last known halt state

function seq(){return Array(N).fill(null)}

function boot(){
  const ctx=document.getElementById('cCur').getContext('2d');
  cCur=new Chart(ctx,{
    type:'line',
    data:{labels:Array.from({length:N},(_,i)=>i),datasets:[
      {label:'Raw',data:seq(),borderColor:'#38b2ff',borderWidth:2,pointRadius:0,
       fill:true,backgroundColor:'rgba(56,178,255,.08)',tension:.3,spanGaps:true},
      {label:'EWMA',data:seq(),borderColor:'#00e5c8',borderWidth:2,pointRadius:0,
       fill:false,tension:.3,spanGaps:true},
      {label:'Stall',data:seq(),borderColor:'rgba(255,77,106,.7)',borderWidth:1.5,
       borderDash:[6,4],pointRadius:0,fill:false,spanGaps:true},
      {label:'Dry-run',data:seq(),borderColor:'rgba(245,197,66,.7)',borderWidth:1.5,
       borderDash:[6,4],pointRadius:0,fill:false,spanGaps:true}
    ]},
    options:{
      animation:false,responsive:true,maintainAspectRatio:false,
      plugins:{legend:{display:false},tooltip:{mode:'index',intersect:false}},
      scales:{
        x:{display:false},
        y:{beginAtZero:true,grid:{color:'rgba(56,178,255,.06)'},
           ticks:{color:'#5a8caa',font:{size:10}}}
      }
    }
  });
  startPolling();
}

function push(ds,v){
  cCur.data.datasets[ds].data.shift();
  cCur.data.datasets[ds].data.push(v);
}
function flat(ds,v){cCur.data.datasets[ds].data=Array(N).fill(v)}

async function pollData(){
  try{
    const d=await(await fetch('/data')).json();
    failing=0;
    set('v-temp', d.T>-50?d.T.toFixed(1):'—');
    set('v-cur',  d.I.toFixed(1));
    set('v-turb', d.turb>=0?d.turb.toFixed(0):'—');
    const p=document.getElementById('phase');
    const m=(d.locked||d.thalt)?'HALTED':d.mode;
    p.textContent=m; p.className='phase '+m;
    // Threshold dashed lines: draw while calibrated; clear them the moment the
    // observer reports no calibration (th=0), e.g. after a mode change.
    if(d.th_stall>0){ if(d.th_stall!==thStall){thStall=d.th_stall; flat(2,thStall);} }
    else if(thStall!==null){ thStall=null; flat(2,NaN); }
    if(d.th_dry>0){ if(d.th_dry!==thDry){thDry=d.th_dry; flat(3,thDry);} }
    else if(thDry!==null){ thDry=null; flat(3,NaN); }
    push(0,d.I); push(1,d.I_ewma);
    cCur.update('none');
    // Keep the default-mode toggle in sync with the observer's NVS state
    // (unless we're mid-toggle, to avoid bouncing while the request flies).
    if(!togglePending && typeof d.auto === 'boolean') syncToggle(d.auto);
    // Manual controls: enabled only when default mode is OFF.
    if(typeof d.auto === 'boolean') setManualEnabled(!d.auto);
    if(typeof d.pump === 'boolean') syncPump(d.pump);
    syncHalt(!!d.locked || !!d.thalt, !d.auto);
    setConn(true);
  }catch(e){ if(++failing>3) setConn(false); }
}

// User clicked the toggle in the UI.
async function setAutoMode(on){
  togglePending = true;
  syncToggle(on);   // optimistic UI update
  const tg = document.getElementById('tg-auto'); tg.disabled = true;
  try{
    await fetch('/mode?set=' + (on?'on':'off'));
  }catch(_){ /* if the call fails, /data will eventually re-sync */ }
  tg.disabled = false;
  // Give the observer a moment to settle before re-syncing from /data
  setTimeout(()=>{ togglePending = false; }, 1500);
}

// Reflect a boolean auto-mode state into the UI without firing onchange.
function syncToggle(on){
  const tg = document.getElementById('tg-auto');
  if (tg.checked !== on) tg.checked = on;
  const st = document.getElementById('tg-state');
  st.textContent = on ? 'ON' : 'OFF';
  st.className = 'toggle-state ' + (on ? 'on' : 'off');
}

// Enable/disable the whole manual panel based on default mode.
function setManualEnabled(enabled){
  autoOn = !enabled;
  const panel = document.getElementById('manual');
  panel.className = 'manual' + (enabled ? '' : ' locked');
  ['btn-pump','btn-feed','btn-cal','pump-val','pump-unit','feed-val','feed-unit']
    .forEach(id=>{ const e=document.getElementById(id); if(e) e.disabled = !enabled; });
}

// Reflect pump state on the toggle-pump button.
function syncPump(on){
  pumpOn = on;
  const b = document.getElementById('btn-pump');
  b.textContent = 'Pump: ' + (on ? 'ON' : 'OFF');
  b.className = 'mbtn' + (on ? ' on' : '');
  // Show the calibrate warning only when about to calibrate with pump off.
  document.getElementById('cal-warn').className = 'mwarn' + (on ? '' : ' show');
}

// Read a value+unit field, return seconds (0 if empty / invalid).
function readSeconds(valId, unitId){
  const v = parseFloat(document.getElementById(valId).value);
  if (!isFinite(v) || v <= 0) return 0;
  const mult = parseInt(document.getElementById(unitId).value, 10) || 1;
  return Math.round(v * mult);
}

async function sendCmd(qs){
  try{ await fetch('/cmd?' + qs); }catch(_){ /* /data will re-sync */ }
}

function togglePump(){
  if (autoOn) return;
  if (pumpOn){
    syncPump(false);              // optimistic
    sendCmd('a=pump_off');
  } else {
    const sec = readSeconds('pump-val','pump-unit');
    syncPump(true);               // optimistic
    sendCmd('a=pump_on&sec=' + sec);
  }
}

function dispenseFood(){
  if (autoOn) return;
  const sec = readSeconds('feed-val','feed-unit');
  sendCmd('a=feed&sec=' + sec);
}

function calibrate(){
  if (autoOn) return;
  sendCmd('a=calibrate');
}

// Reflect halt state: in OFF mode, a halt shows the Clear HALT row and locks
// the pump/feed/calibrate buttons until the user clears it.
function syncHalt(isHalted, manualMode){
  halted = isHalted;
  const row = document.getElementById('halt-row');
  // Show the Clear HALT panel whenever halted, in BOTH automatic and manual
  // mode (it lives outside the manual panel so it stays clickable when auto).
  row.style.display = isHalted ? 'block' : 'none';
  // While halted in manual mode, disable the normal controls (only Clear HALT).
  if (manualMode){
    const dis = isHalted;
    ['btn-pump','btn-feed','btn-cal','pump-val','pump-unit','feed-val','feed-unit']
      .forEach(id=>{ const e=document.getElementById(id); if(e) e.disabled = dis; });
  }
}

function clearHalt(){
  // Allowed in both modes: in auto it restarts a fresh cycle, in manual it
  // returns to idle. The observer accepts clear_halt regardless of mode.
  sendCmd('a=clear_halt');
}

async function pollEvents(){
  try{
    const arr=await(await fetch('/events?last='+lastEvtId)).json();
    for(const e of arr){ if(e.id>lastEvtId){lastEvtId=e.id; addEvt(e);
      if(toastsArmed && (e.type==='halt'||e.type==='warn')) showToast(e.type, e.msg); } }
    toastsArmed=true;   // only toast events that arrive AFTER the first load
  }catch(_){}
}

// Top-right pop-up notification. Red ('halt') for anomalies that stop the pump
// (motor stall, dry-run); orange ('warn') for advisory ones (voltage, temp).
function showToast(type, reason){
  const labels={
    MOTOR_STALL:'Stallo motore — sistema in HALT',
    DRY_RUN:'Funzionamento a secco — sistema in HALT',
    VOLTAGE_DROP:'Tensione di alimentazione bassa',
    TEMP_TOO_HIGH:'Temperatura troppo alta',
    TEMP_TOO_LOW:'Temperatura troppo bassa',
    DEGRADATION:'Degrado pompa — manutenzione consigliata'};
  const title = type==='halt' ? 'HALT' : 'Attenzione';
  const icon  = type==='halt' ? '⛔' : '⚠';
  const msg   = labels[reason] || reason;
  const c=document.getElementById('toasts');
  const t=document.createElement('div');
  t.className='toast '+type;
  t.innerHTML='<span class="ti">'+icon+'</span><span><span class="tt">'+title+
              '</span>'+esc(msg)+'</span>';
  const kill=()=>{ t.classList.add('fade'); setTimeout(()=>t.remove(),350); };
  t.onclick=kill;
  c.appendChild(t);
  setTimeout(kill, type==='halt'?12000:7000);
  while(c.children.length>4) c.removeChild(c.firstChild);
}

function startPolling(){
  buildEvalBtns(); pollData(); pollEvents(); pollEval();
  setInterval(pollData,  600);   // was 400 — reduces concurrent load on observer
  setInterval(pollEvents,1500);  // was 700 — events are rarer, can poll slower
  setInterval(pollEval,  1500);
}

const EVAL_LABELS=['Normale','Stallo','Dry-run','Tensione','Temp alta','Temp bassa'];
const EVAL_SHORT =['NOR','STA','DRY','VLT','T+','T-'];
let evalTruth=-1;
function buildEvalBtns(){
  const c=document.getElementById('eval-btns');
  c.innerHTML='';
  EVAL_LABELS.forEach((lab,k)=>{
    const b=document.createElement('button');
    b.className='mbtn'; b.id='ev-'+k; b.textContent=lab;
    b.onclick=()=>setTruth(k);
    c.appendChild(b);
  });
}
async function setTruth(k){ evalTruth=k; await sendCmd0('/eval?truth='+k); markTruth(); pollEval(); }
async function evalOff(){ evalTruth=-1; await sendCmd0('/eval?off=1'); markTruth(); pollEval(); }
async function evalReset(){ await sendCmd0('/eval?reset=1'); pollEval(); }
async function sendCmd0(u){ try{ await fetch(u); }catch(_){} }
function markTruth(){
  for(let k=0;k<6;k++){ const b=document.getElementById('ev-'+k); if(b) b.className='mbtn'+(k===evalTruth?' act':''); }
}
async function pollEval(){
  try{
    const d=await(await fetch('/eval')).json();
    evalTruth=d.truth; markTruth(); renderEval(d);
  }catch(_){}
}
function renderEval(d){
  const m=d.m, T=6;
  let tot=0,diag=0,fpRow=0,fpErr=0,fnRow=0,fnMiss=0;
  for(let i=0;i<T;i++)for(let j=0;j<T;j++){
    const v=m[i*T+j]; tot+=v; if(i===j)diag+=v;
    if(i===0){ if(j!==0)fpErr+=v; fpRow+=v; }       // truth NORMAL → false positives
    else { fnRow+=v; if(j===0)fnMiss+=v; }           // truth fault → missed (FN)
  }
  const acc=tot?(100*diag/tot):0, fpr=fpRow?(100*fpErr/fpRow):0, fnr=fnRow?(100*fnMiss/fnRow):0;
  const st=document.getElementById('eval-stats');
  const cur=(d.truth>=0)?('Verità attiva: <b>'+EVAL_LABELS[d.truth]+'</b>'):'Etichettatura disattivata';
  st.innerHTML=cur+' · campioni: <b>'+d.count+'</b> · accuratezza: <b>'+acc.toFixed(0)+
    '%</b> · falsi positivi: <b>'+fpr.toFixed(0)+'%</b> · falsi negativi: <b>'+fnr.toFixed(0)+
    '%</b> · latenza media: <b>'+d.lat+' ms</b>';
  let h='<tr><th>↓v r→</th>';
  for(let j=0;j<T;j++) h+='<th>'+EVAL_SHORT[j]+'</th>';
  h+='</tr>';
  for(let i=0;i<T;i++){
    h+='<tr><th>'+EVAL_SHORT[i]+'</th>';
    for(let j=0;j<T;j++){ const v=m[i*T+j];
      const cls=(i===j)?'diag':(v>0?'hit':'zero');
      h+='<td class="'+cls+'">'+v+'</td>';
    }
    h+='</tr>';
  }
  document.getElementById('eval-tab').innerHTML=h;
}

function set(id,v){const e=document.getElementById(id); if(e)e.textContent=v}
function setConn(ok){
  document.getElementById('dot').className='dot '+(ok?'on':'err');
  document.getElementById('conn').textContent=ok?'Live':'No signal';
  document.getElementById('lost').style.display=ok?'none':'block';
}
function addEvt(e){
  const list=document.getElementById('elist');
  if(list.children.length===1 && list.children[0].querySelector('.ets')?.textContent==='—')
    list.innerHTML='';
  const d=document.createElement('div');
  d.className='ei '+(e.type||'log');
  const s=Math.floor((Date.now()-e.ts)/1000);
  const ago=s<2?'ora':s<60?s+'s fa':Math.floor(s/60)+'m fa';
  d.innerHTML='<span class="ets">'+ago+'</span><span class="em">'+esc(e.msg)+'</span>';
  list.prepend(d);
  while(list.children.length>25) list.removeChild(list.lastChild);
}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}

// Chart.js loaded via <script> tag in <head>; start once DOM+lib are ready
if(window.Chart) boot();
else window.addEventListener('load',()=>{ if(window.Chart) boot(); else setConn(false); });
</script>
</body>
</html>
)rawliteral";

#endif