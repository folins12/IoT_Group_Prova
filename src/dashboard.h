#ifndef DASHBOARD_H
#define DASHBOARD_H

// The dashboard is served by main.cpp over HTTP on port 80.
// It uses Server-Sent Events (/api/events) for push notifications —
// the browser never needs to poll; the server pushes every 400 ms.
// An alert log panel records every anomaly event with timestamp.
// A browser Notification API request is made on first load so the
// user can receive OS-level push alerts even when the tab is in the background.

const char* html_page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FLOAT System | Aquarium Edge Control</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;600&display=swap" rel="stylesheet">
  <style>
    :root {
      --c-bg:       #06090f;
      --c-panel:    rgba(10, 16, 28, 0.75);
      --c-border:   rgba(56, 189, 248, 0.15);
      --c-cyan:     #22d3ee;
      --c-orange:   #fb923c;
      --c-purple:   #a78bfa;
      --c-green:    #4ade80;
      --c-red:      #f87171;
      --c-yellow:   #fbbf24;
      --c-text:     #cbd5e1;
      --c-muted:    #475569;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--c-bg);
      font-family: 'DM Sans', sans-serif;
      color: var(--c-text);
      min-height: 100vh;
      overflow-x: hidden;
    }
    /* Animated deep-sea gradient background */
    body::before {
      content: '';
      position: fixed; inset: 0; z-index: 0;
      background:
        radial-gradient(ellipse 80% 60% at 20% 30%, rgba(6,78,112,0.25) 0%, transparent 70%),
        radial-gradient(ellipse 60% 50% at 80% 70%, rgba(30,58,138,0.20) 0%, transparent 70%);
      animation: drift 20s ease-in-out infinite alternate;
    }
    @keyframes drift {
      from { opacity: 0.6; }
      to   { opacity: 1.0; }
    }
    /* Video background */
    #bg-video {
      position: fixed; top: 0; left: 0;
      width: 100vw; height: 100vh;
      object-fit: cover; z-index: -1;
      filter: brightness(0.25) saturate(1.4) hue-rotate(10deg);
    }
    /* Glass panels */
    .panel {
      position: relative; z-index: 1;
      background: var(--c-panel);
      backdrop-filter: blur(14px);
      -webkit-backdrop-filter: blur(14px);
      border: 1px solid var(--c-border);
      border-radius: 1rem;
    }
    /* Mono headings */
    .mono { font-family: 'Space Mono', monospace; }
    /* Neon glow utilities */
    .glow-cyan   { text-shadow: 0 0 12px rgba(34,211,238,0.7);  color: var(--c-cyan); }
    .glow-orange { text-shadow: 0 0 12px rgba(251,146,60,0.7);  color: var(--c-orange); }
    .glow-purple { text-shadow: 0 0 12px rgba(167,139,250,0.7); color: var(--c-purple); }
    .glow-green  { text-shadow: 0 0 12px rgba(74,222,128,0.7);  color: var(--c-green); }
    .glow-red    { text-shadow: 0 0 12px rgba(248,113,113,0.7); color: var(--c-red); }
    /* Status badge */
    .badge {
      display: inline-block;
      padding: 3px 14px; border-radius: 99px;
      font-family: 'Space Mono', monospace;
      font-size: 0.7rem; font-weight: 700;
      letter-spacing: 0.08em;
      border: 1px solid;
    }
    .badge-idle       { background: rgba(71,85,105,0.3); color:#94a3b8;   border-color:#475569; }
    .badge-learning   { background: rgba(245,158,11,0.2); color:#fbbf24;  border-color:#f59e0b; animation: pulse 1.2s ease-in-out infinite; }
    .badge-monitoring { background: rgba(74,222,128,0.2); color:#4ade80;  border-color:#22c55e; }
    .badge-halted     { background: rgba(239,68,68,0.25); color:#f87171;  border-color:#ef4444; box-shadow: 0 0 12px rgba(239,68,68,0.4); }
    @keyframes pulse { 0%,100% { opacity:1; } 50% { opacity:0.5; } }
    /* Alert banner */
    #alert-banner {
      position: relative; z-index: 10;
      background: rgba(127,29,29,0.7);
      border: 1px solid #ef4444;
      border-radius: 0.75rem;
      box-shadow: 0 0 30px rgba(239,68,68,0.4);
    }
    /* Buttons */
    .btn {
      width: 100%; padding: 10px 0;
      border-radius: 0.65rem;
      font-family: 'Space Mono', monospace;
      font-size: 0.75rem; font-weight: 700;
      letter-spacing: 0.05em;
      cursor: pointer; border: 1px solid;
      transition: all 0.15s ease;
    }
    .btn:active { transform: scale(0.97); }
    .btn-amber  { background: rgba(217,119,6,0.25); color:#fbbf24;  border-color: rgba(217,119,6,0.6); }
    .btn-amber:hover  { background: rgba(217,119,6,0.5); box-shadow: 0 0 12px rgba(245,158,11,0.4); }
    .btn-purple { background: rgba(139,92,246,0.2);  color:#a78bfa; border-color: rgba(139,92,246,0.5); }
    .btn-purple:hover { background: rgba(139,92,246,0.4); box-shadow: 0 0 12px rgba(139,92,246,0.4); }
    .btn-blue   { background: rgba(59,130,246,0.2);  color:#93c5fd; border-color: rgba(59,130,246,0.4); }
    .btn-blue:hover   { background: rgba(59,130,246,0.4); box-shadow: 0 0 12px rgba(59,130,246,0.4); }
    .btn-green  { background: rgba(34,197,94,0.2);   color:#86efac; border-color: rgba(34,197,94,0.4); }
    .btn-green:hover  { background: rgba(34,197,94,0.4); box-shadow: 0 0 12px rgba(34,197,94,0.4); }
    .btn-red    { background: rgba(239,68,68,0.3);   color:#fca5a5; border-color: rgba(239,68,68,0.5); }
    .btn-red:hover    { background: rgba(239,68,68,0.5); box-shadow: 0 0 12px rgba(239,68,68,0.4); }
    /* Metric card */
    .metric-val { font-family: 'Space Mono', monospace; font-size: 1.75rem; font-weight: 700; }
    .metric-lbl { font-size: 0.65rem; letter-spacing: 0.1em; text-transform: uppercase; color: var(--c-muted); }
    /* Alert log */
    #alert-log { max-height: 180px; overflow-y: auto; }
    #alert-log::-webkit-scrollbar { width: 4px; }
    #alert-log::-webkit-scrollbar-track { background: transparent; }
    #alert-log::-webkit-scrollbar-thumb { background: rgba(56,189,248,0.3); border-radius: 2px; }
    .log-entry {
      display: flex; gap: 8px; align-items: flex-start;
      padding: 5px 0; border-bottom: 1px solid rgba(255,255,255,0.05);
      font-family: 'Space Mono', monospace; font-size: 0.65rem;
    }
    .log-entry:last-child { border-bottom: none; }
    /* Divider */
    .divider { height: 1px; background: var(--c-border); margin: 8px 0; }
    /* Threshold line legend */
    .th-dot { width: 10px; height: 3px; border-radius: 1px; display: inline-block; }
  </style>
</head>
<body class="p-3 md:p-6">

  <video autoplay loop muted playsinline id="bg-video">
    <source src="https://files.catbox.moe/ycet3b.mp4" type="video/mp4">
  </video>

  <div class="max-w-7xl mx-auto space-y-4">

    <!-- ── Header ────────────────────────────────────────────────────────── -->
    <div class="panel p-4 flex flex-col sm:flex-row justify-between items-center gap-3">
      <div class="flex items-center gap-3">
        <div class="w-10 h-10 rounded-full flex items-center justify-center"
             style="background:rgba(34,211,238,0.1);border:1px solid rgba(34,211,238,0.4);box-shadow:0 0 16px rgba(34,211,238,0.3)">
          <!-- wave icon -->
          <svg class="w-5 h-5" style="color:var(--c-cyan)" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M3 12c1.5-3 3-3 4.5 0s3 3 4.5 0 3-3 4.5 0 3 3 4.5 0"/>
          </svg>
        </div>
        <div>
          <h1 class="mono text-xl font-bold text-white">FLOAT <span class="glow-cyan">SYSTEM</span></h1>
          <p class="text-xs" style="color:var(--c-muted)">Autonomous Aquarium Edge-Control · v2</p>
        </div>
      </div>
      <div class="flex gap-2 items-center flex-wrap justify-center">
        <span id="auto-badge" class="badge badge-idle" style="display:none">AUTO ACTIVE</span>
        <span id="notif-badge" class="badge" style="background:rgba(251,146,60,0.15);color:#fb923c;border-color:#c2410c;cursor:pointer"
              onclick="requestNotifPermission()">🔔 ENABLE ALERTS</span>
        <span id="status-badge" class="badge badge-idle">IDLE</span>
      </div>
    </div>

    <!-- ── Alert banner (shown only when HALTED) ─────────────────────────── -->
    <div id="alert-banner" style="display:none" class="p-3 flex justify-between items-center gap-3">
      <div class="flex items-center gap-2">
        <svg class="w-5 h-5 shrink-0" style="color:var(--c-red)" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
            d="M12 9v2m0 4h.01M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/>
        </svg>
        <span class="mono font-bold text-sm" style="color:var(--c-red)">CRITICAL HALT — </span>
        <span id="alert-text" class="mono text-sm" style="color:#fca5a5">MOTOR STALL</span>
      </div>
      <button onclick="apiCall('/api/reset')" class="btn btn-red" style="width:auto;padding:6px 18px">RESET</button>
    </div>

    <!-- ── Main grid ─────────────────────────────────────────────────────── -->
    <div class="grid grid-cols-1 lg:grid-cols-4 gap-4">

      <!-- Left column: controls + alert log -->
      <div class="panel p-5 space-y-4 flex flex-col lg:col-span-1">

        <div>
          <p class="mono text-xs font-bold tracking-widest" style="color:var(--c-muted)">COMMAND CENTER</p>
          <div class="divider"></div>
        </div>

        <div class="space-y-2">
          <button onclick="apiCall('/api/learn')" class="btn btn-amber">① CALIBRATE (LEARN)</button>
          <button onclick="apiCall('/api/auto')"  id="btn-auto" class="btn btn-purple">② TOGGLE AUTO MODE</button>
        </div>

        <div class="divider"></div>
        <p class="mono text-xs text-center tracking-widest" style="color:var(--c-muted)">MANUAL OVERRIDE</p>

        <div class="space-y-2">
          <button onclick="apiCall('/api/pump')"  id="btn-pump"  class="btn btn-blue">TOGGLE PUMP</button>
          <button onclick="apiCall('/api/servo')" id="btn-servo" class="btn btn-green">DISPENSE FOOD</button>
        </div>

        <!-- Alert log -->
        <div style="flex:1">
          <div class="divider"></div>
          <div class="flex justify-between items-center mb-2">
            <p class="mono text-xs font-bold tracking-widest" style="color:var(--c-muted)">ALERT LOG</p>
            <button onclick="clearLog()" class="mono text-xs" style="color:var(--c-muted)">CLR</button>
          </div>
          <div id="alert-log">
            <p class="mono text-xs" style="color:var(--c-muted)">No events.</p>
          </div>
        </div>
      </div>

      <!-- Right: metrics + chart -->
      <div class="lg:col-span-3 space-y-4">

        <!-- Sensor metric row — 5 cards -->
        <div class="grid grid-cols-2 sm:grid-cols-5 gap-3">

          <div class="panel p-4 flex flex-col items-center justify-center">
            <span class="metric-lbl">Turbidity</span>
            <span id="val-turb" class="metric-val glow-cyan mt-1">—</span>
            <span class="metric-lbl">NTU</span>
          </div>

          <div class="panel p-4 flex flex-col items-center justify-center">
            <span class="metric-lbl">Temperature</span>
            <span id="val-temp" class="metric-val glow-orange mt-1">—</span>
            <span class="metric-lbl">°C</span>
          </div>

          <div class="panel p-4 flex flex-col items-center justify-center">
            <span class="metric-lbl">Motor I</span>
            <span id="val-curr" class="metric-val glow-purple mt-1">—</span>
            <span class="metric-lbl">mA</span>
          </div>

          <div class="panel p-4 flex flex-col items-center justify-center">
            <span class="metric-lbl">Bus Voltage</span>
            <span id="val-volt" class="metric-val mt-1" style="color:var(--c-yellow);text-shadow:0 0 12px rgba(251,191,36,0.7)">—</span>
            <span class="metric-lbl">V</span>
          </div>

          <div class="panel p-4 flex flex-col items-center justify-center">
            <span class="metric-lbl">EWMA I</span>
            <span id="val-ewma" class="metric-val mt-1" style="color:#67e8f9;text-shadow:0 0 12px rgba(103,232,249,0.6)">—</span>
            <span class="metric-lbl">mA smooth</span>
          </div>

        </div>

        <!-- Live chart -->
        <div class="panel p-5" style="flex:1">
          <div class="flex justify-between items-center mb-3 flex-wrap gap-2">
            <p class="mono text-xs font-bold tracking-widest" style="color:var(--c-muted)">LIVE POWER SIGNATURE</p>
            <div class="flex gap-4 items-center">
              <span class="flex items-center gap-1 mono text-xs" style="color:var(--c-red)">
                <span class="th-dot" style="background:var(--c-red)"></span>
                Stall: <span id="val-stall">—</span>
              </span>
              <span class="flex items-center gap-1 mono text-xs" style="color:var(--c-yellow)">
                <span class="th-dot" style="background:var(--c-yellow)"></span>
                Dry: <span id="val-dry">—</span>
              </span>
              <span class="flex items-center gap-1 mono text-xs" style="color:#67e8f9">
                <span class="th-dot" style="background:#67e8f9"></span>
                EWMA
              </span>
            </div>
          </div>
          <div style="position:relative;height:220px">
            <canvas id="powerChart"></canvas>
          </div>
        </div>

        <!-- Anomaly algorithm info -->
        <div class="panel p-4 grid grid-cols-1 sm:grid-cols-3 gap-4 text-xs">
          <div>
            <p class="mono font-bold tracking-widest mb-1" style="color:var(--c-muted)">ALGORITHM</p>
            <p>EWMA + Z-score sliding window</p>
            <p style="color:var(--c-muted)">α = 0.2 · confirmed at 3 ticks</p>
          </div>
          <div>
            <p class="mono font-bold tracking-widest mb-1" style="color:var(--c-muted)">ANOMALY TYPES</p>
            <p><span style="color:var(--c-red)">●</span> Motor stall  (I_ewma &gt; μ+3σ)</p>
            <p><span style="color:var(--c-yellow)">●</span> Dry run     (I_ewma &lt; μ−3σ)</p>
            <p><span style="color:var(--c-orange)">●</span> Voltage drop (&lt;90 % V_base)</p>
          </div>
          <div>
            <p class="mono font-bold tracking-widest mb-1" style="color:var(--c-muted)">REACTION TIME</p>
            <p>3 × 400 ms = <span class="glow-cyan">1 200 ms</span> max</p>
            <p style="color:var(--c-muted)">Req: &lt; 2 000 ms ✓</p>
          </div>
        </div>

      </div>
    </div>
  </div><!-- /max-w -->

  <script>
  // ── Chart setup ────────────────────────────────────────────────────────
  const POINTS = 60;
  const emptyArr = () => Array(POINTS).fill(null);

  const ctx   = document.getElementById('powerChart').getContext('2d');
  const chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: emptyArr(),
      datasets: [
        {
          label: 'Current (mA)',
          borderColor: '#a78bfa', backgroundColor: 'rgba(167,139,250,0.08)',
          borderWidth: 2, pointRadius: 0, fill: true, tension: 0.35,
          data: emptyArr()
        },
        {
          label: 'EWMA (mA)',
          borderColor: '#67e8f9',
          borderWidth: 1.5, pointRadius: 0, fill: false, tension: 0.35,
          data: emptyArr()
        },
        {
          label: 'Stall threshold',
          borderColor: '#ef4444', borderWidth: 1.5,
          borderDash: [5,4], pointRadius: 0, fill: false,
          data: emptyArr()
        },
        {
          label: 'Dry-run threshold',
          borderColor: '#fbbf24', borderWidth: 1.5,
          borderDash: [5,4], pointRadius: 0, fill: false,
          data: emptyArr()
        }
      ]
    },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      scales: {
        y: { beginAtZero: true, grid: { color: 'rgba(255,255,255,0.04)' }, ticks: { color: '#64748b', font: { family: 'Space Mono', size: 10 } } },
        x: { display: false }
      },
      plugins: { legend: { display: false } }
    }
  });

  // ── Alert log ──────────────────────────────────────────────────────────
  const logEntries = [];
  function addLog(type, msg) {
    const ts = new Date().toLocaleTimeString();
    logEntries.unshift({ ts, type, msg });
    if (logEntries.length > 50) logEntries.pop();
    renderLog();
  }
  function renderLog() {
    const el = document.getElementById('alert-log');
    if (!logEntries.length) { el.innerHTML = '<p class="mono text-xs" style="color:var(--c-muted)">No events.</p>'; return; }
    el.innerHTML = logEntries.map(e => {
      const col = e.type === 'HALT' ? 'var(--c-red)' :
                  e.type === 'WARN' ? 'var(--c-yellow)' : 'var(--c-muted)';
      return `<div class="log-entry"><span style="color:var(--c-muted)">${e.ts}</span><span style="color:${col}">[${e.type}]</span><span>${e.msg}</span></div>`;
    }).join('');
  }
  function clearLog() { logEntries.length = 0; renderLog(); }

  // ── Browser push notifications ─────────────────────────────────────────
  let notifGranted = Notification.permission === 'granted';
  function requestNotifPermission() {
    if (!('Notification' in window)) return;
    Notification.requestPermission().then(p => {
      notifGranted = (p === 'granted');
      if (notifGranted) {
        document.getElementById('notif-badge').textContent = '🔔 ALERTS ON';
        document.getElementById('notif-badge').style.color = 'var(--c-green)';
        document.getElementById('notif-badge').style.borderColor = 'var(--c-green)';
      }
    });
  }
  function sendNotification(title, body) {
    if (notifGranted) new Notification(title, { body, icon: '/favicon.ico' });
  }
  // Auto-request on load (modern browsers require user gesture — the badge covers that)

  // ── SSE  — Server-Sent Events for push data ────────────────────────────
  // The server pushes a JSON event every ~400 ms.
  // This eliminates polling and enables instant alert delivery.
  let lastStatus = '';
  const evtSource = new EventSource('/api/events');

  evtSource.onmessage = function(e) {
    let d;
    try { d = JSON.parse(e.data); } catch(_) { return; }

    // ── Metrics ──
    if (d.current   != null) document.getElementById('val-curr').textContent = d.current.toFixed(1);
    if (d.ewma      != null) document.getElementById('val-ewma').textContent = d.ewma.toFixed(1);
    if (d.voltage   != null) document.getElementById('val-volt').textContent = d.voltage.toFixed(2);
    if (d.temp      != null) document.getElementById('val-temp').textContent = d.temp.toFixed(1);
    if (d.turbidity != null) document.getElementById('val-turb').textContent = Number(d.turbidity).toFixed(0);
    if (d.th_stall  != null) document.getElementById('val-stall').textContent = d.th_stall > 0 ? d.th_stall.toFixed(1) : '—';
    if (d.th_dry    != null) document.getElementById('val-dry').textContent   = d.th_dry   > 0 ? d.th_dry.toFixed(1)   : '—';

    // ── Chart ──
    chart.data.datasets[0].data.push(d.current  ?? null); chart.data.datasets[0].data.shift();
    chart.data.datasets[1].data.push(d.ewma      ?? null); chart.data.datasets[1].data.shift();
    chart.data.datasets[2].data.push(d.th_stall  > 0 ? d.th_stall : null); chart.data.datasets[2].data.shift();
    chart.data.datasets[3].data.push(d.th_dry    > 0 ? d.th_dry   : null); chart.data.datasets[3].data.shift();
    chart.update();

    // ── Status badge ──
    const badge = document.getElementById('status-badge');
    const s = d.status || 'IDLE';
    badge.textContent = s;
    badge.className = 'badge badge-' + s.toLowerCase();

    // ── Auto-mode badge ──
    const autoBadge = document.getElementById('auto-badge');
    const btnAuto   = document.getElementById('btn-auto');
    autoBadge.style.display = d.auto_mode ? 'inline-block' : 'none';
    autoBadge.className = d.auto_mode ? 'badge badge-monitoring' : 'badge badge-idle';
    btnAuto.textContent = d.auto_mode ? '② STOP AUTO MODE' : '② TOGGLE AUTO MODE';
    btnAuto.className   = d.auto_mode ? 'btn btn-red' : 'btn btn-purple';

    // ── Alert banner & push notification on status change ──
    const alertBanner = document.getElementById('alert-banner');
    if (s === 'HALTED') {
      alertBanner.style.display = 'flex';
      document.getElementById('alert-text').textContent = d.anomaly || 'UNKNOWN';
      if (lastStatus !== 'HALTED') {
        const msg = 'Anomaly: ' + (d.anomaly || 'UNKNOWN');
        addLog('HALT', msg);
        sendNotification('⚠ FLOAT SYSTEM HALT', msg);
      }
    } else {
      alertBanner.style.display = 'none';
    }

    // Log mode transitions
    if (s !== lastStatus) {
      if (s === 'LEARNING')   addLog('INFO', 'Calibration started');
      if (s === 'MONITORING') addLog('INFO', 'Monitoring active');
      if (s === 'IDLE' && lastStatus === 'LEARNING') addLog('INFO', 'Calibration complete');
      lastStatus = s;
    }

    // ── Pump button ──
    const btnPump = document.getElementById('btn-pump');
    if (d.pump) {
      btnPump.textContent = 'TURN PUMP OFF';
      btnPump.className = 'btn btn-red';
    } else {
      btnPump.textContent = 'TURN PUMP ON';
      btnPump.className = 'btn btn-blue';
    }
  };

  evtSource.onerror = function() {
    addLog('WARN', 'SSE stream lost — reconnecting...');
  };

  async function apiCall(endpoint) {
    try { await fetch(endpoint, { method: 'POST' }); }
    catch(e) { addLog('WARN', 'API error: ' + endpoint); }
  }
  </script>
</body>
</html>
)rawliteral";

#endif