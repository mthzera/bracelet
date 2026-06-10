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

/**
 * O rawHex decodificado é a fonte de verdade; `metrics` (opcional) só preenche
 * o que o decode não conseguiu (ex.: pacote truncado). Por isso o decoded tem
 * preferência e o metrics entra como fallback quando o campo veio 0/ausente.
 */
export function enrichDecodedHealthFromMetrics(
  decoded: DecodedHealth,
  metrics: PacketMetrics,
): DecodedHealth {
  const fallback = (decodedValue: number, metricValue: number | undefined): number => {
    if (decodedValue > 0) return decodedValue;
    return metricValue !== undefined && metricValue > 0 ? metricValue : decodedValue;
  };

  return {
    ...decoded,
    heartRate: fallback(decoded.heartRate, metrics.bpm),
    spo2: fallback(decoded.spo2, metrics.spo2),
    temperature: fallback(decoded.temperature, metrics.temperature),
    hrv: fallback(decoded.hrv, metrics.hrv),
    fatigue: fallback(decoded.fatigue, metrics.fatigue),
    systolicPressure: fallback(decoded.systolicPressure, metrics.bloodPressureSystolic),
    diastolicPressure: fallback(decoded.diastolicPressure, metrics.bloodPressureDiastolic),
  };
}
