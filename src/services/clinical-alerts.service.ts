import type {
  ClinicalAlert,
  ClinicalAlertType,
  ClinicalAssessment,
  OverallStatus,
  AlertSeverity,
  PatientBaseline,
  RecentVitalsSnapshot,
  VitalsContext,
  VitalsInput,
} from "../types/clinical-alerts.types.js";

import {
  CLINICAL_DISCLAIMER,
  CLINICAL_PARAMETERS,
} from "../config/clinical-alerts.catalog.js";

import {
  NEWS2_SCORE_TO_SEVERITY,
  NEWS2_UNAVAILABLE_PARAMETERS,
  NEWS2_RESPONSE_LEVELS,
  type News2Assessment,
  type News2Component,
  type News2Score,
  news2ResponseToOverallStatus,
  resolveNews2ResponseLevel,
  scoreNews2Pulse,
  scoreNews2SpO2Scale1,
  scoreNews2Systolic,
  scoreNews2Temperature,
} from "../config/news2.catalog.js";

const BASELINE_MIN_SAMPLES = CLINICAL_PARAMETERS.baseline.minRestingSamples;
const BASELINE_CALIBRATION_DAYS = CLINICAL_PARAMETERS.baseline.calibrationDays;

const DISCLAIMER = CLINICAL_DISCLAIMER;

export type EvaluateInput = {
  deviceMac: string;
  measuredAt: string;
  vitals: VitalsInput;
  context?: VitalsContext;
  recentHistory?: RecentVitalsSnapshot[];
};

function median(values: number[]): number | null {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0
    ? (sorted[mid - 1]! + sorted[mid]!) / 2
    : sorted[mid]!;
}

export function computeBaselineFromHistory(
  history: RecentVitalsSnapshot[],
  measuredAt: string,
): PatientBaseline {
  const cutoff = new Date(measuredAt);
  cutoff.setDate(cutoff.getDate() - BASELINE_CALIBRATION_DAYS);

  const resting = history.filter((h) => {
    if (h.context.isResting !== true) return false;
    return new Date(h.measuredAt) >= cutoff;
  });

  const hrvValues = resting.map((h) => h.vitals.hrv).filter((v) => v > 0);
  const hrValues = resting.map((h) => h.vitals.heartRate).filter((v) => v > 0);
  const fatigueValues = resting.map((h) => h.vitals.fatigue).filter((v) => v >= 0);
  const spo2Values = resting.map((h) => h.vitals.spo2).filter((v) => v > 0);

  const sampleCount = resting.length;
  const calibrated = sampleCount >= BASELINE_MIN_SAMPLES;

  return {
    hrv: median(hrvValues),
    heartRate: median(hrValues),
    fatigue: median(fatigueValues),
    spo2: median(spo2Values),
    sampleCount,
    calibrated,
  };
}

function addAlert(
  alerts: ClinicalAlert[],
  type: ClinicalAlertType,
  severity: AlertSeverity,
  message: string,
  reason?: string,
): void {
  if (alerts.some((a) => a.type === type)) return;
  alerts.push({ type, severity, message, reason });
}

function hrvDropPercent(current: number, baseline: number): number {
  if (baseline <= 0) return 0;
  return ((baseline - current) / baseline) * 100;
}

type ParameterAlertMapping = {
  parameter: News2Component["parameter"];
  score: News2Score;
  type: ClinicalAlertType;
  message: string;
  reason: (value: number, score: News2Score) => string;
};

const PULSE_ALERTS: ParameterAlertMapping[] = [
  {
    parameter: "pulso",
    score: 3,
    type: "FC_CRITICA",
    message: "Pulso fora da faixa segura (NEWS 2 = 3).",
    reason: (v) => `Pulso ${v} bpm — NEWS 2: ≤40 ou ≥131 bpm.`,
  },
  {
    parameter: "pulso",
    score: 2,
    type: "FC_ALERTA",
    message: "Pulso elevado (NEWS 2 = 2).",
    reason: (v) => `Pulso ${v} bpm — NEWS 2: 111–130 bpm.`,
  },
];

const SYSTOLIC_ALERTS: ParameterAlertMapping[] = [
  {
    parameter: "pressao_sistolica",
    score: 3,
    type: "PA_CRITICA",
    message: "Pressão sistólica crítica (NEWS 2 = 3).",
    reason: (v) => `PAS ${v} mmHg — NEWS 2: ≤90 ou ≥220 mmHg.`,
  },
  {
    parameter: "pressao_sistolica",
    score: 2,
    type: "PA_ESTAGIO_2",
    message: "Pressão sistólica baixa (NEWS 2 = 2).",
    reason: (v) => `PAS ${v} mmHg — NEWS 2: 91–100 mmHg.`,
  },
  {
    parameter: "pressao_sistolica",
    score: 1,
    type: "PA_ESTAGIO_1",
    message: "Pressão sistólica levemente baixa (NEWS 2 = 1).",
    reason: (v) => `PAS ${v} mmHg — NEWS 2: 101–110 mmHg.`,
  },
];

const SPO2_ALERTS: ParameterAlertMapping[] = [
  {
    parameter: "spo2",
    score: 3,
    type: "SPO2_CRITICA",
    message: "SpO₂ crítica (NEWS 2 = 3).",
    reason: (v) => `SpO₂ ${v}% — NEWS 2 Escala 1: ≤91%.`,
  },
  {
    parameter: "spo2",
    score: 2,
    type: "SPO2_BAIXA",
    message: "SpO₂ baixa (NEWS 2 = 2).",
    reason: (v) => `SpO₂ ${v}% — NEWS 2 Escala 1: 92–93%.`,
  },
  {
    parameter: "spo2",
    score: 1,
    type: "SPO2_ATENCAO",
    message: "SpO₂ levemente reduzida (NEWS 2 = 1).",
    reason: (v) => `SpO₂ ${v}% — NEWS 2 Escala 1: 94–95%.`,
  },
];

const TEMPERATURE_ALERTS: ParameterAlertMapping[] = [
  {
    parameter: "temperatura",
    score: 3,
    type: "TEMP_BAIXA",
    message: "Hipotermia (NEWS 2 = 3).",
    reason: (v) => `Temperatura ${v} °C — NEWS 2: ≤35,0 °C.`,
  },
  {
    parameter: "temperatura",
    score: 2,
    type: "FEBRE_ALTA",
    message: "Temperatura muito elevada (NEWS 2 = 2).",
    reason: (v) => `Temperatura ${v} °C — NEWS 2: ≥39,1 °C.`,
  },
];

const PARAMETER_ALERTS: ParameterAlertMapping[] = [
  ...PULSE_ALERTS,
  ...SYSTOLIC_ALERTS,
  ...SPO2_ALERTS,
  ...TEMPERATURE_ALERTS,
];

function buildNews2Components(vitals: VitalsInput): News2Component[] {
  const scorers: Array<{
    parameter: News2Component["parameter"];
    label: string;
    unit: string;
    value: number;
    score: News2Score | null;
  }> = [
    {
      parameter: "pulso",
      label: "Pulso",
      unit: "bpm",
      value: vitals.heartRate,
      score: scoreNews2Pulse(vitals.heartRate),
    },
    {
      parameter: "pressao_sistolica",
      label: "Pressão arterial sistólica",
      unit: "mmHg",
      value: vitals.systolic,
      score: scoreNews2Systolic(vitals.systolic),
    },
    {
      parameter: "spo2",
      label: "SpO₂ (Escala 1)",
      unit: "%",
      value: vitals.spo2,
      score: scoreNews2SpO2Scale1(vitals.spo2),
    },
    {
      parameter: "temperatura",
      label: "Temperatura",
      unit: "°C",
      value: vitals.temperature,
      score: scoreNews2Temperature(vitals.temperature),
    },
  ];

  return scorers
    .filter((s): s is typeof s & { score: News2Score } => s.score != null)
    .map((s) => ({
      parameter: s.parameter,
      label: s.label,
      score: s.score,
      value: s.value,
      unit: s.unit,
    }));
}

function computeNews2Assessment(vitals: VitalsInput): News2Assessment {
  const components = buildNews2Components(vitals);
  const totalScore = components.reduce((sum, c) => sum + c.score, 0);
  const maxComponentScore = components.reduce<News2Score>(
    (max, c) => (c.score > max ? c.score : max),
    0,
  );
  const responseLevel = resolveNews2ResponseLevel(totalScore, maxComponentScore);

  return {
    totalScore,
    maxPossibleScore: 4 * 3,
    components,
    unavailableParameters: [...NEWS2_UNAVAILABLE_PARAMETERS],
    responseLevel,
  };
}

function addTemperatureScore1Alert(component: News2Component, alerts: ClinicalAlert[]): void {
  const isLow = component.value <= 36.0;
  addAlert(
    alerts,
    isLow ? "TEMP_ELEVADA" : "FEBRE",
    "LOW",
    isLow ? "Temperatura levemente baixa (NEWS 2 = 1)." : "Febre leve (NEWS 2 = 1).",
    isLow
      ? `Temperatura ${component.value} °C — NEWS 2: 35,1–36,0 °C.`
      : `Temperatura ${component.value} °C — NEWS 2: 38,1–39,0 °C.`,
  );
}

function addPulseScore1Alert(component: News2Component, alerts: ClinicalAlert[]): void {
  const isBradycardia = component.value <= 50;
  addAlert(
    alerts,
    isBradycardia ? "BRADICARDIA_LEVE" : "TAQUICARDIA_LEVE",
    "LOW",
    isBradycardia
      ? "Bradicardia leve (NEWS 2 = 1)."
      : "Pulso levemente elevado (NEWS 2 = 1).",
    isBradycardia
      ? `Pulso ${component.value} bpm — NEWS 2: 41–50 bpm.`
      : `Pulso ${component.value} bpm — NEWS 2: 91–110 bpm.`,
  );
}

function addParameterAlerts(
  components: News2Component[],
  alerts: ClinicalAlert[],
  notes: string[],
): void {
  for (const component of components) {
    if (component.score === 0) {
      notes.push(`${component.label}: ${component.value} ${component.unit} (NEWS 2 = 0).`);
      continue;
    }

    if (component.parameter === "pulso" && component.score === 1) {
      addPulseScore1Alert(component, alerts);
      continue;
    }

    if (component.parameter === "temperatura" && component.score === 1) {
      addTemperatureScore1Alert(component, alerts);
      continue;
    }

    const mapping = PARAMETER_ALERTS.find(
      (m) => m.parameter === component.parameter && m.score === component.score,
    );
    if (!mapping) continue;

    const severity = NEWS2_SCORE_TO_SEVERITY[component.score];
    if (!severity) continue;

    addAlert(
      alerts,
      mapping.type,
      severity,
      mapping.message,
      mapping.reason(component.value, component.score),
    );
  }
}

function addAggregateNews2Alerts(news2: News2Assessment, alerts: ClinicalAlert[]): void {
  if (news2.responseLevel === "emergency") {
    addAlert(
      alerts,
      "NEWS2_RESPOSTA_EMERGENCIA",
      "CRITICAL",
      "NEWS 2: resposta clínica de emergência.",
      `Pontuação NEWS 2 = ${news2.totalScore} (≥7). ${NEWS2_RESPONSE_LEVELS.emergency.label}.`,
    );
    return;
  }

  if (news2.responseLevel === "urgent") {
    const reason =
      news2.totalScore >= 5
        ? `Pontuação NEWS 2 = ${news2.totalScore} (5–6).`
        : `Parâmetro com pontuação 3 isolada (NEWS 2 = ${news2.totalScore}).`;

    addAlert(
      alerts,
      "NEWS2_RESPOSTA_URGENTE",
      "HIGH",
      "NEWS 2: resposta clínica urgente.",
      `${reason} ${NEWS2_RESPONSE_LEVELS.urgent.label}.`,
    );
  }
}

function noteMissingParameters(vitals: VitalsInput, notes: string[]): void {
  if (vitals.heartRate <= 0) notes.push("Pulso ausente — não pontuado no NEWS 2.");
  if (vitals.systolic <= 0) notes.push("Pressão sistólica ausente — não pontuada no NEWS 2.");
  if (vitals.spo2 <= 0) notes.push("SpO₂ ausente — não pontuada no NEWS 2.");
  if (vitals.temperature <= 0) notes.push("Temperatura ausente — não pontuada no NEWS 2.");

  notes.push(
    "NEWS 2 parcial: frequência respiratória, consciência e oxigênio suplementar não são medidos pela pulseira.",
  );
  notes.push("SpO₂ avaliada pela Escala 1 (ar ambiente assumido).");
}

function noteSupplementaryMetrics(
  vitals: VitalsInput,
  baseline: PatientBaseline,
  context: VitalsContext,
  notes: string[],
): void {
  if (vitals.hrv > 0 && baseline.calibrated && baseline.hrv != null) {
    const drop = hrvDropPercent(vitals.hrv, baseline.hrv);
    notes.push(
      `HRV ${vitals.hrv} ms (baseline ~${Math.round(baseline.hrv)} ms, variação ${drop >= 0 ? "−" : "+"}${Math.abs(Math.round(drop))}%).`,
    );
  } else if (vitals.hrv > 0) {
    notes.push(
      `HRV ${vitals.hrv} ms — baseline em calibração (${baseline.sampleCount}/${BASELINE_MIN_SAMPLES} leituras).`,
    );
  }

  if (vitals.fatigue >= 0) {
    notes.push(`Fadiga da pulseira: ${vitals.fatigue}%.`);
  }

  if (context.isResting === false) {
    notes.push("Leitura sem confirmação de repouso — interpretar pulso com cautela.");
  }
}

const SEVERITY_RANK: Record<AlertSeverity, number> = {
  LOW: 1,
  MEDIUM: 2,
  HIGH: 3,
  CRITICAL: 4,
};

function maxSeverity(alerts: ClinicalAlert[]): AlertSeverity {
  let max: AlertSeverity = "LOW";
  for (const a of alerts) {
    if (SEVERITY_RANK[a.severity] > SEVERITY_RANK[max]) {
      max = a.severity;
    }
  }
  return max;
}

function overallStatusFromNews2(
  news2: News2Assessment,
  alerts: ClinicalAlert[],
): OverallStatus {
  if (
    news2.responseLevel === "emergency" ||
    alerts.some((a) => a.type === "NEWS2_RESPOSTA_EMERGENCIA")
  ) {
    return "CRITICAL";
  }

  if (
    news2.responseLevel === "urgent" ||
    alerts.some((a) => a.type === "NEWS2_RESPOSTA_URGENTE")
  ) {
    return "ALERT";
  }

  return news2ResponseToOverallStatus(news2.responseLevel);
}

/** Avaliação clínica baseada no NEWS 2 (versão brasileira). */
export function evaluateClinicalAlerts(input: EvaluateInput): ClinicalAssessment {
  const context: VitalsContext = {
    isResting: true,
    signalQuality: "unknown",
    source: "2208A",
    ...input.context,
  };

  const history = input.recentHistory ?? [];
  const baseline = computeBaselineFromHistory(history, input.measuredAt);

  const alerts: ClinicalAlert[] = [];
  const notes: string[] = [];

  const news2 = computeNews2Assessment(input.vitals);

  addParameterAlerts(news2.components, alerts, notes);
  addAggregateNews2Alerts(news2, alerts);
  noteMissingParameters(input.vitals, notes);
  noteSupplementaryMetrics(input.vitals, baseline, context, notes);

  const severity = alerts.length === 0 ? "LOW" : maxSeverity(alerts);
  const overallStatus = overallStatusFromNews2(news2, alerts);

  return {
    deviceMac: input.deviceMac,
    measuredAt: input.measuredAt,
    vitals: input.vitals,
    context,
    alerts,
    notes,
    riskScore: news2.totalScore,
    overallStatus,
    severity,
    baseline,
    news2,
    disclaimer: DISCLAIMER,
  };
}

/** Monta vitals a partir do pacote 0x28 decodificado. */
export function vitalsFromDecodedHealth(decoded: {
  heartRate: number;
  spo2: number;
  hrv: number;
  fatigue: number;
  systolicPressure: number;
  diastolicPressure: number;
  temperature: number;
}): VitalsInput {
  return {
    heartRate: decoded.heartRate,
    systolic: decoded.systolicPressure,
    diastolic: decoded.diastolicPressure,
    temperature: decoded.temperature,
    spo2: decoded.spo2,
    hrv: decoded.hrv,
    fatigue: decoded.fatigue,
  };
}
