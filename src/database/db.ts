import { Pool, type PoolClient } from "pg";

let pool: Pool | null = null;

function requireDatabaseUrl(): string {
  const url = process.env.DATABASE_URL;
  if (!url) {
    throw new Error("DATABASE_URL is required to start the server");
  }
  return url;
}

async function migrate(client: PoolClient): Promise<void> {
  await client.query(`
    CREATE TABLE IF NOT EXISTS packets (
      id SERIAL PRIMARY KEY,
      device_mac TEXT NOT NULL,
      packet_type TEXT NOT NULL,
      raw_hex TEXT NOT NULL,
      source TEXT NOT NULL,
      bytes JSONB,
      crc_valid BOOLEAN NOT NULL DEFAULT FALSE,
      decoded JSONB,
      decode_error TEXT,
      created_at TIMESTAMPTZ NOT NULL DEFAULT now()
    );
  `);

  await client.query(`CREATE INDEX IF NOT EXISTS idx_packets_device_mac ON packets(device_mac);`);
  await client.query(`CREATE INDEX IF NOT EXISTS idx_packets_created_at ON packets(created_at);`);
}

export async function initDatabase(): Promise<Pool> {
  if (pool) {
    return pool;
  }

  const connectionString = requireDatabaseUrl();

  pool = new Pool({
    connectionString,
    ssl: { rejectUnauthorized: false },
  });

  const client = await pool.connect();
  try {
    await migrate(client);
  } finally {
    client.release();
  }

  return pool;
}

export function getPool(): Pool {
  if (!pool) {
    throw new Error("Database not initialized. Call initDatabase() first.");
  }
  return pool;
}

export async function closeDatabase(): Promise<void> {
  if (pool) {
    await pool.end();
    pool = null;
  }
}
