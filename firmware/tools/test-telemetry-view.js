const assert = require('node:assert/strict');
const { SampleHistory, sampleState, fitPh, rowsToCsv } = require('../data/js/telemetry-view.js');
const healthy = { enabled: true, state: 'healthy', simulated: false };
const sample = (sequence, extra = {}) => ({ hardwareId: 'board-a', bootId: 'boot-a', sampleSequence: sequence,
    sampleAgeMs: 0, readIntervalMs: 2000, sampledUptimeMs: sequence * 2000,
    tds_ppm: 800, temp_c: 25, humidity: 50, water_temp_c: 22, lux: 300, wl_percent: 40, ph_val: 0, vpd_kpa: 1,
    sensorStatus: { tds: healthy, ph: healthy, dht: healthy, water_temp: healthy, light: healthy, water_level: healthy }, ...extra });
const history = new SampleHistory(3);
assert.equal(history.add(sample(1)), true);
assert.equal(history.add(sample(1, { sampleAgeMs: 1000 })), false, 'pushes cannot invent a new sensor cycle');
history.add(sample(2, { temp_c: null, sensorStatus: { ...sample(1).sensorStatus, dht: { ...healthy, state: 'failing' } } }));
assert.equal(history.add(sample(1)), false, 'late frames cannot invent old cycles');
assert.equal(history.rows[1].temp_c, null);
assert.match(rowsToCsv(history.rows), /2,4000,800,,/);
assert.equal(sampleState(sample(1), 'ph', 11000), 'stale');
assert.equal(sampleState(sample(1, { sensorStatus: { ph: { ...healthy, simulated: true } } }), 'ph', 0), 'demo');
assert.equal(sampleState(sample(0), 'ph', 0), 'waiting');
assert.equal(sampleState(sample(1, { sensorStatus: { ph: { enabled: false } } }), 'ph', 0), 'disabled');
history.add(sample(1, { bootId: 'boot-b' }));
assert.equal(history.rows.length, 1, 'a new boot starts a distinct graph window');
history.add(sample(2, { bootId: 'boot-b', sensorStatus: { ...sample(1).sensorStatus, ph: { ...healthy, simulated: true } } }));
assert.equal(history.rows.length, 1, 'real and simulated windows stay separate');
const fit = fitPh(1500, 2026);
assert.ok(Math.abs(fit.offset - 15.555) < .01, 'intercept is not a displayed pH reading');
assert.ok(Math.abs(fit.slope + 5.703) < .01);
assert.throws(() => fitPh(1500, 1500), /different/);
assert.throws(() => fitPh(null, 2026), /voltage/);
assert.match(rowsToCsv([sample(1, { tds_ppm: null })]), /board-a,boot-a,1,2000,,25/, 'non-TDS samples can be exported');
console.log('Dashboard sample deduplication, freshness, provenance, aligned null CSV, boot boundaries, and raw pH fitting passed.');
