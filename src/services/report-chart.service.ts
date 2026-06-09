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
          borderColor: "rgb(255, 99, 132)",
          backgroundColor: "rgba(255, 99, 132, 0.1)",
          yAxisID: "y",
          tension: 0.3,
          fill: false,
        },
        {
          label: "SpO2 (%)",
          data: seriesValues(points, (p) => p.spo2),
          borderColor: "rgb(54, 162, 235)",
          backgroundColor: "rgba(54, 162, 235, 0.1)",
          yAxisID: "y1",
          tension: 0.3,
          fill: false,
        },
        {
          label: "Temp (°C)",
          data: seriesValues(points, (p) => p.temperature),
          borderColor: "rgb(255, 206, 86)",
          backgroundColor: "rgba(255, 206, 86, 0.1)",
          yAxisID: "y2",
          tension: 0.3,
          fill: false,
        },
      ],
    },
    options: {
      title: {
        display: true,
        text: `Variação — ${patientName}`,
        fontSize: 14,
      },
      legend: { display: true, position: "bottom" },
      scales: {
        y: {
          type: "linear",
          position: "left",
          title: { display: true, text: "BPM" },
          suggestedMin: 40,
          suggestedMax: 120,
        },
        y1: {
          type: "linear",
          position: "right",
          title: { display: true, text: "SpO2" },
          suggestedMin: 90,
          suggestedMax: 100,
          grid: { drawOnChartArea: false },
        },
        y2: {
          type: "linear",
          position: "right",
          title: { display: true, text: "°C" },
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
    body: JSON.stringify({
      chart: buildVitalsChartConfig(patientName, points),
      width: 700,
      height: 320,
      format: "png",
      backgroundColor: "white",
    }),
  });

  if (!res.ok) {
    const fallback = await fetch(chartUrl);
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

export type VitalsSummary = {
  dataPointCount: number;
  heartRate: { min: number; max: number; latest: number };
  spo2: { min: number; max: number; latest: number };
  temperature: { min: number; max: number; latest: number };
};

function summarizeMetric(values: number[]): { min: number; max: number; latest: number } {
  const valid = values.filter((v) => v > 0);
  if (valid.length === 0) return { min: 0, max: 0, latest: 0 };
  return {
    min: Math.min(...valid),
    max: Math.max(...valid),
    latest: valid[valid.length - 1]!,
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
