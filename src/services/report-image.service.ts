import sharp from "sharp";
import type { VitalsSummary } from "./report-chart.service.js";

const REPORT_WIDTH = 748;
const CHART_WIDTH = 700;
const CHART_HEIGHT = 320;
const PADDING = 24;

function escapeXml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function statusColors(status: string | null): { bg: string; fg: string; border: string } {
  const key = (status ?? "").toUpperCase();
  if (key.includes("CRITICAL") || key.includes("URGENT") || key.includes("ALERT")) {
    return { bg: "#fee2e2", fg: "#991b1b", border: "#f87171" };
  }
  if (key.includes("ATTENTION") || key.includes("WARNING")) {
    return { bg: "#ffedd5", fg: "#9a3412", border: "#fb923c" };
  }
  if (key.includes("OK") || key.includes("NORMAL") || key.includes("STABLE")) {
    return { bg: "#dcfce7", fg: "#166534", border: "#4ade80" };
  }
  return { bg: "#e2e8f0", fg: "#334155", border: "#94a3b8" };
}

function formatWindowLabel(windowMinutes: number): string {
  return windowMinutes === 60 ? "última hora" : `últimos ${windowMinutes} min`;
}

function buildHeaderSvg(input: {
  patientName: string;
  label: string;
  windowMinutes: number;
  summary: VitalsSummary;
  overallStatus: string | null;
}): { svg: string; height: number } {
  const bannerH = 78;
  const cardsTop = bannerH + 20;
  const cardH = 128;
  const cardW = 220;
  const cardGap = 14;
  const statusH = input.overallStatus ? 42 : 0;
  const footerPad = 20;
  const height = cardsTop + cardH + statusH + footerPad;

  const hasData = input.summary.dataPointCount > 0;
  const status = statusColors(input.overallStatus);

  const metricCard = (
    x: number,
    title: string,
    latestLine: string,
    avgLine: string,
    rangeLine: string,
    accent: string,
    tint: string,
  ) => `
    <rect x="${x}" y="${cardsTop}" width="${cardW}" height="${cardH}" rx="12" fill="${tint}" stroke="${accent}" stroke-width="2"/>
    <text x="${x + 16}" y="${cardsTop + 22}" font-family="Arial,Helvetica,sans-serif" font-size="13" font-weight="bold" fill="${accent}">${escapeXml(title)}</text>
    <text x="${x + 16}" y="${cardsTop + 46}" font-family="Arial,Helvetica,sans-serif" font-size="12" fill="#64748b">Última medição</text>
    <text x="${x + 16}" y="${cardsTop + 66}" font-family="Arial,Helvetica,sans-serif" font-size="20" font-weight="bold" fill="#0f172a">${escapeXml(latestLine)}</text>
    <text x="${x + 16}" y="${cardsTop + 84}" font-family="Arial,Helvetica,sans-serif" font-size="12" fill="#64748b">Média do período</text>
    <text x="${x + 16}" y="${cardsTop + 102}" font-family="Arial,Helvetica,sans-serif" font-size="15" font-weight="bold" fill="#334155">${escapeXml(avgLine)}</text>
    <text x="${x + 16}" y="${cardsTop + 120}" font-family="Arial,Helvetica,sans-serif" font-size="11" fill="#94a3b8">${escapeXml(rangeLine)}</text>
  `;

  const hr = input.summary.heartRate;
  const spo2 = input.summary.spo2;
  const temp = input.summary.temperature;

  const cards = hasData
    ? [
        metricCard(
          PADDING,
          "BPM",
          String(hr.latest),
          String(Math.round(hr.avg)),
          `menor ${hr.min} · maior ${hr.max}`,
          "#e11d48",
          "#fff1f2",
        ),
        metricCard(
          PADDING + cardW + cardGap,
          "SpO2",
          `${spo2.latest}%`,
          `${Math.round(spo2.avg)}%`,
          `menor ${spo2.min}% · maior ${spo2.max}%`,
          "#0284c7",
          "#eff6ff",
        ),
        metricCard(
          PADDING + (cardW + cardGap) * 2,
          "Temperatura",
          `${temp.latest.toFixed(1)}°C`,
          `${temp.avg.toFixed(1)}°C`,
          `menor ${temp.min.toFixed(1)}°C · maior ${temp.max.toFixed(1)}°C`,
          "#d97706",
          "#fffbeb",
        ),
      ].join("")
    : `<text x="${PADDING}" y="${cardsTop + 40}" font-family="Arial,Helvetica,sans-serif" font-size="16" fill="#64748b">Sem medições no período.</text>`;

  const readingsBadge = hasData
    ? `<rect x="${REPORT_WIDTH - PADDING - 108}" y="22" width="108" height="34" rx="17" fill="rgba(255,255,255,0.22)"/>
       <text x="${REPORT_WIDTH - PADDING - 54}" y="44" text-anchor="middle" font-family="Arial,Helvetica,sans-serif" font-size="13" font-weight="bold" fill="#ffffff">${input.summary.dataPointCount} leituras</text>`
    : "";

  const statusBlock = input.overallStatus
    ? `
    <rect x="${PADDING}" y="${cardsTop + cardH + 14}" width="${REPORT_WIDTH - PADDING * 2}" height="34" rx="10" fill="${status.bg}" stroke="${status.border}" stroke-width="1.5"/>
    <text x="${PADDING + 16}" y="${cardsTop + cardH + 36}" font-family="Arial,Helvetica,sans-serif" font-size="14" font-weight="bold" fill="${status.fg}">Status clínico: ${escapeXml(input.overallStatus)}</text>
  `
    : "";

  const svg = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="${REPORT_WIDTH}" height="${height}">
  <defs>
    <linearGradient id="banner" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#0ea5e9"/>
      <stop offset="55%" stop-color="#6366f1"/>
      <stop offset="100%" stop-color="#8b5cf6"/>
    </linearGradient>
  </defs>
  <rect width="100%" height="100%" fill="#f8fafc"/>
  <rect x="0" y="0" width="${REPORT_WIDTH}" height="${bannerH}" fill="url(#banner)"/>
  <text x="${PADDING}" y="34" font-family="Arial,Helvetica,sans-serif" font-size="20" font-weight="bold" fill="#ffffff">Relatório pulseira — ${escapeXml(input.patientName)}</text>
  <text x="${PADDING}" y="58" font-family="Arial,Helvetica,sans-serif" font-size="13" fill="#e0e7ff">${escapeXml(input.label)} · ${escapeXml(formatWindowLabel(input.windowMinutes))}</text>
  ${readingsBadge}
  ${cards}
  ${statusBlock}
</svg>`;

  return { svg, height };
}

export type VitalsReportImageInput = {
  patientName: string;
  label: string;
  windowMinutes: number;
  summary: VitalsSummary;
  overallStatus: string | null;
  chartPngBase64?: string | null;
};

/** Monta PNG: cabeçalho colorido + gráfico composto via sharp (base64 no SVG falha no librsvg). */
export async function composeVitalsReportPng(input: VitalsReportImageInput): Promise<Buffer> {
  const { svg: headerSvg, height: headerHeight } = buildHeaderSvg(input);
  const headerPng = await sharp(Buffer.from(headerSvg)).png().toBuffer();

  const hasChart = Boolean(input.chartPngBase64);
  if (!hasChart) {
    return headerPng;
  }

  const chartBuf = Buffer.from(input.chartPngBase64!, "base64");
  const chartTop = headerHeight + 8;
  const totalHeight = chartTop + CHART_HEIGHT + PADDING;

  return sharp({
    create: {
      width: REPORT_WIDTH,
      height: totalHeight,
      channels: 4,
      background: { r: 248, g: 250, b: 252, alpha: 1 },
    },
  })
    .composite([
      { input: headerPng, top: 0, left: 0 },
      {
        input: await sharp(chartBuf)
          .resize(CHART_WIDTH, CHART_HEIGHT, { fit: "fill" })
          .png()
          .toBuffer(),
        top: chartTop,
        left: PADDING,
      },
    ])
    .png()
    .toBuffer();
}
