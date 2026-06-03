import type { AlertSeverity, ClinicalAlertType, OverallStatus } from "../types/clinical-alerts.types.js";

export const CLINICAL_DISCLAIMER =
  "Triagem por tendência — não substitui diagnóstico clínico. Valores de pulseira podem variar por movimento, circulação e pigmentação da pele.";

export type AlertTypeCatalogEntry = {
  type: ClinicalAlertType;
  label: string;
  category: "heart_rate" | "blood_pressure" | "spo2" | "temperature" | "hrv" | "fatigue" | "combined";
  severities: AlertSeverity[];
  description: string;
};

export const CLINICAL_ALERT_TYPES: AlertTypeCatalogEntry[] = [
  { type: "BRADICARDIA_LEVE", label: "Bradicardia leve", category: "heart_rate", severities: ["LOW"], description: "FC 45–49 bpm em repouso" },
  { type: "TAQUICARDIA_LEVE", label: "Taquicardia leve", category: "heart_rate", severities: ["LOW"], description: "FC 101–119 bpm em repouso (2+ leituras)" },
  { type: "FC_ALERTA", label: "FC em alerta", category: "heart_rate", severities: ["HIGH"], description: "FC <45 ou 120–139 bpm em repouso (2+ leituras)" },
  { type: "FC_CRITICA", label: "FC crítica", category: "heart_rate", severities: ["CRITICAL"], description: "FC <40 ou ≥140 bpm em repouso" },
  { type: "PA_ELEVADA", label: "Pressão elevada", category: "blood_pressure", severities: ["LOW"], description: "Sistólica 120–129 e diastólica <80" },
  { type: "PA_ESTAGIO_1", label: "Hipertensão estágio 1", category: "blood_pressure", severities: ["MEDIUM"], description: "Sistólica ≥130 ou diastólica ≥80" },
  { type: "PA_ESTAGIO_2", label: "Hipertensão estágio 2", category: "blood_pressure", severities: ["HIGH"], description: "Sistólica ≥140 ou diastólica ≥90 (2+ leituras)" },
  { type: "PA_CRITICA", label: "Pressão crítica", category: "blood_pressure", severities: ["CRITICAL"], description: "Sistólica >180 ou diastólica >120" },
  { type: "SPO2_ATENCAO", label: "SpO₂ em atenção", category: "spo2", severities: ["LOW"], description: "SpO₂ 93–94%" },
  { type: "SPO2_BAIXA", label: "SpO₂ baixa", category: "spo2", severities: ["HIGH"], description: "SpO₂ 90–92% ou queda ≥4 p.p. vs basal" },
  { type: "SPO2_CRITICA", label: "SpO₂ crítica", category: "spo2", severities: ["CRITICAL"], description: "SpO₂ <90% ou ≤92% em 2 leituras" },
  { type: "TEMP_ELEVADA", label: "Temperatura elevada", category: "temperature", severities: ["LOW"], description: "37,5–37,9 °C (pele/punho)" },
  { type: "FEBRE", label: "Febre", category: "temperature", severities: ["MEDIUM"], description: "38,0–38,9 °C" },
  { type: "FEBRE_ALTA", label: "Febre alta", category: "temperature", severities: ["HIGH", "CRITICAL"], description: "≥39,0 °C (crítico se ≥39,5)" },
  { type: "TEMP_BAIXA", label: "Temperatura baixa", category: "temperature", severities: ["MEDIUM"], description: "<35,5 °C" },
  { type: "HRV_ATENCAO", label: "HRV abaixo do basal", category: "hrv", severities: ["LOW"], description: "Queda 15–25% vs baseline (requer calibração)" },
  { type: "HRV_ALERTA", label: "Queda de HRV", category: "hrv", severities: ["MEDIUM", "HIGH"], description: "Queda 25–40%+ vs baseline" },
  { type: "HRV_CRITICO_FUNCIONAL", label: "Estresse fisiológico", category: "hrv", severities: ["HIGH"], description: "HRV >40% abaixo + FC +10 bpm + fadiga alta" },
  { type: "FADIGA_MODERADA", label: "Fadiga moderada", category: "fatigue", severities: ["LOW"], description: "Fadiga 40–59%" },
  { type: "FADIGA_ALTA", label: "Fadiga alta", category: "fatigue", severities: ["MEDIUM"], description: "Fadiga 60–79%" },
  { type: "FADIGA_CRITICA_FUNCIONAL", label: "Fadiga muito alta", category: "fatigue", severities: ["HIGH"], description: "Fadiga ≥80%" },
  { type: "POSSIVEL_FADIGA_FISIOLOGICA", label: "Possível fadiga fisiológica", category: "combined", severities: ["MEDIUM"], description: "HRV −25% + fadiga ≥60% + FC +10 bpm vs basal" },
  { type: "POSSIVEL_ESTRESSE_INFECCIOSO", label: "Possível quadro febril", category: "combined", severities: ["MEDIUM"], description: "Temp ≥37,8 + FC +10 bpm + HRV −20%" },
  { type: "POSSIVEL_RISCO_RESPIRATORIO", label: "Possível risco respiratório", category: "combined", severities: ["HIGH"], description: "SpO₂ baixa/queda + FC ≥100 em repouso" },
  { type: "POSSIVEL_ESTRESSE_CARDIOVASCULAR", label: "Possível estresse cardiovascular", category: "combined", severities: ["HIGH"], description: "PA ≥140/90 + FC ≥100 + HRV −25%" },
  { type: "ALERTA_CRITICO", label: "Alerta crítico imediato", category: "combined", severities: ["CRITICAL"], description: "SpO₂ <90, PA >180/120, FC ≥140 repouso ou temp ≥39,5" },
];

export const CLINICAL_PARAMETERS = {
  baseline: {
    minRestingSamples: 5,
    calibrationDays: 7,
    hrvDeviationPercent: { normal: 15, attention: 25, alert: 40 },
  },
  persistence: {
    consecutiveReadings: 2,
  },
  heartRateResting: {
    normal: { min: 50, max: 100 },
    bradycardiaMild: { min: 45, max: 49 },
    tachycardiaMild: { min: 101, max: 119 },
    alert: { below: 45, highMin: 120, highMax: 139 },
    critical: { below: 40, from: 140 },
  },
  bloodPressure: {
    elevated: { systolicMin: 120, systolicMax: 129, diastolicMax: 79 },
    stage1: { systolicMin: 130, diastolicMin: 80 },
    stage2: { systolicMin: 140, diastolicMin: 90 },
    critical: { systolicAbove: 180, diastolicAbove: 120 },
  },
  spo2: {
    normalFrom: 95,
    attention: { min: 93, max: 94 },
    low: { min: 90, max: 92 },
    criticalBelow: 90,
    baselineDropPoints: 4,
  },
  temperature: {
    normal: { min: 35.8, max: 37.4 },
    elevated: { min: 37.5, max: 37.9 },
    fever: { min: 38.0, max: 38.9 },
    highFeverFrom: 39.0,
    criticalFrom: 39.5,
    lowBelow: 35.5,
  },
  fatigue: {
    low: { max: 39 },
    moderate: { min: 40, max: 59 },
    high: { min: 60, max: 79 },
    criticalFrom: 80,
  },
  overallStatus: ["STABLE", "ATTENTION", "ALERT", "CRITICAL"] as OverallStatus[],
  severity: ["LOW", "MEDIUM", "HIGH", "CRITICAL"] as AlertSeverity[],
  riskScore: { min: 0, max: 100 },
};

export function getClinicalAlertsCatalog() {
  return {
    version: "1.0.0",
    disclaimer: CLINICAL_DISCLAIMER,
    alertTypes: CLINICAL_ALERT_TYPES,
    parameters: CLINICAL_PARAMETERS,
  };
}
