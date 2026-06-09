import type { FastifyInstance } from "fastify";
import { ZodError } from "zod";
import {
  getMergedHealthForDevice,
  listPackets,
  savePacket,
} from "../repositories/packet.repository.js";
import { packetPayloadSchema } from "../schemas/packet.schema.js";
import { getDevicesRouteSchema } from "../schemas/device.swagger.js";
import {
  getVitalsReportImageRouteSchema,
  getVitalsReportRouteSchema,
} from "../schemas/report.swagger.js";
import {
  getPacketRouteSchema,
  postPacketRouteSchema,
} from "../schemas/packet.swagger.js";
import {
  buildDevicesOverview,
  enrichWithPatient,
  listRegisteredBracelets,
  resolvePatientForMac,
} from "../services/device-registry.service.js";
import {
  processClinicalAlertsFromDecoded,
  processClinicalAlertsAfterHealthPacket,
} from "../services/clinical-alerts.processor.js";
import {
  decodePacket,
  PacketDecoderError,
  rawHexToBytes,
} from "../services/packet-decoder.service.js";
import { vitalsFromDecodedHealth } from "../services/clinical-alerts.service.js";
import {
  enrichDecodedHealthFromMetrics,
  hasMandatoryVitals,
  mandatoryVitalsFromDecoded,
  MANDATORY_VITALS_ERROR,
} from "../services/vitals-validation.service.js";
import { DEFAULT_VITALS_REPORT_WINDOW_MINUTES } from "../config/teams-report.config.js";
import {
  buildAllVitalsReports,
  buildVitalsReportPngByPatientName,
  buildVitalsReportByPatientName,
} from "../services/teams-report.service.js";

function resolveReportWindowMinutes(value: number | undefined): number {
  return typeof value === "number" && Number.isFinite(value)
    ? Math.min(Math.max(value, 5), 1440)
    : DEFAULT_VITALS_REPORT_WINDOW_MINUTES;
}

export async function braceletRoutes(app: FastifyInstance): Promise<void> {
  app.get("/bracelets/devices", { schema: getDevicesRouteSchema }, async (_request, reply) => {
    const devices = await buildDevicesOverview();
    return reply.status(200).send({ devices });
  });

  app.get(
    "/bracelets/reports/vitals",
    { schema: getVitalsReportRouteSchema },
    async (request, reply) => {
      const query = request.query as { patientName?: string; windowMinutes?: number };
      const windowMinutes = resolveReportWindowMinutes(query.windowMinutes);

      if (query.patientName?.trim()) {
        const report = await buildVitalsReportByPatientName(query.patientName, windowMinutes);
        if (!report) {
          return reply.status(404).send({
            error: "patientName not registered",
            patientName: query.patientName,
          });
        }
        return reply.status(200).send({ report });
      }

      const reports = await buildAllVitalsReports(windowMinutes);
      return reply.status(200).send({ windowMinutes, reports });
    },
  );

  app.get(
    "/bracelets/reports/vitals/image",
    { schema: getVitalsReportImageRouteSchema },
    async (request, reply) => {
      const query = request.query as { patientName?: string; windowMinutes?: number };

      if (!query.patientName?.trim()) {
        return reply.status(400).send({ error: "patientName is required" });
      }

      const windowMinutes = resolveReportWindowMinutes(query.windowMinutes);

      const png = await buildVitalsReportPngByPatientName(query.patientName, windowMinutes);
      if (!png.ok) {
        if (png.reason === "patient_not_found") {
          return reply.status(404).send({
            error: "Paciente não cadastrado",
            patientName: query.patientName,
          });
        }
        return reply.status(500).send({
          error: "Falha ao gerar imagem do relatório",
          patientName: query.patientName,
        });
      }

      const safeName = png.patientName.replace(/[^\w\s-]/g, "").replace(/\s+/g, "-");
      return reply
        .header("Content-Type", "image/png")
        .header("Content-Disposition", `attachment; filename="relatorio-${safeName}.png"`)
        .send(png.buffer);
    },
  );

  app.get("/bracelets/devices/registry", async (_request, reply) => {
    return reply.status(200).send({
      devices: listRegisteredBracelets().map((bracelet) => ({
        deviceMac: bracelet.deviceMac,
        label: bracelet.label,
        patient: bracelet.patient,
      })),
    });
  });

  app.get("/bracelets/packets", { schema: getPacketRouteSchema }, async (request, reply) => {
    const query = request.query as { limit?: number; deviceMac?: string };
    const limit = typeof query.limit === "number" && Number.isFinite(query.limit) ? query.limit : 50;
    const deviceMac = typeof query.deviceMac === "string" ? query.deviceMac : undefined;

    request.log.info({ limit, deviceMac }, "Listing bracelet packets");

    const packets = (await listPackets(limit, deviceMac)).map(enrichWithPatient);

    request.log.info({ limit, deviceMac, count: packets.length }, "Listed bracelet packets");

    return reply.status(200).send({ packets });
  });

  app.post("/bracelets/packets", { schema: postPacketRouteSchema }, async (request, reply) => {
    let payload;

    try {
      payload = packetPayloadSchema.parse(request.body);
    } catch (err) {
      if (err instanceof ZodError) {
        request.log.warn({ details: err.flatten() }, "Bracelet packet validation failed");
        return reply.status(400).send({
          error: "Validation failed",
          details: err.flatten(),
        });
      }
      throw err;
    }

    request.log.info(
      {
        deviceMac: payload.deviceMac,
        packetType: payload.packetType,
        source: payload.source,
        rawHex: payload.rawHex,
      },
      "Received bracelet packet",
    );

    try {
      let { bytes, decoded, crcValid } = decodePacket(payload.packetType, payload.rawHex);

      if (decoded.type === "0x28") {
        let healthDecoded = decoded;
        if (payload.metrics) {
          healthDecoded = enrichDecodedHealthFromMetrics(healthDecoded, payload.metrics);
        }
        if (!hasMandatoryVitals(mandatoryVitalsFromDecoded(healthDecoded))) {
          throw new PacketDecoderError(MANDATORY_VITALS_ERROR);
        }
        decoded = healthDecoded;
      }

      const saved = await savePacket({
        payload,
        bytes,
        crcValid,
        decoded,
      });

      const mergedHealth =
        decoded.type === "0x28" || decoded.type === "0x56"
          ? await getMergedHealthForDevice(payload.deviceMac)
          : null;

      const response = {
        id: saved.id,
        deviceMac: payload.deviceMac,
        packetType: payload.packetType,
        source: payload.source,
        bytes,
        crcValid,
        decoded,
        mergedHealth,
        patient: resolvePatientForMac(payload.deviceMac),
        savedAt: saved.createdAt,
      };

      request.log.info(
        {
          id: saved.id,
          deviceMac: payload.deviceMac,
          packetType: payload.packetType,
          crcValid,
          decoded,
        },
        "Bracelet packet decoded and saved",
      );

      try {
        if (decoded.type === "0x28") {
          await processClinicalAlertsAfterHealthPacket({
            deviceMac: payload.deviceMac,
            source: payload.source,
            packetId: saved.id,
            measuredAt: saved.createdAt,
            vitals: vitalsFromDecodedHealth(decoded),
          });
        } else if (decoded.type === "0x56") {
          await processClinicalAlertsFromDecoded(
            payload.deviceMac,
            payload.source,
            saved.id,
            saved.createdAt,
            decoded,
          );
        }
      } catch (alertErr) {
        request.log.error({ err: alertErr }, "Clinical assessment after packet failed");
      }

      return reply.status(200).send(response);
    } catch (err) {
      if (err instanceof PacketDecoderError) {
        request.log.warn(
          {
            deviceMac: payload.deviceMac,
            packetType: payload.packetType,
            source: payload.source,
            rawHex: payload.rawHex,
            error: err.message,
          },
          "Bracelet packet decode failed",
        );

        let bytes: number[] | undefined;
        try {
          bytes = rawHexToBytes(payload.rawHex);
        } catch {
          bytes = undefined;
        }

        const saved = await savePacket({
          payload,
          bytes,
          crcValid: false,
          decodeError: err.message,
        });

        request.log.info(
          {
            id: saved.id,
            deviceMac: payload.deviceMac,
            packetType: payload.packetType,
            error: err.message,
          },
          "Bracelet packet saved with decode error",
        );

        return reply.status(422).send({
          id: saved.id,
          error: err.message,
          deviceMac: payload.deviceMac,
          packetType: payload.packetType,
          source: payload.source,
          patient: resolvePatientForMac(payload.deviceMac),
          savedAt: saved.createdAt,
        });
      }
      throw err;
    }
  });
}
