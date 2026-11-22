import assert from 'node:assert/strict';
import { createHmac, randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { createPool } from '../dist/db.js';
import { migrate } from '../dist/migrate.js';
import { createRedis } from '../dist/redis.js';
import { createApp } from '../dist/app.js';
import { AuthService } from '../dist/auth/service.js';
import { open } from '../dist/auth/crypto.js';

const url = process.env.HYGROW_TEST_DATABASE_URL;
const redisUrl = process.env.HYGROW_TEST_REDIS_URL;

async function fixture(t) {
  const parsed = new URL(url);
  assert.ok(['localhost', '127.0.0.1', 'db-test'].includes(parsed.hostname));
  assert.equal(parsed.pathname, '/hygrow_test');
  const pool = createPool(url);
  await migrate(pool);
  const settings = { otpPepper: Buffer.alloc(32, 11), mail: { mode: 'mailpit', url: 'http://127.0.0.1:8026', from: 'noreply@hygrow.test', encryptionKey: Buffer.alloc(32, 12) } };
  const service = new AuthService(pool, settings);
  const emails = [];
  t.after(async () => {
    await pool.query('DELETE FROM background_jobs WHERE id IN (SELECT m.id FROM mail_outbox m JOIN users u ON u.id=m.user_id WHERE u.email=ANY($1))', [emails]);
    await pool.query('DELETE FROM users WHERE email=ANY($1)', [emails]);
    await pool.end();
  });
  const email = () => { const address = `security-${randomUUID()}@example.test`; emails.push(address); return address; };
  const code = async id => {
    const result = await pool.query('SELECT id,payload FROM mail_outbox WHERE challenge_id=$1', [id]);
    return open(result.rows[0].payload, settings.mail.encryptionKey, result.rows[0].id).code;
  };
  return { pool, service, settings, email, code };
}

test('OTP attempt exhaustion, expiry and purpose binding persist safely', { skip: !url }, async t => {
  const f = await fixture(t);
  const id = await f.service.signup(f.email(), 'Farmer', 'test-long-passphrase');
  const correct = await f.code(id);
  const wrong = correct === '000000' ? '000001' : '000000';
  for (let attempt = 0; attempt < 5; attempt++) await assert.rejects(f.service.verify(id, wrong), error => error.code === 'invalid_otp');
  await assert.rejects(f.service.verify(id, correct), error => error.code === 'invalid_otp');
  const exhausted = (await f.pool.query('SELECT attempts,consumed_at FROM otp_challenges WHERE id=$1', [id])).rows[0];
  assert.equal(exhausted.attempts, 5);
  assert.ok(exhausted.consumed_at);
  const expired = await f.service.signup(f.email(), 'Farmer', 'test-long-passphrase');
  const expiredCode = await f.code(expired);
  await f.pool.query("UPDATE otp_challenges SET expires_at=now()-interval '1 second' WHERE id=$1", [expired]);
  await assert.rejects(f.service.verify(expired, expiredCode), error => error.code === 'invalid_otp');
});

test('resend supersedes old code and concurrent logout/refresh cannot leave a live family', { skip: !url }, async t => {
  const f = await fixture(t);
  const id = await f.service.signup(f.email(), 'Farmer', 'test-long-passphrase');
  const oldCode = await f.code(id);
  await assert.rejects(f.service.resend(id, 'signup'), error => error.statusCode === 429);
  await f.pool.query("UPDATE otp_challenges SET created_at=now()-interval '61 seconds' WHERE id=$1", [id]);
  const newer = await f.service.resend(id, 'signup');
  await assert.rejects(f.service.verify(id, oldCode), error => error.code === 'invalid_otp');
  const session = await f.service.verify(newer, await f.code(newer));
  await Promise.allSettled([f.service.refresh(session.refreshToken), f.service.logout(session.accessToken)]);
  assert.equal((await f.pool.query('SELECT id FROM sessions WHERE user_id=$1 AND revoked_at IS NULL', [session.user.id])).rowCount, 0);
});

test('concurrent reset and refresh cannot mint a session surviving reset', { skip: !url }, async t => {
  const f = await fixture(t);
  const address = f.email();
  const id = await f.service.signup(address, 'Farmer', 'test-long-passphrase');
  const session = await f.service.verify(id, await f.code(id));
  const resetId = await f.service.requestReset(address);
  const resetCode = await f.code(resetId);
  const outcomes = await Promise.allSettled([f.service.refresh(session.refreshToken), f.service.reset(resetId, resetCode, 'changed-long-passphrase')]);
  assert.equal(outcomes[1].status, 'fulfilled');
  assert.equal((await f.pool.query('SELECT id FROM sessions WHERE user_id=$1 AND revoked_at IS NULL', [session.user.id])).rowCount, 0);
  await assert.rejects(f.service.login(address, 'test-long-passphrase'), error => error.code === 'invalid_credentials');
  assert.equal((await f.service.login(address, 'changed-long-passphrase')).user.id, session.user.id);
});

test('known/unknown account requests and resend cooldown have the same public shape', { skip: !url || !redisUrl }, async t => {
  const f = await fixture(t);
  const existing = f.email();
  const firstId = await f.service.signup(existing, 'Farmer', 'test-long-passphrase');
  await f.service.verify(firstId, await f.code(firstId));
  const redis = await createRedis(redisUrl);
  const app = createApp({ pool: f.pool, redis, auth: f.settings });
  t.after(async () => { await app.close(); await redis.quit(); });
  const signup = address => app.inject({ method: 'POST', url: '/api/v1/auth/signup', payload: { email: address, displayName: 'Farmer', password: 'test-long-passphrase', role: 'farmer' } });
  const known = await signup(existing);
  const unknown = await signup(f.email());
  assert.equal(known.statusCode, 202);
  assert.equal(unknown.statusCode, 202);
  assert.deepEqual(Object.keys(known.json()), Object.keys(unknown.json()));
  assert.equal(known.json().message, unknown.json().message);
  const address = (await f.pool.query('SELECT email,created_at FROM users WHERE id=(SELECT user_id FROM otp_challenges WHERE id=$1)', [unknown.json().challengeId])).rows[0].email;
  const createdAt = (await f.pool.query('SELECT created_at FROM otp_challenges WHERE id=$1', [unknown.json().challengeId])).rows[0].created_at;
  const opaque = createHmac('sha256', f.settings.otpPepper).update('auth-budget:' + address).digest('hex');
  const ttl = await redis.pTTL('hygrow:rate:auth:otp-minute:' + opaque);
  const databaseRemaining = 60000 - (Date.now() - new Date(createdAt).getTime());
  assert.ok(ttl >= databaseRemaining - 15, 'The public cooldown must cover the full database cooldown, including signup hashing time');
  for (const response of [known, unknown]) {
    const resend = await app.inject({ method: 'POST', url: '/api/v1/auth/otp/resend', payload: { challengeId: response.json().challengeId, purpose: 'signup' } });
    assert.equal(resend.statusCode, 429);
    assert.equal(resend.json().error.code, 'rate_limited');
    assert.ok(Number(resend.headers['retry-after']) > 0);
  }
  const waiting = (await f.pool.query('SELECT payload FROM mail_outbox WHERE payload IS NOT NULL LIMIT 1')).rows[0].payload;
  assert.ok(!waiting.includes('@example.test'));
});

test('resending an older signup preserves its own chosen password and profile', { skip: !url }, async t => {
  const f = await fixture(t);
  const address = f.email();
  const victim = await f.service.signup(address, 'Victim Farmer', 'victim-long-passphrase');
  await f.pool.query("UPDATE otp_challenges SET created_at=now()-interval '61 seconds' WHERE id=$1", [victim]);
  const attacker = await f.service.signup(address, 'Other Name', 'attacker-long-passphrase');
  await f.pool.query("UPDATE otp_challenges SET created_at=now()-interval '61 seconds' WHERE id=$1", [attacker]);
  const resent = await f.service.resend(victim, 'signup');
  await f.service.verify(resent, await f.code(resent));
  const legitimate = await f.service.login(address, 'victim-long-passphrase');
  assert.equal(legitimate.user.displayName, 'Victim Farmer');
  await assert.rejects(f.service.login(address, 'attacker-long-passphrase'), error => error.code === 'invalid_credentials');
});
