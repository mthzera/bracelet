import type { FastifyInstance, FastifyBaseLogger } from "fastify";
import { randomUUID } from "node:crypto";
import { ZodError } from "zod";
import {
  getMergedHealthForDevice,
  getLastSnapshotVitalsForDevice,
  listPackets,
  savePacket,
} from "../repositories/packet.repository.js";
import {
  packetBatchPayloadSchema,
  type PacketItem,
  type PacketPayload,
} from "../schemas/packet.schema.js";
import { getDevicesRouteSchema } from "../schemas/device.swagger.js";
import {
  getVitalsReportImageRouteSchema,
  getVitalsReportRouteSchema,
} from "../schemas/report.swagger.js";
import {
  getPacketRouteSchema,
  getPacketSummaryRouteSchema,
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
  processClinicalAlertsFromRecentPackets,
} from "../services/clinical-alerts.processor.js";
import {
  decodePacket,
  PacketDecoderError,
  rawHexToBytes,
  type DecodedHealth,
  type DecodedPacket,
} from "../services/packet-decoder.service.js";
import {
  enrichDecodedHealthFromMetrics,
  hasAnyHealthReading,
} from "../services/vitals-validation.service.js";
import { DEFAULT_VITALS_REPORT_WINDOW_MINUTES } from "../config/teams-report.config.js";
import {
  buildAllVitalsReports,
  buildVitalsReportPngByPatientName,
  buildVitalsReportByPatientName,
} from "../services/teams-report.service.js";
import {
  buildCycleSummaries,
  buildSnapshotsFromPackets,
} from "../services/measurement-snapshot.service.js";
import {
  applySnapshotVitals,
  buildConsolidatedSnapshot,
  SNAPSHOT_VITALS_TYPE,
  sleepFieldsFromMetrics,
  type ConsolidatedVitals,
  type InboundPacket,
} from "../services/vitals-consolidation.service.js";
import { expandPacketsForLegacyRead } from "../services/legacy-packet-read.service.js";

function resolveReportWindowMinutes(value: number | undefined): number {
  return typeof value === "number" && Number.isFinite(value)
    ? Math.min(Math.max(value, 5), 1440)
    : DEFAULT_VITALS_REPORT_WINDOW_MINUTES;
}

type PacketProcessResult =
  | {
      ok: true;
      id: number;
      packetType: string;
      receivedAtMs: number;
      bytes: number[];
      crcValid: boolean;
      decoded: DecodedPacket;
      mergedHealth: DecodedHealth | null;
      savedAt: string;
      staleFields?: string[];
    }
  | {
      ok: false;
      id: number;
      packetType: string;
      receivedAtMs: number;
      error: string;
      savedAt: string;
    };

const STALE_VITAL_KEYS = ["hrv", "fatigue", "systolicPressure", "diastolicPressure"] as const;

type SnapshotExtras = {
  sources?: Record<string, string>;
};

function staleSourceKey(vital: (typeof STALE_VITAL_KEYS)[number]): string {
  if (vital === "systolicPressure" || vital === "diastolicPressure") return "bloodPressure";
  return vital;
}

function detectAndClearStaleVitals(
  vitals: ConsolidatedVitals,
  prevDecoded: { hrv: number | null; fatigue: number | null; systolicPressure: number | null; diastolicPressure: number | null } | null,
  extras?: SnapshotExtras,
): string[] {
  const staleFields: string[] = [];

  for (const key of STALE_VITAL_KEYS) {
    const source = extras?.sources?.[staleSourceKey(key)];
    if (source?.startsWith("stale_duplicate_")) {
      vitals[key] = null;
      staleFields.push(key);
      continue;
    }

    if (
      prevDecoded &&
      vitals[key] !== null &&
      prevDecoded[key] !== null &&
      vitals[key] === prevDecoded[key]
    ) {
      vitals[key] = null;
      staleFields.push(key);
    }
  }

  return staleFields;
}

async function processInboundPacket(
  payload: PacketPayload,
  item: PacketItem,
  log: FastifyBaseLogger,
  ingestionBatchId: string,
  rawSnapshot?: unknown,
): Promise<PacketProcessResult> {
  const base = {
    packetType: item.packetType,
    receivedAtMs: item.receivedAtMs,
  };

  // SNAPSHOT_VITALS não tem rawHex: persiste a leitura consolidada (vinda em
  // `metrics`) como debug/histórico. Não passa pelo decoder de hex.
  if (item.packetType === SNAPSHOT_VITALS_TYPE) {
    const vitals = applySnapshotVitals({ packetType: item.packetType, metrics: item.metrics });

    const prevPacket = await getLastSnapshotVitalsForDevice(payload.deviceMac);
    const prevDecoded = prevPacket?.decoded?.type === "snapshot" ? prevPacket.decoded : null;
    const extras = rawSnapshot as SnapshotExtras | undefined;
    const isTestMode =
      item.metrics?.testMode === true ||
      payload.source === "ESP32_FAST_TEST" ||
      extras?.sources?.bpm?.includes("FAST_TEST") === true;
    const staleFields = isTestMode
      ? []
      : detectAndClearStaleVitals(vitals, prevDecoded, extras);
    const sleepFields = sleepFieldsFromMetrics(item.metrics);

    const decoded: DecodedPacket = { type: "snapshot", ...vitals, ...sleepFields, staleFields };
    const saved = await savePacket({
      payload,
      crcValid: false,
      decoded,
      receivedAtMs: item.receivedAtMs,
      ingestionBatchId,
      rawSnapshot,
    });
    log.info(
      { id: saved.id, deviceMac: payload.deviceMac, vitals, staleFields },
      "SNAPSHOT_VITALS saved",
    );
    return {
      ok: true,
      id: saved.id,
      ...base,
      bytes: [],
      crcValid: false,
      decoded,
      mergedHealth: null,
      savedAt: saved.createdAt,
      staleFields,
    };
  }

  try {
    let { bytes, decoded, crcValid } = decodePacket(item.packetType, item.rawHex ?? "");

    // 0x28 é o "active measurement packet": nunca descartamos por vir com campos
    // zerados — o raw é sempre persistido (rule 1). Só registramos se há alguma
    // leitura útil para decidir se vale acionar a avaliação clínica.
    let healthHasReading = false;
    if (decoded.type === "0x28") {
      let healthDecoded = decoded;
      if (item.metrics) {
        healthDecoded = enrichDecodedHealthFromMetrics(healthDecoded, item.metrics);
      }
      healthHasReading = hasAnyHealthReading(healthDecoded);
      decoded = healthDecoded;
    }

    const saved = await savePacket({
      payload,
      bytes,
      crcValid,
      decoded,
      receivedAtMs: item.receivedAtMs,
      ingestionBatchId,
    });

    const isHealthPacket = decoded.type === "0x28" || decoded.type === "0x56";
    const mergedHealth = isHealthPacket
      ? await getMergedHealthForDevice(payload.deviceMac)
      : null;

    if (decoded.type === "0x28") {
      log.info(
        {
          id: saved.id,
          deviceMac: payload.deviceMac,
          mode: decoded.measurementMode,
          heartRate: decoded.heartRate,
          spo2: decoded.spo2,
          hrv: decoded.hrv,
          temperature: decoded.temperature,
        },
        "0x28 vital saved",
      );
    } else if (decoded.type === "0x56") {
      log.info(
        {
          id: saved.id,
          deviceMac: payload.deviceMac,
          hrv: decoded.hrv,
          fatigue: decoded.fatigue,
        },
        "0x56 HRV saved",
      );
    } else if (decoded.type === "0x53") {
      log.info(
        {
          id: saved.id,
          deviceMac: payload.deviceMac,
          date: decoded.date,
          sleepMinutes: decoded.sleepMinutes,
        },
        "0x53 sleep saved",
      );
    }

    try {
      if (decoded.type === "0x28" && healthHasReading) {
        await processClinicalAlertsFromRecentPackets(
          payload.deviceMac,
          payload.source,
          saved.id,
          saved.createdAt,
        );
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
      log.error({ err: alertErr }, "Clinical assessment after packet failed");
    }

    return {
      ok: true,
      id: saved.id,
      ...base,
      bytes,
      crcValid,
      decoded,
      mergedHealth,
      savedAt: saved.createdAt,
    };
  } catch (err) {
    if (err instanceof PacketDecoderError) {
      log.warn(
        {
          deviceMac: payload.deviceMac,
          packetType: item.packetType,
          source: payload.source,
          rawHex: item.rawHex,
          error: err.message,
        },
        "Bracelet packet decode failed",
      );

      let bytes: number[] | undefined;
      try {
        bytes = rawHexToBytes(item.rawHex ?? "");
      } catch {
        bytes = undefined;
      }

      const saved = await savePacket({
        payload,
        bytes,
        crcValid: false,
        decodeError: err.message,
        receivedAtMs: item.receivedAtMs,
        ingestionBatchId,
      });

      return {
        ok: false,
        id: saved.id,
        ...base,
        error: err.message,
        savedAt: saved.createdAt,
      };
    }
    throw err;
  }
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

  async function buildConsolidatedCycles(limit: number, deviceMac?: string) {
    // Cada ciclo agrega muitos pacotes crus; busca um múltiplo do limite pedido.
    const rawLimit = Math.min(Math.max(limit * 80, 400), 2000);
    const packets = await listPackets(rawLimit, deviceMac);
    return buildCycleSummaries(packets, limit).map((cycle) => ({
      ...cycle,
      patient: resolvePatientForMac(cycle.deviceMac),
    }));
  }

  app.get("/bracelets/packets", { schema: getPacketRouteSchema }, async (request, reply) => {
    const query = request.query as {
      limit?: number;
      deviceMac?: string;
      view?: string;
      group?: string | boolean;
    };
    const grouped = query.group === true || query.group === "true";
    const viewParam = typeof query.view === "string" ? query.view : undefined;
    const wantSnapshots = viewParam === "snapshots";
    const wantRawExplicit = viewParam === "raw";
    const limit =
      typeof query.limit === "number" && Number.isFinite(query.limit)
        ? query.limit
        : wantSnapshots
          ? 30
          : 50;
    const deviceMac = typeof query.deviceMac === "string" ? query.deviceMac : undefined;

    if (grouped) {
      const items = await buildConsolidatedCycles(limit, deviceMac);
      return reply.status(200).send({ view: "consolidated", items });
    }

    if (wantSnapshots) {
      const rawLimit = Math.min(Math.max(limit * 80, 400), 2000);
      const packets = await listPackets(rawLimit, deviceMac);
      const snapshots = buildSnapshotsFromPackets(packets, limit).map((snapshot) => ({
        ...snapshot,
        patient: resolvePatientForMac(snapshot.deviceMac),
      }));
      return reply.status(200).send({ view: "snapshots", snapshots });
    }

    const packets = expandPacketsForLegacyRead(
      (await listPackets(limit, deviceMac)).map(enrichWithPatient),
    );

    if (wantRawExplicit) {
      return reply.status(200).send({ view: "raw", packets });
    }

    return reply.status(200).send({ packets });
  });

  app.get(
    "/bracelets/packets/summary",
    { schema: getPacketSummaryRouteSchema },
    async (request, reply) => {
      const query = request.query as { limit?: number; deviceMac?: string };
      const limit =
        typeof query.limit === "number" && Number.isFinite(query.limit) ? query.limit : 30;
      const deviceMac = typeof query.deviceMac === "string" ? query.deviceMac : undefined;

      const items = await buildConsolidatedCycles(limit, deviceMac);
      return reply.status(200).send({ items });
    },
  );

  app.post("/bracelets/packets", { schema: postPacketRouteSchema }, async (request, reply) => {
    let batch;

    try {
      batch = packetBatchPayloadSchema.parse(request.body);
    } catch (err) {
      if (err instanceof ZodError) {
        request.log.warn({ details: err.flatten() }, "Bracelet packet batch validation failed");
        return reply.status(400).send({
          error: "Validation failed",
          details: err.flatten(),
        });
      }
      throw err;
    }

    // Identificador de ciclo: usa o do body se enviado, senão gera um por request.
    // Todos os pacotes deste batch compartilham o mesmo ingestionBatchId.
    const ingestionBatchId =
      batch.ingestionBatchId?.trim() || batch.cycleId?.trim() || randomUUID();

    const results: PacketProcessResult[] = [];

    const batchExtras = batch as Record<string, unknown>;

    for (const item of batch.packets) {
      const payload: PacketPayload = {
        deviceMac: batch.deviceMac,
        source: batch.source,
        packetType: item.packetType,
        rawHex: item.rawHex ?? "",
        metrics: item.metrics,
      };

      // Para SNAPSHOT_VITALS, salva o payload bruto completo para auditoria.
      const rawSnapshotPayload =
        item.packetType === SNAPSHOT_VITALS_TYPE
          ? {
              ...(item as Record<string, unknown>),
              bootCounter: batchExtras.bootCounter,
              measurementSessionId: batchExtras.measurementSessionId,
            }
          : undefined;

      results.push(
        await processInboundPacket(payload, item, request.log, ingestionBatchId, rawSnapshotPayload),
      );
    }

    const okCount = results.filter((result) => result.ok).length;
    const failedCount = results.length - okCount;
    if (failedCount > 0 || okCount > 0) {
      request.log.info(
        {
          deviceMac: batch.deviceMac,
          total: results.length,
          ok: okCount,
          failed: failedCount,
        },
        failedCount > 0 ? "Batch from ESP32 (with errors)" : "Batch from ESP32",
      );
    }

    // Consolida o body INTEIRO em UMA leitura principal confiável (rules 1, 2, 8):
    // SNAPSHOT_VITALS é principal quando presente; senão funde os raws com
    // prioridade de fonte; zero/inválido nunca sobrescreve valor válido.
    const inbound: InboundPacket[] = batch.packets.map((item) => ({
      packetType: item.packetType,
      rawHex: item.rawHex,
      metrics: item.metrics,
    }));
    const consolidated = buildConsolidatedSnapshot(inbound);

    const measuredAt = results.reduce(
      (latest, result) => (result.savedAt > latest ? result.savedAt : latest),
      results[0]?.savedAt ?? new Date().toISOString(),
    );

    // Extrai staleFields do resultado do SNAPSHOT_VITALS, se presente neste batch.
    const snapshotResult = results.find(
      (r): r is Extract<PacketProcessResult, { ok: true }> =>
        r.ok && r.packetType === SNAPSHOT_VITALS_TYPE,
    );
    const staleFields: string[] = snapshotResult?.staleFields ?? [];

    const snapshot = {
      deviceMac: batch.deviceMac,
      source: batch.source,
      measuredAt,
      vitals: consolidated.vitals,
      sources: consolidated.sources,
      quality: consolidated.quality,
      staleFields,
      patient: resolvePatientForMac(batch.deviceMac),
    };

    if (!consolidated.quality.snapshotComplete) {
      request.log.warn(
        { deviceMac: batch.deviceMac, vitals: consolidated.vitals, sources: consolidated.sources },
        "Consolidated snapshot incomplete",
      );
    }

    return reply.status(200).send({
      deviceMac: batch.deviceMac,
      source: batch.source,
      ingestionBatchId,
      patient: resolvePatientForMac(batch.deviceMac),
      snapshot,
      staleFields,
      usedSnapshotVitals: consolidated.usedSnapshotVitals,
      ignoredPartialSnapshots: consolidated.stats.ignoredPartialSnapshots,
      stats: {
        total: results.length,
        ok: okCount,
        failed: failedCount,
        rawSaved: okCount,
        invalidIgnored: consolidated.stats.invalidIgnored,
        snapshotComplete: consolidated.quality.snapshotComplete,
      },
    });
  });
}
