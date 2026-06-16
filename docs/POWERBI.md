# Como criar um Dashboard no Power BI com a Bracelet API

Guia passo a passo para conectar o **Power BI Desktop** à API da pulseira e montar
um painel com os dados de saúde (batimentos, SpO₂, temperatura, passos, bateria etc.).

Não é preciso saber programar. Basta seguir os passos na ordem.

---

## 1. O que você vai consumir

A API expõe os pacotes BLE já decodificados em formato JSON.

| Item | Valor |
|------|-------|
| **URL base** | `https://bracelet-pn7r.onrender.com` |
| **Endpoint dos dados** | `GET /bracelets/packets` |
| **URL completa** | `https://bracelet-pn7r.onrender.com/bracelets/packets` |
| **Autenticação** | Nenhuma (endpoint público) |
| **Formato** | JSON |
| **Documentação interativa (Swagger)** | `https://bracelet-pn7r.onrender.com/docs` |

### Parâmetro opcional `limit`

Por padrão a API devolve os **50** registros mais recentes. Você pode pedir até **200**:

```
https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200
```

> ⚠️ **Atenção (plano grátis do Render):** a API "hiberna" após alguns minutos sem uso.
> A **primeira** chamada pode demorar ~30–60 segundos para acordar o servidor.
> Se der erro de tempo esgotado, tente atualizar novamente.

---

## 2. Estrutura da resposta (entenda os campos)

A resposta é um objeto com uma lista `packets`. Cada item tem campos "de topo" e um
objeto aninhado `decoded` com as métricas já interpretadas:

```jsonc
{
  "packets": [
    {
      "id": 1234,
      "deviceMac": "E6:64:0D:30:D3:F9",
      "packetType": "0x28",
      "rawHex": "28 02 4B 62 ...",
      "source": "ESP32",
      "bytes": [40, 2, 75, 98],
      "crcValid": true,
      "decodeError": null,
      "createdAt": "2026-06-05T14:30:00.000Z",
      "decoded": {
        "type": "0x28",
        "measurementMode": "heart",
        "heartRate": 75,
        "spo2": 98,
        "hrv": 60,
        "fatigue": 13,
        "systolicPressure": 120,
        "diastolicPressure": 80,
        "temperature": 36.5
      }
    }
  ]
}
```

### Campos de topo (sempre presentes)

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `id` | número | ID do registro no banco |
| `deviceMac` | texto | MAC da pulseira (`AA:BB:CC:DD:EE:FF`) |
| `packetType` | texto | Tipo do pacote em hex (`0x28`, `0x09`, `0x13`, `0x22`) |
| `source` | texto | Origem (ex.: `ESP32`) |
| `crcValid` | verdadeiro/falso | Se o pacote passou na validação CRC |
| `decodeError` | texto/nulo | Mensagem de erro se a decodificação falhou |
| `createdAt` | data/hora | Quando o pacote foi recebido (UTC) |

### Campos do objeto `decoded` — variam por `type`

O conteúdo de `decoded` depende do tipo de pacote:

**`0x28` — Saúde (health):**

| Campo | Descrição |
|-------|-----------|
| `measurementMode` | Modo da medição: `hrv`, `heart`, `oxygen`, `temperature`, `blood_pressure` |
| `heartRate` | Batimentos por minuto |
| `spo2` | Oxigenação do sangue (%) |
| `hrv` | Variabilidade da frequência cardíaca |
| `fatigue` | Índice de fadiga |
| `systolicPressure` / `diastolicPressure` | Pressão arterial (0 = não medido) |
| `temperature` | Temperatura em °C |

**`0x09` — Tempo real (realtime):**

| Campo | Descrição |
|-------|-----------|
| `steps` | Passos |
| `caloriesKcal` | Calorias (kcal) |
| `distanceKm` | Distância (km) |
| `heartRate` | Batimentos por minuto |
| `spo2` | Oxigenação (%) |
| `temperature` | Temperatura em °C |

**`0x13` — Bateria:** `battery` (% de carga)
**`0x22` — MAC:** `mac` (endereço da pulseira)

**`0x53` — Sono:**

| Campo | Descrição |
|-------|-----------|
| `date` | Data do registro (`YYYY-MM-DD`) |
| `time` | Hora do registro (`HH:MM:SS`) |
| `sleepMinutes` | Minutos dormidos (cada unidade do byte = 5 min) |
| `recordId` | ID do registro na pulseira |

**`SNAPSHOT_VITALS` — Leitura consolidada do ESP32:**

Quando a pulseira envia só o snapshot (sem pacotes `0x28` crus), cada item em `packets` terá `packetType: "SNAPSHOT_VITALS"` e `decoded` com os vitais. A API também gera automaticamente uma linha `0x53` com os dados de sono do snapshot, para o painel de sono continuar funcionando sem mudanças no Power BI.

| Campo em `decoded` | Descrição |
|--------------------|-----------|
| `heartRate`, `spo2`, `temperature` | Vitais principais |
| `hrv`, `fatigue` | HRV e fadiga (podem vir `null` se repetidos) |
| `systolicPressure`, `diastolicPressure` | Pressão arterial |
| `sleepMinutes`, `sleepDate`, `sleepTime` | Sono (também exposto como pacote `0x53`) |

> Como cada tipo tem campos diferentes, ao expandir `decoded` no Power BI algumas
> colunas ficarão vazias (`null`) nas linhas de outro tipo. Isso é normal — basta
> filtrar por `packetType` no visual.

---

## 3. Conectar o Power BI Desktop à API

### Passo 3.1 — Abrir o conector Web

1. Abra o **Power BI Desktop**.
2. Na faixa de opções, clique em **Página Inicial → Obter Dados → Web**.
3. Selecione **Básico** e cole a URL:
   ```
   https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200
   ```
4. Clique em **OK**.
5. Se aparecer a tela de autenticação, escolha **Anônimo** e clique em **Conectar**.

Vai abrir o **Editor do Power Query** com o conteúdo da API.

### Passo 3.2 — Transformar a lista em tabela

1. O Power Query mostra um registro com o campo `packets`. Clique no valor
   **List** (ou **Record**) ao lado de `packets`.
2. Clique em **Para a Tabela** (To Table) na faixa de opções. Confirme com **OK**.
3. Você terá uma coluna chamada `Column1` com vários **Record**. Clique no ícone de
   **expandir** (duas setas ⇆) no cabeçalho dessa coluna.
4. Marque os campos que quer trazer (`id`, `deviceMac`, `packetType`, `source`,
   `crcValid`, `createdAt`, `decoded`) e clique em **OK**.
   - Dica: **desmarque** "Usar o nome da coluna original como prefixo" para deixar
     os nomes limpos.

### Passo 3.3 — Expandir o objeto `decoded`

1. Localize a coluna `decoded` (cada célula é um **Record**).
2. Clique no ícone de **expandir** (⇆) no cabeçalho dela.
3. Marque as métricas que quer (`heartRate`, `spo2`, `hrv`, `temperature`,
   `steps`, `battery`, `measurementMode` etc.) e clique em **OK**.

### Passo 3.4 — Ajustar os tipos de dados

Clique no ícone de tipo (ABC/123) no cabeçalho de cada coluna e defina:

| Coluna | Tipo |
|--------|------|
| `createdAt` | **Data/Hora** |
| `heartRate`, `spo2`, `hrv`, `steps`, `battery` | **Número inteiro** |
| `temperature`, `caloriesKcal`, `distanceKm` | **Número decimal** |
| `deviceMac`, `packetType`, `source`, `measurementMode` | **Texto** |

> `createdAt` vem em UTC. Para ajustar ao fuso de Brasília, selecione a coluna e use
> **Transformar → Data/Hora → Fuso Horário → -3 horas**, ou crie uma coluna calculada.

### Passo 3.5 — Carregar

Clique em **Página Inicial → Fechar e Aplicar**. Os dados entram no modelo do Power BI.

---

## 4. Atualizar os dados automaticamente

- **Manual:** botão **Atualizar** na faixa de opções recarrega da API.
- **Automático (publicado no Power BI Service):** configure a **Atualização Agendada**.
  - Como o endpoint é anônimo, no Service defina o nível de privacidade da fonte
    como **Público** (em *Configurações do conjunto de dados → Fonte de dados*).
  - O **Gateway de dados** geralmente **não** é necessário para uma API pública na web.

---

## 5. Ideias de visuais para o dashboard

| Visual | Eixo / Campos | O que mostra |
|--------|---------------|--------------|
| **Cartão** | `heartRate` (média) | Batimento médio |
| **Cartão** | `spo2` (média) | SpO₂ médio |
| **Cartão** | `battery` (último) | Bateria atual |
| **Gráfico de linhas** | Eixo `createdAt`, valor `heartRate` | Batimentos ao longo do tempo |
| **Gráfico de linhas** | Eixo `createdAt`, valor `temperature` | Temperatura ao longo do tempo |
| **Gráfico de colunas** | Eixo `createdAt` (por dia), valor `steps` | Passos por dia |
| **Segmentação (slicer)** | `deviceMac` | Filtrar por pulseira |
| **Segmentação (slicer)** | `packetType` ou `measurementMode` | Filtrar por tipo de medição |
| **Tabela** | Todos os campos | Listagem bruta dos pacotes |

**Dica de filtro:** Para painéis de saúde, filtre `packetType = "0x28"` ou `"SNAPSHOT_VITALS"` (ou `0x09`)
para evitar linhas com métricas vazias dos pacotes de bateria/MAC. Para sono, filtre `packetType = "0x53"`.

---

## 6. Solução de problemas

| Problema | Causa provável / solução |
|----------|--------------------------|
| Demora muito / erro de tempo esgotado na 1ª chamada | Render hibernando — aguarde e tente de novo |
| Pede usuário e senha | Escolha **Anônimo** na tela de autenticação |
| Colunas `decoded` aparecem vazias | Linhas de outro `packetType` — filtre por tipo |
| Datas erradas (3h a mais) | `createdAt` está em UTC — ajuste o fuso (passo 3.4) |
| Quero ver mais histórico | Aumente o `limit` na URL (máx. **200**) |

---

## 7. Referência rápida

- **Dados:** `https://bracelet-pn7r.onrender.com/bracelets/packets?limit=200`
- **Swagger (testar no navegador):** `https://bracelet-pn7r.onrender.com/docs`
- **Método:** GET · **Auth:** nenhuma · **Formato:** JSON
