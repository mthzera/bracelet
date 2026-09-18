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

  // Payload bruto do SNAPSHOT_VITALS para auditoria (metrics, sources, sampleCounts, etc.).
  await client.query(`
    ALTER TABLE packets
    ADD COLUMN IF NOT EXISTS raw_snapshot JSONB;
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

  // Último heartbeat de cada tablet gateway (BioSync mobile). UPSERT por tablet_id.
  await client.query(`
    CREATE TABLE IF NOT EXISTS gateway_heartbeats (
      tablet_id TEXT PRIMARY KEY,
      tablet_label TEXT NOT NULL DEFAULT '',
      app_version TEXT NOT NULL DEFAULT '',
      bracelet_mac TEXT,
      ble_connected BOOLEAN NOT NULL DEFAULT FALSE,
      is_measuring BOOLEAN NOT NULL DEFAULT FALSE,
      phase TEXT NOT NULL DEFAULT '',
      status_message TEXT NOT NULL DEFAULT '',
      pending_sync_count INTEGER NOT NULL DEFAULT 0,
      last_sync_ok BOOLEAN,
      last_sync_at TIMESTAMPTZ,
      last_error TEXT,
      api_reachable BOOLEAN,
      client_sent_at TIMESTAMPTZ,
      received_at TIMESTAMPTZ NOT NULL DEFAULT now(),
      first_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
      tablet_battery_percent INTEGER,
      tablet_charging BOOLEAN,
      device_model TEXT,
      device_manufacturer TEXT,
      android_id TEXT,
      session_user TEXT,
      session_role TEXT
    );
  `);

  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS first_seen_at TIMESTAMPTZ NOT NULL DEFAULT now();
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS tablet_battery_percent INTEGER;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS tablet_charging BOOLEAN;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS device_model TEXT;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS device_manufacturer TEXT;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS android_id TEXT;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS session_user TEXT;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS session_role TEXT;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS last_ble_connected_at TIMESTAMPTZ;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS last_ble_disconnected_at TIMESTAMPTZ;
  `);
  await client.query(`
    ALTER TABLE gateway_heartbeats
    ADD COLUMN IF NOT EXISTS last_ble_disconnect_reason TEXT;
  `);

  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_gateway_heartbeats_received_at ON gateway_heartbeats(received_at);`,
  );

  await client.query(`
    CREATE TABLE IF NOT EXISTS gateway_commands (
      id SERIAL PRIMARY KEY,
      tablet_id TEXT NOT NULL,
      command TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'pending',
      result TEXT,
      created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
      acked_at TIMESTAMPTZ,
      finished_at TIMESTAMPTZ
    );
  `);
  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_gateway_commands_tablet_status ON gateway_commands(tablet_id, status, created_at DESC);`,
  );

  await client.query(`
    CREATE TABLE IF NOT EXISTS gateway_logs (
      id SERIAL PRIMARY KEY,
      tablet_id TEXT NOT NULL,
      logged_at TIMESTAMPTZ NOT NULL DEFAULT now(),
      level TEXT NOT NULL DEFAULT 'info',
      message TEXT NOT NULL
    );
  `);
  await client.query(
    `CREATE INDEX IF NOT EXISTS idx_gateway_logs_tablet_logged ON gateway_logs(tablet_id, logged_at DESC);`,
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
