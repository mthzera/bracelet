/** Cadastro fixo para testes com 4 pulseiras (substitua os MACs pelos reais). */
export type PatientInfo = {
  patientId: string;
  patientName: string;
  room: string;
  age: number;
};

export type TestBracelet = {
  deviceMac: string;
  label: string;
  patient: PatientInfo;
};

export const TEST_BRACELETS: TestBracelet[] = [
  {
    deviceMac: "E6:64:0D:30:D3:F9",
    label: "Pulseira 1",
    patient: {
      patientId: "P001",
      patientName: "Maria Silva",
      room: "101",
      age: 68,
    },
  },
  {
    deviceMac: "E6:64:0D:30:D3:FA",
    label: "Pulseira 2",
    patient: {
      patientId: "P002",
      patientName: "João Santos",
      room: "102",
      age: 54,
    },
  },
  {
    deviceMac: "E6:64:0D:30:D3:FB",
    label: "Pulseira 3",
    patient: {
      patientId: "P003",
      patientName: "Ana Costa",
      room: "103",
      age: 72,
    },
  },
  {
    deviceMac: "E6:64:0D:30:D3:FC",
    label: "Pulseira 4",
    patient: {
      patientId: "P004",
      patientName: "Pedro Oliveira",
      room: "104",
      age: 61,
    },
  },
];
