import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

export const commandTypeSchema = z.enum(["start", "stop", "config", "reset_config"]);

export const enqueueCommandBodySchema = z.object({
  type: commandTypeSchema,
  payload: z.record(z.unknown()).optional(),
});

export type EnqueueCommandBody = z.infer<typeof enqueueCommandBodySchema>;

export const commandAckBodySchema = z.object({
  status: z.enum(["completed", "failed"]),
  error: z.string().optional(),
  runtime: z
    .object({
      status: z.string(),
      error: z.string().optional(),
      readingActive: z.boolean().optional(),
      sendingData: z.boolean().optional(),
      wifiConnected: z.boolean().optional(),
      wifiSsid: z.string().optional(),
      ip: z.string().optional(),
      rssi: z.number().optional(),
      heapFree: z.number().optional(),
    })
    .optional(),
});

export type CommandAckBody = z.infer<typeof commandAckBodySchema>;

export const heartbeatBodySchema = z.object({
  status: z.string(),
  error: z.string().optional(),
  readingActive: z.boolean().optional(),
  sendingData: z.boolean().optional(),
  wifiConnected: z.boolean().optional(),
  wifiSsid: z.string().optional(),
  ip: z.string().optional(),
  rssi: z.number().optional(),
  heapFree: z.number().optional(),
});

export type HeartbeatBody = z.infer<typeof heartbeatBodySchema>;

export const configPayloadSchema = z
  .object({
    wifiSsid: z.string().min(1).optional(),
    wifiPass: z.string().optional(),
    apiUrl: z.string().url().optional(),
    deviceMac: z.string().regex(macAddressRegex).optional(),
    scanTimeoutMs: z.number().int().min(1).max(120_000).optional(),
    restartDelaySuccessMs: z.number().int().min(0).max(600_000).optional(),
    restartDelayErrorMs: z.number().int().min(0).max(600_000).optional(),
  })
  .optional();
