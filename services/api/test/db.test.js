import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createPool } from '../dist/db.js';

test('an idle PostgreSQL connection failure does not crash the API process', async () => {
  const pool = createPool('postgres://fixture:fixture@127.0.0.1:1/hygrow_test');
  assert.doesNotThrow(() => pool.emit('error', new Error('connection lost')));
  await pool.end();
});
