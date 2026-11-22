import Fastify from 'fastify';
import type { Pool } from 'pg';
import { createHash } from 'node:crypto';
import { allowRequest, type RedisConnection } from './redis.js';

export type AppDependencies = { pool: Pool; redis?: RedisConnection; version?: string };

export function createApp({ pool, redis, version = 'development' }: AppDependencies) {
  const app = Fastify({ logger: false, bodyLimit: 65536, requestTimeout: 15000, ajv: { customOptions: { removeAdditional: false, coerceTypes: false } } });
  app.addHook('onSend', async (request, reply) => {
    reply.header('X-Content-Type-Options', 'nosniff');
    reply.header('Referrer-Policy', 'same-origin');
    reply.header('Cache-Control', 'no-store');
  });
  app.addHook('onRequest', async (request, reply) => {
    if (!request.url.startsWith('/api/') || !redis) return;
    try {
      const key = createHash('sha256').update(request.ip).digest('hex');
      const result = await allowRequest(redis, `hygrow:rate:api:${key}`, 120, 60);
      if (!result.allowed) return reply.code(429).header('Retry-After', Math.ceil(result.retryAfterMs / 1000)).send({ error: { code: 'rate_limited', message: 'Please retry later' } });
    } catch {
      return reply.code(503).send({ error: { code: 'temporarily_unavailable', message: 'Please retry later' } });
    }
  });
  app.get('/health/live', async () => ({ service: 'hygrow-api', status: 'alive', version }));
  app.get('/health/ready', async (request, reply) => {
    try {
      await pool.query('SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 1');
      if (redis) await redis.ping();
      return { service: 'hygrow-api', status: 'ready', version };
    } catch {
      return reply.code(503).send({ service: 'hygrow-api', status: 'unavailable' });
    }
  });
  app.setNotFoundHandler(async (request, reply) => reply.code(404).send({ error: { code: 'not_found', message: 'Route not found' } }));
  app.setErrorHandler((error, request, reply) => {
    const code = typeof error === 'object' && error !== null && 'statusCode' in error ? Number(error.statusCode) : 500;
    const status = code >= 400 && code < 500 ? code : 500;
    reply.code(status).send({ error: { code: status === 500 ? 'internal_error' : 'invalid_request', message: status === 500 ? 'Request could not be completed' : 'Invalid request' } });
  });
  return app;
}
