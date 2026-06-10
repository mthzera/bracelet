import { zodToJsonSchema } from "zod-to-json-schema";
import { packetBatchPayloadSchema } from "./packet.schema.js";
import { patientFieldSchema } from "./device.swagger.js";

function flattenZodJsonSchema(schema: typeof packetBatchPayloadSchema): Record<string, unknown> {
  const json = zodToJsonSchema(schema, {
    $refStrategy: "none",
    target: "openApi3",
  }) as Record<string, unknown>;

  if (json.definitions && typeof json.definitions === "object") {
    const first = Object.values(json.definitions as Record<string, unknown>)[0];
    if (first && typeof first === "object") {
      return first as Record<string, unknown>;
    }
  }

  const { $schema: _schema, definitions: _definitions, ...rest } = json;
  return rest;
}

/** Flat OpenAPI body schema (no $ref wrapper). */
export const packetBodySchema = flattenZodJsonSchema(packetBatchPayloadSchema);

const decodedSchema = {
  type: "object",
  additionalProperties: true,
  description: "Decoded packet (0x13 battery, 0x22 MAC, 0x28 health, 0x56 HRV history, 0x09 realtime)",
} as const;

const packetResultSchema = {
  type: "object",
  required: ["ok", "packetType", "receivedAtMs", "savedAt"],
  properties: {
    ok: { type: "boolean" },
    id: { type: "integer", description: "Database record ID" },
    packetType: { type: "string", example: "0x28" },
    receivedAtMs: { type: "integer", example: 123456 },
    bytes: {
      type: "array",
      items: { type: "integer", minimum: 0, maximum: 255 },
    },
    crcValid: { type: "boolean" },
    decoded: decodedSchema,
    mergedHealth: {
      ...decodedSchema,
      nullable: true,
      description: "Leitura 0x28/0x56 combinada do mesmo ciclo (últimos 5 min)",
    },
    error: { type: "string", description: "Present when ok is false" },
    savedAt: { type: "string", example: "2026-05-25T14:30:00.000Z" },
  },
};

const snapshotVitalsSchema = {
  type: "object",
  properties: {
    heartRate: { type: "number", example: 86 },
    spo2: { type: "number", example: 95 },
    temperature: { type: "number", example: 36.5 },
    hrv: { type: "number", example: 48 },
    fatigue: { type: "number", example: 109 },
    systolicPressure: { type: "number", example: 114 },
    diastolicPressure: { type: "number", example: 64 },
  },
} as const;

const measurementSnapshotSchema = {
  type: "object",
  required: ["id", "deviceMac", "source", "measuredAt", "vitals", "packetCount", "failedCount"],
  properties: {
    id: { type: "integer", description: "ID do último pacote do ciclo" },
    deviceMac: { type: "string" },
    source: { type: "string", example: "ESP32" },
    measuredAt: { type: "string", format: "date-time" },
    vitals: snapshotVitalsSchema,
    sleep: {
      type: "object",
      nullable: true,
      properties: {
        date: { type: "string" },
        time: { type: "string" },
        sleepMinutes: { type: "number" },
        recordId: { type: "number" },
      },
    },
    battery: { type: "number", nullable: true, example: 83 },
    firmware: { type: "string", nullable: true, example: "V0.0.3" },
    deviceMacReported: { type: "string", nullable: true },
    packetCount: { type: "integer", example: 15 },
    failedCount: { type: "integer", example: 0 },
    patient: patientFieldSchema,
  },
} as const;

export const packetBatchSuccessResponseSchema = {
  type: "object",
  required: ["deviceMac", "source", "snapshot", "stats"],
  properties: {
    deviceMac: { type: "string", example: "E6:64:0D:30:D3:F9" },
    source: { type: "string", example: "ESP32" },
    patient: patientFieldSchema,
    snapshot: { ...measurementSnapshotSchema, nullable: true },
    stats: {
      type: "object",
      properties: {
        total: { type: "integer" },
        ok: { type: "integer" },
        failed: { type: "integer" },
      },
    },
  },
};

const savedPacketSchema = {
  type: "object",
  properties: {
    id: { type: "integer" },
    deviceMac: { type: "string" },
    packetType: { type: "string" },
    rawHex: { type: "string" },
    source: { type: "string" },
    bytes: {
      type: "array",
      nullable: true,
      items: { type: "integer" },
    },
    crcValid: { type: "boolean" },
    decoded: { ...decodedSchema, nullable: true },
    mergedHealth: { ...decodedSchema, nullable: true },
    decodeError: { type: "string", nullable: true },
    createdAt: { type: "string" },
    patient: patientFieldSchema,
  },
};

export const listPacketsResponseSchema = {
  type: "object",
  required: ["view"],
  properties: {
    view: { type: "string", enum: ["snapshots", "raw"] },
    snapshots: {
      type: "array",
      items: measurementSnapshotSchema,
    },
    packets: {
      type: "array",
      items: savedPacketSchema,
    },
  },
};

export const validationErrorResponseSchema = {
  type: "object",
  properties: {
    error: { type: "string", example: "Validation failed" },
    details: { type: "object", additionalProperties: true },
  },
};

export const getPacketRouteSchema = {
  tags: ["bracelets"],
  summary: "List measurement cycles",
  description:
    "Default view=snapshots returns one consolidated row per ESP32 cycle (vitals + sleep + battery). Use view=raw for individual BLE packets.",
  querystring: {
    type: "object",
    properties: {
      view: {
        type: "string",
        enum: ["snapshots", "raw"],
        default: "snapshots",
      },
      limit: {
        type: "integer",
        minimum: 1,
        maximum: 200,
        default: 30,
        description: "Max snapshots (default) or raw packets when view=raw",
      },
      deviceMac: {
        type: "string",
        description: "Filter packets by bracelet MAC (case-insensitive)",
        example: "E6:64:0D:30:D3:F9",
      },
    },
  },
  response: {
    200: {
      description: "Measurement snapshots or raw packets",
      ...listPacketsResponseSchema,
    },
  },
};

export const postPacketRouteSchema = {
  tags: ["bracelets"],
  summary: "Receive and decode BLE packets (batch)",
  description:
    "Accepts a batch of raw hex packets from the ESP32 gateway for a single device. CRC is validated for 16-byte packets (sum of all bytes except last & 0xFF === last byte); 0x13 battery notifies are short packets without CRC. Decodes supported types (0x13, 0x22, 0x28, 0x56, 0x09) and persists each result. receivedAtMs is stored as created_at (Unix epoch milliseconds).",
  body: packetBodySchema,
  response: {
    200: {
      description: "Batch processed (each item reports ok or decode error)",
      ...packetBatchSuccessResponseSchema,
    },
    400: {
      description: "Validation error",
      ...validationErrorResponseSchema,
    },
  },
};
