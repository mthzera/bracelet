import type { FastifyInstance } from "fastify";
import {
  getLatestClinicalAssessment,
  listClinicalAssessments,
} from "../repositories/clinical-alerts.repository.js";
import { getClinicalAlertsCatalog } from "../config/clinical-alerts.catalog.js";
import {
  getClinicalAlertsCatalogRouteSchema,
  getClinicalAssessmentsRouteSchema,
  getLatestClinicalAssessmentRouteSchema,
} from "../schemas/clinical-alerts.swagger.js";

export async function clinicalAlertsRoutes(app: FastifyInstance): Promise<void> {
  app.get(
    "/bracelets/clinical-alerts/catalog",
    { schema: getClinicalAlertsCatalogRouteSchema },
    async (_request, reply) => {
      return reply.status(200).send(getClinicalAlertsCatalog());
    },
  );

  app.get(
    "/bracelets/clinical-alerts/latest",
    { schema: getLatestClinicalAssessmentRouteSchema },
    async (request, reply) => {
      const query = request.query as { deviceMac?: string };

      if (!query.deviceMac) {
        return reply.status(400).send({
          error: "deviceMac query parameter is required",
        });
      }

      const assessment = await getLatestClinicalAssessment(query.deviceMac);

      if (!assessment) {
        return reply.status(404).send({
          error: "No clinical assessment found for this device",
          deviceMac: query.deviceMac,
        });
      }

      return reply.status(200).send(assessment);
    },
  );

  app.get(
    "/bracelets/clinical-alerts",
    { schema: getClinicalAssessmentsRouteSchema },
    async (request, reply) => {
      const query = request.query as { deviceMac?: string; limit?: number };
      const limit =
        typeof query.limit === "number" && Number.isFinite(query.limit) ? query.limit : 50;

      const assessments = await listClinicalAssessments(query.deviceMac, limit);

      return reply.status(200).send({ assessments });
    },
  );
}
