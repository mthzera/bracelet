import type { PacketMetrics } from "../schemas/packet.schema.js";

export type VitalTimeKey =
  | "heartRate"
  | "spo2"
  | "temperature"
  | "hrv"
  | "fatigue"
  | "bloodPressure"
  | "battery";

export type VitalMeasuredAtMap = Partial<Record<VitalTimeKey, string>>;

const OFFSET_KEY_TO_VITAL: Record<string, VitalTimeKey> = {
  bpm: "heartRate",
  spo2: "spo2",
  temperature: "temperature",
  hrv: "hrv",
  fatigue: "fatigue",
  bloodPressure: "bloodPressure",
  battery: "battery",
};

export type TimingBatchExtras = {
  postDelayMs?: number;
  collectionPostedAt?: string;
};

type MetricsWithTiming = PacketMetrics & {
  collectionDurationMs?: number;
  measurementTimestampsMs?: Record<string, number | null>;
};

/** Estima o instante de cada vital a partir dos offsets da sessão BLE. */
export function computeVitalMeasuredAt(
  metrics: PacketMetrics | undefined,
  receivedAt: string,
  batchExtras?: TimingBatchExtras,
): VitalMeasuredAtMap {
  const m = metrics as MetricsWithTiming | undefined;
  const offsets = m?.measurementTimestampsMs;
  const duration = m?.collectionDurationMs;
  if (!offsets || typeof duration !== "number" || duration <= 0) {
    return {};
  }

  let sessionStartMs: number;

  if (
    batchExtras?.collectionPostedAt &&
    typeof batchExtras.postDelayMs === "number" &&
    batchExtras.postDelayMs >= 0
  ) {
    const postedMs = new Date(batchExtras.collectionPostedAt).getTime();
    sessionStartMs = postedMs - batchExtras.postDelayMs - duration;
  } else {
    const BOOT2_GAP_MS = 12_000;
    sessionStartMs = new Date(receivedAt).getTime() - duration - BOOT2_GAP_MS;
  }

  const result: VitalMeasuredAtMap = {};

  for (const [offsetKey, vitalKey] of Object.entries(OFFSET_KEY_TO_VITAL)) {
    const offset = offsets[offsetKey];
    if (typeof offset === "number" && offset >= 0) {
      result[vitalKey] = new Date(sessionStartMs + offset).toISOString();
    }
  }

  return result;
}

export function batteryFromSnapshotMetrics(metrics: PacketMetrics | undefined): number | null {
  const value = metrics?.battery;
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  if (value < 0 || value > 100) return null;
  return Math.round(value);
}
