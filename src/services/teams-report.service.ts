import { getTeamsReportConfig } from "../config/teams-report.config.js";
import {
  findBraceletByPatientName,
  TEST_BRACELETS,
  type PatientInfo,
} from "../config/test-bracelets.js";
import { getLatestClinicalAssessment } from "../repositories/clinical-alerts.repository.js";
import {
  getVitalsHistoryForDevice,
  type VitalsHistoryPoint,
} from "../repositories/vitals-history.repository.js";
import { composeVitalsReportPng } from "./report-image.service.js";
import {
  buildVitalsChartUrl,
  fetchVitalsChartImage,
  summarizeVitals,
  type VitalsSummary,
} from "./report-chart.service.js";
import {
  sendTeamsReportViaFlowWebhook,
  sendTeamsReportViaGraph,
  type TeamsReportPayload,
} from "./teams-graph.service.js";

export type PatientReportResult = {
  deviceMac: string;
  patientName: string;
  email: string;
  sent: boolean;
  skipped: boolean;
  reason?: string;
  dataPointCount: number;
};

export type VitalsReport = {
  deviceMac: string;
  label: string;
  patient: PatientInfo & { email: string };
  windowMinutes: number;
  generatedAt: string;
  dataPointCount: number;
  summary: VitalsSummary;
  summaryText: string;
  chartImageUrl: string | null;
  /** PNG completo (resumo + gráfico) em base64 — salve como .png ou anexe no Teams */
  chartImageBase64: string | null;
  chartImageMimeType: "image/png" | null;
  overallStatus: string | null;
  history: VitalsHistoryPoint[];
  /** Texto pronto para colar no chat do Teams */
  teamsMessage: string;
};

function formatSummary(summary: VitalsSummary): string {
  const hr = summary.heartRate;
  const spo2 = summary.spo2;
  const temp = summary.temperature;

  if (summary.dataPointCount === 0) {
    return "Sem medições no período.";
  }

  return [
    `Leituras: ${summary.dataPointCount}`,
    `BPM: ${hr.latest} (min ${hr.min} / max ${hr.max})`,
    `SpO2: ${spo2.latest}% (min ${spo2.min}% / max ${spo2.max}%)`,
    `Temp: ${temp.latest}°C (min ${temp.min}°C / max ${temp.max}°C)`,
  ].join("\n");
}

function buildTeamsMessage(
  patientName: string,
  label: string,
  windowMinutes: number,
  summaryText: string,
  overallStatus: string | null,
): string {
  const lines = [
    `📊 Relatório pulseira — ${patientName}`,
    `${label} · últimos ${windowMinutes} min`,
    "",
    summaryText,
  ];
  if (overallStatus) lines.push("", `Status: ${overallStatus}`);
  return lines.join("\n");
}

async function buildReportPngBuffer(
  patientName: string,
  label: string,
  windowMinutes: number,
  summaryText: string,
  overallStatus: string | null,
  chartBase64: string,
): Promise<Buffer> {
  return composeVitalsReportPng({
    patientName,
    label,
    windowMinutes,
    summaryText,
    overallStatus,
    chartPngBase64: chartBase64,
  });
}

export async function buildVitalsReport(
  deviceMac: string,
  label: string,
  patient: PatientInfo,
  windowMinutes: number,
): Promise<VitalsReport> {
  const points = await getVitalsHistoryForDevice(deviceMac, windowMinutes);
  const summary = summarizeVitals(points);
  const latestAssessment = await getLatestClinicalAssessment(deviceMac);
  const summaryText = formatSummary(summary);
  const chartImage = await fetchVitalsChartImage(patient.patientName, points);
  const chartImageUrl = chartImage?.url ?? buildVitalsChartUrl(patient.patientName, points);
  const overallStatus = latestAssessment?.overallStatus ?? null;

  let reportPngBase64: string | null = null;
  if (chartImage?.base64) {
    const reportBuffer = await buildReportPngBuffer(
      patient.patientName,
      label,
      windowMinutes,
      summaryText,
      overallStatus,
      chartImage.base64,
    );
    reportPngBase64 = reportBuffer.toString("base64");
  }

  return {
    deviceMac,
    label,
    patient: { ...patient, email: patient.email },
    windowMinutes,
    generatedAt: new Date().toISOString(),
    dataPointCount: summary.dataPointCount,
    summary,
    summaryText,
    chartImageUrl,
    chartImageBase64: reportPngBase64,
    chartImageMimeType: reportPngBase64 ? "image/png" : null,
    overallStatus,
    history: points,
    teamsMessage: buildTeamsMessage(
      patient.patientName,
      label,
      windowMinutes,
      summaryText,
      overallStatus,
    ),
  };
}

export async function buildVitalsReportPngByPatientName(
  patientName: string,
  windowMinutes: number,
): Promise<{ buffer: Buffer; patientName: string } | null> {
  const report = await buildVitalsReportByPatientName(patientName, windowMinutes);
  if (!report?.chartImageBase64) return null;

  return {
    patientName: report.patient.patientName,
    buffer: Buffer.from(report.chartImageBase64, "base64"),
  };
}

export async function buildAllVitalsReports(windowMinutes: number): Promise<VitalsReport[]> {
  return Promise.all(
    TEST_BRACELETS.map((b) => buildVitalsReport(b.deviceMac, b.label, b.patient, windowMinutes)),
  );
}

export async function buildVitalsReportByMac(
  deviceMac: string,
  windowMinutes: number,
): Promise<VitalsReport | null> {
  const bracelet = TEST_BRACELETS.find(
    (b) => b.deviceMac.toUpperCase() === deviceMac.trim().toUpperCase(),
  );
  if (!bracelet) return null;
  return buildVitalsReport(bracelet.deviceMac, bracelet.label, bracelet.patient, windowMinutes);
}

export async function buildVitalsReportByPatientName(
  patientName: string,
  windowMinutes: number,
): Promise<VitalsReport | null> {
  const bracelet = findBraceletByPatientName(patientName);
  if (!bracelet) return null;
  return buildVitalsReport(bracelet.deviceMac, bracelet.label, bracelet.patient, windowMinutes);
}

async function buildPayload(
  deviceMac: string,
  label: string,
  patientName: string,
  email: string,
  windowMinutes: number,
): Promise<{ payload: TeamsReportPayload; dataPointCount: number }> {
  const bracelet = TEST_BRACELETS.find((b) => b.deviceMac === deviceMac);
  const report = await buildVitalsReport(
    deviceMac,
    label,
    bracelet?.patient ?? {
      patientId: "",
      patientName,
      age: 0,
      email,
    },
    windowMinutes,
  );

  const payload: TeamsReportPayload = {
    recipientEmail: email,
    patientName,
    braceletLabel: label,
    windowMinutes,
    chartImageUrl: report.chartImageUrl,
    summaryText: report.summaryText,
    overallStatus: report.overallStatus,
  };

  return { payload, dataPointCount: report.dataPointCount };
}

export async function sendPatientTeamsReport(
  deviceMac: string,
  label: string,
  patientName: string,
  email: string,
  windowMinutes: number,
): Promise<PatientReportResult> {
  const config = getTeamsReportConfig();

  if (!config.enabled) {
    return {
      deviceMac,
      patientName,
      email,
      sent: false,
      skipped: true,
      reason: "Teams report disabled",
      dataPointCount: 0,
    };
  }

  const { payload, dataPointCount } = await buildPayload(
    deviceMac,
    label,
    patientName,
    email,
    windowMinutes,
  );

  if (dataPointCount === 0) {
    return {
      deviceMac,
      patientName,
      email,
      sent: false,
      skipped: true,
      reason: "Sem medições no período",
      dataPointCount: 0,
    };
  }

  if (config.flowWebhookUrl) {
    await sendTeamsReportViaFlowWebhook(config.flowWebhookUrl, payload);
  } else if (config.graph) {
    await sendTeamsReportViaGraph(config.graph, payload);
  } else {
    return {
      deviceMac,
      patientName,
      email,
      sent: false,
      skipped: true,
      reason: "Nenhum canal Teams configurado",
      dataPointCount,
    };
  }

  return {
    deviceMac,
    patientName,
    email,
    sent: true,
    skipped: false,
    dataPointCount,
  };
}

export async function runTeamsVitalsReports(): Promise<PatientReportResult[]> {
  const config = getTeamsReportConfig();
  if (!config.enabled) return [];

  const results: PatientReportResult[] = [];

  for (const bracelet of TEST_BRACELETS) {
    try {
      const result = await sendPatientTeamsReport(
        bracelet.deviceMac,
        bracelet.label,
        bracelet.patient.patientName,
        bracelet.patient.email,
        config.windowMinutes,
      );
      results.push(result);
    } catch (err) {
      results.push({
        deviceMac: bracelet.deviceMac,
        patientName: bracelet.patient.patientName,
        email: bracelet.patient.email,
        sent: false,
        skipped: false,
        reason: err instanceof Error ? err.message : String(err),
        dataPointCount: 0,
      });
    }
  }

  return results;
}
