import type { PacketMetrics } from "../schemas/packet.schema.js";
import {
  decodePacket,
  normalizePacketType,
  PacketDecoderError,
  type DecodedPacket,
  type DecodedSnapshot,
} from "./packet-decoder.service.js";
import { sleepDetailFromMetrics } from "./sleep-parser.service.js";

export type SleepSnapshotFields = Pick<
  DecodedSnapshot,
  | "sleepMinutes"
  | "sleepDate"
  | "sleepTime"
  | "sleepRecordId"
  | "sleepEndTime"
  | "sleepInBedMinutes"
  | "sleepQuality"
  | "sleepSegments"
  | "sleepTotals"
>;

export function sleepFieldsFromMetrics(metrics: PacketMetrics | undefined): SleepSnapshotFields {
  if (!metrics) return {};
  const out: SleepSnapshotFields = {};
  if (typeof metrics.sleepMinutes === "number") out.sleepMinutes = metrics.sleepMinutes;
  if (typeof metrics.sleepDate === "string") out.sleepDate = metrics.sleepDate;
  if (typeof metrics.sleepTime === "string") out.sleepTime = metrics.sleepTime;
  if (typeof metrics.sleepRecordId === "number") out.sleepRecordId = metrics.sleepRecordId;

  const detail = sleepDetailFromMetrics(metrics as Record<string, unknown>);
  if (detail) {
    out.sleepMinutes = detail.sleepMinutes;
    out.sleepDate = detail.date;
    out.sleepTime = detail.startTime;
    out.sleepRecordId = detail.recordId;
    out.sleepEndTime = detail.endTime;
    out.sleepInBedMinutes = detail.inBedMinutes;
    out.sleepQuality = detail.quality;
    out.sleepSegments = detail.segments;
    out.sleepTotals = detail.totals;
  }

  return out;
}

/**
 * Consolidação de vitais para a leitura principal exposta à tela.
 *
 * Este módulo é a fonte de verdade da API: recebe o body completo do POST
 * (SNAPSHOT_VITALS + raw packets) e devolve UMA única leitura consolidada,
 * confiável, sem zeros sobrescrevendo valores válidos e sem bytes desalinhados.
 *
 * Regras (ver issue de ingestão 2208A):
 *  1. SNAPSHOT_VITALS, quando presente, é a fonte principal — raws viram debug.
 *  2. Sem SNAPSHOT_VITALS, consolida todos os raws em UMA leitura (não 1 por batch).
 *  3. Zero/ausente nunca sobrescreve valor válido.
 *  4. Valor inválido vira null (nunca 0) na leitura principal.
 *  5. Faixas fisiológicas validam cada vital.
 *  6. Prioridade de fonte por vital (ex.: BPM prefere 0x54, cai pra 0x28).
 */

export const SNAPSHOT_VITALS_TYPE = "SNAPSHOT_VITALS";

export type VitalKey =
  | "heartRate"
  | "spo2"
  | "temperature"
  | "hrv"
  | "fatigue"
  | "systolicPressure"
  | "diastolicPressure";

export type ConsolidatedVitals = Record<VitalKey, number | null>;

const VITAL_KEYS: VitalKey[] = [
  "heartRate",
  "spo2",
  "temperature",
  "hrv",
  "fatigue",
  "systolicPressure",
  "diastolicPressure",
];

/**
 * Faixas válidas (rule 5). `fatigue` tem mínimo 1: o firmware usa 0 como
 * "sem leitura", então tratamos 0 como ausente (rule 4), não zero real.
 */
export const VITAL_RANGES: Record<VitalKey, { min: number; max: number }> = {
  heartRate: { min: 35, max: 220 },
  spo2: { min: 50, max: 100 },
  temperature: { min: 30, max: 43 },
  hrv: { min: 1, max: 200 },
  fatigue: { min: 1, max: 100 },
  systolicPressure: { min: 70, max: 220 },
  diastolicPressure: { min: 40, max: 140 },
};

/** Diferença mínima sistólica-diastólica para a PA ser plausível (rule 5). */
const MIN_PULSE_PRESSURE = 15;

/**
 * Prioridade de fonte por vital, melhor primeiro (rule 6). SNAPSHOT_VITALS é
 * sempre a fonte principal. Uma fonte ausente da lista de um vital NÃO pode
 * contribuir aquele vital (ex.: 0x54 nunca fornece pressão).
 */
export const VITAL_SOURCE_PRIORITY: Record<VitalKey, string[]> = {
  heartRate: [SNAPSHOT_VITALS_TYPE, "0x54", "0x55", "0x28"],
  spo2: [SNAPSHOT_VITALS_TYPE, "0x66", "0x60", "0x28"],
  temperature: [SNAPSHOT_VITALS_TYPE, "0x65", "0x62", "0x28"],
  hrv: [SNAPSHOT_VITALS_TYPE, "0x56", "0x28"],
  fatigue: [SNAPSHOT_VITALS_TYPE, "0x56", "0x28"],
  systolicPressure: [SNAPSHOT_VITALS_TYPE, "0x28"],
  diastolicPressure: [SNAPSHOT_VITALS_TYPE, "0x28"],
};

/** Rótulo descritivo da origem de cada vital, exposto em `sources`. */
const SOURCE_LABEL: Record<string, string> = {
  [SNAPSHOT_VITALS_TYPE]: "SNAPSHOT_VITALS",
  "0x28": "0x28_REALTIME",
  "0x54": "0x54_HISTORY",
  "0x55": "0x55_HISTORY",
  "0x56": "0x56_HISTORY",
  "0x60": "0x60_HISTORY_MANUAL",
  "0x66": "0x66_HISTORY_AUTO",
  "0x62": "0x62_HISTORY_MANUAL",
  "0x65": "0x65_HISTORY_AUTO",
};

function sourceLabel(packetType: string): string {
  return SOURCE_LABEL[packetType] ?? packetType;
}

/** Posição na lista de prioridade. Maior = melhor. -1 = fonte não permitida. */
function priorityRank(
  key: VitalKey,
  packetType: string,
  priority: Record<VitalKey, string[]> = VITAL_SOURCE_PRIORITY,
): number {
  const list = priority[key];
  const index = list.indexOf(packetType);
  return index === -1 ? -1 : list.length - index;
}

/** Um vital é válido se for número finito, > 0 e dentro da faixa fisiológica. */
export function isVitalValid(key: VitalKey, value: number | null | undefined): value is number {
  if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) return false;
  const range = VITAL_RANGES[key];
  return value >= range.min && value <= range.max;
}

/**
 * Aplica faixas e regras de pressão; campos inválidos viram null (rule 4).
 * A pressão exige sistólica > diastólica e diferença mínima — senão ambas null.
 */
export function validateVitals(raw: Partial<ConsolidatedVitals>): ConsolidatedVitals {
  const out: ConsolidatedVitals = {
    heartRate: null,
    spo2: null,
    temperature: null,
    hrv: null,
    fatigue: null,
    systolicPressure: null,
    diastolicPressure: null,
  };

  for (const key of VITAL_KEYS) {
    const value = raw[key];
    if (isVitalValid(key, value)) out[key] = value;
  }

  const sys = out.systolicPressure;
  const dia = out.diastolicPressure;
  const bpValid =
    sys !== null && dia !== null && sys > dia && sys - dia >= MIN_PULSE_PRESSURE;
  if (!bpValid) {
    out.systolicPressure = null;
    out.diastolicPressure = null;
  }

  return out;
}

/** Pacote bruto recebido no body do POST. */
export type InboundPacket = {
  packetType: string;
  rawHex?: string;
  metrics?: PacketMetrics;
  /** Decoded já calculado pela rota (evita decodar de novo). */
  decoded?: DecodedPacket | null;
};

/** Resultado de `parsePacket`: a contribuição de vitais de um único pacote. */
export type ParsedPacket = {
  packetType: string;
  source: string;
  vitals: Partial<Record<VitalKey, number>>;
};

export function hasSnapshotVitals(packets: InboundPacket[]): boolean {
  return packets.some((p) => normalizePacketType(p.packetType) === SNAPSHOT_VITALS_TYPE);
}

function metricsToVitals(metrics: PacketMetrics | undefined): Partial<Record<VitalKey, number>> {
  if (!metrics) return {};
  const vitals: Partial<Record<VitalKey, number>> = {};
  if (typeof metrics.bpm === "number") vitals.heartRate = metrics.bpm;
  if (typeof metrics.spo2 === "number") vitals.spo2 = metrics.spo2;
  if (typeof metrics.temperature === "number") vitals.temperature = metrics.temperature;
  if (typeof metrics.hrv === "number") vitals.hrv = metrics.hrv;
  if (typeof metrics.fatigue === "number") vitals.fatigue = metrics.fatigue;
  if (typeof metrics.bloodPressureSystolic === "number")
    vitals.systolicPressure = metrics.bloodPressureSystolic;
  if (typeof metrics.bloodPressureDiastolic === "number")
    vitals.diastolicPressure = metrics.bloodPressureDiastolic;
  return vitals;
}

/** Lê o SNAPSHOT_VITALS já validado (rule 1). */
export function applySnapshotVitals(packet: InboundPacket): ConsolidatedVitals {
  return validateVitals(metricsToVitals(packet.metrics));
}

function vitalsFromDecoded(decoded: DecodedPacket): Partial<Record<VitalKey, number>> {
  switch (decoded.type) {
    case "0x28":
      return {
        heartRate: decoded.heartRate,
        spo2: decoded.spo2,
        temperature: decoded.temperature,
        hrv: decoded.hrv,
        fatigue: decoded.fatigue,
        systolicPressure: decoded.systolicPressure,
        diastolicPressure: decoded.diastolicPressure,
      };
    case "0x56":
      return { hrv: decoded.hrv, fatigue: decoded.fatigue };
    default:
      return {};
  }
}

/**
 * Normaliza um pacote bruto numa contribuição de vitais com sua fonte.
 * - SNAPSHOT_VITALS / 0x28: usa `metrics` direto quando presente, senão decoda o rawHex.
 * - Históricos (0x54/0x56/0x60/0x62/0x65/0x66): decoda o rawHex.
 * Retorna null quando o pacote não carrega vitais (bateria, MAC, etc.).
 */
export function parsePacket(packet: InboundPacket): ParsedPacket | null {
  const packetType = normalizePacketType(packet.packetType);

  if (packetType === SNAPSHOT_VITALS_TYPE) {
    return { packetType, source: sourceLabel(packetType), vitals: metricsToVitals(packet.metrics) };
  }

  // 0x28: metrics do ESP32 espelham o rawHex; preferimos o que existir, com
  // metrics preenchendo lacunas. Demais tipos vêm só como rawHex.
  let decoded = packet.decoded ?? null;
  if (!decoded && packet.rawHex) {
    try {
      decoded = decodePacket(packetType, packet.rawHex).decoded;
    } catch (err) {
      if (!(err instanceof PacketDecoderError)) throw err;
      decoded = null;
    }
  }

  const fromDecoded = decoded ? vitalsFromDecoded(decoded) : {};

  if (packetType === "0x28") {
    const fromMetrics = metricsToVitals(packet.metrics);
    // rawHex decodado é fonte de verdade; metrics só preenche o que veio 0/ausente.
    const merged: Partial<Record<VitalKey, number>> = { ...fromMetrics };
    for (const key of VITAL_KEYS) {
      const decodedValue = fromDecoded[key];
      if (typeof decodedValue === "number" && decodedValue > 0) merged[key] = decodedValue;
    }
    return { packetType, source: sourceLabel(packetType), vitals: merged };
  }

  if (Object.keys(fromDecoded).length === 0) return null;
  return { packetType, source: sourceLabel(packetType), vitals: fromDecoded };
}

type MergedEntry = { value: number; packetType: string; source: string };
export type MergedState = Partial<Record<VitalKey, MergedEntry>>;

/**
 * Funde a contribuição de um pacote no estado consolidado respeitando:
 *  - valor inválido/zero nunca sobrescreve (rules 3, 4);
 *  - fonte não permitida para o vital é ignorada (rule 6 — ex.: PA só 0x28/SNAPSHOT);
 *  - em empate de prioridade, o último registro válido vence (rule 11).
 */
export function mergeVitals(
  current: MergedState,
  next: ParsedPacket,
  sourcePriority: Record<VitalKey, string[]> = VITAL_SOURCE_PRIORITY,
): MergedState {
  const out: MergedState = { ...current };

  for (const key of VITAL_KEYS) {
    const value = next.vitals[key];
    if (!isVitalValid(key, value)) continue;

    const rank = priorityRank(key, next.packetType, sourcePriority);
    if (rank < 0) continue;

    const existing = out[key];
    if (!existing) {
      out[key] = { value, packetType: next.packetType, source: next.source };
      continue;
    }

    const existingRank = priorityRank(key, existing.packetType, sourcePriority);
    if (rank >= existingRank) {
      out[key] = { value, packetType: next.packetType, source: next.source };
    }
  }

  return out;
}

export type ConsolidatedSources = {
  heartRate: string | null;
  spo2: string | null;
  temperature: string | null;
  hrv: string | null;
  fatigue: string | null;
  bloodPressure: string | null;
};

export type ConsolidatedQuality = {
  /** Todos os vitais medidos pela pulseira (HR, SpO2, temp, HRV, fadiga) presentes. */
  snapshotComplete: boolean;
  /** Pressão da pulseira é sempre estimada; "absent" quando não há leitura válida. */
  bloodPressure: "estimated" | "absent";
};

export type ConsolidatedStats = {
  total: number;
  parsed: number;
  invalidIgnored: number;
  rawHealthPackets: number;
  ignoredPartialSnapshots: number;
};

export type ConsolidatedSnapshot = {
  vitals: ConsolidatedVitals;
  sources: ConsolidatedSources;
  quality: ConsolidatedQuality;
  usedSnapshotVitals: boolean;
  stats: ConsolidatedStats;
};

function countInvalidContributions(parsed: ParsedPacket[]): number {
  let invalid = 0;
  for (const packet of parsed) {
    for (const key of VITAL_KEYS) {
      const value = packet.vitals[key];
      if (value !== undefined && !isVitalValid(key, value)) invalid++;
    }
  }
  return invalid;
}

/**
 * Consolida o body inteiro em UMA leitura principal (rules 1, 2, 8).
 * Com SNAPSHOT_VITALS, ele é a fonte principal e os raws ficam só como debug.
 */
export function buildConsolidatedSnapshot(packets: InboundPacket[]): ConsolidatedSnapshot {
  const parsed = packets
    .map((packet) => parsePacket(packet))
    .filter((packet): packet is ParsedPacket => packet !== null);

  const rawHealthParsed = parsed.filter((p) => p.packetType !== SNAPSHOT_VITALS_TYPE);
  const snapshotPacket = packets.find(
    (p) => normalizePacketType(p.packetType) === SNAPSHOT_VITALS_TYPE,
  );

  let merged: MergedState = {};
  let usedSnapshotVitals = false;

  if (snapshotPacket) {
    // Rule 1: SNAPSHOT_VITALS é a leitura principal; raws NÃO sobrescrevem.
    usedSnapshotVitals = true;
    const snapshotParsed = parsePacket(snapshotPacket);
    if (snapshotParsed) merged = mergeVitals({}, snapshotParsed);
  } else {
    // Rule 2: consolida todos os raws numa única leitura final.
    for (const packet of rawHealthParsed) {
      merged = mergeVitals(merged, packet);
    }
  }

  const rawVitals: Partial<ConsolidatedVitals> = {};
  for (const key of VITAL_KEYS) {
    const entry = merged[key];
    if (entry) rawVitals[key] = entry.value;
  }
  const vitals = validateVitals(rawVitals);

  const sourceFor = (key: VitalKey): string | null =>
    vitals[key] !== null && merged[key] ? merged[key]!.source : null;

  const sources: ConsolidatedSources = {
    heartRate: sourceFor("heartRate"),
    spo2: sourceFor("spo2"),
    temperature: sourceFor("temperature"),
    hrv: sourceFor("hrv"),
    fatigue: sourceFor("fatigue"),
    bloodPressure: vitals.systolicPressure !== null && merged.systolicPressure
      ? merged.systolicPressure.source
      : null,
  };

  const snapshotComplete =
    vitals.heartRate !== null &&
    vitals.spo2 !== null &&
    vitals.temperature !== null &&
    vitals.hrv !== null &&
    vitals.fatigue !== null;

  const quality: ConsolidatedQuality = {
    snapshotComplete,
    bloodPressure: vitals.systolicPressure !== null ? "estimated" : "absent",
  };

  const stats: ConsolidatedStats = {
    total: packets.length,
    parsed: parsed.length,
    invalidIgnored: countInvalidContributions(parsed),
    rawHealthPackets: rawHealthParsed.length,
    // Com snapshot principal, os raws de saúde não geram leitura principal própria.
    ignoredPartialSnapshots: usedSnapshotVitals ? rawHealthParsed.length : 0,
  };

  return { vitals, sources, quality, usedSnapshotVitals, stats };
}
