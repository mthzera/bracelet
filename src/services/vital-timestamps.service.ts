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

/** Unix ms a partir de 2020-01-01 — abaixo disso tratamos como offset de sessão (millis do ESP32). */
const MIN_UNIX_MS = 1_577_836_800_000;

const OFFSET_KEY_TO_VITAL: Record<string, VitalTimeKey> = {
  bpm: "heartRate",
  heartRate: "heartRate",
  spo2: "spo2",
  temperature: "temperature",
  hrv: "hrv",
  fatigue: "fatigue",
  bloodPressure: "bloodPressure",
  battery: "battery",
};

/** Campos *AtMs do snapshot ESP32 (millis desde boot). */
const ESP_AT_MS_TO_VITAL: Record<string, VitalTimeKey> = {
  bpmAtMs: "heartRate",
  spo2AtMs: "spo2",
  temperatureAtMs: "temperature",
  hrvAtMs: "hrv",
  fatigueAtMs: "fatigue",
  bloodPressureAtMs: "bloodPressure",
  batteryAtMs: "battery",
};

/** Campos ISO opcionais enviados pelo app mobile. */
const ISO_AT_TO_VITAL: Record<string, VitalTimeKey> = {
  bpmAt: "heartRate",
  spo2At: "spo2",
  temperatureAt: "temperature",
  hrvAt: "hrv",
  fatigueAt: "fatigue",
  bloodPressureAt: "bloodPressure",
  batteryAt: "battery",
};

export type TimingBatchExtras = {
  postDelayMs?: number;
  collectionPostedAt?: string;
  /** millis() do ESP32 no início da sessão de coleta */
  measurementSessionStartedAtMs?: number;
  /** millis() do ESP32 no momento do pacote (receivedAtMs relativo) */
  deviceReceivedAtMs?: number;
};

type MetricsWithTiming = PacketMetrics & {
  collectionDurationMs?: number;
  measurementTimestampsMs?: Record<string, number | null>;
  measurementSessionStartedAtMs?: number;
  [key: string]: unknown;
};

function isAbsoluteUnixMs(value: number): boolean {
  return value >= MIN_UNIX_MS;
}

function toIsoIfValid(ms: number): string | null {
  if (!Number.isFinite(ms)) return null;
  const d = new Date(ms);
  if (Number.isNaN(d.getTime())) return null;
  return d.toISOString();
}

/**
 * Estima o instante de cada vital.
 * Aceita:
 *  - measurementTimestampsMs com epoch Unix (app mobile)
 *  - measurementTimestampsMs com offset de sessão (legado)
 *  - *At ISO strings (app mobile)
 *  - *AtMs + measurementSessionStartedAtMs do ESP32 (millis relativos ao boot)
 */
export function computeVitalMeasuredAt(
  metrics: PacketMetrics | undefined,
  receivedAt: string,
  batchExtras?: TimingBatchExtras,
): VitalMeasuredAtMap {
  const m = metrics as MetricsWithTiming | undefined;
  if (!m) return {};

  const result: VitalMeasuredAtMap = {};
  const receivedAtMs = new Date(receivedAt).getTime();

  // 1) ISO explícito (mobile): prioridade máxima
  for (const [key, vitalKey] of Object.entries(ISO_AT_TO_VITAL)) {
    const raw = m[key];
    if (typeof raw !== "string" || raw.length === 0) continue;
    const ms = new Date(raw).getTime();
    const iso = toIsoIfValid(ms);
    if (iso) result[vitalKey] = iso;
  }

  // 2) measurementTimestampsMs: absoluto ou offset
  const offsets = m.measurementTimestampsMs;
  const duration = m.collectionDurationMs;
  if (offsets && typeof offsets === "object") {
    let sessionStartMs: number | null = null;

    if (
      batchExtras?.collectionPostedAt &&
      typeof batchExtras.postDelayMs === "number" &&
      batchExtras.postDelayMs >= 0 &&
      typeof duration === "number" &&
      duration > 0
    ) {
      const postedMs = new Date(batchExtras.collectionPostedAt).getTime();
      sessionStartMs = postedMs - batchExtras.postDelayMs - duration;
    } else if (typeof duration === "number" && duration > 0) {
      const BOOT2_GAP_MS = 12_000;
      sessionStartMs = receivedAtMs - duration - BOOT2_GAP_MS;
    }

    for (const [offsetKey, value] of Object.entries(offsets)) {
      if (typeof value !== "number" || value < 0) continue;
      const vitalKey = OFFSET_KEY_TO_VITAL[offsetKey];
      if (!vitalKey || result[vitalKey]) continue;

      if (isAbsoluteUnixMs(value)) {
        const iso = toIsoIfValid(value);
        if (iso) result[vitalKey] = iso;
        continue;
      }

      if (sessionStartMs != null) {
        const iso = toIsoIfValid(sessionStartMs + value);
        if (iso) result[vitalKey] = iso;
      }
    }
  }

  // 3) ESP32: bpmAtMs etc. são millis() desde o boot
  const sessionStartedAtMs =
    batchExtras?.measurementSessionStartedAtMs ??
    (typeof m.measurementSessionStartedAtMs === "number"
      ? m.measurementSessionStartedAtMs
      : undefined);
  const deviceReceivedAtMs =
    batchExtras?.deviceReceivedAtMs ??
    (typeof m.receivedAtMs === "number" ? m.receivedAtMs : undefined);

  if (typeof sessionStartedAtMs === "number" && sessionStartedAtMs >= 0) {
    // Relógio de parede no início da sessão ≈ created_at - (millisPacote - millisSessão)
    const deviceNowMs =
      typeof deviceReceivedAtMs === "number" && deviceReceivedAtMs >= sessionStartedAtMs
        ? deviceReceivedAtMs
        : undefined;
    const wallSessionStartMs =
      deviceNowMs != null ? receivedAtMs - (deviceNowMs - sessionStartedAtMs) : receivedAtMs;

    for (const [atKey, vitalKey] of Object.entries(ESP_AT_MS_TO_VITAL)) {
      if (result[vitalKey]) continue;
      const atMs = m[atKey];
      if (typeof atMs !== "number" || atMs < 0) continue;

      if (isAbsoluteUnixMs(atMs)) {
        const iso = toIsoIfValid(atMs);
        if (iso) result[vitalKey] = iso;
        continue;
      }

      const iso = toIsoIfValid(wallSessionStartMs + (atMs - sessionStartedAtMs));
      if (iso) result[vitalKey] = iso;
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
