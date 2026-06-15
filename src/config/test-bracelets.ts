/**
 * Cadastro fixo: MAC, label e dados do paciente (nome, idade, email).
 * Medições (HR, SpO2, bateria, etc.) vêm dos pacotes reais no banco — não ficam aqui.
 */
export type PatientInfo = {
  patientId: string;
  patientName: string;
  age: number;
  email: string;
  leito?: string;
};

export type TestBracelet = {
  deviceMac: string;
  label: string;
  patient: PatientInfo;
};

/** Nomes para dropdown no Swagger (GET /bracelets/reports/vitals). */
export const REPORT_PATIENT_NAMES = [
  "Matheus dev",
  "Jurandir",
  "Anderson",
  "Luciene",
] as const;

export function findBraceletByPatientName(name: string): TestBracelet | undefined {
  const key = name.trim().toLowerCase();
  return TEST_BRACELETS.find((b) => b.patient.patientName.toLowerCase() === key);
}

export const TEST_BRACELETS: TestBracelet[] = [
  {
    deviceMac: "E6:64:0D:30:D3:F9",
    label: "Bracelet 1",
    patient: {
      patientId: "P001",
      patientName: "Matheus dev",
      age: 22,
      email: "Ana.trindade@anery.com.br",
      leito: "Leito 0",
    },
  },
  {
    deviceMac: "DB:31:0D:30:7B:F8",
    label: "Bracelet 2",
    patient: {
      patientId: "P002",
      patientName: "Jurandir",
      age: 40,
      email: "carlos.mozer@pcpsaude.com.br",
      leito: "Leito 215",
    },
  },
  {
    deviceMac: "F4:41:0D:30:6E:F7",
    label: "Bracelet 3",
    patient: {
      patientId: "P003",
      patientName: "Anderson",
      age: 24,
      email: "Escala@anery.com.br",
      leito: "Leito 101",
    },
  },
  {
    deviceMac: "EF:7A:0D:30:B3:FA",
    label: "Bracelet 4",
    patient: {
      patientId: "P004",
      patientName: "Luciene",
      age: 20,
      email: "Daniela.silva@anery.com.br",
      leito: "Leito 214",
    },
  },
];
