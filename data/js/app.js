/* ═══════════════════════════════════════════════════════════════════════
   THEME MANAGEMENT SYSTEM
   ═══════════════════════════════════════════════════════════════════════ */

class ThemeManager {
    constructor() {
        this.themes = ['auto', 'light', 'dark'];
        this.currentTheme = this.loadTheme();
        this.init();
    }

    loadTheme() {
        const saved = localStorage.getItem('hygrow-theme');
        return saved || 'auto';
    }

    saveTheme(theme) {
        localStorage.setItem('hygrow-theme', theme);
        this.currentTheme = theme;
    }

    applyTheme(theme) {
        document.documentElement.setAttribute('data-theme', theme);
        this.saveTheme(theme);
        this.updateThemeButtons();
    }

    init() {
        this.applyTheme(this.currentTheme);
        this.setupThemeListeners();
    }

    setupThemeListeners() {
        // Settings page theme buttons
        document.querySelectorAll('.theme-option').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const theme = e.currentTarget.getAttribute('data-theme');
                this.applyTheme(theme);
            });
        });

        // Mobile header theme toggle
        const headerToggle = document.getElementById('header-theme-toggle');
        if (headerToggle) {
            headerToggle.addEventListener('click', () => {
                const themes = this.themes;
                const currentIndex = themes.indexOf(this.currentTheme);
                const nextIndex = (currentIndex + 1) % themes.length;
                this.applyTheme(themes[nextIndex]);
            });
        }
    }

    updateThemeButtons() {
        document.querySelectorAll('.theme-option').forEach(btn => {
            const btnTheme = btn.getAttribute('data-theme');
            if (btnTheme === this.currentTheme) {
                btn.classList.add('active');
            } else {
                btn.classList.remove('active');
            }
        });
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   MOBILE MENU MANAGEMENT
   ═══════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════
   NAVIGATION SYSTEM
   ═══════════════════════════════════════════════════════════════════════ */

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

        // Update active states
        document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));

        targetBtn.classList.add('active');
        targetBtn.setAttribute('aria-current', 'page');

        const targetPage = targetBtn.getAttribute('data-target');
        const page = document.getElementById(targetPage);
        if (page) {
            page.classList.add('active');
        }

        // Show/hide warning banner for incomplete sensors
        const banner = document.getElementById('incomplete-banner');
        if (banner) {
            const isWL = targetPage === 'page-wl' && document.getElementById('tog-0').checked;
            const isPH = targetPage === 'page-ph' && document.getElementById('tog-4').checked;
            banner.style.display = (isWL || isPH) ? 'flex' : 'none';
        }

        // Close mobile menu
        this.mobileMenu.closeMenuPanel();
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   WEBSOCKET CONNECTION MANAGER
   ═══════════════════════════════════════════════════════════════════════ */

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
                console.log('[HyGrow] WebSocket connected');
                this.reconnectAttempts = 0;
                this.updateConnectionStatus(true);
            };

            this.ws.onmessage = (event) => {
                this.handleMessage(event.data);
            };

            this.ws.onerror = (error) => {
                console.error('[HyGrow] WebSocket error:', error);
                this.updateConnectionStatus(false);
            };

            this.ws.onclose = () => {
                console.log('[HyGrow] WebSocket closed');
                this.updateConnectionStatus(false);
                this.attemptReconnect();
            };
        } catch (err) {
            console.error('[HyGrow] Failed to create WebSocket:', err);
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
            }
        } catch (err) {
            console.error('[HyGrow] Error parsing WebSocket message:', err);
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
        const map = {
            '&': '&amp;',
            '<': '&lt;',
            '>': '&gt;',
            '"': '&quot;',
            "'": '&#039;'
        };
        return text.replace(/[&<>"']/g, m => map[m]);
    }

    updateDashboard(data) {
        const updates = [
            ['dash-tds', data.tds.toFixed(1)],
            ['dash-temp', data.temp.toFixed(1)],
            ['dash-hum', data.hum.toFixed(1)],
            ['dash-wt', data.w_t.toFixed(1)],
            ['dash-lux', data.lux.toFixed(0)],
            ['val-tds', data.tds.toFixed(1)],
            ['val-temp', data.temp.toFixed(1)],
            ['val-hum', data.hum.toFixed(1)],
            ['val-wt', data.w_t.toFixed(1)],
            ['val-lux', data.lux.toFixed(0)]
        ];

        updates.forEach(([id, value]) => {
            const el = document.getElementById(id);
            if (el) el.innerText = value;
        });

        this.updateMetricBars(data);
    }

    updateMetricBars(data) {
        const bars = [
            ['bar-tds', data.tds, 0, 2000],
            ['bar-temp', data.temp, 0, 40],
            ['bar-hum', data.hum, 0, 100],
            ['bar-wt', data.w_t, 0, 40],
            ['bar-lux', data.lux, 0, 5000]
        ];

        bars.forEach(([id, value, min, max]) => {
            const el = document.getElementById(id);
            if (el) {
                const percentage = ((value - min) / (max - min)) * 100;
                el.style.width = Math.max(0, Math.min(100, percentage)) + '%';
            }
        });
    }

    updateCharts(data) {
        pushAndDraw('tds', data.tds, 'chart-tds', '#3b82f6');
        pushAndDraw('temp', data.temp, 'chart-temp', '#10b981');
        pushAndDraw('wt', data.w_t, 'chart-wt', '#06b6d4');
        pushAndDraw('lux', data.lux, 'chart-lux', '#f59e0b');
    }

    updateConnectionStatus(isConnected) {
        const indicator = document.querySelector('.status-dot');
        if (indicator) {
            indicator.style.background = isConnected ? '#10b981' : '#ef4444';
        }
    }

    attemptReconnect() {
        if (this.reconnectAttempts < this.maxReconnectAttempts) {
            this.reconnectAttempts++;
            const delay = 3000 * this.reconnectAttempts;
            console.log(`[HyGrow] Reconnecting in ${delay}ms (${this.reconnectAttempts}/${this.maxReconnectAttempts})...`);
            setTimeout(() => this.connect(), delay);
        }
    }

    send(payload) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(payload));
        } else {
            console.warn('[HyGrow] WebSocket not connected');
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   GLOBAL FUNCTIONS FOR HTML EVENTS
   ═══════════════════════════════════════════════════════════════════════ */

let wsManager;

function toggleSensor(sensorId, isEnabled) {
    if (wsManager) {
        wsManager.send({ cmd: 'toggle_sensor', id: sensorId, state: isEnabled });
    }

    const activeBtn = document.querySelector('.nav-btn.active');
    if (activeBtn) activeBtn.click();
}

function toggleDemo(isEnabled) {
    if (wsManager) {
        wsManager.send({ cmd: 'toggle_demo', state: isEnabled });
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   TERMINAL UTILITIES
   ═══════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════
   INITIALIZATION
   ═══════════════════════════════════════════════════════════════════════ */

document.addEventListener('DOMContentLoaded', () => {
    console.log('[HyGrow] Initializing dashboard...');

    // Initialize theme system
    const themeManager = new ThemeManager();

    // Initialize mobile menu
    const mobileMenu = new MobileMenuManager();

    // Initialize navigation
    const navManager = new NavigationManager(mobileMenu);

    // Initialize WebSocket
    wsManager = new WebSocketManager();

    // Initialize terminal
    TerminalManager.init();

    console.log('[HyGrow] Dashboard ready');
});
