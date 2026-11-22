import assert from 'node:assert/strict';
import { createHmac, randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { createApp } from '../dist/app.js';
import { createPool } from '../dist/db.js';
import { migrate } from '../dist/migrate.js';
import { createRedis } from '../dist/redis.js';
import { createJobQueue, createJobWorker, dispatchPending } from '../dist/jobs.js';

const database = process.env.HYGROW_TEST_DATABASE_URL;
const redisUrl = process.env.HYGROW_TEST_REDIS_URL;
const mailpitUrl = process.env.HYGROW_TEST_MAILPIT_URL;

test('real email signup, one-time OTP, rotating sessions, replay defense and password reset', { skip: !database || !redisUrl || !mailpitUrl }, async t => {
  const parsed = new URL(database);
  assert.ok(['localhost', '127.0.0.1', 'db-test'].includes(parsed.hostname));
  assert.equal(parsed.pathname, '/hygrow_test');
  assert.ok(['localhost', '127.0.0.1', 'mailpit-test'].includes(new URL(mailpitUrl).hostname));
  const pool = createPool(database);
  const redis = await createRedis(redisUrl);
  await migrate(pool);
  const mail = { mode: 'mailpit', url: mailpitUrl, from: 'noreply@hygrow.test', encryptionKey: Buffer.alloc(32, 7) };
  const auth = { otpPepper: Buffer.alloc(32, 8), mail };
  const app = createApp({ pool, redis, auth });
  const email = `auth-${randomUUID()}@example.test`;
  let queue, worker;
  t.after(async () => {
    await app.close();
    if (worker) await worker.close();
    if (queue) { await queue.obliterate({ force: true }); await queue.close(); }
    await pool.query('DELETE FROM background_jobs WHERE id IN (SELECT m.id FROM mail_outbox m JOIN users u ON u.id=m.user_id WHERE u.email=$1)', [email]);
    await pool.query('DELETE FROM users WHERE email=$1', [email]);
    await redis.quit();
    await pool.end();
  });
  const post = (path, payload, access) => app.inject({ method: 'POST', url: '/api/v1' + path, payload, headers: access ? { authorization: 'Bearer ' + access } : {} });
  const signedUp = await post('/auth/signup', { email, displayName: 'Test Farmer', password: 'first-long-passphrase', role: 'farmer' });
  assert.equal(signedUp.statusCode, 202, signedUp.body);
  const { challengeId } = signedUp.json();
  assert.match(challengeId, /^[0-9a-f-]{36}$/);
  assert.ok(!signedUp.body.includes('codeHash'));
  assert.equal((await post('/auth/login', { email, password: 'first-long-passphrase' })).statusCode, 401);
  const queueName = `auth-test-${randomUUID()}`;
  queue = createJobQueue(redisUrl, queueName);
  worker = createJobWorker(pool, redisUrl, queueName, mail);
  async function deliverAndReadCode(subject) {
    await dispatchPending(pool, queue);
    for (let attempt = 0; attempt < 100; attempt++) {
      const messages = await (await fetch(mailpitUrl + '/api/v1/messages')).json();
      const message = messages.messages.find(item => item.Subject === subject && item.To?.some(to => to.Address === email));
      if (message) {
        const content = await (await fetch(mailpitUrl + '/api/v1/message/' + message.ID)).json();
        const code = content.Text.match(/\b\d{6}\b/);
        if (code) return { code: code[0], messageId: message.ID };
      }
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error('Expected local test email was not delivered');
  }
  const first = await deliverAndReadCode('HyGrow verification code');
  assert.equal((await post('/auth/verify', { challengeId, code: 'not-a-code' })).statusCode, 400);
  const verified = await post('/auth/verify', { challengeId, code: first.code });
  assert.equal(verified.statusCode, 200, verified.body);
  const session = verified.json();
  assert.equal(session.user.email, email);
  assert.deepEqual(session.user.roles, ['farmer']);
  assert.equal((await post('/auth/verify', { challengeId, code: first.code })).statusCode, 400);
  const stored = await pool.query('SELECT access_hash,refresh_hash FROM sessions WHERE user_id=$1', [session.user.id]);
  assert.notEqual(stored.rows[0].access_hash, session.accessToken);
  assert.notEqual(stored.rows[0].refresh_hash, session.refreshToken);
  const me = await app.inject({ url: '/api/v1/me', headers: { authorization: 'Bearer ' + session.accessToken } });
  assert.equal(me.statusCode, 200);
  assert.ok(!me.body.includes('password_hash'));
  const rotated = await post('/auth/refresh', { refreshToken: session.refreshToken });
  assert.equal(rotated.statusCode, 200, rotated.body);
  assert.equal((await post('/auth/refresh', { refreshToken: session.refreshToken })).statusCode, 401);
  assert.equal((await app.inject({ url: '/api/v1/me', headers: { authorization: 'Bearer ' + rotated.json().accessToken } })).statusCode, 401, 'Replay must revoke the rotated family too');
  const loggedIn = await post('/auth/login', { email, password: 'first-long-passphrase' });
  assert.equal(loggedIn.statusCode, 200);
  assert.equal((await post('/auth/password/request', { email })).statusCode, 429, 'OTP cooldown is shared across purposes');
  // Expire only this account's disposable Redis fixture instead of sleeping a minute.
  const key = createHmac('sha256', auth.otpPepper).update('auth-budget:' + email).digest('hex');
  await redis.del('hygrow:rate:auth:otp-minute:' + key);
  const reset = await post('/auth/password/request', { email });
  assert.equal(reset.statusCode, 202);
  const second = await deliverAndReadCode('HyGrow password reset code');
  assert.equal((await post('/auth/verify', { challengeId: reset.json().challengeId, code: second.code })).statusCode, 400, 'Reset OTP cannot enroll an account');
  const resetResult = await post('/auth/password/reset', { challengeId: reset.json().challengeId, code: second.code, password: 'second-long-passphrase' });
  assert.equal(resetResult.statusCode, 200, resetResult.body);
  assert.equal((await app.inject({ url: '/api/v1/me', headers: { authorization: 'Bearer ' + loggedIn.json().accessToken } })).statusCode, 401);
  assert.equal((await post('/auth/login', { email, password: 'first-long-passphrase' })).statusCode, 401);
  const recoveredLogin = await post('/auth/login', { email, password: 'second-long-passphrase' });
  assert.equal(recoveredLogin.statusCode, 200);
  const logout = await app.inject({ method: 'POST', url: '/api/v1/auth/logout', headers: { authorization: 'Bearer ' + recoveredLogin.json().accessToken } });
  assert.equal(logout.statusCode, 200, logout.body);
  assert.equal((await app.inject({ url: '/api/v1/me', headers: { authorization: 'Bearer ' + recoveredLogin.json().accessToken } })).statusCode, 401);
  const reuse = await post('/auth/password/reset', { challengeId: reset.json().challengeId, code: second.code, password: 'another-long-passphrase' });
  assert.equal(reuse.statusCode, 400);
});
