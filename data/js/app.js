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
    labels: ["Dashboard & Vitals", "TDS", "Air Temp & Hum", "Water Temp", "Light", "Water Level", "pH", "Live Calibration", "System Settings", "Terminal"],
    icons: ["monitoring", "water_drop", "thermostat", "device_thermostat", "light_mode", "waves", "science", "settings_input_component", "settings", "terminal"],
    activeStyle: "text-white font-bold bg-[rgba(255,255,255,0.1)] rounded-2xl shadow-inner",
    inactiveStyle: "text-on-surface-variant font-medium hover:bg-[rgba(255,255,255,0.05)] hover:text-white rounded-2xl transition-colors duration-200",
    baseStyle: "flex items-center justify-start gap-4 p-3 lg:px-6 lg:py-3 cursor-pointer transition-all duration-150 w-full",
    // Placeholder values shown only until the first "config" WS message arrives
    // and overwrites these with the device's real, live pin assignments.
    gpios: [null, DEFAULT_PINS.tds, DEFAULT_PINS.dht, DEFAULT_PINS.wt, DEFAULT_PINS.sda, DEFAULT_PINS.wl, DEFAULT_PINS.ph, null, null, null],
    // Real sensor_enabled[] state per tab, populated from msg.s_en[] once the
    // first "config" frame arrives. Distinct from `gpios` above: a sensor can
    // have a valid pin (>= 0) but still be enabled:false (e.g. pH ships off by
    // default, or any sensor that auto-disabled after failing startup
    // validation) — the per-sensor detail page toggle should reflect this real
    // flag, not just "does this tab have a pin assigned".
    enabled: [null, true, true, true, true, true, false, null, null, null],
    units: ["", "ppm", "", "°C", "lux", "%", "pH", "", "", ""]
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

// Resolves whether a sensor tab should show as "on": prefers the real
// sensor_enabled[] flag once synced from the device (tabsData.enabled[index]),
// falling back to "does this tab have a pin assigned" only before the first
// config frame arrives. Needed because a sensor can have a valid pin but
// still be enabled:false (pH ships off by default; any sensor can
// auto-disable after failed startup validation).
function resolveSensorOn(index, pin) {
    const enabled = tabsData.enabled[index];
    return (enabled !== null && enabled !== undefined) ? enabled : (pin >= 0);
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

    if (index === 0 || index === 7 || index === 8 || index === 9) {
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
        document.getElementById('dual-sensor-toggle').checked = resolveSensorOn(index, pin);

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

        const sensorOn = resolveSensorOn(index, pin);
        document.getElementById('sensor-toggle').checked = sensorOn;

        if (!sensorOn) {
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
let wsBackoff = 2000;

function initWebSocket() {
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}

function onOpen(event) {
    wsBackoff = 2000;
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
    setTimeout(initWebSocket, wsBackoff);
    wsBackoff = Math.min(60000, wsBackoff * 2);
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
    // `|| 0` on every field here (not just some) protects against a partial
    // "data" frame — e.g. if broadcastData() is ever extended to omit a
    // disabled sensor's field, the same way firebaseUploadCycle() already
    // does. Without it, msg.tds.toFixed() on an undefined field throws and
    // aborts the rest of this handler, silently freezing every OTHER tile
    // on the dashboard too (they're all in the same function, after the
    // line that throws).
    if(document.getElementById('dash-val-tds')) document.getElementById('dash-val-tds').innerText = (msg.tds || 0).toFixed(0);
    if(document.getElementById('dash-val-ph')) document.getElementById('dash-val-ph').innerText = (msg.ph_val || 0).toFixed(2);
    if(document.getElementById('dash-val-atemp')) document.getElementById('dash-val-atemp').innerText = (msg.temp || 0).toFixed(1);
    if(document.getElementById('dash-val-hum')) document.getElementById('dash-val-hum').innerText = (msg.hum || 0).toFixed(0);
    if(document.getElementById('dash-val-wtemp')) document.getElementById('dash-val-wtemp').innerText = (msg.w_t || 0).toFixed(1);
    if(document.getElementById('dash-val-lux')) document.getElementById('dash-val-lux').innerText = (msg.lux || 0).toFixed(0);
    if(document.getElementById('dash-val-wl')) document.getElementById('dash-val-wl').innerText = (msg.wl_percent || 0).toFixed(0);
    if(document.getElementById('dash-val-vpd')) document.getElementById('dash-val-vpd').innerText = (msg.vpd_kpa || 0).toFixed(2);

    if(document.getElementById('cal-tds-raw')) document.getElementById('cal-tds-raw').innerText = (msg.tds || 0).toFixed(1);
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

// SensorID enum order from config.h (S_WL, S_LIGHT, S_TDS, S_DHT, S_PH, S_WTEMP) —
// this is how msg.s_en[] is ordered by broadcastConfig(), which is a DIFFERENT
// order than msg.pins[]/tabsData.gpios (tab-index order). Keep these mappings
// distinct and intentional, same note as in task_network.cpp.
const S_EN_INDEX = { wl: 0, light: 1, tds: 2, dht: 3, ph: 4, wt: 5 };

// ------------------------------------------------------------------
// Client-side pin validation (Part 4 / 5.5). Module-scope (not just inside
// DOMContentLoaded) so it can also be re-run from updateConfigForm() after a
// fresh "config" WS frame lands and repopulates the pin fields — a UX nicety
// only; the real safety boundary is the server-side check in
// save_pins/save_sensor_enabled (task_network.cpp) plus the boot-time
// enforceForbiddenPins() guard.
// ------------------------------------------------------------------
const PIN_FIELD_LABELS = {
    'cfg-pin-tds': 'TDS',
    'cfg-pin-dht': 'DHT22',
    'cfg-pin-ph': 'pH',
    'cfg-pin-wt': 'DS18B20 (Water Temp)',
    'cfg-pin-wl': 'Water Level Signal',
    'cfg-pin-wlp': 'Water Level Power',
    'cfg-pin-sda': 'BH1750 SDA',
    'cfg-pin-scl': 'BH1750 SCL'
};

function validateAllPinFields() {
    const ids = Object.keys(PIN_FIELD_LABELS);
    const fields = ids
        .map((id) => ({ id, el: document.getElementById(id) }))
        .filter((f) => f.el);

    if (fields.length === 0) return true; // Settings page not in the DOM yet / fields not rendered

    // Clear previous error highlighting before re-checking.
    fields.forEach((f) => f.el.classList.remove('border-error', 'text-error'));

    let problem = "";
    let offendingIds = [];

    // Forbidden pins first (19/20 = native USB D-/D+ on this board).
    fields.forEach((f) => {
        const v = parseInt(f.el.value, 10);
        if (v === 19 || v === 20) {
            problem = `GPIO 19 and 20 are reserved for USB on this board and can't be used for a sensor. Change that pin and try again.`;
            offendingIds.push(f.id);
        }
    });

    // Duplicate pin assignments across sensors (Part 5.5). -1 (disabled)
    // never conflicts with anything.
    if (!problem) {
        for (let i = 0; i < fields.length; i++) {
            const vi = parseInt(fields[i].el.value, 10);
            if (isNaN(vi) || vi < 0) continue;
            for (let j = i + 1; j < fields.length; j++) {
                const vj = parseInt(fields[j].el.value, 10);
                if (isNaN(vj) || vj < 0) continue;
                if (vi === vj) {
                    problem = `${PIN_FIELD_LABELS[fields[i].id]} and ${PIN_FIELD_LABELS[fields[j].id]} are both assigned to GPIO${vi}. Each sensor needs its own pin.`;
                    offendingIds.push(fields[i].id, fields[j].id);
                    break;
                }
            }
            if (problem) break;
        }
    }

    offendingIds.forEach((id) => {
        const el = document.getElementById(id);
        if (el) el.classList.add('border-error', 'text-error');
    });

    const pinValidationBanner = document.getElementById('pin-validation-error');
    const pinValidationText = document.getElementById('pin-validation-error-text');
    const btnSavePinsEl = document.getElementById('btn-save-pins');

    if (pinValidationBanner) pinValidationBanner.classList.toggle('hidden', !problem);
    if (pinValidationText && problem) pinValidationText.innerText = problem;
    if (btnSavePinsEl) btnSavePinsEl.disabled = !!problem;

    return problem === "";
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

    // Feature Flags (Part 1 / 2.3) — none of these require a reboot, so we can
    // just reflect the device's live state every time a config frame arrives.
    if(document.getElementById('cfg-demo-mode')) document.getElementById('cfg-demo-mode').checked = !!msg.demo;
    if(document.getElementById('cfg-fb-enabled')) document.getElementById('cfg-fb-enabled').checked = !!msg.fb_en;

    // Demo Mode badge on the Dashboard — only shown while demo mode is on, so
    // simulated readings are never mistaken for real sensor data.
    const demoBadge = document.getElementById('demo-mode-badge');
    if (demoBadge) {
        demoBadge.classList.toggle('hidden', !msg.demo);
        demoBadge.classList.toggle('flex', !!msg.demo);
    }

    // Timing intervals (Part 5.8)
    if(document.getElementById('cfg-int-read') && msg.int_read !== undefined) document.getElementById('cfg-int-read').value = msg.int_read;
    if(document.getElementById('cfg-int-ws') && msg.int_ws !== undefined) document.getElementById('cfg-int-ws').value = msg.int_ws;
    if(document.getElementById('cfg-int-vit') && msg.int_vit !== undefined) document.getElementById('cfg-int-vit').value = msg.int_vit;
    if(document.getElementById('cfg-int-fb') && msg.int_fb !== undefined) document.getElementById('cfg-int-fb').value = msg.int_fb;

    // Per-sensor enabled state (Part 2.4) — reflects the REAL sensor_enabled[]
    // flag, not just "pin >= 0". A sensor can have a valid pin saved but still
    // be enabled:false (pH ships off by default; any sensor can auto-disable
    // after failed startup validation), and the toggle should show that.
    if (msg.s_en && msg.s_en.length >= 6) {
        Object.keys(S_EN_INDEX).forEach((sensorId) => {
            const el = document.getElementById('cfg-sensor-enabled-' + sensorId);
            if (el) el.checked = !!msg.s_en[S_EN_INDEX[sensorId]];
        });

        // Same data, indexed by tab id instead of short sensor-id string, for
        // the per-sensor detail page toggle/error-banner (see resolveSensorOn()).
        tabsData.enabled[1] = !!msg.s_en[S_EN_INDEX.tds];   // TDS
        tabsData.enabled[2] = !!msg.s_en[S_EN_INDEX.dht];   // Air Temp & Humidity
        tabsData.enabled[3] = !!msg.s_en[S_EN_INDEX.wt];    // Water Temp
        tabsData.enabled[4] = !!msg.s_en[S_EN_INDEX.light]; // Light
        tabsData.enabled[5] = !!msg.s_en[S_EN_INDEX.wl];    // Water Level
        tabsData.enabled[6] = !!msg.s_en[S_EN_INDEX.ph];    // pH
    }

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

        // Re-run pin validation now that fresh values landed in the fields —
        // keeps the Save Pins button's disabled state in sync with reality
        // instead of whatever it was before this config frame arrived.
        if (typeof validateAllPinFields === 'function') validateAllPinFields();
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

    // ------------------------------------------------------------------
    // Client-side pin validation (Part 4 / 5.5) — validateAllPinFields()
    // itself is defined at module scope (near updateConfigForm) so it can
    // also be called after a fresh "config" frame lands. Just wire up the
    // live listeners and initial pass here.
    // ------------------------------------------------------------------
    Object.keys(PIN_FIELD_LABELS).forEach((id) => {
        const el = document.getElementById(id);
        if (!el) return;
        el.addEventListener('input', validateAllPinFields);
        el.addEventListener('change', validateAllPinFields);
    });
    validateAllPinFields(); // initial pass in case fields already have values

    const btnSavePins = document.getElementById('btn-save-pins');
    if(btnSavePins) {
        btnSavePins.addEventListener('click', () => {
            // Re-check right before send — don't rely solely on the live
            // listener having already run (e.g. a value changed via script).
            if (!validateAllPinFields()) return;

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
            const targetPpm = parseFloat(document.getElementById('cfg-tds-target').value);
            const currentPpm = parseFloat(document.getElementById('cal-tds-raw').innerText);
            if (isNaN(targetPpm) || isNaN(currentPpm) || currentPpm === 0) {
                alert("Invalid TDS readings");
                return;
            }
            const currentK = globalConfigCache.tds_k || 1.0;
            const newK = currentK * (targetPpm / currentPpm);

            const payload = { command: "calibrate_tds", tds_k: parseFloat(newK.toFixed(2)) };
            websocket.send(JSON.stringify(payload));
            btnCalTds.innerText = "Saved!";
            setTimeout(() => { btnCalTds.innerText = "Calibrate & Save"; }, 2000);
        });
    }

    let ph7Volt = null;
    let ph4Volt = null;

    const btnCalPh7 = document.getElementById('btn-cal-ph-7');
    if(btnCalPh7) {
        btnCalPh7.addEventListener('click', () => {
            const livePh = parseFloat(document.getElementById('cal-ph-raw').innerText);
            if (isNaN(livePh)) return;
            const off = globalConfigCache.ph_off || 0.0;
            const slope = globalConfigCache.ph_slope || 1.0;
            ph7Volt = (livePh - off) / slope;
            document.getElementById('cal-ph-7-val').innerText = ph7Volt.toFixed(3) + " V";
        });
    }

    const btnCalPh4 = document.getElementById('btn-cal-ph-4');
    if(btnCalPh4) {
        btnCalPh4.addEventListener('click', () => {
            const livePh = parseFloat(document.getElementById('cal-ph-raw').innerText);
            if (isNaN(livePh)) return;
            const off = globalConfigCache.ph_off || 0.0;
            const slope = globalConfigCache.ph_slope || 1.0;
            ph4Volt = (livePh - off) / slope;
            document.getElementById('cal-ph-4-val').innerText = ph4Volt.toFixed(3) + " V";
        });
    }

    const btnCalPhSave = document.getElementById('btn-cal-ph-save');
    if(btnCalPhSave) {
        btnCalPhSave.addEventListener('click', () => {
            if (ph7Volt === null || ph4Volt === null || ph7Volt === ph4Volt) {
                alert("Please set both 7.0 and 4.0 calibration points.");
                return;
            }
            const newSlope = (7.0 - 4.0) / (ph7Volt - ph4Volt);
            const newOff = 7.0 - (newSlope * ph7Volt);

            const payload = {
                command: "calibrate_ph",
                offset: parseFloat(newOff.toFixed(2)),
                slope: parseFloat(newSlope.toFixed(2))
            };
            websocket.send(JSON.stringify(payload));
            btnCalPhSave.innerText = "Saved!";
            setTimeout(() => { btnCalPhSave.innerText = "Calculate & Save"; }, 2000);
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
        let sensorId = ""; // short id used by save_sensor_enabled / TAB_TO_SENSOR_ID

        if (tabId === 1) { payload.pin_tds = isEnabled ? DEFAULT_PINS.tds : -1; sensorName = "TDS"; sensorId = "tds"; }
        else if (tabId === 2) { payload.pin_dht = isEnabled ? DEFAULT_PINS.dht : -1; sensorName = "DHT"; sensorId = "dht"; }
        else if (tabId === 3) { payload.pin_wt = isEnabled ? DEFAULT_PINS.wt : -1; sensorName = "Water Temp"; sensorId = "wt"; }
        else if (tabId === 4) { payload.pin_sda = isEnabled ? DEFAULT_PINS.sda : -1; payload.pin_scl = isEnabled ? DEFAULT_PINS.scl : -1; sensorName = "I2C Light"; sensorId = "light"; }
        else if (tabId === 5) { payload.pin_wl = isEnabled ? DEFAULT_PINS.wl : -1; payload.pin_wlp = isEnabled ? DEFAULT_PINS.wlp : -1; sensorName = "Water Level"; sensorId = "wl"; }
        else if (tabId === 6) { payload.pin_ph = isEnabled ? DEFAULT_PINS.ph : -1; sensorName = "pH"; sensorId = "ph"; }

        if (sensorName !== "") {
            websocket.send(JSON.stringify(payload));

            // Part 5.1 fix: restoring the pin alone used to leave
            // sensor_enabled[id] permanently false once a sensor had
            // auto-disabled — nothing else ever set it back to true from this
            // toggle. Send save_sensor_enabled on BOTH paths so this toggle's
            // "enabled" flag always matches its pin state:
            //  - power ON: also clears any lingering auto-disable flag, same
            //    as before.
            //  - power OFF: also flips sensor_enabled[id] to false. Without
            //    this, sensor_enabled[] stayed true while the pin went to -1,
            //    so every read after reboot would fail forever (pin -1 always
            //    fails) and permanently trip the error LED for a sensor the
            //    user deliberately turned off — the "solid green = healthy"
            //    guarantee broke the moment anyone used this toggle to power
            //    something down. (An earlier version of this comment argued
            //    sending enabled:false here was unsafe because the pin-restore
            //    branch in save_sensor_enabled's server handler would refire —
            //    it doesn't: that branch only runs when enabled is true, so
            //    enabled:false here never touches pins, only the flag.)
            if (sensorId) {
                websocket.send(JSON.stringify({ command: "save_sensor_enabled", sensor: sensorId, enabled: isEnabled }));
            }

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

    // ------------------------------------------------------------------
    // Feature Flags card (Part 2.3 / 1.3) — Demo Mode, Firebase Upload
    // Updates. One "Save Feature Flags" button sends all three current
    // checkbox states via save_features. None of these need a reboot.
    // ------------------------------------------------------------------
    const btnSaveFeatures = document.getElementById('btn-save-features');
    if (btnSaveFeatures) {
        btnSaveFeatures.addEventListener('click', () => {
            const payload = {
                command: "save_features",
                demo: !!document.getElementById('cfg-demo-mode')?.checked,
                fb_en: !!document.getElementById('cfg-fb-enabled')?.checked
            };
            websocket.send(JSON.stringify(payload));
            btnSaveFeatures.innerText = "Saved!";
            setTimeout(() => { btnSaveFeatures.innerText = "Save Feature Flags"; }, 2000);
        });
    }

    // ------------------------------------------------------------------
    // Per-sensor enable toggle inside each pinout card (Part 2.4 / 1.4).
    // Bound to save_sensor_enabled. Requires a reboot to apply (same as
    // save_pins), since a restored pin only takes effect after the sensor
    // task re-inits at boot — so this uses the same reboot-confirm UX.
    // ------------------------------------------------------------------
    document.querySelectorAll('[data-sensor-enable]').forEach((el) => {
        el.addEventListener('change', (e) => {
            const sensorId = el.dataset.sensorEnable;
            const enabled = e.target.checked;
            websocket.send(JSON.stringify({ command: "save_sensor_enabled", sensor: sensorId, enabled }));
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Sensor '${sensorId}' ${enabled ? "ENABLED" : "DISABLED"}.</div>`;
            setTimeout(() => {
                if (confirm(`Sensor enabled state changed. The ESP32 must reboot to safely apply hardware changes. Reboot now?`)) {
                    websocket.send(JSON.stringify({ command: "reboot" }));
                } else {
                    e.target.checked = !enabled;
                }
            }, 300);
        });
    });

    // ------------------------------------------------------------------
    // Timing card (Part 5.8) — sample rate / WS push / vitals push / Firestore
    // push, all in ms. Same "omit-to-leave-unchanged" pattern as save_pins;
    // here we just always send all four current field values.
    // ------------------------------------------------------------------
    const btnSaveIntervals = document.getElementById('btn-save-intervals');
    if (btnSaveIntervals) {
        btnSaveIntervals.addEventListener('click', () => {
            const clamp = (v, fallback) => {
                const n = parseInt(v, 10);
                if (isNaN(n)) return fallback;
                return Math.min(60000, Math.max(2000, n));
            };
            const payload = {
                command: "save_intervals",
                int_read: clamp(document.getElementById('cfg-int-read')?.value, 2000),
                int_ws: clamp(document.getElementById('cfg-int-ws')?.value, 1000),
                int_vit: clamp(document.getElementById('cfg-int-vit')?.value, 1000),
                int_fb: clamp(document.getElementById('cfg-int-fb')?.value, 10000)
            };
            websocket.send(JSON.stringify(payload));
            btnSaveIntervals.innerText = "Saved!";
            setTimeout(() => { btnSaveIntervals.innerText = "Save Timing"; }, 2000);
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
