import { isIP } from 'node:net';

// Explicit addresses/CIDRs only. Never accept blanket or hop-count trust from env.
export function trustedProxiesFromEnv(value = process.env.TRUSTED_PROXIES): string[] {
  if (!value?.trim()) return [];
  const entries = value.split(',').map(part => part.trim());
  for (const entry of entries) {
    const parts = entry.split('/');
    const family = isIP(parts[0]);
    const prefix = parts[1];
    if (!family || parts.length > 2 || (prefix !== undefined &&
      (!/^\d+$/.test(prefix) || Number(prefix) < 1 || Number(prefix) > (family === 4 ? 32 : 128)))) {
      throw new Error('TRUSTED_PROXIES requires explicit IP addresses or bounded CIDRs');
    }
  }
  return entries;
}
