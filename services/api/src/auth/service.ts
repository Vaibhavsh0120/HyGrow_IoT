import { randomInt, randomUUID, timingSafeEqual } from 'node:crypto';
import type { Pool, PoolClient } from 'pg';
import { transaction } from '../db.js';
import { ApiError } from '../errors.js';
import { enqueueMail, mailSettingsFromEnv, type MailSettings } from '../mail.js';
import { dummyPasswordHash, hashPassword, newToken, otpHash, tokenHash, verifyPassword } from './crypto.js';

export type AuthSettings = { otpPepper: Buffer; mail: MailSettings };
type User = { id: string; email: string; display_name: string; email_verified_at: Date | null; password_hash: string | null };
export type PublicUser = { id: string; email: string; displayName: string; roles: string[] };
export type Principal = { userId: string; familyId: string; sessionId: string };
type Session = { accessToken: string; refreshToken: string; user: PublicUser };
type Purpose = 'signup' | 'password_reset';
type PendingSignup = { passwordHash: string; displayName: string };
type VerifiedCode = { userId: string; pendingPasswordHash: string | null; pendingDisplayName: string | null };
const invalidCode = () => new ApiError(400, 'invalid_otp', 'This code is incorrect, expired or already used. Request a new code.');
const invalidSession = () => new ApiError(401, 'unauthorized', 'Please log in again.');

export function authSettingsFromEnv(): AuthSettings {
  if (!/^[a-f0-9]{64}$/i.test(process.env.AUTH_OTP_PEPPER || '')) throw new Error('AUTH_OTP_PEPPER must be a 32-byte hex key');
  return { otpPepper: Buffer.from(process.env.AUTH_OTP_PEPPER!, 'hex'), mail: mailSettingsFromEnv() };
}

export class AuthService {
  constructor(private pool: Pool, private settings: AuthSettings) {
    if (settings.otpPepper.length !== 32 || settings.mail.encryptionKey.length !== 32) throw new Error('Invalid auth key configuration');
  }

  async publicUser(client: Pool | PoolClient, id: string): Promise<PublicUser> {
    const result = await client.query<User>('SELECT id,email,display_name FROM users WHERE id=$1 AND email_verified_at IS NOT NULL', [id]);
    if (!result.rowCount) throw invalidSession();
    const roles = await client.query('SELECT role FROM user_roles WHERE user_id=$1 ORDER BY role', [id]);
    const user = result.rows[0];
    return { id: user.id, email: user.email, displayName: user.display_name, roles: roles.rows.map(row => row.role) };
  }

  private async session(client: PoolClient, userId: string, familyId = randomUUID()): Promise<Session> {
    const accessToken = newToken();
    const refreshToken = newToken();
    await client.query("INSERT INTO sessions(id,user_id,access_hash,refresh_hash,family_id,access_expires_at,refresh_expires_at) VALUES($1,$2,$3,$4,$5,now()+interval '15 minutes',now()+interval '30 days')", [randomUUID(), userId, tokenHash(accessToken), tokenHash(refreshToken), familyId]);
    return { accessToken, refreshToken, user: await this.publicUser(client, userId) };
  }

  private async challenge(client: PoolClient, user: User, purpose: Purpose, pending?: PendingSignup): Promise<string> {
    if (purpose === 'signup' && !pending) throw new Error('signup_binding_missing');
    const recent = await client.query('SELECT created_at FROM otp_challenges WHERE user_id=$1 AND purpose=$2 ORDER BY created_at DESC LIMIT 1', [user.id, purpose]);
    if (recent.rowCount && Date.now() - new Date(recent.rows[0].created_at).getTime() < 60000) throw new ApiError(429, 'otp_cooldown', 'Please wait before requesting another code.', 60);
    await client.query('UPDATE otp_challenges SET consumed_at=now() WHERE user_id=$1 AND purpose=$2 AND consumed_at IS NULL', [user.id, purpose]);
    const id = randomUUID();
    const code = randomInt(0, 1000000).toString().padStart(6, '0');
    await client.query("INSERT INTO otp_challenges(id,user_id,purpose,code_hash,expires_at,pending_password_hash,pending_display_name) VALUES($1,$2,$3,$4,now()+interval '10 minutes',$5,$6)", [id, user.id, purpose, otpHash(this.settings.otpPepper, id, purpose, code), pending?.passwordHash || null, pending?.displayName || null]);
    await enqueueMail(client, this.settings.mail, randomUUID(), { email: user.email, name: pending?.displayName || user.display_name, purpose, code }, user.id, id);
    return id;
  }

  async signup(email: string, displayName: string, password: string): Promise<string> {
    const passwordHash = await hashPassword(password);
    return transaction(this.pool, async client => {
      await client.query('SELECT pg_advisory_xact_lock(hashtextextended($1,0))', [email]);
      let user = (await client.query<User>('SELECT * FROM users WHERE email=$1 FOR UPDATE', [email])).rows[0];
      // Same accepted shape for existing verified accounts; never overwrite them.
      if (user?.email_verified_at) return randomUUID();
      if (!user) {
        user = (await client.query<User>('INSERT INTO users(id,email,display_name) VALUES($1,$2,$3) RETURNING *', [randomUUID(), email, displayName])).rows[0];
      }
      return this.challenge(client, user, 'signup', { passwordHash, displayName });
    });
  }

  private async consumeCode(client: PoolClient, id: string, purpose: Purpose, code: string): Promise<VerifiedCode | undefined> {
    const identity = await client.query('SELECT user_id FROM otp_challenges WHERE id=$1', [id]);
    if (!identity.rowCount) return;
    await client.query('SELECT id FROM users WHERE id=$1 FOR UPDATE', [identity.rows[0].user_id]);
    const result = await client.query('SELECT * FROM otp_challenges WHERE id=$1 FOR UPDATE', [id]);
    if (!result.rowCount) return;
    const record = result.rows[0];
    if (record.purpose !== purpose || record.consumed_at || record.attempts >= 5 || new Date(record.expires_at).getTime() <= Date.now()) return;
    const expected = Buffer.from(otpHash(this.settings.otpPepper, id, purpose, code), 'hex');
    const stored = Buffer.from(record.code_hash, 'hex');
    const valid = stored.length === expected.length && timingSafeEqual(stored, expected);
    await client.query('UPDATE otp_challenges SET attempts=attempts+1,consumed_at=CASE WHEN $2 OR attempts+1>=5 THEN now() ELSE consumed_at END WHERE id=$1', [id, valid]);
    return valid ? { userId: record.user_id, pendingPasswordHash: record.pending_password_hash, pendingDisplayName: record.pending_display_name } : undefined;
  }

  async verify(id: string, code: string): Promise<Session> {
    const result = await transaction(this.pool, async client => {
      const verified = await this.consumeCode(client, id, 'signup', code);
      if (!verified?.pendingPasswordHash || !verified.pendingDisplayName) return;
      const userId = verified.userId;
      const user = await client.query<User>('UPDATE users SET email_verified_at=now(),password_hash=$2,display_name=$3 WHERE id=$1 AND email_verified_at IS NULL RETURNING *', [userId, verified.pendingPasswordHash, verified.pendingDisplayName]);
      if (!user.rowCount) return;
      await client.query("INSERT INTO user_roles(user_id,role) VALUES($1,'farmer') ON CONFLICT DO NOTHING", [userId]);
      await client.query('UPDATE otp_challenges SET consumed_at=COALESCE(consumed_at,now()),pending_password_hash=NULL,pending_display_name=NULL WHERE user_id=$1', [userId]);
      await enqueueMail(client, this.settings.mail, randomUUID(), { email: user.rows[0].email, name: user.rows[0].display_name, purpose: 'welcome' }, userId);
      return this.session(client, userId);
    });
    if (!result) throw invalidCode();
    return result;
  }

  async login(email: string, password: string): Promise<Session> {
    const found = await this.pool.query<User>('SELECT * FROM users WHERE email=$1', [email]);
    const user = found.rows[0];
    const valid = await verifyPassword(password, user?.password_hash || await dummyPasswordHash);
    if (!valid || !user?.email_verified_at) throw new ApiError(401, 'invalid_credentials', 'Check your email and password.');
    return transaction(this.pool, async client => {
      // Lock against reset so a pre-reset password check cannot mint a new session.
      const current = await client.query<User>('SELECT * FROM users WHERE id=$1 FOR UPDATE', [user.id]);
      if (!current.rowCount || current.rows[0].password_hash !== user.password_hash) throw new ApiError(401, 'invalid_credentials', 'Check your email and password.');
      return this.session(client, user.id);
    });
  }

  async refresh(token: string): Promise<Session> {
    const result = await transaction(this.pool, async client => {
      const identity = await client.query('SELECT user_id FROM sessions WHERE refresh_hash=$1', [tokenHash(token)]);
      if (!identity.rowCount) return;
      // All session mutations serialize on the user, including reset/logout.
      await client.query('SELECT id FROM users WHERE id=$1 FOR UPDATE', [identity.rows[0].user_id]);
      const found = await client.query('SELECT * FROM sessions WHERE refresh_hash=$1 FOR UPDATE', [tokenHash(token)]);
      if (!found.rowCount) return;
      const old = found.rows[0];
      if (old.revoked_at || old.rotated_at || new Date(old.refresh_expires_at).getTime() <= Date.now()) {
        await client.query('UPDATE sessions SET revoked_at=COALESCE(revoked_at,now()) WHERE family_id=$1', [old.family_id]);
        return;
      }
      await client.query('UPDATE sessions SET rotated_at=now(),revoked_at=now() WHERE id=$1', [old.id]);
      return this.session(client, old.user_id, old.family_id);
    });
    // Rejections follow commit: replay-family revocation must not roll back.
    if (!result) throw invalidSession();
    return result;
  }

  async principal(token: string | undefined): Promise<Principal> {
    if (!token || !/^[A-Za-z0-9_-]{43}$/.test(token)) throw invalidSession();
    const found = await this.pool.query('SELECT id,user_id,family_id FROM sessions WHERE access_hash=$1 AND revoked_at IS NULL AND access_expires_at>now()', [tokenHash(token)]);
    if (!found.rowCount) throw invalidSession();
    return { userId: found.rows[0].user_id, familyId: found.rows[0].family_id, sessionId: found.rows[0].id };
  }

  async logout(token: string | undefined): Promise<void> {
    if (!token || !/^[A-Za-z0-9_-]{43}$/.test(token)) return;
    await transaction(this.pool, async client => {
      const found = await client.query('SELECT user_id,family_id FROM sessions WHERE access_hash=$1', [tokenHash(token)]);
      if (!found.rowCount) return;
      await client.query('SELECT id FROM users WHERE id=$1 FOR UPDATE', [found.rows[0].user_id]);
      await client.query('UPDATE sessions SET revoked_at=COALESCE(revoked_at,now()) WHERE family_id=$1', [found.rows[0].family_id]);
    });
  }

  async requestReset(email: string): Promise<string> {
    return transaction(this.pool, async client => {
      const user = (await client.query<User>('SELECT * FROM users WHERE email=$1 AND email_verified_at IS NOT NULL FOR UPDATE', [email])).rows[0];
      return user ? this.challenge(client, user, 'password_reset') : randomUUID();
    });
  }

  async reset(id: string, code: string, password: string): Promise<void> {
    const passwordHash = await hashPassword(password);
    const success = await transaction(this.pool, async client => {
      const verified = await this.consumeCode(client, id, 'password_reset', code);
      if (!verified) return false;
      const userId = verified.userId;
      await client.query('UPDATE users SET password_hash=$2 WHERE id=$1', [userId, passwordHash]);
      await client.query('UPDATE sessions SET revoked_at=COALESCE(revoked_at,now()) WHERE user_id=$1', [userId]);
      await client.query('UPDATE otp_challenges SET consumed_at=COALESCE(consumed_at,now()),pending_password_hash=NULL,pending_display_name=NULL WHERE user_id=$1', [userId]);
      return true;
    });
    if (!success) throw invalidCode();
  }

  async resend(id: string, purpose: Purpose): Promise<string> {
    return transaction(this.pool, async client => {
      const found = await client.query<User>('SELECT u.* FROM users u JOIN otp_challenges c ON c.user_id=u.id WHERE c.id=$1 AND c.purpose=$2 FOR UPDATE OF u', [id, purpose]);
      const user = found.rows[0];
      if (!user || (purpose === 'signup' && user.email_verified_at)) return randomUUID();
      const original = (await client.query('SELECT pending_password_hash,pending_display_name FROM otp_challenges WHERE id=$1', [id])).rows[0];
      if (purpose === 'signup' && (!original?.pending_password_hash || !original.pending_display_name)) return randomUUID();
      return this.challenge(client, user, purpose, purpose === 'signup' ? { passwordHash: original.pending_password_hash, displayName: original.pending_display_name } : undefined);
    });
  }
}
