import type { FastifyInstance } from "fastify";
import { ZodError } from "zod";
import {
  claimPendingCommand,
  completeCommand,
  enqueueCommand,
  getRuntimeStatus,
  listRecentCommands,
  upsertRuntimeStatus,
} from "../repositories/device-command.repository.js";
import {
  commandAckBodySchema,
  enqueueCommandBodySchema,
  heartbeatBodySchema,
} from "../schemas/device.schema.js";
import { normalizeDeviceId } from "../utils/device-id.js";

function parseDeviceIdParam(raw: string): string {
  try {
    return normalizeDeviceId(raw);
  } catch {
    throw new Error("INVALID_DEVICE_ID");
  }
}

export async function deviceRoutes(app: FastifyInstance): Promise<void> {
  app.post("/devices/:deviceId/commands", async (request, reply) => {
    let deviceId: string;

    try {
      deviceId = parseDeviceIdParam((request.params as { deviceId: string }).deviceId);
    } catch {
      return reply.status(400).send({ error: "Invalid deviceId (use bracelet MAC)" });
    }

    try {
      const body = enqueueCommandBodySchema.parse(request.body);
      const command = await enqueueCommand(deviceId, body.type, body.payload);

      request.log.info({ deviceId, commandId: command.id, type: body.type }, "Command enqueued");

      return reply.status(201).send({ ok: true, command });
    } catch (err) {
      if (err instanceof ZodError) {
        return reply.status(400).send({
          error: "Validation failed",
          details: err.flatten(),
        });
      }
      throw err;
    }
  });

  app.get("/devices/:deviceId/commands/pending", async (request, reply) => {
    let deviceId: string;

    try {
      deviceId = parseDeviceIdParam((request.params as { deviceId: string }).deviceId);
    } catch {
      return reply.status(400).send({ error: "Invalid deviceId (use bracelet MAC)" });
    }

    const command = await claimPendingCommand(deviceId);

    if (!command) {
      return reply.status(200).send({ command: null });
    }

    request.log.info({ deviceId, commandId: command.id, type: command.type }, "Command claimed by ESP32");

    return reply.status(200).send({ command });
  });

  app.post("/devices/:deviceId/commands/:commandId/ack", async (request, reply) => {
    let deviceId: string;

    try {
      deviceId = parseDeviceIdParam((request.params as { deviceId: string }).deviceId);
    } catch {
      return reply.status(400).send({ error: "Invalid deviceId (use bracelet MAC)" });
    }

    const commandId = Number((request.params as { commandId: string }).commandId);

    if (!Number.isInteger(commandId) || commandId <= 0) {
      return reply.status(400).send({ error: "Invalid commandId" });
    }

    try {
      const body = commandAckBodySchema.parse(request.body);
      const updated = await completeCommand(commandId, deviceId, body.status, body.error);

      if (!updated) {
        return reply.status(404).send({ error: "Command not found or already finished" });
      }

      if (body.runtime) {
        await upsertRuntimeStatus(deviceId, {
          status: body.runtime.status,
          error: body.runtime.error,
          readingActive: body.runtime.readingActive,
          sendingData: body.runtime.sendingData,
          wifiConnected: body.runtime.wifiConnected,
          wifiSsid: body.runtime.wifiSsid,
          ip: body.runtime.ip,
          rssi: body.runtime.rssi ?? null,
          heapFree: body.runtime.heapFree ?? null,
        });
      }

      request.log.info(
        { deviceId, commandId, status: body.status },
        "Command acknowledged by ESP32",
      );

      return reply.status(200).send({ ok: true, command: updated });
    } catch (err) {
      if (err instanceof ZodError) {
        return reply.status(400).send({
          error: "Validation failed",
          details: err.flatten(),
        });
      }
      throw err;
    }
  });

  app.post("/devices/:deviceId/heartbeat", async (request, reply) => {
    let deviceId: string;

    try {
      deviceId = parseDeviceIdParam((request.params as { deviceId: string }).deviceId);
    } catch {
      return reply.status(400).send({ error: "Invalid deviceId (use bracelet MAC)" });
    }

    try {
      const body = heartbeatBodySchema.parse(request.body);

      await upsertRuntimeStatus(deviceId, {
        status: body.status,
        error: body.error,
        readingActive: body.readingActive,
        sendingData: body.sendingData,
        wifiConnected: body.wifiConnected,
        wifiSsid: body.wifiSsid,
        ip: body.ip,
        rssi: body.rssi ?? null,
        heapFree: body.heapFree ?? null,
      });

      return reply.status(200).send({ ok: true });
    } catch (err) {
      if (err instanceof ZodError) {
        return reply.status(400).send({
          error: "Validation failed",
          details: err.flatten(),
        });
      }
      throw err;
    }
  });

  app.get("/devices/:deviceId/status", async (request, reply) => {
    let deviceId: string;

    try {
      deviceId = parseDeviceIdParam((request.params as { deviceId: string }).deviceId);
    } catch {
      return reply.status(400).send({ error: "Invalid deviceId (use bracelet MAC)" });
    }

    const runtime = await getRuntimeStatus(deviceId);
    const recentCommands = await listRecentCommands(deviceId, 10);

    return reply.status(200).send({
      deviceId,
      runtime,
      recentCommands,
    });
  });
}
