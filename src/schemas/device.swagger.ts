const patientInfoSchema = {
  type: "object",
  required: ["patientId", "patientName", "age", "email"],
  properties: {
    patientId: { type: "string", example: "3691", description: "Código do paciente" },
    patientName: { type: "string", example: "Jurandir Filadelfo Dos Santos" },
    age: { type: "integer", example: 40 },
    email: { type: "string", example: "carlos.mozer@pcpsaude.com.br" },
    leito: { type: "string", example: "LEITO 215", nullable: true },
    idatendimento: {
      type: "integer",
      example: 5054,
      nullable: true,
      description: "Código de atendimento (null para paciente de teste)",
    },
    convenio: { type: "string", example: "Caixa Economica Federal", nullable: true },
    unidade: { type: "string", example: "Alto da Boa Vista", nullable: true },
    internacao: { type: "string", example: "2ª ANDAR-ABV", nullable: true },
    perfil: { type: "string", example: "Reabilitação", nullable: true },
    dataEntrada: { type: "string", example: "10/06/2026", nullable: true },
  },
} as const;

const resolvedPatientSchema = {
  type: "object",
  required: ["deviceMac", "label", "patientId", "patientName", "age", "email"],
  properties: {
    deviceMac: { type: "string", example: "E6:64:0D:30:D3:F9" },
    label: { type: "string", example: "Bracelet 1" },
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
    "Returns the 4 configured bracelets (3 pacientes reais + Arlene de teste). patient/label/deviceMac são fixos; battery, mergedHealth, online e lastSeenAt vêm dos pacotes reais no banco.",
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

export const idAtendimentoFieldSchema = {
  type: "integer",
  nullable: true,
  description: "Código de atendimento do paciente vinculado ao MAC (null se não cadastrado ou teste)",
  example: 5054,
};
