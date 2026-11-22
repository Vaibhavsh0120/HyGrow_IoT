import { Queue, Worker, RedisConnection, createNodeRedisClient } from 'bullmq';
import { createClient } from 'redis';
import type { Pool } from 'pg';
import { transaction } from './db.js';

// BullMQ owns these connections, including closing blocking worker duplicates.
// Reuse the installed node-redis driver rather than adding another Redis client.
RedisConnection.clientFactory = options => {
  const raw = createClient({ url: options.url, username: options.username, password: options.password, database: options.db, socket: { host: options.host, port: options.port, reconnectStrategy: retries => Math.min(100 * 2 ** Math.min(retries, 6), 5000) } });
  raw.on('error', () => { /* Job lifecycle and dependency health expose safe diagnostics. */ });
  return createNodeRedisClient(raw);
};

const jobOptions = { attempts: 3, backoff: { type: 'exponential', delay: 1000 }, removeOnComplete: { age: 86400, count: 500 }, removeOnFail: { count: 1000 } };

export function createJobQueue(redisUrl: string, name = 'hygrow-background') {
  const queue = new Queue(name, { connection: { url: redisUrl }, defaultJobOptions: jobOptions });
  queue.on('error', () => console.error(JSON.stringify({ event: 'queue_connection_error', code: 'redis_unavailable' })));
  return queue;
}

export async function submitJob(pool: Pool, kind: string, id: string): Promise<void> {
  if (kind !== 'expire_otps') throw new Error('Unsupported background job');
  await pool.query('INSERT INTO background_jobs(id,kind) VALUES($1,$2) ON CONFLICT(id) DO NOTHING', [id, kind]);
}

export async function dispatchPending(pool: Pool, queue: Queue): Promise<number> {
  const jobs = await pool.query("SELECT id,kind FROM background_jobs WHERE status='pending' ORDER BY created_at LIMIT 100");
  for (const job of jobs.rows) {
    const existing = await queue.getJob(job.id);
    if (existing) {
      const state = await existing.getState();
      if (state === 'failed' || state === 'completed') {
        // Retained Redis IDs ignore queue.add. Reconcile the durable state
        // so interrupted failure recording cannot starve all newer work.
        await pool.query("UPDATE background_jobs SET status='failed',attempts=GREATEST(attempts,$2),last_error_code=$3,updated_at=now() WHERE id=$1 AND status='pending'", [job.id, existing.attemptsMade, state === 'failed' ? 'queue_attempts_exhausted' : 'job_state_requires_reconciliation']);
        continue;
      }
    }
    // Redis stores only a durable record reference, never OTPs or email bodies.
    await queue.add(job.kind, { recordId: job.id }, { jobId: job.id });
  }
  return jobs.rowCount || 0;
}

export function createJobWorker(pool: Pool, redisUrl: string, name = 'hygrow-background') {
  const worker = new Worker(name, async job => {
    const id = job.data?.recordId;
    if (typeof id !== 'string' || !/^[a-f0-9-]{36}$/.test(id)) throw new Error('invalid_job_reference');
    try {
      return await transaction(pool, async client => {
        const record = await client.query('SELECT kind,status FROM background_jobs WHERE id=$1 FOR UPDATE', [id]);
        if (!record.rowCount) throw new Error('job_record_missing');
        if (record.rows[0].status === 'completed') return { skipped: true };
        if (record.rows[0].kind !== 'expire_otps' || job.name !== 'expire_otps') throw new Error('unsupported_job');
        await client.query('UPDATE otp_challenges SET consumed_at=now() WHERE consumed_at IS NULL AND expires_at <= now()');
        await client.query("UPDATE background_jobs SET status='completed',attempts=attempts+1,completed_at=now(),updated_at=now(),last_error_code=NULL WHERE id=$1", [id]);
        return { completed: true };
      });
    } catch {
      const exhausted = job.attemptsMade + 1 >= Number(job.opts.attempts || 1);
      await pool.query('UPDATE background_jobs SET status=$2,attempts=attempts+1,last_error_code=$3,updated_at=now() WHERE id=$1', [id, exhausted ? 'failed' : 'pending', 'job_execution_failed']);
      throw new Error('job_execution_failed');
    }
  }, { connection: { url: redisUrl }, concurrency: 2 });
  worker.on('error', () => console.error(JSON.stringify({ event: 'worker_connection_error', code: 'dependency_unavailable' })));
  worker.on('failed', job => console.error(JSON.stringify({ event: 'job_failed', jobId: job?.id, attempts: job?.attemptsMade, code: 'job_execution_failed' })));
  return worker;
}
