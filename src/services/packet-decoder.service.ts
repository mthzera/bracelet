export type DecodedBattery = {
  type: "0x13";
  battery: number;
};

export type DecodedMac = {
  type: "0x22";
  mac: string;
};

export type DecodedFirmware = {
  type: "0x27";
  major: number;
  minor: number;
  patch: number;
};

export type HealthMeasurementMode =
  | "hrv"
  | "heart"
  | "oxygen"
  | "temperature"
  | "blood_pressure"
  | "unknown";

export type DecodedHealth = {
  type: "0x28";
  /** Byte AA — modo ativo da medição (0x01 HRV … 0x04 temperatura). */
  measurementType: number;
  measurementMode: HealthMeasurementMode;
  heartRate: number;
  spo2: number;
  hrv: number;
  fatigue: number;
  systolicPressure: number;
  diastolicPressure: number;
  temperature: number;
};

/** Histórico HRV — comando 0x56 (registros de 16 bytes concatenados). */
export type DecodedHrvHistory = {
  type: "0x56";
  hrv: number;
  fatigue: number;
  /** Presente quando o pacote traz múltiplos registros de histórico. */
  recordCount?: number;
};

/** Pacote 0x09 — tempo real (passos, calorias, distância, HR, SpO2, temperatura). */
export type DecodedRealtime = {
  type: "0x09";
  steps: number;
  caloriesKcal: number;
  distanceKm: number;
  movementTimeRaw: number;
  fastMovementTimeRaw: number;
  heartRate: number;
  temperature: number;
  spo2: number;
};

/** Pacote 0x53 — sono (histórico). */
export type DecodedSleep = {
  type: "0x53";
  recordId: number;
  date: string;
  time: string;
  sleepMinutes: number;
};

/** Pacote 0x18 — esporte em tempo real. */
export type DecodedSport = {
  type: "0x18";
  heartRate: number;
  steps: number;
  caloriesRaw: number;
  exerciseTime: number;
};

/**
 * Pacotes de histórico sem decoder específico (0x51, 0x52, 0x54, 0x55, 0x5C,
 * 0x60, 0x62, 0x65, 0x66) e qualquer tipo desconhecido.
 * Salvo como-está para inspeção posterior.
 */
export type DecodedRaw = {
  type: "raw";
  originalType: string;
  bytesReceived: number;
};

export type DecodedPacket =
  | DecodedBattery
  | DecodedMac
  | DecodedFirmware
  | DecodedHealth
  | DecodedHrvHistory
  | DecodedRealtime
  | DecodedSleep
  | DecodedSport
  | DecodedRaw;

export type HealthMeasurementKind = "hrv" | "heartRate" | "spo2" | "temperature";

const HEALTH_TYPE_BYTE: Record<HealthMeasurementKind, number> = {
  hrv: 0x01,
  heartRate: 0x02,
  spo2: 0x03,
  temperature: 0x04,
};

/** Monta pacote BLE 16 bytes: 0x28 AA BB (PDF §33). */
export function buildHealthMeasurementPacket(
  kind: HealthMeasurementKind,
  start: boolean,
): number[] {
  const packet = new Array<number>(16).fill(0);
  packet[0] = 0x28;
  packet[1] = HEALTH_TYPE_BYTE[kind];
  packet[2] = start ? 0x01 : 0x00;
  packet[15] = packet.slice(0, 15).reduce((sum, b) => sum + b, 0) & 0xff;
  return packet;
}

/** Interpreta resposta 0x28 (notify FFF7, 16 bytes). */
export function parseHealthMeasurement(bytes: number[]): DecodedHealth {
  if (bytes.length < 16) {
    throw new PacketDecoderError("Health packet (0x28) requires 16 bytes");
  }
  if (bytes[0] !== 0x28) {
    throw new PacketDecoderError("Not a 0x28 health measurement packet");
  }
  validateCrc(bytes);
  return decodeHealth(bytes);
}

export class PacketDecoderError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PacketDecoderError";
  }
}

export function parsePacketType(packetType: string): number {
  const normalized = packetType.trim().toLowerCase();
  if (!normalized.startsWith("0x")) {
    throw new PacketDecoderError(`Invalid packetType: ${packetType}`);
  }
  const value = parseInt(normalized.slice(2), 16);
  if (Number.isNaN(value)) {
    throw new PacketDecoderError(`Invalid packetType: ${packetType}`);
  }
  return value;
}

export function rawHexToBytes(rawHex: string): number[] {
  const parts = rawHex.trim().split(/\s+/);
  if (parts.length === 0) {
    throw new PacketDecoderError("rawHex is empty");
  }

  const bytes = parts.map((part, index) => {
    if (!/^[0-9A-Fa-f]{2}$/.test(part)) {
      throw new PacketDecoderError(`Invalid hex byte at position ${index}: "${part}"`);
    }
    return parseInt(part, 16);
  });

  return bytes;
}

export function validateCrc(bytes: number[]): void {
  if (bytes.length < 2) {
    throw new PacketDecoderError(
      `Packet too short for CRC validation (got ${bytes.length})`,
    );
  }

  const crcIndex = bytes.length - 1;
  const sum = bytes.slice(0, crcIndex).reduce((acc, b) => acc + b, 0);
  const expectedCrc = sum & 0xff;
  const actualCrc = bytes[crcIndex];

  if (expectedCrc !== actualCrc) {
    throw new PacketDecoderError(
      `CRC mismatch: expected 0x${expectedCrc.toString(16).padStart(2, "0").toUpperCase()}, ` +
        `got 0x${actualCrc.toString(16).padStart(2, "0").toUpperCase()}`,
    );
  }
}

/**
 * Pacotes com CRC no último byte do buffer inteiro.
 * Históricos bulk (0x56, 0x54, …) são múltiplos blocos de 16 bytes — CRC é por bloco.
 */
function skipsCrcValidation(typeByte: number): boolean {
  switch (typeByte) {
    case 0x22: // MAC — resposta 16 bytes do dispositivo
    case 0x28: // Health — notify ativo 16 bytes com CRC
      return false;
    default:
      return true;
  }
}

function tryValidateCrc(bytes: number[]): boolean {
  try {
    validateCrc(bytes);
    return true;
  } catch {
    return false;
  }
}

/** Valida CRC de cada registro de 16 bytes no histórico HRV bulk. */
function crcValidForHrvHistory(bytes: number[]): boolean {
  if (bytes.length < 16) return false;
  if (bytes.length === 16) return tryValidateCrc(bytes);

  let validRecords = 0;
  let totalRecords = 0;

  for (let offset = 0; offset + 16 <= bytes.length; offset += 16) {
    totalRecords++;
    if (tryValidateCrc(bytes.slice(offset, offset + 16))) {
      validRecords++;
    }
  }

  return totalRecords > 0 && validRecords === totalRecords;
}

function formatMacFromBytes(bytes: number[]): string {
  return bytes.map((b) => b.toString(16).padStart(2, "0").toUpperCase()).join(":");
}

function decodeBattery(bytes: number[]): DecodedBattery {
  if (bytes.length < 2) {
    throw new PacketDecoderError("Battery packet (0x13) requires at least 2 bytes");
  }
  return {
    type: "0x13",
    battery: bytes[1],
  };
}

function decodeMac(bytes: number[]): DecodedMac {
  if (bytes.length < 7) {
    throw new PacketDecoderError("MAC packet (0x22) requires at least 7 bytes");
  }
  return {
    type: "0x22",
    mac: formatMacFromBytes(bytes.slice(1, 7)),
  };
}

function decodeFirmware(bytes: number[]): DecodedFirmware {
  return {
    type: "0x27",
    major: bytes[1] ?? 0,
    minor: bytes[2] ?? 0,
    patch: bytes[3] ?? 0,
  };
}

function bcdByte(value: number): number {
  return ((value >> 4) & 0x0f) * 10 + (value & 0x0f);
}

function decodeSleep(bytes: number[]): DecodedSleep {
  const recordId = (bytes[1] ?? 0) | ((bytes[2] ?? 0) << 8);
  const yy = bcdByte(bytes[3] ?? 0);
  const mo = bcdByte(bytes[4] ?? 0);
  const dd = bcdByte(bytes[5] ?? 0);
  const hh = bcdByte(bytes[6] ?? 0);
  const mm = bcdByte(bytes[7] ?? 0);
  const ss = bcdByte(bytes[8] ?? 0);
  const segLen = bytes[9] ?? 0;
  return {
    type: "0x53",
    recordId,
    date: `20${String(yy).padStart(2, "0")}-${String(mo).padStart(2, "0")}-${String(dd).padStart(2, "0")}`,
    time: `${String(hh).padStart(2, "0")}:${String(mm).padStart(2, "0")}:${String(ss).padStart(2, "0")}`,
    sleepMinutes: segLen * 5,
  };
}

function decodeSport(bytes: number[]): DecodedSport {
  return {
    type: "0x18",
    heartRate: bytes[1] ?? 0,
    steps: readUint32LE(bytes, 2),
    caloriesRaw: readUint32LE(bytes, 6),
    exerciseTime: readUint32LE(bytes, 10),
  };
}

/**
 * 2208A often sends bytes 6–7 as fixed 0x7B/0x49 (123/73) with HRV 0x4A and fatigue 0x0D.
 * That block is a stored calibration baseline, not a live BP reading (same for every user).
 */
function readUint32LE(bytes: number[], offset: number): number {
  return (
    (bytes[offset] ?? 0) |
    ((bytes[offset + 1] ?? 0) << 8) |
    ((bytes[offset + 2] ?? 0) << 16) |
    ((bytes[offset + 3] ?? 0) << 24)
  ) >>> 0;
}

export function healthMeasurementMode(typeByte: number): HealthMeasurementMode {
  switch (typeByte) {
    case 0x01:
      return "hrv";
    case 0x02:
      return "heart";
    case 0x03:
      return "oxygen";
    case 0x04:
      return "temperature";
    case 0x05:
      return "blood_pressure";
    default:
      return "unknown";
  }
}

function isPlausibleBloodPressure(systolic: number, diastolic: number): boolean {
  return (
    systolic > 0 &&
    diastolic > 0 &&
    systolic < 250 &&
    diastolic < 200 &&
    systolic > diastolic
  );
}

function decodeBloodPressure(bytes: number[]): {
  systolicPressure: number;
  diastolicPressure: number;
} {
  const systolic = bytes[6] ?? 0;
  const diastolic = bytes[7] ?? 0;

  const isCalibrationPlaceholder =
    systolic === 0x7b &&
    diastolic === 0x49 &&
    bytes[4] === 0x4a &&
    bytes[5] === 0x0d;

  /** Valor fixo observado na 2208A em quase todo notify — não é leitura real. */
  const isStaleBraceletBp = systolic === 0x75 && diastolic === 0x44;

  if (
    isCalibrationPlaceholder ||
    isStaleBraceletBp ||
    (systolic === 0 && diastolic === 0) ||
    !isPlausibleBloodPressure(systolic, diastolic)
  ) {
    return { systolicPressure: 0, diastolicPressure: 0 };
  }

  return { systolicPressure: systolic, diastolicPressure: diastolic };
}

function decodeHrvHistory(bytes: number[]): DecodedHrvHistory {
  if (bytes.length < 13) {
    throw new PacketDecoderError("HRV history packet (0x56) requires at least 13 bytes");
  }

  // PDF §27: registro mínimo 15 bytes; notify BLE costuma vir em blocos de 16 com CRC.
  const recordLen = bytes.length % 16 === 0 ? 16 : 15;
  const recordCount = Math.max(1, Math.floor(bytes.length / recordLen));
  const recordStart = (recordCount - 1) * recordLen;

  return {
    type: "0x56",
    hrv: bytes[recordStart + 9] ?? 0,
    fatigue: bytes[recordStart + 12] ?? 0,
    recordCount: recordCount > 1 ? recordCount : undefined,
  };
}

/**
 * PDF §33 — resposta 0x28 (16 bytes):
 * [1]=tipo AA, [2]=BB HR, [3]=CC SpO2, [4]=DD HRV, [5]=EE fadiga,
 * [6]=FF sistólica, [7]=GG diastólica, [8..9]=HH II temperatura (/10).
 */
function decodeHealth(bytes: number[]): DecodedHealth {
  if (bytes.length < 10) {
    throw new PacketDecoderError("Health packet (0x28) requires at least 10 bytes");
  }

  const measurementType = bytes[1];
  const bp = decodeBloodPressure(bytes);
  const temperatureRaw = ((bytes[8] ?? 0) << 8) | (bytes[9] ?? 0);

  return {
    type: "0x28",
    measurementType,
    measurementMode: healthMeasurementMode(measurementType),
    heartRate: bytes[2] ?? 0,
    spo2: bytes[3] ?? 0,
    hrv: bytes[4] ?? 0,
    fatigue: bytes[5] ?? 0,
    systolicPressure: bp.systolicPressure,
    diastolicPressure: bp.diastolicPressure,
    temperature: temperatureRaw > 0 ? temperatureRaw / 10 : 0,
  };
}

function pickPositive(current: number, next: number): number {
  return next > 0 ? next : current;
}

/** Combina leituras parciais 0x28/0x56 do mesmo ciclo de medição. */
export function mergeHealthReadings(
  decodedList: Array<DecodedHealth | DecodedHrvHistory>,
): DecodedHealth {
  const merged: DecodedHealth = {
    type: "0x28",
    measurementType: 0,
    measurementMode: "unknown",
    heartRate: 0,
    spo2: 0,
    hrv: 0,
    fatigue: 0,
    systolicPressure: 0,
    diastolicPressure: 0,
    temperature: 0,
  };

  for (const decoded of decodedList) {
    if (decoded.type === "0x56") {
      merged.hrv = pickPositive(merged.hrv, decoded.hrv);
      merged.fatigue = pickPositive(merged.fatigue, decoded.fatigue);
      continue;
    }

    merged.heartRate = pickPositive(merged.heartRate, decoded.heartRate);
    merged.spo2 = pickPositive(merged.spo2, decoded.spo2);
    merged.hrv = pickPositive(merged.hrv, decoded.hrv);
    merged.fatigue = pickPositive(merged.fatigue, decoded.fatigue);
    merged.systolicPressure = pickPositive(merged.systolicPressure, decoded.systolicPressure);
    merged.diastolicPressure = pickPositive(merged.diastolicPressure, decoded.diastolicPressure);
    merged.temperature = pickPositive(merged.temperature, decoded.temperature);
  }

  return merged;
}

/**
 * O ESP32 trunca todos os pacotes em 16 bytes via storeHex().
 * O pacote real do 0x09 tem 26 bytes, mas só os primeiros 16 chegam.
 * Lê o que estiver disponível e usa 0 para campos ausentes.
 */
function decodeRealtime(bytes: number[]): DecodedRealtime {
  if (bytes.length < 2) {
    throw new PacketDecoderError(
      `Realtime packet (0x09) requires at least 2 bytes (got ${bytes.length})`,
    );
  }

  const temperatureRaw =
    bytes.length >= 24 ? ((bytes[22] ?? 0) | ((bytes[23] ?? 0) << 8)) : 0;

  return {
    type: "0x09",
    steps: bytes.length >= 5 ? readUint32LE(bytes, 1) : 0,
    caloriesKcal: bytes.length >= 9 ? readUint32LE(bytes, 5) / 100 : 0,
    distanceKm: bytes.length >= 13 ? readUint32LE(bytes, 9) / 100 : 0,
    movementTimeRaw: bytes.length >= 17 ? readUint32LE(bytes, 13) : 0,
    fastMovementTimeRaw: bytes.length >= 21 ? readUint32LE(bytes, 17) : 0,
    heartRate: bytes.length >= 22 ? (bytes[21] ?? 0) : 0,
    temperature: temperatureRaw / 10,
    spo2: bytes.length >= 25 ? (bytes[24] ?? 0) : 0,
  };
}

export function decodePacket(packetType: string, rawHex: string): {
  bytes: number[];
  decoded: DecodedPacket;
  crcValid: boolean;
} {
  const typeByte = parsePacketType(packetType);
  const bytes = rawHexToBytes(rawHex);

  const skipPacketCrc = skipsCrcValidation(typeByte);
  let crcValid = false;

  if (!skipPacketCrc) {
    validateCrc(bytes);
    crcValid = true;
  } else if (typeByte === 0x56) {
    crcValid = crcValidForHrvHistory(bytes);
  }

  if (bytes[0] !== typeByte) {
    throw new PacketDecoderError(
      `packetType ${packetType} does not match first byte 0x${bytes[0].toString(16).padStart(2, "0").toUpperCase()}`,
    );
  }

  let decoded: DecodedPacket;

  switch (typeByte) {
    case 0x13:
      decoded = decodeBattery(bytes);
      break;
    case 0x22:
      decoded = decodeMac(bytes);
      break;
    case 0x27:
      decoded = decodeFirmware(bytes);
      break;
    case 0x28:
      decoded = decodeHealth(bytes);
      break;
    case 0x53:
      decoded = decodeSleep(bytes);
      break;
    case 0x18:
      decoded = decodeSport(bytes);
      break;
    case 0x56:
      decoded = decodeHrvHistory(bytes);
      break;
    case 0x09:
      decoded = decodeRealtime(bytes);
      break;
    default:
      // Tipos de histórico (0x51, 0x52, 0x54, 0x55, 0x5C, 0x60, 0x62, 0x65, 0x66)
      // e quaisquer outros tipos futuros — salva como raw para inspeção.
      decoded = {
        type: "raw",
        originalType: `0x${typeByte.toString(16).padStart(2, "0")}`,
        bytesReceived: bytes.length,
      };
  }

  return { bytes, decoded, crcValid };
}
