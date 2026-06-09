# Guia Power BI — Bracelet API

**Como criar um Dashboard no Power BI com a Bracelet API**

Guia passo a passo para conectar o Power BI Desktop à API da pulseira e montar um painel com os dados de saúde (batimentos, SpO₂, temperatura, pressão, bateria, status clínico etc.).

Não é preciso saber programar. Basta seguir os passos na ordem.

---

## 1. O que você vai consumir

A API expõe os pacotes BLE já decodificados em formato JSON, com **cadastro fixo de 4 pulseiras** (paciente + MAC) e medições reais vindas do banco.

| Item | Valor |
|------|-------|
| **URL base** | `https://bracelet-pn7r.onrender.com` |
| **Dashboard web (referência)** | `https://bracelet-pn7r.onrender.com/dashboard` |
| **Documentação interativa (Swagger)** | `https://bracelet-pn7r.onrender.com/docs` |
| **Autenticação** | Nenhuma (endpoints públicos de leitura) |
| **Formato** | JSON |

### Endpoints principais para o Power BI

| Uso no BI | Método | Endpoint | Filtro por pulseira |
|-----------|--------|----------|---------------------|
| Lista de pulseiras + status | GET | `/bracelets/devices` | — (retorna as 4) |
| Pacotes brutos / histórico | GET | `/bracelets/packets` | `?deviceMac=...` |
| Avaliação clínica atual | GET | `/bracelets/clinical-alerts/latest` | `?deviceMac=...` **obrigatório** |
| Histórico clínico | GET | `/bracelets/clinical-alerts` | `?deviceMac=...` opcional |
| Cadastro fixo (só referência) | GET | `/bracelets/devices/registry` | — |
| Relatório resumo (JSON) | GET | `/bracelets/reports/vitals` | `?patientName=...` |

> **Regra nova (consulta por device):** sempre que quiser dados de **uma pulseira específica**, passe o MAC em `deviceMac` (maiúsculas ou minúsculas — a API normaliza). Para visão geral das 4 pulseiras, use `/bracelets/devices` sem filtro.

### Pulseiras cadastradas

| Label | Paciente | MAC | E-mail |
|-------|----------|-----|--------|
| Bracelet 1 | Ana Clara | `E6:64:0D:30:D3:F9` | Ana.trindade@anery.com.br |
| Bracelet 2 | Carlos | `DB:31:0D:30:7B:F8` | carlos.mozer@pcpsaude.com.br |
| Bracelet 3 | Bárbara Mascarenhas | `F4:41:0D:30:6E:F7` | Escala@anery.com.br |
| Bracelet 4 | Daniela | `EF:7A:0D:30:B3:FA` | Daniela.silva@anery.com.br |

### Parâmetros úteis

| Parâmetro | Onde | Padrão | Máximo | Descrição |
|-----------|------|--------|--------|-----------|
| `limit` | `/bracelets/packets`, `/bracelets/clinical-alerts` | 50 | 200 | Quantidade de registros |
| `deviceMac` | `/bracelets/packets`, `/bracelets/clinical-alerts` | — | — | Filtra por pulseira |
| `deviceMac` | `/bracelets/clinical-alerts/latest` | — | — | **Obrigatório** |
| `patientName` | `/bracelets/reports/vitals` | — | — | Nome do paciente (ver tabela acima) |
| `windowMinutes` | `/bracelets/reports/vitals` | 60 | 1440 | Janela do histórico (minutos) |

### URLs de exemplo

```
# Todas as pulseiras (visão resumo)
https://bracelet-pn7r.onrender.com/bracelets/devices

# Pacotes do Carlos (últimos 200)
https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200&deviceMac=DB:31:0D:30:7B:F8

# Última avaliação clínica da Ana Clara
https://bracelet-pn7r.onrender.com/bracelets/clinical-alerts/latest?deviceMac=E6:64:0D:30:D3:F9

# Histórico clínico da Daniela
https://bracelet-pn7r.onrender.com/bracelets/clinical-alerts?deviceMac=EF:7A:0D:30:B3:FA&limit=100

# Relatório JSON (última hora) — Carlos
https://bracelet-pn7r.onrender.com/bracelets/reports/vitals?patientName=Carlos&windowMinutes=60
```

> ⚠️ **Atenção (plano grátis do Render):** a API "hiberna" após alguns minutos sem uso. A primeira chamada pode demorar **~30–60 segundos** para acordar o servidor. Se der erro de tempo esgotado, tente atualizar novamente.

---

## 2. Estrutura das respostas

### 2.1 GET `/bracelets/devices` — Visão das 4 pulseiras

Use este endpoint para **cartões de status**, **slicers de paciente** e **bateria/online** sem processar todos os pacotes.

```json
{
  "devices": [
    {
      "deviceMac": "DB:31:0D:30:7B:F8",
      "label": "Bracelet 2",
      "patient": {
        "patientId": "P002",
        "patientName": "Carlos",
        "age": 40,
        "email": "carlos.mozer@pcpsaude.com.br"
      },
      "online": true,
      "lastSeenAt": "2026-06-09T16:34:57.018Z",
      "battery": 87,
      "mergedHealth": {
        "type": "0x28",
        "heartRate": 87,
        "spo2": 95,
        "temperature": 36.4,
        "systolicPressure": 129,
        "diastolicPressure": 70,
        "hrv": 0,
        "fatigue": 0,
        "measurementMode": "unknown"
      }
    }
  ]
}
```

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `deviceMac` | texto | MAC da pulseira |
| `label` | texto | Nome fixo (Bracelet 1…4) |
| `patient` | objeto | Nome, idade, e-mail (cadastro fixo) |
| `online` | verdadeiro/falso | `true` se houve pacote nos últimos **15 min** |
| `lastSeenAt` | data/hora UTC | Último pacote recebido |
| `battery` | número / null | Última bateria (pacote `0x13`) |
| `mergedHealth` | objeto / null | Vitais mesclados dos últimos **5 min** |

---

### 2.2 GET `/bracelets/packets` — Histórico de pacotes

Resposta: objeto com lista `packets`. Cada item inclui **`patient`** (resolvido pelo MAC) e, quando aplicável, **`mergedHealth`** (vitais do ciclo de medição mesclados).

```json
{
  "packets": [
    {
      "id": 654,
      "deviceMac": "db:31:0d:30:7b:f8",
      "packetType": "0x28",
      "rawHex": "28 02 57 5F ...",
      "source": "ESP32",
      "bytes": [40, 2, 87, 95],
      "crcValid": true,
      "decodeError": null,
      "createdAt": "2026-06-09T16:34:57.018Z",
      "decoded": {
        "type": "0x28",
        "measurementMode": "heart",
        "heartRate": 87,
        "spo2": 95,
        "temperature": 36.4,
        "systolicPressure": 129,
        "diastolicPressure": 70
      },
      "mergedHealth": {
        "type": "0x28",
        "heartRate": 87,
        "spo2": 95,
        "temperature": 36.4,
        "systolicPressure": 129,
        "diastolicPressure": 70
      },
      "patient": {
        "deviceMac": "DB:31:0D:30:7B:F8",
        "label": "Bracelet 2",
        "patientId": "P002",
        "patientName": "Carlos",
        "age": 40,
        "email": "carlos.mozer@pcpsaude.com.br"
      }
    }
  ]
}
```

#### Campos de topo (sempre presentes)

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `id` | número | ID do registro no banco |
| `deviceMac` | texto | MAC da pulseira |
| `packetType` | texto | `0x28`, `0x09`, `0x13`, `0x22`, `0x56` |
| `source` | texto | Origem (ex.: `ESP32`) |
| `crcValid` | verdadeiro/falso | Validação CRC (`0x13` bateria costuma ser `false` — normal) |
| `decodeError` | texto / null | Erro de decodificação, se houver |
| `createdAt` | data/hora UTC | Quando o pacote foi recebido |
| `patient` | objeto / null | Paciente cadastrado para este MAC |

#### `decoded` e `mergedHealth` — qual usar no BI?

| Campo | Quando usar |
|-------|-------------|
| **`mergedHealth`** | **Preferencial** para BPM, SpO₂ e temperatura em pacotes `0x28`/`0x56` — já combina leituras parciais do mesmo ciclo |
| **`decoded`** | Valor bruto daquele pacote específico (pode ter só um sinal preenchido) |

> Para gráficos de saúde, filtre `packetType = "0x28"` e use colunas de **`mergedHealth`** (ou `decoded` se `mergedHealth` for null).

#### Campos do `decoded` / `mergedHealth` por tipo

**`0x28` — Saúde:**

| Campo | Descrição |
|-------|-----------|
| `measurementMode` | `hrv`, `heart`, `oxygen`, `temperature`, `blood_pressure` |
| `heartRate` | Batimentos por minuto |
| `spo2` | Oxigenação (%) |
| `hrv` | Variabilidade da frequência cardíaca |
| `fatigue` | Índice de fadiga |
| `systolicPressure` / `diastolicPressure` | Pressão arterial (0 = não medido) |
| `temperature` | Temperatura (°C) |

**`0x09` — Tempo real:**

| Campo | Descrição |
|-------|-----------|
| `steps` | Passos |
| `caloriesKcal` | Calorias (kcal) |
| `distanceKm` | Distância (km) |
| `heartRate`, `spo2`, `temperature` | Vitais em tempo real |

**`0x13` — Bateria:** `battery` (%)

**`0x22` — MAC:** `mac` (endereço da pulseira)

---

### 2.3 GET `/bracelets/clinical-alerts/latest` — Status clínico atual

**Obrigatório:** `?deviceMac=...`

```json
{
  "id": 42,
  "deviceMac": "DB:31:0D:30:7B:F8",
  "measuredAt": "2026-06-09T16:34:57.018Z",
  "vitals": {
    "heartRate": 87,
    "spo2": 95,
    "temperature": 36.4,
    "systolic": 129,
    "diastolic": 70,
    "hrv": 0,
    "fatigue": 0
  },
  "overallStatus": "ATTENTION",
  "severity": "MEDIUM",
  "riskScore": 3,
  "alerts": [
    {
      "type": "PA_ELEVADA",
      "severity": "MEDIUM",
      "message": "Pressão arterial elevada"
    }
  ],
  "patient": {
    "deviceMac": "DB:31:0D:30:7B:F8",
    "patientName": "Carlos",
    "label": "Bracelet 2"
  }
}
```

| Campo | Valores possíveis | Uso no BI |
|-------|-------------------|-----------|
| `overallStatus` | `STABLE`, `ATTENTION`, `ALERT`, `CRITICAL` | Cartão / cor condicional |
| `severity` | `LOW`, `MEDIUM`, `HIGH`, `CRITICAL` | Alertas |
| `riskScore` | 0–12 (NEWS2 parcial) | Indicador numérico |

---

### 2.4 GET `/bracelets/clinical-alerts` — Histórico clínico

Mesma estrutura de cada avaliação, em lista `assessments`. Filtre por `deviceMac` para uma pulseira.

---

### 2.5 GET `/bracelets/reports/vitals` — Relatório agregado (opcional)

Retorna resumo da **última hora** (padrão) com média, última leitura, min/max e histórico.

```json
{
  "report": {
    "patient": { "patientName": "Carlos", "email": "..." },
    "deviceMac": "DB:31:0D:30:7B:F8",
    "windowMinutes": 60,
    "dataPointCount": 6,
    "summary": {
      "heartRate": { "latest": 87, "avg": 79, "min": 76, "max": 87 },
      "spo2": { "latest": 95, "avg": 97, "min": 95, "max": 98 },
      "temperature": { "latest": 36.4, "avg": 36.5, "min": 36.4, "max": 36.5 }
    },
    "overallStatus": "ATTENTION",
    "history": [
      {
        "measuredAt": "2026-06-09T16:34:57.018Z",
        "heartRate": 87,
        "spo2": 95,
        "temperature": 36.4
      }
    ]
  }
}
```

> O Power BI consegue consumir `history[]` para gráficos prontos sem expandir pacotes brutos. Crie uma consulta por paciente ou use `patientName` como parâmetro.

---

## 3. Arquitetura recomendada no Power BI

Monte **3 consultas Web** (ou mais, uma por paciente) e relacione pelo `deviceMac`:

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│  Dispositivos   │     │     Pacotes      │     │  Alertas clínicos   │
│  /devices       │────▶│  /packets        │     │  /clinical-alerts   │
│  (4 linhas)     │     │  ?deviceMac=...  │     │  ?deviceMac=...     │
└─────────────────┘     └──────────────────┘     └─────────────────────┘
         │                        │                         │
         └────────────────────────┴─────────────────────────┘
                           deviceMac (chave)
```

---

## 4. Conectar o Power BI Desktop — passo a passo

### 4.1 Consulta 1 — Pulseiras (slicer / cartões)

1. **Página Inicial → Obter Dados → Web → Básico**
2. URL: `https://bracelet-pn7r.onrender.com/bracelets/devices`
3. Autenticação: **Anônimo**
4. No Power Query: expandir `devices`
5. Expandir `patient` → marcar `patientName`, `age`, `email`
6. Expandir `mergedHealth` → marcar `heartRate`, `spo2`, `temperature` (opcional)
7. Tipos: `lastSeenAt` = Data/Hora; números = Inteiro/Decimal; textos = Texto
8. Renomear consulta: `Dispositivos`

---

### 4.2 Consulta 2 — Pacotes por pulseira (histórico)

1. **Obter Dados → Web → Avançado** (recomendado para parâmetro de MAC)

**Partes da URL:**

| Parte | Valor |
|-------|-------|
| 1 | `https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200&deviceMac=` |
| 2 | *(parâmetro)* |

2. Crie um **Parâmetro** no Power Query chamado `DeviceMac` (Texto), valor inicial: `DB:31:0D:30:7B:F8`
3. Use a URL:  
   `https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200&deviceMac=` & `DeviceMac`
4. Autenticação: **Anônimo**
5. Expandir `packets` → Para Tabela → Expandir colunas: `id`, `deviceMac`, `packetType`, `source`, `crcValid`, `createdAt`, `patient`, `mergedHealth`, `decoded`
6. Expandir `patient` → `patientName`, `label`
7. **Importante:** expandir **`mergedHealth`** (não só `decoded`) → `heartRate`, `spo2`, `temperature`, `systolicPressure`, `diastolicPressure`
8. Filtrar linhas: `packetType = "0x28"` (para gráficos de saúde)
9. Coluna calculada para fuso Brasília (UTC−3):

```powerquery
DateTimeLocal = DateTimeZone.ToLocal(DateTimeZone.SwitchZone([createdAt], -3))
```

10. Renomear: `Pacotes`

**Repetir** para cada paciente (duplicar consulta e alterar `DeviceMac`) **ou** usar a função M abaixo.

---

### 4.3 Consulta 3 — Status clínico

1. URL (Avançado):  
   `https://bracelet-pn7r.onrender.com/bracelets/clinical-alerts/latest?deviceMac=` & `DeviceMac`
2. Expandir `vitals`, `alerts` conforme necessário
3. Renomear: `StatusClinico`

---

### 4.4 Função M — Uma consulta para as 4 pulseiras (avançado)

No Power Query → **Consulta em branco** → Editor Avançado:

```powerquery
let
    BaseUrl = "https://bracelet-pn7r.onrender.com",
    Macs = {
        "E6:64:0D:30:D3:F9",
        "DB:31:0D:30:7B:F8",
        "F4:41:0D:30:6E:F7",
        "EF:7A:0D:30:B3:FA"
    },
    FetchPackets = (mac as text) =>
        let
            Url = BaseUrl & "/bracelets/packets?limit=200&deviceMac=" & mac,
            Json = Json.Document(Web.Contents(Url, [Timeout=#duration(0,2,0,0)])),
            Packets = Json[packets],
            Table = Table.FromList(Packets, Splitter.SplitByNothing(), {"Record"}),
            Expanded = Table.ExpandRecordColumn(Table, "Record", {
                "id", "deviceMac", "packetType", "createdAt", "mergedHealth", "patient"
            }, {
                "id", "deviceMac", "packetType", "createdAt", "mergedHealth", "patient"
            }),
            WithMac = Table.AddColumn(Expanded, "deviceMacNorm", each Text.Upper([deviceMac]))
        in
            WithMac,
    Combined = Table.Combine(List.Transform(Macs, each FetchPackets(_))),
    ExpandHealth = Table.ExpandRecordColumn(Combined, "mergedHealth", {
        "heartRate", "spo2", "temperature", "systolicPressure", "diastolicPressure"
    }),
    ExpandPatient = Table.ExpandRecordColumn(ExpandHealth, "patient", {"patientName", "label"})
in
    ExpandPatient
```

> Aumente o `Timeout` se o Render estiver hibernando (cold start).

---

### 4.5 Relacionamentos no modelo

| Tabela 1 | Coluna | Tabela 2 | Coluna | Cardinalidade |
|----------|--------|----------|--------|---------------|
| `Dispositivos` | `deviceMac` | `Pacotes` | `deviceMac` | 1 para muitos |
| `Dispositivos` | `deviceMac` | `StatusClinico` | `deviceMac` | 1 para 1 |

Se os MACs tiverem caixa diferente (`db:31...` vs `DB:31...`), crie coluna calculada em ambas:

```powerquery
deviceMacKey = Text.Upper([deviceMac])
```

Use `deviceMacKey` no relacionamento.

---

### 4.6 Carregar

**Página Inicial → Fechar e Aplicar**

---

## 5. Medidas DAX sugeridas

```dax
BPM Último = 
CALCULATE(
    MAX(Pacotes[heartRate]),
    FILTER(Pacotes, Pacotes[packetType] = "0x28" && Pacotes[heartRate] > 0)
)

SpO2 Último = 
CALCULATE(
    MAX(Pacotes[spo2]),
    FILTER(Pacotes, Pacotes[packetType] = "0x28" && Pacotes[spo2] > 0)
)

BPM Média = 
CALCULATE(
    AVERAGE(Pacotes[heartRate]),
    Pacotes[packetType] = "0x28",
    Pacotes[heartRate] > 0
)

Bateria Atual = 
MAX(Dispositivos[battery])

Status Clínico = 
MAX(StatusClinico[overallStatus])
```

---

## 6. Ideias de visuais para o dashboard

| Visual | Campos | O que mostra |
|--------|--------|--------------|
| **Cartão** | `BPM Último` | Batimento mais recente |
| **Cartão** | `BPM Média` | Média do período carregado |
| **Cartão** | `SpO2 Último` | SpO₂ atual |
| **Cartão** | `Bateria Atual` | Bateria (de `Dispositivos`) |
| **Cartão** | `Status Clínico` | STABLE / ATTENTION / ALERT |
| **Gráfico de linhas** | Eixo `createdAt`, valor `heartRate` | BPM ao longo do tempo |
| **Gráfico de linhas** | Eixo `createdAt`, valor `temperature` | Temperatura |
| **Gráfico de linhas** | Eixo `createdAt`, valor `spo2` | SpO₂ |
| **Gráfico de colunas** | `patientName`, `heartRate` (média) | Comparar pacientes |
| **Segmentação** | `patientName` ou `deviceMac` | Filtrar por pulseira |
| **Segmentação** | `packetType` | Filtrar tipo de pacote |
| **Tabela** | Pacotes + `patientName` + vitais | Log detalhado |
| **Indicador** | `online` (Dispositivos) | Pulseira ativa (15 min) |

### Cores condicionais (status clínico)

| Status | Cor sugerida |
|--------|--------------|
| `STABLE` | Verde |
| `ATTENTION` | Laranja |
| `ALERT` | Vermelho claro |
| `CRITICAL` | Vermelho |

### Filtros importantes

- Saúde: `packetType = "0x28"`
- Use `mergedHealth.heartRate > 0` (evita zeros de pacotes parciais)
- Bateria: `packetType = "0x13"` ou use `Dispositivos[battery]`

---

## 7. Atualizar os dados automaticamente

| Modo | Como |
|------|------|
| **Manual** | Botão **Atualizar** no Power BI Desktop |
| **Agendado (Service)** | Atualização agendada no Power BI Service |
| **Privacidade da fonte** | Defina como **Público** (API anônima) |
| **Gateway** | Geralmente **não necessário** para API pública na web |

> Com 4 consultas (uma por MAC), cada atualização faz 4+ chamadas HTTP. No plano grátis do Render, a primeira atualização após hibernar pode demorar.

---

## 8. Solução de problemas

| Problema | Causa provável / solução |
|----------|--------------------------|
| Demora / tempo esgotado na 1ª chamada | Render hibernando — aguarde 30–60 s e atualize de novo |
| Pede usuário e senha | Escolha **Anônimo** na autenticação |
| Colunas de saúde vazias | Use `mergedHealth`; filtre `packetType = "0x28"` |
| MAC não bate entre tabelas | Normalize com `Text.Upper` no Power Query |
| `clinical-alerts/latest` retorna 400 | Falta `?deviceMac=...` na URL |
| `clinical-alerts/latest` retorna 404 | Pulseira ainda sem avaliação clínica no banco |
| Datas 3 h adiantadas | `createdAt` está em UTC — converta para local (UTC−3) |
| Pouco histórico | Aumente `limit` (máx. 200 por chamada) |
| CRC vermelho em `0x13` | Normal — pacote de bateria não traz CRC válido |
| Quero mais de 200 pacotes | Faça paginação manual (várias consultas por período) ou use `/reports/vitals` |

---

## 9. Referência rápida

| Recurso | URL |
|---------|-----|
| **Dispositivos** | `https://bracelet-pn7r.onrender.com/bracelets/devices` |
| **Pacotes (filtrado)** | `https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200&deviceMac=MAC` |
| **Status clínico** | `https://bracelet-pn7r.onrender.com/bracelets/clinical-alerts/latest?deviceMac=MAC` |
| **Histórico clínico** | `https://bracelet-pn7r.onrender.com/bracelets/clinical-alerts?deviceMac=MAC&limit=100` |
| **Relatório** | `https://bracelet-pn7r.onrender.com/bracelets/reports/vitals?patientName=Nome&windowMinutes=60` |
| **Swagger** | `https://bracelet-pn7r.onrender.com/docs` |
| **Dashboard web** | `https://bracelet-pn7r.onrender.com/dashboard` |

**Método:** GET · **Auth:** nenhuma · **Formato:** JSON

---

## 10. Changelog em relação ao guia anterior

| Antes | Agora |
|-------|-------|
| Só `/bracelets/packets` sem filtro | Filtro `?deviceMac=` por pulseira |
| Só `decoded` | Preferir **`mergedHealth`** para vitais completos |
| Sem cadastro de paciente na API | Campo **`patient`** em pacotes + `/devices` |
| Sem status clínico | `/clinical-alerts/latest?deviceMac=` |
| Sem visão das 4 pulseiras | `/bracelets/devices` com online/bateria |
| Sem relatório agregado | `/bracelets/reports/vitals` com média + última leitura |

---

*Documento gerado para o projeto bracelet-api — junho/2026.*
