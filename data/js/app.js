/**
 * HyGrow IoT - Main Application Logic
 * Handles WebSockets, DOM binding, settings payloads, and UI state.
 */

// ============================================================================
// 1. UI STATE & NAVIGATION
// ============================================================================
const tabsData = {
    labels: ["Dashboard & Vitals", "TDS", "Air Temp & Hum", "Water Temp", "Light", "Water Level", "pH", "Live Calibration", "System Settings", "Terminal", "Firmware"],
    icons: ["monitoring", "water_drop", "thermostat", "device_thermostat", "light_mode", "waves", "science", "settings_input_component", "settings", "terminal", "system_update_alt"],
    activeStyle: "text-white font-bold bg-[rgba(255,255,255,0.1)] rounded-2xl shadow-inner",
    inactiveStyle: "text-on-surface-variant font-medium hover:bg-[rgba(255,255,255,0.05)] hover:text-white rounded-2xl transition-colors duration-200",
    baseStyle: "flex items-center justify-center lg:justify-start gap-4 p-3 lg:px-4 lg:py-3 cursor-pointer transition-all duration-150 w-full",
    gpios: [null, 14, 4, 15, 21, null, 32, null, null, null, null], // Initial visual defaults
    units: ["", "ppm", "", "°C", "lux", "%", "pH", "", "", "", ""]
};

let currentTabId = 0;
let isTerminalPaused = false;
let currentTermFilter = "all";

// Chart Buffers (Keep last 20 readings for the UI)
const MAX_POINTS = 20;
const sensorBuffers = {
    1: [], // TDS
    2: { hum: [], temp: [] }, // Dual (Air Temp/Hum)
    3: [], // Water Temp
    4: [], // Light
    5: [], // Water Level
    6: []  // pH
};

// Canvas instances
let currentSensorCanvas = null;
let currentSensorCtx = null;
const canvasDual = document.getElementById('telemetryChartDual');
const ctxDual = canvasDual ? canvasDual.getContext('2d') : null;

function initNavigation() {
    const navTabsContainer = document.getElementById('nav-tabs');
    if (!navTabsContainer) return;

    tabsData.labels.forEach((label, index) => {
        const li = document.createElement('li');
        li.className = `${tabsData.baseStyle} ${index === 0 ? tabsData.activeStyle : tabsData.inactiveStyle}`;
        li.dataset.id = index;
        li.innerHTML = `
            <span class="material-symbols-outlined">${tabsData.icons[index]}</span>
            <span class="font-label-md text-label-md whitespace-nowrap hidden lg:block">${label}</span>
        `;
        li.addEventListener('click', () => switchTab(index, li));
        navTabsContainer.appendChild(li);
    });
}

function switchTab(index, element) {
    currentTabId = index;
    const navTabsContainer = document.getElementById('nav-tabs');

    // Update Active Classes
    Array.from(navTabsContainer.children).forEach(child => {
        child.className = `${tabsData.baseStyle} ${tabsData.inactiveStyle}`;
    });
    element.className = `${tabsData.baseStyle} ${tabsData.activeStyle} scale-95 transition-transform duration-150`;
    setTimeout(() => { element.classList.remove('scale-95'); }, 150);

    // Hide all pages
    const pages = document.querySelectorAll('.page-section');
    pages.forEach(p => {
        p.classList.add('hidden');
        p.classList.remove('flex');
    });

    // Show relevant page
    const sensorPage = document.getElementById('page-sensor');
    const dualSensorPage = document.getElementById('page-dual-sensor');
    const sensorCanvasContainer = document.getElementById('sensor-canvas-container');

    if (index === 0 || index === 7 || index === 8 || index === 9 || index === 10) {
        const page = document.getElementById(`page-${index}`);
        if(page) {
            page.classList.remove('hidden');
            page.classList.add('flex');
        }
    } else if (index === 2) {
        // Dual Sensor Page
        dualSensorPage.classList.remove('hidden');
        dualSensorPage.classList.add('flex');
        setTimeout(resizeCanvas, 50);
    } else {
        // Generic Single Sensor Page
        sensorPage.classList.remove('hidden');
        sensorPage.classList.add('flex');
        document.getElementById('sensor-name').innerText = tabsData.labels[index] + " Sensor";
        document.getElementById('sensor-icon').innerText = tabsData.icons[index];

        // Retrieve pin from DOM if config loaded it, otherwise fallback
        let pin = tabsData.gpios[index] ? tabsData.gpios[index] : '--';
        document.getElementById('sensor-pin').innerText = `GPIO ${pin}`;

        if (index === 5) {
            document.getElementById('sensor-error').classList.remove('hidden'); // Simulated error for stub
        } else {
            document.getElementById('sensor-error').classList.add('hidden');
        }

        // Setup new canvas
        sensorCanvasContainer.innerHTML = '';
        currentSensorCanvas = document.createElement('canvas');
        currentSensorCanvas.className = 'w-full h-full absolute inset-0';
        sensorCanvasContainer.appendChild(currentSensorCanvas);
        currentSensorCtx = currentSensorCanvas.getContext('2d');

        document.getElementById('sensor-current-val').innerHTML = `-- <span class="text-headline-md text-white/50 ml-1">${tabsData.units[index]}</span>`;
        setTimeout(resizeCanvas, 50);
    }
}

function resizeCanvas() {
    const sensorPage = document.getElementById('page-sensor');
    const dualSensorPage = document.getElementById('page-dual-sensor');

    if(sensorPage && !sensorPage.classList.contains('hidden') && currentSensorCanvas && currentSensorCanvas.parentElement) {
        currentSensorCanvas.width = currentSensorCanvas.parentElement.clientWidth * window.devicePixelRatio;
        currentSensorCanvas.height = currentSensorCanvas.parentElement.clientHeight * window.devicePixelRatio;
        currentSensorCtx.scale(window.devicePixelRatio, window.devicePixelRatio);
        if(typeof drawChart === 'function') {
            drawChart(currentSensorCtx, currentSensorCanvas, sensorBuffers[currentTabId], currentTabId === 1 ? '#4edea3' : '#afc6ff');
        }
    }

    if(dualSensorPage && !dualSensorPage.classList.contains('hidden') && canvasDual && canvasDual.parentElement) {
        canvasDual.width = canvasDual.parentElement.clientWidth * window.devicePixelRatio;
        canvasDual.height = canvasDual.parentElement.clientHeight * window.devicePixelRatio;
        ctxDual.scale(window.devicePixelRatio, window.devicePixelRatio);
        if(typeof drawDualChart === 'function') {
            drawDualChart(ctxDual, canvasDual, sensorBuffers[2].hum, sensorBuffers[2].temp);
        }
    }
}
window.addEventListener('resize', resizeCanvas);


// ============================================================================
// 2. WEBSOCKET & DATA HANDLING
// ============================================================================
let gateway = `ws://${window.location.hostname}/ws`;
let websocket;

function initWebSocket() {
    console.log('Connecting to WS:', gateway);
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}

function onOpen(event) {
    console.log('Connection opened');
    document.getElementById('vital-link-dot').classList.remove('bg-error');
    document.getElementById('vital-link-dot').classList.add('bg-secondary', 'animate-pulse');
    document.getElementById('vital-link-text').innerText = 'LIVE SYS.LINK';
    document.getElementById('vital-link-text').classList.replace('text-error', 'text-secondary');
}

function onClose(event) {
    console.log('Connection closed');
    document.getElementById('vital-link-dot').classList.remove('bg-secondary', 'animate-pulse');
    document.getElementById('vital-link-dot').classList.add('bg-error');
    document.getElementById('vital-link-text').innerText = 'OFFLINE';
    document.getElementById('vital-link-text').classList.replace('text-secondary', 'text-error');
    setTimeout(initWebSocket, 2000); // Auto-reconnect
}

function onMessage(event) {
    let msg;
    try {
        msg = JSON.parse(event.data);
    } catch (e) {
        console.error("WS Parse Error:", e);
        return;
    }

    if (msg.type === "vitals") {
        updateVitals(msg);
    } else if (msg.type === "data") {
        updateTelemetry(msg);
    } else if (msg.type === "config") {
        updateConfigForm(msg);
    } else if (msg.type === "log") {
        updateTerminal(msg);
    }
}

// ============================================================================
// 3. UI UPDATERS
// ============================================================================
function updateVitals(msg) {
    document.getElementById('dash-rssi').innerText = `${msg.rssi} dBm`;
    document.getElementById('dash-heap').innerText = `${(msg.free_heap / 1024).toFixed(1)} KB`;

    // Format Uptime
    let u = msg.uptime;
    let d = Math.floor(u / (3600*24));
    let h = Math.floor(u % (3600*24) / 3600);
    let m = Math.floor(u % 3600 / 60);
    document.getElementById('dash-uptime').innerText = `${d}d ${h}h ${m}m`;
}

function updateTelemetry(msg) {
    // 1. Dashboard Values
    document.getElementById('dash-val-tds').innerText = msg.tds.toFixed(0);
    document.getElementById('dash-val-atemp').innerText = msg.temp.toFixed(1);
    document.getElementById('dash-val-hum').innerText = msg.hum.toFixed(0);

    // Live Calibration Raw ADC Updates (if we had raw analog payload, but we'll use value for now)
    document.getElementById('cal-tds-raw').innerText = msg.tds.toFixed(2);
    document.getElementById('cal-ph-raw').innerText = (msg.ph_val || 0).toFixed(2);

    // 2. Buffer Management
    const pushBuffer = (arr, val) => {
        arr.push(val);
        if(arr.length > MAX_POINTS) arr.shift();
    };

    pushBuffer(sensorBuffers[1], msg.tds);
    pushBuffer(sensorBuffers[2].hum, msg.hum);
    pushBuffer(sensorBuffers[2].temp, msg.temp);
    pushBuffer(sensorBuffers[3], msg.w_t);
    pushBuffer(sensorBuffers[4], msg.lux);
    pushBuffer(sensorBuffers[5], msg.wl_percent || 0);
    pushBuffer(sensorBuffers[6], msg.ph_val || 0);

    // 3. Redraw Active Canvas
    const sensorPage = document.getElementById('page-sensor');
    const dualSensorPage = document.getElementById('page-dual-sensor');

    if(sensorPage && !sensorPage.classList.contains('hidden') && [1,3,4,5,6].includes(currentTabId)) {
        if(typeof drawChart === 'function') {
            drawChart(currentSensorCtx, currentSensorCanvas, sensorBuffers[currentTabId], currentTabId === 1 ? '#4edea3' : '#afc6ff');
        }
        const unitStr = tabsData.units[currentTabId];
        const currentVal = sensorBuffers[currentTabId][sensorBuffers[currentTabId].length-1];
        document.getElementById('sensor-current-val').innerHTML = `${currentVal.toFixed(1)} <span class="text-headline-md text-white/50 ml-1">${unitStr}</span>`;
    }
    else if(dualSensorPage && !dualSensorPage.classList.contains('hidden') && currentTabId === 2) {
        if(typeof drawDualChart === 'function') {
            drawDualChart(ctxDual, canvasDual, sensorBuffers[2].hum, sensorBuffers[2].temp);
        }
        document.getElementById('sensor-dual-temp').innerHTML = `${sensorBuffers[2].temp[sensorBuffers[2].temp.length-1].toFixed(1)} <span class="text-headline-md text-white/50 ml-1">°C</span>`;
        document.getElementById('sensor-dual-hum').innerHTML = `${sensorBuffers[2].hum[sensorBuffers[2].hum.length-1].toFixed(0)} <span class="text-headline-md text-white/50 ml-1">%</span>`;
    }
}

function updateConfigForm(msg) {
    // Populate Inputs
    if(document.getElementById('cfg-wifi-ssid')) document.getElementById('cfg-wifi-ssid').value = msg.wifi_ssid || "";
    if(document.getElementById('cfg-fb-proj')) document.getElementById('cfg-fb-proj').value = msg.fb_proj || "";
    if(document.getElementById('cfg-fb-api')) document.getElementById('cfg-fb-api').value = msg.fb_api || "";
    if(document.getElementById('cfg-fb-email')) document.getElementById('cfg-fb-email').value = msg.fb_email || "";
    if(document.getElementById('cfg-fb-col')) document.getElementById('cfg-fb-col').value = msg.fb_col || "";

    if(document.getElementById('cfg-tds-k')) document.getElementById('cfg-tds-k').value = (msg.tds_k || 1.0).toFixed(2);
    if(document.getElementById('cfg-ph-off')) document.getElementById('cfg-ph-off').value = (msg.ph_off || 0.0).toFixed(2);
    if(document.getElementById('cfg-ph-slope')) document.getElementById('cfg-ph-slope').value = (msg.ph_slope || 1.0).toFixed(2);

    // Update GPIO defaults for the UI headers
    if(msg.pins && msg.pins.length >= 2) {
        tabsData.gpios[1] = msg.pins[0]; // TDS
        tabsData.gpios[6] = msg.pins[1]; // pH
    }
}

function updateTerminal(msg) {
    if (isTerminalPaused) return;

    // Optional filtering if you implemented it in HTML, skipping for standard output
    const term = document.getElementById('terminal-output');
    if(!term) return;

    if(term.children.length > 100) term.removeChild(term.firstChild); // Keep DOM light

    const log = document.createElement('div');
    const colorClass = msg.core === 0 ? "log-core-0" : "log-core-1";
    const levelClass = msg.level === "error" ? "log-error" : (msg.level === "warn" ? "log-warn" : "");

    log.innerHTML = `<span class="${colorClass} opacity-80">[CORE ${msg.core}]</span> <span class="${levelClass}">${msg.msg}</span>`;
    term.appendChild(log);
    term.scrollTop = term.scrollHeight;
}

// ============================================================================
// 4. EVENT LISTENERS & DOM BINDING
// ============================================================================
document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    initWebSocket();
    setTimeout(resizeCanvas, 100);

    // Network Save
    const btnSaveWifi = document.getElementById('btn-save-wifi');
    if(btnSaveWifi) {
        btnSaveWifi.addEventListener('click', () => {
            const payload = {
                command: "save_wifi",
                ssid: document.getElementById('cfg-wifi-ssid').value,
                pass: document.getElementById('cfg-wifi-pass').value
            };
            websocket.send(JSON.stringify(payload));
            btnSaveWifi.innerText = "Saved! Rebooting...";
        });
    }

    // Firebase Save
    const btnSaveFb = document.getElementById('btn-save-firebase');
    if(btnSaveFb) {
        btnSaveFb.addEventListener('click', () => {
            const payload = {
                command: "save_firebase",
                proj: document.getElementById('cfg-fb-proj').value,
                api: document.getElementById('cfg-fb-api').value,
                email: document.getElementById('cfg-fb-email').value,
                pass: document.getElementById('cfg-fb-pass').value,
                col: document.getElementById('cfg-fb-col').value
            };
            websocket.send(JSON.stringify(payload));
            btnSaveFb.innerText = "Credentials Saved";
            setTimeout(() => { btnSaveFb.innerText = "Save Credentials"; }, 2000);
        });
    }

    // Calibration Saves
    const btnCalTds = document.getElementById('btn-cal-tds');
    if(btnCalTds) {
        btnCalTds.addEventListener('click', () => {
            const payload = {
                command: "calibrate_tds",
                tds_k: parseFloat(document.getElementById('cfg-tds-k').value)
            };
            websocket.send(JSON.stringify(payload));
        });
    }

    const btnCalPh = document.getElementById('btn-cal-ph');
    if(btnCalPh) {
        btnCalPh.addEventListener('click', () => {
            const payload = {
                command: "calibrate_ph",
                offset: parseFloat(document.getElementById('cfg-ph-off').value),
                slope: parseFloat(document.getElementById('cfg-ph-slope').value)
            };
            websocket.send(JSON.stringify(payload));
        });
    }

    // Danger Zone
    const btnReboot = document.getElementById('btn-reboot');
    if(btnReboot) {
        btnReboot.addEventListener('click', () => {
            if(confirm("Are you sure you want to reboot the device?")) {
                websocket.send(JSON.stringify({command: "reboot"}));
            }
        });
    }

    const btnReset = document.getElementById('btn-factory-reset');
    if(btnReset) {
        btnReset.addEventListener('click', () => {
            if(confirm("DANGER! This will wipe all Wi-Fi, Firebase, and Calibration data. The device will become a local access point. Proceed?")) {
                websocket.send(JSON.stringify({command: "factory_reset"}));
            }
        });
    }

    // Terminal Controls
    const btnTermPause = document.getElementById('btn-term-pause');
    if(btnTermPause) {
        btnTermPause.addEventListener('click', () => {
            isTerminalPaused = !isTerminalPaused;
            btnTermPause.innerText = isTerminalPaused ? "Resume" : "Pause";
            btnTermPause.classList.toggle('bg-white/30');
        });
    }

    const btnTermClear = document.getElementById('btn-term-clear');
    if(btnTermClear) {
        btnTermClear.addEventListener('click', () => {
            document.getElementById('terminal-output').innerHTML = '<div><span class="text-secondary opacity-70">[SYS]</span> Terminal cleared.</div>';
        });
    }

    // CSV Export
    const btnExport = document.getElementById('btn-export-csv');
    if(btnExport) {
        btnExport.addEventListener('click', () => {
            if(typeof exportSeriesToCsv === 'function') {
                // Export the TDS buffer as an example
                exportSeriesToCsv('TDS_Log', sensorBuffers[1]);
            }
        });
    }
});
