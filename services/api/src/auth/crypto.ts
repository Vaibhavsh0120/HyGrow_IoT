import { createCipheriv, createDecipheriv, createHash, createHmac, randomBytes, scrypt, timingSafeEqual } from 'node:crypto';

function derive(password: string, salt: Buffer): Promise<Buffer> {
  return new Promise((resolve, reject) => scrypt(password, salt, 64, { N: 32768, r: 8, p: 3, maxmem: 64 * 1024 * 1024 }, (error, result) => error ? reject(error) : resolve(result)));
}

export async function hashPassword(password: string): Promise<string> {
  const salt = randomBytes(16);
  return `scrypt-v1$${salt.toString('hex')}$${(await derive(password, salt)).toString('hex')}`;
}

export async function verifyPassword(password: string, encoded: string): Promise<boolean> {
  const match = /^scrypt-v1\$([a-f0-9]{32})\$([a-f0-9]{128})$/.exec(encoded);
  if (!match) return false;
  return timingSafeEqual(await derive(password, Buffer.from(match[1], 'hex')), Buffer.from(match[2], 'hex'));
}

export const dummyPasswordHash = hashPassword(randomBytes(32).toString('hex'));
export const tokenHash = (token: string) => createHash('sha256').update(token).digest('hex');
export const newToken = () => randomBytes(32).toString('base64url');
export const otpHash = (key: Buffer, id: string, purpose: string, code: string) => createHmac('sha256', key).update(`${id}\0${purpose}\0${code}`).digest('hex');

export function seal(payload: unknown, key: Buffer, recordId: string): string {
  const iv = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', key, iv);
  cipher.setAAD(Buffer.from(`hygrow-mail-v1:${recordId}`));
  const ciphertext = Buffer.concat([cipher.update(JSON.stringify(payload), 'utf8'), cipher.final()]);
  return 'v1.' + Buffer.concat([iv, cipher.getAuthTag(), ciphertext]).toString('base64');
}

export function open(payload: string, key: Buffer, recordId: string): unknown {
  if (!payload.startsWith('v1.')) throw new Error('unsupported_mail_encryption');
  const bytes = Buffer.from(payload.slice(3), 'base64');
  if (bytes.length < 29 || bytes.length > 32768) throw new Error('invalid_mail_envelope');
  const decipher = createDecipheriv('aes-256-gcm', key, bytes.subarray(0, 12));
  decipher.setAAD(Buffer.from(`hygrow-mail-v1:${recordId}`));
  decipher.setAuthTag(bytes.subarray(12, 28));
  return JSON.parse(Buffer.concat([decipher.update(bytes.subarray(28)), decipher.final()]).toString('utf8'));
}
