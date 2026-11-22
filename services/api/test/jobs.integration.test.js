import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { createPool } from '../dist/db.js';
import { migrate } from '../dist/migrate.js';
import { createJobQueue, dispatchPending, submitJob, createJobWorker } from '../dist/jobs.js';
import { Worker } from 'bullmq';

const database = process.env.HYGROW_TEST_DATABASE_URL;
const redis = process.env.HYGROW_TEST_REDIS_URL;

test('BullMQ dispatches durable jobs once and recovers pending records', { skip: !database || !redis }, async t => {
  const parsed = new URL(database);
  assert.equal(parsed.pathname, '/hygrow_test');
  assert.ok(['localhost','127.0.0.1'].includes(parsed.hostname));
  const pool = createPool(database);
  const queue = createJobQueue(redis, `hygrow-test-${randomUUID()}`);
  await migrate(pool);
  const worker = createJobWorker(pool, redis, queue.name);
  t.after(async () => { await worker.close(); await queue.obliterate({ force: true }); await queue.close(); await pool.end(); });
  const userId = randomUUID();
  const challengeId = randomUUID();
  await pool.query('INSERT INTO users(id,email,display_name) VALUES($1,$2,$3)', [userId, `${userId}@example.test`, 'Job test']);
  await pool.query("INSERT INTO otp_challenges(id,user_id,purpose,code_hash,expires_at) VALUES($1,$2,'signup','fixture',now()-interval '1 minute')", [challengeId, userId]);
  const jobId = randomUUID();
  await submitJob(pool, 'expire_otps', jobId);
  await dispatchPending(pool, queue);
  await dispatchPending(pool, queue);
  const deadline = Date.now() + 10000;
  let status;
  while (Date.now() < deadline) {
    status = (await pool.query('SELECT status FROM background_jobs WHERE id=$1', [jobId])).rows[0].status;
    if (status === 'completed') break;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  assert.equal(status, 'completed');
  assert.ok((await pool.query('SELECT consumed_at FROM otp_challenges WHERE id=$1', [challengeId])).rows[0].consumed_at);
  assert.equal((await pool.query('SELECT attempts FROM background_jobs WHERE id=$1', [jobId])).rows[0].attempts, 1);
  assert.equal((await queue.getJobs(['completed'])).length, 1);
  await assert.rejects(submitJob(pool, 'arbitrary_shell', randomUUID()));
  await pool.query('DELETE FROM background_jobs WHERE id=$1', [jobId]);
  await pool.query('DELETE FROM users WHERE id=$1', [userId]);
});

test('terminal Redis failure cannot strand a pending database record', { skip: !database || !redis }, async t => {
  const pool = createPool(database);
  const queue = createJobQueue(redis, `hygrow-reconcile-${randomUUID()}`);
  const worker = new Worker(queue.name, async () => { throw new Error('simulated_database_outage'); }, { connection: { url: redis } });
  worker.on('error', () => {});
  t.after(async () => { await worker.close(); await queue.obliterate({ force: true }); await queue.close(); await pool.end(); });
  await migrate(pool);
  const id = randomUUID();
  await submitJob(pool, 'expire_otps', id);
  const job = await queue.add('expire_otps', { recordId: id }, { jobId: id, attempts: 1 });
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline && await job.getState() !== 'failed') await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(await job.getState(), 'failed');
  await dispatchPending(pool, queue);
  const record = (await pool.query('SELECT status,last_error_code FROM background_jobs WHERE id=$1', [id])).rows[0];
  assert.equal(record.status, 'failed');
  assert.equal(record.last_error_code, 'queue_attempts_exhausted');
  await pool.query('DELETE FROM background_jobs WHERE id=$1', [id]);
});
