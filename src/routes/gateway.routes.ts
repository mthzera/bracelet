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
  cloudEnqueueAction,
  cloudEnqueueConfig,
  cloudGetGatewayConfig,
  cloudGetGatewayStatus,
  resolveDeviceMac,
} from "../services/gateway-cloud.service.js";
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
  "GET /gateway/status": "ESP32 status (cloud queue or LAN proxy)",
  "GET /gateway/config": "ESP32 config (cloud queue or LAN proxy)",
  "POST /gateway/config": "Update config (cloud queue or LAN proxy)",
  "PUT /gateway/config": "Same as POST /gateway/config",
  "POST /gateway/start": "Start BLE cycle",
  "POST /gateway/stop": "Cancel reading",
  "POST /gateway/reset-config": "Reset ESP32 NVS",
};

function gatewayQuery(request: FastifyRequest) {
  return gatewayUrlQuerySchema.safeParse(request.query);
}

function gatewayUrlFromRequest(request: FastifyRequest): string | undefined {
  const query = gatewayQuery(request);
  return query.success ? query.data.gatewayUrl : undefined;
}

function deviceMacFromQuery(request: FastifyRequest): string | undefined {
  const query = gatewayQuery(request);
  return query.success ? query.data.deviceMac : undefined;
}

function useCloudGateway(request: FastifyRequest): boolean {
  return !getConfiguredGatewayUrl() && !gatewayUrlFromRequest(request);
}

function deviceMacRequired(reply: FastifyReply): unknown {
  return reply.status(400).send({
    error: "deviceMac is required in cloud mode",
    hint: "Add ?deviceMac=AA:BB:CC:DD:EE:FF or include deviceMac in the JSON body",
  });
}

function handleDeviceMacError(reply: FastifyReply, err: unknown): unknown {
  if (err instanceof Error && err.message === "DEVICE_MAC_REQUIRED") {
    return deviceMacRequired(reply);
  }

  if (err instanceof Error && err.message === "INVALID_DEVICE_ID") {
    return reply.status(400).send({ error: "Invalid deviceMac (use bracelet MAC)" });
  }

  return null;
}

function handleGatewayRouteError(
  request: FastifyRequest,
  reply: FastifyReply,
  err: unknown,
): unknown {
  if (err instanceof GatewayNotConfiguredError) {
    return reply.status(503).send({
      error: "ESP32_GATEWAY_URL is not set. Use ?deviceMac= for cloud mode or configure ESP32_GATEWAY_URL.",
    });
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

    if (useCloudGateway(request)) {
      let deviceMac: string;
      try {
        deviceMac = resolveDeviceMac(deviceMacFromQuery(request), body.deviceMac);
      } catch (err) {
        const handled = handleDeviceMacError(reply, err);
        if (handled) return handled;
        throw err;
      }

      const result = await cloudEnqueueConfig(deviceMac, body);
      return reply.status(200).send(result);
    }

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
      const cloudMode = !gatewayUrl;

      return reply.status(200).send({
        name: "Bracelet API — ESP32 Gateway",
        configured: gatewayUrl !== null,
        cloudMode,
        gatewayUrl,
        endpoints: GATEWAY_ENDPOINTS,
        cloudHint: cloudMode
          ? "Pass ?deviceMac=AA:BB:CC:DD:EE:FF on /gateway/* routes. ESP32 polls /devices/:deviceId/commands/pending."
          : undefined,
        env: {
          ESP32_GATEWAY_URL: "Optional. LAN proxy to ESP32. If unset, uses cloud command queue.",
          ESP32_GATEWAY_TIMEOUT_MS: "Proxy timeout in ms (default 120000)",
        },
      });
    },
  );

  app.get(
    "/gateway/status",
    { schema: getGatewayStatusRouteSchema },
    async (request, reply) => {
      if (useCloudGateway(request)) {
        try {
          const deviceMac = resolveDeviceMac(deviceMacFromQuery(request));
          const status = await cloudGetGatewayStatus(deviceMac);
          return reply.status(200).send(status);
        } catch (err) {
          const handled = handleDeviceMacError(reply, err);
          if (handled) return handled;
          throw err;
        }
      }

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
      if (useCloudGateway(request)) {
        try {
          const deviceMac = resolveDeviceMac(deviceMacFromQuery(request));
          const config = await cloudGetGatewayConfig(deviceMac);
          return reply.status(200).send(config);
        } catch (err) {
          const handled = handleDeviceMacError(reply, err);
          if (handled) return handled;
          throw err;
        }
      }

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
      if (useCloudGateway(request)) {
        try {
          const body = (request.body ?? {}) as { deviceMac?: string };
          const deviceMac = resolveDeviceMac(deviceMacFromQuery(request), body.deviceMac);
          const result = await cloudEnqueueAction(deviceMac, "start");
          return reply.status(200).send(result);
        } catch (err) {
          const handled = handleDeviceMacError(reply, err);
          if (handled) return handled;
          throw err;
        }
      }

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
      if (useCloudGateway(request)) {
        try {
          const body = (request.body ?? {}) as { deviceMac?: string };
          const deviceMac = resolveDeviceMac(deviceMacFromQuery(request), body.deviceMac);
          const result = await cloudEnqueueAction(deviceMac, "stop");
          return reply.status(200).send(result);
        } catch (err) {
          const handled = handleDeviceMacError(reply, err);
          if (handled) return handled;
          throw err;
        }
      }

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
      if (useCloudGateway(request)) {
        try {
          const body = (request.body ?? {}) as { deviceMac?: string };
          const deviceMac = resolveDeviceMac(deviceMacFromQuery(request), body.deviceMac);
          const result = await cloudEnqueueAction(deviceMac, "reset_config");
          return reply.status(200).send(result);
        } catch (err) {
          const handled = handleDeviceMacError(reply, err);
          if (handled) return handled;
          throw err;
        }
      }

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
