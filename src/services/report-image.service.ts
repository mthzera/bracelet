import sharp from "sharp";

function escapeXml(text: string): string {
  return text
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

export type VitalsReportImageInput = {
  patientName: string;
  label: string;
  windowMinutes: number;
  summaryText: string;
  overallStatus: string | null;
  chartPngBase64?: string | null;
};

/** Monta PNG único: cabeçalho com resumo e gráfico embutido (quando houver dados). */
export async function composeVitalsReportPng(input: VitalsReportImageInput): Promise<Buffer> {
  const headerLines = [
    `Relatório pulseira — ${input.patientName}`,
    `${input.label} · últimos ${input.windowMinutes} min`,
    "",
    ...input.summaryText.split("\n"),
  ];
  if (input.overallStatus) {
    headerLines.push("", `Status: ${input.overallStatus}`);
  }

  const lineHeight = 22;
  const padding = 24;
  const chartWidth = 700;
  const chartHeight = 320;
  const hasChart = Boolean(input.chartPngBase64);
  const headerHeight = padding + headerLines.length * lineHeight + 16;
  const totalHeight = hasChart ? headerHeight + chartHeight + padding : headerHeight + padding;
  const totalWidth = hasChart ? chartWidth + padding * 2 : 520;

  const textElements = headerLines
    .map((line, index) => {
      const y = padding + 20 + index * lineHeight;
      const weight = index === 0 ? "bold" : "normal";
      const size = index === 0 ? 18 : 14;
      return `<text x="${padding}" y="${y}" font-family="Arial,Helvetica,sans-serif" font-size="${size}" font-weight="${weight}" fill="#0f172a">${escapeXml(line || " ")}</text>`;
    })
    .join("\n");

  const chartElement = hasChart
    ? `<image x="${padding}" y="${headerHeight}" width="${chartWidth}" height="${chartHeight}" href="data:image/png;base64,${input.chartPngBase64}"/>`
    : "";

  const svg = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="${totalWidth}" height="${totalHeight}">
  <rect width="100%" height="100%" fill="#ffffff"/>
  ${textElements}
  ${chartElement}
</svg>`;

  return sharp(Buffer.from(svg)).png().toBuffer();
}
