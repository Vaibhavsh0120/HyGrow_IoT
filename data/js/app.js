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
    activeStyle: "text-white font-bold bg-white/10 rounded-2xl shadow-inner",
    inactiveStyle: "text-on-surface-variant font-medium hover:bg-white/10 hover:text-white rounded-2xl transition-colors duration-200",
    // #sideNav only ever renders at lg (1024px+) now — see the breakpoint
    // fix in index.html — so this row's classes no longer need a
    // non-lg: base state at all; px-6/py-3 apply unconditionally since
    // there's no smaller-viewport version of this element to size for.
    baseStyle: "flex items-center justify-start gap-4 px-6 py-3 cursor-pointer transition-all duration-150 w-full",
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

// ------------------------------------------------------------------
// Settings staging state (Part 5.9). Every Settings card whose Save
// button now buffers changes instead of sending on `change` tracks its
// own "last confirmed device value" and "has unsaved edits" flag here,
// module-scope so updateConfigForm() (triggered by any incoming "config"
// frame, not just this tab's own saves) can skip overwriting a field the
// user hasn't saved yet — see the guards in updateConfigForm() below.
// Each card is independent, matching how Save Pinout/Save
// Credentials/Save Wi-Fi were already independent buttons before this.
// ------------------------------------------------------------------
let lastConfirmedDemo = false;
let featuresDirty = false;
let lastConfirmedFbEnabled = false;
let fbEnabledDirty = false;
// Keyed by short sensor id (tds/dht/ph/wt/wl/light), matching S_EN_INDEX/
// TAB_TO_SENSOR_ID elsewhere in this file.
let lastConfirmedSensorEnabled = {};
// Keyed by pin field element id (cfg-pin-tds, cfg-pin-wlp, etc), the
// device's real current value for each pin field — the pin-field
// equivalent of lastConfirmedSensorEnabled above. Populated in
// updateConfigForm() from every config frame's msg.pins[].
let lastConfirmedPins = {};
let pinoutDirty = false; // true if ANY pin field OR ANY sensor-enable toggle differs from last-confirmed

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

// Mobile sidebar overlay — legacy/currently inert: #btn-mobile-menu (the
// only trigger for setMobileNavOpen(true)) is permanently `hidden` in the
// DOM (see its comment in index.html), so this never actually opens today.
// #sideNav itself now only ever renders at the lg breakpoint (moved from
// md — see the #sideNav/#bottomNav comments in index.html), below which
// #bottomNav is the real navigation. Left in place rather than removed
// since deleting it isn't part of this breakpoint fix's scope. Owns
// opening/closing
// #sideNav's `.hg-sidenav-mobile-open` class and #mobile-nav-scrim's
// visibility together, so they can never drift out of sync with each other.
function setMobileNavOpen(open) {
    const nav = document.getElementById('sideNav');
    const scrim = document.getElementById('mobile-nav-scrim');
    const btn = document.getElementById('btn-mobile-menu');
    if (!nav) return;
    nav.classList.toggle('hg-sidenav-mobile-open', open);
    if (scrim) scrim.classList.toggle('hidden', !open);
    if (btn) btn.setAttribute('aria-expanded', open ? 'true' : 'false');
    // Prevents the page behind the full-screen overlay from scrolling along
    // with it on iOS — without this, a swipe that starts on the overlay can
    // still rubber-band/scroll <main> underneath, which reads as the menu
    // itself being broken/laggy.
    document.body.classList.toggle('overflow-hidden', open);
}

function isMobileNavOpen() {
    const nav = document.getElementById('sideNav');
    return !!nav && nav.classList.contains('hg-sidenav-mobile-open');
}

function initMobileNav() {
    const btnOpen = document.getElementById('btn-mobile-menu');
    const btnClose = document.getElementById('btn-mobile-nav-close');
    const scrim = document.getElementById('mobile-nav-scrim');

    if (btnOpen) btnOpen.addEventListener('click', () => setMobileNavOpen(!isMobileNavOpen()));
    if (btnClose) btnClose.addEventListener('click', () => setMobileNavOpen(false));
    if (scrim) scrim.addEventListener('click', () => setMobileNavOpen(false));

    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && isMobileNavOpen()) setMobileNavOpen(false);
    });

    // A resize that crosses back up into the lg desktop layout (e.g.
    // rotating an iPhone in a stage-manager/external-display setup, or
    // just a window resize on a browser dev-tools device toolbar) should
    // never leave the overlay state stuck open and unreachable behind the
    // now-fixed desktop sidebar — close it whenever the viewport is no
    // longer in the mobile range this behavior applies to. Threshold moved
    // to 1024 to match #sideNav's breakpoint (was 768/md).
    window.addEventListener('resize', () => {
        if (window.innerWidth >= 1024 && isMobileNavOpen()) setMobileNavOpen(false);
    });
}

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
            <span class="material-symbols-outlined" aria-hidden="true" data-icon="${tabsData.icons[index]}"></span>
            <span class="font-label-md text-nav-label whitespace-nowrap">${label}</span>
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

// Same ON/OFF text-sync job as syncPowerToggleLabel() above, kept as its
// own function (rather than reused directly) since Demo Mode is styled
// with text-primary rather than text-secondary while active — unlike a
// sensor's power toggle, "on" here means "you're looking at simulated
// data, not the real sensor", so it deliberately reads as a distinct
// accent from the healthy sensor-ON state instead of an identical green.
// (No dedicated warning/amber token exists in this stylesheet — see
// style.css's .text-error/.text-primary/.text-secondary — so this reuses
// an existing one rather than adding a new color for a single label.)
function syncDemoToggleLabel(toggleId, labelId) {
    const toggle = document.getElementById(toggleId);
    const label = document.getElementById(labelId);
    if (!toggle || !label) return;
    label.innerText = toggle.checked ? 'ON' : 'OFF';
    label.classList.toggle('text-primary', toggle.checked);
    label.classList.toggle('text-on-surface-variant', !toggle.checked);
}

// iPhone bottom navbar (chunk 4d). Only 4 fixed destinations, unlike the
// full 10-entry #nav-tabs sidebar: Dashboard (0), Live Calibration (7),
// Terminal (9), Settings (8) — in that display order. Per-sensor detail
// pages (tabs 1,2,3,4,5,6) are intentionally NOT here; those are reached
// by tapping a sensor's own card on the Dashboard (see initSensorCardLinks
// below). This list is deliberately hardcoded rather than derived from
// tabsData, since it's a curated subset with its own order, not "all tabs
// that fit" — a future tab added to tabsData should NOT automatically
// appear here without a deliberate decision about whether it belongs.
const bottomNavItems = [
    { index: 0, icon: 'monitoring', label: 'Dashboard' },
    { index: 7, icon: 'settings_input_component', label: 'Calibration' },
    { index: 9, icon: 'terminal', label: 'Terminal' },
    { index: 8, icon: 'settings', label: 'Settings' },
];

function initBottomNav() {
    const bottomNavContainer = document.getElementById('bottomNav');
    if (!bottomNavContainer) return;

    bottomNavItems.forEach(({ index, icon, label }) => {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.dataset.id = index;
        btn.setAttribute('role', 'tab');
        btn.setAttribute('aria-selected', index === 0 ? 'true' : 'false');
        btn.setAttribute('aria-label', label);
        // flex-1 so all 4 buttons share the bar evenly; py-2 gives a real
        // tap target without the label wrapping on narrow iPhone widths.
        btn.className = `flex-1 flex flex-col items-center justify-center gap-1 py-2 transition-colors duration-300 ${index === 0 ? 'text-white' : 'text-on-surface-variant'}`;
        btn.innerHTML = `
            <span class="material-symbols-outlined" aria-hidden="true" data-icon="${icon}"></span>
            <span class="font-label-sm text-label-sm">${label}</span>
        `;
        // switchTab's own element lookup falls back to #nav-tabs's children
        // when called with just an index (see the comment inside switchTab
        // below) — that's exactly what's wanted here, so no second arg.
        btn.addEventListener('click', () => switchTab(index));
        bottomNavContainer.appendChild(btn);
    });
}

// Keeps #bottomNav's own active-item highlight in sync with switchTab().
// Separate DOM tree from #nav-tabs, and only 4 of the 10 tabsData indices
// ever appear here — an index with no matching button (any per-sensor tab)
// simply leaves every bottom-nav button inactive, which is correct since
// none of them represent that page.
function syncBottomNavActive(index) {
    const bottomNavContainer = document.getElementById('bottomNav');
    if (!bottomNavContainer) return;
    Array.from(bottomNavContainer.children).forEach(btn => {
        const isActive = Number(btn.dataset.id) === index;
        btn.classList.toggle('text-white', isActive);
        btn.classList.toggle('text-on-surface-variant', !isActive);
        btn.setAttribute('aria-selected', isActive ? 'true' : 'false');
    });
}

// Makes each Dashboard vitals-grid sensor card its own tap target to that
// sensor's detail page (chunk 4d) — previously plain, non-interactive divs.
// The index.html->tab-index mapping lives as a data-goto-tab="N" attribute
// directly on each card in the markup (single source of truth, visible in
// the HTML itself) rather than duplicated here — this just reads it.
//
// VPD's card has no data-goto-tab attribute at all: it has no detail page
// of its own (tabsData.labels[7] is "Live Calibration", a different page —
// VPD is a derived dashboard-only reading with no tabsData index to route
// to), so it's correctly skipped by the querySelectorAll below rather than
// being routed nowhere.
function initSensorCardLinks() {
    document.querySelectorAll('#page-0 [data-goto-tab]').forEach(card => {
        const tabIndex = Number(card.dataset.gotoTab);
        card.classList.add('cursor-pointer');
        card.setAttribute('role', 'button');
        card.setAttribute('tabindex', '0');
        card.addEventListener('click', () => switchTab(tabIndex));
        card.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); switchTab(tabIndex); }
        });
    });
}

function switchTab(index, element) {
    // Scroll-to-top handling (Part 6.1) — <main> is the single shared
    // scroll container for every page-section, so without this a tab
    // switch could open the new page still scrolled to wherever the
    // PREVIOUS page happened to be, and re-tapping the already-active tab
    // did nothing at all. Checked here, before currentTabId is reassigned
    // below, since that's the only point that still knows whether this is
    // a real switch or a repeat tap on the current tab.
    const isReTap = (index === currentTabId);
    const mainEl = document.querySelector('main');
    if (mainEl) {
        if (isReTap) {
            // Re-tapping the active tab: treat it like "scroll to top" —
            // smooth, since the user is already looking at this page and
            // the motion itself is the whole point.
            mainEl.scrollTo({ top: 0, behavior: 'smooth' });
        } else {
            // Genuine page change: snap instantly. This isn't a "nice
            // scroll" moment, it's making sure the newly-shown page isn't
            // silently pre-scrolled from whatever the last page's
            // scrollTop happened to be.
            mainEl.scrollTo({ top: 0, behavior: 'auto' });
        }
    }
    // A re-tap doesn't change which page is showing or which nav item is
    // active — every step below this exists to switch pages/update
    // highlighting, none of which needs to re-run for a no-op tap.
    if (isReTap) return;

    currentTabId = index;
    const navTabsContainer = document.getElementById('nav-tabs');

    // element is optional (chunk 4d) — callers outside the sidebar itself
    // (dashboard sensor cards, the iPhone bottom navbar) only know the tab
    // INDEX they want, not which <li> in #nav-tabs corresponds to it. Fall
    // back to looking it up here rather than making every new caller
    // duplicate that lookup (or worse, reach into #nav-tabs's DOM order
    // itself, which would silently break if initNavigation()'s creation
    // order ever changed). The two original call sites in initNavigation()
    // still pass `li` directly since they already have it in hand — this
    // is purely additive, not a behavior change for them.
    if (!element) element = navTabsContainer.children[index];

    // On the mobile overlay, selecting a page means "go there" — leaving the
    // menu open over the freshly-switched page would just be in the way.
    // No-op at md and up, where the sidebar was never in overlay mode.
    if (isMobileNavOpen()) setMobileNavOpen(false);

    // Update Active Classes
    Array.from(navTabsContainer.children).forEach(child => {
        child.className = `${tabsData.baseStyle} ${tabsData.inactiveStyle}`;
        child.setAttribute('aria-selected', 'false');
    });
    // element can still be undefined here if index doesn't correspond to
    // any sidebar tab at all — every current caller only ever passes a
    // valid tabsData index (0-9), so this is defensive rather than
    // expected, but guards against a future caller passing something out
    // of range and crashing the whole page-switch instead of just skipping
    // the (nonexistent) active-highlight step.
    if (element) {
        element.className = `${tabsData.baseStyle} ${tabsData.activeStyle} scale-95 transition-transform duration-150`;
        element.setAttribute('aria-selected', 'true');
        setTimeout(() => { element.classList.remove('scale-95'); }, 150);
    }

    // Sync the iPhone bottom navbar's own active-item highlight (chunk 4d).
    // This is a completely separate set of DOM nodes from #nav-tabs (see
    // initBottomNav() below) — the sidebar's active-class logic above has
    // no way to reach it, and the bottom navbar only ever has 3 items
    // (Dashboard/Settings/Terminal, indices 0/8/9), not one per tabsData
    // entry, so it can't just mirror the same forEach over the same list.
    syncBottomNavActive(index);

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
        // Demo Mode toggle: PER-SENSOR now, same reasoning as the
        // single-sensor branch below — reflects this sensor's own pin
        // against the DEMO_MODE_PIN sentinel, not the global demo_mode
        // flag. tabsData.gpios[index] carries the live raw pin value from
        // every config frame regardless of which page is open, so this is
        // safe to read here even before this tab's first visit, same as
        // before.
        document.getElementById('dual-sensor-demo-toggle').checked = (pin === -42);
        syncDemoToggleLabel('dual-sensor-demo-toggle', 'dual-sensor-demo-toggle-state');

        // Same immediate-from-buffer treatment as the single-sensor branch
        // above — sensorBuffers[2] isn't cleared on tab switch, so render
        // the last-known temp/humidity now instead of leaving whatever
        // text was last written (stale, from before this switch) on
        // screen until the next "data" frame arrives.
        const lastTemp = sensorBuffers[2].temp[sensorBuffers[2].temp.length - 1];
        const lastHum = sensorBuffers[2].hum[sensorBuffers[2].hum.length - 1];
        document.getElementById('sensor-dual-temp').innerHTML = lastTemp !== undefined
            ? `${lastTemp.toFixed(1)} <span class="text-headline-md text-white/50 ml-1">°C</span>`
            : `-- <span class="text-headline-md text-white/50 ml-1">°C</span>`;
        document.getElementById('sensor-dual-hum').innerHTML = lastHum !== undefined
            ? `${lastHum.toFixed(0)} <span class="text-headline-md text-white/50 ml-1">%</span>`
            : `-- <span class="text-headline-md text-white/50 ml-1">%</span>`;

        setTimeout(resizeCanvas, 50);
    } else {
        sensorPage.classList.remove('hidden');
        sensorPage.classList.add('flex');
        document.getElementById('sensor-name').innerText = tabsData.labels[index] + " Sensor";
        document.getElementById('sensor-icon').setAttribute('data-icon', tabsData.icons[index]);

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
        // Demo Mode toggle — PER-SENSOR now (see save_sensor_demo,
        // command_handlers.cpp / handleSensorPageDemoToggle(), further down
        // this file): reflects whether THIS sensor's own pin currently equals
        // the DEMO_MODE_PIN sentinel (-42, config.h), not the global
        // demo_mode flag. tabsData.gpios[index] already carries the sensor's
        // live raw pin value from every config frame (see the msg.pins[]
        // parsing above), so no extra field is needed to detect this
        // client-side — same sentinel-equality check sensorPinIsDemo()
        // (task_sensor.cpp) does server-side.
        document.getElementById('sensor-demo-toggle').checked = (pin === -42);
        syncDemoToggleLabel('sensor-demo-toggle', 'sensor-demo-toggle-state');

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

        // Render the last-known value immediately from the existing
        // sensorBuffers[] buffer instead of blanking to "--" and waiting
        // for the next "data" WebSocket frame (up to interval_ws_ms, 1s by
        // default) to arrive — sensorBuffers[] isn't cleared on tab switch,
        // so a value from before this switch is already sitting there.
        // resizeCanvas() below (its own setTimeout) redraws the chart line
        // from the same buffer once the fresh canvas has real dimensions,
        // so only the numeric readout needs handling here.
        const bufferedVal = sensorBuffers[index] ? sensorBuffers[index][sensorBuffers[index].length - 1] : undefined;
        document.getElementById('sensor-current-val').innerHTML = bufferedVal !== undefined
            ? `${bufferedVal.toFixed(1)} <span class="text-headline-md text-white/50 ml-1">${tabsData.units[index]}</span>`
            : `-- <span class="text-headline-md text-white/50 ml-1">${tabsData.units[index]}</span>`;
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
            drawChart(currentSensorCtx, currentSensorCanvas, sensorBuffers[currentTabId], currentTabId === 1 ? 'secondary' : 'primary');
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

function setAuthButtonsSubmitting(submitting, type) {
    const btnLogin = document.getElementById('btn-auth-login');
    const btnSetup = document.getElementById('btn-auth-setup');
    if (btnLogin) {
        btnLogin.disabled = submitting;
        btnLogin.innerText = submitting && type === 'login' ? 'Logging in…' : 'Login';
    }
    if (btnSetup) {
        btnSetup.disabled = submitting;
        btnSetup.innerText = submitting && type === 'setup' ? 'Setting Password…' : 'Set Password & Continue';
    }
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

    setAuthButtonsSubmitting(false);

    if (panel === 'none') {
        overlay.classList.add('hidden');
        return;
    }
    overlay.classList.remove('hidden');
    if (spinner) spinner.classList.toggle('hidden', panel !== 'spinner');
    if (setup) setup.classList.toggle('hidden', panel !== 'setup');
    if (login) login.classList.toggle('hidden', panel !== 'login');
}

// Shows the "Reboot Required?" modal in place of the browser's native
// confirm() — every flow that needs a post-save reboot (Wi-Fi credentials,
// Pinout, per-sensor Enabled toggle, Demo Mode) now calls this instead of
// window.confirm() directly, so the prompt matches the rest of the UI (and,
// on iOS/iPadOS Safari, isn't a blocking native dialog that looks jarringly
// out of place in an installed/full-screen web app).
//
// `message` replaces the modal's body text with the exact wording each call
// site already used with confirm() — kept verbatim per call site rather than
// generalized, since each one explains something slightly different (Wi-Fi's
// SoftAP-fallback note, Pinout's "reassign hardware interrupts" framing,
// etc.) that's worth keeping specific. `onConfirm` runs only if the user taps
// "Reboot Now" — same contract confirm() had (only the truthy branch used to
// do anything), so callers that only ever branched on `if (confirm(...))`
// port over unchanged. `onCancel` is optional, for the few call sites that
// also had real work in confirm()'s `else` branch (reverting a toggle the
// user just flipped, since the change was persisted but won't take effect
// without the reboot they just declined).
let s_rebootConfirmHandler = null;
let s_rebootCancelHandler = null;

function confirmReboot(message, onConfirm, onCancel) {
    const modal = document.getElementById('reboot-confirm');
    const text = document.getElementById('reboot-confirm-text');
    if (!modal) { if (onConfirm) onConfirm(); return; } // defensive fallback — should never happen
    if (text) text.innerText = message;
    s_rebootConfirmHandler = onConfirm;
    s_rebootCancelHandler = onCancel || null;
    modal.classList.remove('hidden');
    modal.classList.add('flex');
}

function closeRebootConfirm() {
    const modal = document.getElementById('reboot-confirm');
    if (modal) { modal.classList.add('hidden'); modal.classList.remove('flex'); }
    s_rebootConfirmHandler = null;
    s_rebootCancelHandler = null;
}

// ----------------------------------------------------------------------
// Generic alert / confirm / prompt modals — replace every remaining
// window.alert()/confirm()/prompt() call in the app with an in-UI popup
// that matches the rest of the interface (and isn't a blocking native
// dialog that looks out of place in an installed/full-screen web app).
// Each follows the same open/close/state-handler shape as confirmReboot()
// above, just generalized: alerts take a message and optional isError
// flag, confirms take a message plus onConfirm/onCancel callbacks (same
// contract confirm() had), and the prompt takes a message, the exact
// phrase the user must type, and an onConfirm callback that only runs on
// an exact match.
// ----------------------------------------------------------------------

function showAlertModal(message, isError) {
    const modal = document.getElementById('alert-modal');
    const text = document.getElementById('alert-modal-text');
    const icon = document.getElementById('alert-modal-icon');
    const title = document.getElementById('alert-modal-title');
    if (!modal) { return; } // defensive fallback — should never happen
    if (text) text.innerText = message;
    if (icon) {
        icon.setAttribute('data-icon', isError ? 'error' : 'info');
        icon.classList.toggle('text-error', !!isError);
        icon.classList.toggle('text-primary', !isError);
    }
    if (title) title.innerText = isError ? 'Error' : 'Notice';
    modal.classList.remove('hidden');
    modal.classList.add('flex');
}

function closeAlertModal() {
    const modal = document.getElementById('alert-modal');
    if (modal) { modal.classList.add('hidden'); modal.classList.remove('flex'); }
}

let s_confirmModalHandler = null;
let s_confirmModalCancelHandler = null;

function confirmModal(message, onConfirm, onCancel, opts) {
    const modal = document.getElementById('confirm-modal');
    const text = document.getElementById('confirm-modal-text');
    const title = document.getElementById('confirm-modal-title');
    const yesBtn = document.getElementById('btn-confirm-modal-yes');
    if (!modal) { if (onConfirm) onConfirm(); return; } // defensive fallback
    if (text) text.innerText = message;
    if (title && opts && opts.title) title.innerText = opts.title;
    else if (title) title.innerText = 'Are you sure?';
    if (yesBtn && opts && opts.confirmLabel) yesBtn.innerText = opts.confirmLabel;
    else if (yesBtn) yesBtn.innerText = 'Confirm';
    s_confirmModalHandler = onConfirm;
    s_confirmModalCancelHandler = onCancel || null;
    modal.classList.remove('hidden');
    modal.classList.add('flex');
}

function closeConfirmModal() {
    const modal = document.getElementById('confirm-modal');
    if (modal) { modal.classList.add('hidden'); modal.classList.remove('flex'); }
    s_confirmModalHandler = null;
    s_confirmModalCancelHandler = null;
}

let s_promptModalHandler = null;
let s_promptModalRequiredText = null;

function promptModal(message, requiredText, onConfirm) {
    const modal = document.getElementById('prompt-modal');
    const text = document.getElementById('prompt-modal-text');
    const input = document.getElementById('prompt-modal-input');
    const confirmBtn = document.getElementById('btn-prompt-modal-confirm');
    if (!modal) { return; } // defensive fallback — should never happen
    if (text) text.innerText = message;
    if (input) input.value = '';
    if (confirmBtn) confirmBtn.disabled = true;
    s_promptModalHandler = onConfirm;
    s_promptModalRequiredText = requiredText;
    modal.classList.remove('hidden');
    modal.classList.add('flex');
    if (input) setTimeout(() => input.focus(), 50);
}

function closePromptModal() {
    const modal = document.getElementById('prompt-modal');
    const input = document.getElementById('prompt-modal-input');
    if (modal) { modal.classList.add('hidden'); modal.classList.remove('flex'); }
    if (input) input.value = '';
    s_promptModalHandler = null;
    s_promptModalRequiredText = null;
}

// Shared "actually send the reboot" action for confirmReboot()'s onConfirm
// callback — flags the next spinner cycle to read "REBOOTING DEVICE..."
// (see s_pendingRebootLabel/initWebSocket() above) before sending, so the
// disconnect that's about to happen reads as expected rather than alarming.
// Deliberately NOT routed through sendCommand(): reboot's handler
// (command_handlers.cpp) never sends an ack — it calls ESP.restart()
// directly — so waiting on one would always time out and show a false
// "failed" error. The plain websocket.send() + swallowed catch here matches
// what every pre-existing reboot call site already did.
function sendReboot() {
    s_pendingRebootLabel = true;
    if (!websocket || websocket.readyState !== WebSocket.OPEN) return;
    try { websocket.send(JSON.stringify({ command: "reboot" })); } catch (e) { /* device is about to drop the connection anyway */ }
}

// Handles the device's "auth_status" frame — the very first message sent on
// every fresh WS connection (see sendAuthStatus() in task_network.cpp). If a
// session token is already stored from a previous login, try it silently
// before ever showing the Login modal; otherwise branch straight to
// Setup/Login based on setup_required.
let lastAuthStatusSetupRequired = false;

function handleAuthStatus(msg) {
    lastAuthStatusSetupRequired = !!msg.setup_required;
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
    setAuthButtonsSubmitting(false);
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
        err.innerText = msg.error || 'Could not set password. Please try again.';
        err.classList.remove('hidden');
    } else if (lastAuthStatusSetupRequired) {
        showAuthPanel('setup');
        const err = document.getElementById('auth-setup-error');
        if (err && msg.error) {
            err.innerText = msg.error;
            err.classList.remove('hidden');
        }
    } else {
        showAuthPanel('login');
        if (loginError) {
            loginError.innerText = msg.error || 'Incorrect password. Please try again.';
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
        // cfg-admin-pass-display is intentionally NOT cleared here — it's
        // read-only and will refresh itself with the new password via the
        // "config" frame the server broadcasts right after a successful
        // change (see broadcastConfig() in handleChangePasswordCommand(),
        // auth.cpp), so clearing it would just show a blank field for a
        // moment for no reason.
        ['cfg-pass-new', 'cfg-pass-confirm'].forEach((id) => {
            const el = document.getElementById(id);
            if (el) el.value = '';
        });
    } else if (errEl) {
        errEl.innerText = msg.error || 'Could not update password.';
        errEl.classList.remove('hidden');
    }
}

// Handles the device's "logout_result" frame, sent in response to
// { command: "logout" } (see handleLogoutCommand() in auth.cpp). The server
// has already invalidated the stored session token and broadcast a fresh
// auth_status to every connection by this point, but this client's own
// localStorage token is cleared here immediately rather than waiting for
// that broadcast to arrive — otherwise handleAuthStatus() would try one
// doomed silent-reauth with the now-invalid token first (getStoredAuthToken()
// still returning the stale value), get rejected, and only then fall back to
// the login screen. Clearing it here skips straight there.
function handleLogoutResult(msg) {
    if (!msg.ok) return; // nothing to clean up client-side if the server didn't confirm
    setStoredAuthToken('');
    showAuthPanel(lastAuthStatusSetupRequired ? 'setup' : 'login');
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

// Set true for exactly one initWebSocket() cycle — the one immediately
// following a user-confirmed reboot (see confirmReboot() below) — so that
// cycle's spinner reads "REBOOTING DEVICE..." instead of the generic
// "CONNECTING...". Cleared the moment that spinner is actually shown, so an
// ordinary drop/retry afterward (e.g. the reboot's first reconnect attempt
// failing because the board is still mid-restart) falls back to the normal
// label rather than claiming "rebooting" for the rest of the backoff cycle.
let s_pendingRebootLabel = false;

function resetSpinnerLabel() {
    const label = document.getElementById('auth-spinner-label');
    if (label) label.innerText = 'CONNECTING...';
}

function initWebSocket() {
    // Every fresh connection starts unauthenticated — including reconnects —
    // so the spinner (and, once auth_status arrives, the Setup/Login modal)
    // reappears until this connection re-authenticates. This mirrors the
    // backend: authentication state lives per-WebSocket-connection, not per
    // browser tab, so a dropped/reconnected socket must prove itself again.
    showAuthPanel('spinner');
    if (s_pendingRebootLabel) {
        const label = document.getElementById('auth-spinner-label');
        if (label) label.innerText = 'REBOOTING DEVICE...';
        s_pendingRebootLabel = false;
    } else {
        resetSpinnerLabel();
    }
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
    else if (msg.type === "logout_result") handleLogoutResult(msg);
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
    // Small icon badge next to the TDS status dot — see the comment on the
    // element in index.html / tds_comp_using_fake_water_temp in state.h.
    // Only ever true while TDS itself is live (checked server-side), so no
    // extra guard needed here beyond the flag itself.
    if(document.getElementById('dash-tds-fakewt-badge')) document.getElementById('dash-tds-fakewt-badge').classList.toggle('hidden', !msg.tds_fake_wt_comp);
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
            drawChart(currentSensorCtx, currentSensorCanvas, sensorBuffers[currentTabId], currentTabId === 1 ? 'secondary' : 'primary');
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

// Tab index -> short sensor id used by save_sensor_enabled/reset_sensor_pin/
// save_sensor_demo. Module-scope (not just inside DOMContentLoaded) so both
// the per-sensor detail page's power toggle (handleToggle()), the "Reset"
// button (btn-reset-current-sensor), and the per-sensor Demo Mode toggle
// (handleSensorPageDemoToggle(), further down) share
// this one mapping instead of each maintaining their own copy.
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

    if (pinValidationBanner) pinValidationBanner.classList.toggle('hidden', !problem);
    if (pinValidationText && problem) pinValidationText.innerText = problem;

    // Save Pinout Config's disabled state has two independent inputs now
    // (Part 5.9b): a pin-validation problem always disables it regardless of
    // dirty state, but a *clean* pass no longer unconditionally re-enables
    // it — that's recomputePinoutDirty()'s job, since the button should stay
    // disabled when there's simply nothing unsaved to send. Set the error
    // half here; recomputePinoutDirty() (called right after, wherever this
    // function is invoked) reconciles the rest.
    lastPinValidationOk = problem === "";
    if (typeof recomputePinoutDirty === 'function') recomputePinoutDirty();

    return problem === "";
}
let lastPinValidationOk = true;

// ------------------------------------------------------------------
// Combined dirty-tracking for the Settings > Sensor Implementation Config
// card (Part 5.9b) — covers BOTH the 8 pin fields and the 6 per-sensor
// Enabled toggles living in the same card, saved/discarded together via
// btn-save-pins/btn-discard-pins. Mirrors the simpler single-field pattern
// used by Feature Flags (setFeaturesDirty()) and Cloud Provisioning
// further down, just checking more inputs at once. Module-scope (not just
// inside DOMContentLoaded) so updateConfigForm() can call it directly
// after a fresh config frame updates lastConfirmedSensorEnabled/pin values,
// same reasoning as validateAllPinFields() above.
// ------------------------------------------------------------------
function recomputePinoutDirty() {
    const btnSavePins = document.getElementById('btn-save-pins');
    const btnDiscardPins = document.getElementById('btn-discard-pins');
    if (!btnSavePins) return; // Settings page not in the DOM yet

    let dirty = false;
    Object.keys(PIN_FIELD_LABELS).forEach((id) => {
        const el = document.getElementById(id);
        if (el && lastConfirmedPins[id] !== undefined && parseInt(el.value, 10) !== lastConfirmedPins[id]) {
            dirty = true;
        }
    });
    Object.keys(S_EN_INDEX).forEach((sensorId) => {
        const el = document.getElementById('cfg-sensor-enabled-' + sensorId);
        if (el && lastConfirmedSensorEnabled[sensorId] !== undefined && el.checked !== lastConfirmedSensorEnabled[sensorId]) {
            dirty = true;
        }
    });

    pinoutDirty = dirty;
    // Demo Mode lock (see updateConfigForm()) always wins over dirty state —
    // don't re-enable Save just because something's unsaved if the whole
    // card is locked. A pin-validation problem also always wins.
    const demoLocked = !!(document.getElementById('pinout-demo-lock') && !document.getElementById('pinout-demo-lock').classList.contains('hidden'));
    btnSavePins.disabled = demoLocked || !lastPinValidationOk || !dirty;
    if (btnDiscardPins) btnDiscardPins.classList.toggle('hidden', !dirty);
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

    // Plaintext credentials (see broadcastConfig(), task_network.cpp, and
    // auth_get_password_for_ws(), state.cpp, for the tradeoff this
    // represents). wifi-pass/ap-pass are the same "leave blank to keep
    // current" inputs used for saving — pre-filling them means an untouched
    // Update Network click now re-submits the current password instead of
    // leaving it unchanged server-side, so save_wifi (command_handlers.cpp)
    // must treat "unchanged from msg.wifi_pass" the same as blank. cfg-fb-pass
    // gets the same treatment for consistency with cfg-fb-api just above.
    // cfg-admin-pass-display is read-only and never submitted anywhere.
    if(document.getElementById('cfg-wifi-pass')) document.getElementById('cfg-wifi-pass').value = msg.wifi_pass || "";
    if(document.getElementById('cfg-ap-pass')) document.getElementById('cfg-ap-pass').value = msg.ap_pass || "";
    if(document.getElementById('cfg-fb-pass')) document.getElementById('cfg-fb-pass').value = msg.fb_pass || "";
    if(document.getElementById('cfg-admin-pass-display')) document.getElementById('cfg-admin-pass-display').value = msg.admin_pass || "";

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

    // Feature Flags — fb_en and demo_mode are both staged now (Part 5.9,
    // see the Settings > Feature Flags / Cloud Provisioning wiring further
    // down): lastConfirmedDemo/lastConfirmedFbEnabled always track the
    // device's real value from every config frame, but the visible
    // checkbox is only overwritten when the card has no unsaved edit in
    // progress — otherwise an incoming config frame (e.g. triggered by
    // this same user's OWN save in a different card) would silently wipe
    // out a change they haven't hit Save on yet, which is exactly the kind
    // of surprise staging is meant to prevent. The sensor-page Demo Mode
    // toggles are NOT staged (see handleSensorPageDemoToggle() below) so
    // they always mirror msg.demo directly, same as before.
    lastConfirmedDemo = !!msg.demo;
    lastConfirmedFbEnabled = !!msg.fb_en;
    if (!featuresDirty && document.getElementById('cfg-demo-mode')) document.getElementById('cfg-demo-mode').checked = lastConfirmedDemo;
    if (!fbEnabledDirty && document.getElementById('cfg-fb-enabled')) document.getElementById('cfg-fb-enabled').checked = lastConfirmedFbEnabled;

    // Demo Mode toggle duplicated on the per-sensor and dual-sensor pages
    // (see handleSensorPageDemoToggle() further
    // down) — PER-SENSOR now, not the global demo_mode flag (that's what
    // cfg-demo-mode above still mirrors). Reads directly from msg.pins[]
    // rather than tabsData.gpios[] since that array isn't populated until
    // later in this same function (see the msg.pins[] parsing further
    // down) — this runs first, so it can't depend on that having already
    // happened. Order matches broadcastConfig()'s own comment (task_network.cpp):
    // TDS, DHT, pH, WaterTemp, WaterLevel, SDA, SCL, WaterLevelPower.
    // Kept in sync here (not just in switchTab()) so a page that isn't
    // currently visible doesn't show stale state if the user switches to
    // it later without another config frame arriving first. Not staged —
    // always mirrors the device's live per-sensor value directly, same as
    // the toggle always did before this was per-sensor.
    if (Array.isArray(msg.pins) && msg.pins.length >= 8) {
        const currentSensorId = TAB_TO_SENSOR_ID[currentTabId];
        const pinsBySensor = { tds: msg.pins[0], dht: msg.pins[1], ph: msg.pins[2], wt: msg.pins[3], wl: msg.pins[4], light: msg.pins[5] };
        const currentPin = currentSensorId ? pinsBySensor[currentSensorId] : undefined;
        if (document.getElementById('sensor-demo-toggle')) document.getElementById('sensor-demo-toggle').checked = (currentPin === -42);
        if (document.getElementById('dual-sensor-demo-toggle')) document.getElementById('dual-sensor-demo-toggle').checked = (currentPin === -42);
    }
    syncDemoToggleLabel('sensor-demo-toggle', 'sensor-demo-toggle-state');
    syncDemoToggleLabel('dual-sensor-demo-toggle', 'dual-sensor-demo-toggle-state');

    // Demo Mode badge on the Dashboard — shown whenever ANY sensor is
    // currently simulated (msg.demo OR any single pin at the DEMO_MODE_PIN
    // sentinel), not just while the global flag is on. This has to widen
    // now that demo state can be per-sensor (save_sensor_demo,
    // command_handlers.cpp): the badge's whole purpose is "don't mistake
    // what you're seeing for real sensor data", and with a partial demo
    // state (e.g. only TDS simulated) that warning is if anything MORE
    // important, not less — showing it only for the all-sensors case would
    // silently mislead someone looking at one faked card on an otherwise
    // real dashboard.
    const demoBadge = document.getElementById('demo-mode-badge');
    if (demoBadge) {
        const anySensorDemo = Array.isArray(msg.pins) && msg.pins.length >= 8 &&
            [0, 1, 2, 3, 4, 5, 6].some((i) => msg.pins[i] === -42);
        const showBadge = !!msg.demo || anySensorDemo;
        demoBadge.classList.toggle('hidden', !showBadge);
        demoBadge.classList.toggle('flex', showBadge);
    }

    // Pinout card lock — while Demo Mode is on, every pin field on the
    // device is pinned to the DEMO_MODE_PIN sentinel (config.h) and any edit
    // here would just be overwritten the next time Demo Mode is turned off.
    // Lock the fields and swap in the explainer banner instead of letting
    // someone edit values that don't mean anything right now. Disabling the
    // <input> also means validateAllPinFields() (which iterates
    // PIN_FIELD_LABELS via document.getElementById) still runs against
    // their last real values without user interaction reopening them.
    const pinoutLockBanner = document.getElementById('pinout-demo-lock');
    if (pinoutLockBanner) pinoutLockBanner.classList.toggle('hidden', !msg.demo);
    Object.keys(PIN_FIELD_LABELS).forEach((id) => {
        const el = document.getElementById(id);
        if (el) el.disabled = !!msg.demo;
    });
    document.querySelectorAll('[data-reset-sensor]').forEach((btn) => { btn.disabled = !!msg.demo; });
    const btnSavePinsLock = document.getElementById('btn-save-pins');
    if (btnSavePinsLock) btnSavePinsLock.disabled = !!msg.demo;
    // Per-sensor enable toggles ride along with the same demo-mode lock as
    // the pin fields — while Demo Mode is on, sensor_enabled[] is still a
    // real flag (unlike pins, it isn't pinned to a sentinel), but every
    // sensor is force-simulated regardless of this flag, so editing it here
    // would look like it did something and silently not until Demo Mode is
    // turned back off. Simplest to just lock it alongside the pins it lives
    // next to in the same card.
    document.querySelectorAll('[data-sensor-enable]').forEach((el) => { el.disabled = !!msg.demo; });

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
        // lastConfirmedSensorEnabled always tracks the device's real value
        // from every config frame, same as lastConfirmedDemo/lastConfirmedFbEnabled
        // above. The visible Settings-card checkbox is only overwritten when
        // the pinout card has no unsaved edit in progress (Part 5.9b) — see
        // recomputePinoutDirty()/btn-save-pins below. tabsData.enabled[]
        // (used by the per-sensor detail page and calibration gating) always
        // mirrors the device's real value directly regardless of staging,
        // same as before.
        Object.keys(S_EN_INDEX).forEach((sensorId) => {
            const real = !!msg.s_en[S_EN_INDEX[sensorId]];
            lastConfirmedSensorEnabled[sensorId] = real;
            const el = document.getElementById('cfg-sensor-enabled-' + sensorId);
            if (el && !pinoutDirty) el.checked = real;
        });
        if (typeof recomputePinoutDirty === 'function') recomputePinoutDirty();

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

        // lastConfirmedPins always tracks the device's real values from every
        // config frame (Part 5.9b), same pattern as lastConfirmedSensorEnabled
        // above. The visible <input> fields are only overwritten while the
        // pinout card has no unsaved edit in progress — otherwise a config
        // frame arriving mid-edit (e.g. triggered by another card's own save)
        // would silently wipe out pin values the user hasn't saved yet.
        lastConfirmedPins['cfg-pin-tds'] = msg.pins[0];
        lastConfirmedPins['cfg-pin-dht'] = msg.pins[1];
        lastConfirmedPins['cfg-pin-ph'] = msg.pins[2];
        lastConfirmedPins['cfg-pin-wt'] = msg.pins[3];
        lastConfirmedPins['cfg-pin-wl'] = msg.pins[4];
        lastConfirmedPins['cfg-pin-sda'] = msg.pins[5];
        lastConfirmedPins['cfg-pin-scl'] = msg.pins[6];
        // 8th element (added alongside pin_wl_power support) — older firmware
        // that hasn't been reflashed yet just won't send it, so guard the length.
        if (msg.pins.length >= 8) lastConfirmedPins['cfg-pin-wlp'] = msg.pins[7];

        if (!pinoutDirty) {
            Object.keys(lastConfirmedPins).forEach((id) => {
                const el = document.getElementById(id);
                if (el) el.value = lastConfirmedPins[id];
            });
        }

        // Re-run pin validation now that fresh values landed in the fields —
        // keeps the Save Pins button's disabled state in sync with reality
        // instead of whatever it was before this config frame arrived.
        // validateAllPinFields() itself calls recomputePinoutDirty() at the
        // end, so that's covered here too.
        if (typeof validateAllPinFields === 'function') validateAllPinFields();
    }
}

// Shows the "sensor is disabled" banner and hides the interactive controls
// on the Live Calibration page for whichever of TDS/pH is currently off.
// This must live at module scope because updateConfigForm() calls it whenever
// a fresh device config frame arrives.
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

    // A sensor going from enabled to disabled mid-wizard invalidates whatever
    // is in progress. The wizard state itself lives inside DOMContentLoaded,
    // so it exposes this tiny reset callback after it is initialized.
    if (!phEnabled && typeof window.resetPhWizardForGating === 'function') {
        window.resetPhWizardForGating();
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

// Smart autoscroll (Part 6.2) — how close to the bottom (in px) still counts
// as "was already following the log", vs. "deliberately scrolled up to read
// something". Generous enough to absorb sub-pixel/rounding scroll positions
// without treating a genuine scroll-up as still-at-bottom.
const TERMINAL_AUTOSCROLL_THRESHOLD_PX = 40;

function updateTerminal(msg) {
    if (isTerminalPaused) return;
    const term = document.getElementById('terminal-output');
    if(!term) return;

    // Was the view already at (or very near) the bottom BEFORE this line is
    // appended? Have to read this before appending/trimming below, since
    // both change scrollHeight out from under us.
    const wasAtBottom = (term.scrollHeight - term.scrollTop - term.clientHeight) <= TERMINAL_AUTOSCROLL_THRESHOLD_PX;

    if(term.children.length > 100) term.removeChild(term.firstChild);

    const log = document.createElement('div');
    const colorClass = msg.core === 0 ? "log-core-0" : "log-core-1";
    const levelClass = msg.level === "error" ? "text-error font-bold" : (msg.level === "warn" ? "text-secondary" : "");
    log.innerHTML = `<span class="${colorClass} opacity-80">[CORE ${msg.core}]</span> <span class="${levelClass}">${escapeHtml(msg.msg)}</span>`;
    term.appendChild(log);

    // Only follow the log automatically if the user was already at the
    // bottom — this used to force-scroll unconditionally, which yanked the
    // view away from anyone who'd scrolled up to read earlier output.
    // Someone who scrolled away sees a "New logs" pill instead (below)
    // rather than being pulled back down mid-read.
    const jumpBtn = document.getElementById('btn-term-jump-latest');
    if (wasAtBottom) {
        term.scrollTop = term.scrollHeight;
        if (jumpBtn) jumpBtn.classList.add('hidden');
    } else if (jumpBtn) {
        jumpBtn.classList.remove('hidden');
        jumpBtn.classList.add('flex');
    }
}

// ============================================================================
// 4. EVENT LISTENERS & DOM BINDING
// ============================================================================
document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    initBottomNav();
    initSensorCardLinks();
    initMobileNav();
    initWebSocket();
    setTimeout(resizeCanvas, 100);

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
            if (icon) icon.setAttribute('data-icon', revealed ? 'visibility' : 'visibility_off');
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
            if (btnAuthSetup.disabled) return;
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
            setAuthButtonsSubmitting(true, 'setup');
            websocket.send(JSON.stringify({ command: "auth", password: pass }));
        });
    }

    const btnAuthLogin = document.getElementById('btn-auth-login');
    const submitLogin = () => {
        if (btnAuthLogin && btnAuthLogin.disabled) return;
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
        setAuthButtonsSubmitting(true, 'login');
        websocket.send(JSON.stringify({ command: "auth", password: pass }));
    };
    if (btnAuthLogin) btnAuthLogin.addEventListener('click', submitLogin);
    // Enter-to-submit on both password fields, for a login flow that doesn't
    // require reaching for the mouse.
    const authLoginPassField = document.getElementById('auth-login-pass');
    if (authLoginPassField) authLoginPassField.addEventListener('keydown', (e) => { if (e.key === 'Enter') submitLogin(); });
    const authSetupPassField = document.getElementById('auth-setup-pass');
    if (authSetupPassField) authSetupPassField.addEventListener('keydown', (e) => { if (e.key === 'Enter' && btnAuthSetup) btnAuthSetup.click(); });
    const authSetupConfirmField = document.getElementById('auth-setup-pass-confirm');
    if (authSetupConfirmField) authSetupConfirmField.addEventListener('keydown', (e) => { if (e.key === 'Enter' && btnAuthSetup) btnAuthSetup.click(); });

    // ------------------------------------------------------------------
    // Reboot confirmation modal (see confirmReboot()/closeRebootConfirm()
    // above) — "Reboot Now" sends the reboot command and proactively flags
    // the next spinner cycle as a reboot (not a generic reconnect); "Not
    // Now" just closes the modal, matching confirm()'s old cancel behavior
    // (the caller's onConfirm callback simply never runs).
    // ------------------------------------------------------------------
    const btnRebootYes = document.getElementById('btn-reboot-confirm-yes');
    if (btnRebootYes) {
        btnRebootYes.addEventListener('click', () => {
            const handler = s_rebootConfirmHandler;
            closeRebootConfirm();
            if (handler) handler();
        });
    }
    const btnRebootLater = document.getElementById('btn-reboot-confirm-later');
    if (btnRebootLater) {
        btnRebootLater.addEventListener('click', () => {
            const cancelHandler = s_rebootCancelHandler;
            closeRebootConfirm();
            if (cancelHandler) cancelHandler();
        });
    }

    // ------------------------------------------------------------------
    // Generic alert modal (see showAlertModal()/closeAlertModal() above) —
    // single "OK" button, matching window.alert()'s only affordance.
    // ------------------------------------------------------------------
    const btnAlertOk = document.getElementById('btn-alert-modal-ok');
    if (btnAlertOk) btnAlertOk.addEventListener('click', closeAlertModal);

    // ------------------------------------------------------------------
    // Generic confirm modal (see confirmModal()/closeConfirmModal() above)
    // — same two-callback contract as the reboot-confirm modal above, just
    // for non-reboot yes/no decisions (logout, manual reboot, pin reset).
    // ------------------------------------------------------------------
    const btnConfirmYes = document.getElementById('btn-confirm-modal-yes');
    if (btnConfirmYes) {
        btnConfirmYes.addEventListener('click', () => {
            const handler = s_confirmModalHandler;
            closeConfirmModal();
            if (handler) handler();
        });
    }
    const btnConfirmCancel = document.getElementById('btn-confirm-modal-cancel');
    if (btnConfirmCancel) {
        btnConfirmCancel.addEventListener('click', () => {
            const cancelHandler = s_confirmModalCancelHandler;
            closeConfirmModal();
            if (cancelHandler) cancelHandler();
        });
    }

    // ------------------------------------------------------------------
    // Generic prompt modal (see promptModal()/closePromptModal() above) —
    // Confirm stays disabled until the typed text exactly matches the
    // required phrase, mirroring the manual `typed !== "RESET"` check the
    // old window.prompt()-based factory-reset flow used to do after the
    // fact (this just prevents the click instead of rejecting it after).
    // ------------------------------------------------------------------
    const promptInput = document.getElementById('prompt-modal-input');
    const btnPromptConfirm = document.getElementById('btn-prompt-modal-confirm');
    if (promptInput && btnPromptConfirm) {
        promptInput.addEventListener('input', () => {
            btnPromptConfirm.disabled = (promptInput.value !== s_promptModalRequiredText);
        });
        promptInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && !btnPromptConfirm.disabled) btnPromptConfirm.click();
        });
    }
    if (btnPromptConfirm) {
        btnPromptConfirm.addEventListener('click', () => {
            if (btnPromptConfirm.disabled) return;
            const handler = s_promptModalHandler;
            closePromptModal();
            if (handler) handler();
        });
    }
    const btnPromptCancel = document.getElementById('btn-prompt-modal-cancel');
    if (btnPromptCancel) btnPromptCancel.addEventListener('click', closePromptModal);

    // Settings > Change Password
    const btnChangePassword = document.getElementById('btn-change-password');
    if (btnChangePassword) {
        btnChangePassword.addEventListener('click', () => {
            // cfg-pass-current (a separate typed re-entry field) was
            // removed as redundant: cfg-admin-pass-display already shows
            // the device's live current password, so its value is the
            // "current" field sent to the server — no separate typed
            // field needed. See the comment on that field in index.html.
            const current = document.getElementById('cfg-admin-pass-display').value;
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
            const rawApPass = apPassEl ? apPassEl.value : '';
            // cfg-ap-pass/cfg-wifi-pass now arrive pre-filled with the
            // device's live password (updateConfigForm() -> msg.ap_pass/
            // msg.wifi_pass) so the reveal-eye has something to show. That
            // means the field is no longer blank just because the user
            // didn't touch it, so "did the user actually change this" has
            // to be a comparison against the last value the device sent us,
            // not a blank check — otherwise every save (even one that only
            // touched the SSID) would resend the current password and
            // needlessly re-trip save_wifi's changingWifiPass/changingApPass
            // branches (command_handlers.cpp) on every click.
            const newApPass = (rawApPass && rawApPass !== (globalConfigCache.ap_pass || '')) ? rawApPass : '';
            const wifiPassEl = document.getElementById('cfg-wifi-pass');
            const rawWifiPass = wifiPassEl ? wifiPassEl.value : '';
            const newWifiPass = (rawWifiPass && rawWifiPass !== (globalConfigCache.wifi_pass || '')) ? rawWifiPass : '';
            const payload = {
                command: "save_wifi",
                ssid: document.getElementById('cfg-wifi-ssid').value,
                pass: newWifiPass
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
                confirmReboot("Wi-Fi credentials saved. The ESP32 must reboot to connect with the new credentials. Reboot now?\n\nIf these credentials turn out to be wrong, the device will automatically fall back to its own \"HyGrow-Setup\" Wi-Fi network after about 15 seconds — reconnect to that network and browse to 192.168.4.1 to try again." + apPassNote, sendReboot);
            }).catch((err) => {
                btnSaveWifi.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnSaveWifi.innerText = "Update Network"; }, 3000);
            });
        });
    }

    // ------------------------------------------------------------------
    // Settings > Cloud Provisioning card — Firebase Upload toggle, staged.
    // Folded into the existing Save Credentials button rather than getting
    // its own Save button: it's the same "everything about Firebase lives
    // in this one card" grouping the card's original comment already
    // established, just extended to include staging. On click: if the
    // toggle is dirty, send save_features first (fb_en only — passing the
    // live cfg-demo-mode checkbox value keeps Demo Mode from being pulled
    // in by a save that's only about Firebase), then always send
    // save_firebase for the credential fields exactly as before. Both
    // requests share runSaveButton's disabled/"Saving…" UI on btnSaveFb;
    // if the save_features leg fails, save_firebase is skipped entirely
    // rather than saving credentials while silently leaving the toggle
    // change unsent.
    // ------------------------------------------------------------------
    const btnSaveFb = document.getElementById('btn-save-firebase');
    const btnDiscardFbEnabled = document.getElementById('btn-discard-fb-enabled');
    const cfgFbEnabled = document.getElementById('cfg-fb-enabled');

    const setFbEnabledDirty = (dirty) => {
        fbEnabledDirty = dirty;
        if (btnDiscardFbEnabled) btnDiscardFbEnabled.classList.toggle('hidden', !dirty);
    };

    if (cfgFbEnabled) {
        cfgFbEnabled.addEventListener('change', () => {
            setFbEnabledDirty(cfgFbEnabled.checked !== lastConfirmedFbEnabled);
        });
    }

    if (btnDiscardFbEnabled) {
        btnDiscardFbEnabled.addEventListener('click', () => {
            if (cfgFbEnabled) cfgFbEnabled.checked = lastConfirmedFbEnabled;
            setFbEnabledDirty(false);
        });
    }

    if(btnSaveFb) {
        btnSaveFb.addEventListener('click', () => {
            if (!validateFirebaseForm()) return;
            // cfg-fb-pass is pre-filled with the live device value the same
            // way cfg-wifi-pass/cfg-ap-pass are (see the save_wifi handler
            // above) — same "only send it if it actually changed" guard,
            // for the same reason.
            const fbPassEl = document.getElementById('cfg-fb-pass');
            const rawFbPass = fbPassEl ? fbPassEl.value : '';
            const newFbPass = (rawFbPass && rawFbPass !== (globalConfigCache.fb_pass || '')) ? rawFbPass : '';
            const payload = {
                command: "save_firebase",
                proj: document.getElementById('cfg-fb-proj').value,
                api: document.getElementById('cfg-fb-api').value,
                email: document.getElementById('cfg-fb-email').value,
                pass: newFbPass,
                col: document.getElementById('cfg-fb-col').value
            };
            const wasDirty = fbEnabledDirty;
            const sendCredentials = () => runSaveButton(btnSaveFb, payload, "Credentials Saved", "Save Credentials");
            if (wasDirty && cfgFbEnabled) {
                const original = btnSaveFb.innerText;
                btnSaveFb.disabled = true;
                btnSaveFb.innerText = 'Saving…';
                sendFeatureFlags(cfgFbEnabled, cfgDemoMode ? cfgDemoMode.checked : undefined).then(() => {
                    lastConfirmedFbEnabled = cfgFbEnabled.checked;
                    setFbEnabledDirty(false);
                    btnSaveFb.disabled = false;
                    btnSaveFb.innerText = original;
                    sendCredentials();
                }).catch((err) => {
                    btnSaveFb.disabled = false;
                    btnSaveFb.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                    setTimeout(() => { btnSaveFb.innerText = original; }, 3000);
                });
            } else {
                sendCredentials();
            }
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
    // live listeners and initial pass here. validateAllPinFields() calls
    // recomputePinoutDirty() internally (Part 5.9b), so a plain pin edit
    // updates both the error banner and the Save/Discard buttons together.
    // ------------------------------------------------------------------
    Object.keys(PIN_FIELD_LABELS).forEach((id) => {
        const el = document.getElementById(id);
        if (!el) return;
        el.addEventListener('input', validateAllPinFields);
        el.addEventListener('change', validateAllPinFields);
    });
    validateAllPinFields(); // initial pass in case fields already have values
    recomputePinoutDirty(); // initial pass — also covers the sensor-enable checkboxes

    // ------------------------------------------------------------------
    // Save Pinout Config / Discard Changes (Part 5.9b) — staged like Feature
    // Flags and Cloud Provisioning above, but this card buffers TWO kinds of
    // field together: the 8 pin number inputs (unchanged payload shape,
    // still save_pins) and the 6 per-sensor Enabled toggles (save_sensor_enabled,
    // one command per changed sensor — the server only accepts one sensor id
    // at a time, see command_handlers.cpp). Only sensors that actually
    // differ from lastConfirmedSensorEnabled are sent, not all 6
    // unconditionally, since an unrelated pin edit shouldn't also churn
    // every sensor's enabled flag.
    //
    // sendCommand() rejects a second in-flight command with the SAME
    // `command` name (see the note above sendCommand() itself) — so the
    // save_sensor_enabled calls must go out one at a time via a promise
    // chain, not Promise.all, or every one after the first would reject
    // immediately. save_pins goes last, after every dirty toggle has been
    // confirmed, so a failure partway through leaves the pins field
    // untouched on the device rather than half-applying a mixed state.
    // ------------------------------------------------------------------
    const btnSavePins = document.getElementById('btn-save-pins');
    const btnDiscardPins = document.getElementById('btn-discard-pins');

    if (btnSavePins) {
        btnSavePins.addEventListener('click', () => {
            // Re-check right before send — don't rely solely on the live
            // listener having already run (e.g. a value changed via script).
            if (!validateAllPinFields()) return;

            const dirtySensorIds = Object.keys(S_EN_INDEX).filter((sensorId) => {
                const el = document.getElementById('cfg-sensor-enabled-' + sensorId);
                return el && lastConfirmedSensorEnabled[sensorId] !== undefined && el.checked !== lastConfirmedSensorEnabled[sensorId];
            });

            const wlpEl = document.getElementById('cfg-pin-wlp');
            const pinsPayload = {
                command: "save_pins",
                pin_tds: parseInt(document.getElementById('cfg-pin-tds').value),
                pin_dht: parseInt(document.getElementById('cfg-pin-dht').value),
                pin_ph: parseInt(document.getElementById('cfg-pin-ph').value),
                pin_wt: parseInt(document.getElementById('cfg-pin-wt').value),
                pin_wl: parseInt(document.getElementById('cfg-pin-wl').value),
                pin_sda: parseInt(document.getElementById('cfg-pin-sda').value),
                pin_scl: parseInt(document.getElementById('cfg-pin-scl').value)
            };
            if (wlpEl) pinsPayload.pin_wlp = parseInt(wlpEl.value);

            const original = btnSavePins.innerText;
            btnSavePins.disabled = true;
            if (btnDiscardPins) btnDiscardPins.disabled = true;
            btnSavePins.innerText = 'Saving…';

            // Chain the dirty sensor-enable saves one after another, then
            // save_pins last. reduce() over a resolved-Promise seed is the
            // standard way to turn an array into a sequential async chain.
            dirtySensorIds.reduce((chain, sensorId) => {
                return chain.then(() => sendCommand({
                    command: "save_sensor_enabled",
                    sensor: sensorId,
                    enabled: document.getElementById('cfg-sensor-enabled-' + sensorId).checked
                }));
            }, Promise.resolve())
                .then(() => sendCommand(pinsPayload))
                .then(() => {
                    // Both kinds of field are now confirmed — pull the
                    // just-saved checkbox states into lastConfirmedSensorEnabled
                    // directly rather than waiting for the next config frame,
                    // same as how Feature Flags' Save handler above updates
                    // lastConfirmedDemo immediately after its own send.
                    dirtySensorIds.forEach((sensorId) => {
                        lastConfirmedSensorEnabled[sensorId] = document.getElementById('cfg-sensor-enabled-' + sensorId).checked;
                    });
                    Object.keys(PIN_FIELD_LABELS).forEach((id) => {
                        const el = document.getElementById(id);
                        if (el) lastConfirmedPins[id] = parseInt(el.value, 10);
                    });
                    if (btnDiscardPins) btnDiscardPins.disabled = false;
                    btnSavePins.innerText = original;
                    recomputePinoutDirty();
                    confirmReboot("Pinout saved. The ESP32 must reboot to reassign hardware interrupts safely. Reboot now?", sendReboot);
                })
                .catch((err) => {
                    if (btnDiscardPins) btnDiscardPins.disabled = false;
                    btnSavePins.disabled = false;
                    btnSavePins.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                    setTimeout(() => {
                        btnSavePins.innerText = original;
                        recomputePinoutDirty();
                    }, 3000);
                });
        });
    }

    if (btnDiscardPins) {
        btnDiscardPins.addEventListener('click', () => {
            Object.keys(PIN_FIELD_LABELS).forEach((id) => {
                const el = document.getElementById(id);
                if (el && lastConfirmedPins[id] !== undefined) el.value = lastConfirmedPins[id];
            });
            Object.keys(S_EN_INDEX).forEach((sensorId) => {
                const el = document.getElementById('cfg-sensor-enabled-' + sensorId);
                if (el && lastConfirmedSensorEnabled[sensorId] !== undefined) el.checked = lastConfirmedSensorEnabled[sensorId];
            });
            validateAllPinFields(); // clears any error highlighting from the discarded values
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
                showAlertModal("Invalid TDS readings", true);
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
            if (isNaN(livePh)) { showAlertModal("No live pH reading yet — make sure the pH sensor is enabled and the probe is connected.", true); return; }
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
            if (isNaN(livePh)) { showAlertModal("No live pH reading yet — make sure the pH sensor is enabled and the probe is connected.", true); return; }
            if (ph7Volt === null) { setPhStepUI(1); return; } // shouldn't happen, but don't let Step 2 run without Step 1
            const off = globalConfigCache.ph_off || 0.0;
            const slope = globalConfigCache.ph_slope || 1.0;
            ph4Volt = (livePh - off) / slope;

            if (ph4Volt === ph7Volt) {
                showAlertModal("The 4.0 reading matches the 7.0 reading exactly — the probe may still be in the first solution. Rinse it and place it in the pH 4.0 buffer before capturing.", true);
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
                showAlertModal("Please complete both Step 1 (pH 7.0) and Step 2 (pH 4.0) before saving.", true);
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


    const btnLogout = document.getElementById('btn-logout');
    if(btnLogout) btnLogout.addEventListener('click', () => {
        confirmModal("Log out? You'll need your password again to get back in.", () => {
            if (!websocket || websocket.readyState !== WebSocket.OPEN) {
                // No live connection to tell the device about — nothing server-side
                // to invalidate, so just drop the local token and show the login
                // screen directly rather than leaving the button silently do nothing.
                setStoredAuthToken('');
                showAuthPanel(lastAuthStatusSetupRequired ? 'setup' : 'login');
                return;
            }
            websocket.send(JSON.stringify({command: "logout"}));
        }, null, { title: 'Log Out?', confirmLabel: 'Log Out' });
    });

    const btnReboot = document.getElementById('btn-reboot');
    if(btnReboot) btnReboot.addEventListener('click', () => {
        confirmModal("Reboot the device?", () => {
            // Deliberately NOT routed through sendCommand(): reboot's handler
            // (command_handlers.cpp) calls ESP.restart() directly with no
            // sendCmdAck() on the success path — the device is gone before it
            // could send one. Waiting on an ack here would time out on every
            // single successful reboot and show a false "failed" error.
            if (!websocket || websocket.readyState !== WebSocket.OPEN) { showAlertModal("Not connected to the device right now.", true); return; }
            websocket.send(JSON.stringify({command: "reboot"}));
        }, null, { title: 'Reboot Device?', confirmLabel: 'Reboot Now' });
    });

    const btnReset = document.getElementById('btn-factory-reset');
    if(btnReset) btnReset.addEventListener('click', () => {
        // A single confirm() popup was one accidental click away from wiping
        // every setting on the device (Wi-Fi, Firebase, calibration, pins,
        // admin password — everything in state_factory_reset()). Requiring
        // the user to type the exact word "RESET" is a much stronger,
        // harder-to-fat-finger gate than a Yes/No dialog, while still being
        // a client-side UX safeguard rather than a security boundary (the
        // device itself has no way to know what the browser prompted with).
        // promptModal()'s Confirm button stays disabled until the typed text
        // is an exact match, so there's no "wrong text" branch to handle here
        // the way the old prompt()-based flow needed — only true cancel.
        promptModal(
            "This will permanently erase ALL settings on this device — Wi-Fi, Firebase credentials, calibration, pin assignments, and the admin password.\n\nThis cannot be undone.\n\nType RESET (all caps) to confirm:",
            "RESET",
            () => {
                // Same reasoning as btn-reboot above: state_factory_reset() also
                // restarts the device with no ack on the way out, so this stays a
                // raw send rather than going through sendCommand().
                if (!websocket || websocket.readyState !== WebSocket.OPEN) { showAlertModal("Not connected to the device right now.", true); return; }
                websocket.send(JSON.stringify({command: "factory_reset"}));
            }
        );
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
        // Clearing the log also clears any reason to show "New logs" —
        // nothing to jump to anymore, and the view is already at the
        // (now-empty) bottom.
        const jumpBtnOnClear = document.getElementById('btn-term-jump-latest');
        if (jumpBtnOnClear) { jumpBtnOnClear.classList.add('hidden'); jumpBtnOnClear.classList.remove('flex'); }
    });

    // Jump-to-latest pill (Part 6.2) — shown by updateTerminal() whenever a
    // new line arrives while the user has scrolled away from the bottom.
    // Tapping it catches the view up and re-arms autoscroll for the next line.
    const btnTermJumpLatest = document.getElementById('btn-term-jump-latest');
    if (btnTermJumpLatest) btnTermJumpLatest.addEventListener('click', () => {
        const term = document.getElementById('terminal-output');
        if (term) term.scrollTop = term.scrollHeight;
        btnTermJumpLatest.classList.add('hidden');
        btnTermJumpLatest.classList.remove('flex');
    });

    // Copy button (Part 6.2) — mirrors the Dashboard's Export CSV pattern,
    // just to the clipboard as plain text instead of a downloaded file
    // (a full terminal session is short enough that a file feels like
    // overkill; Clipboard API also doesn't need a download-anchor dance).
    // Reads innerText per line so the [CORE n] tag and message both come
    // through as plain text, HTML-free.
    const btnTermExport = document.getElementById('btn-term-export');
    if (btnTermExport) btnTermExport.addEventListener('click', async () => {
        const term = document.getElementById('terminal-output');
        if (!term) return;
        const lines = Array.from(term.children).map((el) => el.innerText).join('\n');
        const original = btnTermExport.innerHTML;
        try {
            await navigator.clipboard.writeText(lines);
            btnTermExport.innerHTML = '<span class="material-symbols-outlined text-[18px]" data-icon="check"></span> Copied';
        } catch (e) {
            // Clipboard API can fail (permissions, insecure context, etc.) —
            // fails visibly rather than silently, same spirit as the
            // "Not connected to the device right now." alert used elsewhere
            // in this file for other unavailable actions.
            btnTermExport.innerHTML = '<span class="material-symbols-outlined text-[18px]" data-icon="error"></span> Copy failed';
        }
        setTimeout(() => { btnTermExport.innerHTML = original; }, 2000);
    });

    // Demo Mode banner (Part 6.4) — "Demo Mode is on... Turn off Demo Mode in
    // Feature Flags to edit real pin assignments." (#pinout-demo-lock, Sensor
    // Implementation Config card). It's already a real <button> so Enter/Space
    // fire 'click' natively — no separate keydown handler needed. Both this
    // and the Feature Flags card (#settings-feature-flags-card) live on the
    // same Settings page, so this is a scrollIntoView, not a tab switch.
    const btnPinoutDemoLock = document.getElementById('pinout-demo-lock');
    if (btnPinoutDemoLock) {
        btnPinoutDemoLock.addEventListener('click', () => {
            const target = document.getElementById('settings-feature-flags-card');
            if (!target) return;
            target.scrollIntoView({ behavior: 'smooth', block: 'start' });
            // Brief highlight so it's obvious which card the click landed on —
            // .hg-highlight-flash is a one-shot CSS animation (style.css) that
            // removes itself via 'animationend' below rather than a timeout,
            // so it can't get out of sync with the actual animation duration.
            target.classList.remove('hg-highlight-flash');
            // Force reflow so re-adding the class restarts the animation if
            // the banner is tapped again before the previous flash finished.
            void target.offsetWidth;
            target.classList.add('hg-highlight-flash');
            target.addEventListener('animationend', () => {
                target.classList.remove('hg-highlight-flash');
            }, { once: true });
        });
    }

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
                showAlertModal("CSV export isn't available for this page.", true);
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
            if(!sensorBuffers[1].length) { showAlertModal("Waiting for telemetry data...", true); return; }

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
            confirmReboot(
                `Sensor enabled state changed. The ESP32 must reboot to safely apply hardware changes. Reboot now?`,
                sendReboot,
                () => {
                    e.target.checked = !isEnabled;
                    syncPowerToggleLabel(e.target.id, toggleLabelId);
                }
            );
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
        confirmModal(`Reset the '${sensorId}' pin(s) to the factory default and reboot?`, () => {
            if (!websocket || websocket.readyState !== WebSocket.OPEN) { showAlertModal("Not connected to the device right now.", true); return; }

            const onResult = (msg) => {
                if (msg.type !== "command_result" || msg.command !== "reset_sensor_pin") return;
                resetSensorPinListeners = resetSensorPinListeners.filter((fn) => fn !== onResult);
                if (!msg.ok) {
                    showAlertModal(`Pin reset failed: ${msg.error || 'the device rejected the request.'}`, true);
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
                showAlertModal("Failed to send reset request: " + e.message, true);
            }
        }, null, { title: 'Reset Pin?', confirmLabel: 'Reset & Reboot' });
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
    // Feature Flags — Demo Mode (Settings > Feature Flags card) persists via
    // save_features. Firebase Upload (Settings > Cloud Provisioning card)
    // shares the same save_features command but is staged and sent
    // separately below, alongside Save Credentials — see the
    // btn-save-firebase handler further down.
    //
    // Staged (Part 5.9): unlike the Demo Mode toggle duplicated on the
    // sensor/dual-sensor pages (which still applies immediately — see
    // handleSensorPageDemoToggle() below, unchanged from before), the
    // Settings > Feature Flags copy of this toggle only updates local
    // buffered state on change now. Nothing is sent to the device until
    // Save is clicked. This closes the gap where a live-applying toggle
    // could show "Not saved — Connection lost..." after a disconnect even
    // though the device had already applied and persisted the change
    // moments earlier, before the ack made it back.
    //
    // sendFeatureFlags() itself is unchanged — still the single function
    // that actually sends save_features, still takes an explicit demo
    // value so the sensor-page toggles (which have no cfg-fb-enabled field
    // in the DOM at all) can supply it directly. fb_en always falls back
    // to globalConfigCache.fb_en when the Settings checkbox isn't present
    // on the current page.
    // ------------------------------------------------------------------
    const sendFeatureFlags = (sourceEl, demoOverride) => {
        const cfgFbEnabledEl = document.getElementById('cfg-fb-enabled');
        const demo = demoOverride !== undefined ? !!demoOverride : !!document.getElementById('cfg-demo-mode')?.checked;
        const payload = {
            command: "save_features",
            demo,
            fb_en: cfgFbEnabledEl ? !!cfgFbEnabledEl.checked : !!globalConfigCache.fb_en
        };
        return sendCommand(payload).then((msg) => {
            // reboot_required only ever comes back true when demo_mode
            // itself just changed (see save_features, command_handlers.cpp)
            // — Firebase-only changes stay reboot-free, so this never fires
            // for the cfg-fb-enabled toggle.
            if (msg && msg.reboot_required) {
                confirmReboot(
                    `Demo Mode ${payload.demo ? "enabled" : "disabled"}. The ESP32 must reboot to safely apply this change. Reboot now?`,
                    sendReboot,
                    () => { if (sourceEl) sourceEl.checked = !sourceEl.checked; }
                );
            }
            return msg;
        });
    };

    // ------------------------------------------------------------------
    // Settings > Feature Flags card — Demo Mode toggle, staged.
    // lastConfirmedDemo/featuresDirty are declared at module scope (top of
    // file) so updateConfigForm() can also see them; this block just wires
    // the DOM listeners against that shared state.
    // ------------------------------------------------------------------
    const btnSaveFeatures = document.getElementById('btn-save-features');
    const btnDiscardFeatures = document.getElementById('btn-discard-features');

    const setFeaturesDirty = (dirty) => {
        featuresDirty = dirty;
        if (btnSaveFeatures) btnSaveFeatures.disabled = !dirty;
        if (btnDiscardFeatures) btnDiscardFeatures.classList.toggle('hidden', !dirty);
    };

    const cfgDemoMode = document.getElementById('cfg-demo-mode');
    if (cfgDemoMode) {
        cfgDemoMode.addEventListener('change', () => {
            setFeaturesDirty(cfgDemoMode.checked !== lastConfirmedDemo);
        });
    }

    if (btnSaveFeatures) {
        btnSaveFeatures.addEventListener('click', () => {
            if (!cfgDemoMode) return;
            const original = btnSaveFeatures.innerText;
            btnSaveFeatures.disabled = true;
            btnSaveFeatures.innerText = 'Saving…';
            sendFeatureFlags(cfgDemoMode, cfgDemoMode.checked).then(() => {
                lastConfirmedDemo = cfgDemoMode.checked;
                setFeaturesDirty(false);
                btnSaveFeatures.innerText = 'Saved!';
                setTimeout(() => { btnSaveFeatures.innerText = original; }, 2000);
            }).catch((err) => {
                btnSaveFeatures.disabled = false;
                btnSaveFeatures.innerText = 'Not saved — ' + (err && err.message ? err.message : 'error');
                setTimeout(() => { btnSaveFeatures.innerText = original; btnSaveFeatures.disabled = !featuresDirty; }, 3000);
                document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Feature flag save failed: ${escapeHtml(err && err.message ? err.message : 'error')}.</div>`;
            });
        });
    }

    if (btnDiscardFeatures) {
        btnDiscardFeatures.addEventListener('click', () => {
            if (cfgDemoMode) cfgDemoMode.checked = lastConfirmedDemo;
            setFeaturesDirty(false);
        });
    }

    // Demo Mode toggle duplicated on the single-sensor detail page and the
    // dual-sensor (Air Temp & Hum) page — now a PER-SENSOR toggle (see
    // save_sensor_demo, command_handlers.cpp) rather than the global
    // demo_mode flag: flipping it only simulates the ONE sensor currently
    // showing, leaving every other sensor's real/demo state untouched.
    // This is a behavior change from before — the toggle used to call
    // sendFeatureFlags (save_features, the same global switch as
    // Settings > Feature Flags), which is still what the Settings copy of
    // this toggle uses (see sendFeatureFlags() above, unchanged). Still
    // applies immediately with its own reboot-confirm dialog, matching the
    // existing "Enable Power" toggle right next to it on the same page —
    // modeled directly on handleToggle() just above (same TAB_TO_SENSOR_ID
    // lookup, same startedOnTabId staleness guard against the tab changing
    // before the WS ack returns, since #sensor-demo-toggle/
    // #dual-sensor-demo-toggle are shared DOM elements reused across every
    // tab, exactly like #sensor-toggle/#dual-sensor-toggle are) — a quick
    // contextual action on a sensor's own page, not a Settings form field,
    // so not staged like the Settings > Feature Flags copy.
    const handleSensorPageDemoToggle = (e, tabId) => {
        const demo = e.target.checked;
        const sensorId = TAB_TO_SENSOR_ID[tabId];
        if (!sensorId) return; // toggle fired on a tab with no corresponding sensor — shouldn't happen, but don't send a malformed command if it does

        const toggleLabelId = e.target.id === 'dual-sensor-demo-toggle' ? 'dual-sensor-demo-toggle-state' : 'sensor-demo-toggle-state';
        syncDemoToggleLabel(e.target.id, toggleLabelId); // immediate feedback on click, before the round-trip completes

        const startedOnTabId = tabId;
        e.target.disabled = true;
        sendCommand({ command: "save_sensor_demo", sensor: sensorId, demo }).then((msg) => {
            if (currentTabId !== startedOnTabId) {
                // Same staleness guard as handleToggle() above — user
                // navigated away before the ack came back. Still re-enable
                // the toggle so it isn't stranded disabled if they come
                // back to this tab; switchTab() already re-syncs its
                // checked state from the fresh pin data by then.
                e.target.disabled = false;
                return;
            }
            e.target.disabled = false;
            // reboot_required mirrors save_features's own convention (see
            // sendFeatureFlags() above) — only true when this sensor's demo
            // state actually changed (see command_handlers.cpp), so
            // re-toggling to the same state never pops a needless prompt.
            if (msg && msg.reboot_required) {
                confirmReboot(
                    `Demo Mode ${demo ? "enabled" : "disabled"} for this sensor. The ESP32 must reboot to safely apply this change. Reboot now?`,
                    sendReboot,
                    () => {
                        e.target.checked = !demo;
                        syncDemoToggleLabel(e.target.id, toggleLabelId);
                    }
                );
            }
        }).catch((err) => {
            document.getElementById('terminal-output').innerHTML += `<div><span class="text-secondary opacity-80">[SYS]</span> Demo Mode change failed: ${escapeHtml(err && err.message ? err.message : 'error')}.</div>`;
            e.target.disabled = false;
            if (currentTabId !== startedOnTabId) return; // stale — switchTab() already shows the real state for whatever tab is open now
            e.target.checked = !demo; // revert — the device never actually applied this
            syncDemoToggleLabel(e.target.id, toggleLabelId);
        });
    };
    const sensorDemoToggle = document.getElementById('sensor-demo-toggle');
    if (sensorDemoToggle) sensorDemoToggle.addEventListener('change', (e) => handleSensorPageDemoToggle(e, currentTabId));
    const dualSensorDemoToggle = document.getElementById('dual-sensor-demo-toggle');
    if (dualSensorDemoToggle) dualSensorDemoToggle.addEventListener('change', (e) => handleSensorPageDemoToggle(e, 2));

    // ------------------------------------------------------------------
    // Per-sensor enable toggle inside each pinout card in Settings — now
    // STAGED (Part 5.9b) and folded into the same Save Pinout Config /
    // Discard Changes pair as the pin fields below, instead of sending
    // save_sensor_enabled immediately on change. This is still the exact
    // same underlying flag as the "Enable Power" toggle on the per-sensor
    // detail page (see handleToggle() above) — that copy is UNCHANGED and
    // still applies immediately with its own reboot-confirm, matching how
    // the sensor-page Demo Mode toggle stays live while the Settings copy
    // is staged. Only this Settings-card copy is buffered now.
    //
    // lastConfirmedSensorEnabled/pinoutDirty are declared at module scope
    // (top of file) so updateConfigForm() can see them too. recomputePinoutDirty()
    // is the single source of truth for "is anything in this card unsaved" —
    // both the pin-field listeners further below and these checkboxes call
    // it on every change instead of each maintaining their own dirty flag.
    // ------------------------------------------------------------------
    document.querySelectorAll('[data-sensor-enable]').forEach((el) => {
        el.addEventListener('change', () => {
            recomputePinoutDirty();
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

    // Default to "disabled" banners showing until the first config frame's
    // tabsData.enabled[] confirms otherwise -- matches resolveSensorOn()'s
    // same fail-closed default for the per-sensor toggle before any config
    // has arrived. Moved to the end of DOMContentLoaded (was originally
    // called near the top) because it indirectly calls resetPhWizard(),
    // which touches the ph7Volt/ph4Volt/phWizardDirty `let` bindings
    // declared further down in this same handler -- calling it before
    // those declarations execute throws "Cannot access 'ph7Volt' before
    // initialization" (a TDZ error), which aborts the rest of this handler
    // and silently kills every event listener registered after it,
    // including the login/setup buttons and the eye-icon reveal toggles.
    updateCalibrationGating();
});
