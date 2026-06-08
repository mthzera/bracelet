import type { AlertSeverity, ClinicalAlertType, OverallStatus } from "../types/clinical-alerts.types.js";
import { NEWS2_RESPONSE_LEVELS } from "./news2.catalog.js";

export const CLINICAL_DISCLAIMER =
  "Triagem por NEWS 2 (versão brasileira) — parcial, pois a pulseira não mede frequência respiratória, consciência nem oxigênio suplementar. Não substitui diagnóstico clínico.";

export type AlertTypeCatalogEntry = {
  type: ClinicalAlertType;
  label: string;
  category: "heart_rate" | "blood_pressure" | "spo2" | "temperature" | "hrv" | "fatigue" | "combined" | "news2";
  severities: AlertSeverity[];
  description: string;
};

export const CLINICAL_ALERT_TYPES: AlertTypeCatalogEntry[] = [
  { type: "BRADICARDIA_LEVE", label: "Bradicardia leve", category: "heart_rate", severities: ["LOW"], description: "NEWS 2 Pulso = 1 (41–50 bpm)" },
  { type: "TAQUICARDIA_LEVE", label: "Pulso levemente elevado", category: "heart_rate", severities: ["LOW"], description: "NEWS 2 Pulso = 1 (91–110 bpm)" },
  { type: "FC_ALERTA", label: "Pulso elevado", category: "heart_rate", severities: ["MEDIUM"], description: "NEWS 2 Pulso = 2 (111–130 bpm)" },
  { type: "FC_CRITICA", label: "Pulso crítico", category: "heart_rate", severities: ["HIGH"], description: "NEWS 2 Pulso = 3 (≤40 ou ≥131 bpm)" },
  { type: "PA_ESTAGIO_1", label: "PAS levemente baixa", category: "blood_pressure", severities: ["LOW"], description: "NEWS 2 PAS = 1 (101–110 mmHg)" },
  { type: "PA_ESTAGIO_2", label: "PAS baixa", category: "blood_pressure", severities: ["MEDIUM"], description: "NEWS 2 PAS = 2 (91–100 mmHg)" },
  { type: "PA_CRITICA", label: "PAS crítica", category: "blood_pressure", severities: ["HIGH"], description: "NEWS 2 PAS = 3 (≤90 ou ≥220 mmHg)" },
  { type: "SPO2_ATENCAO", label: "SpO₂ levemente reduzida", category: "spo2", severities: ["LOW"], description: "NEWS 2 SpO₂ Escala 1 = 1 (94–95%)" },
  { type: "SPO2_BAIXA", label: "SpO₂ baixa", category: "spo2", severities: ["MEDIUM"], description: "NEWS 2 SpO₂ Escala 1 = 2 (92–93%)" },
  { type: "SPO2_CRITICA", label: "SpO₂ crítica", category: "spo2", severities: ["HIGH"], description: "NEWS 2 SpO₂ Escala 1 = 3 (≤91%)" },
  { type: "TEMP_ELEVADA", label: "Temperatura levemente baixa", category: "temperature", severities: ["LOW"], description: "NEWS 2 Temperatura = 1 (35,1–36,0 °C)" },
  { type: "FEBRE", label: "Febre leve", category: "temperature", severities: ["LOW"], description: "NEWS 2 Temperatura = 1 (38,1–39,0 °C)" },
  { type: "FEBRE_ALTA", label: "Temperatura muito elevada", category: "temperature", severities: ["MEDIUM"], description: "NEWS 2 Temperatura = 2 (≥39,1 °C)" },
  { type: "TEMP_BAIXA", label: "Hipotermia", category: "temperature", severities: ["HIGH"], description: "NEWS 2 Temperatura = 3 (≤35,0 °C)" },
  { type: "NEWS2_RESPOSTA_URGENTE", label: "NEWS 2 — resposta urgente", category: "news2", severities: ["HIGH"], description: "Pontuação 5–6 ou parâmetro isolado com pontuação 3" },
  { type: "NEWS2_RESPOSTA_EMERGENCIA", label: "NEWS 2 — emergência", category: "news2", severities: ["CRITICAL"], description: "Pontuação total ≥ 7" },
];

export const CLINICAL_PARAMETERS = {
  scoringSystem: "NEWS2-BR",
  news2: {
    measurableParameters: ["pulso", "pressao_sistolica", "spo2_escala_1", "temperatura"],
    unavailableParameters: [
      "frequencia_respiratoria",
      "spo2_escala_2",
      "suplementacao_oxigenio",
      "consciencia",
    ],
    pulse: {
      score3: { max: 40, minHigh: 131 },
      score2: { min: 111, max: 130 },
      score1: [{ min: 41, max: 50 }, { min: 91, max: 110 }],
      score0: { min: 51, max: 90 },
    },
    systolic: {
      score3: { max: 90, minHigh: 220 },
      score2: { min: 91, max: 100 },
      score1: { min: 101, max: 110 },
      score0: { min: 111, max: 219 },
    },
    spo2Scale1: {
      score3: { max: 91 },
      score2: { min: 92, max: 93 },
      score1: { min: 94, max: 95 },
      score0: { min: 96 },
    },
    temperature: {
      score3: { max: 35.0 },
      score1Low: { min: 35.1, max: 36.0 },
      score0: { min: 36.1, max: 38.0 },
      score1High: { min: 38.1, max: 39.0 },
      score2: { min: 39.1 },
    },
    responseLevels: NEWS2_RESPONSE_LEVELS,
  },
  baseline: {
    minRestingSamples: 5,
    calibrationDays: 7,
    note: "Baseline usado apenas para notas de HRV; alertas seguem NEWS 2 absoluto.",
  },
  overallStatus: ["STABLE", "ATTENTION", "ALERT", "CRITICAL"] as OverallStatus[],
  severity: ["LOW", "MEDIUM", "HIGH", "CRITICAL"] as AlertSeverity[],
  riskScore: {
    min: 0,
    max: 12,
    description: "Pontuação total NEWS 2 parcial (4 parâmetros × 3 pontos).",
  },
};

export function getClinicalAlertsCatalog() {
  return {
    version: "2.0.0",
    disclaimer: CLINICAL_DISCLAIMER,
    alertTypes: CLINICAL_ALERT_TYPES,
    parameters: CLINICAL_PARAMETERS,
  };
}
