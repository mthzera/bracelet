import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

export const vitalsInputSchema = z.object({
  heartRate: z.number().int().nonnegative(),
  systolic: z.number().int().nonnegative(),
  diastolic: z.number().int().nonnegative(),
  temperature: z.number().nonnegative(),
  spo2: z.number().int().min(0).max(100),
  hrv: z.number().int().nonnegative(),
  fatigue: z.number().int().min(0).max(100),
});

export const vitalsContextSchema = z
  .object({
    isResting: z.boolean().optional(),
    signalQuality: z.enum(["good", "fair", "poor", "unknown"]).optional(),
    source: z.string().min(1).optional(),
  })
  .optional();

export const clinicalAssessmentRequestSchema = z.object({
  deviceMac: z
    .string()
    .regex(macAddressRegex, "deviceMac must be a valid MAC address (AA:BB:CC:DD:EE:FF)"),
  measuredAt: z.string().datetime({ offset: true }),
  vitals: vitalsInputSchema,
  context: vitalsContextSchema,
  packetId: z.number().int().positive().optional(),
});

export type ClinicalAssessmentRequest = z.infer<typeof clinicalAssessmentRequestSchema>;
