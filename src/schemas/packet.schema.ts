import { z } from "zod";
import { normalizePacketType } from "../services/packet-decoder.service.js";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

// Aceita prefixo "0x" ou "0X" (normalizado depois via transform) OU o snapshot
// consolidado gerado pelo ESP32 ("SNAPSHOT_VITALS"), que vem só com `metrics`.
const packetTypeRegex = /^(0[xX][0-9A-Fa-f]{1,2}|SNAPSHOT_VITALS)$/;

const rawHexRegex = /^([0-9A-Fa-f]{2}\s*)+$/;

/** SNAPSHOT_VITALS não tem rawHex; carrega a leitura consolidada em `metrics`. */
function isSnapshotVitalsType(packetType: string): boolean {
  return packetType.trim() === "SNAPSHOT_VITALS";
}

/**
 * Métricas opcionais já interpretadas pelo ESP32 (espelham o rawHex do 0x28).
 * São SEMPRE opcionais: históricos (0x54/0x56/0x60/0x62/0x65/0x66) vêm sem metrics.
 * A API decodifica o rawHex de qualquer jeito; metrics só complementa/preenche lacunas.
 * `passthrough` mantém campos extras e tolera firmwares com nomes legados.
 */
export const metricsSchema = z
  .object({
    measurementMode: z.number().int().optional(),
    bpm: z.number().int().optional(),
    spo2: z.number().int().optional(),
    temperature: z.number().optional(),
    hrv: z.number().int().optional(),
    fatigue: z.number().int().optional(),
    bloodPressureSystolic: z.number().int().optional(),
    bloodPressureDiastolic: z.number().int().optional(),
    battery: z.number().int().min(0).max(100).optional(),
    collectionDurationMs: z.number().int().nonnegative().optional(),
    measurementTimestampsMs: z.record(z.string(), z.number().int().nonnegative().nullable()).optional(),
    sleepMinutes: z.number().int().optional(),
    sleepDate: z.string().optional(),
    sleepTime: z.string().optional(),
    sleepRecordId: z.number().int().optional(),
    testMode: z.boolean().optional(),
  })
  .passthrough()
  .optional();

export type PacketMetrics = NonNullable<z.infer<typeof metricsSchema>>;

const packetItemSchema = z
  .object({
    packetType: z
      .string()
      .regex(packetTypeRegex, 'packetType must be a hex literal (e.g. "0x28") or "SNAPSHOT_VITALS"')
      .transform(normalizePacketType),
    // Opcional só para SNAPSHOT_VITALS (que vem em `metrics`); obrigatório p/ raws.
    rawHex: z
      .string()
      .regex(rawHexRegex, "rawHex must be space-separated hex byte pairs")
      .optional(),
    receivedAtMs: z.number().int().nonnegative(),
    metrics: metricsSchema,
  })
  .passthrough()
  .superRefine((item, ctx) => {
    if (isSnapshotVitalsType(item.packetType)) {
      if (!item.metrics) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          path: ["metrics"],
          message: "SNAPSHOT_VITALS requires a metrics object",
        });
      }
      return;
    }
    if (!item.rawHex || item.rawHex.length === 0) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ["rawHex"],
        message: "rawHex is required for raw packets",
      });
    }
  });

export const packetBatchPayloadSchema = z
  .object({
    deviceMac: z
      .string()
      .regex(macAddressRegex, "deviceMac must be a valid MAC address (AA:BB:CC:DD:EE:FF)"),
    source: z.string().min(1).optional().default("ESP32"),
    /**
     * Identificador do ciclo de coleta. Opcional: se ausente, a API gera um no POST
     * e o aplica a todos os pacotes do mesmo batch. Aceita ingestionBatchId ou cycleId.
     */
    ingestionBatchId: z.string().min(1).max(128).optional(),
    cycleId: z.string().min(1).max(128).optional(),
    packets: z.array(packetItemSchema).min(1).max(50),
  })
  .passthrough();

export type PacketBatchPayload = z.infer<typeof packetBatchPayloadSchema>;
export type PacketItem = z.infer<typeof packetItemSchema>;

/** Single packet fields used when persisting to the database. */
export type PacketPayload = {
  deviceMac: string;
  source: string;
  packetType: string;
  rawHex: string;
  metrics?: PacketMetrics;
};
