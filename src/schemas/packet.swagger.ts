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

const consolidatedVitalsSchema = {
  type: "object",
  description: "Leitura consolidada. Vitais ausentes/inválidos = null (nunca 0).",
  properties: {
    heartRate: { type: "number", nullable: true, example: 83 },
    spo2: { type: "number", nullable: true, example: 97 },
    temperature: { type: "number", nullable: true, example: 36.6 },
    hrv: { type: "number", nullable: true, example: 75 },
    fatigue: { type: "number", nullable: true, example: 41 },
    systolicPressure: { type: "number", nullable: true, example: 123 },
    diastolicPressure: { type: "number", nullable: true, example: 78 },
  },
} as const;

const consolidatedSourcesSchema = {
  type: "object",
  description: "Origem efetiva de cada vital na leitura consolidada (rule 9).",
  properties: {
    heartRate: { type: "string", nullable: true, example: "0x54_HISTORY" },
    spo2: { type: "string", nullable: true, example: "0x66_HISTORY_AUTO" },
    temperature: { type: "string", nullable: true, example: "0x65_HISTORY_AUTO" },
    hrv: { type: "string", nullable: true, example: "0x56_HISTORY" },
    fatigue: { type: "string", nullable: true, example: "0x56_HISTORY" },
    bloodPressure: { type: "string", nullable: true, example: "0x28_REALTIME" },
  },
} as const;

const consolidatedSnapshotSchema = {
  type: "object",
  required: ["deviceMac", "source", "measuredAt", "vitals", "sources", "quality"],
  properties: {
    deviceMac: { type: "string" },
    source: { type: "string", example: "ESP32" },
    measuredAt: { type: "string", format: "date-time" },
    vitals: consolidatedVitalsSchema,
    sources: consolidatedSourcesSchema,
    quality: {
      type: "object",
      properties: {
        snapshotComplete: { type: "boolean" },
        bloodPressure: { type: "string", enum: ["estimated", "absent"] },
      },
    },
    patient: patientFieldSchema,
  },
} as const;

export const packetBatchSuccessResponseSchema = {
  type: "object",
  required: ["deviceMac", "source", "snapshot", "stats"],
  properties: {
    deviceMac: { type: "string", example: "E6:64:0D:30:D3:F9" },
    source: { type: "string", example: "ESP32" },
    ingestionBatchId: { type: "string", description: "Cycle id applied to every packet in this batch" },
    patient: patientFieldSchema,
    snapshot: { ...consolidatedSnapshotSchema, nullable: true },
    usedSnapshotVitals: {
      type: "boolean",
      description: "true quando a leitura principal veio de um pacote SNAPSHOT_VITALS",
    },
    ignoredPartialSnapshots: {
      type: "integer",
      description: "Qtd de raws de saúde que NÃO viraram leitura principal (havia SNAPSHOT_VITALS)",
    },
    stats: {
      type: "object",
      properties: {
        total: { type: "integer", description: "Pacotes processados no body" },
        ok: { type: "integer" },
        failed: { type: "integer" },
        rawSaved: { type: "integer", description: "Raws persistidos" },
        invalidIgnored: { type: "integer", description: "Leituras fora de faixa ignoradas" },
        snapshotComplete: { type: "boolean" },
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

const cycleSummaryItemSchema = {
  type: "object",
  required: ["cycleId", "deviceMac", "source", "startedAt", "endedAt", "summary", "packetTypes", "packetsCount", "crc"],
  properties: {
    cycleId: { type: "string", description: "ingestionBatchId, ou win-<minId>-<maxId> p/ registros antigos" },
    deviceMac: { type: "string", example: "ef:7a:0d:30:b3:fa" },
    patient: patientFieldSchema,
    source: { type: "string", example: "ESP32" },
    startedAt: { type: "string", format: "date-time" },
    endedAt: { type: "string", format: "date-time" },
    summary: {
      type: "object",
      description: "Resumo clínico do ciclo = média dos valores válidos da janela. Campos null = ausente (o front mostra \"--\", nunca 0).",
      properties: {
        heartRate: { type: "number", nullable: true, example: 76, description: "Média dos BPM válidos da janela" },
        heartRateMin: { type: "number", nullable: true, example: 63 },
        heartRateMax: { type: "number", nullable: true, example: 91 },
        temperature: { type: "number", nullable: true, example: 35.6 },
        hrv: { type: "number", nullable: true, example: 88 },
        fatigue: { type: "number", nullable: true, example: 63 },
        bloodPressure: {
          type: "object",
          properties: {
            systolic: { type: "number", nullable: true, example: 127 },
            diastolic: { type: "number", nullable: true, example: 77 },
            measuredAt: { type: "string", format: "date-time", nullable: true, description: "Horário da leitura de PA mais recente da janela" },
          },
        },
        spo2: { type: "number", nullable: true, example: null, description: "Opcional; null quando ausente" },
        battery: { type: "number", nullable: true, example: 83 },
        sleepMinutes: { type: "number", nullable: true, example: 140 },
        firmware: { type: "string", nullable: true, example: "V0.0.3" },
      },
    },
    sources: {
      type: "object",
      description: "Quais famílias de pacote alimentaram o ciclo",
      properties: {
        activePackets: { type: "boolean" },
        heartRateHistory: { type: "boolean" },
        hrvFatigueHistory: { type: "boolean" },
        temperatureHistory: { type: "boolean" },
      },
    },
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
    deviceMacReported: { type: "string", nullable: true },
    packetTypes: {
      type: "array",
      items: { type: "string" },
      example: ["0x28", "0x53", "0x54", "0x55", "0x56", "0x60", "0x62", "0x65", "0x66"],
    },
    packetsCount: { type: "integer", example: 20 },
    crc: {
      type: "object",
      properties: {
        valid: { type: "integer", example: 10 },
        invalid: { type: "integer", example: 2 },
        notApplicable: { type: "integer", example: 8 },
      },
    },
    complete: { type: "boolean" },
    missing: { type: "array", items: { type: "string" } },
    rawPackets: {
      type: "array",
      items: {
        type: "object",
        properties: {
          id: { type: "integer" },
          packetType: { type: "string" },
          rawHex: { type: "string" },
          crcValid: { type: "boolean" },
          crcApplicable: { type: "boolean" },
          decodeError: { type: "string", nullable: true },
          decoded: { ...decodedSchema, nullable: true },
          receivedAt: { type: "string", format: "date-time" },
        },
      },
    },
  },
} as const;

export const consolidatedCyclesResponseSchema = {
  type: "object",
  required: ["items"],
  properties: {
    items: { type: "array", items: cycleSummaryItemSchema },
  },
};

export const getPacketSummaryRouteSchema = {
  tags: ["bracelets"],
  summary: "List consolidated measurement cycles",
  description:
    "Retorna 1 registro lógico por ciclo de coleta, agregando todos os pacotes crus relacionados (vitais, sono, bateria, firmware). Agrupa por ingestionBatchId quando presente; registros antigos sem batch id são agrupados por janela de tempo (~3 min). Inclui rawPackets para depuração.",
  querystring: {
    type: "object",
    properties: {
      limit: { type: "integer", minimum: 1, maximum: 200, default: 30 },
      deviceMac: {
        type: "string",
        description: "Filter by bracelet MAC (case-insensitive)",
        example: "ef:7a:0d:30:b3:fa",
      },
    },
  },
  response: {
    200: {
      description: "Consolidated cycles",
      ...consolidatedCyclesResponseSchema,
    },
  },
};

const listPacketsResponseSchema = {
  type: "object",
  required: ["view"],
  properties: {
    view: { type: "string", enum: ["snapshots", "raw", "consolidated"] },
    snapshots: {
      type: "array",
      items: measurementSnapshotSchema,
    },
    items: {
      type: "array",
      items: cycleSummaryItemSchema,
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
      group: {
        type: "boolean",
        default: false,
        description: "Quando true, devolve ciclos consolidados ({ items }) — igual a /bracelets/packets/summary",
      },
      limit: {
        type: "integer",
        minimum: 1,
        maximum: 200,
        default: 30,
        description: "Max snapshots/cycles (default) or raw packets when view=raw",
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
