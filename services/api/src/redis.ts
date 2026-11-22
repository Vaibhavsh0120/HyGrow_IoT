import { createClient } from 'redis';

export async function createRedis(url: string) {
  const client = createClient({ url, disableOfflineQueue: true, socket: { connectTimeout: 5000, reconnectStrategy: retries => Math.min(100 * 2 ** Math.min(retries, 6), 5000) } });
  client.on('error', () => { /* Health/failed requests report availability; never log connection strings. */ });
  await client.connect();
  return client;
}

export type RedisConnection = Awaited<ReturnType<typeof createRedis>>;

const counterScript = `
local total = redis.call('INCR', KEYS[1])
if total == 1 then redis.call('EXPIRE', KEYS[1], ARGV[2]) end
return {total, redis.call('PTTL', KEYS[1])}
`;

export async function allowRequest(redis: RedisConnection, key: string, limit: number, windowSeconds: number) {
  const result = await redis.eval(counterScript, { keys: [key], arguments: [String(limit), String(windowSeconds)] }) as number[];
  return { allowed: result[0] <= limit, retryAfterMs: result[0] <= limit ? 0 : Math.max(1, result[1]) };
}
