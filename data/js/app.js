/**
 * HyGrow IoT - Main Application Logic
 * Handles WebSockets, DOM binding, settings payloads, and UI state.
 */

// ============================================================================
// 1. UI STATE & NAVIGATION
// ============================================================================
// Compiled defaults from config.h — the single source of truth for "what
// pin should this sensor use when the user turns it on or resets it".
// Keep this in sync with config.h's DEFAULT_PIN_* macros.
const DEFAULT_PINS = { tds: 2, dht: 6, wt: 4, sda: 8, scl: 9, wl: 1, wlp: 5, ph: 7 };

const tabsData = {
    labels: ["Dashboard & Vitals", "TDS", "Air Temp & Hum", "Water Temp", "Light", "Water Level", "pH", "Live Calibration", "System Settings", "Terminal", "Firmware"],
    icons: ["monitoring", "water_drop", "thermostat", "device_thermostat", "light_mode", "waves", "science", "settings_input_component", "settings", "terminal", "system_update_alt"],
    activeStyle: "text-white font-bold bg-[rgba(255,255,255,0.1)] rounded-2xl shadow-inner",
    inactiveStyle: "text-on-surface-variant font-medium hover:bg-[rgba(255,255,255,0.05)] hover:text-white rounded-2xl transition-colors duration-200",
    baseStyle: "flex items-center justify-start gap-4 p-3 lg:px-6 lg:py-3 cursor-pointer transition-all duration-150 w-full",
    // Placeholder values shown only until the first "config" WS message arrives
    // and overwrites these with the device's real, live pin assignments.
    gpios: [null, DEFAULT_PINS.tds, DEFAULT_PINS.dht, DEFAULT_PINS.wt, DEFAULT_PINS.sda, DEFAULT_PINS.wl, DEFAULT_PINS.ph, null, null, null, null],
    units: ["", "ppm", "", "°C", "lux", "%", "pH", "", "", "", ""]
};

let currentTabId = 0;
let isTerminalPaused = false;
let globalConfigCache = {}; // Cache config data for CSV export

// Chart Buffers (Keep last 20 readings for the UI graphs and CSV Export)
const MAX_POINTS = 20;
const sensorBuffers = {
    1: [], // TDS
    2: { hum: [], temp: [] }, // Dual (Air Temp/Hum)
    3: [], // Water Temp
    4: [], // Light
    5: [], // Water Level
    6: [], // pH
    7: []  // VPD (internal array just for CSV export)
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
        dualSensorPage.classList.remove('hidden');
        dualSensorPage.classList.add('flex');

        let pin = tabsData.gpios[index];
        document.getElementById('dual-sensor-pin').innerText = (pin === null || pin < 0) ? '-- (Disabled)' : pin;
        document.getElementById('dual-sensor-toggle').checked = (pin >= 0);

        setTimeout(resizeCanvas, 50);
    } else {
        sensorPage.classList.remove('hidden');
        sensorPage.classList.add('flex');
        document.getElementById('sensor-name').innerText = tabsData.labels[index] + " Sensor";
        document.getElementById('sensor-icon').innerText = tabsData.icons[index];

        let pin = tabsData.gpios[index];
        // Special display case for I2C Light sensor
        if (index === 4 && pin >= 0) {
            document.getElementById('sensor-pin').innerText = `SDA: ${pin}`;
        } else {
            document.getElementById('sensor-pin').innerText = (pin === null || pin < 0) ? '-- (Disabled)' : pin;
        }

        document.getElementById('sensor-toggle').checked = (pin >= 0);

        if (pin < 0) {
            document.getElementById('sensor-error').classList.remove('hidden');
        } else {
            document.getElementById('sensor-error').classList.add('hidden');
        }

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
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}

function onOpen(event) {
    document.getElementById('vital-link-dot').classList.remove('bg-error');
    document.getElementById('vital-link-dot').classList.add('bg-secondary', 'animate-pulse');
    document.getElementById('vital-link-text').innerText = 'LIVE SYS.LINK';
    document.getElementById('vital-link-text').classList.replace('text-error', 'text-secondary');
}

function onClose(event) {
    document.getElementById('vital-link-dot').classList.remove('bg-secondary', 'animate-pulse');
    document.getElementById('vital-link-dot').classList.add('bg-error');
    document.getElementById('vital-link-text').innerText = 'OFFLINE';
    document.getElementById('vital-link-text').classList.replace('text-secondary', 'text-error');
    setTimeout(initWebSocket, 2000); // Auto-reconnect
}

function onMessage(event) {
    let msg;
    try { msg = JSON.parse(event.data); } catch (e) { return; }

    if (msg.type === "vitals") updateVitals(msg);
    else if (msg.type === "data") updateTelemetry(msg);
    else if (msg.type === "config") updateConfigForm(msg);
    else if (msg.type === "log") updateTerminal(msg);
}

// ============================================================================
// 3. UI UPDATERS
// ============================================================================
function updateVitals(msg) {
    document.getElementById('dash-rssi').innerText = `${msg.rssi} dBm`;
    document.getElementById('dash-heap').innerText = `${(msg.free_heap / 1024).toFixed(1)} KB`;
    let u = msg.uptime;
    let d = Math.floor(u / (3600*24));
    let h = Math.floor(u % (3600*24) / 3600);
    let m = Math.floor(u % 3600 / 60);
    document.getElementById('dash-uptime').innerText = `${d}d ${h}h ${m}m`;
}

function updateTelemetry(msg) {
    if(document.getElementById('dash-val-tds')) document.getElementById('dash-val-tds').innerText = msg.tds.toFixed(0);
    if(document.getElementById('dash-val-ph')) document.getElementById('dash-val-ph').innerText = (msg.ph_val || 0).toFixed(2);
    if(document.getElementById('dash-val-atemp')) document.getElementById('dash-val-atemp').innerText = msg.temp.toFixed(1);
    if(document.getElementById('dash-val-hum')) document.getElementById('dash-val-hum').innerText = msg.hum.toFixed(0);
    if(document.getElementById('dash-val-wtemp')) document.getElementById('dash-val-wtemp').innerText = (msg.w_t || 0).toFixed(1);
    if(document.getElementById('dash-val-lux')) document.getElementById('dash-val-lux').innerText = (msg.lux || 0).toFixed(0);
    if(document.getElementById('dash-val-wl')) document.getElementById('dash-val-wl').innerText = (msg.wl_percent || 0).toFixed(0);
    if(document.getElementById('dash-val-vpd')) document.getElementById('dash-val-vpd').innerText = (msg.vpd_kpa || 0).toFixed(2);

    if(document.getElementById('cal-tds-raw')) document.getElementById('cal-tds-raw').innerText = msg.tds.toFixed(1);
    if(document.getElementById('cal-ph-raw')) document.getElementById('cal-ph-raw').innerText = (msg.ph_val || 0).toFixed(2);

    const pushBuffer = (arr, val) => {
        arr.push(val);
        if(arr.length > MAX_POINTS) arr.shift();
    };

    pushBuffer(sensorBuffers[1], msg.tds);
    pushBuffer(sensorBuffers[2].hum, msg.hum);
    pushBuffer(sensorBuffers[2].temp, msg.temp);
    pushBuffer(sensorBuffers[3], msg.w_t || 0);
    pushBuffer(sensorBuffers[4], msg.lux || 0);
    pushBuffer(sensorBuffers[5], msg.wl_percent || 0);
    pushBuffer(sensorBuffers[6], msg.ph_val || 0);
    pushBuffer(sensorBuffers[7], msg.vpd_kpa || 0);

    const sensorPage = document.getElementById('page-sensor');
    const dualSensorPage = document.getElementById('page-dual-sensor');

    if(sensorPage && !sensorPage.classList.contains('hidden') && [1,3,4,5,6].includes(currentTabId)) {
        if(typeof drawChart === 'function') {
            drawChart(currentSensorCtx, currentSensorCanvas, sensorBuffers[currentTabId], currentTabId === 1 ? '#4edea3' : '#afc6ff');
        }
        const unitStr = tabsData.units[currentTabId];
        const currentVal = sensorBuffers[currentTabId][sensorBuffers[currentTabId].length-1];
        if (currentVal !== undefined) {
            document.getElementById('sensor-current-val').innerHTML = `${currentVal.toFixed(1)} <span class="text-headline-md text-white/50 ml-1">${unitStr}</span>`;
        }
    }
    else if(dualSensorPage && !dualSensorPage.classList.contains('hidden') && currentTabId === 2) {
        if(typeof drawDualChart === 'function') {
            drawDualChart(ctxDual, canvasDual, sensorBuffers[2].hum, sensorBuffers[2].temp);
        }
        if (sensorBuffers[2].temp.length > 0) {
            document.getElementById('sensor-dual-temp').innerHTML = `${sensorBuffers[2].temp[sensorBuffers[2].temp.length-1].toFixed(1)} <span class="text-headline-md text-white/50 ml-1">°C</span>`;
            document.getElementById('sensor-dual-hum').innerHTML = `${sensorBuffers[2].hum[sensorBuffers[2].hum.length-1].toFixed(0)} <span class="text-headline-md text-white/50 ml-1">%</span>`;
        }
    }
}

function updateConfigForm(msg) {
    globalConfigCache = msg; // Cache for CSV export

    if(document.getElementById('cfg-wifi-ssid')) document.getElementById('cfg-wifi-ssid').value = msg.wifi_ssid || "";
    if(document.getElementById('cfg-fb-proj')) document.getElementById('cfg-fb-proj').value = msg.fb_proj || "";
    if(document.getElementById('cfg-fb-api')) document.getElementById('cfg-fb-api').value = msg.fb_api || "";
    if(document.getElementById('cfg-fb-email')) document.getElementById('cfg-fb-email').value = msg.fb_email || "";
    if(document.getElementById('cfg-fb-col')) document.getElementById('cfg-fb-col').value = msg.fb_col || "";

    if(document.getElementById('cfg-tds-k')) document.getElementById('cfg-tds-k').value = (msg.tds_k || 1.0).toFixed(2);
    if(document.getElementById('cfg-ph-off')) document.getElementById('cfg-ph-off').value = (msg.ph_off || 0.0).toFixed(2);
    if(document.getElementById('cfg-ph-slope')) document.getElementById('cfg-ph-slope').value = (msg.ph_slope || 1.0).toFixed(2);

    if(msg.pins && msg.pins.length >= 7) {
        tabsData.gpios[1] = msg.pins[0]; // TDS
        tabsData.gpios[2] = msg.pins[1]; // DHT
        tabsData.gpios[6] = msg.pins[2]; // pH
        tabsData.gpios[3] = msg.pins[3]; // W_Temp
        tabsData.gpios[5] = msg.pins[4]; // W_Level
        tabsData.gpios[4] = msg.pins[5]; // Light SDA

        if(document.getElementById('cfg-pin-tds')) document.getElementById('cfg-pin-tds').value = msg.pins[0];
        if(document.getElementById('cfg-pin-dht')) document.getElementById('cfg-pin-dht').value = msg.pins[1];
        if(document.getElementById('cfg-pin-ph'))  document.getElementById('cfg-pin-ph').value = msg.pins[2];
        if(document.getElementById('cfg-pin-wt'))  document.getElementById('cfg-pin-wt').value = msg.pins[3];
        if(document.getElementById('cfg-pin-wl'))  document.getElementById('cfg-pin-wl').value = msg.pins[4];
        if(document.getElementById('cfg-pin-sda')) document.getElementById('cfg-pin-sda').value = msg.pins[5];
        if(document.getElementById('cfg-pin-scl')) document.getElementById('cfg-pin-scl').value = msg.pins[6];

        // 8th element (added alongside pin_wl_power support) — older firmware
        // that hasn't been reflashed yet just won't send it, so guard the length.
        if(msg.pins.length >= 8 && document.getElementById('cfg-pin-wlp')) {
            document.getElementById('cfg-pin-wlp').value = msg.pins[7];
        }
    }
}

function updateTerminal(msg) {
    if (isTerminalPaused) return;
    const term = document.getElementById('terminal-output');
    if(!term) return;
    if(term.children.length > 100) term.removeChild(term.firstChild);

    const log = document.createElement('div');
    const colorClass = msg.core === 0 ? "log-core-0" : "log-core-1";
    const levelClass = msg.level === "error" ? "text-error font-bold" : (msg.level === "warn" ? "text-secondary" : "");
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

    const btnSavePins = document.getElementById('btn-save-pins');
    if(btnSavePins) {
        btnSavePins.addEventListener('click', () => {
            const wlpEl = document.getElementById('cfg-pin-wlp');
            const payload = {
                command: "save_pins",
                pin_tds: parseInt(document.getElementById('cfg-pin-tds').value),
                pin_dht: parseInt(document.getElementById('cfg-pin-dht').value),
                pin_ph: parseInt(document.getElementById('cfg-pin-ph').value),
                pin_wt: parseInt(document.getElementById('cfg-pin-wt').value),
                pin_wl: parseInt(document.getElementById('cfg-pin-wl').value),
                pin_sda: parseInt(document.getElementById('cfg-pin-sda').value),
                pin_scl: parseInt(document.getElementById('cfg-pin-scl').value)
            };
            if (wlpEl) payload.pin_wlp = parseInt(wlpEl.value);
            websocket.send(JSON.stringify(payload));
            if(confirm("Pinout saved. The ESP32 must reboot to reassign hardware interrupts safely. Reboot now?")) {
                websocket.send(JSON.stringify({command: "reboot"}));
            }
        });
    }

    const btnCalTds = document.getElementById('btn-cal-tds');
    if(btnCalTds) {
        btnCalTds.addEventListener('click', () => {
            const payload = { command: "calibrate_tds", tds_k: parseFloat(document.getElementById('cfg-tds-k').value) };
            websocket.send(JSON.stringify(payload));
            btnCalTds.innerText = "Saved!";
            setTimeout(() => { btnCalTds.innerText = "Save TDS Calibration"; }, 2000);
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
            btnCalPh.innerText = "Saved!";
            setTimeout(() => { btnCalPh.innerText = "Save pH Calibration"; }, 2000);
        });
    }

    const btnReboot = document.getElementById('btn-reboot');
    if(btnReboot) btnReboot.addEventListener('click', () => { if(confirm("Reboot the device?")) websocket.send(JSON.stringify({command: "reboot"})); });

    const btnReset = document.getElementById('btn-factory-reset');
    if(btnReset) btnReset.addEventListener('click', () => { if(confirm("DANGER! Wipe all NVS data?")) websocket.send(JSON.stringify({command: "factory_reset"})); });

    const btnTermPause = document.getElementById('btn-term-pause');
    if(btnTermPause) btnTermPause.addEventListener('click', () => {
        isTerminalPaused = !isTerminalPaused;
        btnTermPause.innerText = isTerminalPaused ? "Resume" : "Pause";
        btnTermPause.classList.toggle('bg-white/30');
    });

    const btnTermClear = document.getElementById('btn-term-clear');
    if(btnTermClear) btnTermClear.addEventListener('click', () => {
        document.getElementById('terminal-output').innerHTML = '<div><span class="text-secondary opacity-70">[SYS]</span> Terminal cleared.</div>';
    });

    // Advanced CSV Export (Bundles config and the 20-point buffers for all 8 sensors)
    const btnExport = document.getElementById('btn-export-csv');
    if(btnExport) {
        btnExport.addEventListener('click', () => {
            if(!sensorBuffers[1].length) { alert("Waiting for telemetry data..."); return; }

            let csv = "data:text/csv;charset=utf-8,\n";
            csv += "--- SYSTEM CONFIGURATION ---\n";
            csv += `Firebase Project,${globalConfigCache.fb_proj || "N/A"}\n`;
            csv += `Firestore Collection,${globalConfigCache.fb_col || "N/A"}\n`;
            csv += `TDS Calibration (K),${globalConfigCache.tds_k || "1.0"}\n`;
            csv += `pH Calibration (Offset),${globalConfigCache.ph_off || "0.0"}\n`;
            csv += `pH Calibration (Slope),${globalConfigCache.ph_slope || "1.0"}\n\n`;

            csv += "--- TELEMETRY HISTORY (Last 20 Reads) ---\n";
            csv += "Index,TDS(ppm),AirTemp(C),Humidity(%),WaterTemp(C),Light(lux),WaterLevel(%),pH,VPD(kPa)\n";

            for(let i=0; i < sensorBuffers[1].length; i++) {
                csv += `${i},`;
                csv += `${(sensorBuffers[1][i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[2].temp[i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[2].hum[i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[3][i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[4][i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[5][i]||0).toFixed(1)},`;
                csv += `${(sensorBuffers[6][i]||0).toFixed(2)},`;
                csv += `${(sensorBuffers[7][i]||0).toFixed(2)}\n`;
            }

            const link = document.createElement("a");
            link.setAttribute("href", encodeURI(csv));
            const d = new Date();
            link.setAttribute("download", `hygrow_export_${d.getFullYear()}${(d.getMonth()+1)}${d.getDate()}.csv`);
            document.body.appendChild(link);
            link.click();
            document.body.removeChild(link);
        });
    }

    // Toggle logic includes SDA and SCL for I2C Light
    const handleToggle = (e, tabId) => {
        const isEnabled = e.target.checked;
        let payload = { command: "save_pins" };
        let sensorName = "";

        if (tabId === 1) { payload.pin_tds = isEnabled ? DEFAULT_PINS.tds : -1; sensorName = "TDS"; }
        else if (tabId === 2) { payload.pin_dht = isEnabled ? DEFAULT_PINS.dht : -1; sensorName = "DHT"; }
        else if (tabId === 3) { payload.pin_wt = isEnabled ? DEFAULT_PINS.wt : -1; sensorName = "Water Temp"; }
        else if (tabId === 4) { payload.pin_sda = isEnabled ? DEFAULT_PINS.sda : -1; payload.pin_scl = isEnabled ? DEFAULT_PINS.scl : -1; sensorName = "I2C Light"; }
        else if (tabId === 5) { payload.pin_wl = isEnabled ? DEFAULT_PINS.wl : -1; payload.pin_wlp = isEnabled ? DEFAULT_PINS.wlp : -1; sensorName = "Water Level"; }
        else if (tabId === 6) { payload.pin_ph = isEnabled ? DEFAULT_PINS.ph : -1; sensorName = "pH"; }

        if (sensorName !== "") {
            websocket.send(JSON.stringify(payload));
            const status = isEnabled ? "POWERED ON" : "POWERED OFF";
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> ${sensorName} ${status}.</div>`;
            setTimeout(() => {
                if(confirm(`Sensor power state changed. The ESP32 must reboot to safely apply hardware changes. Reboot now?`)) {
                    websocket.send(JSON.stringify({command: "reboot"}));
                } else { e.target.checked = !isEnabled; }
            }, 300);
        }
    };

    const singleSensorToggle = document.getElementById('sensor-toggle');
    if (singleSensorToggle) singleSensorToggle.addEventListener('change', (e) => handleToggle(e, currentTabId));

    const dualSensorToggle = document.getElementById('dual-sensor-toggle');
    if (dualSensorToggle) dualSensorToggle.addEventListener('change', (e) => handleToggle(e, 2));

    // Reset-to-default-pin buttons — one per pinout card, plus a generic one
    // on the per-sensor offline banner that resets whichever sensor tab is open.
    const sendResetSensorPin = (sensorId) => {
        if (!confirm(`Reset the '${sensorId}' pin(s) to the factory default and reboot?`)) return;
        websocket.send(JSON.stringify({ command: "reset_sensor_pin", sensor: sensorId }));
    };

    document.querySelectorAll('[data-reset-sensor]').forEach((btn) => {
        btn.addEventListener('click', () => sendResetSensorPin(btn.dataset.resetSensor));
    });

    const TAB_TO_SENSOR_ID = { 1: "tds", 2: "dht", 3: "wt", 4: "light", 5: "wl", 6: "ph" };
    const btnResetCurrent = document.getElementById('btn-reset-current-sensor');
    if (btnResetCurrent) {
        btnResetCurrent.addEventListener('click', () => {
            const sensorId = TAB_TO_SENSOR_ID[currentTabId];
            if (sensorId) sendResetSensorPin(sensorId);
        });
    }

    // Theme: Light / Dark / Auto, persisted in localStorage. The <head> has a
    // small inline script that applies the saved theme before first paint to
    // avoid a flash of the wrong theme — this just keeps the picker in sync.
    const applyTheme = (theme) => {
        const html = document.documentElement;
        html.classList.remove('dark', 'light');
        if (theme === 'auto') {
            const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
            html.classList.add(prefersDark ? 'dark' : 'light');
        } else {
            html.classList.add(theme);
        }
    };

    const themeSelect = document.getElementById('cfg-theme-select');
    const savedTheme = localStorage.getItem('hygrow_theme') || 'dark';
    if (themeSelect) themeSelect.value = savedTheme;

    if (themeSelect) {
        themeSelect.addEventListener('change', () => {
            localStorage.setItem('hygrow_theme', themeSelect.value);
            applyTheme(themeSelect.value);
        });
    }

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
        if ((localStorage.getItem('hygrow_theme') || 'dark') === 'auto') applyTheme('auto');
    });
});
