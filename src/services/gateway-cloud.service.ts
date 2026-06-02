import {
  enqueueCommand,
  getRuntimeStatus,
  type DeviceRuntimeStatus,
} from "../repositories/device-command.repository.js";
import type { GatewayConfigBody } from "../schemas/gateway.schema.js";
import { normalizeDeviceId } from "../utils/device-id.js";

export function resolveDeviceMac(
  queryMac?: string,
  bodyMac?: string,
): string {
  const raw = (bodyMac ?? queryMac ?? "").trim();

  if (!raw) {
    throw new Error("DEVICE_MAC_REQUIRED");
  }

  try {
    return normalizeDeviceId(raw);
  } catch {
    throw new Error("INVALID_DEVICE_ID");
  }
}

export function runtimeToGatewayStatus(
  deviceMac: string,
  runtime: DeviceRuntimeStatus | null,
) {
  const base = {
    status: runtime?.status ?? "unknown",
    error: runtime?.error ?? "Aguardando primeiro contato do ESP32 na API",
    readingActive: runtime?.readingActive ?? false,
    sendingData: runtime?.sendingData ?? false,
    bleConnected: false,
    wifiConnected: runtime?.wifiConnected ?? false,
    wifiSsid: runtime?.wifiSsid ?? "",
    ip: runtime?.ip ?? "",
    rssi: runtime?.rssi ?? 0,
    apiUrl: "",
    deviceMac,
    scanTimeoutMs: 15000,
    heapFree: runtime?.heapFree ?? 0,
    batteryCaptured: false,
    batteryRawHex: "",
    readPhase: 0,
    hrvCaptured: false,
    heartCaptured: false,
    spo2Captured: false,
    temperatureCaptured: false,
    bloodPressureCaptured: false,
    hrvRawHex: "",
    heartRawHex: "",
    spo2RawHex: "",
    temperatureRawHex: "",
    bloodPressureRawHex: "",
    lastSeenAt: runtime?.lastSeenAt ?? null,
    mode: "cloud",
  };

  return base;
}

export async function cloudGetGatewayStatus(deviceMac: string) {
  const runtime = await getRuntimeStatus(deviceMac);
  return runtimeToGatewayStatus(deviceMac, runtime);
}

export async function cloudGetGatewayConfig(deviceMac: string) {
  const runtime = await getRuntimeStatus(deviceMac);

  return {
    wifiSsid: runtime?.wifiSsid ?? "",
    apiUrl: "",
    deviceMac,
    scanTimeoutMs: 15000,
    ip: runtime?.ip ?? "",
    wifiConnected: runtime?.wifiConnected ?? false,
    mode: "cloud",
  };
}

export async function cloudEnqueueConfig(deviceMac: string, body: GatewayConfigBody) {
  const payload: Record<string, unknown> = { ...body };
  delete payload.deviceMac;

  const command = await enqueueCommand(deviceMac, "config", payload);

  return {
    ok: true,
    message: "Configuração enfileirada. O ESP32 aplicará no próximo polling (~5s).",
    commandId: command.id,
    mode: "cloud",
  };
}

export async function cloudEnqueueAction(
  deviceMac: string,
  type: "start" | "stop" | "reset_config",
) {
  const command = await enqueueCommand(deviceMac, type);

  const messages: Record<string, string> = {
    start: "Leitura enfileirada. O ESP32 iniciará no próximo polling (~5s).",
    stop: "Parada enfileirada.",
    reset_config: "Reset enfileirado. O ESP32 limpará a configuração no próximo polling.",
  };

  return {
    ok: true,
    message: messages[type],
    commandId: command.id,
    mode: "cloud",
  };
}
