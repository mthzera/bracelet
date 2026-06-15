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

  // Identificador de ciclo/ingestão: todos os pacotes de um mesmo POST batch o compartilham.
  // Registros antigos ficam NULL e são agrupados por janela de tempo no consolidador.
  await client.query(`
    ALTER TABLE packets
    ADD COLUMN IF NOT EXISTS ingestion_batch_id TEXT;
  `);

  await client.query(`CREATE INDEX IF NOT EXISTS idx_packets_device_mac ON packets(device_mac);`);
  await client.query(`CREATE INDEX IF NOT EXISTS idx_packets_created_at ON packets(created_at);`);
  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_packets_ingestion_batch_id ON packets(ingestion_batch_id);`,
  );

  await client.query(`
    CREATE TABLE IF NOT EXISTS vital_assessments (
      id SERIAL PRIMARY KEY,
      device_mac TEXT NOT NULL,
      measured_at TIMESTAMPTZ NOT NULL,
      packet_id INTEGER REFERENCES packets(id) ON DELETE SET NULL,
      vitals JSONB NOT NULL,
      context JSONB NOT NULL DEFAULT '{}',
      alerts JSONB NOT NULL DEFAULT '[]',
      notes JSONB NOT NULL DEFAULT '[]',
      risk_score INTEGER NOT NULL DEFAULT 0,
      overall_status TEXT NOT NULL,
      severity TEXT NOT NULL,
      baseline JSONB NOT NULL DEFAULT '{}',
      disclaimer TEXT NOT NULL,
      news2 JSONB NOT NULL DEFAULT '{}',
      created_at TIMESTAMPTZ NOT NULL DEFAULT now()
    );
  `);

  await client.query(`
    ALTER TABLE vital_assessments
    ADD COLUMN IF NOT EXISTS news2 JSONB NOT NULL DEFAULT '{}';
  `);

  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_vital_assessments_device_mac ON vital_assessments(device_mac);`,
  );
  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_vital_assessments_measured_at ON vital_assessments(measured_at);`,
  );
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
