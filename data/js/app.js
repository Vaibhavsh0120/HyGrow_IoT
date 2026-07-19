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
    // Per-tab health status from msg.s_ok[] (0=disabled, 1=healthy, 2=enabled
    // but failing to read), populated in updateTelemetry() once a "data" WS
    // frame arrives. null until then — matches `enabled`'s null-until-synced
    // convention above for tabs with no corresponding sensor (0, 7, 8, 9).
    ok: [null, null, null, null, null, null, null, null, null, null],
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

    navTabsContainer.setAttribute('role', 'tablist');

    tabsData.labels.forEach((label, index) => {
        const li = document.createElement('li');
        li.className = `${tabsData.baseStyle} ${index === 0 ? tabsData.activeStyle : tabsData.inactiveStyle}`;
        li.dataset.id = index;
        // Fix (gap #8): nav items were plain <li> click targets with no
        // keyboard or screen-reader support at all — a mouse-only control.
        li.setAttribute('role', 'tab');
        li.setAttribute('tabindex', '0');
        li.setAttribute('aria-selected', index === 0 ? 'true' : 'false');
        li.setAttribute('aria-label', label);
        li.innerHTML = `
            <span class="material-symbols-outlined" aria-hidden="true">${tabsData.icons[index]}</span>
            <span class="font-label-md text-label-md whitespace-nowrap hidden lg:block">${label}</span>
        `;
        li.addEventListener('click', () => switchTab(index, li));
        li.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); switchTab(index, li); }
        });
        navTabsContainer.appendChild(li);
    });
}

// Resolves whether a sensor tab should show as "on": reads the real
// sensor_enabled[] flag once synced from the device (tabsData.enabled[index]).
// sensor_enabled[] is the ONLY on/off switch in this firmware — a pin value
// is never consulted here, so a sensor with a valid pin but enabled:false
// (pH ships off by default; any sensor can auto-disable after failed
// startup validation) correctly shows as off. Before the first config frame
// arrives there's nothing to resolve yet, so this defaults to "off" rather
// than guessing from the pin.
function resolveSensorOn(index) {
    return !!tabsData.enabled[index];
}

// Keeps the small "ON"/"OFF" text next to a power toggle in sync with its
// checked state. The toggle's blue-vs-dark track color alone was hard to
// read at a glance in this dark theme, especially before the knob position
// registers — this makes the state unambiguous regardless of color contrast.
function syncPowerToggleLabel(toggleId, labelId) {
    const toggle = document.getElementById(toggleId);
    const label = document.getElementById(labelId);
    if (!toggle || !label) return;
    label.innerText = toggle.checked ? 'ON' : 'OFF';
    label.classList.toggle('text-secondary', toggle.checked);
    label.classList.toggle('text-on-surface-variant', !toggle.checked);
}

function switchTab(index, element) {
    currentTabId = index;
    const navTabsContainer = document.getElementById('nav-tabs');

    // Update Active Classes
    Array.from(navTabsContainer.children).forEach(child => {
        child.className = `${tabsData.baseStyle} ${tabsData.inactiveStyle}`;
        child.setAttribute('aria-selected', 'false');
    });
    element.className = `${tabsData.baseStyle} ${tabsData.activeStyle} scale-95 transition-transform duration-150`;
    element.setAttribute('aria-selected', 'true');
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
        document.getElementById('dual-sensor-pin').innerText = (pin === null || pin < 0) ? '--' : pin;
        document.getElementById('dual-sensor-toggle').checked = resolveSensorOn(index);
        syncPowerToggleLabel('dual-sensor-toggle', 'dual-sensor-toggle-state');

        setTimeout(resizeCanvas, 50);
    } else {
        sensorPage.classList.remove('hidden');
        sensorPage.classList.add('flex');
        document.getElementById('sensor-name').innerText = tabsData.labels[index] + " Sensor";
        document.getElementById('sensor-icon').innerText = tabsData.icons[index];

        let pin = tabsData.gpios[index];
        // Special display case for I2C Light sensor. The pin is always shown
        // as a plain GPIO number — it's never used to infer on/off state,
        // see resolveSensorOn() above.
        if (index === 4 && pin !== null && pin !== undefined) {
            document.getElementById('sensor-pin').innerText = `SDA: ${pin}`;
        } else {
            document.getElementById('sensor-pin').innerText = (pin === null || pin === undefined) ? '--' : pin;
        }

        const sensorOn = resolveSensorOn(index);
        document.getElementById('sensor-toggle').checked = sensorOn;
        syncPowerToggleLabel('sensor-toggle', 'sensor-toggle-state');

        // tabsData.ok[index] (from the latest "data" frame's s_ok[], see
        // updateTelemetry()) distinguishes "disabled" from "enabled but not
        // actually reading" — resolveSensorOn() alone only knows disabled.
        // null/undefined means no "data" frame has arrived yet for this tab;
        // fall back to the disabled-only check in that case.
        const okCode = tabsData.ok[index];
        const errorBanner = document.getElementById('sensor-error');
        const errorText = document.getElementById('sensor-error-text');
        if (!sensorOn) {
            if (errorText) errorText.innerText = 'Sensor disabled.';
            errorBanner.classList.remove('hidden');
        } else if (okCode === 2) {
            if (errorText) errorText.innerText = 'Sensor enabled but not reading — check wiring, then see the Terminal log for the last error.';
            errorBanner.classList.remove('hidden');
        } else {
            errorBanner.classList.add('hidden');
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
// 1b. AUTH OVERLAY (single-owner login / first-time setup)
// ============================================================================
// Session persistence: a successful login/setup issues a session token from
// the device (see handleAuthCommand() in task_network.cpp), stored here in
// localStorage. On the next page load, if a token exists it's sent as the
// very first WS frame instead of a password, so a returning browser skips
// straight past the login screen (see initWebSocket()/onMessage() below).
const AUTH_TOKEN_KEY = 'hygrow_auth_token';

function getStoredAuthToken() {
    try { return localStorage.getItem(AUTH_TOKEN_KEY) || ''; }
    catch (e) { return ''; } // localStorage can throw in some private-browsing modes
}

function setStoredAuthToken(token) {
    try {
        if (token) localStorage.setItem(AUTH_TOKEN_KEY, token);
        else localStorage.removeItem(AUTH_TOKEN_KEY);
    } catch (e) { /* ignore — worst case, the user logs in again next reload */ }
}

// Shows exactly one of the three overlay panels (spinner / setup / login) and
// hides the other two. Passing 'none' hides the whole overlay, revealing the
// dashboard underneath — only done once authentication actually succeeds.
function showAuthPanel(panel) {
    const overlay = document.getElementById('auth-overlay');
    const spinner = document.getElementById('auth-spinner');
    const setup = document.getElementById('auth-setup');
    const login = document.getElementById('auth-login');
    if (!overlay) return;

    if (panel === 'none') {
        overlay.classList.add('hidden');
        return;
    }
    overlay.classList.remove('hidden');
    if (spinner) spinner.classList.toggle('hidden', panel !== 'spinner');
    if (setup) setup.classList.toggle('hidden', panel !== 'setup');
    if (login) login.classList.toggle('hidden', panel !== 'login');
}

// Handles the device's "auth_status" frame — the very first message sent on
// every fresh WS connection (see sendAuthStatus() in task_network.cpp). If a
// session token is already stored from a previous login, try it silently
// before ever showing the Login modal; otherwise branch straight to
// Setup/Login based on setup_required.
function handleAuthStatus(msg) {
    const storedToken = getStoredAuthToken();
    if (storedToken) {
        websocket.send(JSON.stringify({ command: "auth", token: storedToken }));
        return; // wait for auth_result — keep showing the spinner meanwhile
    }
    showAuthPanel(msg.setup_required ? 'setup' : 'login');
}

// Handles the device's "auth_result" frame, sent in response to every
// { command: "auth", ... } this client sends (see handleAuthCommand() in
// task_network.cpp).
function handleAuthResult(msg) {
    if (msg.ok) {
        if (msg.token) setStoredAuthToken(msg.token);
        showAuthPanel('none');
        return;
    }

    // A stored token that the device no longer recognizes (e.g. after an
    // auth reset via the BOOT button, or a password change from another
    // browser) — drop it and fall back to a normal login, rather than
    // looping forever on a dead token.
    setStoredAuthToken('');

    const loginError = document.getElementById('auth-login-error');
    const setupPanelVisible = !document.getElementById('auth-setup').classList.contains('hidden');
    if (setupPanelVisible) {
        const err = document.getElementById('auth-setup-error');
        err.innerText = 'Could not set password. Please try again.';
        err.classList.remove('hidden');
    } else {
        showAuthPanel('login');
        if (loginError) {
            loginError.innerText = 'Incorrect password. Please try again.';
            loginError.classList.remove('hidden');
        }
    }
}

// Handles the device's "change_password_result" frame (Settings > Change
// Password), sent in response to { command: "change_password", ... }.
function handleChangePasswordResult(msg) {
    const btn = document.getElementById('btn-change-password');
    const errEl = document.getElementById('cfg-pass-error');
    if (msg.ok) {
        if (msg.token) setStoredAuthToken(msg.token); // old token was just invalidated server-side
        if (errEl) errEl.classList.add('hidden');
        if (btn) {
            const original = 'Update Password';
            btn.innerText = 'Password Updated!';
            setTimeout(() => { btn.innerText = original; }, 2000);
        }
        ['cfg-pass-current', 'cfg-pass-new', 'cfg-pass-confirm'].forEach((id) => {
            const el = document.getElementById(id);
            if (el) el.value = '';
        });
    } else if (errEl) {
        errEl.innerText = msg.error || 'Could not update password.';
        errEl.classList.remove('hidden');
    }
}

// ============================================================================
// 2. WEBSOCKET & DATA HANDLING
// ============================================================================
let gateway = `ws://${window.location.hostname}/ws`;
let websocket;
let wsBackoff = 2000;
let wsConnectAttempt = 0;

// Fix (gap #4): the spinner used to just say "CONNECTING..." forever with no
// feedback about how long it had been retrying, and no way to force a retry
// sooner than the current backoff. This surfaces the attempt count once the
// backoff has clearly kicked in (a fast first reconnect is normal and not
// worth alarming anyone about) and offers a manual "Retry now" button.
function updateSpinnerStatus() {
    const label = document.getElementById('auth-spinner-status');
    const retryBtn = document.getElementById('auth-spinner-retry');
    if (!label) return;
    if (wsConnectAttempt <= 1) {
        label.innerText = '';
        if (retryBtn) retryBtn.classList.add('hidden');
    } else {
        label.innerText = `Still trying to connect… (attempt ${wsConnectAttempt})`;
        if (retryBtn) retryBtn.classList.remove('hidden');
    }
}

function initWebSocket() {
    // Every fresh connection starts unauthenticated — including reconnects —
    // so the spinner (and, once auth_status arrives, the Setup/Login modal)
    // reappears until this connection re-authenticates. This mirrors the
    // backend: authentication state lives per-WebSocket-connection, not per
    // browser tab, so a dropped/reconnected socket must prove itself again.
    showAuthPanel('spinner');
    wsConnectAttempt++;
    updateSpinnerStatus();
    websocket = new WebSocket(gateway);
    websocket.onopen = onOpen;
    websocket.onclose = onClose;
    websocket.onmessage = onMessage;
}

function onOpen(event) {
    wsBackoff = 2000;
    wsConnectAttempt = 0;
    updateSpinnerStatus();
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
    // Any save awaiting an ack can no longer receive one on this (now dead)
    // socket — reject them immediately instead of letting them time out.
    rejectAllPendingCommands('Connection lost before the device could confirm.');
    reconnectTimer = setTimeout(initWebSocket, wsBackoff);
    wsBackoff = Math.min(60000, wsBackoff * 2);
}

let reconnectTimer = null;

// Fix (gap #4): lets a user staring at a stuck spinner force an immediate
// retry instead of waiting out the current exponential-backoff delay.
function retryConnectionNow() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    if (websocket) { try { websocket.close(); } catch (e) { /* already closed */ } }
    initWebSocket();
}

// ------------------------------------------------------------------
// Fix (gap #1 - the biggest one): every "Saved!" button used to call
// websocket.send() and immediately show success, with no check that the
// socket was even open and no wait for the device to actually confirm it.
// During a reconnect window, send() on a CLOSED/CONNECTING socket either
// throws (swallowed, since there was no try/catch) or silently drops the
// frame — either way the button lied.
//
// sendCommand() replaces every raw websocket.send(JSON.stringify(...)) call
// used by a save/calibrate action. It:
//   1. Refuses to send at all if the socket isn't OPEN, returning a
//      rejected promise the caller can show an error for.
//   2. Tags the outgoing command with the same object the device echoes
//      back in its "command_result" ack (see sendCmdAck() in
//      task_network.cpp), and resolves/rejects the returned promise only
//      when that specific ack arrives.
//   3. Falls back to a timeout so a lost ack (e.g. the connection drops
//      mid-flight) doesn't leave a button stuck showing "Saving..." forever.
// ------------------------------------------------------------------
const ACK_TIMEOUT_MS = 5000;
// Commands that can legitimately take longer than the default ack timeout.
// test_firebase does up to two HTTPS round trips server-side (Identity
// Toolkit sign-in + a Firestore GET), each capped at 7s on the device — so
// the client timeout has to comfortably exceed that worst case, or a
// successful device-side check could still show up as "timed out" here.
const COMMAND_TIMEOUT_OVERRIDES = { test_firebase: 16000 };
let pendingCommands = []; // { id, command, resolve, reject, timer }
let nextPendingId = 1;
// Separate from pendingCommands above: reset_sensor_pin only acks on
// failure (see sendResetSensorPin() for the full reasoning), so it can't
// use the same "every command eventually gets an ack" assumption that
// sendCommand()/pendingCommands relies on. Small array of one-shot
// listener functions instead.
let resetSensorPinListeners = [];

function rejectAllPendingCommands(reason) {
    pendingCommands.forEach((p) => { clearTimeout(p.timer); p.reject(new Error(reason)); });
    pendingCommands = [];
}

// The device's command_result ack only echoes back the command name (see
// sendCmdAck() in command_handlers.cpp), not a per-request id — so if two
// commands of the SAME type are ever in flight at once, there's no way to
// tell which ack belongs to which from the wire alone. We match the oldest
// still-pending entry for that command name (FIFO — the ack for a command
// almost always arrives before the ack for one sent later), which is the
// best a client can do without protocol changes. sendCommand() below closes
// the actual race this used to cause by refusing to send a second copy of
// the same command while one is already pending, rather than leaving two
// ambiguous entries in the queue at once.
function handleCommandResult(msg) {
    // reset_sensor_pin listeners (see sendResetSensorPin()) are separate
    // from pendingCommands below — dispatch to them first. Copy the array
    // before iterating since a listener removes itself from the live array
    // as its first action, which would otherwise skip entries mid-forEach.
    if (msg.command === "reset_sensor_pin" && resetSensorPinListeners.length > 0) {
        resetSensorPinListeners.slice().forEach((fn) => fn(msg));
    }

    const idx = pendingCommands.findIndex((p) => p.command === msg.command);
    if (idx === -1) return; // no button waiting on this ack (or it already timed out)
    const pending = pendingCommands[idx];
    pendingCommands.splice(idx, 1);
    clearTimeout(pending.timer);
    if (msg.ok) pending.resolve(msg);
    else pending.reject(new Error(msg.error || 'Device rejected the command.'));
}

function sendCommand(payload) {
    return new Promise((resolve, reject) => {
        if (!websocket || websocket.readyState !== WebSocket.OPEN) {
            reject(new Error('Not connected to the device right now.'));
            return;
        }
        // Refuse a second copy of the same command while one is already
        // awaiting its ack — without this, two same-type commands in
        // flight together are indistinguishable once the ack comes back
        // (see the note on handleCommandResult() above), and a timeout on
        // either one used to be able to wipe out the OTHER's pending entry
        // too (a plain command-name filter removed every match, not just
        // the one that timed out), permanently stalling its promise.
        if (pendingCommands.some((p) => p.command === payload.command)) {
            reject(new Error('A previous request for this action is still in progress.'));
            return;
        }
        try {
            websocket.send(JSON.stringify(payload));
        } catch (e) {
            reject(e);
            return;
        }
        const id = nextPendingId++;
        const timeoutMs = COMMAND_TIMEOUT_OVERRIDES[payload.command] || ACK_TIMEOUT_MS;
        const timer = setTimeout(() => {
            pendingCommands = pendingCommands.filter((p) => p.id !== id);
            reject(new Error('No response from the device — it may be offline.'));
        }, timeoutMs);
        pendingCommands.push({ id, command: payload.command, resolve, reject, timer });
    });
}

// Shared success/failure UI for a save button: shows the normal transient
// "Saved!" state only once the device actually confirms, and an inline error
// state (auto-reverting) if the command was rejected, dropped, or timed out.
function runSaveButton(btn, payload, savedText, idleText) {
    if (!btn) { sendCommand(payload).catch(() => {}); return; }
    const original = idleText || btn.innerText;
    btn.disabled = true;
    btn.innerText = 'Saving…';
    sendCommand(payload).then(() => {
        btn.disabled = false;
        btn.innerText = savedText || 'Saved!';
        setTimeout(() => { btn.innerText = original; }, 2000);
    }).catch((err) => {
        btn.disabled = false;
        btn.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
        setTimeout(() => { btn.innerText = original; }, 3000);
    });
}

function onMessage(event) {
    let msg;
    try { msg = JSON.parse(event.data); } catch (e) { return; }

    if (msg.type === "auth_status") handleAuthStatus(msg);
    else if (msg.type === "auth_result") handleAuthResult(msg);
    else if (msg.type === "change_password_result") handleChangePasswordResult(msg);
    else if (msg.type === "command_result") handleCommandResult(msg);
    else if (msg.type === "vitals") updateVitals(msg);
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

    // Fix (gap #2): surface wifi_status ("connected" | "ap_mode") — the
    // backend has always sent this, but nothing read it, so a user on the
    // HyGrow-Setup fallback AP had no way to tell from the dashboard.
    const wifiModeEl = document.getElementById('dash-wifi-mode');
    const wifiModeTextEl = document.getElementById('dash-wifi-mode-text');
    if (wifiModeEl && wifiModeTextEl) {
        const onFallbackAp = msg.wifi_status === 'ap_mode';
        wifiModeEl.classList.toggle('hidden', !onFallbackAp);
        wifiModeEl.classList.toggle('flex', onFallbackAp);
        wifiModeEl.classList.toggle('text-error', onFallbackAp);
        if (onFallbackAp) {
            wifiModeTextEl.innerText = 'On setup network — go to Settings → Network to connect Wi-Fi';
        }
    }

    // Fix (gap #3): surface Firebase/Firestore upload health
    // (firebase_ready, firebase_last_ok_ms, firebase_last_error) — all
    // already sent by the backend, none of it previously shown anywhere, so
    // uploads could fail silently forever with zero visibility here.
    const fbDot = document.getElementById('dash-fb-status-dot');
    const fbText = document.getElementById('dash-fb-status-text');
    if (fbDot && fbText) {
        fbDot.classList.remove('bg-white/30', 'bg-secondary', 'bg-error', 'animate-pulse');
        if (msg.firebase_ready) {
            fbDot.classList.add('bg-secondary');
            // Both firebase_last_ok_ms and the device's uptime are millis()
            // timestamps from the same clock, so (uptime_ms - last_ok_ms)
            // gives elapsed time since the last successful upload without
            // needing the browser's clock at all.
            const secsAgo = msg.firebase_last_ok_ms ? Math.max(0, Math.floor((u * 1000 - msg.firebase_last_ok_ms) / 1000)) : null;
            if (secsAgo !== null && !isNaN(secsAgo)) {
                const mins = Math.floor(secsAgo / 60);
                fbText.innerText = mins > 0 ? `Last upload: ${mins}m ago` : `Last upload: ${secsAgo}s ago`;
            } else {
                fbText.innerText = 'Uploading normally';
            }
        } else if (msg.firebase_last_error && msg.firebase_last_error.length > 0) {
            fbDot.classList.add('bg-error');
            fbText.innerText = `Last error: ${msg.firebase_last_error}`;
        } else {
            fbDot.classList.add('bg-white/30');
            fbText.innerText = 'Never uploaded — check credentials, or enable Firebase Upload in Feature Flags';
        }
    }
}

// Colors one dashboard tile's status dot from an s_ok[] code (0=disabled,
// 1=healthy, 2=enabled-but-failing). Mirrors the existing fbDot convention
// above (updateVitals): bg-secondary+animate-pulse = live, bg-error = failing,
// bg-white/30 (no pulse) = off/disabled. `code` may be undefined if s_ok[]
// wasn't sent yet (e.g. before the first "data" frame) — treated as healthy
// so a tile doesn't flash "disabled" for a moment before real data lands.
function setDashDotStatus(dotId, code) {
    const dot = document.getElementById(dotId);
    if (!dot) return;
    dot.classList.remove('bg-white/30', 'bg-secondary', 'bg-error', 'animate-pulse');
    if (code === 0) {
        dot.classList.add('bg-white/30');
        dot.title = 'Sensor disabled';
    } else if (code === 2) {
        dot.classList.add('bg-error');
        dot.title = 'Sensor enabled but not reading — check wiring/Terminal log';
    } else {
        dot.classList.add('bg-secondary', 'animate-pulse');
        dot.title = 'Live';
    }
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

    // Color each dashboard tile's status dot from msg.s_ok[] (see S_EN_INDEX
    // below for the SensorID-order mapping; VPD has no sensor of its own —
    // it's derived from DHT temp+humidity, so it mirrors DHT's status).
    // Dots default to "live" green in the HTML, so this only needs to
    // override that when a sensor is actually disabled or failing.
    if (Array.isArray(msg.s_ok)) {
        setDashDotStatus('dash-dot-tds', msg.s_ok[S_EN_INDEX.tds]);
        setDashDotStatus('dash-dot-ph', msg.s_ok[S_EN_INDEX.ph]);
        setDashDotStatus('dash-dot-atemp', msg.s_ok[S_EN_INDEX.dht]);
        setDashDotStatus('dash-dot-hum', msg.s_ok[S_EN_INDEX.dht]);
        setDashDotStatus('dash-dot-wtemp', msg.s_ok[S_EN_INDEX.wt]);
        setDashDotStatus('dash-dot-lux', msg.s_ok[S_EN_INDEX.light]);
        setDashDotStatus('dash-dot-wl', msg.s_ok[S_EN_INDEX.wl]);
        setDashDotStatus('dash-dot-vpd', msg.s_ok[S_EN_INDEX.dht]);

        // Mirror the same signal into tabsData.ok[], parallel to the existing
        // tabsData.enabled[], so the per-sensor detail page banner (switchTab)
        // can also distinguish "disabled" from "enabled but not reading"
        // instead of only checking enabled state.
        tabsData.ok[1] = msg.s_ok[S_EN_INDEX.tds];
        tabsData.ok[2] = msg.s_ok[S_EN_INDEX.dht];
        tabsData.ok[3] = msg.s_ok[S_EN_INDEX.wt];
        tabsData.ok[4] = msg.s_ok[S_EN_INDEX.light];
        tabsData.ok[5] = msg.s_ok[S_EN_INDEX.wl];
        tabsData.ok[6] = msg.s_ok[S_EN_INDEX.ph];
    }

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

// Tab index -> short sensor id used by save_sensor_enabled/reset_sensor_pin.
// Module-scope (not just inside DOMContentLoaded) so both the per-sensor
// detail page's power toggle (handleToggle()) and the "Reset" button
// (btn-reset-current-sensor) share this one mapping instead of each
// maintaining their own copy.
const TAB_TO_SENSOR_ID = { 1: "tds", 2: "dht", 3: "wt", 4: "light", 5: "wl", 6: "ph" };

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

    // Duplicate pin assignments across sensors (Part 5.5). A negative value
    // is never a real GPIO number (fields are always populated with a valid
    // pin by the device, this is just a defensive skip) and never conflicts
    // with anything.
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

// Client-side form validation (Part 2.3 / Forms) — stop users from saving
// an empty Wi-Fi name or a Firebase Project ID that isn't shaped like a
// real one. Module-scope for the same reason as validateAllPinFields()
// above: updateConfigForm() re-runs these after a fresh "config" WS frame
// repopulates the fields. The real safety boundary is server-side
// (save_wifi/save_firebase in command_handlers.cpp) — this is a UX nicety
// that fails fast without a round trip.
const FIREBASE_PROJECT_ID_RE = /^[a-z0-9-]{6,30}$/;

function validateWifiForm() {
    const ssidEl = document.getElementById('cfg-wifi-ssid');
    const err = document.getElementById('cfg-wifi-error');
    if (!ssidEl) return true;
    const problem = ssidEl.value.trim().length === 0;
    ssidEl.classList.toggle('border-error', problem);
    ssidEl.classList.toggle('text-error', problem);
    if (err) {
        err.innerText = 'Network name (SSID) cannot be empty.';
        err.classList.toggle('hidden', !problem);
    }
    return !problem;
}

// Validates the optional SoftAP recovery password field. Blank means "leave
// the current one unchanged" (never a problem — this mirrors how
// cfg-wifi-pass/cfg-fb-pass work, since passwords are never sent back down
// from the device, so there's nothing to show as "current"). Only a
// non-empty value under 8 characters is a problem, matching the server-side
// WPA2 minimum enforced in save_wifi (command_handlers.cpp).
function validateApPassField() {
    const apPassEl = document.getElementById('cfg-ap-pass');
    const err = document.getElementById('cfg-ap-pass-error');
    if (!apPassEl) return true;
    const val = apPassEl.value;
    const problem = val.length > 0 && val.length < 8;
    apPassEl.classList.toggle('border-error', problem);
    apPassEl.classList.toggle('text-error', problem);
    if (err) {
        err.innerText = 'SoftAP recovery password must be at least 8 characters.';
        err.classList.toggle('hidden', !problem);
    }
    return !problem;
}

function validateFirebaseForm() {
    const projEl = document.getElementById('cfg-fb-proj');
    const err = document.getElementById('cfg-fb-error');
    if (!projEl) return true;
    const val = projEl.value.trim();
    // Empty is allowed — that's how Firebase provisioning gets cleared.
    // Only a non-empty value has to look like a real project ID (lowercase
    // letters/digits/hyphens, 6-30 chars, no leading/trailing hyphen —
    // Google's own Firebase project ID rules).
    const problem = val.length > 0 && (!FIREBASE_PROJECT_ID_RE.test(val) || val.startsWith('-') || val.endsWith('-'));
    projEl.classList.toggle('border-error', problem);
    projEl.classList.toggle('text-error', problem);
    if (err) {
        err.innerText = 'Invalid Project ID. Use 6-30 lowercase letters, digits, or hyphens (no leading/trailing hyphen).';
        err.classList.toggle('hidden', !problem);
    }
    return !problem;
}

function updateConfigForm(msg) {
    globalConfigCache = msg; // Cache for CSV export

    if(document.getElementById('cfg-wifi-ssid')) document.getElementById('cfg-wifi-ssid').value = msg.wifi_ssid || "";
    if(document.getElementById('cfg-fb-proj')) document.getElementById('cfg-fb-proj').value = msg.fb_proj || "";
    if(document.getElementById('cfg-fb-api')) document.getElementById('cfg-fb-api').value = msg.fb_api || "";
    if(document.getElementById('cfg-fb-email')) document.getElementById('cfg-fb-email').value = msg.fb_email || "";
    if(document.getElementById('cfg-fb-col')) document.getElementById('cfg-fb-col').value = msg.fb_col || "";

    // Re-run form validation now that fresh values landed in these fields —
    // same reasoning as the pin-field re-validation below.
    if (typeof validateWifiForm === 'function') validateWifiForm();
    if (typeof validateApPassField === 'function') validateApPassField();
    if (typeof validateFirebaseForm === 'function') validateFirebaseForm();

    // Note: raw pH offset/slope and TDS K-factor are no longer shown as
    // editable fields — the guided calibration wizard (btn-cal-ph-7/4/save,
    // btn-cal-tds below) replaced them. The values still arrive in every
    // config frame and are read straight from globalConfigCache by the
    // wizard's math and the CSV export, so nothing here needs to write them
    // into the DOM.

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

        // Re-gate the Live Calibration page (TDS card / pH wizard) any time
        // enabled state changes -- including right after a save_sensor_enabled
        // reboot, so this page never has to be manually revisited to notice
        // a sensor came back on/off. See updateCalibrationGating() below.
        updateCalibrationGating();
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

// Escapes text that will be inserted into innerHTML so device-supplied
// strings (log messages, sensor names echoed back, etc.) are always
// rendered as plain text and never parsed as markup. Used anywhere a
// WS-sourced string is interpolated into innerHTML across this file —
// see updateTerminal() and the terminal-log lines in handleToggle() /
// the sensor-enable handler below.
function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = String(str);
    return div.innerHTML;
}

function updateTerminal(msg) {
    if (isTerminalPaused) return;
    const term = document.getElementById('terminal-output');
    if(!term) return;
    if(term.children.length > 100) term.removeChild(term.firstChild);

    const log = document.createElement('div');
    const colorClass = msg.core === 0 ? "log-core-0" : "log-core-1";
    const levelClass = msg.level === "error" ? "text-error font-bold" : (msg.level === "warn" ? "text-secondary" : "");
    log.innerHTML = `<span class="${colorClass} opacity-80">[CORE ${msg.core}]</span> <span class="${levelClass}">${escapeHtml(msg.msg)}</span>`;
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

    // Default to "disabled" banners showing until the first config frame's
    // tabsData.enabled[] confirms otherwise -- matches resolveSensorOn()'s
    // same fail-closed default for the per-sensor toggle before any config
    // has arrived.
    updateCalibrationGating();

    const btnSpinnerRetry = document.getElementById('auth-spinner-retry');
    if (btnSpinnerRetry) btnSpinnerRetry.addEventListener('click', retryConnectionNow);

    // ------------------------------------------------------------------
    // hg-secret-field reveal toggles — one handler wires every eye-icon
    // button in the app (Web API Key, Firebase password, admin current/
    // new/confirm password). Each button's data-reveal-target points at
    // the input id it controls; toggling swaps type password<->text and
    // swaps the Material icon between "visibility" and "visibility_off".
    // ------------------------------------------------------------------
    document.querySelectorAll('.hg-secret-toggle').forEach((btn) => {
        btn.addEventListener('click', () => {
            const targetId = btn.getAttribute('data-reveal-target');
            const input = targetId && document.getElementById(targetId);
            if (!input) return;
            const icon = btn.querySelector('.material-symbols-outlined');
            const revealed = input.type === 'text';
            input.type = revealed ? 'password' : 'text';
            if (icon) icon.textContent = revealed ? 'visibility' : 'visibility_off';
            btn.setAttribute('aria-label', (revealed ? 'Show ' : 'Hide ') + (btn.getAttribute('aria-label') || '').replace(/^(Show|Hide) /, ''));
        });
    });

    // ------------------------------------------------------------------
    // Auth overlay button bindings. The "auth" command's response
    // (auth_result) is handled centrally in onMessage()/handleAuthResult()
    // above — these handlers only send the request and do basic client-side
    // validation (non-empty, matching confirm field) before sending.
    // ------------------------------------------------------------------
    const btnAuthSetup = document.getElementById('btn-auth-setup');
    if (btnAuthSetup) {
        btnAuthSetup.addEventListener('click', () => {
            const pass = document.getElementById('auth-setup-pass').value;
            const confirmPass = document.getElementById('auth-setup-pass-confirm').value;
            const err = document.getElementById('auth-setup-error');

            if (!pass) {
                err.innerText = 'Password cannot be empty.';
                err.classList.remove('hidden');
                return;
            }
            if (pass !== confirmPass) {
                err.innerText = 'Passwords do not match.';
                err.classList.remove('hidden');
                return;
            }
            if (!websocket || websocket.readyState !== WebSocket.OPEN) {
                err.innerText = 'Not connected to the device right now — try again in a moment.';
                err.classList.remove('hidden');
                return;
            }
            err.classList.add('hidden');
            websocket.send(JSON.stringify({ command: "auth", password: pass }));
        });
    }

    const btnAuthLogin = document.getElementById('btn-auth-login');
    const submitLogin = () => {
        const pass = document.getElementById('auth-login-pass').value;
        const err = document.getElementById('auth-login-error');
        if (!pass) {
            err.innerText = 'Please enter your password.';
            err.classList.remove('hidden');
            return;
        }
        if (!websocket || websocket.readyState !== WebSocket.OPEN) {
            err.innerText = 'Not connected to the device right now — try again in a moment.';
            err.classList.remove('hidden');
            return;
        }
        err.classList.add('hidden');
        websocket.send(JSON.stringify({ command: "auth", password: pass }));
    };
    if (btnAuthLogin) btnAuthLogin.addEventListener('click', submitLogin);
    // Enter-to-submit on both password fields, for a login flow that doesn't
    // require reaching for the mouse.
    const authLoginPassField = document.getElementById('auth-login-pass');
    if (authLoginPassField) authLoginPassField.addEventListener('keydown', (e) => { if (e.key === 'Enter') submitLogin(); });
    const authSetupConfirmField = document.getElementById('auth-setup-pass-confirm');
    if (authSetupConfirmField) authSetupConfirmField.addEventListener('keydown', (e) => { if (e.key === 'Enter' && btnAuthSetup) btnAuthSetup.click(); });

    // Settings > Change Password
    const btnChangePassword = document.getElementById('btn-change-password');
    if (btnChangePassword) {
        btnChangePassword.addEventListener('click', () => {
            const current = document.getElementById('cfg-pass-current').value;
            const next = document.getElementById('cfg-pass-new').value;
            const confirmNext = document.getElementById('cfg-pass-confirm').value;
            const err = document.getElementById('cfg-pass-error');

            if (!current || !next) {
                err.innerText = 'Please fill in all fields.';
                err.classList.remove('hidden');
                return;
            }
            if (next !== confirmNext) {
                err.innerText = 'New passwords do not match.';
                err.classList.remove('hidden');
                return;
            }
            if (!websocket || websocket.readyState !== WebSocket.OPEN) {
                err.innerText = 'Not connected to the device right now — try again in a moment.';
                err.classList.remove('hidden');
                return;
            }
            err.classList.add('hidden');
            websocket.send(JSON.stringify({ command: "change_password", current: current, new_pass: next }));
        });
    }

    // Client-side form validation (Part 2.3 / Forms) — stop users from
    // saving an empty Wi-Fi name or a Firebase Project ID that isn't shaped
    // like a real one. validateWifiForm()/validateFirebaseForm() themselves
    // are defined at module scope (near validateAllPinFields) so
    // updateConfigForm() can also re-run them after a fresh "config" frame
    // repopulates these fields — just wire up the live listeners here.
    const wifiSsidInput = document.getElementById('cfg-wifi-ssid');
    if (wifiSsidInput) {
        wifiSsidInput.addEventListener('input', validateWifiForm);
        wifiSsidInput.addEventListener('change', validateWifiForm);
    }
    const apPassInput = document.getElementById('cfg-ap-pass');
    if (apPassInput) {
        apPassInput.addEventListener('input', validateApPassField);
        apPassInput.addEventListener('change', validateApPassField);
    }
    const fbProjInput = document.getElementById('cfg-fb-proj');
    if (fbProjInput) {
        fbProjInput.addEventListener('input', validateFirebaseForm);
        fbProjInput.addEventListener('change', validateFirebaseForm);
    }

    const btnSaveWifi = document.getElementById('btn-save-wifi');
    if(btnSaveWifi) {
        btnSaveWifi.addEventListener('click', () => {
            if (!validateWifiForm() || !validateApPassField()) return;
            const apPassEl = document.getElementById('cfg-ap-pass');
            const newApPass = apPassEl ? apPassEl.value : '';
            const payload = {
                command: "save_wifi",
                ssid: document.getElementById('cfg-wifi-ssid').value,
                pass: document.getElementById('cfg-wifi-pass').value
            };
            if (newApPass) payload.ap_pass = newApPass; // omit entirely when blank — server keeps the current one
            sendCommand(payload).then(() => {
                btnSaveWifi.innerText = "Saved!";
                setTimeout(() => { btnSaveWifi.innerText = "Update Network"; }, 2000);
                if (apPassEl) apPassEl.value = ''; // never leave a saved password sitting in the field
                // Fix (gap #6): if the new credentials are wrong, the device
                // safely falls back to its HyGrow-Setup SoftAP after ~15s
                // (see initNetworkTask() in task_network.cpp) — but this
                // browser tab has no way to follow it there, since its IP
                // changes. Tell the user what to do before they're staring at
                // a dead "OFFLINE" indicator with no explanation. If the AP
                // password was just changed too, say so explicitly — the OLD
                // one won't get them back into the recovery network anymore.
                const apPassNote = newApPass
                    ? "\n\nNote: you also just changed the SoftAP recovery password — use the NEW one, not the old one, when reconnecting."
                    : "";
                if(confirm("Wi-Fi credentials saved. The ESP32 must reboot to connect with the new credentials. Reboot now?\n\nIf these credentials turn out to be wrong, the device will automatically fall back to its own \"HyGrow-Setup\" Wi-Fi network after about 15 seconds — reconnect to that network and browse to 192.168.4.1 to try again." + apPassNote)) {
                    sendCommand({command: "reboot"}).catch(() => {});
                }
            }).catch((err) => {
                btnSaveWifi.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnSaveWifi.innerText = "Update Network"; }, 3000);
            });
        });
    }

    const btnSaveFb = document.getElementById('btn-save-firebase');
    if(btnSaveFb) {
        btnSaveFb.addEventListener('click', () => {
            if (!validateFirebaseForm()) return;
            const payload = {
                command: "save_firebase",
                proj: document.getElementById('cfg-fb-proj').value,
                api: document.getElementById('cfg-fb-api').value,
                email: document.getElementById('cfg-fb-email').value,
                pass: document.getElementById('cfg-fb-pass').value,
                col: document.getElementById('cfg-fb-col').value
            };
            runSaveButton(btnSaveFb, payload, "Credentials Saved", "Save Credentials");
        });
    }

    // Test Connection — real check (test_firebase command) against whatever
    // is currently SAVED on the device, not whatever is currently typed in
    // the form. Signs in via Identity Toolkit and does a live Firestore read
    // (see firebaseTestConnection() in firebase.cpp). Uses its own longer
    // ack timeout via COMMAND_TIMEOUT_OVERRIDES since it's a live network
    // round trip on the device, not an instant NVS write.
    const btnTestFb = document.getElementById('btn-test-firebase');
    if (btnTestFb) {
        btnTestFb.addEventListener('click', () => {
            const original = 'Test Connection';
            btnTestFb.disabled = true;
            btnTestFb.innerText = 'Testing…';
            sendCommand({ command: "test_firebase" }).then(() => {
                btnTestFb.disabled = false;
                btnTestFb.innerText = 'Connected ✓';
                setTimeout(() => { btnTestFb.innerText = original; }, 3000);
            }).catch((err) => {
                btnTestFb.disabled = false;
                btnTestFb.innerText = 'Failed — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnTestFb.innerText = original; }, 4000);
            });
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
            const original = btnSavePins.innerText;
            btnSavePins.disabled = true;
            btnSavePins.innerText = 'Saving…';
            sendCommand(payload).then(() => {
                btnSavePins.disabled = false;
                btnSavePins.innerText = original;
                if(confirm("Pinout saved. The ESP32 must reboot to reassign hardware interrupts safely. Reboot now?")) {
                    sendCommand({command: "reboot"}).catch(() => {});
                }
            }).catch((err) => {
                btnSavePins.disabled = false;
                btnSavePins.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnSavePins.innerText = original; }, 3000);
            });
        });
    }

    // ------------------------------------------------------------------
    // TDS Calibration — reject impossible values (Part 2.2). -100 ppm or
    // 999999 ppm used to sail straight through to calibrate_tds and wreck
    // the tds_k scale factor for every future reading. Bounds mirror the
    // server-side check in calibrate_tds (command_handlers.cpp).
    // ------------------------------------------------------------------
    const TDS_TARGET_MIN = 0;
    const TDS_TARGET_MAX = 10000;

    function validateTdsTarget() {
        const input = document.getElementById('cfg-tds-target');
        const err = document.getElementById('cfg-tds-target-error');
        const btn = document.getElementById('btn-cal-tds');
        if (!input) return true;

        const v = parseFloat(input.value);
        const problem = isNaN(v) || v < TDS_TARGET_MIN || v > TDS_TARGET_MAX;

        input.classList.toggle('border-error', problem);
        input.classList.toggle('text-error', problem);
        if (err) err.classList.toggle('hidden', !problem);
        if (btn) btn.disabled = problem;

        return !problem;
    }

    const tdsTargetInput = document.getElementById('cfg-tds-target');
    if (tdsTargetInput) {
        tdsTargetInput.addEventListener('input', validateTdsTarget);
        tdsTargetInput.addEventListener('change', validateTdsTarget);
        validateTdsTarget(); // initial pass
    }

    const btnCalTds = document.getElementById('btn-cal-tds');
    if(btnCalTds) {
        btnCalTds.addEventListener('click', () => {
            if (!tabsData.enabled[1]) { updateCalibrationGating(); return; }
            if (!validateTdsTarget()) return;

            const targetPpm = parseFloat(document.getElementById('cfg-tds-target').value);
            const currentPpm = parseFloat(document.getElementById('cal-tds-raw').innerText);
            if (isNaN(targetPpm) || isNaN(currentPpm) || currentPpm === 0) {
                alert("Invalid TDS readings");
                return;
            }
            const currentK = globalConfigCache.tds_k || 1.0;
            const newK = currentK * (targetPpm / currentPpm);

            // target_ppm rides along so the server can reject an impossible
            // target directly, not just the derived K-factor (see
            // calibrate_tds in command_handlers.cpp).
            const payload = { command: "calibrate_tds", tds_k: parseFloat(newK.toFixed(2)), target_ppm: targetPpm };
            runSaveButton(btnCalTds, payload, "Saved!", "Calibrate & Save");
        });
    }

    // ------------------------------------------------------------------
    // pH Calibration Wizard (Part 2.1) — Step 1 (capture pH 7) -> Step 2
    // (capture pH 4) -> Step 3 (review + save). Each step is only reachable
    // once the previous one is complete; going back resets the steps ahead
    // of it so a stale half-finished attempt can't be silently saved.
    // A beforeunload warning fires whenever calibration is in progress
    // (Step 1 started but not yet saved) so a wayward tab-close/refresh/nav
    // doesn't silently lose a mid-calibration reading.
    // ------------------------------------------------------------------
    let ph7Volt = null;
    let ph4Volt = null;
    let phWizardDirty = false; // true once Step 1 starts, false again after a successful save (or a full reset)

    function phWizardBeforeUnload(e) {
        if (!phWizardDirty) return;
        e.preventDefault();
        e.returnValue = ''; // required for the native "leave site?" prompt in most browsers
        return '';
    }
    window.addEventListener('beforeunload', phWizardBeforeUnload);

    function setPhStepUI(step) {
        // step: 1, 2, or 3 — which panel is visible and how the progress
        // dots/bars above it read.
        const panels = { 1: document.getElementById('ph-step-1'), 2: document.getElementById('ph-step-2'), 3: document.getElementById('ph-step-3') };
        Object.keys(panels).forEach((k) => {
            const el = panels[k];
            if (!el) return;
            const show = Number(k) === step;
            el.classList.toggle('hidden', !show);
            el.classList.toggle('flex', show);
        });

        for (let i = 1; i <= 3; i++) {
            const dot = document.getElementById(`ph-step-dot-${i}`);
            if (dot) {
                const done = i < step;
                const active = i === step;
                dot.classList.toggle('bg-secondary', done || active);
                dot.classList.toggle('text-on-secondary', done || active);
                dot.classList.toggle('bg-white/10', !(done || active));
                dot.classList.toggle('text-on-surface-variant', !(done || active));
                dot.innerText = done ? '✓' : String(i);
            }
            const bar = document.getElementById(`ph-step-bar-${i}`);
            if (bar) bar.style.width = (i < step) ? '100%' : '0%';
        }
    }

    function resetPhWizard() {
        ph7Volt = null;
        ph4Volt = null;
        phWizardDirty = false;
        setPhStepUI(1);
    }

    // Shows the "sensor is disabled" banner and hides the interactive
    // controls on the Live Calibration page for whichever of TDS/pH is
    // currently off, instead of letting the wizard/button run against a
    // sensor whose currentSensors value the firmware never touches (see
    // the banner comments in index.html). Called on init and any time a
    // config frame updates tabsData.enabled[].
    function updateCalibrationGating() {
        const tdsEnabled = !!tabsData.enabled[1];
        const phEnabled = !!tabsData.enabled[6];

        const tdsBanner = document.getElementById('cal-tds-disabled-banner');
        const tdsControls = document.getElementById('cal-tds-controls');
        if (tdsBanner) tdsBanner.classList.toggle('hidden', tdsEnabled);
        if (tdsControls) tdsControls.classList.toggle('hidden', !tdsEnabled);

        const phBanner = document.getElementById('cal-ph-disabled-banner');
        const phControls = document.getElementById('ph-wizard-controls');
        if (phBanner) phBanner.classList.toggle('hidden', phEnabled);
        if (phControls) phControls.classList.toggle('hidden', !phEnabled);

        // A sensor going from enabled to disabled mid-wizard (another tab
        // toggled it, or a reboot from an unrelated save) invalidates
        // whatever's in progress -- reset back to Step 1 so a later
        // re-enable never lets Step 3 "Save" fire off stale captured volts.
        if (!phEnabled && typeof resetPhWizard === 'function') resetPhWizard();
    }

    const btnCalPh7 = document.getElementById('btn-cal-ph-7');
    if(btnCalPh7) {
        btnCalPh7.addEventListener('click', () => {
            // Belt-and-suspenders: the wizard controls are hidden behind
            // cal-ph-disabled-banner while pH is off (updateCalibrationGating()),
            // but guard the handler itself too in case this fires from a
            // stale click queued just before the sensor was disabled.
            if (!tabsData.enabled[6]) { updateCalibrationGating(); return; }
            const livePh = parseFloat(document.getElementById('cal-ph-raw').innerText);
            if (isNaN(livePh)) { alert("No live pH reading yet — make sure the pH sensor is enabled and the probe is connected."); return; }
            const off = globalConfigCache.ph_off || 0.0;
            const slope = globalConfigCache.ph_slope || 1.0;
            ph7Volt = (livePh - off) / slope;
            phWizardDirty = true;
            document.querySelectorAll('#cal-ph-7-val').forEach((el) => { el.innerText = ph7Volt.toFixed(3) + " V"; });
            setPhStepUI(2);
        });
    }

    const btnCalPh4 = document.getElementById('btn-cal-ph-4');
    if(btnCalPh4) {
        btnCalPh4.addEventListener('click', () => {
            if (!tabsData.enabled[6]) { updateCalibrationGating(); return; }
            const livePh = parseFloat(document.getElementById('cal-ph-raw').innerText);
            if (isNaN(livePh)) { alert("No live pH reading yet — make sure the pH sensor is enabled and the probe is connected."); return; }
            if (ph7Volt === null) { setPhStepUI(1); return; } // shouldn't happen, but don't let Step 2 run without Step 1
            const off = globalConfigCache.ph_off || 0.0;
            const slope = globalConfigCache.ph_slope || 1.0;
            ph4Volt = (livePh - off) / slope;

            if (ph4Volt === ph7Volt) {
                alert("The 4.0 reading matches the 7.0 reading exactly — the probe may still be in the first solution. Rinse it and place it in the pH 4.0 buffer before capturing.");
                ph4Volt = null;
                return;
            }

            document.querySelectorAll('#cal-ph-4-val').forEach((el) => { el.innerText = ph4Volt.toFixed(3) + " V"; });
            const review7 = document.getElementById('ph-review-7');
            const review4 = document.getElementById('ph-review-4');
            if (review7) review7.innerText = ph7Volt.toFixed(3) + " V";
            if (review4) review4.innerText = ph4Volt.toFixed(3) + " V";
            setPhStepUI(3);
        });
    }

    const btnCalPhRestart1 = document.getElementById('btn-cal-ph-restart-1');
    if (btnCalPhRestart1) btnCalPhRestart1.addEventListener('click', () => { ph7Volt = null; setPhStepUI(1); });

    const btnCalPhRestart2 = document.getElementById('btn-cal-ph-restart-2');
    if (btnCalPhRestart2) btnCalPhRestart2.addEventListener('click', () => { ph4Volt = null; setPhStepUI(2); });

    const btnCalPhSave = document.getElementById('btn-cal-ph-save');
    if(btnCalPhSave) {
        btnCalPhSave.addEventListener('click', () => {
            if (ph7Volt === null || ph4Volt === null || ph7Volt === ph4Volt) {
                alert("Please complete both Step 1 (pH 7.0) and Step 2 (pH 4.0) before saving.");
                return;
            }
            const newSlope = (7.0 - 4.0) / (ph7Volt - ph4Volt);
            const newOff = 7.0 - (newSlope * ph7Volt);

            const payload = {
                command: "calibrate_ph",
                offset: parseFloat(newOff.toFixed(2)),
                slope: parseFloat(newSlope.toFixed(2))
            };
            btnCalPhSave.disabled = true;
            const original = btnCalPhSave.innerText;
            btnCalPhSave.innerText = 'Saving…';
            sendCommand(payload).then(() => {
                btnCalPhSave.disabled = false;
                btnCalPhSave.innerText = 'Saved!';
                // Calibration is now safely persisted — clear the "in
                // progress" flag so leaving the page no longer warns, then
                // reset the wizard back to Step 1 for the next run.
                phWizardDirty = false;
                setTimeout(() => { btnCalPhSave.innerText = original; resetPhWizard(); }, 2000);
            }).catch((err) => {
                btnCalPhSave.disabled = false;
                btnCalPhSave.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnCalPhSave.innerText = original; }, 3000);
            });
        });
    }


    const btnReboot = document.getElementById('btn-reboot');
    if(btnReboot) btnReboot.addEventListener('click', () => {
        if(!confirm("Reboot the device?")) return;
        // Deliberately NOT routed through sendCommand(): reboot's handler
        // (command_handlers.cpp) calls ESP.restart() directly with no
        // sendCmdAck() on the success path — the device is gone before it
        // could send one. Waiting on an ack here would time out on every
        // single successful reboot and show a false "failed" error.
        if (!websocket || websocket.readyState !== WebSocket.OPEN) { alert("Not connected to the device right now."); return; }
        websocket.send(JSON.stringify({command: "reboot"}));
    });

    const btnReset = document.getElementById('btn-factory-reset');
    if(btnReset) btnReset.addEventListener('click', () => {
        // A single confirm() popup is one accidental click away from wiping
        // every setting on the device (Wi-Fi, Firebase, calibration, pins,
        // admin password — everything in state_factory_reset()). Requiring
        // the user to type the exact word "RESET" is a much stronger,
        // harder-to-fat-finger gate than a Yes/No dialog, while still being
        // a client-side UX safeguard rather than a security boundary (the
        // device itself has no way to know what the browser prompted with).
        const typed = prompt("This will permanently erase ALL settings on this device — Wi-Fi, Firebase credentials, calibration, pin assignments, and the admin password.\n\nThis cannot be undone.\n\nType RESET (all caps) to confirm:");
        if (typed === null) return; // user cancelled
        if (typed !== "RESET") {
            alert("Factory reset cancelled — you must type RESET exactly.");
            return;
        }
        // Same reasoning as btn-reboot above: state_factory_reset() also
        // restarts the device with no ack on the way out, so this stays a
        // raw send rather than going through sendCommand().
        if (!websocket || websocket.readyState !== WebSocket.OPEN) { alert("Not connected to the device right now."); return; }
        websocket.send(JSON.stringify({command: "factory_reset"}));
    });

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

    // Per-sensor detail page CSV export — uses exportSeriesToCsv() (charts.js),
    // which was defined but never wired to any button before this. The
    // dashboard's "Export CSV" button above has its own bundled multi-sensor
    // export; this one exports just the currently-viewed sensor's buffer.
    const btnExportSensor = document.getElementById('btn-export-sensor-csv');
    if (btnExportSensor) {
        btnExportSensor.addEventListener('click', () => {
            const buf = sensorBuffers[currentTabId];
            // Only tabs 1,3,4,5,6 have a flat array buffer (tab 2 is the dual
            // Air Temp/Hum page with its own {hum,temp} shape, not a plain
            // array exportSeriesToCsv expects) — guard the same way
            // updateTelemetry() already gates chart drawing for these tabs.
            if (!Array.isArray(buf)) {
                alert("CSV export isn't available for this page.");
                return;
            }
            const sensorName = (tabsData.labels[currentTabId] || "sensor").replace(/\s+/g, '_');
            if (typeof exportSeriesToCsv === 'function') {
                exportSeriesToCsv(sensorName, buf);
            }
        });
    }

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

    // Power toggle on the per-sensor detail page (and the Air Temp/Humidity
    // dual-sensor page). This is the SAME on/off switch as the "Enabled"
    // toggle on each pinout card in Settings — both ultimately just send
    // save_sensor_enabled. Pins are never touched here: a pin is a plain
    // GPIO assignment, kept and shown regardless of on/off state (see
    // config.h's comment on ConfigState::pin_* for the full reasoning).
    const handleToggle = (e, tabId) => {
        const isEnabled = e.target.checked;
        const sensorId = TAB_TO_SENSOR_ID[tabId]; // short id used by save_sensor_enabled
        if (!sensorId) return;

        const sensorName = tabsData.labels[tabId] || sensorId;

        // e.target is whichever of the two power toggles fired this handler
        // (single-sensor page or dual-sensor/Air-Temp-Hum page) — map it to
        // its matching ON/OFF label so syncPowerToggleLabel() below updates
        // the right one regardless of which toggle was clicked.
        const toggleLabelId = e.target.id === 'dual-sensor-toggle' ? 'dual-sensor-toggle-state' : 'sensor-toggle-state';
        syncPowerToggleLabel(e.target.id, toggleLabelId); // immediate feedback on click, before the round-trip completes

        // #sensor-toggle/#dual-sensor-toggle are single shared DOM elements
        // reused across every sensor tab — e.target is a live reference to
        // whichever one fired, not a copy scoped to this tab. If the user
        // switches tabs before the save_sensor_enabled ack arrives, the
        // .then()/.catch() below used to write disabled/checked to
        // whatever tab happened to be showing when the promise settled,
        // not the tab that actually triggered it. Snapshot the tab id that
        // was active at click time and no-op the UI writes below if
        // currentTabId has since moved on — switchTab() already re-syncs
        // the toggle correctly from tabsData.enabled[] when a tab opens,
        // so there's nothing stale left to fix once the user has navigated
        // away.
        const startedOnTabId = tabId;

        e.target.disabled = true;
        sendCommand({ command: "save_sensor_enabled", sensor: sensorId, enabled: isEnabled }).then(() => {
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> ${escapeHtml(sensorName)} ${isEnabled ? "ENABLED" : "DISABLED"}.</div>`;
            if (currentTabId !== startedOnTabId) {
                // Still re-enable the toggle even though we're skipping the
                // rest of the UI update — leaving it permanently disabled
                // would strand it if the user navigates back to this tab.
                e.target.disabled = false;
                return;
            }
            e.target.disabled = false;
            if(confirm(`Sensor enabled state changed. The ESP32 must reboot to safely apply hardware changes. Reboot now?`)) {
                sendCommand({command: "reboot"}).catch(() => {});
            } else {
                e.target.checked = !isEnabled;
                syncPowerToggleLabel(e.target.id, toggleLabelId);
            }
        }).catch((err) => {
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> ${escapeHtml(sensorName)} enable change failed: ${escapeHtml(err && err.message ? err.message : 'error')}.</div>`;
            e.target.disabled = false;
            if (currentTabId !== startedOnTabId) return; // stale — switchTab() already shows the real state for whatever tab is open now
            e.target.checked = !isEnabled; // revert — the device never actually applied this
            syncPowerToggleLabel(e.target.id, toggleLabelId);
        });
    };

    const singleSensorToggle = document.getElementById('sensor-toggle');
    if (singleSensorToggle) singleSensorToggle.addEventListener('change', (e) => handleToggle(e, currentTabId));

    const dualSensorToggle = document.getElementById('dual-sensor-toggle');
    if (dualSensorToggle) dualSensorToggle.addEventListener('change', (e) => handleToggle(e, 2));

    // Reset-to-default-pin buttons — one per pinout card, plus a generic one
    // on the per-sensor offline banner that resets whichever sensor tab is open.
    //
    // Deliberately NOT routed through sendCommand(): reset_sensor_pin's
    // server handler (command_handlers.cpp) only ever sends an ack on the
    // FAILURE path (sendCmdAck(..., false, "Failed to save...") when
    // state_save() fails) — on success it calls ESP.restart() directly with
    // no ack at all, same as reboot/factory_reset above. Routing through
    // sendCommand() would therefore time out and show a false "failed"
    // error on every successful reset. Instead, this listens for exactly
    // one command_result matching this command within a short window and
    // only acts on an explicit failure; silence (the expected case, since
    // the device reboots) is treated as success and simply times out the
    // listener with no user-visible effect.
    const sendResetSensorPin = (sensorId) => {
        if (!confirm(`Reset the '${sensorId}' pin(s) to the factory default and reboot?`)) return;
        if (!websocket || websocket.readyState !== WebSocket.OPEN) { alert("Not connected to the device right now."); return; }

        const onResult = (msg) => {
            if (msg.type !== "command_result" || msg.command !== "reset_sensor_pin") return;
            resetSensorPinListeners = resetSensorPinListeners.filter((fn) => fn !== onResult);
            if (!msg.ok) {
                alert(`Pin reset failed: ${msg.error || 'the device rejected the request.'}`);
            }
            // ok:true is never actually sent (see comment above) — this
            // branch exists only so a future firmware change that DOES ack
            // success doesn't silently do nothing here.
        };
        resetSensorPinListeners.push(onResult);
        setTimeout(() => {
            resetSensorPinListeners = resetSensorPinListeners.filter((fn) => fn !== onResult);
        }, ACK_TIMEOUT_MS);

        try {
            websocket.send(JSON.stringify({ command: "reset_sensor_pin", sensor: sensorId }));
        } catch (e) {
            resetSensorPinListeners = resetSensorPinListeners.filter((fn) => fn !== onResult);
            alert("Failed to send reset request: " + e.message);
        }
    };

    document.querySelectorAll('[data-reset-sensor]').forEach((btn) => {
        btn.addEventListener('click', () => sendResetSensorPin(btn.dataset.resetSensor));
    });

    const btnResetCurrent = document.getElementById('btn-reset-current-sensor');
    if (btnResetCurrent) {
        btnResetCurrent.addEventListener('click', () => {
            const sensorId = TAB_TO_SENSOR_ID[currentTabId];
            if (sensorId) sendResetSensorPin(sensorId);
        });
    }

    // ------------------------------------------------------------------
    // Feature Flags — Demo Mode (Settings > Feature Flags card) and
    // Firebase Upload (Settings > Cloud Provisioning card) both persist via
    // the same save_features command, but they live in two DIFFERENT cards
    // and are wired independently so each one saves the moment it's
    // toggled — a switch that visually flips but silently isn't saved
    // until some other button (in another card) is clicked looks broken:
    // the next "config" frame from the device (e.g. right after Save
    // Credentials or Test Connection triggers a broadcast) reflects the
    // real still-unsaved device state and snaps the checkbox back, which
    // reads as "the toggle turns itself off". Sending on `change` for both
    // switches removes that gap entirely. Always send BOTH current checkbox
    // values together (not just the one that changed) since save_features
    // treats an omitted field as "leave unchanged" but these two live
    // fields are the actual source of truth for what's currently checked.
    // ------------------------------------------------------------------
    const sendFeatureFlags = (sourceEl) => {
        const payload = {
            command: "save_features",
            demo: !!document.getElementById('cfg-demo-mode')?.checked,
            fb_en: !!document.getElementById('cfg-fb-enabled')?.checked
        };
        sendCommand(payload).catch((err) => {
            // Roll the checkbox back to the last known device state on
            // failure (e.g. offline/timeout) instead of leaving the UI
            // showing a state the device never actually accepted.
            if (sourceEl) sourceEl.checked = !sourceEl.checked;
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Feature flag change failed: ${escapeHtml(err && err.message ? err.message : 'error')}.</div>`;
        });
    };

    const cfgDemoMode = document.getElementById('cfg-demo-mode');
    if (cfgDemoMode) cfgDemoMode.addEventListener('change', () => sendFeatureFlags(cfgDemoMode));

    const cfgFbEnabled = document.getElementById('cfg-fb-enabled');
    if (cfgFbEnabled) cfgFbEnabled.addEventListener('change', () => sendFeatureFlags(cfgFbEnabled));

    // ------------------------------------------------------------------
    // Per-sensor enable toggle inside each pinout card in Settings — the
    // SINGLE on/off switch for a sensor, bound to save_sensor_enabled. This
    // is the exact same switch as the "Enable Power" toggle on the
    // per-sensor detail page (see handleToggle() above) — both just flip
    // sensor_enabled[] and never touch pins. Requires a reboot to apply,
    // since the change only takes effect after the sensor task re-inits.
    // ------------------------------------------------------------------
    document.querySelectorAll('[data-sensor-enable]').forEach((el) => {
        el.addEventListener('change', (e) => {
            const sensorId = el.dataset.sensorEnable;
            const enabled = e.target.checked;
            e.target.disabled = true;
            sendCommand({ command: "save_sensor_enabled", sensor: sensorId, enabled }).then(() => {
                e.target.disabled = false;
                document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Sensor '${escapeHtml(sensorId)}' ${enabled ? "ENABLED" : "DISABLED"}.</div>`;
                if (confirm(`Sensor enabled state changed. The ESP32 must reboot to safely apply hardware changes. Reboot now?`)) {
                    sendCommand({ command: "reboot" }).catch(() => {});
                } else {
                    e.target.checked = !enabled;
                }
            }).catch((err) => {
                e.target.disabled = false;
                e.target.checked = !enabled; // revert — the device never actually applied this
                document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Sensor '${escapeHtml(sensorId)}' enable change failed: ${escapeHtml(err && err.message ? err.message : 'error')}.</div>`;
            });
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
            runSaveButton(btnSaveIntervals, payload, "Saved!", "Save Timing");
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
