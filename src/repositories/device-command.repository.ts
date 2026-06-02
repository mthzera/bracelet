import { getPool } from "../database/db.js";

export type CommandType = "start" | "stop" | "config" | "reset_config";
export type CommandStatus = "pending" | "claimed" | "completed" | "failed" | "cancelled";

export type DeviceCommand = {
  id: number;
  deviceId: string;
  type: CommandType;
  payload: Record<string, unknown> | null;
  status: CommandStatus;
  error: string | null;
  createdAt: string;
  claimedAt: string | null;
  completedAt: string | null;
};

export type DeviceRuntimeStatus = {
  deviceId: string;
  status: string;
  error: string;
  readingActive: boolean;
  sendingData: boolean;
  wifiConnected: boolean;
  wifiSsid: string;
  ip: string;
  rssi: number | null;
  heapFree: number | null;
  lastSeenAt: string;
  updatedAt: string;
};

type CommandRow = {
  id: number;
  device_id: string;
  command_type: CommandType;
  payload: Record<string, unknown> | null;
  status: CommandStatus;
  error: string | null;
  created_at: Date;
  claimed_at: Date | null;
  completed_at: Date | null;
};

function rowToCommand(row: CommandRow): DeviceCommand {
  return {
    id: row.id,
    deviceId: row.device_id,
    type: row.command_type,
    payload: row.payload,
    status: row.status,
    error: row.error,
    createdAt: row.created_at.toISOString(),
    claimedAt: row.claimed_at?.toISOString() ?? null,
    completedAt: row.completed_at?.toISOString() ?? null,
  };
}

export async function enqueueCommand(
  deviceId: string,
  type: CommandType,
  payload?: Record<string, unknown>,
): Promise<DeviceCommand> {
  const pool = getPool();

  const result = await pool.query<CommandRow>(
    `INSERT INTO device_commands (device_id, command_type, payload, status)
     VALUES ($1, $2, $3::jsonb, 'pending')
     RETURNING *`,
    [deviceId, type, payload ? JSON.stringify(payload) : null],
  );

  return rowToCommand(result.rows[0]);
}

export async function claimPendingCommand(deviceId: string): Promise<DeviceCommand | null> {
  const pool = getPool();
  const client = await pool.connect();

  try {
    await client.query("BEGIN");

    const pending = await client.query<CommandRow>(
      `SELECT * FROM device_commands
       WHERE device_id = $1 AND status = 'pending'
       ORDER BY created_at ASC
       LIMIT 1
       FOR UPDATE SKIP LOCKED`,
      [deviceId],
    );

    if (pending.rows.length === 0) {
      await client.query("COMMIT");
      return null;
    }

    const claimed = await client.query<CommandRow>(
      `UPDATE device_commands
       SET status = 'claimed', claimed_at = now()
       WHERE id = $1
       RETURNING *`,
      [pending.rows[0].id],
    );

    await client.query("COMMIT");
    return rowToCommand(claimed.rows[0]);
  } catch (err) {
    await client.query("ROLLBACK");
    throw err;
  } finally {
    client.release();
  }
}

export async function completeCommand(
  commandId: number,
  deviceId: string,
  status: "completed" | "failed",
  error?: string,
): Promise<DeviceCommand | null> {
  const pool = getPool();

  const result = await pool.query<CommandRow>(
    `UPDATE device_commands
     SET status = $1, error = $2, completed_at = now()
     WHERE id = $3 AND device_id = $4 AND status IN ('claimed', 'pending')
     RETURNING *`,
    [status, error ?? null, commandId, deviceId],
  );

  if (result.rows.length === 0) {
    return null;
  }

  return rowToCommand(result.rows[0]);
}

export async function upsertRuntimeStatus(
  deviceId: string,
  input: {
    status: string;
    error?: string;
    readingActive?: boolean;
    sendingData?: boolean;
    wifiConnected?: boolean;
    wifiSsid?: string;
    ip?: string;
    rssi?: number | null;
    heapFree?: number | null;
  },
): Promise<void> {
  const pool = getPool();

  await pool.query(
    `INSERT INTO device_runtime_status (
       device_id, status, error, reading_active, sending_data,
       wifi_connected, wifi_ssid, ip, rssi, heap_free, last_seen_at, updated_at
     ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, now(), now())
     ON CONFLICT (device_id) DO UPDATE SET
       status = EXCLUDED.status,
       error = EXCLUDED.error,
       reading_active = EXCLUDED.reading_active,
       sending_data = EXCLUDED.sending_data,
       wifi_connected = EXCLUDED.wifi_connected,
       wifi_ssid = EXCLUDED.wifi_ssid,
       ip = EXCLUDED.ip,
       rssi = EXCLUDED.rssi,
       heap_free = EXCLUDED.heap_free,
       last_seen_at = now(),
       updated_at = now()`,
    [
      deviceId,
      input.status,
      input.error ?? "",
      input.readingActive ?? false,
      input.sendingData ?? false,
      input.wifiConnected ?? false,
      input.wifiSsid ?? "",
      input.ip ?? "",
      input.rssi ?? null,
      input.heapFree ?? null,
    ],
  );
}

export async function getRuntimeStatus(deviceId: string): Promise<DeviceRuntimeStatus | null> {
  const pool = getPool();

  const result = await pool.query<{
    device_id: string;
    status: string;
    error: string;
    reading_active: boolean;
    sending_data: boolean;
    wifi_connected: boolean;
    wifi_ssid: string;
    ip: string;
    rssi: number | null;
    heap_free: number | null;
    last_seen_at: Date;
    updated_at: Date;
  }>(`SELECT * FROM device_runtime_status WHERE device_id = $1`, [deviceId]);

  if (result.rows.length === 0) {
    return null;
  }

  const row = result.rows[0];
  return {
    deviceId: row.device_id,
    status: row.status,
    error: row.error,
    readingActive: row.reading_active,
    sendingData: row.sending_data,
    wifiConnected: row.wifi_connected,
    wifiSsid: row.wifi_ssid,
    ip: row.ip,
    rssi: row.rssi,
    heapFree: row.heap_free,
    lastSeenAt: row.last_seen_at.toISOString(),
    updatedAt: row.updated_at.toISOString(),
  };
}

export async function listRecentCommands(deviceId: string, limit = 20): Promise<DeviceCommand[]> {
  const pool = getPool();

  const result = await pool.query<CommandRow>(
    `SELECT * FROM device_commands
     WHERE device_id = $1
     ORDER BY created_at DESC
     LIMIT $2`,
    [deviceId, limit],
  );

  return result.rows.map(rowToCommand);
}
