import "dotenv/config";
import Fastify from "fastify";
import cors from "@fastify/cors";
import { closeDatabase, initDatabase } from "./database/db.js";
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
  });

  app.addHook("onClose", async () => {
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
      callback(null, allowedOrigins.has(normalized) ? origin : false);
    },
  });

  app.get("/health", async () => ({ status: "ok" }));

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
}

main();
