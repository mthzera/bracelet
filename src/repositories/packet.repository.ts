import { getPool } from "../database/db.js";
import type { PacketPayload } from "../schemas/packet.schema.js";
import {
  decodePacket,
  mergeHealthReadings,
  PacketDecoderError,
  type DecodedHealth,
  type DecodedHrvHistory,
  type DecodedPacket,
} from "../services/packet-decoder.service.js";

export type SavePacketInput = {
  payload: PacketPayload;
  bytes?: number[];
  crcValid: boolean;
  decoded?: DecodedPacket;
  decodeError?: string;
  receivedAtMs?: number;
  ingestionBatchId?: string;
  rawSnapshot?: unknown;
};

export type SavedPacket = {
  id: number;
  deviceMac: string;
  packetType: string;
  rawHex: string;
  source: string;
  bytes: number[] | null;
  crcValid: boolean;
  decoded: DecodedPacket | null;
  mergedHealth?: DecodedHealth | null;
  decodeError: string | null;
  createdAt: string;
  ingestionBatchId?: string | null;
  rawSnapshot?: unknown;
};

const HEALTH_MERGE_WINDOW_MS = 5 * 60 * 1000;

type PacketRow = {
  id: number;
  device_mac: string;
  packet_type: string;
  raw_hex: string;
  source: string;
  bytes: number[] | null;
  crc_valid: boolean;
  decoded: DecodedPacket | null;
  decode_error: string | null;
  created_at: Date;
  ingestion_batch_id: string | null;
  raw_snapshot: unknown | null;
};

function isHealthDecoded(
  decoded: DecodedPacket | null,
): decoded is DecodedHealth | DecodedHrvHistory {
  return decoded?.type === "0x28" || decoded?.type === "0x56";
}

function freshDecoded(packet: SavedPacket): DecodedPacket | null {
  if (!packet.crcValid || !packet.rawHex) return packet.decoded;
  try {
    return decodePacket(packet.packetType, packet.rawHex).decoded;
  } catch (err) {
    if (!(err instanceof PacketDecoderError)) throw err;
    return packet.decoded;
  }
}

function attachMergedHealth(packets: SavedPacket[]): SavedPacket[] {
  const byDevice = new Map<string, SavedPacket[]>();

  for (const packet of packets) {
    if (packet.packetType !== "0x28" && packet.packetType !== "0x56") continue;
    const list = byDevice.get(packet.deviceMac) ?? [];
    list.push(packet);
    byDevice.set(packet.deviceMac, list);
  }

  for (const devicePackets of byDevice.values()) {
    const sorted = [...devicePackets].sort(
      (a, b) => new Date(a.createdAt).getTime() - new Date(b.createdAt).getTime(),
    );

    for (let i = 0; i < sorted.length; i++) {
      const anchor = sorted[i]!;
      const anchorTime = new Date(anchor.createdAt).getTime();
      const group = sorted.filter((packet) => {
        const delta = Math.abs(new Date(packet.createdAt).getTime() - anchorTime);
        return delta <= HEALTH_MERGE_WINDOW_MS;
      });

      const decodedGroup = group
        .map((packet) => freshDecoded(packet))
        .filter(isHealthDecoded);

      if (decodedGroup.length === 0) continue;

      anchor.mergedHealth = mergeHealthReadings(decodedGroup);
    }
  }

  return packets;
}

function rowToSavedPacket(row: PacketRow): SavedPacket {
  return {
    id: row.id,
    deviceMac: row.device_mac,
    packetType: row.packet_type,
    rawHex: row.raw_hex,
    source: row.source,
    bytes: row.bytes,
    crcValid: row.crc_valid,
    decoded: row.decoded,
    decodeError: row.decode_error,
    createdAt: row.created_at.toISOString(),
    ingestionBatchId: row.ingestion_batch_id,
    rawSnapshot: row.raw_snapshot ?? undefined,
  };
}

export async function savePacket(input: SavePacketInput): Promise<SavedPacket> {
  const pool = getPool();

  const bytesJson = input.bytes ? JSON.stringify(input.bytes) : null;
  const decodedJson = input.decoded ? JSON.stringify(input.decoded) : null;
  const rawSnapshotJson = input.rawSnapshot !== undefined ? JSON.stringify(input.rawSnapshot) : null;

  // ESP32 sends millis() (uptime since boot), not a Unix timestamp.
  // A valid Unix ms timestamp for 2020+ is > 1_577_836_800_000 (13 digits).
  // Anything smaller is uptime — fall back to DB now().
  const MIN_UNIX_MS = 1_577_836_800_000;
  const createdAt =
    input.receivedAtMs !== undefined && input.receivedAtMs >= MIN_UNIX_MS
      ? new Date(input.receivedAtMs).toISOString()
      : null;

  const result = await pool.query<PacketRow>(
    `
      INSERT INTO packets (
        device_mac,
        packet_type,
        raw_hex,
        source,
        bytes,
        crc_valid,
        decoded,
        decode_error,
        created_at,
        ingestion_batch_id,
        raw_snapshot
      )
      VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7::jsonb, $8, COALESCE($9::timestamptz, now()), $10, $11::jsonb)
      RETURNING id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at, ingestion_batch_id, raw_snapshot
    `,
    [
      input.payload.deviceMac,
      input.payload.packetType,
      input.payload.rawHex,
      input.payload.source,
      bytesJson,
      input.crcValid,
      decodedJson,
      input.decodeError ?? null,
      createdAt,
      input.ingestionBatchId ?? null,
      rawSnapshotJson,
    ],
  );

  return rowToSavedPacket(result.rows[0] as PacketRow);
}

export async function listPackets(limit = 50, deviceMac?: string): Promise<SavedPacket[]> {
  const pool = getPool();
  const safeLimit = Math.min(Math.max(limit, 1), 200);

  const params: Array<string | number> = [safeLimit];
  let deviceFilter = "";

  if (deviceMac?.trim()) {
    params.push(deviceMac.trim().toUpperCase());
    deviceFilter = "WHERE UPPER(device_mac) = $2";
  }

  const { rows } = await pool.query<PacketRow>(
    `
      SELECT id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
      FROM packets
      ${deviceFilter}
      ORDER BY id DESC
      LIMIT $1
    `,
    params,
  );

  return attachMergedHealth(rows.map(rowToSavedPacket));
}

export async function getLatestPacketForDevice(
  deviceMac: string,
  packetType?: string,
): Promise<SavedPacket | null> {
  const pool = getPool();
  const params: string[] = [deviceMac.trim().toUpperCase()];
  let typeFilter = "";

  if (packetType?.trim()) {
    params.push(packetType.trim().toLowerCase());
    typeFilter = "AND LOWER(packet_type) = $2";
  }

  const { rows } = await pool.query<PacketRow>(
    `
      SELECT id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
      FROM packets
      WHERE UPPER(device_mac) = $1
        ${typeFilter}
      ORDER BY id DESC
      LIMIT 1
    `,
    params,
  );

  if (rows.length === 0) return null;
  return rowToSavedPacket(rows[0] as PacketRow);
}

export async function getRecentHealthPackets(
  deviceMac: string,
  limit = 12,
): Promise<SavedPacket[]> {
  const pool = getPool();
  const safeLimit = Math.min(Math.max(limit, 1), 30);

  const { rows } = await pool.query<PacketRow>(
    `
      SELECT id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
      FROM packets
      WHERE device_mac = $1
        AND packet_type IN ('0x28', '0x56')
        AND crc_valid = TRUE
        AND decoded IS NOT NULL
        AND created_at > now() - interval '15 minutes'
      ORDER BY created_at DESC
      LIMIT $2
    `,
    [deviceMac, safeLimit],
  );

  return attachMergedHealth(rows.map(rowToSavedPacket));
}

function snapshotToMergedHealth(snapshot: DecodedPacket & { type: "snapshot" }): DecodedHealth {
  return {
    type: "0x28",
    measurementType: 0,
    heartRate: snapshot.heartRate ?? 0,
    spo2: snapshot.spo2 ?? 0,
    temperature: snapshot.temperature ?? 0,
    hrv: snapshot.hrv ?? 0,
    fatigue: snapshot.fatigue ?? 0,
    systolicPressure: snapshot.systolicPressure ?? 0,
    diastolicPressure: snapshot.diastolicPressure ?? 0,
    measurementMode: "unknown",
  };
}

export async function getMergedHealthForDevice(
  deviceMac: string,
  windowMinutes = 5,
): Promise<DecodedHealth | null> {
  const cutoff = Date.now() - windowMinutes * 60 * 1000;
  const snapshotPacket = await getLastSnapshotVitalsForDevice(deviceMac);
  if (
    snapshotPacket &&
    new Date(snapshotPacket.createdAt).getTime() >= cutoff &&
    snapshotPacket.decoded?.type === "snapshot"
  ) {
    return snapshotToMergedHealth(snapshotPacket.decoded);
  }

  const recent = await getRecentHealthPackets(deviceMac, 12);
  const decoded = recent
    .filter((packet) => new Date(packet.createdAt).getTime() >= cutoff)
    .map((packet) => freshDecoded(packet))
    .filter(isHealthDecoded);

  if (decoded.length === 0) return null;
  return mergeHealthReadings(decoded);
}

/** Retorna o último pacote SNAPSHOT_VITALS salvo para o dispositivo, para comparação de staleness. */
export async function getLastSnapshotVitalsForDevice(
  deviceMac: string,
): Promise<SavedPacket | null> {
  const pool = getPool();
  const { rows } = await pool.query<PacketRow>(
    `
      SELECT id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at, ingestion_batch_id, raw_snapshot
      FROM packets
      WHERE UPPER(device_mac) = $1
        AND LOWER(packet_type) = 'snapshot_vitals'
      ORDER BY id DESC
      LIMIT 1
    `,
    [deviceMac.trim().toUpperCase()],
  );
  if (rows.length === 0) return null;
  return rowToSavedPacket(rows[0] as PacketRow);
}
