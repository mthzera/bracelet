export type DecodedBattery = {
  type: "0x13";
  battery: number;
};

export type DecodedMac = {
  type: "0x22";
  mac: string;
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

/** Histórico HRV — comando 0x56 (D1=HRV, D4=fadiga). */
export type DecodedHrvHistory = {
  type: "0x56";
  hrv: number;
  fatigue: number;
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

export type DecodedPacket =
  | DecodedBattery
  | DecodedMac
  | DecodedHealth
  | DecodedHrvHistory
  | DecodedRealtime;

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
  return {
    type: "0x56",
    hrv: bytes[9] ?? 0,
    fatigue: bytes[12] ?? 0,
  };
}

function decodeHealth(bytes: number[]): DecodedHealth {
  if (bytes.length < 10) {
    throw new PacketDecoderError("Health packet (0x28) requires at least 10 bytes");
  }

  const measurementType = bytes[1];
  const measurementMode = healthMeasurementMode(measurementType);
  const temperatureRaw = bytes[8] | (bytes[9] << 8);
  const temperature = temperatureRaw > 0 ? temperatureRaw / 10 : 0;

  let heartRate = 0;
  let spo2 = 0;
  let hrv = 0;
  let fatigue = 0;
  let systolicPressure = 0;
  let diastolicPressure = 0;

  switch (measurementType) {
    case 0x01: {
      if (bytes[4] > 0) hrv = bytes[4];
      if (bytes[5] > 0) fatigue = bytes[5];
      break;
    }
    case 0x02: {
      if (bytes[2] > 0) heartRate = bytes[2];
      if (heartRate > 0) {
        const bp = decodeBloodPressure(bytes);
        systolicPressure = bp.systolicPressure;
        diastolicPressure = bp.diastolicPressure;
      }
      break;
    }
    case 0x03: {
      if (bytes[3] > 0) spo2 = bytes[3];
      if (bytes[2] > 0) heartRate = bytes[2];
      break;
    }
    case 0x04: {
      break;
    }
    case 0x05: {
      const bp = decodeBloodPressure(bytes);
      systolicPressure = bp.systolicPressure;
      diastolicPressure = bp.diastolicPressure;
      break;
    }
    default: {
      if (bytes[2] > 0) heartRate = bytes[2];
      if (bytes[3] > 0) spo2 = bytes[3];
      if (bytes[4] > 0) hrv = bytes[4];
      if (bytes[5] > 0) fatigue = bytes[5];
      const bp = decodeBloodPressure(bytes);
      systolicPressure = bp.systolicPressure;
      diastolicPressure = bp.diastolicPressure;
    }
  }

  return {
    type: "0x28",
    measurementType,
    measurementMode,
    heartRate,
    spo2,
    hrv,
    fatigue,
    systolicPressure,
    diastolicPressure,
    temperature,
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

function decodeRealtime(bytes: number[]): DecodedRealtime {
  if (bytes.length < 26) {
    throw new PacketDecoderError(
      `Realtime packet (0x09) requires at least 26 bytes (got ${bytes.length})`,
    );
  }

  const temperatureRaw = bytes[22] | ((bytes[23] ?? 0) << 8);

  return {
    type: "0x09",
    steps: readUint32LE(bytes, 1),
    caloriesKcal: readUint32LE(bytes, 5) / 100,
    distanceKm: readUint32LE(bytes, 9) / 100,
    movementTimeRaw: readUint32LE(bytes, 13),
    fastMovementTimeRaw: readUint32LE(bytes, 17),
    heartRate: bytes[21],
    temperature: temperatureRaw / 10,
    spo2: bytes[24],
  };
}

export function decodePacket(packetType: string, rawHex: string): {
  bytes: number[];
  decoded: DecodedPacket;
} {
  const typeByte = parsePacketType(packetType);
  const bytes = rawHexToBytes(rawHex);

  validateCrc(bytes);

  if (bytes[0] !== typeByte) {
    throw new PacketDecoderError(
      `packetType ${packetType} does not match first byte 0x${bytes[0].toString(16).padStart(2, "0").toUpperCase()}`,
    );
  }

  const typeHex = `0x${typeByte.toString(16).padStart(2, "0")}` as DecodedPacket["type"];

  let decoded: DecodedPacket;

  switch (typeByte) {
    case 0x13:
      decoded = decodeBattery(bytes);
      break;
    case 0x22:
      decoded = decodeMac(bytes);
      break;
    case 0x28:
      decoded = decodeHealth(bytes);
      break;
    case 0x56:
      decoded = decodeHrvHistory(bytes);
      break;
    case 0x09:
      decoded = decodeRealtime(bytes);
      break;
    default:
      throw new PacketDecoderError(`Unsupported packet type: ${typeHex}`);
  }

  return { bytes, decoded };
}
