// Real DOM checks; all device traffic is mocked. No GPIO/Firebase requests.
// Requires Playwright, optionally supplied via HYGROW_PLAYWRIGHT.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { chromium } = require(process.env.HYGROW_PLAYWRIGHT || 'playwright');
const output = process.env.HYGROW_QA_DIR || path.join(os.tmpdir(), 'hygrow-web-qa');
fs.mkdirSync(output, { recursive: true });

(async () => {
    const browser = await chromium.launch(fs.existsSync(chromium.executablePath()) ? { headless: true } : { channel: 'msedge', headless: true });
    try {
        const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
        page.setDefaultTimeout(10000);
        const errors = [];
        const commands = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.addInitScript(() => localStorage.setItem('hygrow_auth_token', 'browser-test-token'));
        await page.routeWebSocket('**/ws', ws => {
            ws.onMessage(frame => {
                const msg = JSON.parse(frame);
                commands.push(msg);
                if (msg.command === 'auth') ws.send(JSON.stringify({ type: 'auth_result', ok: true, token: 'browser-test-token' }));
            });
            setTimeout(() => ws.send(JSON.stringify({ type: 'auth_status', setup_required: false, boot_id: 701 })), 40);
        });
        await page.goto(process.env.HYGROW_QA_URL || 'http://127.0.0.1:8766/');
        await page.waitForFunction(() => document.getElementById('auth-overlay').classList.contains('hidden'));
        await page.evaluate(() => switchTab(9));
        await page.evaluate(() => {
            onMessage({ data: JSON.stringify({ type: 'log_batch', entries: [
                { logBootId: 'test-boot', sequence: 1, uptimeMs: 1000, core: 0, level: 'info', msg: 'Wi-Fi connected. Dashboard ready at http://192.168.0.24/' },
                { logBootId: 'test-boot', sequence: 2, uptimeMs: 2200, core: 1, level: 'info', msg: 'Water temperature sensor ready.' },
                { logBootId: 'test-boot', sequence: 3, uptimeMs: 3000, core: 1, level: 'warn', msg: 'Light sensor unavailable. Check SDA and SCL wiring, then reboot.' },
                { logBootId: 'test-boot', sequence: 4, uptimeMs: 3400, core: 0, level: 'error', msg: 'Cloud upload failed. Check the saved credentials in Settings.' },
                { logBootId: 'test-boot', sequence: 5, uptimeMs: 4400, core: 0, level: 'error', msg: 'Cloud upload failed. Check the saved credentials in Settings.' },
                { logBootId: 'test-boot', sequence: 6, uptimeMs: 5000, core: 0, level: 'info', msg: '<img src=x onerror=alert(1)> should remain plain text. ' + 'long-device-id-'.repeat(16) },
            ] }) });
        });
        assert.equal(await page.locator('.terminal-row').count(), 5);
        assert.equal(await page.locator('#terminal-output img').count(), 0);
        assert.match(await page.locator('#terminal-output').innerText(), /Repeated 2 times/);
        await page.locator('#terminal-level').selectOption('issues');
        assert.equal(await page.locator('.terminal-row').count(), 2);
        await page.locator('#terminal-search').fill('cloud');
        assert.equal(await page.locator('.terminal-row').count(), 1);
        await page.locator('#terminal-search').fill('absent-message');
        assert.match(await page.locator('#terminal-output').innerText(), /No matching messages/);
        await page.locator('#terminal-search').fill('');
        await page.locator('#terminal-level').selectOption('all');
        await page.locator('#btn-term-pause').click();
        await page.evaluate(() => updateTerminal({ level: 'warn', msg: 'Message received during pause.', uptimeMs: 6000 }));
        assert.doesNotMatch(await page.locator('#terminal-output').innerText(), /received during pause/);
        assert.match(await page.locator('#terminal-summary').innerText(), /1 new message/);
        await page.locator('#btn-term-pause').click();
        assert.match(await page.locator('#terminal-output').innerText(), /received during pause/);
        // HTTP LAN copy fallback, including cleanup and visible outcome.
        await page.evaluate(() => {
            Object.defineProperty(navigator, 'clipboard', { configurable: true, value: undefined });
            window.testCopiedText = '';
            document.execCommand = () => { window.testCopiedText = document.querySelector('textarea').value; return true; };
        });
        await page.locator('#btn-term-export').click();
        assert.match(await page.locator('#btn-term-export').innerText(), /Copied/);
        assert.match(await page.evaluate(() => window.testCopiedText), /00:00:01.*Wi-Fi connected/);
        assert.equal(await page.locator('textarea').count(), 0);
        const states = [];
        const captureFile = process.env.HYGROW_SPACING_SCRIPT;
        for (const theme of ['dark', 'light']) {
            await page.evaluate(value => {
                const select = document.getElementById('cfg-theme-select');
                select.value = value;
                select.dispatchEvent(new Event('change'));
            }, theme);
            for (const width of [390, 1440, 1728]) {
                await page.setViewportSize({ width, height: width === 390 ? 844 : 1000 });
                await page.waitForFunction(() => Math.abs(parseFloat(getComputedStyle(document.querySelector('main')).marginLeft) - (innerWidth < 1024 ? 0 : 320)) < 1);
                await page.evaluate(() => document.fonts.ready);
                await page.screenshot({ path: path.join(output, `terminal-${theme}-${width}.png`), fullPage: true });
                const geometry = await page.evaluate(() => ({
                    bodyWidth: document.documentElement.scrollWidth, viewport: innerWidth,
                    logWidth: document.getElementById('terminal-output').scrollWidth,
                    logClientWidth: document.getElementById('terminal-output').clientWidth,
                    font: getComputedStyle(document.getElementById('terminal-output')).fontFamily,
                }));
                assert.ok(geometry.bodyWidth <= width + 1, `page overflow at ${theme}/${width}`);
                assert.ok(geometry.logWidth <= geometry.logClientWidth + 1, `log overflow at ${theme}/${width}`);
                assert.match(geometry.font, /JetBrains/);
                if (captureFile) {
                    await page.evaluate(() => { window.__name = fn => fn; });
                    const state = await page.evaluate(fs.readFileSync(captureFile, 'utf8'));
                    // Intentionally 1px-clipped accessible labels are not
                    // visible layout. Retain every visible control/icon/row.
                    states.push({ ...state, elements: state.elements.filter(el => !(el.rect.width <= 1 && el.rect.height <= 1 && el.style?.position === 'absolute')),
                        name: `terminal-${theme}-${width}` });
                }
            }
        }
        fs.writeFileSync(path.join(output, 'spacing-states.json'), JSON.stringify(states));
        await page.locator('#btn-term-clear').click();
        assert.equal(await page.locator('.terminal-row').count(), 0);
        assert.match(await page.locator('#terminal-output').innerText(), /Log cleared/);
        // A real hidden ancestor must not conceal authenticated app dialogs.
        await page.evaluate(() => confirmModal('Review dialog visibility.', () => {}));
        assert.equal(await page.locator('#confirm-modal').isVisible(), true, 'authenticated confirmation must be visible');
        await page.locator('#btn-confirm-modal-cancel').click();
        // Dialog keyboard paths, focus containment, and typed destructive guard.
        await page.locator('#btn-term-clear').focus();
        await page.evaluate(() => confirmModal('Keyboard confirmation.', () => {}));
        assert.equal(await page.locator('#btn-confirm-modal-cancel').evaluate(el => el === document.activeElement), true);
        await page.keyboard.press('Shift+Tab');
        assert.equal(await page.locator('#btn-confirm-modal-yes').evaluate(el => el === document.activeElement), true);
        await page.keyboard.press('Tab');
        assert.equal(await page.locator('#btn-confirm-modal-cancel').evaluate(el => el === document.activeElement), true);
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('#confirm-modal').isVisible(), false);
        assert.equal(await page.locator('#btn-term-clear').evaluate(el => el === document.activeElement), true);
        await page.evaluate(() => promptModal('Type RESET to proceed.', 'RESET', () => {}));
        assert.equal(await page.locator('#prompt-modal').isVisible(), true);
        assert.equal(await page.locator('#btn-prompt-modal-confirm').isDisabled(), true);
        await page.locator('#prompt-modal-input').fill('RESET');
        assert.equal(await page.locator('#btn-prompt-modal-confirm').isDisabled(), false);
        await page.keyboard.press('Escape');
        await page.evaluate(() => confirmReboot('Settings saved.', () => {}));
        assert.equal(await page.locator('#reboot-confirm').isVisible(), true);
        await page.keyboard.press('Escape');
        await page.evaluate(() => showAlertModal('Read-only test alert.'));
        assert.equal(await page.locator('#alert-modal').isVisible(), true);
        await page.keyboard.press('Escape');

        const health = { enabled: true, state: 'healthy', simulated: false };
        const sample = {
            type: 'data', hardwareId: 'browser-board', bootId: 'browser-boot', sampleSequence: 1,
            sampleAgeMs: 0, sampledUptimeMs: 2000, readIntervalMs: 2000,
            tds_ppm: null, temp_c: null, humidity: null, water_temp_c: 22, lux: 300, wl_percent: 50, ph_val: null, ph_voltage_mv: null, vpd_kpa: null,
            tds: null, temp: null, hum: null, w_t: 22,
            sensorStatus: { tds: { ...health, enabled: false }, dht: { ...health, state: 'failing' }, water_temp: health, light: health, water_level: health, ph: { ...health, enabled: false } },
        };
        const send = msg => page.evaluate(value => onMessage({ data: JSON.stringify(value) }), msg);
        await send({ type: 'config', demo: false, fb_en: false, pins: [2,6,7,4,1,8,9,5], s_en: [true,true,true,true,true,true],
            int_read: 2000, int_ws: 1000, int_vit: 2000, int_fb: 30000 });
        await send(sample);
        await send({ type: 'vitals', rssi: -60, free_heap: 210000, uptime: 60, wifi_status: 'connected',
            firebase_ready: false, firebase_last_error: 'Cloud unavailable. Check the saved credentials and internet connection.' });
        for (const id of [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]) {
            await page.evaluate(index => switchTab(index), id);
            assert.equal(await page.locator(id >= 1 && id <= 6 ? (id === 2 ? '#page-dual-sensor' : '#page-sensor') : `#page-${id}`).isVisible(), true, `tab ${id}`);
        }
        // Check the whole page shell, not only the terminal, at every target.
        for (const theme of ['dark', 'light']) {
            await page.evaluate(() => switchTab(8));
            await page.locator('#cfg-theme-select').selectOption(theme);
            for (const width of [390, 1440, 1728]) {
                await page.setViewportSize({ width, height: width === 390 ? 844 : 1000 });
                await page.waitForFunction(() => Math.abs(parseFloat(getComputedStyle(document.querySelector('main')).marginLeft) - (innerWidth < 1024 ? 0 : 320)) < 1);
                for (let id = 0; id <= 9; id++) {
                    await page.evaluate(index => { switchTab(index); document.querySelector('main').scrollTop = 0; }, id);
                    assert.equal(await page.evaluate(() => currentTabId), id);
                    assert.equal(await page.locator(`#nav-tabs [data-id="${id}"]`).getAttribute('aria-selected'), 'true');
                    const overflow = await page.evaluate(() => {
                        const main = document.querySelector('main');
                        return main.scrollWidth - main.clientWidth;
                    });
                    assert.ok(overflow <= 1, `tab ${id} overflows at ${theme}/${width}: ${overflow}px`);
                    if ([0, 7, 8].includes(id)) await page.screenshot({ path: path.join(output, `page-${id}-${theme}-${width}.png`) });
                }
            }
        }
        await page.evaluate(() => { switchTab(0); showConnectionNotice('Read-only notice layout check.'); });
        const noticeOverlaps = await page.evaluate(() => document.getElementById('connection-notice').getBoundingClientRect().bottom > document.querySelector('#page-0 header').getBoundingClientRect().top);
        assert.equal(noticeOverlaps, false, 'connection notices must not cover the page heading');
        await send({ ...sample, sampleAgeMs: 1000 });
        assert.equal(await page.evaluate(() => sampleHistory.rows.length), 1, 'repeat pushes cannot duplicate a sample');
        await page.evaluate(() => switchTab(0));
        const downloadEvent = page.waitForEvent('download');
        await page.locator('#btn-export-csv').click();
        const download = await downloadEvent;
        const csv = fs.readFileSync(await download.path(), 'utf8');
        assert.match(csv, /hardwareId,bootId,sampleSequence,sampledUptimeMs/);
        assert.match(csv, /browser-board,browser-boot,1,2000,,,,22,300,50,,,/);
        assert.match(csv, /sensorStatus/);

        // Start from the factory pH clamp (0 pH) while raw voltage is 1.5V.
        const live = { ...sample, sampleSequence: 2, sampleAgeMs: 0, ph_val: 0, ph_voltage_mv: 1500,
            sensorStatus: { ...sample.sensorStatus, ph: health } };
        await send(live);
        await page.evaluate(() => switchTab(7));
        assert.equal(await page.locator('#cal-ph-raw').innerText(), '1.500 V');
        await page.locator('#btn-cal-ph-7').click();
        await page.locator('#btn-cal-ph-4').click();
        assert.match(await page.locator('#alert-modal').innerText(), /new sensor sample/);
        await page.keyboard.press('Escape');
        await send({ ...live, sampleSequence: 3, ph_val: 0, ph_voltage_mv: 2026 });
        await page.locator('#btn-cal-ph-4').click();
        await page.locator('#btn-cal-ph-save').click();
        await page.waitForFunction(() => pendingCommands.some(p => p.command === 'calibrate_ph'));
        await page.waitForTimeout(40);
        const calibration = commands.find(msg => msg.command === 'calibrate_ph');
        assert.ok(calibration.offset > 14 && Math.abs(calibration.slope + 5.703422) < .00001);
        await send({ type: 'command_result', command: 'calibrate_ph', ok: true });
        assert.match(await page.locator('#btn-cal-ph-save').innerText(), /Saved/);
        await send({ ...live, sampleSequence: 4, sampleAgeMs: 11000 });
        assert.match(await page.locator('#cal-ph-unavailable-text').innerText(), /out of date/);
        assert.equal(await page.locator('#ph-wizard-controls').isVisible(), false);
        await send({ ...live, sampleSequence: 5 });
        await page.evaluate(() => { lastTelemetryReceivedAt -= 11000; });
        await page.waitForFunction(() => !document.getElementById('cal-ph-disabled-banner').classList.contains('hidden'));
        assert.match(await page.locator('#cal-ph-unavailable-text').innerText(), /out of date/);
        await send({ ...live, sampleSequence: 5, sampleAgeMs: 0 });
        assert.equal(await page.evaluate(() => sensorStatusForTab(6)), 'stale', 'repeat pushes cannot revive a stale cycle');
        await send({ ...live, sampleSequence: 6, sensorStatus: { ...live.sensorStatus, ph: { ...health, simulated: true } } });
        assert.match(await page.locator('#cal-ph-unavailable-text').innerText(), /simulated/);
        assert.equal(await page.evaluate(() => sampleHistory.rows.length), 1, 'simulation switches start a separate graph window');

        // Edits made after submitting must remain unsaved after its ack.
        await page.evaluate(() => switchTab(8));
        await page.locator('#cfg-demo-mode').locator('..').click();
        assert.equal(await page.locator('#cfg-demo-mode').isChecked(), true);
        await page.locator('#btn-save-features').click();
        await page.locator('#cfg-demo-mode').focus();
        await page.keyboard.press('Space');
        assert.equal(await page.locator('#btn-save-features').isDisabled(), true);
        await send({ type: 'command_result', command: 'save_features', ok: true });
        assert.equal(await page.evaluate(() => featuresDirty), true);
        assert.equal(await page.locator('#cfg-demo-mode').isChecked(), false);
        await page.locator('#btn-discard-features').click();
        await send({ type: 'config', demo: false, pins: [2,6,7,4,1,8,9,5], s_en: [true,true,true,true,true,true] });
        await page.locator('#cfg-pin-ph').fill('10');
        await page.locator('#btn-save-pins').click();
        await page.locator('#cfg-pin-ph').fill('11');
        assert.equal(await page.locator('#btn-save-pins').isDisabled(), true);
        await send({ type: 'command_result', command: 'save_pins', ok: true });
        assert.equal(await page.evaluate(() => lastConfirmedPins['cfg-pin-ph']), 10);
        assert.equal(await page.evaluate(() => pinoutDirty), true);
        await page.keyboard.press('Escape');
        await page.locator('#btn-discard-pins').click();
        await page.locator('[data-reset-sensor="ph"]').click();
        await page.locator('#btn-confirm-modal-yes').click();
        await send({ type: 'command_result', command: 'reset_sensor_pin', ok: false, error: 'Synthetic reset rejection.' });
        assert.equal(await page.evaluate(() => deviceAuthenticated), true);
        assert.equal(await page.locator('#alert-modal').isVisible(), true);
        await page.keyboard.press('Escape');

        const unnamed = await page.evaluate(() => Array.from(document.querySelectorAll('input:not([type="hidden"]),select,textarea')).filter(el =>
            !el.getAttribute('aria-label') && !el.getAttribute('aria-labelledby') &&
            !(el.labels && Array.from(el.labels).some(label => label.textContent.trim()))).map(el => el.id));
        assert.deepEqual(unnamed, [], 'form controls need accessible names');
        assert.deepEqual(errors, [], 'dashboard scripts must not throw');
        console.log('Browser terminal, HTTP copy, six layouts, dialogs/keyboard, all tabs/null readings, CSV, raw pH/stale/demo gating, pending edits, reset rejection, and control labels passed.');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
