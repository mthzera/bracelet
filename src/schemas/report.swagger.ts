import { REPORT_PATIENT_NAMES, TEST_BRACELETS } from "../config/test-bracelets.js";

const patientOptionsDescription = TEST_BRACELETS.map(
  (b) => `${b.patient.patientName} (${b.label} · ${b.deviceMac})`,
).join(" · ");

export const getVitalsReportRouteSchema = {
  tags: ["bracelets"],
  summary: "Gerar relatório de vitais com gráfico",
  description:
    "Selecione o paciente no dropdown. Retorna resumo, imagem PNG completa (texto + gráfico) em `chartImageBase64` e `teamsMessage` só com texto. Baixe o PNG em GET `/bracelets/reports/vitals/image`. Deixe `patientName` vazio para as 4 pulseiras.",
  querystring: {
    type: "object",
    properties: {
      patientName: {
        type: "string",
        enum: [...REPORT_PATIENT_NAMES],
        description: `Paciente cadastrado. ${patientOptionsDescription}`,
      },
      windowMinutes: {
        type: "integer",
        minimum: 5,
        maximum: 1440,
        default: 60,
        description: "Janela do histórico em minutos (padrão: última hora)",
      },
    },
  },
  response: {
    200: {
      description: "Relatório(s) de vitais",
      type: "object",
      additionalProperties: true,
    },
    404: {
      description: "Paciente não cadastrado",
      type: "object",
      properties: {
        error: { type: "string" },
        patientName: { type: "string" },
      },
    },
  },
};

export const getVitalsReportImageRouteSchema = {
  tags: ["bracelets"],
  summary: "Baixar relatório PNG (resumo + gráfico)",
  description:
    "Faz download direto do PNG com resumo e gráfico na mesma imagem. Use o mesmo dropdown de `patientName`.",
  querystring: {
    type: "object",
    required: ["patientName"],
    properties: {
      patientName: {
        type: "string",
        enum: [...REPORT_PATIENT_NAMES],
        description: patientOptionsDescription,
      },
      windowMinutes: {
        type: "integer",
        minimum: 5,
        maximum: 1440,
        default: 60,
      },
    },
  },
  response: {
    200: {
      description: "PNG do relatório completo",
      type: "string",
      format: "binary",
    },
    400: {
      description: "patientName obrigatório",
      type: "object",
      properties: { error: { type: "string" } },
    },
    404: {
      description: "Paciente não cadastrado",
      type: "object",
      properties: {
        error: { type: "string" },
        patientName: { type: "string" },
      },
    },
    500: {
      description: "Falha ao gerar imagem do relatório",
      type: "object",
      properties: {
        error: { type: "string" },
        patientName: { type: "string" },
      },
    },
  },
};
