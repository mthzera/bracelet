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
  };
}

export async function savePacket(input: SavePacketInput): Promise<SavedPacket> {
  const pool = getPool();

  const bytesJson = input.bytes ? JSON.stringify(input.bytes) : null;
  const decodedJson = input.decoded ? JSON.stringify(input.decoded) : null;

  const createdAt =
    input.receivedAtMs !== undefined
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
        created_at
      )
      VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7::jsonb, $8, COALESCE($9::timestamptz, now()))
      RETURNING id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
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

export async function getMergedHealthForDevice(
  deviceMac: string,
  windowMinutes = 5,
): Promise<DecodedHealth | null> {
  const recent = await getRecentHealthPackets(deviceMac, 12);
  const cutoff = Date.now() - windowMinutes * 60 * 1000;
  const decoded = recent
    .filter((packet) => new Date(packet.createdAt).getTime() >= cutoff)
    .map((packet) => freshDecoded(packet))
    .filter(isHealthDecoded);

  if (decoded.length === 0) return null;
  return mergeHealthReadings(decoded);
}
