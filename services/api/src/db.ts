import pg from 'pg';
import type { Pool, PoolClient } from 'pg';

export function createPool(url = process.env.DATABASE_URL): Pool {
  if (!url) throw new Error('DATABASE_URL is required');
  const pool = new pg.Pool({ connectionString: url, max: 10, connectionTimeoutMillis: 5000, idleTimeoutMillis: 30000, statement_timeout: 10000 });
  pool.on('error', () => { console.error(JSON.stringify({ event: 'database_connection_error', code: 'dependency_unavailable' })); });
  return pool;
}

export async function transaction<T>(pool: Pool, operation: (client: PoolClient) => Promise<T>): Promise<T> {
  const client = await pool.connect();
  try {
    await client.query('BEGIN');
    const result = await operation(client);
    await client.query('COMMIT');
    return result;
  } catch (error) {
    await client.query('ROLLBACK');
    throw error;
  } finally {
    client.release();
  }
}
