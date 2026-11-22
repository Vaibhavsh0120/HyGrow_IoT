(function (root) {
    'use strict';
    const fields = ['tds_ppm', 'temp_c', 'humidity', 'water_temp_c', 'lux', 'wl_percent', 'ph_val', 'vpd_kpa'];
    const owners = ['tds', 'dht', 'dht', 'water_temp', 'light', 'water_level', 'ph', 'dht'];
    function sampleState(sample, name, elapsedMs = 0) {
        const health = sample?.sensorStatus?.[name];
        if (!health) return 'waiting';
        if (!health.enabled) return 'disabled';
        if (!sample.sampleSequence || health.state === 'waiting') return 'waiting';
        if (health.state === 'failing') return 'error';
        const threshold = Math.max(10000, 3 * (sample.readIntervalMs || 2000));
        if (!Number.isFinite(sample.sampleAgeMs) || sample.sampleAgeMs + Math.max(0, elapsedMs) > threshold) return 'stale';
        return health.simulated ? 'demo' : 'live';
    }
    class SampleHistory {
        constructor(limit = 20) { this.limit = limit; this.rows = []; this.key = ''; this.boundary = ''; this.identity = ''; this.sequence = 0; }
        add(sample) {
            if (!sample?.sampleSequence || !sample.bootId) return false;
            const key = `${sample.hardwareId}:${sample.bootId}:${sample.sampleSequence}`;
            const identity = `${sample.hardwareId}:${sample.bootId}`;
            if (identity === this.identity && sample.sampleSequence <= this.sequence) return false;
            const boundary = `${sample.hardwareId}:${sample.bootId}:` + owners.map(name => !!sample.sensorStatus?.[name]?.simulated).join(',');
            if (this.boundary && boundary !== this.boundary) this.rows = [];
            this.boundary = boundary;
            this.key = key;
            this.identity = identity;
            this.sequence = sample.sampleSequence;
            const row = { hardwareId: sample.hardwareId, bootId: sample.bootId, sampleSequence: sample.sampleSequence,
                sampledUptimeMs: sample.sampledUptimeMs, sensorStatus: sample.sensorStatus };
            fields.forEach((field, i) => {
                const state = sampleState(sample, owners[i]);
                row[field] = ['live', 'demo'].includes(state) && Number.isFinite(sample[field]) ? sample[field] : null;
            });
            this.rows.push(row);
            if (this.rows.length > this.limit) this.rows.shift();
            return true;
        }
    }
    function fitPh(ph7MilliVolts, ph4MilliVolts) {
        if (![ph7MilliVolts, ph4MilliVolts].every(value => Number.isFinite(value) && value > 0 && value <= 3300)) throw new Error('A fresh raw probe voltage is required.');
        const difference = (ph7MilliVolts - ph4MilliVolts) / 1000;
        if (Math.abs(difference) < .005) throw new Error('The two buffer voltages must be different.');
        const slope = 3 / difference;
        const offset = 7 - slope * ph7MilliVolts / 1000;
        if (Math.abs(slope) > 20 || Math.abs(offset) > 100) throw new Error('Buffer readings produced an unsupported calibration.');
        return { slope, offset };
    }
    function csvCell(value) {
        if (value == null || (typeof value === 'number' && !Number.isFinite(value))) return '';
        let text = String(value);
        if (typeof value === 'string' && /^[=+\-@]/.test(text)) text = "'" + text;
        return /[",\r\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
    }
    function rowsToCsv(rows) {
        const headers = ['hardwareId', 'bootId', 'sampleSequence', 'sampledUptimeMs', ...fields, 'sensorStatus'];
        return headers.join(',') + '\n' + rows.map(row => [...headers.slice(0, -1).map(name => csvCell(row[name])), csvCell(JSON.stringify(row.sensorStatus || {}))].join(',')).join('\n') + '\n';
    }
    const api = { SampleHistory, sampleState, fitPh, rowsToCsv };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.HyGrowTelemetry = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
