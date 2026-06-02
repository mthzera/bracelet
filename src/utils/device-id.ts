const macWithColons = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;
const macWithDashes = /^([0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2}$/;

/** Normalizes device id path param to uppercase MAC with colons. */
export function normalizeDeviceId(raw: string): string {
  const trimmed = raw.trim();

  if (macWithColons.test(trimmed)) {
    return trimmed.toUpperCase();
  }

  if (macWithDashes.test(trimmed)) {
    return trimmed.replace(/-/g, ":").toUpperCase();
  }

  throw new Error("deviceId must be a MAC address (AA:BB:CC:DD:EE:FF or AA-BB-CC-DD-EE-FF)");
}
