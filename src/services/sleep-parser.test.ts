import assert from "node:assert/strict";
import { test } from "node:test";

import { addMinutesToDateTime, parseBestSleepPacket, parseSleepRecord } from "./sleep-parser.service.js";

test("parses segment-based 0x53 packet", () => {
  const bytes = [
    0x53, 0x01, 0x00,
    0x26, 0x06, 0x16,
    0x23, 0x44, 0x00,
    0x55,
    0x01, 0x85,
    0x02, 0xa6,
    0x03, 0x7d,
    0x04, 0x0d,
  ];

  const detail = parseSleepRecord(bytes);
  assert.ok(detail);
  assert.equal(detail!.sleepMinutes, 0x85 + 0xa6 + 0x7d);
  assert.equal(detail!.inBedMinutes, 0x85 + 0xa6 + 0x7d + 0x0d);
  assert.equal(detail!.totals.deep, 0x85);
  assert.equal(detail!.totals.awake, 0x0d);
  assert.equal(detail!.segments.length, 4);
});

test("falls back to quality byte * 5 for short packets", () => {
  const bytes = [0x53, 0x02, 0x00, 0x26, 0x06, 0x16, 0x07, 0x12, 0x00, 0x29];
  const detail = parseSleepRecord(bytes);
  assert.equal(detail?.sleepMinutes, 0x29 * 5);
});

test("adds minutes across midnight", () => {
  const end = addMinutesToDateTime("2026-06-16", "23:44:00", 489);
  assert.equal(end.date, "2026-06-17");
  assert.equal(end.time, "07:53:00");
});

test("picks best record from multi-record buffer", () => {
  const short = [0x53, 0x01, 0x00, 0x26, 0x06, 0x15, 0x08, 0x00, 0x00, 0x10];
  const long = [
    0x53, 0x02, 0x00, 0x26, 0x06, 0x16, 0x23, 0x44, 0x00, 0x55,
    0x01, 0x50, 0x02, 0x80, 0x03, 0x40, 0x04, 0x05,
  ];
  const best = parseBestSleepPacket([...short, ...long]);
  assert.equal(best?.recordId, 2);
  assert.ok(best!.sleepMinutes > 0x10 * 5);
});
