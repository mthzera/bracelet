import { getPool } from "../database/db.js";
import type { VitalsInput } from "../types/clinical-alerts.types.js";
import {
  mergeHealthReadings,
  type DecodedHealth,
  type DecodedHrvHistory,
} from "../services/packet-decoder.service.js";

export type VitalsHistoryPoint = {
  measuredAt: string;
  heartRate: number;
  spo2: number;
  temperature: number;
  systolic: number;
  diastolic: number;
};

const HEALTH_MERGE_WINDOW_MS = 5 * 60 * 1000;

function isHealthDecoded(
  decoded: DecodedHealth | DecodedHrvHistory | null | undefined,
): decoded is DecodedHealth | DecodedHrvHistory {
  return decoded?.type === "0x28" || decoded?.type === "0x56";
}

function vitalsFromDecoded(decoded: DecodedHealth): VitalsHistoryPoint | null {
  if (decoded.heartRate <= 0 && decoded.spo2 <= 0 && decoded.temperature <= 0) {
    return null;
  }
  return {
    measuredAt: "",
    heartRate: decoded.heartRate,
    spo2: decoded.spo2,
    temperature: decoded.temperature,
    systolic: decoded.systolicPressure,
    diastolic: decoded.diastolicPressure,
  };
}

function vitalsFromAssessment(vitals: VitalsInput, measuredAt: string): VitalsHistoryPoint | null {
  if (vitals.heartRate <= 0 && vitals.spo2 <= 0 && vitals.temperature <= 0) {
    return null;
  }
  return {
    measuredAt,
    heartRate: vitals.heartRate,
    spo2: vitals.spo2,
    temperature: vitals.temperature,
    systolic: vitals.systolic,
    diastolic: vitals.diastolic,
  };
}

type PacketRow = {
  created_at: Date;
  decoded: DecodedHealth | DecodedHrvHistory | null;
};

function clusterPacketRows(rows: PacketRow[]): PacketRow[][] {
  const sorted = [...rows].sort(
    (a, b) => a.created_at.getTime() - b.created_at.getTime(),
  );
  const groups: PacketRow[][] = [];
  let current: PacketRow[] = [];

  for (const row of sorted) {
    if (current.length === 0) {
      current.push(row);
      continue;
    }

    const lastTime = current[current.length - 1]!.created_at.getTime();
    const rowTime = row.created_at.getTime();
    if (rowTime - lastTime <= HEALTH_MERGE_WINDOW_MS) {
      current.push(row);
    } else {
      groups.push(current);
      current = [row];
    }
  }

  if (current.length > 0) groups.push(current);
  return groups;
}

function pointsFromMergedPackets(rows: PacketRow[]): VitalsHistoryPoint[] {
  const points: VitalsHistoryPoint[] = [];

  for (const group of clusterPacketRows(rows)) {
    const decodedList = group
      .map((row) => row.decoded)
      .filter(isHealthDecoded);

    if (decodedList.length === 0) continue;

    const merged = mergeHealthReadings(decodedList);
    const point = vitalsFromDecoded(merged);
    if (!point) continue;

    point.measuredAt = group[0]!.created_at.toISOString();
    points.push(point);
  }

  return points;
}

export async function getVitalsHistoryForDevice(
  deviceMac: string,
  windowMinutes: number,
): Promise<VitalsHistoryPoint[]> {
  const pool = getPool();
  const mac = deviceMac.trim().toUpperCase();
  const byTime = new Map<string, VitalsHistoryPoint>();

  const { rows: assessments } = await pool.query<{
    measured_at: Date;
    vitals: VitalsInput;
  }>(
    `
      SELECT measured_at, vitals
      FROM vital_assessments
      WHERE UPPER(device_mac) = $1
        AND measured_at >= now() - ($2::int * interval '1 minute')
      ORDER BY measured_at ASC
    `,
    [mac, windowMinutes],
  );

  for (const row of assessments) {
    const point = vitalsFromAssessment(row.vitals, row.measured_at.toISOString());
    if (point) byTime.set(point.measuredAt, point);
  }

  const { rows: packets } = await pool.query<PacketRow>(
    `
      SELECT created_at, decoded
      FROM packets
      WHERE UPPER(device_mac) = $1
        AND packet_type IN ('0x28', '0x56')
        AND decoded IS NOT NULL
        AND created_at >= now() - ($2::int * interval '1 minute')
      ORDER BY created_at ASC
    `,
    [mac, windowMinutes],
  );

  for (const point of pointsFromMergedPackets(packets)) {
    if (!byTime.has(point.measuredAt)) {
      byTime.set(point.measuredAt, point);
    }
  }

  return [...byTime.values()].sort(
    (a, b) => new Date(a.measuredAt).getTime() - new Date(b.measuredAt).getTime(),
  );
}
