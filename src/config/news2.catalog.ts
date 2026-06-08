import type { AlertSeverity, OverallStatus } from "../types/clinical-alerts.types.js";

/** Parâmetros mensuráveis pela pulseira (Escala 1 de SpO₂, ar ambiente assumido). */
export type News2MeasurableParameter = "pulso" | "pressao_sistolica" | "spo2" | "temperatura";

export type News2Score = 0 | 1 | 2 | 3;

export type News2Component = {
  parameter: News2MeasurableParameter;
  label: string;
  score: News2Score;
  value: number;
  unit: string;
};

export type News2Assessment = {
  totalScore: number;
  maxPossibleScore: number;
  components: News2Component[];
  unavailableParameters: string[];
  responseLevel: News2ResponseLevel;
};

export type News2ResponseLevel = "routine" | "low" | "medium" | "urgent" | "emergency";

export const NEWS2_UNAVAILABLE_PARAMETERS = [
  "frequencia_respiratoria",
  "spo2_escala_2",
  "suplementacao_oxigenio",
  "consciencia",
] as const;

export const NEWS2_RESPONSE_LEVELS: Record<
  News2ResponseLevel,
  { label: string; overallStatus: OverallStatus; minTotalScore?: number; anyParameterScore3?: boolean }
> = {
  routine: { label: "Monitoramento de rotina", overallStatus: "STABLE" },
  low: { label: "Avaliação por profissional competente", overallStatus: "ATTENTION", minTotalScore: 1 },
  medium: { label: "Avaliação urgente pela equipe", overallStatus: "ATTENTION", minTotalScore: 3 },
  urgent: { label: "Resposta clínica urgente", overallStatus: "ALERT", minTotalScore: 5 },
  emergency: { label: "Resposta clínica de emergência", overallStatus: "CRITICAL", minTotalScore: 7 },
};

export const NEWS2_SCORE_TO_SEVERITY: Record<News2Score, AlertSeverity | null> = {
  0: null,
  1: "LOW",
  2: "MEDIUM",
  3: "HIGH",
};

/** Pontuação NEWS 2 — Pulso (bpm). */
export function scoreNews2Pulse(heartRate: number): News2Score | null {
  if (heartRate <= 0) return null;
  if (heartRate <= 40) return 3;
  if (heartRate <= 50) return 1;
  if (heartRate <= 90) return 0;
  if (heartRate <= 110) return 1;
  if (heartRate <= 130) return 2;
  return 3;
}

/** Pontuação NEWS 2 — Pressão arterial sistólica (mmHg). */
export function scoreNews2Systolic(systolic: number): News2Score | null {
  if (systolic <= 0) return null;
  if (systolic <= 90) return 3;
  if (systolic <= 100) return 2;
  if (systolic <= 110) return 1;
  if (systolic <= 219) return 0;
  return 3;
}

/** Pontuação NEWS 2 — SpO₂ Escala 1 (%). */
export function scoreNews2SpO2Scale1(spo2: number): News2Score | null {
  if (spo2 <= 0) return null;
  if (spo2 <= 91) return 3;
  if (spo2 <= 93) return 2;
  if (spo2 <= 95) return 1;
  return 0;
}

/** Pontuação NEWS 2 — Temperatura (°C). */
export function scoreNews2Temperature(temperature: number): News2Score | null {
  if (temperature <= 0) return null;
  if (temperature <= 35.0) return 3;
  if (temperature <= 36.0) return 1;
  if (temperature <= 38.0) return 0;
  if (temperature <= 39.0) return 1;
  return 2;
}

export function resolveNews2ResponseLevel(
  totalScore: number,
  maxComponentScore: News2Score,
): News2ResponseLevel {
  if (totalScore >= 7) return "emergency";
  if (totalScore >= 5) return "urgent";
  if (maxComponentScore >= 3) return "urgent";
  if (totalScore >= 3) return "medium";
  if (totalScore >= 1) return "low";
  return "routine";
}

export function news2ResponseToOverallStatus(level: News2ResponseLevel): OverallStatus {
  return NEWS2_RESPONSE_LEVELS[level].overallStatus;
}
