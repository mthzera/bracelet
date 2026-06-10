#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEAddress.h>

#include "esp_sleep.h"

// ===================== CONFIG =====================

const char* WIFI_SSID = "IoTs";
const char* WIFI_PASS = "AneryIot158";

const char* API_BASE_URL = "https://bracelet-pn7r.onrender.com";
const char* API_PATH = "/bracelets/packets";

const char* DEVICE_MAC = "ef:7a:0d:30:b3:fa";

static BLEUUID SERVICE_UUID((uint16_t)0xFFF0);
static BLEUUID TX_UUID((uint16_t)0xFFF6);
static BLEUUID RX_UUID((uint16_t)0xFFF7);

const char* PACKETS_FILE = "/packets.ndjson";

// ===================== CICLO =====================

// 0 = configurar automático e dormir
// 1 = ler histórico + pressão tempo real + enviar API
RTC_DATA_ATTR int bootStep = 0;
RTC_DATA_ATTR uint32_t bootCounter = 0;

// Teste: 2 minutos.
// Produção: recomendo 300 ou mais.
const uint64_t COLLECT_SLEEP_SECONDS = 120;
const uint64_t SHORT_SLEEP_SECONDS = 5;

// Para teste, tudo em 1 minuto.
// Depois pode mudar para 5/10.
const uint16_t AUTO_BPM_INTERVAL_MIN = 1;
const uint16_t AUTO_SPO2_INTERVAL_MIN = 1;
const uint16_t AUTO_TEMP_INTERVAL_MIN = 1;
const uint16_t AUTO_HRV_INTERVAL_MIN = 1;

// Coleta curta para pegar pressão pelo 0x28.
// Pressão não tem histórico dedicado no PDF.
const unsigned long PRESSURE_SAMPLE_MS = 20000UL;
const unsigned long PRINT_028_EVERY_MS = 5000UL;
const unsigned long SAVE_028_EVERY_MS = 5000UL;

unsigned long lastPrint028 = 0;
unsigned long lastSave028 = 0;

// ===================== BLE =====================

BLEClient* client = nullptr;
BLERemoteCharacteristic* txChar = nullptr;
BLERemoteCharacteristic* rxChar = nullptr;

bool bleReady = false;
bool captureHistoryEnabled = false;
bool capturePressureEnabled = false;

// ===================== AUTO CONFIG =====================

#define AUTO_MODE_INTERVAL 0x02
#define AUTO_DAYS_ALL 0x7F

int autoMode[5] = { -1, -1, -1, -1, -1 };
int autoInterval[5] = { -1, -1, -1, -1, -1 };

// ===================== UTILS =====================

uint8_t crc16(uint8_t* p) {
  uint16_t s = 0;

  for (int i = 0; i < 15; i++) {
    s += p[i];
  }

  return s & 0xFF;
}

String toHex(uint8_t* data, size_t len) {
  String out;
  out.reserve(len * 3);

  for (size_t i = 0; i < len; i++) {
    if (data[i] < 16) out += "0";
    out += String(data[i], HEX);
    if (i < len - 1) out += " ";
  }

  out.toUpperCase();
  return out;
}

String packetTypeString(uint8_t type) {
  char buf[5];
  snprintf(buf, sizeof(buf), "0x%02X", type);
  return String(buf);
}

// Fuso da pulseira (Brasília, sem horário de verão). O RTC da pulseira guarda
// hora local; convertemos para epoch UTC somando o offset.
const long BRACELET_TZ_OFFSET_SECONDS = -3 * 3600;

uint8_t bcdToDec(uint8_t v) {
  return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
}

// Dias desde 1970-01-01 (algoritmo de Howard Hinnant), sem depender de TZ.
long long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

// Lê o timestamp BCD do registro de histórico (recordId em [1..2], data/hora
// em [3..8] = yy mo dd hh mm ss). Retorna epoch em ms, ou 0 se implausível
// (aí o chamador usa millis() e a API cai no now() do servidor).
uint64_t historyMeasuredAtMs(uint8_t* data, size_t len) {
  if (len < 9) return 0;

  int year = 2000 + bcdToDec(data[3]);
  int month = bcdToDec(data[4]);
  int day = bcdToDec(data[5]);
  int hour = bcdToDec(data[6]);
  int minute = bcdToDec(data[7]);
  int second = bcdToDec(data[8]);

  if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
    return 0;
  }

  long long days = daysFromCivil(year, (unsigned)month, (unsigned)day);
  long long localSecs = days * 86400LL + hour * 3600LL + minute * 60LL + second;
  long long utcSecs = localSecs - BRACELET_TZ_OFFSET_SECONDS;
  return (uint64_t)utcSecs * 1000ULL;
}

void printHeap(const char* label) {
  Serial.print("[MEM] ");
  Serial.print(label);
  Serial.print(" freeHeap=");
  Serial.println(ESP.getFreeHeap());
}

// ===================== SLEEP =====================

void goToDeepSleep(uint64_t seconds, int nextStep) {
  Serial.print("[SLEEP] Próximo step=");
  Serial.print(nextStep);
  Serial.print(" dormindo por ");
  Serial.print(seconds);
  Serial.println("s");

  bootStep = nextStep;

  Serial.flush();

  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ===================== SPIFFS =====================

bool initStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[FS] Falha ao iniciar SPIFFS.");
    return false;
  }

  Serial.println("[FS] SPIFFS OK.");
  return true;
}

void clearPacketsFile() {
  if (SPIFFS.exists(PACKETS_FILE)) {
    SPIFFS.remove(PACKETS_FILE);
  }

  File f = SPIFFS.open(PACKETS_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("[FS] Falha ao criar arquivo de pacotes.");
    return;
  }

  f.close();
  Serial.println("[FS] Arquivo de pacotes limpo.");
}

void appendRawPacketToFile(const char* packetType, const String& rawHex, uint64_t receivedAtMs) {
  File f = SPIFFS.open(PACKETS_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[FS] Falha ao abrir arquivo para append.");
    return;
  }

  char tsBuf[21];
  snprintf(tsBuf, sizeof(tsBuf), "%llu", (unsigned long long)receivedAtMs);

  f.print("{\"packetType\":\"");
  f.print(packetType);
  f.print("\",\"rawHex\":\"");
  f.print(rawHex);
  f.print("\",\"receivedAtMs\":");
  f.print(tsBuf);
  f.println("}");

  f.close();

  Serial.print("[FS] Salvo raw ");
  Serial.print(packetType);
  Serial.print(" rawLen=");
  Serial.println(rawHex.length());
}

void appendMetricsPacket028ToFile(
  const String& rawHex,
  uint8_t mode,
  uint8_t bpm,
  uint8_t spo2,
  uint8_t hrv,
  uint8_t fadiga,
  uint8_t pressaoAlta,
  uint8_t pressaoBaixa,
  float temp,
  uint32_t receivedAtMs) {
  File f = SPIFFS.open(PACKETS_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[FS] Falha ao abrir arquivo para append metrics.");
    return;
  }

  f.print("{\"packetType\":\"0x28\",");
  f.print("\"rawHex\":\"");
  f.print(rawHex);
  f.print("\",\"receivedAtMs\":");
  f.print(receivedAtMs);
  f.print(",\"metrics\":{");

  f.print("\"measurementMode\":");
  f.print(mode);

  f.print(",\"bpm\":");
  f.print(bpm);

  f.print(",\"spo2\":");
  f.print(spo2);

  f.print(",\"temperature\":");
  f.print(temp, 1);

  f.print(",\"hrv\":");
  f.print(hrv);

  f.print(",\"fatigue\":");
  f.print(fadiga);

  f.print(",\"bloodPressureSystolic\":");
  f.print(pressaoAlta);

  f.print(",\"bloodPressureDiastolic\":");
  f.print(pressaoBaixa);

  f.println("}}");

  f.close();

  Serial.print("[FS] Salvo 0x28 metrics BP=");
  Serial.print(pressaoAlta);
  Serial.print("/");
  Serial.print(pressaoBaixa);
  Serial.print(" fadiga=");
  Serial.print(fadiga);
  Serial.print(" hrv=");
  Serial.println(hrv);
}

int countPacketLines() {
  File f = SPIFFS.open(PACKETS_FILE, FILE_READ);
  if (!f) return 0;

  int count = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() > 0) {
      count++;
    }
  }

  f.close();
  return count;
}

// ===================== BLE SEND =====================

void sendRaw16(uint8_t* pkt) {
  if (!bleReady || !txChar) {
    Serial.println("[BLE TX] Ignorado: BLE não pronto.");
    return;
  }

  pkt[15] = crc16(pkt);

  Serial.print("[BLE TX] ");
  Serial.println(toHex(pkt, 16));

  txChar->writeValue(pkt, 16, true);
}

void sendCmd(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0) {
  uint8_t pkt[16] = {
    cmd, p1, p2, p3,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0
  };

  sendRaw16(pkt);
}

// ===================== 0x28 TEMPO REAL =====================

void startBpmPressure() {
  // 0x28 AA=0x02 BB=0x01
  // Retorna BPM, pressão, temperatura, HRV/fadiga calculados no pacote.
  sendCmd(0x28, 0x02, 0x01);
}

void stopBpmPressure() {
  sendCmd(0x28, 0x02, 0x00);
}

// ===================== HISTÓRICOS =====================

void readHistoryBpm() {
  sendCmd(0x54, 0x00, 0x00, 0x00);
}

void readHistoryHrvFatigue() {
  sendCmd(0x56, 0x00, 0x00, 0x00);
}

void readHistorySpo2Manual() {
  sendCmd(0x60, 0x00, 0x00, 0x00);
}

void readHistorySpo2Auto() {
  sendCmd(0x66, 0x00, 0x00, 0x00);
}

void readHistoryTempManual() {
  sendCmd(0x62, 0x00, 0x00, 0x00);
}

void readHistoryTempAuto() {
  sendCmd(0x65, 0x00, 0x00, 0x00);
}

// ===================== AUTO CONFIG =====================

void readAutoConfig(uint8_t type) {
  sendCmd(0x2B, type, 0x00, 0x00);
}

void setAutoConfig(uint8_t type, uint16_t intervalMin) {
  // 0x2A AA BB CC DD EE FF GG HH II
  //
  // AA = modo
  // BB = hora início
  // CC = minuto início
  // DD = hora fim
  // EE = minuto fim
  // FF = dias
  // GG HH = intervalo
  // II = tipo

  uint8_t pkt[16] = {
    0x2A,
    AUTO_MODE_INTERVAL,
    0x00,
    0x00,
    0x23,
    0x59,
    AUTO_DAYS_ALL,
    (uint8_t)((intervalMin >> 8) & 0xFF),
    (uint8_t)(intervalMin & 0xFF),
    type,
    0, 0, 0, 0, 0, 0
  };

  Serial.print("[AUTO SET] tipo=");
  Serial.print(type);
  Serial.print(" intervalo=");
  Serial.print(intervalMin);
  Serial.println("min");

  sendRaw16(pkt);
}

void clearAutoStatus() {
  for (int i = 0; i < 5; i++) {
    autoMode[i] = -1;
    autoInterval[i] = -1;
  }
}

void handleAutoConfigPacket(uint8_t* data, size_t len, String raw) {
  if (len < 10) {
    Serial.println("[AUTO CONFIG] Pacote curto.");
    return;
  }

  uint8_t mode = data[1];

  uint8_t startHour = data[2];
  uint8_t startMin = data[3];
  uint8_t endHour = data[4];
  uint8_t endMin = data[5];
  uint8_t days = data[6];

  uint16_t intervalMin = ((uint16_t)data[7] << 8) | data[8];

  uint8_t type = data[9];

  if (type <= 4) {
    autoMode[type] = mode;
    autoInterval[type] = intervalMin;
  }

  Serial.print("[AUTO CONFIG] tipo=");
  Serial.print(type);

  Serial.print(" modo=");
  Serial.print(mode);

  Serial.print(" inicio=");
  Serial.print(startHour, HEX);
  Serial.print(":");
  Serial.print(startMin, HEX);

  Serial.print(" fim=");
  Serial.print(endHour, HEX);
  Serial.print(":");
  Serial.print(endMin, HEX);

  Serial.print(" dias=0x");
  Serial.print(days, HEX);

  Serial.print(" intervalo=");
  Serial.print(intervalMin);
  Serial.println("min");

  Serial.print("[AUTO CONFIG RAW] ");
  Serial.println(raw);
}

void ensureAutoConfigForType(uint8_t type, uint16_t intervalMin) {
  if (type > 4) return;

  autoMode[type] = -1;
  autoInterval[type] = -1;

  Serial.print("[AUTO] Lendo config tipo=");
  Serial.println(type);

  readAutoConfig(type);

  unsigned long start = millis();

  while (millis() - start < 2500) {
    delay(100);

    if (autoMode[type] != -1) {
      break;
    }
  }

  if (autoMode[type] == -1) {
    Serial.print("[AUTO] Sem resposta do tipo ");
    Serial.print(type);
    Serial.println(". Ativando mesmo assim.");

    setAutoConfig(type, intervalMin);
    delay(1500);
    return;
  }

  Serial.print("[AUTO] Tipo=");
  Serial.print(type);
  Serial.print(" modo atual=");
  Serial.print(autoMode[type]);
  Serial.print(" intervalo atual=");
  Serial.println(autoInterval[type]);

  if (autoMode[type] == AUTO_MODE_INTERVAL && autoInterval[type] == intervalMin) {
    Serial.print("[AUTO] Tipo ");
    Serial.print(type);
    Serial.println(" já está correto.");
    return;
  }

  Serial.print("[AUTO] Atualizando tipo ");
  Serial.println(type);

  setAutoConfig(type, intervalMin);
  delay(1500);
}

void ensureAutoConfigs() {
  Serial.println("[AUTO] Conferindo configurações automáticas...");

  clearAutoStatus();

  // 1 = BPM automático
  ensureAutoConfigForType(0x01, AUTO_BPM_INTERVAL_MIN);
  delay(700);

  // 2 = SpO2 automático
  ensureAutoConfigForType(0x02, AUTO_SPO2_INTERVAL_MIN);
  delay(700);

  // 3 = Temperatura automática
  ensureAutoConfigForType(0x03, AUTO_TEMP_INTERVAL_MIN);
  delay(700);

  // 4 = HRV/fadiga automática
  ensureAutoConfigForType(0x04, AUTO_HRV_INTERVAL_MIN);
  delay(700);

  Serial.println("[AUTO] Configurações automáticas verificadas.");
}

// ===================== PARSE 0x28 PRESSÃO =====================

void handleActivePacket028(uint8_t* data, size_t len, String raw) {
  uint8_t mode = len > 1 ? data[1] : 0;

  uint8_t bpm = len > 2 ? data[2] : 0;
  uint8_t spo2 = len > 3 ? data[3] : 0;
  uint8_t hrv = len > 4 ? data[4] : 0;
  uint8_t fadiga = len > 5 ? data[5] : 0;
  uint8_t pressaoAlta = len > 6 ? data[6] : 0;
  uint8_t pressaoBaixa = len > 7 ? data[7] : 0;

  uint16_t tempRaw = 0;
  float temp = 0;

  if (len > 9) {
    tempRaw = ((uint16_t)data[9] << 8) | data[8];
    temp = tempRaw / 10.0;
  }

  // Para pressão, precisamos de alta/baixa válidas.
  if (capturePressureEnabled) {
    if (bpm == 0 && pressaoAlta == 0 && pressaoBaixa == 0) {
      return;
    }

    unsigned long now = millis();

    if (now - lastPrint028 >= PRINT_028_EVERY_MS) {
      lastPrint028 = now;

      Serial.print("[0x28 PRESSAO] mode=0x");
      Serial.print(mode, HEX);

      Serial.print(" bpm=");
      Serial.print(bpm);

      Serial.print(" spo2=");
      Serial.print(spo2);

      Serial.print(" temp=");
      Serial.print(temp);

      Serial.print(" hrv=");
      Serial.print(hrv);

      Serial.print(" fadiga=");
      Serial.print(fadiga);

      Serial.print(" pressao=");
      Serial.print(pressaoAlta);
      Serial.print("/");
      Serial.print(pressaoBaixa);

      Serial.print(" raw=");
      Serial.println(raw);
    }

    if (now - lastSave028 >= SAVE_028_EVERY_MS) {
      lastSave028 = now;

      appendMetricsPacket028ToFile(
        raw,
        mode,
        bpm,
        spo2,
        hrv,
        fadiga,
        pressaoAlta,
        pressaoBaixa,
        temp,
        millis());
    }
  }
}

// ===================== NOTIFY =====================

void notifyCallback(
  BLERemoteCharacteristic* c,
  uint8_t* data,
  size_t len,
  bool notify) {
  if (len == 0) return;

  uint8_t type = data[0];
  String rawStr = toHex(data, len);

  if (type == 0x28) {
    handleActivePacket028(data, len, rawStr);
    return;
  }

  if (type == 0x2B) {
    handleAutoConfigPacket(data, len, rawStr);
    return;
  }

  if (type == 0x2A) {
    Serial.print("[AUTO SET RESP] raw=");
    Serial.println(rawStr);
    return;
  }

  if (
    type == 0x54 || type == 0x56 || type == 0x60 || type == 0x62 || type == 0x65 || type == 0x66) {
    String packetType = packetTypeString(type);

    // Timestamp real da medição vem dentro do próprio registro de histórico.
    // Se não for plausível, cai em millis() (a API usa now() do servidor).
    uint64_t measuredAtMs = historyMeasuredAtMs(data, len);
    if (measuredAtMs == 0) {
      measuredAtMs = (uint64_t)millis();
    }

    Serial.print("[HIST] ");
    Serial.print(packetType);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" rawLen=");
    Serial.print(rawStr.length());
    Serial.print(" measuredAtMs=");
    {
      char tsBuf[21];
      snprintf(tsBuf, sizeof(tsBuf), "%llu", (unsigned long long)measuredAtMs);
      Serial.println(tsBuf);
    }

    if (captureHistoryEnabled) {
      appendRawPacketToFile(packetType.c_str(), rawStr, measuredAtMs);
    }

    return;
  }

  Serial.print("[BLE RX OUTRO] type=0x");
  Serial.print(type, HEX);
  Serial.print(" len=");
  Serial.print(len);
  Serial.print(" raw=");
  Serial.println(rawStr);
}

// ===================== CONNECT / DISCONNECT BLE =====================

bool connectBle() {
  Serial.println("[BLE] Iniciando...");
  printHeap("antes BLE");

  BLEDevice::init("ESP32_AUTO_HISTORY");

  client = BLEDevice::createClient();

  BLEAddress addr(DEVICE_MAC);

  Serial.println("[BLE] Conectando...");

  bool ok = client->connect(addr, BLE_ADDR_TYPE_RANDOM);

  if (!ok) {
    Serial.println("[BLE] RANDOM falhou, tentando PUBLIC...");
    ok = client->connect(addr, BLE_ADDR_TYPE_PUBLIC);
  }

  if (!ok) {
    Serial.println("[BLE] Falha ao conectar.");
    return false;
  }

  BLERemoteService* service = client->getService(SERVICE_UUID);

  if (!service) {
    Serial.println("[BLE] Serviço FFF0 não encontrado.");
    return false;
  }

  txChar = service->getCharacteristic(TX_UUID);
  rxChar = service->getCharacteristic(RX_UUID);

  if (!txChar || !rxChar) {
    Serial.println("[BLE] TX/RX não encontrados.");
    return false;
  }

  rxChar->registerForNotify(notifyCallback);

  bleReady = true;

  Serial.println("[BLE] Pronto.");
  printHeap("depois BLE");

  return true;
}

void bleOff() {
  Serial.println("[BLE] Desligando seguro...");

  captureHistoryEnabled = false;
  capturePressureEnabled = false;

  bleReady = false;
  txChar = nullptr;
  rxChar = nullptr;

  if (client && client->isConnected()) {
    Serial.println("[BLE] Disconnect client...");
    client->disconnect();
    delay(1000);
  }

  // NÃO usar:
  // delete client;
  // BLEDevice::deinit(true);
  //
  // Essas chamadas estavam causando CORRUPT HEAP.
  // O deep sleep já vai limpar o estado do BLE no próximo boot.

  Serial.println("[BLE] Desconectado. Não vou liberar heap manualmente.");
  printHeap("depois BLE disconnect");
}
// ===================== CICLO 1: CONFIGURAR AUTO =====================

void configureAutoThenSleep() {
  Serial.println("[MODE] CONFIGURAR AUTOMATICO");

  if (!connectBle()) {
    Serial.println("[MODE] Falha BLE no config. Dormindo curto e tentando de novo.");
    bleOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 0);
  }

  ensureAutoConfigs();

  Serial.println("[MODE] Automático configurado. Agora a pulseira mede sozinha.");

  // Define o próximo passo ANTES de mexer no BLE.
  // Assim, mesmo se algo reiniciar, a chance de ficar preso no step 0 diminui.
  bootStep = 1;

  bleOff();

  goToDeepSleep(COLLECT_SLEEP_SECONDS, 1);
}

// ===================== CICLO 2: LER HISTÓRICO + PRESSÃO =====================

void collectPressureRealtime() {
  Serial.println("[APP] Coletando pressão calculada via 0x28 modo 0x02...");

  capturePressureEnabled = true;
  lastPrint028 = 0;
  lastSave028 = 0;

  startBpmPressure();

  unsigned long start = millis();

  while (millis() - start < PRESSURE_SAMPLE_MS) {
    delay(500);
  }

  stopBpmPressure();

  delay(1000);

  capturePressureEnabled = false;

  Serial.println("[APP] Coleta curta de pressão finalizada.");
}

void readHistoriesToFile() {
  Serial.println("[MODE] LER HISTORICOS + PRESSAO");

  clearPacketsFile();

  if (!connectBle()) {
    Serial.println("[MODE] Falha BLE ao ler históricos.");
    bleOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }

  // Pressão não tem histórico próprio no PDF.
  // Então pegamos uma amostra calculada pelo 0x28.
  collectPressureRealtime();

  captureHistoryEnabled = true;

  Serial.println("[APP] Buscando histórico BPM 0x54...");
  readHistoryBpm();
  delay(5000);

  Serial.println("[APP] Buscando histórico HRV/fadiga 0x56...");
  readHistoryHrvFatigue();
  delay(5000);

  Serial.println("[APP] Buscando histórico SpO2 manual 0x60...");
  readHistorySpo2Manual();
  delay(5000);

  Serial.println("[APP] Buscando histórico SpO2 automático 0x66...");
  readHistorySpo2Auto();
  delay(5000);

  Serial.println("[APP] Buscando histórico temperatura manual 0x62...");
  readHistoryTempManual();
  delay(5000);

  Serial.println("[APP] Buscando histórico temperatura automática 0x65...");
  readHistoryTempAuto();
  delay(6000);

  captureHistoryEnabled = false;

  int lines = countPacketLines();

  Serial.print("[FS] Total de pacotes salvos em arquivo=");
  Serial.println(lines);

  bleOff();
  // IMPORTANTE:
  // Não liga Wi-Fi neste mesmo boot.
  // Vamos dormir e acordar em um boot limpo só para HTTP.
  goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
}

// ===================== WIFI / HTTP =====================

bool connectWifi() {
  Serial.println("[WIFI] Ligando...");
  printHeap("antes WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();

  while (millis() - start < 20000) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WIFI] OK IP=");
      Serial.println(WiFi.localIP());
      printHeap("depois WiFi");
      return true;
    }

    delay(500);
  }

  Serial.println("[WIFI] Falha.");
  return false;
}

void wifiOff() {
  Serial.println("[WIFI] Desligando...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  delay(1000);

  printHeap("depois WiFi off");
}

bool pingHealth() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Sem WiFi.");
    return false;
  }

  WiFiClientSecure secure;
  secure.setInsecure();

  HTTPClient http;
  http.setTimeout(45000);
  http.setReuse(false);

  String url = String(API_BASE_URL) + "/health";

  Serial.print("[HTTP] GET ");
  Serial.println(url);

  if (!http.begin(secure, url)) {
    Serial.println("[HTTP] begin /health falhou.");
    return false;
  }

  http.addHeader("Connection", "close");

  int code = http.GET();

  Serial.print("[HTTP] /health status=");
  Serial.println(code);

  if (code > 0) {
    String res = http.getString();
    Serial.print("[HTTP] /health body=");
    Serial.println(res);
  } else {
    Serial.print("[HTTP] /health erro=");
    Serial.println(http.errorToString(code));
  }

  http.end();

  return code >= 200 && code < 300;
}

bool waitApiAwake() {
  Serial.println("[HTTP] Acordando API do Render...");

  for (int attempt = 1; attempt <= 6; attempt++) {
    Serial.print("[HTTP] Wake attempt ");
    Serial.print(attempt);
    Serial.println("/6");

    if (pingHealth()) {
      Serial.println("[HTTP] API OK.");
      return true;
    }

    delay(7000);
  }

  Serial.println("[HTTP] API não respondeu.");
  return false;
}

bool postJsonBody(const String& body) {
  WiFiClientSecure secure;
  secure.setInsecure();

  HTTPClient http;
  http.setTimeout(45000);
  http.setReuse(false);

  String url = String(API_BASE_URL) + API_PATH;

  Serial.print("[HTTP] POST ");
  Serial.println(url);
  Serial.print("[HTTP] Body size=");
  Serial.println(body.length());

  if (!http.begin(secure, url)) {
    Serial.println("[HTTP] begin POST falhou.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int code = http.POST(body);

  Serial.print("[HTTP] Status=");
  Serial.println(code);

  if (code > 0) {
    String res = http.getString();
    Serial.print("[HTTP] Resposta=");
    Serial.println(res);
  } else {
    Serial.print("[HTTP] Erro POST=");
    Serial.println(http.errorToString(code));
  }

  http.end();

  return code == 200 || code == 201 || code == 202;
}

bool sendPacketsFileToApi() {
  int totalLines = countPacketLines();

  Serial.print("[HTTP] Pacotes no arquivo=");
  Serial.println(totalLines);

  if (totalLines == 0) {
    Serial.println("[HTTP] Nada para enviar.");
    return true;
  }

  File f = SPIFFS.open(PACKETS_FILE, FILE_READ);
  if (!f) {
    Serial.println("[FS] Falha ao abrir arquivo para envio.");
    return false;
  }

  const int LOCAL_BATCH_SIZE = 3;

  int sent = 0;
  int batchCount = 0;
  bool firstPacket = true;

  String body;
  body.reserve(4000);

  body = "{";
  body += "\"deviceMac\":\"";
  body += DEVICE_MAC;
  body += "\",";
  body += "\"source\":\"ESP32_HISTORY_SPIFFS\",";
  body += "\"packets\":[";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      continue;
    }

    if (!firstPacket) {
      body += ",";
    }

    body += line;

    firstPacket = false;
    batchCount++;

    if (batchCount >= LOCAL_BATCH_SIZE) {
      body += "]}";

      bool ok = postJsonBody(body);

      if (!ok) {
        Serial.println("[HTTP] Falha ao enviar batch.");
        f.close();
        return false;
      }

      sent += batchCount;

      Serial.print("[HTTP] Enviados até agora=");
      Serial.println(sent);

      delay(1500);

      batchCount = 0;
      firstPacket = true;

      body = "{";
      body += "\"deviceMac\":\"";
      body += DEVICE_MAC;
      body += "\",";
      body += "\"source\":\"ESP32_HISTORY_SPIFFS\",";
      body += "\"packets\":[";
    }
  }

  f.close();

  if (batchCount > 0) {
    body += "]}";

    bool ok = postJsonBody(body);

    if (!ok) {
      Serial.println("[HTTP] Falha ao enviar último batch.");
      return false;
    }

    sent += batchCount;
  }

  Serial.print("[HTTP] Envio finalizado. Total enviado=");
  Serial.println(sent);

  return true;
}

void sendFileThenSleep() {
  Serial.println("[MODE] ENVIAR API");

  if (!connectWifi()) {
    wifiOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }

  bool apiOk = waitApiAwake();

  if (!apiOk) {
    Serial.println("[HTTP] API indisponível. Mantendo arquivo e tentando no próximo ciclo.");
    wifiOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }

  bool sentOk = sendPacketsFileToApi();

  if (sentOk) {
    Serial.println("[HTTP] Enviado com sucesso. Limpando arquivo.");
    clearPacketsFile();
  } else {
    Serial.println("[HTTP] Envio falhou. Arquivo será mantido.");
  }

  wifiOff();

  // Depois de enviar, volta para configurar automático e repetir ciclo.
  goToDeepSleep(SHORT_SLEEP_SECONDS, 0);
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);
  delay(1000);

  bootCounter++;

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32 AUTO HISTORY - BLE/WIFI ISOLADOS");
  Serial.println("BOOT 0=AUTO | BOOT 1=HISTORICO | BOOT 2=API");
  Serial.print("[BOOT] bootCounter=");
  Serial.println(bootCounter);
  Serial.print("[BOOT] bootStep=");
  Serial.println(bootStep);
  Serial.println("========================================");

  printHeap("inicio");

  WiFi.mode(WIFI_OFF);

  if (!initStorage()) {
    Serial.println("[FATAL] SPIFFS falhou. Dormindo curto.");
    goToDeepSleep(SHORT_SLEEP_SECONDS, bootStep);
  }

  if (bootStep == 0) {
    configureAutoThenSleep();
  } else if (bootStep == 1) {
    readHistoriesToFile();
  } else if (bootStep == 2) {
    sendFileThenSleep();
  } else {
    Serial.println("[BOOT] bootStep inválido. Voltando para 0.");
    bootStep = 0;
    goToDeepSleep(SHORT_SLEEP_SECONDS, 0);
  }
}

void loop() {
  // Não usa loop.
  // Tudo roda no setup e depois entra em deep sleep.
}