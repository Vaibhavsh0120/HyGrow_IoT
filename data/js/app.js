// Navigation & Banner Logic
document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', (e) => {
        // Change Active Tab
        document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));

        e.target.classList.add('active');
        const targetPage = e.target.getAttribute('data-target');
        document.getElementById(targetPage).classList.add('active');

        // Check if page is incomplete and toggle is ON
        const banner = document.getElementById('incomplete-banner');
        const isWL = targetPage === 'page-wl' && document.getElementById('tog-0').checked;
        const isPH = targetPage === 'page-ph' && document.getElementById('tog-4').checked;

        banner.style.display = (isWL || isPH) ? 'block' : 'none';
    });
});

// WebSocket Connection
const ws = new WebSocket('ws://' + window.location.hostname + '/ws');

ws.onmessage = function(event) {
    const data = JSON.parse(event.data);

    // ── Handle Terminal Logs ──
    if (data.type === 'log') {
        const term = document.getElementById('term-out');
        const time = new Date().toLocaleTimeString();
        term.innerHTML += `<div><span class="log-time">[${time}]</span> ${data.msg}</div>`;
        term.scrollTop = term.scrollHeight; // Auto-scroll
    }
    // ── Handle Real-Time Sensor Data ──
    else if (data.type === 'data') {
        // Update Dashboard text
        document.getElementById('dash-tds').innerText = data.tds.toFixed(1);
        document.getElementById('dash-temp').innerText = data.temp.toFixed(1);
        document.getElementById('dash-hum').innerText = data.hum.toFixed(1);
        document.getElementById('dash-wt').innerText = data.w_t.toFixed(1);
        document.getElementById('dash-lux').innerText = data.lux.toFixed(0);

        // Update individual sensor text
        document.getElementById('val-tds').innerText = data.tds.toFixed(1);
        document.getElementById('val-temp').innerText = data.temp.toFixed(1);
        document.getElementById('val-hum').innerText = data.hum.toFixed(1);
        document.getElementById('val-wt').innerText = data.w_t.toFixed(1);
        document.getElementById('val-lux').innerText = data.lux.toFixed(0);

        // Push to history arrays and draw charts
        pushAndDraw('tds', data.tds, 'chart-tds', '#3b82f6');
        pushAndDraw('temp', data.temp, 'chart-temp', '#10b981');
        pushAndDraw('wt', data.w_t, 'chart-wt', '#06b6d4');
        pushAndDraw('lux', data.lux, 'chart-lux', '#f59e0b');
    }
};

// Helper to manage chart arrays
function pushAndDraw(key, val, canvasId, color) {
    historyData[key].push(val);
    if (historyData[key].length > MAX_POINTS) historyData[key].shift();
    drawChart(canvasId, historyData[key], color);
}

// ── Outgoing Commands to ESP32 (Sent as JSON) ──
function toggleSensor(sensorId, isEnabled) {
    const payload = { cmd: 'toggle_sensor', id: sensorId, state: isEnabled };
    ws.send(JSON.stringify(payload));

    // Force trigger the banner logic if user is currently on an incomplete page
    const activeBtn = document.querySelector('.nav-btn.active');
    if (activeBtn) activeBtn.click();
}

function toggleDemo(isEnabled) {
    const payload = { cmd: 'toggle_demo', state: isEnabled };
    ws.send(JSON.stringify(payload));
}
