import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

export const gatewayConfigBodySchema = z
  .object({
    wifiSsid: z.string().min(1).optional(),
    wifiPass: z.string().optional(),
    apiUrl: z.string().url().optional(),
    deviceMac: z
      .string()
      .regex(macAddressRegex, "deviceMac must be AA:BB:CC:DD:EE:FF")
      .optional(),
    scanTimeoutMs: z.number().int().positive().max(120_000).optional(),
    restartDelaySuccessMs: z.number().int().nonnegative().max(600_000).optional(),
    restartDelayErrorMs: z.number().int().nonnegative().max(600_000).optional(),
  })
  .refine((data) => Object.values(data).some((v) => v !== undefined), {
    message: "At least one configuration field is required",
  });

export type GatewayConfigBody = z.infer<typeof gatewayConfigBodySchema>;

export const gatewayUrlQuerySchema = z.object({
  gatewayUrl: z
    .string()
    .url()
    .optional()
    .describe("Override ESP32_GATEWAY_URL for this request (e.g. http://192.168.0.42)"),
});

export type GatewayUrlQuery = z.infer<typeof gatewayUrlQuerySchema>;

/** Mirrors ESP32 GET /status JSON (all fields optional for forward compatibility). */
export const gatewayStatusSchema = z
  .object({
    status: z.string(),
    error: z.string(),
    readingActive: z.boolean(),
    sendingData: z.boolean(),
    bleConnected: z.boolean(),
    wifiConnected: z.boolean(),
    wifiSsid: z.string(),
    ip: z.string(),
    rssi: z.number(),
    apiUrl: z.string(),
    deviceMac: z.string(),
    scanTimeoutMs: z.number(),
    heapFree: z.number(),
    batteryCaptured: z.boolean(),
    batteryRawHex: z.string(),
    readPhase: z.number(),
    hrvCaptured: z.boolean(),
    heartCaptured: z.boolean(),
    spo2Captured: z.boolean(),
    temperatureCaptured: z.boolean(),
    bloodPressureCaptured: z.boolean(),
    hrvRawHex: z.string(),
    heartRawHex: z.string(),
    spo2RawHex: z.string(),
    temperatureRawHex: z.string(),
    bloodPressureRawHex: z.string(),
  })
  .partial()
  .passthrough();

export type GatewayStatus = z.infer<typeof gatewayStatusSchema>;

export const gatewayActionResponseSchema = z.object({
  ok: z.boolean(),
  message: z.string().optional(),
  error: z.string().optional(),
});

export type GatewayActionResponse = z.infer<typeof gatewayActionResponseSchema>;
