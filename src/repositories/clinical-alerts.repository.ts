import { getPool } from "../database/db.js";
import type { ClinicalAssessment } from "../types/clinical-alerts.types.js";
import type { RecentVitalsSnapshot } from "../types/clinical-alerts.types.js";

export type SavedClinicalAssessment = ClinicalAssessment & {
  id: number;
  packetId: number | null;
  createdAt: string;
};

type AssessmentRow = {
  id: number;
  device_mac: string;
  measured_at: Date;
  packet_id: number | null;
  vitals: ClinicalAssessment["vitals"];
  context: ClinicalAssessment["context"];
  alerts: ClinicalAssessment["alerts"];
  notes: string[];
  risk_score: number;
  overall_status: string;
  severity: string;
  baseline: ClinicalAssessment["baseline"];
  disclaimer: string;
  created_at: Date;
};

function rowToSaved(row: AssessmentRow): SavedClinicalAssessment {
  return {
    id: row.id,
    packetId: row.packet_id,
    deviceMac: row.device_mac,
    measuredAt: row.measured_at.toISOString(),
    vitals: row.vitals,
    context: row.context,
    alerts: row.alerts,
    notes: row.notes,
    riskScore: row.risk_score,
    overallStatus: row.overall_status as SavedClinicalAssessment["overallStatus"],
    severity: row.severity as SavedClinicalAssessment["severity"],
    baseline: row.baseline,
    disclaimer: row.disclaimer,
    createdAt: row.created_at.toISOString(),
  };
}

export async function saveClinicalAssessment(
  assessment: ClinicalAssessment,
  packetId?: number,
): Promise<SavedClinicalAssessment> {
  const pool = getPool();

  const result = await pool.query<AssessmentRow>(
    `
      INSERT INTO vital_assessments (
        device_mac,
        measured_at,
        packet_id,
        vitals,
        context,
        alerts,
        notes,
        risk_score,
        overall_status,
        severity,
        baseline,
        disclaimer
      )
      VALUES ($1, $2, $3, $4::jsonb, $5::jsonb, $6::jsonb, $7::jsonb, $8, $9, $10, $11::jsonb, $12)
      RETURNING *
    `,
    [
      assessment.deviceMac,
      assessment.measuredAt,
      packetId ?? null,
      JSON.stringify(assessment.vitals),
      JSON.stringify(assessment.context),
      JSON.stringify(assessment.alerts),
      JSON.stringify(assessment.notes),
      assessment.riskScore,
      assessment.overallStatus,
      assessment.severity,
      JSON.stringify(assessment.baseline),
      assessment.disclaimer,
    ],
  );

  return rowToSaved(result.rows[0] as AssessmentRow);
}

export async function getRecentVitalsHistory(
  deviceMac: string,
  limit = 10,
): Promise<RecentVitalsSnapshot[]> {
  const pool = getPool();
  const safeLimit = Math.min(Math.max(limit, 1), 50);

  const { rows } = await pool.query<{
    measured_at: Date;
    vitals: RecentVitalsSnapshot["vitals"];
    context: RecentVitalsSnapshot["context"];
  }>(
    `
      SELECT measured_at, vitals, context
      FROM vital_assessments
      WHERE device_mac = $1
      ORDER BY measured_at DESC
      LIMIT $2
    `,
    [deviceMac, safeLimit],
  );

  return rows.map((row) => ({
    measuredAt: row.measured_at.toISOString(),
    vitals: row.vitals,
    context: row.context ?? {},
  }));
}

export async function listClinicalAssessments(
  deviceMac: string | undefined,
  limit = 50,
): Promise<SavedClinicalAssessment[]> {
  const pool = getPool();
  const safeLimit = Math.min(Math.max(limit, 1), 200);

  if (deviceMac) {
    const { rows } = await pool.query<AssessmentRow>(
      `
        SELECT *
        FROM vital_assessments
        WHERE device_mac = $1
        ORDER BY measured_at DESC
        LIMIT $2
      `,
      [deviceMac, safeLimit],
    );
    return rows.map(rowToSaved);
  }

  const { rows } = await pool.query<AssessmentRow>(
    `
      SELECT *
      FROM vital_assessments
      ORDER BY measured_at DESC
      LIMIT $1
    `,
    [safeLimit],
  );

  return rows.map(rowToSaved);
}

export async function getLatestClinicalAssessment(
  deviceMac: string,
): Promise<SavedClinicalAssessment | null> {
  const list = await listClinicalAssessments(deviceMac, 1);
  return list[0] ?? null;
}
