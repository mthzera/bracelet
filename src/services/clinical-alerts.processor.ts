import {
  getRecentVitalsHistory,
  saveClinicalAssessment,
} from "../repositories/clinical-alerts.repository.js";
import { getRecentHealthPackets } from "../repositories/packet.repository.js";
import type { PacketMetrics } from "../schemas/packet.schema.js";
import {
  mergeHealthReadings,
  type DecodedHealth,
  type DecodedHrvHistory,
} from "../services/packet-decoder.service.js";
import {
  evaluateClinicalAlerts,
  vitalsFromDecodedHealth,
} from "../services/clinical-alerts.service.js";
import type { VitalsInput } from "../types/clinical-alerts.types.js";
import { hasMandatoryVitals } from "./vitals-validation.service.js";

export function mergeVitalsFromHealthPackets(
  decodedList: Array<DecodedHealth | DecodedHrvHistory>,
): VitalsInput {
  const merged = mergeHealthReadings(decodedList);
  return {
    heartRate: merged.heartRate,
    systolic: merged.systolicPressure,
    diastolic: merged.diastolicPressure,
    temperature: merged.temperature,
    spo2: merged.spo2,
    hrv: merged.hrv,
    fatigue: merged.fatigue,
  };
}

export function vitalsFromPacketMetrics(
  metrics: PacketMetrics,
): VitalsInput | null {
  const vitals: VitalsInput = {
    heartRate: metrics.bpm ?? 0,
    systolic: metrics.bloodPressureSystolic ?? 0,
    diastolic: metrics.bloodPressureDiastolic ?? 0,
    temperature: metrics.temperature ?? 0,
    spo2: metrics.spo2 ?? 0,
    hrv: metrics.hrv ?? 0,
    fatigue: metrics.fatigue ?? 0,
  };

  return hasMandatoryVitals({
    heartRate: vitals.heartRate,
    spo2: vitals.spo2,
    temperature: vitals.temperature,
  })
    ? vitals
    : null;
}

/** Backend-only: roda após ingestão de pacote de saúde (0x28 / 0x56 / metrics). */
export async function processClinicalAlertsAfterHealthPacket(input: {
  deviceMac: string;
  source: string;
  packetId: number;
  measuredAt: string;
  vitals: VitalsInput;
}): Promise<void> {
  if (!hasMandatoryVitals(input.vitals)) return;

  const history = await getRecentVitalsHistory(input.deviceMac, 10);

  const assessment = evaluateClinicalAlerts({
    deviceMac: input.deviceMac,
    measuredAt: input.measuredAt,
    vitals: input.vitals,
    context: { isResting: true, signalQuality: "unknown", source: input.source },
    recentHistory: history,
  });

  await saveClinicalAssessment(assessment, input.packetId);
}

export async function processClinicalAlertsFromRecentPackets(
  deviceMac: string,
  source: string,
  packetId: number,
  measuredAt: string,
): Promise<void> {
  const recent = await getRecentHealthPackets(deviceMac, 12);
  const healthDecoded = recent
    .map((p) => p.decoded)
    .filter((d): d is DecodedHealth | DecodedHrvHistory => d?.type === "0x28" || d?.type === "0x56");

  if (healthDecoded.length === 0) return;

  const vitals = mergeVitalsFromHealthPackets(healthDecoded);
  await processClinicalAlertsAfterHealthPacket({
    deviceMac,
    source,
    packetId,
    measuredAt,
    vitals,
  });
}

export async function processClinicalAlertsFromDecoded(
  deviceMac: string,
  source: string,
  packetId: number,
  measuredAt: string,
  decoded: DecodedHealth | DecodedHrvHistory,
): Promise<void> {
  if (decoded.type === "0x56") {
    const vitals: VitalsInput = {
      heartRate: 0,
      systolic: 0,
      diastolic: 0,
      temperature: 0,
      spo2: 0,
      hrv: decoded.hrv,
      fatigue: decoded.fatigue,
    };
    await processClinicalAlertsAfterHealthPacket({ deviceMac, source, packetId, measuredAt, vitals });
    return;
  }

  await processClinicalAlertsFromRecentPackets(deviceMac, source, packetId, measuredAt);
}

export function vitalsFromMetricsOrDecoded(
  metrics: PacketMetrics | undefined,
  decoded?: DecodedHealth,
): VitalsInput | null {
  if (metrics) {
    return vitalsFromPacketMetrics(metrics);
  }
  if (decoded?.type === "0x28") {
    return vitalsFromDecodedHealth(decoded);
  }
  return null;
}
