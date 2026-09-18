export const postGatewayHeartbeatRouteSchema = {
  tags: ["bracelets"],
  summary: "Upsert tablet gateway heartbeat",
  description:
    "BioSync tablets POST diagnostic state every ~30s. Response includes pending remote commands. Optional logs[] are stored.",
  body: {
    type: "object",
    required: ["tabletId"],
    additionalProperties: true,
    properties: {
      tabletId: { type: "string", minLength: 1 },
      tabletLabel: { type: "string" },
      appVersion: { type: "string" },
      braceletMac: { type: "string", nullable: true },
      bleConnected: { type: "boolean" },
      isMeasuring: { type: "boolean" },
      phase: { type: "string" },
      statusMessage: { type: "string" },
      pendingSyncCount: { type: "integer", minimum: 0 },
      lastSyncOk: { type: "boolean", nullable: true },
      lastSyncAt: { type: "string", format: "date-time", nullable: true },
      lastError: { type: "string", nullable: true },
      apiReachable: { type: "boolean", nullable: true },
      sentAt: { type: "string", format: "date-time", nullable: true },
      tabletBatteryPercent: { type: "integer", minimum: 0, maximum: 100, nullable: true },
      tabletCharging: { type: "boolean", nullable: true },
      deviceModel: { type: "string", nullable: true },
      deviceManufacturer: { type: "string", nullable: true },
      androidId: { type: "string", nullable: true },
      sessionUser: { type: "string", nullable: true },
      sessionRole: { type: "string", nullable: true },
      lastBleConnectedAt: { type: "string", format: "date-time", nullable: true },
      lastBleDisconnectedAt: { type: "string", format: "date-time", nullable: true },
      lastBleDisconnectReason: { type: "string", nullable: true },
      logs: {
        type: "array",
        items: {
          type: "object",
          properties: {
            loggedAt: { type: "string", format: "date-time" },
            level: { type: "string" },
            message: { type: "string" },
          },
        },
      },
    },
  },
  response: {
    200: {
      type: "object",
      additionalProperties: true,
    },
    400: { type: "object", properties: { error: { type: "string" } } },
    500: { type: "object", properties: { error: { type: "string" } } },
  },
} as const;

export const getGatewaysRouteSchema = {
  tags: ["bracelets"],
  summary: "List tablet gateways",
  response: {
    200: { type: "object", additionalProperties: true },
  },
} as const;

export const getGatewayDetailRouteSchema = {
  tags: ["bracelets"],
  summary: "Tablet detail with logs and recent commands",
  params: {
    type: "object",
    required: ["tabletId"],
    properties: { tabletId: { type: "string" } },
  },
  response: {
    200: { type: "object", additionalProperties: true },
    404: { type: "object", properties: { error: { type: "string" } } },
  },
} as const;

export const postGatewayCommandRouteSchema = {
  tags: ["bracelets"],
  summary: "Enqueue remote command for tablet",
  description: "Requires header X-Gateway-Admin-Token matching GATEWAY_ADMIN_TOKEN.",
  params: {
    type: "object",
    required: ["tabletId"],
    properties: { tabletId: { type: "string" } },
  },
  body: {
    type: "object",
    required: ["command"],
    properties: {
      command: {
        type: "string",
        enum: [
          "restart_app",
          "reconnect_ble",
          "start_measurement",
          "stop_measurement",
          "retry_pending_sync",
        ],
      },
    },
  },
  response: {
    200: { type: "object", additionalProperties: true },
    400: { type: "object", properties: { error: { type: "string" } } },
    401: { type: "object", properties: { error: { type: "string" } } },
    404: { type: "object", properties: { error: { type: "string" } } },
  },
} as const;

export const postGatewayCommandResultRouteSchema = {
  tags: ["bracelets"],
  summary: "Report remote command result from tablet",
  params: {
    type: "object",
    required: ["tabletId", "commandId"],
    properties: {
      tabletId: { type: "string" },
      commandId: { type: "string" },
    },
  },
  body: {
    type: "object",
    required: ["ok"],
    properties: {
      ok: { type: "boolean" },
      result: { type: "string", nullable: true },
    },
  },
  response: {
    200: { type: "object", additionalProperties: true },
    400: { type: "object", properties: { error: { type: "string" } } },
    404: { type: "object", properties: { error: { type: "string" } } },
  },
} as const;

export const postGatewayLogsRouteSchema = {
  tags: ["bracelets"],
  summary: "Upload tablet log batch",
  params: {
    type: "object",
    required: ["tabletId"],
    properties: { tabletId: { type: "string" } },
  },
  body: {
    type: "object",
    required: ["logs"],
    properties: {
      logs: {
        type: "array",
        items: {
          type: "object",
          required: ["message"],
          properties: {
            loggedAt: { type: "string", format: "date-time" },
            level: { type: "string" },
            message: { type: "string" },
          },
        },
      },
    },
  },
  response: {
    200: { type: "object", additionalProperties: true },
  },
} as const;
