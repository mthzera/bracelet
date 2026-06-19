/**
 * Cadastro fixo: MAC, label e dados do paciente (nome, idade, email, leito, atendimento, etc.).
 * Medições (HR, SpO2, bateria, etc.) vêm dos pacotes reais no banco — não ficam aqui.
 */
export type PatientInfo = {
  patientId: string;
  patientName: string;
  age: number;
  email: string;
  leito?: string;
  idatendimento?: number | null;
  convenio?: string;
  unidade?: string;
  internacao?: string;
  perfil?: string;
  dataEntrada?: string;
};

export type TestBracelet = {
  deviceMac: string;
  label: string;
  patient: PatientInfo;
};

/** Nomes para dropdown no Swagger (GET /bracelets/reports/vitals). */
export const REPORT_PATIENT_NAMES = [
  "Matheus Dev",
  "Jurandir Filadelfo Dos Santos",
  "Anderson Vieira Rodrigues Lino",
  "Luciene Marques Lizardo",
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
      patientId: "0",
      patientName: "Matheus Dev",
      age: 22,
      email: "Ana.trindade@anery.com.br",
      leito: "Leito 0",
      idatendimento: null,
      convenio: "Teste",
    },
  },
  {
    deviceMac: "DB:31:0D:30:7B:F8",
    label: "Bracelet 2",
    patient: {
      patientId: "3691",
      patientName: "Jurandir Filadelfo Dos Santos",
      age: 40,
      email: "carlos.mozer@pcpsaude.com.br",
      leito: "LEITO 215",
      idatendimento: 5054,
      convenio: "Caixa Economica Federal",
      unidade: "Alto da Boa Vista",
      internacao: "2ª ANDAR-ABV",
      perfil: "Reabilitação",
      dataEntrada: "10/06/2026",
    },
  },
  {
    deviceMac: "F4:41:0D:30:6E:F7",
    label: "Bracelet 3",
    patient: {
      patientId: "1758",
      patientName: "Anderson Vieira Rodrigues Lino",
      age: 24,
      email: "Escala@anery.com.br",
      leito: "LEITO 101",
      idatendimento: 2715,
      convenio: "Unimed Seguros Saúde S/A",
      unidade: "Alto da Boa Vista",
      internacao: "1º ANDAR-ABV",
      perfil: "Cuidados Continuados",
      dataEntrada: "28/02/2023",
    },
  },
  {
    deviceMac: "EF:7A:0D:30:B3:FA",
    label: "Bracelet 4",
    patient: {
      patientId: "3578",
      patientName: "Luciene Marques Lizardo",
      age: 20,
      email: "Daniela.silva@anery.com.br",
      leito: "LEITO 214",
      idatendimento: 5011,
      convenio: "Cassi",
      unidade: "Alto da Boa Vista",
      internacao: "2ª ANDAR-ABV",
      perfil: "Cuidados Paliativos",
      dataEntrada: "25/05/2026",
    },
  },
];
