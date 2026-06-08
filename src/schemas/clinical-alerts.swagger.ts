import { zodToJsonSchema } from "zod-to-json-schema";
import { clinicalAssessmentRequestSchema } from "./clinical-alerts.schema.js";

function flattenZodJsonSchema(schema: typeof clinicalAssessmentRequestSchema): Record<string, unknown> {
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

export const clinicalAssessmentBodySchema = flattenZodJsonSchema(clinicalAssessmentRequestSchema);

const alertItemSchema = {
  type: "object",
  properties: {
    type: { type: "string", example: "PA_ELEVADA" },
    severity: { type: "string", enum: ["LOW", "MEDIUM", "HIGH", "CRITICAL"] },
    message: { type: "string" },
    reason: { type: "string" },
  },
} as const;

export const clinicalAssessmentResponseSchema = {
  type: "object",
  required: [
    "id",
    "deviceMac",
    "measuredAt",
    "vitals",
    "context",
    "alerts",
    "notes",
    "riskScore",
    "overallStatus",
    "severity",
    "baseline",
    "news2",
    "disclaimer",
    "createdAt",
  ],
  properties: {
    id: { type: "integer" },
    packetId: { type: "integer", nullable: true },
    deviceMac: { type: "string", example: "E6:64:0D:30:D3:F9" },
    measuredAt: { type: "string", example: "2026-06-03T13:56:00.000Z" },
    vitals: {
      type: "object",
      properties: {
        heartRate: { type: "integer", example: 71 },
        systolic: { type: "integer", example: 129 },
        diastolic: { type: "integer", example: 79 },
        temperature: { type: "number", example: 36.5 },
        spo2: { type: "integer", example: 98 },
        hrv: { type: "integer", example: 88 },
        fatigue: { type: "integer", example: 34 },
      },
    },
    context: {
      type: "object",
      properties: {
        isResting: { type: "boolean", example: true },
        signalQuality: { type: "string", example: "unknown" },
        source: { type: "string", example: "2208A" },
      },
    },
    alerts: { type: "array", items: alertItemSchema },
    notes: { type: "array", items: { type: "string" } },
    riskScore: { type: "integer", example: 3, description: "Pontuação total NEWS 2 parcial" },
    overallStatus: {
      type: "string",
      enum: ["STABLE", "ATTENTION", "ALERT", "CRITICAL"],
      example: "STABLE",
    },
    severity: { type: "string", enum: ["LOW", "MEDIUM", "HIGH", "CRITICAL"], example: "LOW" },
    baseline: { type: "object", additionalProperties: true },
    news2: {
      type: "object",
      properties: {
        totalScore: { type: "integer", example: 3 },
        maxPossibleScore: { type: "integer", example: 12 },
        responseLevel: {
          type: "string",
          enum: ["routine", "low", "medium", "urgent", "emergency"],
          example: "medium",
        },
        components: {
          type: "array",
          items: {
            type: "object",
            properties: {
              parameter: { type: "string" },
              label: { type: "string" },
              score: { type: "integer", minimum: 0, maximum: 3 },
              value: { type: "number" },
              unit: { type: "string" },
            },
          },
        },
        unavailableParameters: { type: "array", items: { type: "string" } },
      },
    },
    disclaimer: { type: "string" },
    createdAt: { type: "string" },
  },
};

export const listClinicalAssessmentsResponseSchema = {
  type: "object",
  required: ["assessments"],
  properties: {
    assessments: {
      type: "array",
      items: clinicalAssessmentResponseSchema,
    },
  },
};

export const clinicalAlertsCatalogResponseSchema = {
  type: "object",
  required: ["version", "disclaimer", "alertTypes", "parameters"],
  properties: {
    version: { type: "string", example: "2.0.0" },
    disclaimer: { type: "string" },
    alertTypes: {
      type: "array",
      items: {
        type: "object",
        properties: {
          type: { type: "string" },
          label: { type: "string" },
          category: { type: "string" },
          severities: { type: "array", items: { type: "string" } },
          description: { type: "string" },
        },
      },
    },
    parameters: { type: "object", additionalProperties: true },
  },
} as const;

export const getClinicalAlertsCatalogRouteSchema = {
  tags: ["clinical-alerts"],
  summary: "Catalog of alert types and rule parameters",
  description: "Metadados estáticos para labels, filtros e tooltips no frontend.",
  response: {
    200: {
      description: "Alert catalog",
      ...clinicalAlertsCatalogResponseSchema,
    },
  },
};

export const getLatestClinicalAssessmentRouteSchema = {
  tags: ["clinical-alerts"],
  summary: "Latest clinical assessment for a device",
  description:
    "Retorna a avaliação mais recente gerada automaticamente pelo backend após pacotes de saúde (0x28/0x56). Uso principal do frontend.",
  querystring: {
    type: "object",
    required: ["deviceMac"],
    properties: {
      deviceMac: {
        type: "string",
        description: "MAC da pulseira (AA:BB:CC:DD:EE:FF)",
        example: "E6:64:0D:30:D3:F9",
      },
    },
  },
  response: {
    200: {
      description: "Latest assessment",
      ...clinicalAssessmentResponseSchema,
    },
    400: {
      description: "Missing deviceMac",
      type: "object",
      properties: {
        error: { type: "string" },
      },
    },
    404: {
      description: "No assessment yet",
      type: "object",
      properties: {
        error: { type: "string" },
        deviceMac: { type: "string" },
      },
    },
  },
};

export const getClinicalAssessmentsRouteSchema = {
  tags: ["clinical-alerts"],
  summary: "List saved clinical assessments",
  description:
    "Histórico de avaliações geradas pelo backend. O frontend só consulta (GET); análise ocorre ao receber pacotes em POST /bracelets/packets.",
  querystring: {
    type: "object",
    properties: {
      deviceMac: {
        type: "string",
        description: "Filter by MAC (AA:BB:CC:DD:EE:FF)",
      },
      limit: {
        type: "integer",
        minimum: 1,
        maximum: 200,
        default: 50,
      },
    },
  },
  response: {
    200: {
      description: "List of assessments",
      ...listClinicalAssessmentsResponseSchema,
    },
  },
};
