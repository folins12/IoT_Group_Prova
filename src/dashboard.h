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
</style>
</head>
<body>
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

  <footer>FLOAT Aquarium Control · polling live</footer>
</div>

<script>
const N=80;
let cCur=null, thStall=null, thDry=null, lastEvtId=0, failing=0;
let togglePending=false;   // ignore /data sync while a toggle is in flight

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
    const m=d.locked?'HALTED':d.mode;
    p.textContent=m; p.className='phase '+m;
    if(d.th_stall>0 && d.th_stall!==thStall){thStall=d.th_stall;flat(2,thStall)}
    if(d.th_dry>0   && d.th_dry!==thDry)   {thDry=d.th_dry;     flat(3,thDry)}
    push(0,d.I); push(1,d.I_ewma);
    cCur.update('none');
    // Keep the default-mode toggle in sync with the observer's NVS state
    // (unless we're mid-toggle, to avoid bouncing while the request flies).
    if(!togglePending && typeof d.auto === 'boolean') syncToggle(d.auto);
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

async function pollEvents(){
  try{
    const arr=await(await fetch('/events?last='+lastEvtId)).json();
    for(const e of arr){ if(e.id>lastEvtId){lastEvtId=e.id; addEvt(e);} }
  }catch(_){}
}

function startPolling(){
  pollData(); pollEvents();
  setInterval(pollData,  600);   // was 400 — reduces concurrent load on observer
  setInterval(pollEvents,1500);  // was 700 — events are rarer, can poll slower
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