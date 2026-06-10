import type { PacketMetrics } from "../schemas/packet.schema.js";
import type { DecodedHealth } from "./packet-decoder.service.js";

export type MandatoryVitals = {
  heartRate: number;
  spo2: number;
  temperature: number;
};

export const MANDATORY_VITALS_ERROR =
  "Health packet requires heartRate (bpm), spO2 (%), and temperature (°C) — all must be > 0";

export const EMPTY_HEALTH_READING_ERROR =
  "Health packet has no measurable vital values (all zero)";

export type FullVitals = {
  heartRate: number;
  spo2: number;
  temperature: number;
  hrv: number;
  systolicPressure: number;
  diastolicPressure: number;
};

const FULL_VITAL_LABELS: Record<keyof FullVitals, string> = {
  heartRate: "heartRate",
  spo2: "spo2",
  temperature: "temperature",
  hrv: "hrv",
  systolicPressure: "systolicPressure",
  diastolicPressure: "diastolicPressure",
};

export function hasCompleteVitals(vitals: FullVitals): boolean {
  return (
    vitals.heartRate > 0 &&
    vitals.spo2 > 0 &&
    vitals.temperature > 0 &&
    vitals.hrv > 0 &&
    vitals.systolicPressure > 0 &&
    vitals.diastolicPressure > 0
  );
}

export function missingVitalFields(vitals: FullVitals): string[] {
  return (Object.keys(FULL_VITAL_LABELS) as Array<keyof FullVitals>).filter(
    (key) => (vitals[key] ?? 0) <= 0,
  );
}

export function hasMandatoryVitals(vitals: MandatoryVitals): boolean {
  return vitals.heartRate > 0 && vitals.spo2 > 0 && vitals.temperature > 0;
}

/** Aceita leituras parciais do ESP32 (HR, SpO2 ou temp em pacotes separados). */
export function hasAnyHealthReading(decoded: {
  heartRate?: number;
  spo2?: number;
  temperature?: number;
  hrv?: number;
  systolicPressure?: number;
  diastolicPressure?: number;
}): boolean {
  return (
    (decoded.heartRate ?? 0) > 0 ||
    (decoded.spo2 ?? 0) > 0 ||
    (decoded.temperature ?? 0) > 0 ||
    (decoded.hrv ?? 0) > 0 ||
    (decoded.systolicPressure ?? 0) > 0 ||
    (decoded.diastolicPressure ?? 0) > 0
  );
}

export function mandatoryVitalsFromDecoded(decoded: {
  heartRate?: number;
  spo2?: number;
  temperature?: number;
}): MandatoryVitals {
  return {
    heartRate: decoded.heartRate ?? 0,
    spo2: decoded.spo2 ?? 0,
    temperature: decoded.temperature ?? 0,
  };
}

function parseBloodPressure(value: string): { systolic: number; diastolic: number } {
  const match = value.match(/(\d+)\s*\/\s*(\d+)/);
  if (!match) return { systolic: 0, diastolic: 0 };
  return { systolic: Number(match[1]), diastolic: Number(match[2]) };
}

/** Alinha decoded com metrics do ESP32 para histórico e alertas usarem os mesmos valores. */
export function enrichDecodedHealthFromMetrics(
  decoded: DecodedHealth,
  metrics: PacketMetrics,
): DecodedHealth {
  const bp = metrics.bloodPressure
    ? parseBloodPressure(metrics.bloodPressure)
    : { systolic: 0, diastolic: 0 };

  const heartRate = metrics.heartRate ?? 0;
  const spO2 = metrics.spO2 ?? 0;
  const temperature = metrics.temperature ?? 0;

  return {
    ...decoded,
    heartRate: heartRate > 0 ? heartRate : decoded.heartRate,
    spo2: spO2 > 0 ? spO2 : decoded.spo2,
    temperature: temperature > 0 ? temperature : decoded.temperature,
    hrv: metrics.hrv !== undefined && metrics.hrv > 0 ? metrics.hrv : decoded.hrv,
    fatigue:
      metrics.fatigue !== undefined && metrics.fatigue > 0 ? metrics.fatigue : decoded.fatigue,
    systolicPressure: bp.systolic > 0 ? bp.systolic : decoded.systolicPressure,
    diastolicPressure: bp.diastolic > 0 ? bp.diastolic : decoded.diastolicPressure,
  };
}
