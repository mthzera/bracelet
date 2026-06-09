const patientInfoSchema = {
  type: "object",
  required: ["patientId", "patientName", "room", "age"],
  properties: {
    patientId: { type: "string", example: "P001" },
    patientName: { type: "string", example: "Maria Silva" },
    room: { type: "string", example: "101" },
    age: { type: "integer", example: 68 },
  },
} as const;

const resolvedPatientSchema = {
  type: "object",
  required: ["deviceMac", "label", "patientId", "patientName", "room", "age"],
  properties: {
    deviceMac: { type: "string", example: "E6:64:0D:30:D3:F9" },
    label: { type: "string", example: "Pulseira 1" },
    ...patientInfoSchema.properties,
  },
} as const;

const deviceOverviewSchema = {
  type: "object",
  required: ["deviceMac", "label", "patient", "online", "lastSeenAt", "battery", "mergedHealth"],
  properties: {
    deviceMac: { type: "string" },
    label: { type: "string" },
    patient: patientInfoSchema,
    online: { type: "boolean" },
    lastSeenAt: { type: "string", nullable: true },
    battery: { type: "integer", nullable: true },
    mergedHealth: {
      type: "object",
      nullable: true,
      additionalProperties: true,
    },
  },
} as const;

export const getDevicesRouteSchema = {
  tags: ["bracelets"],
  summary: "List registered test bracelets with latest vitals",
  description:
    "Returns the 4 configured bracelets (MAC → patient mapping) with last seen time, battery, and merged health per device.",
  response: {
    200: {
      description: "Registered devices overview",
      type: "object",
      required: ["devices"],
      properties: {
        devices: {
          type: "array",
          items: deviceOverviewSchema,
        },
      },
    },
  },
};

export const patientFieldSchema = {
  ...resolvedPatientSchema,
  nullable: true,
  description: "Patient resolved from deviceMac (null if MAC is not registered)",
};
