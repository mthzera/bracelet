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

const BASELINE_MIN_SAMPLES = CLINICAL_PARAMETERS.baseline.minRestingSamples;
const BASELINE_CALIBRATION_DAYS = CLINICAL_PARAMETERS.baseline.calibrationDays;
const PERSISTENCE_READINGS = CLINICAL_PARAMETERS.persistence.consecutiveReadings;

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

function countConsecutive(
  history: RecentVitalsSnapshot[],
  predicate: (v: VitalsInput) => boolean,
  includeCurrent: boolean,
  current: VitalsInput,
): number {
  let count = includeCurrent && predicate(current) ? 1 : 0;
  for (const snap of history) {
    if (!predicate(snap.vitals)) break;
    count++;
  }
  return count;
}

function evaluateHeartRate(
  vitals: VitalsInput,
  context: VitalsContext,
  history: RecentVitalsSnapshot[],
  alerts: ClinicalAlert[],
  notes: string[],
): void {
  const hr = vitals.heartRate;
  const resting = context.isResting !== false;

  if (hr <= 0) {
    notes.push("FC ausente ou inválida na leitura.");
    return;
  }

  if (!resting) {
    notes.push("FC avaliada sem flag de repouso — limites de repouso não aplicados.");
    if (hr >= 140) {
      addAlert(
        alerts,
        "FC_CRITICA",
        "CRITICAL",
        "Frequência cardíaca muito elevada.",
        `FC ${hr} bpm (atividade ou repouso não confirmado).`,
      );
    }
    return;
  }

  if (hr < 40) {
    addAlert(
      alerts,
      "FC_CRITICA",
      "CRITICAL",
      "Bradicardia crítica em repouso.",
      `FC ${hr} bpm < 40.`,
    );
    return;
  }

  if (hr >= 140) {
    addAlert(
      alerts,
      "FC_CRITICA",
      "CRITICAL",
      "Taquicardia crítica em repouso.",
      `FC ${hr} bpm ≥ 140 em repouso.`,
    );
    return;
  }

  const lowAlert = (v: VitalsInput) => v.heartRate < 45;
  const lowAttention = (v: VitalsInput) => v.heartRate >= 45 && v.heartRate <= 49;
  const highAttention = (v: VitalsInput) => v.heartRate >= 101 && v.heartRate <= 119;
  const highAlert = (v: VitalsInput) => v.heartRate >= 120 && v.heartRate <= 139;

  if (hr >= 50 && hr <= 100) {
    notes.push(`FC ${hr} bpm normal em repouso.`);
    return;
  }

  if (lowAttention({ heartRate: hr } as VitalsInput) || hr < 45) {
    if (hr < 45) {
      const streak = countConsecutive(history, lowAlert, true, vitals);
      if (streak >= PERSISTENCE_READINGS) {
        addAlert(
          alerts,
          "FC_ALERTA",
          "HIGH",
          "Bradicardia em repouso — monitorar.",
          `FC ${hr} bpm < 45 por ${streak} leitura(s).`,
        );
      }
    } else {
      addAlert(
        alerts,
        "BRADICARDIA_LEVE",
        "LOW",
        "FC levemente baixa em repouso.",
        `FC ${hr} bpm (45–49).`,
      );
    }
    return;
  }

  if (highAttention({ heartRate: hr } as VitalsInput)) {
    const streak = countConsecutive(history, highAttention, true, vitals);
    if (streak >= PERSISTENCE_READINGS) {
      addAlert(
        alerts,
        "TAQUICARDIA_LEVE",
        "LOW",
        "Taquicardia leve em repouso — verificar tendência.",
        `FC ${hr} bpm (101–119) por ${streak} leitura(s).`,
      );
    } else {
      notes.push(`FC ${hr} bpm acima do repouso ideal; aguardando confirmação.`);
    }
    return;
  }

  if (highAlert({ heartRate: hr } as VitalsInput)) {
    const streak = countConsecutive(history, highAlert, true, vitals);
    if (streak >= PERSISTENCE_READINGS) {
      addAlert(
        alerts,
        "FC_ALERTA",
        "HIGH",
        "Taquicardia em repouso — monitorar.",
        `FC ${hr} bpm (120–139) por ${streak} leitura(s).`,
      );
    }
  }
}

function evaluateBloodPressure(
  vitals: VitalsInput,
  history: RecentVitalsSnapshot[],
  alerts: ClinicalAlert[],
  notes: string[],
): void {
  const { systolic: sys, diastolic: dia } = vitals;

  if (sys <= 0 && dia <= 0) {
    notes.push("Pressão arterial ausente na leitura.");
    return;
  }

  if (sys > 180 || dia > 120) {
    addAlert(
      alerts,
      "PA_CRITICA",
      "CRITICAL",
      "Pressão arterial em faixa de emergência.",
      `Pressão ${sys}/${dia} mmHg — critério AHA severo.`,
    );
    return;
  }

  const stage2 = (v: VitalsInput) => v.systolic >= 140 || v.diastolic >= 90;
  if (stage2(vitals)) {
    const streak = countConsecutive(history, stage2, true, vitals);
    if (streak >= PERSISTENCE_READINGS) {
      addAlert(
        alerts,
        "PA_ESTAGIO_2",
        "HIGH",
        "Hipertensão estágio 2.",
        `Pressão ${sys}/${dia} mmHg em ${streak} leitura(s).`,
      );
    } else {
      addAlert(
        alerts,
        "PA_ESTAGIO_1",
        "MEDIUM",
        "Pressão elevada — confirmar segunda leitura.",
        `Pressão ${sys}/${dia} mmHg (aguardando confirmação estágio 2).`,
      );
    }
    return;
  }

  if (sys >= 130 || dia >= 80) {
    addAlert(
      alerts,
      "PA_ESTAGIO_1",
      "MEDIUM",
      "Hipertensão estágio 1.",
      `Pressão ${sys}/${dia} mmHg.`,
    );
    return;
  }

  if (sys >= 120 && sys <= 129 && dia < 80) {
    addAlert(
      alerts,
      "PA_ELEVADA",
      "LOW",
      "Pressão sistólica em faixa elevada. Monitorar tendência.",
      `Pressão ${sys}/${dia}: sistólica em faixa elevada, sem critério de urgência.`,
    );
    return;
  }

  if (sys > 0 || dia > 0) {
    notes.push(`Pressão ${sys}/${dia} mmHg dentro da faixa normal ideal.`);
  }
}

function evaluateSpO2(
  vitals: VitalsInput,
  baseline: PatientBaseline,
  history: RecentVitalsSnapshot[],
  alerts: ClinicalAlert[],
  notes: string[],
): void {
  const spo2 = vitals.spo2;
  if (spo2 <= 0) {
    notes.push("SpO₂ ausente na leitura.");
    return;
  }

  if (spo2 < 90) {
    addAlert(
      alerts,
      "SPO2_CRITICA",
      "CRITICAL",
      "Saturação crítica.",
      `SpO₂ ${spo2}% < 90%.`,
    );
    return;
  }

  const lowBand = (v: VitalsInput) => v.spo2 >= 90 && v.spo2 <= 92;
  if (lowBand(vitals)) {
    const streak = countConsecutive(history, lowBand, true, vitals);
    if (streak >= PERSISTENCE_READINGS) {
      addAlert(
        alerts,
        "SPO2_CRITICA",
        "CRITICAL",
        "Saturação baixa persistente.",
        `SpO₂ ${spo2}% ≤ 92% por ${streak} leitura(s).`,
      );
    } else {
      addAlert(
        alerts,
        "SPO2_BAIXA",
        "HIGH",
        "Saturação baixa — confirmar tendência.",
        `SpO₂ ${spo2}% (90–92%).`,
      );
    }
  } else if (spo2 >= 93 && spo2 <= 94) {
    addAlert(
      alerts,
      "SPO2_ATENCAO",
      "LOW",
      "SpO₂ levemente abaixo do ideal.",
      `SpO₂ ${spo2}%.`,
    );
  } else if (spo2 >= 95) {
    notes.push(`SpO₂ ${spo2}% normal.`);
  }

  if (baseline.calibrated && baseline.spo2 != null && baseline.spo2 - spo2 >= 4) {
    addAlert(
      alerts,
      "SPO2_BAIXA",
      "HIGH",
      "Queda de SpO₂ em relação ao basal do paciente.",
      `SpO₂ ${spo2}% vs basal ${Math.round(baseline.spo2)}% (queda ≥ 4 p.p.).`,
    );
  }
}

function evaluateTemperature(vitals: VitalsInput, alerts: ClinicalAlert[], notes: string[]): void {
  const temp = vitals.temperature;
  if (temp <= 0) {
    notes.push("Temperatura ausente na leitura.");
    return;
  }

  if (temp >= 39.5) {
    addAlert(
      alerts,
      "FEBRE_ALTA",
      "CRITICAL",
      "Temperatura muito elevada.",
      `Temperatura ${temp} °C ≥ 39,5 °C.`,
    );
    return;
  }

  if (temp >= 39.0) {
    addAlert(
      alerts,
      "FEBRE_ALTA",
      "HIGH",
      "Febre alta.",
      `Temperatura ${temp} °C.`,
    );
    return;
  }

  if (temp >= 38.0) {
    addAlert(alerts, "FEBRE", "MEDIUM", "Febre.", `Temperatura ${temp} °C.`);
    return;
  }

  if (temp >= 37.5) {
    addAlert(
      alerts,
      "TEMP_ELEVADA",
      "LOW",
      "Temperatura levemente elevada (pele/punho).",
      `Temperatura ${temp} °C.`,
    );
    return;
  }

  if (temp < 35.5) {
    addAlert(
      alerts,
      "TEMP_BAIXA",
      "MEDIUM",
      "Temperatura baixa.",
      `Temperatura ${temp} °C < 35,5 °C.`,
    );
    return;
  }

  if (temp >= 35.8 && temp <= 37.4) {
    notes.push(`Temperatura ${temp} °C normal.`);
  }
}

function evaluateFatigue(vitals: VitalsInput, alerts: ClinicalAlert[], notes: string[]): void {
  const f = vitals.fatigue;
  if (f < 0) return;

  if (f >= 80) {
    addAlert(
      alerts,
      "FADIGA_CRITICA_FUNCIONAL",
      "HIGH",
      "Fadiga muito alta (score da pulseira).",
      `Fadiga ${f}%.`,
    );
  } else if (f >= 60) {
    addAlert(alerts, "FADIGA_ALTA", "MEDIUM", "Fadiga alta.", `Fadiga ${f}%.`);
  } else if (f >= 40) {
    addAlert(alerts, "FADIGA_MODERADA", "LOW", "Fadiga moderada.", `Fadiga ${f}%.`);
  } else {
    notes.push(`Fadiga ${f}% baixa.`);
  }
}

function evaluateHrv(
  vitals: VitalsInput,
  baseline: PatientBaseline,
  context: VitalsContext,
  alerts: ClinicalAlert[],
  notes: string[],
): { hrvDrop: number; hrAboveBaseline: number; fatigueHigh: boolean } {
  const hrv = vitals.hrv;
  let hrvDrop = 0;
  let hrAboveBaseline = 0;
  const fatigueHigh = vitals.fatigue >= 60;

  if (hrv <= 0) {
    notes.push("HRV ausente na leitura.");
    return { hrvDrop, hrAboveBaseline, fatigueHigh };
  }

  if (!baseline.calibrated || baseline.hrv == null) {
    notes.push(
      `HRV ${hrv} ms — baseline individual em calibração (${baseline.sampleCount}/${BASELINE_MIN_SAMPLES} leituras em repouso).`,
    );
    if (context.isResting !== false) {
      notes.push(`HRV ${hrv} ms favorável se medido em repouso.`);
    }
    return { hrvDrop, hrAboveBaseline, fatigueHigh };
  }

  hrvDrop = hrvDropPercent(hrv, baseline.hrv);

  if (hrvDrop <= 15) {
    notes.push(`HRV ${hrv} ms dentro do padrão individual (baseline ~${Math.round(baseline.hrv)} ms).`);
  } else if (hrvDrop <= 25) {
    addAlert(
      alerts,
      "HRV_ATENCAO",
      "LOW",
      "HRV abaixo do baseline — atenção.",
      `HRV ${hrv} ms (~${Math.round(hrvDrop)}% abaixo do basal).`,
    );
  } else if (hrvDrop <= 40) {
    addAlert(
      alerts,
      "HRV_ALERTA",
      "MEDIUM",
      "Queda relevante de HRV vs baseline.",
      `HRV ${hrv} ms (~${Math.round(hrvDrop)}% abaixo do basal).`,
    );
  } else {
    addAlert(
      alerts,
      "HRV_ALERTA",
      "HIGH",
      "HRV bem abaixo do baseline.",
      `HRV ${hrv} ms (>40% abaixo do basal).`,
    );
  }

  if (baseline.heartRate != null) {
    hrAboveBaseline = vitals.heartRate - baseline.heartRate;
  }

  const resting = context.isResting !== false;
  if (
    resting &&
    hrvDrop > 40 &&
    hrAboveBaseline >= 10 &&
    fatigueHigh
  ) {
    addAlert(
      alerts,
      "HRV_CRITICO_FUNCIONAL",
      "HIGH",
      "Padrão de estresse fisiológico (HRV + FC + fadiga).",
      `HRV -${Math.round(hrvDrop)}%, FC +${Math.round(hrAboveBaseline)} bpm vs basal, fadiga ${vitals.fatigue}%.`,
    );
  }

  return { hrvDrop, hrAboveBaseline, fatigueHigh };
}

function evaluateCombinedAlerts(
  vitals: VitalsInput,
  context: VitalsContext,
  baseline: PatientBaseline,
  metrics: { hrvDrop: number; hrAboveBaseline: number; fatigueHigh: boolean },
  alerts: ClinicalAlert[],
): void {
  const resting = context.isResting !== false;

  if (
    baseline.calibrated &&
    metrics.hrvDrop >= 25 &&
    vitals.fatigue >= 60 &&
    resting &&
    metrics.hrAboveBaseline >= 10
  ) {
    addAlert(
      alerts,
      "POSSIVEL_FADIGA_FISIOLOGICA",
      "MEDIUM",
      "Possível fadiga fisiológica (HRV + fadiga + FC).",
      "Combinação: HRV ≥25% abaixo do basal, fadiga ≥60%, FC repouso ≥10 bpm acima do basal.",
    );
  }

  if (
    baseline.calibrated &&
    vitals.temperature >= 37.8 &&
    resting &&
    metrics.hrAboveBaseline >= 10 &&
    metrics.hrvDrop >= 20
  ) {
    addAlert(
      alerts,
      "POSSIVEL_ESTRESSE_INFECCIOSO",
      "MEDIUM",
      "Possível estresse febril/infeccioso.",
      "Temperatura elevada + FC acima do basal + queda de HRV.",
    );
  }

  const spo2Low = vitals.spo2 > 0 && vitals.spo2 <= 92;
  const spo2Drop =
    baseline.calibrated &&
    baseline.spo2 != null &&
    vitals.spo2 > 0 &&
    baseline.spo2 - vitals.spo2 >= 4;

  if (resting && vitals.heartRate >= 100 && (spo2Low || spo2Drop)) {
    addAlert(
      alerts,
      "POSSIVEL_RISCO_RESPIRATORIO",
      "HIGH",
      "Possível risco cardiorrespiratório.",
      "SpO₂ baixa ou em queda + FC elevada em repouso.",
    );
  }

  if (
    resting &&
    (vitals.systolic >= 140 || vitals.diastolic >= 90) &&
    vitals.heartRate >= 100 &&
    metrics.hrvDrop >= 25
  ) {
    addAlert(
      alerts,
      "POSSIVEL_ESTRESSE_CARDIOVASCULAR",
      "HIGH",
      "Possível estresse cardiovascular.",
      "PA elevada + FC repouso alta + HRV abaixo do basal.",
    );
  }

  const criticalImmediate =
    (vitals.spo2 > 0 && vitals.spo2 < 90) ||
    vitals.systolic >= 180 ||
    vitals.diastolic >= 120 ||
    (resting && vitals.heartRate >= 140) ||
    vitals.temperature >= 39.5;

  if (criticalImmediate) {
    addAlert(
      alerts,
      "ALERTA_CRITICO",
      "CRITICAL",
      "Critério de alerta imediato atingido.",
      "SpO₂ crítica, PA de emergência, FC crítica em repouso ou temperatura muito alta.",
    );
  }
}

const SEVERITY_RANK: Record<AlertSeverity, number> = {
  LOW: 1,
  MEDIUM: 2,
  HIGH: 3,
  CRITICAL: 4,
};

const SEVERITY_SCORE: Record<AlertSeverity, number> = {
  LOW: 8,
  MEDIUM: 18,
  HIGH: 35,
  CRITICAL: 60,
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

function computeRiskScore(alerts: ClinicalAlert[]): number {
  if (alerts.length === 0) return 0;
  const actionable = alerts;
  const sum = actionable.reduce((acc, a) => acc + SEVERITY_SCORE[a.severity], 0);
  return Math.min(100, Math.round(sum / actionable.length));
}

function overallStatus(severity: AlertSeverity, alerts: ClinicalAlert[]): OverallStatus {
  const hasCritical = alerts.some(
    (a) => a.severity === "CRITICAL" || a.type === "ALERTA_CRITICO",
  );
  if (hasCritical || severity === "CRITICAL") return "CRITICAL";
  if (severity === "HIGH" || alerts.some((a) => a.severity === "HIGH")) return "ALERT";
  if (severity === "MEDIUM" || alerts.some((a) => a.severity === "MEDIUM")) return "ATTENTION";
  return "STABLE";
}

/** Camadas 1–3: absoluto, tendência (baseline/histórico) e combinado. */
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

  evaluateHeartRate(input.vitals, context, history, alerts, notes);
  evaluateBloodPressure(input.vitals, history, alerts, notes);
  evaluateSpO2(input.vitals, baseline, history, alerts, notes);
  evaluateTemperature(input.vitals, alerts, notes);
  evaluateFatigue(input.vitals, alerts, notes);

  const hrvMetrics = evaluateHrv(input.vitals, baseline, context, alerts, notes);
  evaluateCombinedAlerts(input.vitals, context, baseline, hrvMetrics, alerts);

  const severity = maxSeverity(alerts);
  const riskScore = computeRiskScore(alerts);
  const status = overallStatus(severity, alerts);

  return {
    deviceMac: input.deviceMac,
    measuredAt: input.measuredAt,
    vitals: input.vitals,
    context,
    alerts,
    notes,
    riskScore,
    overallStatus: status,
    severity: alerts.length === 0 ? "LOW" : severity,
    baseline,
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
