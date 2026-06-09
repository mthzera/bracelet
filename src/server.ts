import "dotenv/config";
import fs from "node:fs";
import path from "node:path";
import Fastify from "fastify";
import cors from "@fastify/cors";
import { closeDatabase, initDatabase } from "./database/db.js";
import { stopTeamsReportScheduler, startTeamsReportScheduler } from "./jobs/teams-report.scheduler.js";
import { registerApiWithDocs } from "./plugins/swagger.plugin.js";

function parseAllowedOrigins(value: string | undefined): Set<string> | null {
  if (!value || value.trim() === "*") {
    return null;
  }

  return new Set(
    value
      .split(",")
      .map((origin) => origin.trim().replace(/\/$/, ""))
      .filter(Boolean),
  );
}

async function main(): Promise<void> {
  await initDatabase();

  const app = Fastify({
    logger: true,
    ajv: {
      customOptions: {
        // OpenAPI usa `example`; AJV strict não reconhece essa keyword.
        strict: false,
      },
    },
  });

  app.addHook("onClose", async () => {
    stopTeamsReportScheduler();
    await closeDatabase();
  });

  const allowedOrigins = parseAllowedOrigins(process.env.FRONTEND_ORIGIN);
  await app.register(cors, {
    origin: (origin, callback) => {
      if (!origin || !allowedOrigins) {
        callback(null, true);
        return;
      }

      const normalized = origin.replace(/\/$/, "");
      if (allowedOrigins.has(normalized)) {
        callback(null, origin);
        return;
      }

      // Dashboard local (Live Server, Vite, etc.) em localhost
      try {
        const host = new URL(normalized).hostname;
        if (host === "localhost" || host === "127.0.0.1") {
          callback(null, origin);
          return;
        }
      } catch {
        callback(null, false);
        return;
      }

      callback(null, false);
    },
  });

  app.get("/health", async () => ({ status: "ok" }));

  app.get("/", async (_req, reply) => reply.redirect("/dashboard"));

  app.get("/dashboard", async (_req, reply) => {
    const html = fs.readFileSync(
      path.join(process.cwd(), "public", "dashboard.html"),
      "utf-8",
    );
    reply.type("text/html").send(html);
  });

  await app.register(registerApiWithDocs);

  const shutdown = async () => {
    await app.close();
    await closeDatabase();
    process.exit(0);
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);

  const port = Number(process.env.PORT) || 3000;
  const host = "0.0.0.0";

  await app.listen({ port, host });
  app.log.info({ port, host }, "Server listening");

  startTeamsReportScheduler(app.log);
}

main();
