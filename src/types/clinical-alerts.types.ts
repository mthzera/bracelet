export type AlertSeverity = "LOW" | "MEDIUM" | "HIGH" | "CRITICAL";

export type OverallStatus = "STABLE" | "ATTENTION" | "ALERT" | "CRITICAL";

export type ClinicalAlertType =
  | "BRADICARDIA_LEVE"
  | "TAQUICARDIA_LEVE"
  | "FC_ALERTA"
  | "FC_CRITICA"
  | "PA_ELEVADA"
  | "PA_ESTAGIO_1"
  | "PA_ESTAGIO_2"
  | "PA_CRITICA"
  | "SPO2_ATENCAO"
  | "SPO2_BAIXA"
  | "SPO2_CRITICA"
  | "TEMP_ELEVADA"
  | "FEBRE"
  | "FEBRE_ALTA"
  | "TEMP_BAIXA"
  | "HRV_OK"
  | "HRV_ATENCAO"
  | "HRV_ALERTA"
  | "HRV_CRITICO_FUNCIONAL"
  | "FADIGA_BAIXA"
  | "FADIGA_MODERADA"
  | "FADIGA_ALTA"
  | "FADIGA_CRITICA_FUNCIONAL"
  | "POSSIVEL_FADIGA_FISIOLOGICA"
  | "POSSIVEL_ESTRESSE_INFECCIOSO"
  | "POSSIVEL_RISCO_RESPIRATORIO"
  | "POSSIVEL_ESTRESSE_CARDIOVASCULAR"
  | "ALERTA_CRITICO";

export type VitalsInput = {
  heartRate: number;
  systolic: number;
  diastolic: number;
  temperature: number;
  spo2: number;
  hrv: number;
  fatigue: number;
};

export type VitalsContext = {
  isResting?: boolean;
  signalQuality?: "good" | "fair" | "poor" | "unknown";
  source?: string;
};

export type ClinicalAlert = {
  type: ClinicalAlertType;
  severity: AlertSeverity;
  message: string;
  reason?: string;
};

export type PatientBaseline = {
  hrv: number | null;
  heartRate: number | null;
  fatigue: number | null;
  spo2: number | null;
  sampleCount: number;
  calibrated: boolean;
};

export type ClinicalAssessment = {
  deviceMac: string;
  measuredAt: string;
  vitals: VitalsInput;
  context: VitalsContext;
  alerts: ClinicalAlert[];
  notes: string[];
  riskScore: number;
  overallStatus: OverallStatus;
  severity: AlertSeverity;
  baseline: PatientBaseline;
  disclaimer: string;
};

export type RecentVitalsSnapshot = {
  measuredAt: string;
  vitals: VitalsInput;
  context: VitalsContext;
};
