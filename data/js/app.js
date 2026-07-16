// ════════════════════════════════════════════════════════════════════════════
// HYGROW IOT WEB DASHBOARD - ENHANCED SYSTEM
// ════════════════════════════════════════════════════════════════════════════

// Incomplete sensors that cannot be enabled
const INCOMPLETE_SENSORS = [0, 4]; // Water Level, pH

// Sensor configuration
const SENSOR_CONFIG = {
    0: { name: 'Water Level', key: 'wl', incomplete: true },
    1: { name: 'Light', key: 'light', incomplete: false },
    2: { name: 'TDS', key: 'tds', incomplete: false },
    3: { name: 'Air Sensors', key: 'temp', incomplete: false },
    4: { name: 'pH', key: 'ph', incomplete: true },
    5: { name: 'Water Temp', key: 'wt', incomplete: false }
};

let sensorStates = [false, false, false, false, false, false]; // Current on/off state
let sensorErrors = [false, false, false, false, false, false]; // Current error state

// ════════════════════════════════════════════════════════════════════════════
// TOAST NOTIFICATIONS
// ════════════════════════════════════════════════════════════════════════════

function showToast(message, type = 'info', duration = 3000) {
    const container = document.getElementById('toast-container') || createToastContainer();
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.textContent = message;
    container.appendChild(toast);

    setTimeout(() => toast.remove(), duration);
}

function createToastContainer() {
    const container = document.createElement('div');
    container.id = 'toast-container';
    container.style.cssText = `
        position: fixed;
        top: 20px;
        right: 20px;
        z-index: 10000;
        display: flex;
        flex-direction: column;
        gap: 10px;
    `;
    document.body.appendChild(container);

    const style = document.createElement('style');
    style.textContent = `
        .toast {
            padding: 12px 16px;
            border-radius: 6px;
            color: white;
            font-weight: 500;
            animation: slideIn 0.3s ease;
            max-width: 300px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.3);
        }
        .toast-info { background: var(--accent); }
        .toast-error { background: var(--status-error); }
        .toast-warn { background: var(--status-warn); }
        .toast-success { background: var(--status-ok); }
        @keyframes slideIn {
            from { transform: translateX(400px); opacity: 0; }
            to { transform: translateX(0); opacity: 1; }
        }
    `;
    document.head.appendChild(style);
    return container;
}

// ════════════════════════════════════════════════════════════════════════════
// METRIC CARD STATUS MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

function updateMetricStatus(sensorId) {
    const config = SENSOR_CONFIG[sensorId];
    if (!config) return;

    const statusEl = document.getElementById(`status-${config.key}`);
    const toggleBtn = document.getElementById(`toggle-${config.key}`);

    if (!statusEl || !toggleBtn) return;

    const isEnabled = sensorStates[sensorId];
    const hasError = sensorErrors[sensorId];

    // Update indicator
    statusEl.className = 'metric-status';
    if (!isEnabled) {
        statusEl.classList.add('off');
    } else if (hasError) {
        statusEl.classList.add('error');
    } else {
        statusEl.classList.add('on');
    }

    // Show/hide toggle button
    if (!isEnabled) {
        if (config.incomplete) {
            toggleBtn.textContent = 'Not Available';
            toggleBtn.className = 'metric-toggle show disabled';
            toggleBtn.onclick = () => showToast(`${config.name} sensor code is not implemented`, 'error');
        } else {
            toggleBtn.textContent = 'Turn ON';
            toggleBtn.className = 'metric-toggle show';
            toggleBtn.onclick = () => toggleMetricSensor(sensorId);
        }
    } else {
        toggleBtn.classList.remove('show');
    }
}

function toggleMetricSensor(sensorId) {
    const config = SENSOR_CONFIG[sensorId];

    if (config.incomplete) {
        showToast(`${config.name} sensor code is not implemented`, 'error');
        return;
    }

    if (wsManager && wsManager.ws && wsManager.ws.readyState === WebSocket.OPEN) {
        wsManager.send({ cmd: 'toggle_sensor', id: sensorId, state: true });
        showToast(`${config.name} enabled`, 'success');
    }
}

// ════════════════════════════════════════════════════════════════════════════
// THEME MANAGEMENT SYSTEM
// ════════════════════════════════════════════════════════════════════════════

class ThemeManager {
    constructor() {
        this.loadTheme();
        this.setupThemeButtons();
    }

    loadTheme() {
        const saved = localStorage.getItem('hygrow-theme') || 'auto';
        document.documentElement.setAttribute('data-theme', saved);
    }

    setupThemeButtons() {
        document.querySelectorAll('.theme-option').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const theme = e.target.closest('.theme-option').getAttribute('data-theme');
                this.setTheme(theme);
            });
        });

        const headerToggle = document.getElementById('header-theme-toggle');
        if (headerToggle) {
            headerToggle.addEventListener('click', () => this.cycleTheme());
        }
    }

    setTheme(theme) {
        document.documentElement.setAttribute('data-theme', theme);
        localStorage.setItem('hygrow-theme', theme);
        this.updateThemeButtons(theme);
    }

    cycleTheme() {
        const current = document.documentElement.getAttribute('data-theme');
        const next = { auto: 'light', light: 'dark', dark: 'auto' }[current] || 'auto';
        this.setTheme(next);
    }

    updateThemeButtons(active) {
        document.querySelectorAll('.theme-option').forEach(btn => {
            btn.classList.toggle('active', btn.getAttribute('data-theme') === active);
        });
    }
}

// ════════════════════════════════════════════════════════════════════════════
// MOBILE MENU MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

class MobileMenuManager {
    constructor() {
        this.menuToggle = document.getElementById('menu-toggle');
        this.closeMenu = document.getElementById('close-menu');
        this.sidebar = document.getElementById('sidebar');
        this.menuOverlay = document.getElementById('menu-overlay');
        this.init();
    }

    init() {
        if (this.menuToggle) {
            this.menuToggle.addEventListener('click', () => this.toggleMenu());
        }
        if (this.closeMenu) {
            this.closeMenu.addEventListener('click', () => this.closeMenuPanel());
        }
        if (this.menuOverlay) {
            this.menuOverlay.addEventListener('click', () => this.closeMenuPanel());
        }
        window.addEventListener('resize', () => {
            if (window.innerWidth > 768) {
                this.closeMenuPanel();
            }
        });
    }

    toggleMenu() {
        this.sidebar.classList.toggle('open');
        this.menuOverlay.classList.toggle('active');
    }

    closeMenuPanel() {
        this.sidebar.classList.remove('open');
        this.menuOverlay.classList.remove('active');
    }
}

// ════════════════════════════════════════════════════════════════════════════
// NAVIGATION SYSTEM
// ════════════════════════════════════════════════════════════════════════════

class NavigationManager {
    constructor(mobileMenu) {
        this.mobileMenu = mobileMenu;
        this.init();
    }

    init() {
        document.querySelectorAll('.nav-btn').forEach(btn => {
            btn.addEventListener('click', (e) => this.handleNavigation(e));
        });
    }

    handleNavigation(e) {
        const targetBtn = e.currentTarget;
        document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));

        targetBtn.classList.add('active');
        targetBtn.setAttribute('aria-current', 'page');

        const targetPage = targetBtn.getAttribute('data-target');
        const page = document.getElementById(targetPage);
        if (page) {
            page.classList.add('active');
        }

        this.mobileMenu.closeMenuPanel();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// WEBSOCKET CONNECTION MANAGER
// ════════════════════════════════════════════════════════════════════════════

class WebSocketManager {
    constructor() {
        this.ws = null;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 5;
        this.connect();
    }

    connect() {
        try {
            this.ws = new WebSocket('ws://' + window.location.hostname + '/ws');

            this.ws.onopen = () => {
                console.log('[v0] WebSocket connected');
                this.reconnectAttempts = 0;
                this.updateConnectionStatus(true);
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(event.data);
            };

            this.ws.onerror = (error) => {
                console.error('[v0] WebSocket error:', error);
                this.updateConnectionStatus(false);
            };

            this.ws.onclose = () => {
                console.log('[v0] WebSocket closed');
                this.updateConnectionStatus(false);
                this.attemptReconnect();
            };
        } catch (err) {
            console.error('[v0] Failed to create WebSocket:', err);
        }
    }

    handleMessage(data) {
        try {
            const message = JSON.parse(data);

            if (message.type === 'log') {
                this.addLog(message.msg);
            } else if (message.type === 'data') {
                this.updateDashboard(message);
                this.updateCharts(message);
                this.updateSensorStates(message);
            }
        } catch (err) {
            console.error('[v0] Error parsing message:', err);
        }
    }

    addLog(msg) {
        const term = document.getElementById('term-out');
        if (!term) return;

        const time = new Date().toLocaleTimeString();
        const logDiv = document.createElement('div');
        logDiv.innerHTML = `<span class="log-time">[${time}]</span> ${this.escapeHtml(msg)}`;
        term.appendChild(logDiv);
        term.scrollTop = term.scrollHeight;
    }

    escapeHtml(text) {
        const map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' };
        return text.replace(/[&<>"']/g, m => map[m]);
    }

    updateSensorStates(data) {
        // Update enabled states
        if (data.sensor_enabled) {
            sensorStates = data.sensor_enabled;
            Object.keys(SENSOR_CONFIG).forEach(id => {
                updateMetricStatus(parseInt(id));
            });
        }

        // Update error states
        if (data.errors) {
            sensorErrors = data.errors;
            Object.keys(SENSOR_CONFIG).forEach(id => {
                updateMetricStatus(parseInt(id));
            });
        }
    }

    updateDashboard(data) {
        const updates = [
            ['dash-tds', data.tds_ppm?.toFixed(1) || '--'],
            ['dash-temp', data.dht_temp?.toFixed(1) || '--'],
            ['dash-hum', data.dht_hum?.toFixed(1) || '--'],
            ['dash-wt', data.w_temp?.toFixed(1) || '--'],
            ['dash-lux', data.light_lux?.toFixed(0) || '--'],
            ['dash-wl', data.wl_percent?.toFixed(0) || '--'],
            ['dash-ph', data.ph_val?.toFixed(1) || '--'],
            ['dash-vpd', data.vpd_kpa?.toFixed(2) || '--']
        ];

        updates.forEach(([id, value]) => {
            const el = document.getElementById(id);
            if (el) el.innerText = value;
        });

        this.updateMetricBars(data);
    }

    updateMetricBars(data) {
        const bars = [
            ['bar-tds', data.tds_ppm, 0, 2000],
            ['bar-temp', data.dht_temp, 0, 40],
            ['bar-hum', data.dht_hum, 0, 100],
            ['bar-wt', data.w_temp, 0, 40],
            ['bar-lux', data.light_lux, 0, 5000],
            ['bar-wl', data.wl_percent, 0, 100],
            ['bar-ph', data.ph_val, 0, 14],
            ['bar-vpd', data.vpd_kpa, 0, 5]
        ];

        bars.forEach(([id, value, min, max]) => {
            if (value === null || value === undefined) return;
            const el = document.getElementById(id);
            if (el) {
                const percentage = ((value - min) / (max - min)) * 100;
                el.style.width = Math.max(0, Math.min(100, percentage)) + '%';
            }
        });
    }

    updateCharts(data) {
        if (data.tds_ppm !== null) pushAndDraw('tds', data.tds_ppm, 'chart-tds', '#3b82f6');
        if (data.dht_temp !== null) pushAndDraw('temp', data.dht_temp, 'chart-temp', '#10b981');
        if (data.w_temp !== null) pushAndDraw('wt', data.w_temp, 'chart-wt', '#06b6d4');
        if (data.light_lux !== null) pushAndDraw('lux', data.light_lux, 'chart-lux', '#f59e0b');
    }

    updateConnectionStatus(isConnected) {
        const indicator = document.querySelector('.status-dot');
        if (indicator) {
            indicator.style.background = isConnected ? 'var(--status-ok)' : 'var(--status-error)';
        }
    }

    attemptReconnect() {
        if (this.reconnectAttempts < this.maxReconnectAttempts) {
            this.reconnectAttempts++;
            const delay = 3000 * this.reconnectAttempts;
            console.log(`[v0] Reconnecting in ${delay}ms...`);
            setTimeout(() => this.connect(), delay);
        }
    }

    send(payload) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(payload));
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

function saveWiFiConfig() {
    const ssid = document.getElementById('wifi-ssid').value.trim();
    const password = document.getElementById('wifi-password').value.trim();

    if (!ssid) {
        showToast('Please enter WiFi SSID', 'error');
        return;
    }
    if (!password) {
        showToast('Please enter WiFi password', 'error');
        return;
    }

    if (wsManager && wsManager.ws && wsManager.ws.readyState === WebSocket.OPEN) {
        wsManager.send({ cmd: 'save_wifi', ssid, password });
        showToast('WiFi configuration saved', 'success');
        document.getElementById('wifi-ssid').value = '';
        document.getElementById('wifi-password').value = '';
    }
}

function saveFirebaseConfig() {
    const apiKey = document.getElementById('firebase-api-key').value.trim();
    const projectId = document.getElementById('firebase-project').value.trim();
    const collection = document.getElementById('firebase-collection').value.trim();

    if (!apiKey || !projectId || !collection) {
        showToast('Please fill in all Firebase fields', 'error');
        return;
    }

    if (wsManager && wsManager.ws && wsManager.ws.readyState === WebSocket.OPEN) {
        wsManager.send({ cmd: 'save_firebase', api_key: apiKey, project_id: projectId, collection });
        showToast('Firebase configuration saved', 'success');
        document.getElementById('firebase-api-key').value = '';
        document.getElementById('firebase-project').value = '';
        document.getElementById('firebase-collection').value = '';
    }
}

function checkFirmwareUpdate() {
    if (wsManager && wsManager.ws && wsManager.ws.readyState === WebSocket.OPEN) {
        wsManager.send({ cmd: 'check_firmware' });
        showToast('Checking for firmware updates...', 'info');
    }
}

function updateFirmware() {
    if (!confirm('Device will restart during update. Continue?')) return;

    if (wsManager && wsManager.ws && wsManager.ws.readyState === WebSocket.OPEN) {
        wsManager.send({ cmd: 'update_firmware' });
        showToast('Firmware update started', 'success');
    }
}

// ════════════════════════════════════════════════════════════════════════════
// TERMINAL
// ════════════════════════════════════════════════════════════════════════════

class TerminalManager {
    static init() {
        const clearBtn = document.getElementById('clear-logs');
        if (clearBtn) {
            clearBtn.addEventListener('click', () => {
                document.getElementById('term-out').innerHTML = '';
            });
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// DEMO FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

function toggleSensor(sensorId, isEnabled) {
    const config = SENSOR_CONFIG[sensorId];

    if (config.incomplete && isEnabled) {
        showToast(`${config.name} sensor code is incomplete`, 'error');
        return;
    }

    if (wsManager) {
        wsManager.send({ cmd: 'toggle_sensor', id: sensorId, state: isEnabled });
    }
}

function toggleDemo(isEnabled) {
    if (wsManager) {
        wsManager.send({ cmd: 'toggle_demo', state: isEnabled });
    }
}

// ════════════════════════════════════════════════════════════════════════════
// CHART FUNCTIONS (Stub - requires chart library)
// ════════════════════════════════════════════════════════════════════════════

const historyData = {
    tds: [], temp: [], wt: [], lux: []
};
const MAX_POINTS = 100;

function pushAndDraw(key, val, canvasId, color) {
    if (!historyData[key]) historyData[key] = [];
    historyData[key].push(val);
    if (historyData[key].length > MAX_POINTS) historyData[key].shift();
    drawChart(canvasId, historyData[key], color);
}

function drawChart(canvasId, data, color) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const width = canvas.width;
    const height = canvas.height;
    ctx.clearRect(0, 0, width, height);

    if (data.length < 2) return;

    const min = Math.min(...data);
    const max = Math.max(...data);
    const range = max - min || 1;

    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();

    data.forEach((value, idx) => {
        const x = (idx / (data.length - 1)) * width;
        const y = height - ((value - min) / range) * height;
        if (idx === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    });

    ctx.stroke();
}

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL MANAGER
// ════════════════════════════════════════════════════════════════════════════

let wsManager;

// ════════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ════════════════════════════════════════════════════════════════════════════

document.addEventListener('DOMContentLoaded', () => {
    console.log('[v0] Initializing HyGrow Dashboard...');

    new ThemeManager();
    const mobileMenu = new MobileMenuManager();
    new NavigationManager(mobileMenu);
    wsManager = new WebSocketManager();
    TerminalManager.init();

    // Initialize metric status displays
    Object.keys(SENSOR_CONFIG).forEach(id => {
        updateMetricStatus(parseInt(id));
    });

    console.log('[v0] Dashboard initialized');
});
