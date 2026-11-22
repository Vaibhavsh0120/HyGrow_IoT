import Fastify, { LogController } from 'fastify';
import type { Pool } from 'pg';
import { createHash, randomUUID } from 'node:crypto';
import { allowRequest, type RedisConnection } from './redis.js';
import { registerAuth } from './auth/routes.js';
import type { AuthSettings } from './auth/service.js';
import { ApiError } from './errors.js';

export type AppDependencies = { pool: Pool; redis?: RedisConnection; version?: string; logging?: boolean; auth?: AuthSettings };

export function createApp({ pool, redis, auth, version = 'development', logging = false }: AppDependencies) {
  const app = Fastify({ logger: logging, logController: new LogController({ disableRequestLogging: true }), genReqId: () => randomUUID(), requestIdHeader: false, bodyLimit: 65536, requestTimeout: 15000, ajv: { customOptions: { removeAdditional: false, coerceTypes: false } } });
  app.addHook('onSend', async (request, reply) => {
    reply.header('X-Content-Type-Options', 'nosniff');
    reply.header('Referrer-Policy', 'same-origin');
    reply.header('Cache-Control', 'no-store');
    reply.header('X-Request-ID', request.id);
    reply.header('X-HyGrow-Version', version);
  });
  app.addHook('onResponse', async (request, reply) => {
    request.log.info({ event: 'http_response', route: request.routeOptions.url || 'unmatched', method: request.method, status: reply.statusCode, durationMs: reply.elapsedTime, version });
  });
  app.addHook('onRequest', async (request, reply) => {
    if (!request.url.startsWith('/api/') || !redis) return;
    try {
      const key = createHash('sha256').update(request.ip).digest('hex');
      const result = await allowRequest(redis, `hygrow:rate:api:${key}`, 120, 60);
      if (!result.allowed) return reply.code(429).header('Retry-After', Math.ceil(result.retryAfterMs / 1000)).send({ error: { code: 'rate_limited', message: 'Please retry later' }, requestId: request.id });
    } catch {
      return reply.code(503).send({ error: { code: 'temporarily_unavailable', message: 'Please retry later' }, requestId: request.id });
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
  app.get('/health/worker', async (request, reply) => {
    try {
      if (!redis || !await redis.exists('hygrow:worker:heartbeat')) return reply.code(503).send({ service: 'hygrow-worker', status: 'unavailable' });
      return { service: 'hygrow-worker', status: 'ready' };
    } catch {
      return reply.code(503).send({ service: 'hygrow-worker', status: 'unavailable' });
    }
  });
  app.setNotFoundHandler(async (request, reply) => reply.code(404).send({ error: { code: 'not_found', message: 'Route not found' }, requestId: request.id }));
  app.setErrorHandler((error, request, reply) => {
    if (error instanceof ApiError) {
      request.log.warn({ event: 'request_rejected', code: error.code, route: request.routeOptions.url || 'unmatched', version });
      if (error.retryAfterSeconds) reply.header('Retry-After', error.retryAfterSeconds);
      return reply.code(error.statusCode).send({ error: { code: error.code, message: error.message }, requestId: request.id });
    }
    const code = typeof error === 'object' && error !== null && 'statusCode' in error ? Number(error.statusCode) : 500;
    const status = code >= 400 && code < 500 ? code : 500;
    request.log.error({ event: 'http_error', status, route: request.routeOptions.url || 'unmatched', version });
    reply.code(status).send({ error: { code: status === 500 ? 'internal_error' : 'invalid_request', message: status === 500 ? 'Request could not be completed' : 'Invalid request' }, requestId: request.id });
  });
  if (auth) {
    if (!redis) throw new Error('Authentication requires Redis');
    registerAuth(app, pool, redis, auth);
  }
  return app;
}
