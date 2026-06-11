import assert from "node:assert/strict";
import { test } from "node:test";

import { decodePacket } from "./packet-decoder.service.js";
import {
  buildConsolidatedSnapshot,
  hasSnapshotVitals,
  mergeVitals,
  parsePacket,
  validateVitals,
  type InboundPacket,
  type ParsedPacket,
} from "./vitals-consolidation.service.js";

function snapshot(metrics: Record<string, number>): InboundPacket {
  return { packetType: "SNAPSHOT_VITALS", metrics };
}

function health28(metrics: Record<string, number>): InboundPacket {
  return { packetType: "0x28", metrics };
}

// ── Rule 1: SNAPSHOT_VITALS é a fonte principal ──────────────────────────────
test("SNAPSHOT_VITALS tem prioridade sobre raw packets", () => {
  const result = buildConsolidatedSnapshot([
    snapshot({ bpm: 83, spo2: 97, temperature: 36.6, hrv: 75, fatigue: 41 }),
    health28({ hrv: 53, fatigue: 12, bpm: 0 }), // raw com HRV "fantasma"
  ]);

  assert.equal(result.usedSnapshotVitals, true);
  assert.equal(result.vitals.hrv, 75); // nunca 53
  assert.equal(result.vitals.fatigue, 41);
  assert.equal(result.vitals.heartRate, 83);
  assert.equal(result.sources.hrv, "SNAPSHOT_VITALS");
  assert.equal(result.stats.ignoredPartialSnapshots, 1);
});

// ── Rule 3: zero não sobrescreve valor válido ────────────────────────────────
test("zero não sobrescreve valor válido", () => {
  const result = buildConsolidatedSnapshot([
    health28({ bpm: 83, temperature: 36.6 }),
    health28({ bpm: 0, temperature: 0 }),
  ]);

  assert.equal(result.vitals.heartRate, 83);
  assert.equal(result.vitals.temperature, 36.6);
});

// ── Rule 4: inválido vira null, nunca 0 ──────────────────────────────────────
test("vitais ausentes/inválidos viram null, nunca 0", () => {
  const result = buildConsolidatedSnapshot([health28({ bpm: 83 })]);

  assert.equal(result.vitals.heartRate, 83);
  assert.equal(result.vitals.spo2, null);
  assert.equal(result.vitals.temperature, null);
  assert.equal(result.vitals.hrv, null);
  // nenhum vital pode ser 0
  for (const value of Object.values(result.vitals)) {
    assert.notEqual(value, 0);
  }
});

// ── Rule 5: ranges + regra de pressão ────────────────────────────────────────
test("validateVitals aplica faixas e regra de pulse pressure", () => {
  // SpO2 110 inválido, HRV 250 inválido, temp 50 inválido
  const a = validateVitals({ spo2: 110, hrv: 250, temperature: 50 });
  assert.equal(a.spo2, null);
  assert.equal(a.hrv, null);
  assert.equal(a.temperature, null);

  // diferença sistólica-diastólica < 15 -> ambas null
  const b = validateVitals({ systolicPressure: 120, diastolicPressure: 110 });
  assert.equal(b.systolicPressure, null);
  assert.equal(b.diastolicPressure, null);

  // pressão válida
  const c = validateVitals({ systolicPressure: 123, diastolicPressure: 78 });
  assert.equal(c.systolicPressure, 123);
  assert.equal(c.diastolicPressure, 78);
});

// ── Rule 6: pressão só de 0x28 ou SNAPSHOT_VITALS ────────────────────────────
test("mergeVitals ignora pressão vinda de fonte não permitida", () => {
  const fromHistory: ParsedPacket = {
    packetType: "0x54",
    source: "0x54_HISTORY",
    vitals: { systolicPressure: 120, diastolicPressure: 80, heartRate: 70 },
  };

  const merged = mergeVitals({}, fromHistory);
  assert.equal(merged.systolicPressure, undefined); // 0x54 não pode dar pressão
  assert.equal(merged.heartRate?.value, 70); // mas pode dar BPM
});

test("pressão consolidada vem do 0x28", () => {
  const result = buildConsolidatedSnapshot([
    health28({ bpm: 83, bloodPressureSystolic: 123, bloodPressureDiastolic: 78 }),
  ]);
  assert.equal(result.vitals.systolicPressure, 123);
  assert.equal(result.vitals.diastolicPressure, 78);
  assert.equal(result.sources.bloodPressure, "0x28_REALTIME");
  assert.equal(result.quality.bloodPressure, "estimated");
});

// ── Rule 7/11: 0x56 não pode usar byte desalinhado ───────────────────────────
test("0x56 com registro único lê HRV/fadiga alinhados", () => {
  const { decoded } = decodePacket("0x56", "56 01 00 1A 06 0B 0C 00 00 4B 00 00 29 00 00 00");
  assert.equal(decoded.type, "0x56");
  if (decoded.type === "0x56") {
    assert.equal(decoded.hrv, 75); // 0x4B
    assert.equal(decoded.fatigue, 41); // 0x29
  }
});

test("0x56 multi-registro pega o último registro válido (75/41, nunca 53)", () => {
  const rawHex =
    "56 01 00 1A 06 0B 0C 00 00 4C 00 00 21 " + // HRV 76 fadiga 33
    "56 02 00 1A 06 0B 0C 00 00 80 00 00 0D " + // HRV 128 fadiga 13
    "56 03 00 1A 06 0B 0C 00 00 4B 00 00 29"; // HRV 75 fadiga 41
  const { decoded } = decodePacket("0x56", rawHex);
  assert.equal(decoded.type, "0x56");
  if (decoded.type === "0x56") {
    assert.equal(decoded.hrv, 75);
    assert.equal(decoded.fatigue, 41);
    assert.notEqual(decoded.hrv, 53); // jamais byte desalinhado
  }
});

// ── Rule 2/8: batch parcial não gera leitura principal incompleta com zeros ───
test("batch com raw parcial consolida em UMA leitura (sem zeros falsos)", () => {
  const result = buildConsolidatedSnapshot([
    health28({ bpm: 83 }),
    health28({ spo2: 97 }),
    health28({ temperature: 36.6 }),
  ]);

  assert.equal(result.vitals.heartRate, 83);
  assert.equal(result.vitals.spo2, 97);
  assert.equal(result.vitals.temperature, 36.6);
  assert.equal(result.usedSnapshotVitals, false);
  // hrv ausente -> null, não 0
  assert.equal(result.vitals.hrv, null);
});

// ── Rule 8c: snapshot completo não é degradado por raw parcial ────────────────
test("raw parcial com zeros não degrada um SNAPSHOT_VITALS completo", () => {
  const result = buildConsolidatedSnapshot([
    snapshot({ bpm: 83, spo2: 97, temperature: 36.6, hrv: 75, fatigue: 41 }),
    health28({ bpm: 0, spo2: 0, temperature: 0 }), // POST parcial posterior
  ]);

  assert.equal(result.usedSnapshotVitals, true);
  assert.equal(result.vitals.heartRate, 83);
  assert.equal(result.vitals.spo2, 97);
  assert.equal(result.vitals.hrv, 75);
  assert.equal(result.quality.snapshotComplete, true);
});

// ── Prioridade de fonte: 0x54 > 0x28 para BPM ────────────────────────────────
test("BPM prefere histórico 0x54 sobre 0x28", () => {
  const from28: ParsedPacket = { packetType: "0x28", source: "0x28_REALTIME", vitals: { heartRate: 70 } };
  const from54: ParsedPacket = { packetType: "0x54", source: "0x54_HISTORY", vitals: { heartRate: 88 } };

  let merged = mergeVitals({}, from28);
  merged = mergeVitals(merged, from54);
  assert.equal(merged.heartRate?.value, 88);
  assert.equal(merged.heartRate?.source, "0x54_HISTORY");

  // ordem inversa: 0x54 ainda vence
  let merged2 = mergeVitals({}, from54);
  merged2 = mergeVitals(merged2, from28);
  assert.equal(merged2.heartRate?.value, 88);
});

// ── parsePacket / hasSnapshotVitals ──────────────────────────────────────────
test("hasSnapshotVitals detecta o pacote consolidado", () => {
  assert.equal(hasSnapshotVitals([health28({ bpm: 80 })]), false);
  assert.equal(hasSnapshotVitals([snapshot({ bpm: 80 })]), true);
});

test("parsePacket ignora pacotes sem vitais (ex.: bateria)", () => {
  assert.equal(parsePacket({ packetType: "0x13", rawHex: "13 53" }), null);
});
