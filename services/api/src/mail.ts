import type { PoolClient } from 'pg';
import { open, seal } from './auth/crypto.js';

export type MailSettings = { mode: 'mailpit' | 'brevo'; from: string; encryptionKey: Buffer; url?: string; apiKey?: string };
export type MailMessage = { email: string; name: string; purpose: 'signup' | 'password_reset' | 'welcome'; code?: string };
export class MailFailure extends Error {
  constructor(public readonly code: 'mail_transport_unavailable' | 'mail_provider_rejected' | 'mail_payload_invalid') { super(code); }
}

export function mailSettingsFromEnv(): MailSettings {
  if (!/^[a-f0-9]{64}$/i.test(process.env.AUTH_ENCRYPTION_KEY || '')) throw new Error('AUTH_ENCRYPTION_KEY must be a 32-byte hex key');
  const encryptionKey = Buffer.from(process.env.AUTH_ENCRYPTION_KEY!, 'hex');
  if (process.env.MAIL_MODE && !['mailpit', 'brevo'].includes(process.env.MAIL_MODE)) throw new Error('Unsupported MAIL_MODE');
  if (process.env.MAIL_MODE === 'mailpit') {
    if (process.env.NODE_ENV === 'production') throw new Error('Local test mail is forbidden in production');
    const url = new URL(process.env.MAILPIT_URL || 'http://mailpit:8025');
    if (url.protocol !== 'http:' || !['mailpit', 'mailpit-test', 'localhost', '127.0.0.1'].includes(url.hostname) || url.username || url.password) throw new Error('MAILPIT_URL must address the local test inbox');
    return { mode: 'mailpit', url: url.origin, from: 'noreply@hygrow.test', encryptionKey };
  }
  const from = process.env.MAIL_FROM;
  if (!from || !/^[^\s<>@]+@[^\s<>@]+\.[^\s<>@]+$/.test(from) || !process.env.BREVO_API_KEY) throw new Error('Production mail requires a configured Brevo sender and API key');
  return { mode: 'brevo', from, encryptionKey, apiKey: process.env.BREVO_API_KEY };
}

export async function enqueueMail(client: PoolClient, settings: MailSettings, id: string, message: MailMessage, userId: string, challengeId?: string): Promise<void> {
  await client.query("INSERT INTO background_jobs(id,kind) VALUES($1,'send_mail')", [id]);
  await client.query('INSERT INTO mail_outbox(id,user_id,challenge_id,payload) VALUES($1,$2,$3,$4)', [id, userId, challengeId || null, seal(message, settings.encryptionKey, id)]);
}

const escape = (text: string) => text.replace(/[&<>"']/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[character]!);

function render(message: MailMessage) {
  const welcome = message.purpose === 'welcome';
  const subject = welcome ? 'Welcome to HyGrow' : message.purpose === 'signup' ? 'HyGrow verification code' : 'HyGrow password reset code';
  const instruction = welcome ? 'Your verified Farmer account is ready.' : `Your ${message.purpose === 'signup' ? 'email verification' : 'password reset'} code is ${message.code}. It expires in 10 minutes and can be used once. Do not share it.`;
  const text = `HyGrow\n\nHello ${message.name},\n\n${instruction}\n\nIf you did not request this, you can ignore this email.`;
  const html = `<html><body style="margin:0;background:#f4f8fa;color:#14212b;font-family:Arial,sans-serif"><div style="max-width:520px;margin:32px auto;padding:32px;border-radius:24px;background:#ffffff"><h1 style="color:#087b4d">HyGrow</h1><p>Hello ${escape(message.name)},</p><p>${welcome ? escape(instruction) : 'Use this code to ' + (message.purpose === 'signup' ? 'verify your email.' : 'reset your password.')}</p>${welcome ? '' : `<p style="font-size:32px;letter-spacing:8px;font-weight:bold">${escape(message.code!)}</p><p>Expires in 10 minutes. Use once. Do not share it.</p>`}<p style="color:#5a6b7e">If you did not request this, ignore this email.</p></div></body></html>`;
  return { subject, text, html };
}

export async function processMail(client: PoolClient, settings: MailSettings, id: string, send: typeof fetch = fetch): Promise<void> {
  const result = await client.query('SELECT m.payload,m.challenge_id,c.expires_at,c.consumed_at FROM mail_outbox m LEFT JOIN otp_challenges c ON c.id=m.challenge_id WHERE m.id=$1 FOR UPDATE OF m', [id]);
  if (!result.rowCount || !result.rows[0].payload) return;
  const record = result.rows[0];
  if (record.challenge_id && (record.consumed_at || !record.expires_at || new Date(record.expires_at).getTime() <= Date.now())) {
    await client.query('UPDATE mail_outbox SET payload=NULL WHERE id=$1', [id]);
    return;
  }
  const message = open(record.payload, settings.encryptionKey, id) as MailMessage;
  if (!message || typeof message.email !== 'string' || typeof message.name !== 'string' || !['signup', 'password_reset', 'welcome'].includes(message.purpose) || (message.purpose !== 'welcome' && !/^\d{6}$/.test(message.code || ''))) throw new MailFailure('mail_payload_invalid');
  const content = render(message);
  let response: Response;
  try {
    response = await send(settings.mode === 'brevo' ? 'https://api.brevo.com/v3/smtp/email' : settings.url + '/api/v1/send', {
      method: 'POST', signal: AbortSignal.timeout(10000), headers: { 'Content-Type': 'application/json', ...(settings.mode === 'brevo' ? { 'api-key': settings.apiKey!, accept: 'application/json' } : {}) },
      body: JSON.stringify(settings.mode === 'brevo' ? { sender: { name: 'HyGrow', email: settings.from }, to: [{ email: message.email, name: message.name }], subject: content.subject, htmlContent: content.html, headers: { idempotencyKey: id } } : { From: { Name: 'HyGrow', Email: settings.from }, To: [{ Name: message.name, Email: message.email }], Subject: content.subject, Text: content.text, HTML: content.html }),
    });
  } catch { throw new MailFailure('mail_transport_unavailable'); }
  if (!response.ok) {
    // Brevo's stable per-record idempotency key covers a lost success response.
    const body = await response.json().catch(() => ({})) as { code?: string };
    if (!(settings.mode === 'brevo' && response.status === 400 && body.code === 'duplicate_parameter')) throw new MailFailure(response.status >= 500 || response.status === 429 ? 'mail_transport_unavailable' : 'mail_provider_rejected');
  } else {
    await response.body?.cancel();
  }
  await client.query('UPDATE mail_outbox SET payload=NULL,sent_at=now() WHERE id=$1', [id]);
}
