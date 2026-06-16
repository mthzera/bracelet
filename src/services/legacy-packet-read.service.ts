import type { SavedPacket } from "../repositories/packet.repository.js";
import type { ResolvedPatient } from "./device-registry.service.js";
import type {
  DecodedHealth,
  DecodedPacket,
  DecodedSleep,
  DecodedSnapshot,
} from "./packet-decoder.service.js";
import { SNAPSHOT_VITALS_TYPE } from "./vitals-consolidation.service.js";

export type LegacyReadPacket = SavedPacket & {
  patient?: ResolvedPatient | null;
  mergedHealth?: DecodedHealth | null;
};

function snapshotDecoded(packet: SavedPacket): DecodedSnapshot | null {
  if (packet.packetType !== SNAPSHOT_VITALS_TYPE) return null;
  const decoded = packet.decoded;
  return decoded?.type === "snapshot" ? decoded : null;
}

function mergedHealthFromSnapshot(snapshot: DecodedSnapshot): DecodedHealth {
  return {
    type: "0x28",
    measurementType: 0,
    measurementMode: "unknown",
    heartRate: snapshot.heartRate ?? 0,
    spo2: snapshot.spo2 ?? 0,
    temperature: snapshot.temperature ?? 0,
    hrv: snapshot.hrv ?? 0,
    fatigue: snapshot.fatigue ?? 0,
    systolicPressure: snapshot.systolicPressure ?? 0,
    diastolicPressure: snapshot.diastolicPressure ?? 0,
  };
}

function syntheticSleepPacket(
  packet: LegacyReadPacket,
  snapshot: DecodedSnapshot,
): LegacyReadPacket | null {
  if (!snapshot.sleepMinutes || snapshot.sleepMinutes <= 0) return null;

  const decoded: DecodedSleep = {
    type: "0x53",
    recordId: snapshot.sleepRecordId ?? 0,
    date: snapshot.sleepDate ?? "",
    time: snapshot.sleepTime ?? "",
    sleepMinutes: snapshot.sleepMinutes,
  };

  return {
    ...packet,
    packetType: "0x53",
    rawHex: "",
    crcValid: false,
    decodeError: null,
    decoded,
    mergedHealth: null,
  };
}

/** Expõe SNAPSHOT_VITALS no formato legado esperado por dashboard e Power BI. */
export function expandPacketForLegacyRead(packet: LegacyReadPacket): LegacyReadPacket[] {
  const snapshot = snapshotDecoded(packet);
  if (!snapshot) return [packet];

  const enriched: LegacyReadPacket = {
    ...packet,
    mergedHealth: mergedHealthFromSnapshot(snapshot),
  };

  const sleepRow = syntheticSleepPacket(enriched, snapshot);
  return sleepRow ? [enriched, sleepRow] : [enriched];
}

export function expandPacketsForLegacyRead(packets: LegacyReadPacket[]): LegacyReadPacket[] {
  return packets.flatMap((packet) => expandPacketForLegacyRead(packet));
}
