import { getPool } from "../database/db.js";
import type { PacketPayload } from "../schemas/packet.schema.js";
import type { DecodedPacket } from "../services/packet-decoder.service.js";

export type SavePacketInput = {
  payload: PacketPayload;
  bytes?: number[];
  crcValid: boolean;
  decoded?: DecodedPacket;
  decodeError?: string;
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
  decodeError: string | null;
  createdAt: string;
};

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
        decode_error
      )
      VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
      RETURNING id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
    `,
    [
      input.payload.deviceMac,
      input.payload.packetType,
      input.payload.rawHex,
      input.payload.source,
      input.bytes ?? null,
      input.crcValid,
      input.decoded ?? null,
      input.decodeError ?? null,
    ],
  );

  return rowToSavedPacket(result.rows[0] as PacketRow);
}

export async function listPackets(limit = 50): Promise<SavedPacket[]> {
  const pool = getPool();
  const safeLimit = Math.min(Math.max(limit, 1), 200);

  const { rows } = await pool.query<PacketRow>(
    `
      SELECT id, device_mac, packet_type, raw_hex, source, bytes, crc_valid, decoded, decode_error, created_at
      FROM packets
      ORDER BY id DESC
      LIMIT $1
    `,
    [safeLimit],
  );

  return rows.map(rowToSavedPacket);
}
