/** Cadastro fixo para testes com 4 pulseiras. */
export type PatientInfo = {
  patientId: string;
  patientName: string;
  age: number;
  email: string;
};

export type TestBracelet = {
  deviceMac: string;
  label: string;
  patient: PatientInfo;
};

export const TEST_BRACELETS: TestBracelet[] = [
  {
    deviceMac: "E6:64:0D:30:D3:F9",
    label: "Bracelet 1",
    patient: {
      patientId: "P001",
      patientName: "Ana Clara",
      age: 22,
      email: "Ana.trindade@anery.com.br",
    },
  },
  {
    deviceMac: "DB:31:0D:30:7B:F8",
    label: "Bracelet 2",
    patient: {
      patientId: "P002",
      patientName: "Carlos",
      age: 40,
      email: "carlos.mozer@pcpsaude.com.br",
    },
  },
  {
    deviceMac: "F4:41:0D:30:6E:F7",
    label: "Bracelet 3",
    patient: {
      patientId: "P003",
      patientName: "Bárbara Mascarenhas",
      age: 24,
      email: "Escala@anery.com.br",
    },
  },
  {
    deviceMac: "EF:7A:0D:30:B3:FA",
    label: "Bracelet 4",
    patient: {
      patientId: "P004",
      patientName: "Daniela",
      age: 20,
      email: "Daniela.silva@anery.com.br",
    },
  },
];
