import { getPool } from "../database/db.js";

export const GATEWAY_COMMANDS = [
  "restart_app",
  "reconnect_ble",
  "start_measurement",
  "stop_measurement",
  "retry_pending_sync",
] as const;

export type GatewayCommandName = (typeof GATEWAY_COMMANDS)[number];

export type GatewayCommandRow = {
  id: number;
  tabletId: string;
  command: string;
  status: string;
  createdAt: string;
  ackedAt: string | null;
  result: string | null;
};

type DbRow = {
  id: number;
  tablet_id: string;
  command: string;
  status: string;
  created_at: Date;
  acked_at: Date | null;
  result: string | null;
};

function mapRow(row: DbRow): GatewayCommandRow {
  return {
    id: row.id,
    tabletId: row.tablet_id,
    command: row.command,
    status: row.status,
    createdAt: row.created_at.toISOString(),
    ackedAt: row.acked_at ? row.acked_at.toISOString() : null,
    result: row.result,
  };
}

export function isGatewayCommand(value: string): value is GatewayCommandName {
  return (GATEWAY_COMMANDS as readonly string[]).includes(value);
}

export async function enqueueGatewayCommand(
  tabletId: string,
  command: GatewayCommandName,
): Promise<GatewayCommandRow> {
  const pool = getPool();
  const result = await pool.query<DbRow>(
    `
    INSERT INTO gateway_commands (tablet_id, command, status)
    VALUES ($1, $2, 'pending')
    RETURNING *
    `,
    [tabletId.trim(), command],
  );
  return mapRow(result.rows[0]);
}

/** Pending commands for tablet; marks them as acked. */
export async function claimPendingCommands(
  tabletId: string,
  limit = 5,
): Promise<GatewayCommandRow[]> {
  const pool = getPool();
  const client = await pool.connect();
  try {
    await client.query("BEGIN");
    const pending = await client.query<DbRow>(
      `
      SELECT *
      FROM gateway_commands
      WHERE tablet_id = $1 AND status = 'pending'
      ORDER BY created_at ASC
      LIMIT $2
      FOR UPDATE SKIP LOCKED
      `,
      [tabletId.trim(), limit],
    );
    if (pending.rows.length === 0) {
      await client.query("COMMIT");
      return [];
    }
    const ids = pending.rows.map((r) => r.id);
    await client.query(
      `
      UPDATE gateway_commands
      SET status = 'acked', acked_at = now()
      WHERE id = ANY($1::int[])
      `,
      [ids],
    );
    await client.query("COMMIT");
    return pending.rows.map(mapRow);
  } catch (err) {
    await client.query("ROLLBACK");
    throw err;
  } finally {
    client.release();
  }
}

export async function completeGatewayCommand(
  tabletId: string,
  commandId: number,
  ok: boolean,
  result?: string | null,
): Promise<GatewayCommandRow | null> {
  const pool = getPool();
  const updated = await pool.query<DbRow>(
    `
    UPDATE gateway_commands
    SET status = $3, result = $4, finished_at = now()
    WHERE id = $1 AND tablet_id = $2 AND status IN ('pending', 'acked')
    RETURNING *
    `,
    [commandId, tabletId.trim(), ok ? "done" : "failed", result?.trim() || null],
  );
  return updated.rows[0] ? mapRow(updated.rows[0]) : null;
}

export async function listRecentCommands(
  tabletId: string,
  limit = 20,
): Promise<GatewayCommandRow[]> {
  const pool = getPool();
  const result = await pool.query<DbRow>(
    `
    SELECT *
    FROM gateway_commands
    WHERE tablet_id = $1
    ORDER BY created_at DESC
    LIMIT $2
    `,
    [tabletId.trim(), limit],
  );
  return result.rows.map(mapRow);
}
