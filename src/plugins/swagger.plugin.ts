import swagger from "@fastify/swagger";
import swaggerUi from "@fastify/swagger-ui";
import type { FastifyInstance } from "fastify";
import { braceletRoutes } from "../routes/bracelet.routes.js";
import { deviceRoutes } from "../routes/device.routes.js";
import { gatewayRoutes } from "../routes/gateway.routes.js";

const openApiConfig = {
  openapi: {
    openapi: "3.0.3" as const,
    info: {
      title: "Bracelet API",
      description:
        "API for receiving and decoding raw BLE packets from a 2208A wristband via ESP32 gateway.",
      version: "1.0.0",
    },
    tags: [
      {
        name: "bracelets",
        description: "BLE bracelet packet ingestion and decoding",
      },
      {
        name: "gateway",
        description: "Proxy to ESP32 local HTTP API (requires ESP32_GATEWAY_URL)",
      },
      {
        name: "devices",
        description: "Cloud command queue — front enqueues, ESP32 polls (works across networks)",
      },
    ],
  },
};

const swaggerUiConfig = {
  routePrefix: "/docs",
  uiConfig: {
    docExpansion: "list",
    deepLinking: true,
  },
} as const;

/** Swagger + routes + UI no mesmo contexto (evita docs vazios). */
export async function registerApiWithDocs(app: FastifyInstance): Promise<void> {
  await app.register(swagger, openApiConfig);
  await app.register(braceletRoutes);
  await app.register(deviceRoutes);
  await app.register(gatewayRoutes);
  await app.register(swaggerUi, swaggerUiConfig);
}
