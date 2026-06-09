import { TEST_BRACELETS, type PatientInfo, type TestBracelet } from "../config/test-bracelets.js";
import {
  getLatestPacketForDevice,
  getMergedHealthForDevice,
  type SavedPacket,
} from "../repositories/packet.repository.js";
import type { DecodedHealth } from "./packet-decoder.service.js";

const ONLINE_WINDOW_MS = 15 * 60 * 1000;

export type ResolvedPatient = PatientInfo & {
  deviceMac: string;
  label: string;
};

export type DeviceOverview = {
  deviceMac: string;
  label: string;
  patient: PatientInfo;
  online: boolean;
  lastSeenAt: string | null;
  battery: number | null;
  mergedHealth: DecodedHealth | null;
};

export function normalizeMac(mac: string): string {
  return mac.trim().toUpperCase();
}

export function listRegisteredBracelets(): TestBracelet[] {
  return TEST_BRACELETS;
}

export function resolvePatientForMac(mac: string): ResolvedPatient | null {
  const key = normalizeMac(mac);
  const bracelet = TEST_BRACELETS.find((b) => normalizeMac(b.deviceMac) === key);
  if (!bracelet) return null;

  return {
    deviceMac: bracelet.deviceMac,
    label: bracelet.label,
    ...bracelet.patient,
  };
}

export function enrichWithPatient<T extends { deviceMac: string }>(
  item: T,
): T & { patient: ResolvedPatient | null } {
  return {
    ...item,
    patient: resolvePatientForMac(item.deviceMac),
  };
}

function batteryFromPacket(packet: SavedPacket | null): number | null {
  if (!packet?.decoded || packet.decoded.type !== "0x13") return null;
  return packet.decoded.battery;
}

export async function buildDeviceOverview(bracelet: TestBracelet): Promise<DeviceOverview> {
  const [mergedHealth, batteryPacket, lastPacket] = await Promise.all([
    getMergedHealthForDevice(bracelet.deviceMac),
    getLatestPacketForDevice(bracelet.deviceMac, "0x13"),
    getLatestPacketForDevice(bracelet.deviceMac),
  ]);

  const lastSeenAt = lastPacket?.createdAt ?? null;
  const online =
    lastSeenAt !== null &&
    Date.now() - new Date(lastSeenAt).getTime() <= ONLINE_WINDOW_MS;

  return {
    deviceMac: bracelet.deviceMac,
    label: bracelet.label,
    patient: bracelet.patient,
    online,
    lastSeenAt,
    battery: batteryFromPacket(batteryPacket),
    mergedHealth,
  };
}

export async function buildDevicesOverview(): Promise<DeviceOverview[]> {
  return Promise.all(TEST_BRACELETS.map((bracelet) => buildDeviceOverview(bracelet)));
}
