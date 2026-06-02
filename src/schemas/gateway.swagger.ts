import { z, type ZodTypeAny } from "zod";
import { zodToJsonSchema } from "zod-to-json-schema";
import {
  gatewayActionResponseSchema,
  gatewayConfigBodySchema,
  gatewayStatusSchema,
} from "./gateway.schema.js";

function flattenZodJsonSchema(schema: ZodTypeAny): Record<string, unknown> {
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

export const gatewayConfigBodyOpenApi = flattenZodJsonSchema(gatewayConfigBodySchema);

const gatewayUrlQueryOpenApi = {
  type: "object",
  properties: {
    gatewayUrl: {
      type: "string",
      format: "uri",
      description: "Override ESP32_GATEWAY_URL (ESP32 must be reachable from this API)",
    },
  },
} as const;

const gatewayInfoResponseSchema = {
  type: "object",
  required: ["name", "configured", "endpoints"],
  properties: {
    name: { type: "string", example: "Bracelet API — ESP32 Gateway Proxy" },
    configured: { type: "boolean" },
    gatewayUrl: {
      type: "string",
      nullable: true,
      description: "ESP32_GATEWAY_URL from environment (passwords never exposed)",
      example: "http://192.168.0.42",
    },
    endpoints: {
      type: "object",
      additionalProperties: { type: "string" },
    },
  },
} as const;

const gatewayErrorSchema = {
  type: "object",
  properties: {
    error: { type: "string" },
    details: { type: "string" },
  },
} as const;

const gatewayTag = { tags: ["gateway"] } as const;

export const getGatewayInfoRouteSchema = {
  ...gatewayTag,
  summary: "Gateway API discovery",
  description: "Lists proxy endpoints and whether ESP32_GATEWAY_URL is configured.",
  response: {
    200: {
      description: "Gateway proxy metadata",
      ...gatewayInfoResponseSchema,
    },
  },
};

export const getGatewayStatusRouteSchema = {
  ...gatewayTag,
  summary: "ESP32 status",
  description: "Proxies GET /status on the ESP32 (reading state, Wi-Fi, captures, raw hex).",
  querystring: gatewayUrlQueryOpenApi,
  response: {
    200: {
      description: "ESP32 status JSON",
      ...flattenZodJsonSchema(gatewayStatusSchema),
    },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};

export const getGatewayConfigRouteSchema = {
  ...gatewayTag,
  summary: "ESP32 current configuration",
  description:
    "Returns non-secret config derived from ESP32 /status (wifiSsid, apiUrl, deviceMac, scanTimeoutMs).",
  querystring: gatewayUrlQueryOpenApi,
  response: {
    200: {
      description: "Current gateway configuration",
      type: "object",
      properties: {
        wifiSsid: { type: "string" },
        apiUrl: { type: "string" },
        deviceMac: { type: "string" },
        scanTimeoutMs: { type: "number" },
        ip: { type: "string" },
        wifiConnected: { type: "boolean" },
      },
    },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};

export const postGatewayConfigRouteSchema = {
  ...gatewayTag,
  summary: "Update ESP32 configuration",
  description:
    "Proxies POST /config on the ESP32. Changing wifiSsid triggers an ESP32 reboot. wifiPass is write-only.",
  querystring: gatewayUrlQueryOpenApi,
  body: gatewayConfigBodyOpenApi,
  response: {
    200: {
      description: "Configuration saved on ESP32",
      ...flattenZodJsonSchema(gatewayActionResponseSchema),
    },
    400: { description: "Validation error", ...gatewayErrorSchema },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};

export const postGatewayStartRouteSchema = {
  ...gatewayTag,
  summary: "Start BLE reading cycle",
  description:
    "Proxies POST /start — battery (0x13) then HRV, heart, SpO2, temperature, blood pressure (0x28), then POSTs to Bracelet API.",
  querystring: gatewayUrlQueryOpenApi,
  response: {
    200: {
      description: "Reading started",
      ...flattenZodJsonSchema(gatewayActionResponseSchema),
    },
    409: { description: "Reading already in progress on ESP32" },
    500: { description: "Start failed (ESP32 status in body)" },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};

export const postGatewayStopRouteSchema = {
  ...gatewayTag,
  summary: "Stop BLE reading",
  description: "Proxies POST /stop on the ESP32.",
  querystring: gatewayUrlQueryOpenApi,
  response: {
    200: {
      description: "Reading stopped",
      ...flattenZodJsonSchema(gatewayActionResponseSchema),
    },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};

export const postGatewayResetConfigRouteSchema = {
  ...gatewayTag,
  summary: "Reset ESP32 NVS and reboot",
  description: "Proxies POST /reset-config — clears saved Wi-Fi/API/MAC and restarts the ESP32.",
  querystring: gatewayUrlQueryOpenApi,
  response: {
    200: {
      description: "Config cleared, ESP32 restarting",
      ...flattenZodJsonSchema(gatewayActionResponseSchema),
    },
    502: { description: "ESP32 unreachable", ...gatewayErrorSchema },
    503: { description: "ESP32_GATEWAY_URL not set", ...gatewayErrorSchema },
  },
};
