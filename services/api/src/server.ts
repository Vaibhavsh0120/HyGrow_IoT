import { createApp } from './app.js';
import { createPool } from './db.js';
import { createRedis } from './redis.js';
import { authSettingsFromEnv } from './auth/service.js';

if (!process.env.REDIS_URL) throw new Error('REDIS_URL is required');
const pool = createPool();
const redis = await createRedis(process.env.REDIS_URL);
const app = createApp({ pool, redis, auth: authSettingsFromEnv(), version: process.env.SOURCE_REVISION || 'development', logging: true });
app.addHook('onClose', async () => { await redis.quit(); await pool.end(); });
await app.listen({ port: Number(process.env.PORT || 8080), host: '0.0.0.0' });
console.log('HyGrow API started');
let stopping = false;
async function stop() {
  if (stopping) return;
  stopping = true;
  const timeout = setTimeout(() => process.exit(1), 10000).unref();
  await app.close();
  clearTimeout(timeout);
}
process.on('SIGTERM', stop);
process.on('SIGINT', stop);
