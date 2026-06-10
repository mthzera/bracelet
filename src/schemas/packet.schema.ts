import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

const packetTypeRegex = /^0x[0-9A-Fa-f]{1,2}$/i;

const rawHexRegex = /^([0-9A-Fa-f]{2}\s*)+$/;

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
  })
  .passthrough()
  .optional();

export type PacketMetrics = NonNullable<z.infer<typeof metricsSchema>>;

const packetItemSchema = z
  .object({
    packetType: z
      .string()
      .regex(packetTypeRegex, 'packetType must be a hex literal (e.g. "0x28")'),
    rawHex: z
      .string()
      .min(1)
      .regex(rawHexRegex, "rawHex must be space-separated hex byte pairs"),
    receivedAtMs: z.number().int().nonnegative(),
    metrics: metricsSchema,
  })
  .passthrough();

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
