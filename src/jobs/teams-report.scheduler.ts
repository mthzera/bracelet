import type { FastifyBaseLogger } from "fastify";
import { getTeamsReportConfig } from "../config/teams-report.config.js";
import { runTeamsVitalsReports } from "../services/teams-report.service.js";

let intervalHandle: ReturnType<typeof setInterval> | null = null;
let running = false;

async function tick(logger: FastifyBaseLogger): Promise<void> {
  if (running) {
    logger.warn("Teams report tick skipped — previous run still in progress");
    return;
  }

  running = true;
  try {
    const results = await runTeamsVitalsReports();
    for (const r of results) {
      if (r.sent) {
        logger.info(
          { email: r.email, patient: r.patientName, points: r.dataPointCount },
          "Teams vitals report sent",
        );
      } else if (r.skipped) {
        logger.info(
          { email: r.email, patient: r.patientName, reason: r.reason },
          "Teams vitals report skipped",
        );
      } else {
        logger.error(
          { email: r.email, patient: r.patientName, reason: r.reason },
          "Teams vitals report failed",
        );
      }
    }
  } catch (err) {
    logger.error({ err }, "Teams vitals report batch failed");
  } finally {
    running = false;
  }
}

export function startTeamsReportScheduler(logger: FastifyBaseLogger): void {
  const config = getTeamsReportConfig();
  if (!config.enabled) {
    logger.info("Teams vitals report scheduler disabled (set TEAMS_REPORT_ENABLED=true)");
    return;
  }

  const intervalMs = config.intervalMinutes * 60 * 1000;
  logger.info(
    {
      intervalMinutes: config.intervalMinutes,
      windowMinutes: config.windowMinutes,
      channel: config.flowWebhookUrl ? "power-automate" : "graph-api",
    },
    "Teams vitals report scheduler started",
  );

  // Primeira execução após 2 min (servidor e DB estáveis)
  setTimeout(() => {
    void tick(logger);
  }, 2 * 60 * 1000);

  intervalHandle = setInterval(() => {
    void tick(logger);
  }, intervalMs);
}

export function stopTeamsReportScheduler(): void {
  if (intervalHandle) {
    clearInterval(intervalHandle);
    intervalHandle = null;
  }
}
