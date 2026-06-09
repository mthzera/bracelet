import type { VitalsHistoryPoint } from "../repositories/vitals-history.repository.js";

function formatTimeLabel(iso: string): string {
  const d = new Date(iso);
  return d.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit", hour12: false });
}

function seriesValues(points: VitalsHistoryPoint[], pick: (p: VitalsHistoryPoint) => number): number[] {
  return points.map(pick);
}

function buildVitalsChartConfig(patientName: string, points: VitalsHistoryPoint[]) {
  const labels = points.map((p) => formatTimeLabel(p.measuredAt));

  return {
    type: "line",
    data: {
      labels,
      datasets: [
        {
          label: "BPM",
          data: seriesValues(points, (p) => p.heartRate),
          borderColor: "#e11d48",
          backgroundColor: "rgba(225, 29, 72, 0.15)",
          borderWidth: 3,
          pointBackgroundColor: "#e11d48",
          pointRadius: 4,
          yAxisID: "y",
          tension: 0.35,
          fill: true,
        },
        {
          label: "SpO2 (%)",
          data: seriesValues(points, (p) => p.spo2),
          borderColor: "#0284c7",
          backgroundColor: "rgba(2, 132, 199, 0.12)",
          borderWidth: 3,
          pointBackgroundColor: "#0284c7",
          pointRadius: 4,
          yAxisID: "y1",
          tension: 0.35,
          fill: true,
        },
        {
          label: "Temp (°C)",
          data: seriesValues(points, (p) => p.temperature),
          borderColor: "#d97706",
          backgroundColor: "rgba(217, 119, 6, 0.12)",
          borderWidth: 3,
          pointBackgroundColor: "#d97706",
          pointRadius: 4,
          yAxisID: "y2",
          tension: 0.35,
          fill: true,
        },
      ],
    },
    options: {
      title: {
        display: true,
        text: `Variação — ${patientName}`,
        fontColor: "#0f172a",
        fontSize: 16,
        fontStyle: "bold",
      },
      legend: {
        display: true,
        position: "bottom",
        labels: { fontColor: "#334155", fontSize: 12, usePointStyle: true },
      },
      scales: {
        x: {
          grid: { color: "rgba(148, 163, 184, 0.25)" },
          ticks: { color: "#64748b" },
        },
        y: {
          type: "linear",
          position: "left",
          title: { display: true, text: "BPM", color: "#e11d48" },
          ticks: { color: "#e11d48" },
          suggestedMin: 40,
          suggestedMax: 120,
          grid: { color: "rgba(225, 29, 72, 0.08)" },
        },
        y1: {
          type: "linear",
          position: "right",
          title: { display: true, text: "SpO2", color: "#0284c7" },
          ticks: { color: "#0284c7" },
          suggestedMin: 90,
          suggestedMax: 100,
          grid: { drawOnChartArea: false },
        },
        y2: {
          type: "linear",
          position: "right",
          title: { display: true, text: "°C", color: "#d97706" },
          ticks: { color: "#d97706" },
          suggestedMin: 35,
          suggestedMax: 38,
          grid: { drawOnChartArea: false },
        },
      },
    },
  };
}

export function buildVitalsChartUrl(
  patientName: string,
  points: VitalsHistoryPoint[],
): string | null {
  if (points.length === 0) return null;
  const encoded = encodeURIComponent(JSON.stringify(buildVitalsChartConfig(patientName, points)));
  return `https://quickchart.io/chart?width=700&height=320&format=png&c=${encoded}`;
}

export type VitalsChartImage = {
  url: string;
  base64: string;
  mimeType: "image/png";
  byteLength: number;
};

/** Busca PNG no QuickChart e devolve bytes em base64. */
export async function fetchVitalsChartImage(
  patientName: string,
  points: VitalsHistoryPoint[],
): Promise<VitalsChartImage | null> {
  if (points.length === 0) return null;

  const chartUrl = buildVitalsChartUrl(patientName, points);
  if (!chartUrl) return null;

  const res = await fetch("https://quickchart.io/chart", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    signal: AbortSignal.timeout(20_000),
    body: JSON.stringify({
      chart: buildVitalsChartConfig(patientName, points),
      width: 700,
      height: 320,
      format: "png",
      backgroundColor: "white",
    }),
  });

  if (!res.ok) {
    const fallback = await fetch(chartUrl, { signal: AbortSignal.timeout(20_000) });
    if (!fallback.ok) return null;
    const buf = Buffer.from(await fallback.arrayBuffer());
    return {
      url: chartUrl,
      base64: buf.toString("base64"),
      mimeType: "image/png",
      byteLength: buf.length,
    };
  }

  const buf = Buffer.from(await res.arrayBuffer());
  return {
    url: chartUrl,
    base64: buf.toString("base64"),
    mimeType: "image/png",
    byteLength: buf.length,
  };
}

export type VitalsMetricSummary = {
  min: number;
  max: number;
  latest: number;
  avg: number;
};

export type VitalsSummary = {
  dataPointCount: number;
  heartRate: VitalsMetricSummary;
  spo2: VitalsMetricSummary;
  temperature: VitalsMetricSummary;
};

function summarizeMetric(values: number[]): VitalsMetricSummary {
  const valid = values.filter((v) => v > 0);
  if (valid.length === 0) return { min: 0, max: 0, latest: 0, avg: 0 };
  const sum = valid.reduce((acc, v) => acc + v, 0);
  return {
    min: Math.min(...valid),
    max: Math.max(...valid),
    latest: valid[valid.length - 1]!,
    avg: sum / valid.length,
  };
}

export function summarizeVitals(points: VitalsHistoryPoint[]): VitalsSummary {
  return {
    dataPointCount: points.length,
    heartRate: summarizeMetric(points.map((p) => p.heartRate)),
    spo2: summarizeMetric(points.map((p) => p.spo2)),
    temperature: summarizeMetric(points.map((p) => p.temperature)),
  };
}
