/* Bounded log data, shared by the dashboard and host checks. No DOM dependency. */
(function (root) {
    'use strict';
    function formatUptime(ms) {
        if (!Number.isFinite(ms) || ms < 0) return '—';
        const seconds = Math.floor(ms / 1000);
        return [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60]
            .map(value => String(value).padStart(2, '0')).join(':');
    }
    const levelNames = { info: 'Info', warn: 'Warning', error: 'Error' };
    function formatLog(entry) {
        return `${formatUptime(entry.uptimeMs)}  ${levelNames[entry.level]}  ${entry.source}  ${entry.msg}` +
            (entry.count > 1 ? ` (repeated ${entry.count} times)` : '');
    }
    class LogBuffer {
        constructor(limit = 200) { this.limit = limit; this.entries = []; this.records = []; this.seen = new Set(); }
        add(raw) {
            if (!raw || typeof raw.msg !== 'string' || !raw.msg.trim()) return false;
            const id = typeof raw.logBootId === 'string' && Number.isSafeInteger(raw.sequence)
                ? `${raw.logBootId}:${raw.sequence}` : null;
            if (id && this.seen.has(id)) return false;
            if (id) {
                this.seen.add(id);
                while (this.seen.size > this.limit * 4) this.seen.delete(this.seen.values().next().value);
            }
            const entry = {
                msg: raw.msg.trim(), level: levelNames[raw.level] ? raw.level : 'info',
                source: raw.source === 'Dashboard' ? 'Dashboard' : (raw.core === 1 ? 'Sensors' : 'Device'),
                uptimeMs: Number.isFinite(raw.uptimeMs) && raw.uptimeMs >= 0 ? raw.uptimeMs : null,
                logBootId: raw.logBootId || '', sequence: id ? raw.sequence : null, count: 1,
            };
            this.records.push(entry);
            // Core producers and login replay can arrive out of order. Sort
            // device slots within each boot; local dashboard events keep their
            // positions because they have no comparable device clock.
            const boots = new Map();
            this.records.forEach(record => {
                if (record.sequence === null) return;
                if (!boots.has(record.logBootId)) boots.set(record.logBootId, []);
                boots.get(record.logBootId).push(record);
            });
            boots.forEach(records => records.sort((a, b) => a.sequence - b.sequence));
            const offsets = new Map();
            this.records = this.records.map(record => {
                if (record.sequence === null) return record;
                const index = offsets.get(record.logBootId) || 0;
                offsets.set(record.logBootId, index + 1);
                return boots.get(record.logBootId)[index];
            }).slice(-this.limit * 4);
            this.entries = [];
            this.records.forEach(record => {
                const last = this.entries.at(-1);
                if (last && last.msg === record.msg && last.level === record.level &&
                    last.source === record.source && last.logBootId === record.logBootId) {
                    last.count++;
                    last.uptimeMs = record.uptimeMs;
                } else this.entries.push({ ...record });
            });
            this.entries = this.entries.slice(-this.limit);
            return true;
        }
        matching(level = 'all', query = '') {
            const text = query.trim().toLowerCase();
            return this.entries.filter(entry =>
                (level === 'all' || (level === 'issues' ? entry.level !== 'info' : entry.level === level)) &&
                (!text || `${entry.msg} ${entry.source} ${levelNames[entry.level]}`.toLowerCase().includes(text)));
        }
        clear() { this.entries = []; this.records = []; this.seen.clear(); }
    }
    const api = { LogBuffer, formatUptime, formatLog, levelNames };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.HyGrowTerminal = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
