import { getPool } from "../database/db.js";

export type GatewayLogInput = {
  loggedAt?: string | null;
  level?: string | null;
  message: string;
};

export type GatewayLogRow = {
  id: number;
  tabletId: string;
  loggedAt: string;
  level: string;
  message: string;
};

type DbRow = {
  id: number;
  tablet_id: string;
  logged_at: Date;
  level: string;
  message: string;
};

const MAX_LOGS_PER_TABLET = 300;

function mapRow(row: DbRow): GatewayLogRow {
  return {
    id: row.id,
    tabletId: row.tablet_id,
    loggedAt: row.logged_at.toISOString(),
    level: row.level,
    message: row.message,
  };
}

export async function appendGatewayLogs(
  tabletId: string,
  logs: GatewayLogInput[],
): Promise<number> {
  const id = tabletId.trim();
  if (!id || logs.length === 0) return 0;

  const pool = getPool();
  const client = await pool.connect();
  let inserted = 0;
  try {
    await client.query("BEGIN");
    for (const entry of logs.slice(-150)) {
      const message = (entry.message ?? "").trim();
      if (!message) continue;
      await client.query(
        `
        INSERT INTO gateway_logs (tablet_id, logged_at, level, message)
        VALUES ($1, COALESCE($2::timestamptz, now()), $3, $4)
        `,
        [
          id,
          entry.loggedAt ? new Date(entry.loggedAt).toISOString() : null,
          (entry.level ?? "info").trim() || "info",
          message.slice(0, 2000),
        ],
      );
      inserted += 1;
    }

    await client.query(
      `
      DELETE FROM gateway_logs
      WHERE tablet_id = $1
        AND id NOT IN (
          SELECT id FROM gateway_logs
          WHERE tablet_id = $1
          ORDER BY logged_at DESC, id DESC
          LIMIT $2
        )
      `,
      [id, MAX_LOGS_PER_TABLET],
    );
    await client.query("COMMIT");
  } catch (err) {
    await client.query("ROLLBACK");
    throw err;
  } finally {
    client.release();
  }
  return inserted;
}

export async function listGatewayLogs(
  tabletId: string,
  limit = 150,
): Promise<GatewayLogRow[]> {
  const pool = getPool();
  const result = await pool.query<DbRow>(
    `
    SELECT *
    FROM gateway_logs
    WHERE tablet_id = $1
    ORDER BY logged_at DESC, id DESC
    LIMIT $2
    `,
    [tabletId.trim(), Math.min(Math.max(limit, 1), 300)],
  );
  return result.rows.map(mapRow).reverse();
}
