import { createHmac } from 'node:crypto';
import type { FastifyInstance, FastifyRequest } from 'fastify';
import type { Pool } from 'pg';
import { allowRequest, type RedisConnection } from '../redis.js';
import { ApiError } from '../errors.js';
import { dummyPasswordHash } from './crypto.js';
import { AuthService, type AuthSettings } from './service.js';

const email = { type: 'string', minLength: 3, maxLength: 254, pattern: '^[^\\s<>@]+@[^\\s<>@]+\\.[^\\s<>@]+$' };
const password = { type: 'string', minLength: 12, maxLength: 128 };
const challengeId = { type: 'string', pattern: '^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$' };
const code = { type: 'string', pattern: '^\\d{6}$' };
const schema = (properties: Record<string, unknown>) => ({ body: { type: 'object', additionalProperties: false, required: Object.keys(properties), properties } });
const bearer = (request: FastifyRequest) => /^Bearer ([A-Za-z0-9_-]{43})$/.exec(request.headers.authorization || '')?.[1];
const accepted = (id: string) => ({ challengeId: id, resendAfterSeconds: 60, message: 'If eligible, a code will arrive shortly. Check your inbox and spam folder.' });

export function registerAuth(app: FastifyInstance, pool: Pool, redis: RedisConnection, settings: AuthSettings) {
  const service = new AuthService(pool, settings);
  const opaque = (value: string) => createHmac('sha256', settings.otpPepper).update('auth-budget:' + value).digest('hex');
  app.addHook('onReady', async () => { await dummyPasswordHash; });
  async function budget(key: string, limit: number, seconds: number) {
    const result = await allowRequest(redis, 'hygrow:rate:auth:' + key, limit, seconds);
    if (!result.allowed) throw new ApiError(429, 'rate_limited', 'Please wait before trying again.', Math.ceil(result.retryAfterMs / 1000));
  }
  const authRate = async (request: FastifyRequest) => budget('ip:' + opaque(request.ip), 30, 300);
  const emailKey = (value: string) => opaque(value.trim().toLowerCase());
  async function otpBudget(key: string) {
    await budget('otp-minute:' + key, 1, 60);
    await budget('otp-window:' + key, 3, 900);
  }
  async function issue(id: string, purpose: string, key: string) {
    // Start the public deadline after hashing/DB commit, so it always covers
    // the private DB cooldown for real and synthetic transactions alike.
    await redis.set('hygrow:rate:auth:otp-minute:' + key, '1', { EX: 60 });
    // Opaque transaction/abuse metadata only: no email address, OTP or password.
    await redis.set('hygrow:otp:request:' + id, JSON.stringify({ purpose, key }), { EX: 600 });
    return accepted(id);
  }
  app.post<{ Body: { email: string; displayName: string; password: string; role: 'farmer' } }>('/api/v1/auth/signup', { schema: schema({ email, displayName: { type: 'string', minLength: 1, maxLength: 100, pattern: '\\S' }, password, role: { const: 'farmer' } }), preHandler: authRate }, async (request, reply) => {
    const address = request.body.email.trim().toLowerCase();
    const key = emailKey(address);
    await otpBudget(key);
    const id = await service.signup(address, request.body.displayName.trim(), request.body.password);
    return reply.code(202).send(await issue(id, 'signup', key));
  });
  app.post<{ Body: { email: string; password: string } }>('/api/v1/auth/login', { schema: schema({ email, password: { ...password, minLength: 1 } }), preHandler: authRate }, async request => {
    const address = request.body.email.trim().toLowerCase();
    await budget('login:' + emailKey(address), 10, 900);
    return service.login(address, request.body.password);
  });
  app.post<{ Body: { challengeId: string; code: string } }>('/api/v1/auth/verify', { schema: schema({ challengeId, code }), preHandler: authRate }, async request => service.verify(request.body.challengeId, request.body.code));
  app.post<{ Body: { refreshToken: string } }>('/api/v1/auth/refresh', { schema: schema({ refreshToken: { type: 'string', pattern: '^[A-Za-z0-9_-]{43}$' } }), preHandler: authRate }, async request => service.refresh(request.body.refreshToken));
  app.post('/api/v1/auth/logout', async request => { await service.logout(bearer(request)); return { success: true }; });
  app.get('/api/v1/me', async request => ({ user: await service.publicUser(pool, (await service.principal(bearer(request))).userId) }));
  app.post<{ Body: { email: string } }>('/api/v1/auth/password/request', { schema: schema({ email }), preHandler: authRate }, async (request, reply) => {
    const address = request.body.email.trim().toLowerCase();
    const key = emailKey(address);
    await otpBudget(key);
    return reply.code(202).send(await issue(await service.requestReset(address), 'password_reset', key));
  });
  app.post<{ Body: { challengeId: string; code: string; password: string } }>('/api/v1/auth/password/reset', { schema: schema({ challengeId, code, password }), preHandler: authRate }, async request => {
    await service.reset(request.body.challengeId, request.body.code, request.body.password);
    return { success: true };
  });
  app.post<{ Body: { challengeId: string; purpose: 'signup' | 'password_reset' } }>('/api/v1/auth/otp/resend', { schema: schema({ challengeId, purpose: { enum: ['signup', 'password_reset'] } }), preHandler: authRate }, async (request, reply) => {
    const keyName = 'hygrow:otp:request:' + request.body.challengeId;
    const stored = await redis.get(keyName);
    const record = stored ? JSON.parse(stored) as { purpose: string; key: string } : null;
    if (!record || record.purpose !== request.body.purpose) throw new ApiError(400, 'invalid_otp', 'Start again to request a new code.');
    await otpBudget(record.key);
    const response = await issue(await service.resend(request.body.challengeId, record.purpose as 'signup' | 'password_reset'), record.purpose, record.key);
    await redis.del(keyName);
    return reply.code(202).send(response);
  });
}
