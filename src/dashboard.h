#ifndef DASHBOARD_H
#define DASHBOARD_H

const char* html_page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-width=1.0">
    <title>FLOAT System | Aquarium Control</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { 
            margin: 0; 
            font-family: 'Inter', sans-serif; 
            color: #e2e8f0; 
            overflow-x: hidden;
        }
        /* Video di Sfondo */
        #bg-video {
            position: fixed;
            top: 0;
            left: 0;
            width: 100vw;
            height: 100vh;
            object-fit: cover;
            z-index: -1;
            filter: brightness(0.4) saturate(1.2);
        }
        /* Effetto Vetro Smerigliato (Glassmorphism) */
        .glass-panel { 
            background: rgba(15, 23, 42, 0.6); 
            backdrop-filter: blur(12px); 
            -webkit-backdrop-filter: blur(12px);
            border: 1px solid rgba(255, 255, 255, 0.1); 
            border-radius: 1.25rem; 
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
        }
        /* Testo Neon */
        .neon-text {
            color: #22d3ee;
            text-shadow: 0 0 10px rgba(34, 211, 238, 0.6);
        }
    </style>
</head>
<body class="p-4 md:p-8">
    
    <video autoplay loop muted playsinline id="bg-video">
        <source src="https://files.catbox.moe/ycet3b.mp4" type="video/mp4">
    </video>

    <div class="max-w-7xl mx-auto space-y-6">
        
        <div class="flex flex-col md:flex-row justify-between items-center glass-panel p-6">
            <div class="flex items-center gap-4">
                <div class="w-12 h-12 rounded-full bg-cyan-500/20 flex items-center justify-center border border-cyan-400 shadow-[0_0_15px_rgba(34,211,238,0.5)]">
                    <svg class="w-6 h-6 text-cyan-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 10V3L4 14h7v7l9-11h-7z"></path></svg>
                </div>
                <div>
                    <h1 class="text-3xl font-bold text-white tracking-wider">FLOAT <span class="neon-text">SYSTEM</span></h1>
                    <p class="text-cyan-200/70 text-sm mt-1">Autonomous Aquarium Edge-Control</p>
                </div>
            </div>
            <div class="mt-4 md:mt-0 flex gap-4 items-center">
                <div id="auto-badge" class="hidden px-4 py-2 rounded-full font-bold text-sm bg-purple-500/20 text-purple-400 border border-purple-500/50 shadow-[0_0_10px_rgba(168,85,247,0.4)]">
                    AUTO MODE ACTIVE
                </div>
                <div id="status-badge" class="px-4 py-2 rounded-full font-bold text-sm bg-slate-500/20 text-slate-400 border border-slate-500/50">
                    IDLE
                </div>
            </div>
        </div>

        <div id="alert-banner" class="hidden glass-panel !bg-red-900/50 !border-red-500 p-4 text-red-200 font-bold flex justify-between items-center shadow-[0_0_20px_rgba(239,68,68,0.5)]">
            <div class="flex items-center gap-3">
                <svg class="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"></path></svg>
                <span id="alert-text">CRITICAL HALT: MOTOR STALL</span>
            </div>
            <button onclick="apiCall('/api/reset')" class="px-6 py-2 bg-red-600 hover:bg-red-500 text-white rounded-lg transition-all shadow-lg">RESET HARDWARE</button>
        </div>

        <div class="grid grid-cols-1 lg:grid-cols-4 gap-6">
            
            <div class="glass-panel p-6 space-y-6 lg:col-span-1 flex flex-col">
                <h2 class="text-lg font-semibold text-cyan-100 border-b border-white/10 pb-2 uppercase tracking-widest">Command Center</h2>
                
                <div class="space-y-3 flex-grow">
                    <button onclick="apiCall('/api/learn')" id="btn-learn" class="w-full py-3 rounded-xl font-bold transition-all bg-amber-600/80 hover:bg-amber-500 border border-amber-400/50 shadow-[0_0_15px_rgba(217,119,6,0.4)] text-white">
                        1. CALIBRATE (LEARN)
                    </button>
                    
                    <button onclick="apiCall('/api/auto')" id="btn-auto" class="w-full py-3 rounded-xl font-bold transition-all bg-purple-600/80 hover:bg-purple-500 border border-purple-400/50 text-white mt-4">
                        2. TOGGLE AUTO MODE
                    </button>
                    
                    <div class="relative flex py-4 items-center">
                        <div class="flex-grow border-t border-white/10"></div>
                        <span class="flex-shrink-0 mx-4 text-white/30 text-xs uppercase tracking-widest">Manual Override</span>
                        <div class="flex-grow border-t border-white/10"></div>
                    </div>

                    <button onclick="apiCall('/api/pump')" id="btn-pump" class="w-full py-3 rounded-xl font-bold transition-all bg-blue-600/50 hover:bg-blue-500 border border-blue-400/30 text-white">
                        TOGGLE PUMP
                    </button>
                    <button onclick="apiCall('/api/servo')" id="btn-servo" class="w-full py-3 rounded-xl font-bold transition-all bg-emerald-600/50 hover:bg-emerald-500 border border-emerald-400/30 text-white">
                        DISPENSE FOOD
                    </button>
                </div>
            </div>

            <div class="lg:col-span-3 space-y-6 flex flex-col">
                
                <div class="grid grid-cols-3 gap-4">
                    <div class="glass-panel p-4 flex flex-col items-center justify-center relative overflow-hidden">
                        <div class="absolute -right-4 -top-4 w-16 h-16 bg-blue-500/20 rounded-full blur-xl"></div>
                        <span class="text-slate-400 text-xs uppercase tracking-widest">Turbidity</span>
                        <span id="val-turb" class="text-3xl font-bold neon-text mt-1">0%</span>
                    </div>
                    <div class="glass-panel p-4 flex flex-col items-center justify-center relative overflow-hidden">
                        <div class="absolute -right-4 -top-4 w-16 h-16 bg-orange-500/20 rounded-full blur-xl"></div>
                        <span class="text-slate-400 text-xs uppercase tracking-widest">Temperature</span>
                        <span id="val-temp" class="text-3xl font-bold text-orange-400 mt-1">24.5°C</span>
                    </div>
                    <div class="glass-panel p-4 flex flex-col items-center justify-center relative overflow-hidden">
                        <div class="absolute -right-4 -top-4 w-16 h-16 bg-purple-500/20 rounded-full blur-xl"></div>
                        <span class="text-slate-400 text-xs uppercase tracking-widest">Motor Load</span>
                        <span id="val-curr" class="text-3xl font-bold text-purple-400 mt-1">0.0 mA</span>
                    </div>
                </div>

                <div class="glass-panel p-6 flex-grow">
                    <div class="flex justify-between items-center mb-4">
                        <h2 class="text-lg font-semibold text-cyan-100 uppercase tracking-widest">Live Power Signature</h2>
                        <div class="flex gap-4 text-xs font-mono">
                            <span class="text-red-400">Stall: <span id="val-stall">--</span></span>
                            <span class="text-yellow-400">Dry: <span id="val-dry">--</span></span>
                        </div>
                    </div>
                    <div class="relative h-64 w-full">
                        <canvas id="currentChart"></canvas>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <script>
        // Setup Chart.js con colori Neon
        const ctx = document.getElementById('currentChart').getContext('2d');
        const chart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: Array(30).fill(''),
                datasets: [
                { label: 'Current (mA)', borderColor: '#a855f7', backgroundColor: 'rgba(168, 85, 247, 0.1)', borderWidth: 3, pointRadius: 0, fill: true, tension: 0.4, data: Array(30).fill(0) },
                { label: 'Stall Limit (3σ)', borderColor: '#ef4444', borderWidth: 2, pointRadius: 0, borderDash: [5, 5], data: Array(30).fill(null) },
                { label: 'Dry Run (-3σ)', borderColor: '#eab308', borderWidth: 2, pointRadius: 0, borderDash: [5, 5], data: Array(30).fill(null) }
                ]
            },
            options: {
                responsive: true, maintainAspectRatio: false, animation: false,
                scales: { 
                    y: { beginAtZero: true, grid: { color: 'rgba(255,255,255,0.05)' }, ticks: { color: '#94a3b8' } }, 
                    x: { grid: { display: false } } 
                },
                plugins: { legend: { display: false } }
            }
        });

        // Loop di aggiornamento dati
        setInterval(async () => {
            try {
                const res = await fetch('/api/data');
                const data = await res.json();
                
                // Aggiorna Sensori
                document.getElementById('val-turb').innerText = data.turbidity + '%';
                document.getElementById('val-temp').innerText = data.temp.toFixed(1) + '°C';
                document.getElementById('val-curr').innerText = data.current.toFixed(1) + ' mA';
                
                document.getElementById('val-stall').innerText = data.th_stall > 0 ? data.th_stall.toFixed(1) : '--';
                document.getElementById('val-dry').innerText = data.th_dry > 0 ? data.th_dry.toFixed(1) : '--';

                // Status Badge
                const badge = document.getElementById('status-badge');
                badge.innerText = data.status;
                if(data.status === 'MONITORING') badge.className = 'px-4 py-2 rounded-full font-bold text-sm bg-green-500/20 text-green-400 border border-green-500/50';
                else if(data.status === 'HALTED') badge.className = 'px-4 py-2 rounded-full font-bold text-sm bg-red-500/20 text-red-400 border border-red-500/50';
                else if(data.status === 'LEARNING') badge.className = 'px-4 py-2 rounded-full font-bold text-sm bg-amber-500/20 text-amber-400 border border-amber-500/50 shadow-[0_0_10px_rgba(245,158,11,0.5)] animate-pulse';
                else badge.className = 'px-4 py-2 rounded-full font-bold text-sm bg-slate-500/20 text-slate-400 border border-slate-500/50';

                // Auto Badge
                const autoBadge = document.getElementById('auto-badge');
                const btnAuto = document.getElementById('btn-auto');
                if(data.auto_mode) {
                    autoBadge.classList.remove('hidden');
                    btnAuto.classList.replace('bg-purple-600/80', 'bg-red-600/80');
                    btnAuto.innerText = "STOP AUTO MODE";
                } else {
                    autoBadge.classList.add('hidden');
                    btnAuto.classList.replace('bg-red-600/80', 'bg-purple-600/80');
                    btnAuto.innerText = "2. TOGGLE AUTO MODE";
                }

                // Alert Banner
                const alertBanner = document.getElementById('alert-banner');
                if(data.status === 'HALTED') {
                    alertBanner.classList.remove('hidden');
                    document.getElementById('alert-text').innerText = "CRITICAL HALT: " + data.anomaly;
                } else {
                    alertBanner.classList.add('hidden');
                }

                // Pump Button UI
                const btnPump = document.getElementById('btn-pump');
                if(data.pump) {
                    btnPump.innerText = 'TURN PUMP OFF';
                    btnPump.className = 'w-full py-3 rounded-xl font-bold transition-all bg-red-600/80 hover:bg-red-500 border border-red-400/50 text-white';
                } else {
                    btnPump.innerText = 'TURN PUMP ON';
                    btnPump.className = 'w-full py-3 rounded-xl font-bold transition-all bg-blue-600/50 hover:bg-blue-500 border border-blue-400/30 text-white';
                }

                // Aggiorna Grafico
                chart.data.datasets[0].data.push(data.current); chart.data.datasets[0].data.shift();
                chart.data.datasets[1].data.push(data.th_stall > 0 ? data.th_stall : null); chart.data.datasets[1].data.shift();
                chart.data.datasets[2].data.push(data.th_dry > 0 ? data.th_dry : null); chart.data.datasets[2].data.shift();
                chart.update();

            } catch (err) { console.error(err); }
        }, 400);

        async function apiCall(endpoint) {
            await fetch(endpoint, { method: 'POST' });
        }
    </script>
</body>
</html>
)rawliteral";

#endif