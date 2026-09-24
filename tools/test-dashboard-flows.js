const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class Classes {
    constructor() { this.values = new Set(['hidden']); }
    add(...names) { names.forEach((name) => this.values.add(name)); }
    remove(...names) { names.forEach((name) => this.values.delete(name)); }
    contains(name) { return this.values.has(name); }
    toggle(name, force) {
        const add = force === undefined ? !this.contains(name) : force;
        if (add) this.add(name); else this.remove(name);
        return add;
    }
    replace(oldName, newName) { this.remove(oldName); this.add(newName); }
}

const elements = new Map();
function element(id) {
    if (!elements.has(id)) {
        elements.set(id, {
            id, classList: new Classes(), dataset: {}, value: '', innerText: '', disabled: false,
            checked: false, getContext: () => ({}), addEventListener: () => {},
            setAttribute: () => {},
        });
    }
    return elements.get(id);
}

const tokens = new Map();
const sent = [];
class FakeWebSocket {
    static OPEN = 1;
    constructor() { this.readyState = FakeWebSocket.OPEN; }
    send(payload) { sent.push(JSON.parse(payload)); }
    close() { this.readyState = 3; }
}

const context = vm.createContext({
    document: { body: { classList: new Classes() }, getElementById: element, addEventListener: () => {}, querySelectorAll: () => [] },
    window: { location: { hostname: 'hygrow.local' }, addEventListener: () => {} },
    localStorage: {
        getItem: (key) => tokens.get(key) || null,
        setItem: (key, value) => tokens.set(key, value),
        removeItem: (key) => tokens.delete(key),
    },
    WebSocket: FakeWebSocket,
    setTimeout: () => 1,
    clearTimeout: () => {},
    console,
});
const app = fs.readFileSync(path.join(__dirname, '..', 'data', 'js', 'app.js'), 'utf8');
vm.runInContext(app, context);
vm.runInContext('websocket = new WebSocket()', context);

vm.runInContext('offlinePreviewDismissed = true', context);
context.showAuthPanel('spinner');
assert.equal(element('auth-overlay').classList.contains('hidden'), true);
assert.equal(element('offline-preview-bar').classList.contains('hidden'), false);
context.showAuthPanel('login');
assert.equal(element('auth-overlay').classList.contains('hidden'), false);
assert.equal(element('offline-preview-bar').classList.contains('hidden'), true);

// A stale browser token after flashing a fresh device must not look like a
// failed password that nobody entered.
tokens.set('hygrow_auth_token', 'old-token');
context.handleAuthStatus({ setup_required: true, boot_id: 101 });
assert.equal(sent.at(-1).token, 'old-token');
context.handleAuthResult({ ok: false, error: 'Incorrect password.' });
assert.equal(element('auth-setup').classList.contains('hidden'), false);
assert.equal(element('auth-setup-error').classList.contains('hidden'), true);
assert.equal(tokens.has('hygrow_auth_token'), false);

context.handleAuthStatus({ setup_required: false, boot_id: 101 });
context.handleAuthResult({ ok: false, error: 'Incorrect password.' });
assert.equal(element('auth-login-error').classList.contains('hidden'), false);

context.handleAuthStatus({ setup_required: false, boot_id: 101 });
context.handleAuthResult({ ok: true, token: 'new-token' });
assert.equal(context.sendReboot(), true);
assert.equal(element('auth-spinner-label').innerText, 'REBOOTING DEVICE...');
context.handleAuthStatus({ setup_required: false, boot_id: 102 });
context.handleAuthResult({ ok: true });
assert.equal(element('connection-notice').innerText, 'Device restarted and reconnected.');
assert.equal(element('vital-link-text').innerText, 'RECONNECTED');

vm.runInContext('websocket.readyState = 3', context);
assert.equal(context.sendReboot(), false);
assert.equal(element('alert-modal').classList.contains('hidden'), false);

const pins = {
    'cfg-pin-tds': 2, 'cfg-pin-dht': 6, 'cfg-pin-ph': 7, 'cfg-pin-wt': 4,
    'cfg-pin-wl': 1, 'cfg-pin-wlp': 5, 'cfg-pin-sda': 8, 'cfg-pin-scl': 9,
};
for (const [id, pin] of Object.entries(pins)) element(id).value = String(pin);
assert.equal(context.validateAllPinFields(), true);
element('cfg-pin-tds').value = '18';
assert.equal(context.validateAllPinFields(), false); // analog signal requires ADC1
element('cfg-pin-tds').value = '22';
assert.equal(context.validateAllPinFields(), false); // no exposed GPIO22
element('cfg-pin-tds').value = '';
assert.equal(context.validateAllPinFields(), false); // blank cannot become zero
element('cfg-pin-tds').value = '2';
assert.equal(context.validateAllPinFields(), true);

// Settings edits stay visible outside Settings and survive a device config
// refresh. The user can discard each group from the global marker.
vm.runInContext("hasConfigSnapshot = true; globalConfigCache = { wifi_ssid: 'Old Wi-Fi', wifi_pass: 'secret', ap_pass: 'recovery' };", context);
element('cfg-wifi-ssid').value = 'New Wi-Fi';
element('cfg-wifi-pass').value = 'secret';
element('cfg-ap-pass').value = 'recovery';
context.updateUnsavedChanges();
assert.equal(element('unsaved-widget').classList.contains('hidden'), false);
assert.equal(element('unsaved-row-network').classList.contains('hidden'), false);
assert.equal(element('unsaved-count').innerText, '1 unsaved section');
context.discardSettingsGroup('network');
assert.equal(element('cfg-wifi-ssid').value, 'Old Wi-Fi');
assert.equal(element('unsaved-widget').classList.contains('hidden'), true);

vm.runInContext('deviceAuthenticated = true; tabsData.enabled[1] = true; tabsData.gpios[1] = 2; tabsData.ok[1] = 1; lastTelemetry = { tds: 812 };', context);
context.refreshSensorStatuses();
context.renderCurrentReadings();
assert.equal(element('dash-dot-tds').classList.contains('hg-dot-live'), true);
assert.equal(element('dash-val-tds').innerText, '812');
vm.runInContext('tabsData.gpios[1] = -42', context);
context.refreshSensorStatuses();
assert.equal(element('dash-dot-tds').classList.contains('hg-dot-demo'), true);
vm.runInContext('tabsData.ok[1] = 2', context);
context.refreshSensorStatuses();
context.renderCurrentReadings();
assert.equal(element('dash-dot-tds').classList.contains('hg-dot-error'), true);
assert.equal(element('dash-val-tds').innerText, '--');
vm.runInContext('tabsData.ok[1] = 0', context);
context.refreshSensorStatuses();
assert.equal(element('dash-dot-tds').classList.contains('hg-dot-muted'), true);
vm.runInContext('deviceAuthenticated = false', context);
context.refreshSensorStatuses();
assert.equal(element('dash-dot-tds').title, 'Device offline');

console.log('Dashboard auth, reboot, pin, unsaved-settings, and sensor-state checks passed.');
