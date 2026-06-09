import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

const packetTypeRegex = /^0x[0-9A-Fa-f]{1,2}$/i;

const rawHexRegex = /^([0-9A-Fa-f]{2}\s*)+$/;

const metricsSchema = z
  .object({
    mode: z.string().min(1),
    type: z.string().min(1),
    modo_solicitado: z.string().min(1),
    heartRate: z.number().int().nonnegative(),
    spO2: z.number().int().nonnegative(),
    hrv: z.number().int().nonnegative(),
    fatigue: z.number().int().nonnegative(),
    bloodPressure: z.string().min(1),
    temperature: z.number().nonnegative(),
  })
  .partial()
  .passthrough()
  .optional();

export const packetPayloadSchema = z
  .object({
    deviceMac: z
      .string()
      .regex(macAddressRegex, "deviceMac must be a valid MAC address (AA:BB:CC:DD:EE:FF)"),
    packetType: z
      .string()
      .regex(packetTypeRegex, 'packetType must be a hex literal (e.g. "0x28")'),
    rawHex: z
      .string()
      .min(1)
      .regex(rawHexRegex, "rawHex must be space-separated hex byte pairs"),
    source: z.string().min(1),
    metrics: metricsSchema,
  })
  .superRefine((data, ctx) => {
    if (data.packetType.toLowerCase() !== "0x28" || !data.metrics) return;

    const { heartRate, spO2, temperature } = data.metrics;
    if (heartRate === undefined || heartRate < 1) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "metrics.heartRate (bpm) is required and must be >= 1",
        path: ["metrics", "heartRate"],
      });
    }
    if (spO2 === undefined || spO2 < 1) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "metrics.spO2 (%) is required and must be >= 1",
        path: ["metrics", "spO2"],
      });
    }
    if (temperature === undefined || temperature <= 0) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "metrics.temperature (°C) is required and must be > 0",
        path: ["metrics", "temperature"],
      });
    }
  });

export type PacketPayload = z.infer<typeof packetPayloadSchema>;
