import assert from 'node:assert/strict';
import { test } from 'node:test';
import { hashPassword, verifyPassword, seal, open, otpHash } from '../dist/auth/crypto.js';

test('password derivation uses random salt and rejects a wrong credential', async () => {
  const first = await hashPassword('a-long-test-passphrase');
  const second = await hashPassword('a-long-test-passphrase');
  assert.notEqual(first, second);
  assert.equal(await verifyPassword('a-long-test-passphrase', first), true);
  assert.equal(await verifyPassword('wrong-test-passphrase', first), false);
  assert.equal(await verifyPassword('anything', 'malformed'), false);
});

test('encrypted outbox is authenticated to its record and OTPs to purpose/transaction', () => {
  const key = Buffer.alloc(32, 7);
  const payload = { code: '123456', email: 'farmer@example.test' };
  const encrypted = seal(payload, key, 'record-one');
  assert.ok(!encrypted.includes('123456'));
  assert.deepEqual(open(encrypted, key, 'record-one'), payload);
  assert.throws(() => open(encrypted, key, 'record-two'));
  assert.throws(() => open(encrypted, Buffer.alloc(32, 8), 'record-one'));
  assert.notEqual(otpHash(key, 'challenge-one', 'signup', '123456'), otpHash(key, 'challenge-one', 'password_reset', '123456'));
  assert.notEqual(otpHash(key, 'challenge-one', 'signup', '123456'), otpHash(key, 'challenge-two', 'signup', '123456'));
});
