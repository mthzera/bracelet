import { z } from "zod";

const macAddressRegex = /^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$/;

const packetTypeRegex = /^0x[0-9A-Fa-f]{1,2}$/i;

const rawHexRegex = /^([0-9A-Fa-f]{2}\s*)+$/;

export const packetPayloadSchema = z.object({
  deviceMac: z
    .string()
    .regex(macAddressRegex, "deviceMac must be a valid MAC address (AA:BB:CC:DD:EE:FF)"),
  packetType: z
    .string()
    .regex(packetTypeRegex, 'packetType must be a hex literal (e.g. "0x28")'),
  rawHex: z
    .string()
    .min(1)
    .regex(rawHexRegex, "rawHex must be space-separated hex byte pairs"),
  source: z.string().min(1),
});

export type PacketPayload = z.infer<typeof packetPayloadSchema>;
