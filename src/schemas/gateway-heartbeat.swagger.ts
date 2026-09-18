export const postGatewayHeartbeatRouteSchema = {
  tags: ["bracelets"],
  summary: "Upsert tablet gateway heartbeat",
  description:
    "BioSync mobile tablets POST diagnostic state every ~30s so the dashboard can show BLE/sync health without remote desktop. Also registers each device that has used the app (tabletId + androidId).",
  body: {
    type: "object",
    required: ["tabletId"],
    additionalProperties: false,
    properties: {
      tabletId: { type: "string", minLength: 1, example: "a1b2c3d4-e5f6-4789-abcd-ef0123456789" },
      tabletLabel: { type: "string", example: "Casa Dora" },
      appVersion: { type: "string", example: "0.2.5" },
      braceletMac: { type: "string", nullable: true, example: "E6:64:0D:30:D3:F9" },
      bleConnected: { type: "boolean" },
      isMeasuring: { type: "boolean" },
      phase: { type: "string", example: "activeModeHrvFatigue" },
      statusMessage: { type: "string" },
      pendingSyncCount: { type: "integer", minimum: 0 },
      lastSyncOk: { type: "boolean", nullable: true },
      lastSyncAt: { type: "string", format: "date-time", nullable: true },
      lastError: { type: "string", nullable: true },
      apiReachable: { type: "boolean", nullable: true },
      sentAt: { type: "string", format: "date-time", nullable: true },
      tabletBatteryPercent: { type: "integer", minimum: 0, maximum: 100, nullable: true },
      tabletCharging: { type: "boolean", nullable: true },
      deviceModel: { type: "string", nullable: true, example: "TB-X606F" },
      deviceManufacturer: { type: "string", nullable: true, example: "Lenovo" },
      androidId: { type: "string", nullable: true },
      sessionUser: { type: "string", nullable: true, example: "usuario" },
      sessionRole: { type: "string", nullable: true, example: "user" },
    },
  },
  response: {
    200: {
      type: "object",
      properties: {
        gateway: { type: "object", additionalProperties: true },
      },
    },
    400: {
      type: "object",
      properties: {
        error: { type: "string" },
      },
    },
    500: {
      type: "object",
      properties: {
        error: { type: "string" },
      },
    },
  },
} as const;

export const getGatewaysRouteSchema = {
  tags: ["bracelets"],
  summary: "List tablet gateways (latest heartbeat)",
  description:
    "Returns every tablet that has reported at least once (device registry via heartbeat). online=true when receivedAt is within the last 2 minutes.",
  response: {
    200: {
      type: "object",
      properties: {
        gateways: {
          type: "array",
          items: { type: "object", additionalProperties: true },
        },
      },
    },
  },
} as const;
