import type { FastifyInstance } from "fastify";
import { ZodError } from "zod";
import { listPackets, savePacket } from "../repositories/packet.repository.js";
import { packetPayloadSchema } from "../schemas/packet.schema.js";
import {
  getPacketRouteSchema,
  postPacketRouteSchema,
} from "../schemas/packet.swagger.js";
import {
  decodePacket,
  PacketDecoderError,
  rawHexToBytes,
} from "../services/packet-decoder.service.js";

export async function braceletRoutes(app: FastifyInstance): Promise<void> {
  app.get("/bracelets/packets", { schema: getPacketRouteSchema }, async (request, reply) => {
    const query = request.query as { limit?: number };
    const limit = typeof query.limit === "number" && Number.isFinite(query.limit) ? query.limit : 50;

    request.log.info({ limit }, "Listing bracelet packets");

    const packets = await listPackets(limit);

    request.log.info({ limit, count: packets.length }, "Listed bracelet packets");

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
      const { bytes, decoded } = decodePacket(payload.packetType, payload.rawHex);

      const saved = await savePacket({
        payload,
        bytes,
        crcValid: true,
        decoded,
      });

      const response = {
        id: saved.id,
        deviceMac: payload.deviceMac,
        packetType: payload.packetType,
        source: payload.source,
        bytes,
        crcValid: true,
        decoded,
        savedAt: saved.createdAt,
      };

      request.log.info(
        {
          id: saved.id,
          deviceMac: payload.deviceMac,
          packetType: payload.packetType,
          crcValid: true,
          decoded,
        },
        "Bracelet packet decoded and saved",
      );

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
          savedAt: saved.createdAt,
        });
      }
      throw err;
    }
  });
}
