/** Janela padrão dos relatórios de vitais (API manual + Teams). */
export const DEFAULT_VITALS_REPORT_WINDOW_MINUTES = 60;

export type TeamsReportConfig = {
  enabled: boolean;
  intervalMinutes: number;
  windowMinutes: number;
  /** Power Automate — HTTP trigger que posta no chat Teams pelo e-mail do destinatário */
  flowWebhookUrl: string | null;
  /** Microsoft Graph (alternativa ao Power Automate) */
  graph: {
    tenantId: string;
    clientId: string;
    clientSecret: string;
    senderEmail: string;
  } | null;
};

function parsePositiveInt(value: string | undefined, fallback: number): number {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : fallback;
}

export function getTeamsReportConfig(): TeamsReportConfig {
  const tenantId = process.env.TEAMS_TENANT_ID?.trim() ?? "";
  const clientId = process.env.TEAMS_CLIENT_ID?.trim() ?? "";
  const clientSecret = process.env.TEAMS_CLIENT_SECRET?.trim() ?? "";
  const senderEmail = process.env.TEAMS_SENDER_EMAIL?.trim() ?? "";

  const graphConfigured =
    tenantId.length > 0 &&
    clientId.length > 0 &&
    clientSecret.length > 0 &&
    senderEmail.length > 0;

  const flowWebhookUrl = process.env.TEAMS_FLOW_WEBHOOK_URL?.trim() || null;

  const explicitlyEnabled = process.env.TEAMS_REPORT_ENABLED === "true";
  const enabled = explicitlyEnabled && (flowWebhookUrl !== null || graphConfigured);

  return {
    enabled,
    intervalMinutes: parsePositiveInt(process.env.TEAMS_REPORT_INTERVAL_MINUTES, 30),
    windowMinutes: parsePositiveInt(
      process.env.TEAMS_REPORT_WINDOW_MINUTES,
      DEFAULT_VITALS_REPORT_WINDOW_MINUTES,
    ),
    flowWebhookUrl,
    graph: graphConfigured
      ? { tenantId, clientId, clientSecret, senderEmail }
      : null,
  };
}
