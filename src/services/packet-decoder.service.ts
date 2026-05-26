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

export type DecodedPacket = DecodedBattery | DecodedMac | DecodedHealth | DecodedRealtime;

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

  if (isCalibrationPlaceholder || (systolic === 0 && diastolic === 0)) {
    return { systolicPressure: 0, diastolicPressure: 0 };
  }

  return { systolicPressure: systolic, diastolicPressure: diastolic };
}

function decodeHealth(bytes: number[]): DecodedHealth {
  if (bytes.length < 10) {
    throw new PacketDecoderError("Health packet (0x28) requires at least 10 bytes");
  }

  const temperatureRaw = bytes[8] | (bytes[9] << 8);
  const { systolicPressure, diastolicPressure } = decodeBloodPressure(bytes);

  const measurementType = bytes[1];

  return {
    type: "0x28",
    measurementType,
    measurementMode: healthMeasurementMode(measurementType),
    heartRate: bytes[2],
    spo2: bytes[3],
    hrv: bytes[4],
    fatigue: bytes[5],
    systolicPressure,
    diastolicPressure,
    temperature: temperatureRaw / 10,
  };
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
    case 0x09:
      decoded = decodeRealtime(bytes);
      break;
    default:
      throw new PacketDecoderError(`Unsupported packet type: ${typeHex}`);
  }

  return { bytes, decoded };
}
