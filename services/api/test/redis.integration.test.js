import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { createRedis, allowRequest } from '../dist/redis.js';

test('Redis rate limit is shared, atomic under concurrency and expires', { skip: !process.env.HYGROW_TEST_REDIS_URL }, async t => {
  const url = process.env.HYGROW_TEST_REDIS_URL;
  assert.ok(['127.0.0.1','localhost'].includes(new URL(url).hostname));
  const first = await createRedis(url);
  const second = await createRedis(url);
  t.after(async () => { await first.quit(); await second.quit(); });
  const key = `hygrow:test:${randomUUID()}`;
  const results = await Promise.all(Array.from({ length: 20 }, (_, i) => allowRequest(i % 2 ? first : second, key, 5, 1)));
  assert.equal(results.filter(r => r.allowed).length, 5);
  assert.ok(results.filter(r => !r.allowed).every(r => r.retryAfterMs > 0));
  assert.ok(await first.pTTL(key) > 0);
  await new Promise(resolve => setTimeout(resolve, 1100));
  assert.equal((await allowRequest(second, key, 5, 1)).allowed, true);
  await first.del(key);
});
