import { getPool } from "../database/db.js";
import type { VitalsInput } from "../types/clinical-alerts.types.js";
import type { DecodedHealth } from "../services/packet-decoder.service.js";

export type VitalsHistoryPoint = {
  measuredAt: string;
  heartRate: number;
  spo2: number;
  temperature: number;
  systolic: number;
  diastolic: number;
};

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

  const { rows: packets } = await pool.query<{
    created_at: Date;
    decoded: DecodedHealth | null;
  }>(
    `
      SELECT created_at, decoded
      FROM packets
      WHERE UPPER(device_mac) = $1
        AND packet_type = '0x28'
        AND decoded IS NOT NULL
        AND created_at >= now() - ($2::int * interval '1 minute')
      ORDER BY created_at ASC
    `,
    [mac, windowMinutes],
  );

  for (const row of packets) {
    if (!row.decoded || row.decoded.type !== "0x28") continue;
    const base = vitalsFromDecoded(row.decoded);
    if (!base) continue;
    const measuredAt = row.created_at.toISOString();
    if (!byTime.has(measuredAt)) {
      byTime.set(measuredAt, { ...base, measuredAt });
    }
  }

  return [...byTime.values()].sort(
    (a, b) => new Date(a.measuredAt).getTime() - new Date(b.measuredAt).getTime(),
  );
}
