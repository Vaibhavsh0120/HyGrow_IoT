import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { test } from 'node:test';
import { createPool, transaction } from '../dist/db.js';
import { migrate } from '../dist/migrate.js';

const url = process.env.HYGROW_TEST_DATABASE_URL;

test('real PostgreSQL migrations, rollback and identity constraints', { skip: !url }, async t => {
  const parsed = new URL(url);
  assert.ok(['127.0.0.1', 'localhost', 'db-test'].includes(parsed.hostname), 'Tests require an isolated local database');
  assert.equal(parsed.pathname, '/hygrow_test', 'Refusing tests against any non-test database');
  const pool = createPool(url);
  t.after(() => pool.end());
  await migrate(pool);
  await migrate(pool);
  assert.equal((await pool.query("SELECT version FROM schema_migrations WHERE version='001_identity.sql'")).rowCount, 1);
  const id = randomUUID();
  const email = `${id}@example.test`;
  await assert.rejects(transaction(pool, async client => {
    await client.query('INSERT INTO users(id,email,display_name) VALUES($1,$2,$3)', [id, email, 'Farmer']);
    throw new Error('cancel');
  }), /cancel/);
  assert.equal((await pool.query('SELECT id FROM users WHERE id=$1', [id])).rowCount, 0);
  await pool.query('INSERT INTO users(id,email,display_name) VALUES($1,$2,$3)', [id, email, 'Farmer']);
  await assert.rejects(pool.query('INSERT INTO users(id,email,display_name) VALUES($1,$2,$3)', [randomUUID(), email, 'Duplicate']), { code: '23505' });
  await assert.rejects(pool.query('INSERT INTO user_roles(user_id,role) VALUES($1,$2)', [id, 'administrator']), { code: '23514' });
  await pool.query('DELETE FROM users WHERE id=$1', [id]);
});
