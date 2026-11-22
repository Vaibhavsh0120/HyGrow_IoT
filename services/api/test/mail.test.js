import assert from 'node:assert/strict';
import { test } from 'node:test';
import { seal } from '../dist/auth/crypto.js';
import { mailSettingsFromEnv, processMail } from '../dist/mail.js';

test('production never silently falls back to the development inbox', () => {
  const saved = { NODE_ENV: process.env.NODE_ENV, MAIL_MODE: process.env.MAIL_MODE, AUTH_ENCRYPTION_KEY: process.env.AUTH_ENCRYPTION_KEY };
  try {
    process.env.NODE_ENV = 'production';
    process.env.MAIL_MODE = 'mailpit';
    process.env.AUTH_ENCRYPTION_KEY = '1'.repeat(64);
    assert.throws(() => mailSettingsFromEnv(), /forbidden in production/);
  } finally {
    for (const [name, value] of Object.entries(saved)) {
      if (value === undefined) delete process.env[name]; else process.env[name] = value;
    }
  }
});

test('Brevo transport uses record idempotency, escapes names and erases delivered payload', async () => {
  const id = '00000000-0000-4000-8000-000000000001';
  const key = Buffer.alloc(32, 9);
  const message = { email: 'farmer@example.test', name: '<img src=x>', purpose: 'signup', code: '123456' };
  const statements = [];
  const client = { query: async (sql, args) => {
    statements.push(sql);
    return { rowCount: 1, rows: [{ payload: seal(message, key, id), challenge_id: id, expires_at: new Date(Date.now() + 60000), consumed_at: null }] };
  } };
  const settings = { mode: 'brevo', from: 'sender@example.test', apiKey: 'unit-test-not-a-provider-key', encryptionKey: key };
  await processMail(client, settings, id, async (url, request) => {
    assert.equal(url, 'https://api.brevo.com/v3/smtp/email');
    const body = JSON.parse(request.body);
    assert.equal(body.headers.idempotencyKey, id);
    assert.ok(body.htmlContent.includes('&lt;img src=x&gt;'));
    assert.ok(!body.htmlContent.includes('<img src=x>'));
    assert.equal(body.textContent, undefined);
    return new Response(JSON.stringify({ code: 'duplicate_parameter' }), { status: 400 });
  });
  assert.ok(statements.some(sql => sql.includes('payload=NULL,sent_at=now()')));
  await assert.rejects(processMail(client, settings, id, async () => new Response(JSON.stringify({ message: 'private-provider-detail' }), { status: 503 })), error => error.code === 'mail_transport_unavailable' && !error.message.includes('private-provider-detail'));
});
