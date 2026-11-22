import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createApp } from '../dist/app.js';

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
});
