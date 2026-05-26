import "dotenv/config";
import Fastify from "fastify";
import { closeDatabase, initDatabase } from "./database/db.js";
import { registerApiWithDocs } from "./plugins/swagger.plugin.js";

async function main(): Promise<void> {
  await initDatabase();

  const app = Fastify({
    logger: true,
  });

  app.addHook("onClose", async () => {
    await closeDatabase();
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
