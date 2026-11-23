import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createApp } from '../dist/app.js';
import { trustedProxiesFromEnv } from '../dist/proxy.js';

async function serve(t, pool) {
  const app = createApp({ pool, version: 'test-revision' });
  t.after(() => app.close());
  return app.listen({ port: 0, host: '127.0.0.1' });
}

test('liveness works without DB; readiness fails safely without leaking credentials', async t => {
  const base = await serve(t, { query: async () => { throw new Error('postgres://secret@private'); } });
  assert.equal((await fetch(base + '/health/live')).status, 200);
  const response = await fetch(base + '/health/ready');
  assert.equal(response.status, 503);
  assert.equal((await response.text()).includes('secret'), false);
});

test('readiness verifies database and returns the actual source version', async t => {
  const base = await serve(t, { query: async () => ({ rows: [{ ok: 1 }] }) });
  const response = await fetch(base + '/health/ready');
  assert.deepEqual(await response.json(), { service: 'hygrow-api', status: 'ready', version: 'test-revision' });
  assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
  assert.equal(response.headers.get('x-powered-by'), null);
});

test('unknown API routes return JSON 404 and never an HTML success fallback', async t => {
  const base = await serve(t, { query: async () => ({ rows: [] }) });
  const response = await fetch(base + '/api/v1/does-not-exist');
  assert.equal(response.status, 404);
  assert.equal((await response.json()).error.code, 'not_found');
  assert.match(response.headers.get('x-request-id'), /^[0-9a-f-]{36}$/);
});

test('errors carry a server-generated request ID rather than trusting client input', async t => {
  const base = await serve(t, { query: async () => ({ rows: [] }) });
  const response = await fetch(base + '/api/v1/missing', { headers: { 'x-request-id': 'untrusted-client-value' } });
  const payload = await response.json();
  assert.equal(payload.requestId, response.headers.get('x-request-id'));
  assert.notEqual(payload.requestId, 'untrusted-client-value');
  assert.match(payload.requestId, /^[0-9a-f-]{36}$/);
});

test('forwarded client addresses require an explicitly trusted immediate proxy', async t => {
  for (const trustedProxies of [undefined, ['10.23.0.4']]) {
    const app = createApp({ pool: {}, trustedProxies });
    t.after(() => app.close());
    app.get('/test-ip', request => ({ ip: request.ip }));
    const result = await app.inject({ method: 'GET', url: '/test-ip', remoteAddress: '10.23.0.5', headers: { 'x-forwarded-for': '198.51.100.7' } });
    assert.equal(result.json().ip, '10.23.0.5');
  }
  const app = createApp({ pool: {}, trustedProxies: ['10.23.0.4'] });
  t.after(() => app.close());
  app.get('/test-ip', request => ({ ip: request.ip }));
  const result = await app.inject({ method: 'GET', url: '/test-ip', remoteAddress: '10.23.0.4', headers: { 'x-forwarded-for': '203.0.113.99, 198.51.100.7' } });
  assert.equal(result.json().ip, '198.51.100.7', 'stop at the nearest untrusted hop');
});

test('proxy configuration fails closed for blanket or malformed trust', () => {
  assert.deepEqual(trustedProxiesFromEnv(undefined), []);
  assert.deepEqual(trustedProxiesFromEnv('10.23.0.4, fd00::4/128'), ['10.23.0.4', 'fd00::4/128']);
  for (const value of ['true', '1', '*', '0.0.0.0/0', '::/0', '10.0.0.1/33', '::1/129', '10.0.0.1,', 'localhost']) {
    assert.throws(() => trustedProxiesFromEnv(value), /TRUSTED_PROXIES/);
  }
});
