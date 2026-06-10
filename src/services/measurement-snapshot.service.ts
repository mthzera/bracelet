import type { SavedPacket } from "../repositories/packet.repository.js";
import {
  hasCompleteVitals,
  missingVitalFields,
} from "./vitals-validation.service.js";
import {
  crcApplies,
  decodePacket,
  isValidDiastolic,
  isValidFatigue,
  isValidHeartRate,
  isValidHrv,
  isValidSpo2,
  isValidSystolic,
  isValidTemperature,
  mergeHealthReadings,
  PacketDecoderError,
  type DecodedBattery,
  type DecodedFirmware,
  type DecodedHealth,
  type DecodedHrvHistory,
  type DecodedMac,
  type DecodedPacket,
  type DecodedSleep,
} from "./packet-decoder.service.js";

/**
 * Janela de coleta BLE: a pulseira mede BPM por ~10 min (0x28 02 01) e logo
 * depois o ESP32 puxa históricos. Pacotes dentro dessa janela formam 1 ciclo.
 */
const CYCLE_GAP_MS = 10 * 60 * 1000;

export type SnapshotSleep = {
  date: string;
  time: string;
  sleepMinutes: number;
  recordId: number;
};

export type SnapshotVitals = {
  heartRate: number;
  spo2: number;
  temperature: number;
  hrv: number;
  fatigue: number;
  systolicPressure: number;
  diastolicPressure: number;
};

export type MeasurementSnapshot = {
  id: number;
  deviceMac: string;
  source: string;
  measuredAt: string;
  vitals: SnapshotVitals;
  sleep: SnapshotSleep | null;
  battery: number | null;
  firmware: string | null;
  deviceMacReported: string | null;
  packetCount: number;
  failedCount: number;
  complete: boolean;
  missing: string[];
};

function freshDecoded(packet: SavedPacket): DecodedPacket | null {
  if (!packet.crcValid || !packet.rawHex) return packet.decoded;
  try {
    return decodePacket(packet.packetType, packet.rawHex).decoded;
  } catch (err) {
    if (!(err instanceof PacketDecoderError)) throw err;
    return packet.decoded;
  }
}

function isMergeableHealth(
  decoded: DecodedPacket | null,
): decoded is DecodedHealth | DecodedHrvHistory {
  return decoded?.type === "0x28" || decoded?.type === "0x56";
}

function emptyVitals(): SnapshotVitals {
  return {
    heartRate: 0,
    spo2: 0,
    temperature: 0,
    hrv: 0,
    fatigue: 0,
    systolicPressure: 0,
    diastolicPressure: 0,
  };
}

function vitalsFromMerged(merged: DecodedHealth): SnapshotVitals {
  return {
    heartRate: merged.heartRate,
    spo2: merged.spo2,
    temperature: merged.temperature,
    hrv: merged.hrv,
    fatigue: merged.fatigue,
    systolicPressure: merged.systolicPressure,
    diastolicPressure: merged.diastolicPressure,
  };
}

function groupPacketsByCycle(packets: SavedPacket[]): SavedPacket[][] {
  if (packets.length === 0) return [];

  const sorted = [...packets].sort(
    (a, b) => new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime(),
  );

  const groups: SavedPacket[][] = [];

  for (const packet of sorted) {
    const current = groups[groups.length - 1];
    if (!current) {
      groups.push([packet]);
      continue;
    }

    const newestMs = new Date(current[0]!.createdAt).getTime();
    const packetMs = new Date(packet.createdAt).getTime();

    if (newestMs - packetMs <= CYCLE_GAP_MS) {
      current.push(packet);
    } else {
      groups.push([packet]);
    }
  }

  return groups;
}

function latestPacketByType<T extends DecodedPacket["type"]>(
  packets: SavedPacket[],
  type: T,
): (DecodedPacket & { type: T }) | null {
  const ordered = [...packets].sort(
    (a, b) => new Date(b.createdAt).getTime() - new Date(a.createdAt).getTime(),
  );

  for (const packet of ordered) {
    const decoded = freshDecoded(packet);
    if (decoded?.type === type) {
      return decoded as DecodedPacket & { type: T };
    }
  }

  return null;
}

export function buildSnapshotFromPackets(packets: SavedPacket[]): MeasurementSnapshot | null {
  if (packets.length === 0) return null;

  const measuredAt = packets.reduce((latest, packet) => {
    const ts = new Date(packet.createdAt).getTime();
    return ts > latest ? ts : latest;
  }, 0);

  const healthDecoded = packets
    .map((packet) => freshDecoded(packet))
    .filter(isMergeableHealth);

  const vitals =
    healthDecoded.length > 0
      ? vitalsFromMerged(mergeHealthReadings(healthDecoded))
      : emptyVitals();

  const sleepDecoded = latestPacketByType(packets, "0x53") as DecodedSleep | null;
  const batteryDecoded = latestPacketByType(packets, "0x13") as DecodedBattery | null;
  const firmwareDecoded = latestPacketByType(packets, "0x27") as DecodedFirmware | null;
  const macDecoded = latestPacketByType(packets, "0x22") as DecodedMac | null;

  const anchor = packets.reduce((best, packet) => (packet.id > best.id ? packet : best), packets[0]!);
  const failedCount = packets.filter((packet) => packet.decodeError).length;

  const missing = missingVitalFields(vitals);

  return {
    id: anchor.id,
    deviceMac: anchor.deviceMac,
    source: anchor.source,
    measuredAt: new Date(measuredAt).toISOString(),
    vitals,
    complete: hasCompleteVitals(vitals),
    missing,
    sleep: sleepDecoded
      ? {
          date: sleepDecoded.date,
          time: sleepDecoded.time,
          sleepMinutes: sleepDecoded.sleepMinutes,
          recordId: sleepDecoded.recordId,
        }
      : null,
    battery: batteryDecoded?.battery ?? null,
    firmware: firmwareDecoded
      ? `V${firmwareDecoded.major}.${firmwareDecoded.minor}.${firmwareDecoded.patch}`
      : null,
    deviceMacReported: macDecoded?.mac ?? null,
    packetCount: packets.length,
    failedCount,
  };
}

export function buildSnapshotsFromPackets(
  packets: SavedPacket[],
  maxSnapshots?: number,
): MeasurementSnapshot[] {
  const snapshots = groupPacketsByCycle(packets)
    .map((group) => buildSnapshotFromPackets(group))
    .filter((snapshot): snapshot is MeasurementSnapshot => snapshot !== null);

  if (typeof maxSnapshots === "number" && maxSnapshots > 0) {
    return snapshots.slice(0, maxSnapshots);
  }

  return snapshots;
}

// ── Consolidado por ciclo (formato para o front: 1 linha = 1 ciclo) ──────────

export type CycleRawPacket = {
  id: number;
  packetType: string;
  rawHex: string;
  crcValid: boolean;
  crcApplicable: boolean;
  decodeError: string | null;
  decoded: DecodedPacket | null;
  receivedAt: string;
};

export type CycleCrcCounts = {
  valid: number;
  invalid: number;
  notApplicable: number;
};

/** Resumo clínico do ciclo. null = ausente (o front mostra "--", nunca 0). */
export type CycleSummaryData = {
  heartRate: number | null;
  heartRateMin: number | null;
  heartRateMax: number | null;
  temperature: number | null;
  hrv: number | null;
  fatigue: number | null;
  bloodPressure: { systolic: number | null; diastolic: number | null; measuredAt: string | null };
  spo2: number | null;
  battery: number | null;
  sleepMinutes: number | null;
  firmware: string | null;
};

/** Quais famílias de pacote alimentaram este ciclo. */
export type CycleSources = {
  activePackets: boolean;
  heartRateHistory: boolean;
  hrvFatigueHistory: boolean;
  temperatureHistory: boolean;
};

export type CycleSummary = {
  cycleId: string;
  deviceMac: string;
  source: string;
  startedAt: string;
  endedAt: string;
  summary: CycleSummaryData;
  sources: CycleSources;
  sleep: SnapshotSleep | null;
  deviceMacReported: string | null;
  packetTypes: string[];
  packetsCount: number;
  crc: CycleCrcCounts;
  complete: boolean;
  missing: string[];
  rawPackets: CycleRawPacket[];
};

function newestMs(packets: SavedPacket[]): number {
  return packets.reduce((latest, packet) => {
    const ts = new Date(packet.createdAt).getTime();
    return ts > latest ? ts : latest;
  }, 0);
}

/**
 * Agrupa pacotes por ciclo de coleta = deviceMac + janela de ~10 min.
 *
 * O ESP32 transmite a janela em vários POSTs pequenos (cada um com seu próprio
 * ingestionBatchId), então agrupar por batchId fragmentaria a janela em dezenas
 * de "ciclos" de 2-3 pacotes. Por isso agrupamos por tempo+dispositivo; o
 * ingestionBatchId continua salvo só para auditoria.
 */
function groupPacketsByCycleKey(packets: SavedPacket[]): SavedPacket[][] {
  const byDevice = new Map<string, SavedPacket[]>();
  for (const packet of packets) {
    const list = byDevice.get(packet.deviceMac) ?? [];
    list.push(packet);
    byDevice.set(packet.deviceMac, list);
  }

  const groups: SavedPacket[][] = [];
  for (const devicePackets of byDevice.values()) {
    groups.push(...groupPacketsByCycle(devicePackets));
  }

  // Mais recentes primeiro.
  return groups.sort((a, b) => newestMs(b) - newestMs(a));
}

function cycleIdForGroup(group: SavedPacket[]): string {
  // Só usa o batchId como id do ciclo se TODOS os pacotes compartilham o mesmo
  // (ex.: janela enviada em um único POST). Caso contrário, id por janela.
  const batchIds = new Set(group.map((packet) => packet.ingestionBatchId).filter(Boolean));
  if (batchIds.size === 1) {
    return [...batchIds][0] as string;
  }
  const ids = group.map((packet) => packet.id);
  return `win-${Math.min(...ids)}-${Math.max(...ids)}`;
}

function average(values: number[]): number | null {
  if (values.length === 0) return null;
  return values.reduce((sum, v) => sum + v, 0) / values.length;
}

function avgRound(values: number[]): number | null {
  const a = average(values);
  return a === null ? null : Math.round(a);
}

function avgRound1(values: number[]): number | null {
  const a = average(values);
  return a === null ? null : Math.round(a * 10) / 10;
}

/**
 * Consolida a janela de coleta em um único resumo clínico, usando a MÉDIA dos
 * valores válidos de cada parâmetro na janela:
 * - heartRate: média (+ min/máx) dos BPM válidos do 0x28 e do histórico 0x54/0x55.
 * - temperature: média das temperaturas válidas do 0x28 e de 0x62/0x65.
 * - hrv/fatigue: média do histórico 0x56 (fonte real); só cai no 0x28 se não houver
 *   0x56 — o 0x28 costuma repetir um baseline fixo nesses bytes.
 * - spo2: média do 0x28 (modo oxigênio) e de 0x60/0x66.
 * - bloodPressure: média dos pares válidos do 0x28.
 * Faixas fisiológicas filtram leituras inválidas; campos sem valor ficam null.
 */
function buildCycleSummary(group: SavedPacket[]): CycleSummary | null {
  if (group.length === 0) return null;

  const sorted = [...group].sort(
    (a, b) => new Date(a.createdAt).getTime() - new Date(b.createdAt).getTime(),
  );

  // Acumula todos os valores válidos da janela para tirar média.
  const hrValues: number[] = [];
  const tempValues: number[] = [];
  const hrv28: number[] = [];
  const hrv56: number[] = [];
  const fatigue28: number[] = [];
  const fatigue56: number[] = [];
  const systolicValues: number[] = [];
  const diastolicValues: number[] = [];
  let bloodPressureAt: string | null = null; // horário da leitura de PA mais recente
  const spo2Values: number[] = [];
  let battery: number | null = null;
  let firmware: string | null = null;
  let sleep: SnapshotSleep | null = null;
  let deviceMacReported: string | null = null;

  const sources: CycleSources = {
    activePackets: false,
    heartRateHistory: false,
    hrvFatigueHistory: false,
    temperatureHistory: false,
  };

  const crc: CycleCrcCounts = { valid: 0, invalid: 0, notApplicable: 0 };
  const rawPackets: CycleRawPacket[] = [];

  for (const packet of sorted) {
    const applicable = crcApplies(packet.packetType);
    if (!applicable) crc.notApplicable++;
    else if (packet.crcValid) crc.valid++;
    else crc.invalid++;

    rawPackets.push({
      id: packet.id,
      packetType: packet.packetType,
      rawHex: packet.rawHex,
      crcValid: packet.crcValid,
      crcApplicable: applicable,
      decodeError: packet.decodeError,
      decoded: packet.decoded,
      receivedAt: packet.createdAt,
    });

    const decoded = freshDecoded(packet);
    const type = packet.packetType.toLowerCase();

    switch (type) {
      case "0x28": // active measurement packet
        if (decoded?.type === "0x28") {
          sources.activePackets = true;
          if (isValidHeartRate(decoded.heartRate)) hrValues.push(decoded.heartRate);
          if (isValidTemperature(decoded.temperature)) tempValues.push(decoded.temperature);
          if (isValidHrv(decoded.hrv)) hrv28.push(decoded.hrv);
          if (isValidFatigue(decoded.fatigue)) fatigue28.push(decoded.fatigue);
          if (
            isValidSystolic(decoded.systolicPressure) &&
            isValidDiastolic(decoded.diastolicPressure)
          ) {
            systolicValues.push(decoded.systolicPressure);
            diastolicValues.push(decoded.diastolicPressure);
            bloodPressureAt = packet.createdAt; // sorted asc -> fica com a leitura mais recente
          }
          if (isValidSpo2(decoded.spo2)) spo2Values.push(decoded.spo2);
        }
        break;
      case "0x54": // histórico de BPM (bulk)
      case "0x55": // histórico de BPM (pontual)
        sources.heartRateHistory = true;
        if (decoded?.type === "0x28" && isValidHeartRate(decoded.heartRate)) {
          hrValues.push(decoded.heartRate);
        }
        break;
      case "0x56": // histórico HRV/fadiga (fonte real)
        sources.hrvFatigueHistory = true;
        if (decoded?.type === "0x56") {
          if (isValidHrv(decoded.hrv)) hrv56.push(decoded.hrv);
          if (isValidFatigue(decoded.fatigue)) fatigue56.push(decoded.fatigue);
        }
        break;
      case "0x62": // temperatura manual
      case "0x65": // temperatura automática
        sources.temperatureHistory = true;
        if (decoded?.type === "0x28" && isValidTemperature(decoded.temperature)) {
          tempValues.push(decoded.temperature);
        }
        break;
      case "0x60": // SpO2 manual
      case "0x66": // SpO2 automático
        if (decoded?.type === "0x28" && isValidSpo2(decoded.spo2)) {
          spo2Values.push(decoded.spo2);
        }
        break;
      case "0x13":
        if (decoded?.type === "0x13") battery = decoded.battery;
        break;
      case "0x27":
        if (decoded?.type === "0x27") {
          firmware = `V${decoded.major}.${decoded.minor}.${decoded.patch}`;
        }
        break;
      case "0x53":
        if (decoded?.type === "0x53") {
          sleep = {
            date: decoded.date,
            time: decoded.time,
            sleepMinutes: decoded.sleepMinutes,
            recordId: decoded.recordId,
          };
        }
        break;
      case "0x22":
        if (decoded?.type === "0x22") deviceMacReported = decoded.mac;
        break;
    }
  }

  rawPackets.sort((a, b) => b.id - a.id);

  // Média dos BPM válidos da janela (+ extremos).
  const heartRate = avgRound(hrValues);
  const heartRateMin = hrValues.length > 0 ? Math.min(...hrValues) : null;
  const heartRateMax = hrValues.length > 0 ? Math.max(...hrValues) : null;

  const temperature = avgRound1(tempValues);
  // HRV/fadiga: prioriza o histórico 0x56 (valor real); 0x28 é baseline fixo.
  const hrv = avgRound(hrv56.length > 0 ? hrv56 : hrv28);
  const fatigue = avgRound(fatigue56.length > 0 ? fatigue56 : fatigue28);
  const spo2 = avgRound(spo2Values);
  const systolic = avgRound(systolicValues);
  const diastolic = avgRound(diastolicValues);

  const summary: CycleSummaryData = {
    heartRate,
    heartRateMin,
    heartRateMax,
    temperature,
    hrv,
    fatigue,
    bloodPressure: { systolic, diastolic, measuredAt: systolic !== null ? bloodPressureAt : null },
    spo2,
    battery,
    sleepMinutes: sleep?.sleepMinutes ?? null,
    firmware,
  };

  const missing = [
    heartRate === null ? "heartRate" : null,
    temperature === null ? "temperature" : null,
    hrv === null ? "hrv" : null,
  ].filter((field): field is string => field !== null);

  const timestamps = sorted.map((packet) => new Date(packet.createdAt).getTime());

  return {
    cycleId: cycleIdForGroup(group),
    deviceMac: sorted[0]!.deviceMac,
    source: sorted[0]!.source,
    startedAt: new Date(Math.min(...timestamps)).toISOString(),
    endedAt: new Date(Math.max(...timestamps)).toISOString(),
    summary,
    sources,
    sleep,
    deviceMacReported,
    packetTypes: [...new Set(group.map((packet) => packet.packetType))].sort(),
    packetsCount: group.length,
    crc,
    complete: heartRate !== null && temperature !== null,
    missing,
    rawPackets,
  };
}

/** Consolida pacotes crus em 1 registro lógico por ciclo de coleta. */
export function buildCycleSummaries(
  packets: SavedPacket[],
  maxCycles?: number,
): CycleSummary[] {
  const summaries = groupPacketsByCycleKey(packets)
    .map((group) => buildCycleSummary(group))
    .filter((summary): summary is CycleSummary => summary !== null);

  if (typeof maxCycles === "number" && maxCycles > 0) {
    return summaries.slice(0, maxCycles);
  }

  return summaries;
}

export type BatchProcessInput = {
  ok: boolean;
  id: number;
  packetType: string;
  receivedAtMs: number;
  savedAt: string;
  decoded?: DecodedPacket;
  error?: string;
};

/** Consolida o batch recém-processado (mesmo ciclo ESP32). */
export function buildSnapshotFromBatchResults(
  deviceMac: string,
  source: string,
  results: BatchProcessInput[],
): MeasurementSnapshot | null {
  const packets: SavedPacket[] = results.map((result) => ({
    id: result.id,
    deviceMac,
    packetType: result.packetType,
    rawHex: "",
    source,
    bytes: null,
    crcValid: result.ok,
    decoded: result.ok ? (result.decoded ?? null) : null,
    decodeError: result.ok ? null : (result.error ?? "decode error"),
    ingestionBatchId: null,
    createdAt: result.savedAt,
  }));

  return buildSnapshotFromPackets(packets);
}
