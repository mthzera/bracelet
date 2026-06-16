/** Estágios de sono (protocolo J-Style 0x53, segmentos após o cabeçalho). */
export type SleepStage = "awake" | "rem" | "light" | "deep" | "nap" | "unknown";

export type SleepSegment = {
  stage: SleepStage;
  minutes: number;
};

export type SleepStageTotals = {
  awake: number;
  rem: number;
  light: number;
  deep: number;
  nap: number;
  unknown: number;
};

export type SleepDetail = {
  recordId: number;
  date: string;
  startTime: string;
  endTime: string;
  sleepMinutes: number;
  inBedMinutes: number;
  quality: number | null;
  segments: SleepSegment[];
  totals: SleepStageTotals;
};

const STAGE_MAP: Record<number, SleepStage> = {
  0x01: "deep",
  0x02: "light",
  0x03: "rem",
  0x04: "awake",
  0x05: "nap",
};

function bcd(b: number): number {
  return ((b >> 4) * 10) + (b & 0x0f);
}

function emptyTotals(): SleepStageTotals {
  return { awake: 0, rem: 0, light: 0, deep: 0, nap: 0, unknown: 0 };
}

function addToTotals(totals: SleepStageTotals, stage: SleepStage, minutes: number): void {
  totals[stage] += minutes;
}

function formatDate(y: number, mo: number, d: number): string {
  return `20${String(y).padStart(2, "0")}-${String(mo).padStart(2, "0")}-${String(d).padStart(2, "0")}`;
}

function formatTime(h: number, mi: number, s: number): string {
  return `${String(h).padStart(2, "0")}:${String(mi).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

/** Soma minutos a uma data/hora BCD (suporta virada de dia). */
export function addMinutesToDateTime(
  date: string,
  time: string,
  minutes: number,
): { date: string; time: string } {
  const [y, mo, d] = date.slice(2).split("-").map((v) => parseInt(v, 10));
  const [h, mi, s] = time.split(":").map((v) => parseInt(v, 10));

  let totalMin = h * 60 + mi + minutes;
  let day = d;
  let month = mo;
  let year = y;

  while (totalMin >= 24 * 60) {
    totalMin -= 24 * 60;
    day += 1;
    const daysInMonth = new Date(2000 + year, month, 0).getDate();
    if (day > daysInMonth) {
      day = 1;
      month += 1;
      if (month > 12) {
        month = 1;
        year += 1;
      }
    }
  }

  const eh = Math.floor(totalMin / 60);
  const em = totalMin % 60;

  return {
    date: formatDate(year, month, day),
    time: formatTime(eh, em, s),
  };
}

function parseSegments(bytes: number[], offset: number): {
  segments: SleepSegment[];
  totals: SleepStageTotals;
  inBedMinutes: number;
  sleepMinutes: number;
} {
  const segments: SleepSegment[] = [];
  const totals = emptyTotals();
  let inBedMinutes = 0;
  let sleepMinutes = 0;

  for (let i = offset; i + 1 < bytes.length; i += 2) {
    if (bytes[i] === 0x53 && i > offset) break;

    const stageByte = bytes[i] ?? 0;
    const dur = bytes[i + 1] ?? 0;
    if (dur === 0) continue;

    const stage = STAGE_MAP[stageByte] ?? "unknown";
    segments.push({ stage, minutes: dur });
    addToTotals(totals, stage, dur);
    inBedMinutes += dur;
    if (stage !== "awake" && stage !== "unknown") {
      sleepMinutes += dur;
    }
  }

  return { segments, totals, inBedMinutes, sleepMinutes };
}

function sleepRecordLength(bytes: number[], offset: number): number {
  if (offset + 10 > bytes.length) return 0;
  if (bytes.length <= offset + 10) return bytes.length - offset;

  for (let i = offset + 10; i < bytes.length; i++) {
    if (bytes[i] === 0x53) return i - offset;
  }
  return bytes.length - offset;
}

/** Interpreta um registro 0x53 (cabeçalho + segmentos). */
export function parseSleepRecord(bytes: number[], offset = 0): SleepDetail | null {
  const recordLen = sleepRecordLength(bytes, offset);
  if (recordLen < 10 || bytes[offset] !== 0x53) return null;

  const slice = bytes.slice(offset, offset + recordLen);
  const recordId = (slice[1] ?? 0) | ((slice[2] ?? 0) << 8);
  const yy = bcd(slice[3] ?? 0);
  const mo = bcd(slice[4] ?? 0);
  const dd = bcd(slice[5] ?? 0);
  const hh = bcd(slice[6] ?? 0);
  const mm = bcd(slice[7] ?? 0);
  const ss = bcd(slice[8] ?? 0);
  const qualityByte = slice[9] ?? 0;
  const date = formatDate(yy, mo, dd);
  const startTime = formatTime(hh, mm, ss);

  const hasSegments = slice.length > 11;
  let segments: SleepSegment[] = [];
  let totals = emptyTotals();
  let inBedMinutes = 0;
  let sleepMinutes = 0;
  let quality: number | null = null;

  if (hasSegments) {
    const parsed = parseSegments(slice, 10);
    segments = parsed.segments;
    totals = parsed.totals;
    inBedMinutes = parsed.inBedMinutes;
    sleepMinutes = parsed.sleepMinutes;
    quality = qualityByte > 0 && qualityByte <= 100 ? qualityByte : null;
  }

  if (sleepMinutes <= 0 && qualityByte > 0) {
    sleepMinutes = qualityByte * 5;
    inBedMinutes = sleepMinutes;
  }

  if (sleepMinutes < 5) return null;

  const end = addMinutesToDateTime(date, startTime, inBedMinutes > 0 ? inBedMinutes : sleepMinutes);

  return {
    recordId,
    date,
    startTime,
    endTime: end.time,
    sleepMinutes,
    inBedMinutes: inBedMinutes > 0 ? inBedMinutes : sleepMinutes,
    quality,
    segments,
    totals,
  };
}

/** Escolhe o melhor registro 0x53 em um buffer (pode conter vários). */
export function parseBestSleepPacket(bytes: number[]): SleepDetail | null {
  let best: SleepDetail | null = null;

  for (let offset = 0; offset + 10 < bytes.length; offset++) {
    if (bytes[offset] !== 0x53) continue;
    const parsed = parseSleepRecord(bytes, offset);
    if (!parsed) continue;
    if (!best || parsed.sleepMinutes > best.sleepMinutes) {
      best = parsed;
    }
    offset += sleepRecordLength(bytes, offset) - 1;
  }

  return best;
}

/** Converte sleepDetail vindo do SNAPSHOT_VITALS.metrics. */
export function sleepDetailFromMetrics(metrics: Record<string, unknown> | undefined): SleepDetail | null {
  if (!metrics || typeof metrics !== "object") return null;
  const raw = metrics.sleepDetail;
  if (!raw || typeof raw !== "object") return null;

  const d = raw as Record<string, unknown>;
  const sleepMinutes = typeof d.sleepMinutes === "number" ? d.sleepMinutes : 0;
  if (sleepMinutes < 5) return null;

  const totals = emptyTotals();
  const segments: SleepSegment[] = [];

  if (Array.isArray(d.segments)) {
    for (const seg of d.segments) {
      if (!seg || typeof seg !== "object") continue;
      const s = seg as Record<string, unknown>;
      const stage = typeof s.stage === "string" ? (s.stage as SleepStage) : "unknown";
      const minutes = typeof s.minutes === "number" ? s.minutes : 0;
      if (minutes <= 0) continue;
      segments.push({ stage, minutes });
      if (stage in totals) addToTotals(totals, stage, minutes);
      else totals.unknown += minutes;
    }
  }

  if (d.totals && typeof d.totals === "object") {
    const t = d.totals as Record<string, unknown>;
    for (const key of Object.keys(totals) as (keyof SleepStageTotals)[]) {
      if (typeof t[key] === "number") totals[key] = t[key] as number;
    }
  }

  return {
    recordId: typeof d.recordId === "number" ? d.recordId : 0,
    date: typeof d.date === "string" ? d.date : "",
    startTime: typeof d.startTime === "string" ? d.startTime : "",
    endTime: typeof d.endTime === "string" ? d.endTime : "",
    sleepMinutes,
    inBedMinutes: typeof d.inBedMinutes === "number" ? d.inBedMinutes : sleepMinutes,
    quality: typeof d.quality === "number" ? d.quality : null,
    segments,
    totals,
  };
}
