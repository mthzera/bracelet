import type { FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import { ZodError } from "zod";
import {
  gatewayConfigBodySchema,
  gatewayStatusSchema,
  gatewayUrlQuerySchema,
} from "../schemas/gateway.schema.js";
import {
  getGatewayConfigRouteSchema,
  getGatewayInfoRouteSchema,
  getGatewayStatusRouteSchema,
  postGatewayConfigRouteSchema,
  postGatewayResetConfigRouteSchema,
  postGatewayStartRouteSchema,
  postGatewayStopRouteSchema,
} from "../schemas/gateway.swagger.js";
import {
  GatewayNotConfiguredError,
  GatewayUnreachableError,
  getConfiguredGatewayUrl,
  proxyToGateway,
  sendGatewayProxy,
  type GatewayReply,
} from "../services/gateway.service.js";

function asGatewayReply(reply: FastifyReply): GatewayReply {
  return reply as unknown as GatewayReply;
}

const GATEWAY_ENDPOINTS: Record<string, string> = {
  "GET /gateway": "API discovery (this service)",
  "GET /gateway/status": "ESP32 reading state, Wi-Fi, captures",
  "GET /gateway/config": "Current ESP32 config (from status)",
  "POST /gateway/config": "Update Wi-Fi, API URL, bracelet MAC, timeouts",
  "PUT /gateway/config": "Same as POST /gateway/config",
  "POST /gateway/start": "Start full BLE cycle (0x13 + 5× 0x28)",
  "POST /gateway/stop": "Cancel active reading",
  "POST /gateway/reset-config": "Clear NVS and reboot ESP32",
};

function gatewayUrlFromRequest(request: FastifyRequest): string | undefined {
  const query = gatewayUrlQuerySchema.safeParse(request.query);
  return query.success ? query.data.gatewayUrl : undefined;
}

function handleGatewayRouteError(
  request: FastifyRequest,
  reply: FastifyReply,
  err: unknown,
): unknown {
  if (err instanceof GatewayNotConfiguredError) {
    return reply.status(503).send({ error: err.message });
  }

  if (err instanceof GatewayUnreachableError) {
    request.log.warn({ err: err.message }, "ESP32 gateway unreachable");
    return reply.status(502).send({
      error: "Failed to reach ESP32 gateway",
      details: err.message,
    });
  }

  throw err;
}

async function handleGatewayConfig(
  request: FastifyRequest,
  reply: FastifyReply,
): Promise<unknown> {
  try {
    const body = gatewayConfigBodySchema.parse(request.body);
    const result = await proxyToGateway(
      "/config",
      {
        method: "POST",
        body: JSON.stringify(body),
      },
      gatewayUrlFromRequest(request),
    );
    return sendGatewayProxy(asGatewayReply(reply), result);
  } catch (err) {
    if (err instanceof ZodError) {
      return reply.status(400).send({
        error: "Validation failed",
        details: err.flatten(),
      });
    }
    return handleGatewayRouteError(request, reply, err);
  }
}

export async function gatewayRoutes(app: FastifyInstance): Promise<void> {
  app.get(
    "/gateway",
    { schema: getGatewayInfoRouteSchema },
    async (_request, reply) => {
      const gatewayUrl = getConfiguredGatewayUrl();

      return reply.status(200).send({
        name: "Bracelet API — ESP32 Gateway Proxy",
        configured: gatewayUrl !== null,
        gatewayUrl,
        endpoints: GATEWAY_ENDPOINTS,
        esp32LocalEndpoints: {
          "GET /": "ESP32 discovery",
          "GET /status": "Full status JSON",
          "POST /config": "Persist config to NVS",
          "POST /start": "Start reading",
          "POST /stop": "Stop reading",
          "POST /reset-config": "Factory reset NVS",
        },
        env: {
          ESP32_GATEWAY_URL: "Base URL of ESP32 HTTP server (required for proxy)",
          ESP32_GATEWAY_TIMEOUT_MS: "Proxy timeout in ms (default 120000)",
        },
      });
    },
  );

  app.get(
    "/gateway/status",
    { schema: getGatewayStatusRouteSchema },
    async (request, reply) => {
      try {
        const result = await proxyToGateway(
          "/status",
          { method: "GET" },
          gatewayUrlFromRequest(request),
        );
        return sendGatewayProxy(asGatewayReply(reply), result);
      } catch (err) {
        return handleGatewayRouteError(request, reply, err);
      }
    },
  );

  app.get(
    "/gateway/config",
    { schema: getGatewayConfigRouteSchema },
    async (request, reply) => {
      try {
        const result = await proxyToGateway(
          "/status",
          { method: "GET" },
          gatewayUrlFromRequest(request),
        );

        if (result.status !== 200) {
          return sendGatewayProxy(asGatewayReply(reply), result);
        }

        const status = gatewayStatusSchema.parse(result.json);

        return reply.status(200).send({
          wifiSsid: status.wifiSsid ?? "",
          apiUrl: status.apiUrl ?? "",
          deviceMac: status.deviceMac ?? "",
          scanTimeoutMs: status.scanTimeoutMs,
          ip: status.ip ?? "",
          wifiConnected: status.wifiConnected ?? false,
        });
      } catch (err) {
        if (err instanceof ZodError) {
          return reply.status(502).send({
            error: "ESP32 returned unexpected status format",
            details: err.flatten(),
          });
        }
        return handleGatewayRouteError(request, reply, err);
      }
    },
  );

  app.post(
    "/gateway/config",
    { schema: postGatewayConfigRouteSchema },
    handleGatewayConfig,
  );

  app.put(
    "/gateway/config",
    { schema: postGatewayConfigRouteSchema },
    handleGatewayConfig,
  );

  app.post(
    "/gateway/start",
    { schema: postGatewayStartRouteSchema },
    async (request, reply) => {
      try {
        const result = await proxyToGateway(
          "/start",
          { method: "POST", body: "{}" },
          gatewayUrlFromRequest(request),
        );
        return sendGatewayProxy(asGatewayReply(reply), result);
      } catch (err) {
        return handleGatewayRouteError(request, reply, err);
      }
    },
  );

  app.post(
    "/gateway/stop",
    { schema: postGatewayStopRouteSchema },
    async (request, reply) => {
      try {
        const result = await proxyToGateway(
          "/stop",
          { method: "POST", body: "{}" },
          gatewayUrlFromRequest(request),
        );
        return sendGatewayProxy(asGatewayReply(reply), result);
      } catch (err) {
        return handleGatewayRouteError(request, reply, err);
      }
    },
  );

  app.post(
    "/gateway/reset-config",
    { schema: postGatewayResetConfigRouteSchema },
    async (request, reply) => {
      try {
        const result = await proxyToGateway(
          "/reset-config",
          { method: "POST", body: "{}" },
          gatewayUrlFromRequest(request),
        );
        return sendGatewayProxy(asGatewayReply(reply), result);
      } catch (err) {
        return handleGatewayRouteError(request, reply, err);
      }
    },
  );
}
