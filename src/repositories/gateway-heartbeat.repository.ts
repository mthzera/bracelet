import { getPool } from "../database/db.js";

export type GatewayHeartbeatInput = {
  tabletId: string;
  tabletLabel?: string;
  appVersion?: string;
  braceletMac?: string | null;
  bleConnected?: boolean;
  isMeasuring?: boolean;
  phase?: string;
  statusMessage?: string;
  pendingSyncCount?: number;
  lastSyncOk?: boolean | null;
  lastSyncAt?: string | null;
  lastError?: string | null;
  apiReachable?: boolean | null;
  sentAt?: string | null;
  tabletBatteryPercent?: number | null;
  tabletCharging?: boolean | null;
  deviceModel?: string | null;
  deviceManufacturer?: string | null;
  androidId?: string | null;
  sessionUser?: string | null;
  sessionRole?: string | null;
};

export type GatewayHeartbeatRow = {
  tabletId: string;
  tabletLabel: string;
  appVersion: string;
  braceletMac: string | null;
  bleConnected: boolean;
  isMeasuring: boolean;
  phase: string;
  statusMessage: string;
  pendingSyncCount: number;
  lastSyncOk: boolean | null;
  lastSyncAt: string | null;
  lastError: string | null;
  apiReachable: boolean | null;
  clientSentAt: string | null;
  receivedAt: string;
  firstSeenAt: string;
  online: boolean;
  tabletBatteryPercent: number | null;
  tabletCharging: boolean | null;
  deviceModel: string | null;
  deviceManufacturer: string | null;
  androidId: string | null;
  sessionUser: string | null;
  sessionRole: string | null;
};

const ONLINE_WINDOW_MS = 2 * 60 * 1000;

type DbRow = {
  tablet_id: string;
  tablet_label: string;
  app_version: string;
  bracelet_mac: string | null;
  ble_connected: boolean;
  is_measuring: boolean;
  phase: string;
  status_message: string;
  pending_sync_count: number;
  last_sync_ok: boolean | null;
  last_sync_at: Date | null;
  last_error: string | null;
  api_reachable: boolean | null;
  client_sent_at: Date | null;
  received_at: Date;
  first_seen_at: Date;
  tablet_battery_percent: number | null;
  tablet_charging: boolean | null;
  device_model: string | null;
  device_manufacturer: string | null;
  android_id: string | null;
  session_user: string | null;
  session_role: string | null;
};

function toIso(value: Date | null | undefined): string | null {
  return value ? value.toISOString() : null;
}

function mapRow(row: DbRow, nowMs = Date.now()): GatewayHeartbeatRow {
  const receivedMs = row.received_at.getTime();
  return {
    tabletId: row.tablet_id,
    tabletLabel: row.tablet_label ?? "",
    appVersion: row.app_version ?? "",
    braceletMac: row.bracelet_mac,
    bleConnected: Boolean(row.ble_connected),
    isMeasuring: Boolean(row.is_measuring),
    phase: row.phase ?? "",
    statusMessage: row.status_message ?? "",
    pendingSyncCount: Number(row.pending_sync_count) || 0,
    lastSyncOk: row.last_sync_ok,
    lastSyncAt: toIso(row.last_sync_at),
    lastError: row.last_error,
    apiReachable: row.api_reachable,
    clientSentAt: toIso(row.client_sent_at),
    receivedAt: row.received_at.toISOString(),
    firstSeenAt: (row.first_seen_at ?? row.received_at).toISOString(),
    online: Number.isFinite(receivedMs) && nowMs - receivedMs < ONLINE_WINDOW_MS,
    tabletBatteryPercent:
      typeof row.tablet_battery_percent === "number" ? row.tablet_battery_percent : null,
    tabletCharging: row.tablet_charging,
    deviceModel: row.device_model,
    deviceManufacturer: row.device_manufacturer,
    androidId: row.android_id,
    sessionUser: row.session_user,
    sessionRole: row.session_role,
  };
}

export async function upsertGatewayHeartbeat(
  input: GatewayHeartbeatInput,
): Promise<GatewayHeartbeatRow> {
  const pool = getPool();
  const tabletId = input.tabletId.trim();
  if (!tabletId) {
    throw new Error("tabletId is required");
  }

  const battery =
    typeof input.tabletBatteryPercent === "number" && Number.isFinite(input.tabletBatteryPercent)
      ? Math.round(Math.min(100, Math.max(0, input.tabletBatteryPercent)))
      : null;

  const result = await pool.query<DbRow>(
    `
    INSERT INTO gateway_heartbeats (
      tablet_id,
      tablet_label,
      app_version,
      bracelet_mac,
      ble_connected,
      is_measuring,
      phase,
      status_message,
      pending_sync_count,
      last_sync_ok,
      last_sync_at,
      last_error,
      api_reachable,
      client_sent_at,
      received_at,
      first_seen_at,
      tablet_battery_percent,
      tablet_charging,
      device_model,
      device_manufacturer,
      android_id,
      session_user,
      session_role
    ) VALUES (
      $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14,
      now(), now(), $15, $16, $17, $18, $19, $20, $21
    )
    ON CONFLICT (tablet_id) DO UPDATE SET
      tablet_label = EXCLUDED.tablet_label,
      app_version = EXCLUDED.app_version,
      bracelet_mac = EXCLUDED.bracelet_mac,
      ble_connected = EXCLUDED.ble_connected,
      is_measuring = EXCLUDED.is_measuring,
      phase = EXCLUDED.phase,
      status_message = EXCLUDED.status_message,
      pending_sync_count = EXCLUDED.pending_sync_count,
      last_sync_ok = EXCLUDED.last_sync_ok,
      last_sync_at = EXCLUDED.last_sync_at,
      last_error = EXCLUDED.last_error,
      api_reachable = EXCLUDED.api_reachable,
      client_sent_at = EXCLUDED.client_sent_at,
      received_at = now(),
      tablet_battery_percent = EXCLUDED.tablet_battery_percent,
      tablet_charging = EXCLUDED.tablet_charging,
      device_model = EXCLUDED.device_model,
      device_manufacturer = EXCLUDED.device_manufacturer,
      android_id = EXCLUDED.android_id,
      session_user = EXCLUDED.session_user,
      session_role = EXCLUDED.session_role
    RETURNING *
    `,
    [
      tabletId,
      (input.tabletLabel ?? "").trim(),
      (input.appVersion ?? "").trim(),
      input.braceletMac?.trim().toUpperCase() || null,
      Boolean(input.bleConnected),
      Boolean(input.isMeasuring),
      (input.phase ?? "").trim(),
      (input.statusMessage ?? "").trim(),
      Number.isFinite(input.pendingSyncCount) ? Number(input.pendingSyncCount) : 0,
      input.lastSyncOk ?? null,
      input.lastSyncAt ? new Date(input.lastSyncAt) : null,
      input.lastError?.trim() || null,
      input.apiReachable ?? null,
      input.sentAt ? new Date(input.sentAt) : null,
      battery,
      input.tabletCharging ?? null,
      input.deviceModel?.trim() || null,
      input.deviceManufacturer?.trim() || null,
      input.androidId?.trim() || null,
      input.sessionUser?.trim() || null,
      input.sessionRole?.trim() || null,
    ],
  );

  return mapRow(result.rows[0]);
}

export async function listGatewayHeartbeats(): Promise<GatewayHeartbeatRow[]> {
  const pool = getPool();
  const result = await pool.query<DbRow>(
    `
    SELECT *
    FROM gateway_heartbeats
    ORDER BY received_at DESC
    `,
  );
  const nowMs = Date.now();
  return result.rows.map((row) => mapRow(row, nowMs));
}
