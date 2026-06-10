import type { SavedPacket } from "../repositories/packet.repository.js";
import {
  decodePacket,
  mergeHealthReadings,
  PacketDecoderError,
  type DecodedBattery,
  type DecodedFirmware,
  type DecodedHealth,
  type DecodedHrvHistory,
  type DecodedMac,
  type DecodedPacket,
  type DecodedSleep,
} from "./packet-decoder.service.js";

/** Espaço entre pacotes do mesmo ciclo ESP32 (coleta BLE + histórico). */
const CYCLE_GAP_MS = 3 * 60 * 1000;

export type SnapshotSleep = {
  date: string;
  time: string;
  sleepMinutes: number;
  recordId: number;
};

export type SnapshotVitals = {
  heartRate: number;
  spo2: number;
  temperature: number;
  hrv: number;
  fatigue: number;
  systolicPressure: number;
  diastolicPressure: number;
};

export type MeasurementSnapshot = {
  id: number;
  deviceMac: string;
  source: string;
  measuredAt: string;
  vitals: SnapshotVitals;
  sleep: SnapshotSleep | null;
  battery: number | null;
  firmware: string | null;
  deviceMacReported: string | null;
  packetCount: number;
  failedCount: number;
};

function freshDecoded(packet: SavedPacket): DecodedPacket | null {
  if (!packet.crcValid || !packet.rawHex) return packet.decoded;
  try {
    return decodePacket(packet.packetType, packet.rawHex).decoded;
  } catch (err) {
    if (!(err instanceof PacketDecoderError)) throw err;
    return packet.decoded;
  }
}

function isMergeableHealth(
  decoded: DecodedPacket | null,
): decoded is DecodedHealth | DecodedHrvHistory {
  return decoded?.type === "0x28" || decoded?.type === "0x56";
}

function emptyVitals(): SnapshotVitals {
  return {
    heartRate: 0,
    spo2: 0,
    temperature: 0,
    hrv: 0,
    fatigue: 0,
    systolicPressure: 0,
    diastolicPressure: 0,
  };
}

function vitalsFromMerged(merged: DecodedHealth): SnapshotVitals {
  return {
    heartRate: merged.heartRate,
    spo2: merged.spo2,
    temperature: merged.temperature,
    hrv: merged.hrv,
    fatigue: merged.fatigue,
    systolicPressure: merged.systolicPressure,
    diastolicPressure: merged.diastolicPressure,
  };
}

function groupPacketsByCycle(packets: SavedPacket[]): SavedPacket[][] {
  if (packets.length === 0) return [];

  const sorted = [...packets].sort(
    (a, b) => new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime(),
  );

  const groups: SavedPacket[][] = [];

  for (const packet of sorted) {
    const current = groups[groups.length - 1];
    if (!current) {
      groups.push([packet]);
      continue;
    }

    const newestMs = new Date(current[0]!.createdAt).getTime();
    const packetMs = new Date(packet.createdAt).getTime();

    if (newestMs - packetMs <= CYCLE_GAP_MS) {
      current.push(packet);
    } else {
      groups.push([packet]);
    }
  }

  return groups;
}

function latestPacketByType<T extends DecodedPacket["type"]>(
  packets: SavedPacket[],
  type: T,
): (DecodedPacket & { type: T }) | null {
  const ordered = [...packets].sort(
    (a, b) => new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime(),
  );

  for (const packet of ordered) {
    const decoded = freshDecoded(packet);
    if (decoded?.type === type) {
      return decoded as DecodedPacket & { type: T };
    }
  }

  return null;
}

export function buildSnapshotFromPackets(packets: SavedPacket[]): MeasurementSnapshot | null {
  if (packets.length === 0) return null;

  const measuredAt = packets.reduce((latest, packet) => {
    const ts = new Date(packet.createdAt).getTime();
    return ts > latest ? ts : latest;
  }, 0);

  const healthDecoded = packets
    .map((packet) => freshDecoded(packet))
    .filter(isMergeableHealth);

  const vitals =
    healthDecoded.length > 0
      ? vitalsFromMerged(mergeHealthReadings(healthDecoded))
      : emptyVitals();

  const sleepDecoded = latestPacketByType(packets, "0x53") as DecodedSleep | null;
  const batteryDecoded = latestPacketByType(packets, "0x13") as DecodedBattery | null;
  const firmwareDecoded = latestPacketByType(packets, "0x27") as DecodedFirmware | null;
  const macDecoded = latestPacketByType(packets, "0x22") as DecodedMac | null;

  const anchor = packets.reduce((best, packet) => (packet.id > best.id ? packet : best), packets[0]!);
  const failedCount = packets.filter((packet) => packet.decodeError).length;

  return {
    id: anchor.id,
    deviceMac: anchor.deviceMac,
    source: anchor.source,
    measuredAt: new Date(measuredAt).toISOString(),
    vitals,
    sleep: sleepDecoded
      ? {
          date: sleepDecoded.date,
          time: sleepDecoded.time,
          sleepMinutes: sleepDecoded.sleepMinutes,
          recordId: sleepDecoded.recordId,
        }
      : null,
    battery: batteryDecoded?.battery ?? null,
    firmware: firmwareDecoded
      ? `V${firmwareDecoded.major}.${firmwareDecoded.minor}.${firmwareDecoded.patch}`
      : null,
    deviceMacReported: macDecoded?.mac ?? null,
    packetCount: packets.length,
    failedCount,
  };
}

export function buildSnapshotsFromPackets(
  packets: SavedPacket[],
  maxSnapshots?: number,
): MeasurementSnapshot[] {
  const snapshots = groupPacketsByCycle(packets)
    .map((group) => buildSnapshotFromPackets(group))
    .filter((snapshot): snapshot is MeasurementSnapshot => snapshot !== null);

  if (typeof maxSnapshots === "number" && maxSnapshots > 0) {
    return snapshots.slice(0, maxSnapshots);
  }

  return snapshots;
}

export type BatchProcessInput = {
  ok: boolean;
  id: number;
  packetType: string;
  receivedAtMs: number;
  savedAt: string;
  decoded?: DecodedPacket;
  error?: string;
};

/** Consolida o batch recém-processado (mesmo ciclo ESP32). */
export function buildSnapshotFromBatchResults(
  deviceMac: string,
  source: string,
  results: BatchProcessInput[],
): MeasurementSnapshot | null {
  const packets: SavedPacket[] = results.map((result) => ({
    id: result.id,
    deviceMac,
    packetType: result.packetType,
    rawHex: "",
    source,
    bytes: null,
    crcValid: result.ok,
    decoded: result.ok ? (result.decoded ?? null) : null,
    decodeError: result.ok ? null : (result.error ?? "decode error"),
    createdAt: result.savedAt,
  }));

  return buildSnapshotFromPackets(packets);
}
