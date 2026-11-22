import { createHash } from 'node:crypto';
import { createPool } from './db.js';
import { createRedis } from './redis.js';
import { createJobQueue, createJobWorker, submitJob, dispatchPending } from './jobs.js';

if (!process.env.REDIS_URL) throw new Error('REDIS_URL is required');
const pool = createPool();
const redis = await createRedis(process.env.REDIS_URL);
const queue = createJobQueue(process.env.REDIS_URL);
const worker = createJobWorker(pool, process.env.REDIS_URL);
let stopping = false;
let timer: ReturnType<typeof setTimeout>;
let running: Promise<void> | undefined;

async function tick() {
  try {
    // Stable per-minute ID permits safe reconciliation by multiple worker instances.
    const hash = createHash('sha256').update(`expire_otps:${Math.floor(Date.now() / 60000)}`).digest('hex').slice(0, 32);
    const id = `${hash.slice(0, 8)}-${hash.slice(8, 12)}-${hash.slice(12, 16)}-${hash.slice(16, 20)}-${hash.slice(20)}`;
    await submitJob(pool, 'expire_otps', id);
    await dispatchPending(pool, queue);
    await redis.set('hygrow:worker:heartbeat', process.env.SOURCE_REVISION || 'development', { EX: 60 });
  } catch {
    console.error(JSON.stringify({ event: 'job_dispatch_failed', code: 'dependency_unavailable' }));
  } finally {
    if (!stopping) timer = setTimeout(() => { running = tick(); }, 10000);
  }
}
running = tick();
async function stop() {
  if (stopping) return;
  stopping = true;
  clearTimeout(timer);
  const deadline = setTimeout(() => process.exit(1), 15000).unref();
  await running;
  await worker.close();
  await queue.close();
  await redis.quit();
  await pool.end();
  clearTimeout(deadline);
}
process.on('SIGTERM', stop);
process.on('SIGINT', stop);
