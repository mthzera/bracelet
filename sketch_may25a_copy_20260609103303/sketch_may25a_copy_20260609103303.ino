/**
 * 2208A + ESP32 — protocolo PDF "2208A API V1"
 * v4 — arquitetura não-bloqueante com máquina de estados
 *
 * Fases de medição:
 *   Fase 0 → 0x03 SpO2  (captura BPM + SpO2)
 *   Fase 1 → 0x04 Temp  (captura temperatura)
 *   Fase 2 → 0x02 BP    (captura pressão — opcional, não bloqueia envio)
 *
 * Melhorias v4:
 *  - State machine não-bloqueante (sem delay longo no loop principal)
 *  - Fila HTTP para pacotes pendentes (WiFi e BLE independentes)
 *  - Sem ESP.restart() no fluxo normal
 *  - Sem bleStop() após cada ciclo de medição
 *  - BLE reconecta sem derrubar WiFi; WiFi reconecta sem derrubar BLE
 *  - notifyCallback virou roteador por data[0]
 *  - Suporte: saúde, atividade, sono, históricos, exercícios, dispositivo,
 *             configurações automáticas, notificações, alarmes, vibração
 *  - WRITE_PERI_REG: brownout detector desabilitado
 */

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <time.h>

// ── Configurações ─────────────────────────────────────────────────────────────
const char* WIFI_SSID  = "IoTs";
const char* WIFI_PASS  = "AneryIot158";
const char* API_URL    = "https://bracelet-pn7r.onrender.com/bracelets/packets";
const char* API_BASE   = "https://bracelet-pn7r.onrender.com";
const char* DEVICE_MAC = "e6:64:0d:30:d3:f9";

// ── Intervalos ────────────────────────────────────────────────────────────────
const unsigned long MEASURE_INTERVAL_MS    = 1UL * 60UL * 1000UL;
const unsigned long PING_INTERVAL_MS       = 30UL * 1000UL;
const unsigned long WIFI_RETRY_MS          = 10000UL;
const unsigned long BLE_RETRY_DELAY_MS     = 10000UL;
const unsigned long HTTP_RETRY_INTERVAL_MS = 5000UL;

// ── Timeouts BLE ──────────────────────────────────────────────────────────────
const unsigned long SPO2_WARMUP_MS         = 15000;
const unsigned long TEMP_WARMUP_MS         = 5000;
const unsigned long BP_WARMUP_MS           = 15000;
const unsigned long SPO2_TIMEOUT_MS        = 50000;
const unsigned long TEMP_TIMEOUT_MS        = 30000;
const unsigned long BP_TIMEOUT_MS          = 90000;
const unsigned long BATTERY_TIMEOUT_MS     = 6000;
const int           MAX_PHASE_RETRIES      = 3;
const int           BLE_CONNECT_MAX_ATTEMPTS = 6;
const uint32_t      BLE_CONNECT_TIMEOUT_MS = 15000;

// ── Fases de medição ──────────────────────────────────────────────────────────
const uint8_t PHASE_CMD[]   = { 0x03, 0x04, 0x02 };
const char* PHASE_LABELS[]  = { "SpO2+BPM", "Temperatura", "Pressao" };
const int NUM_PHASES = 3;

const int SLOT_SPO2 = 0;
const int SLOT_TEMP = 1;
const int SLOT_BP   = 2;
const int NUM_SLOTS = 3;

// ── Tipos de esporte ──────────────────────────────────────────────────────────
const uint8_t SPORT_RUNNING   = 0x00;
const uint8_t SPORT_CYCLING   = 0x01;
const uint8_t SPORT_YOGA      = 0x06;
const uint8_t SPORT_BREATHING = 0x07;

// ── Fila HTTP ─────────────────────────────────────────────────────────────────
const int HTTP_QUEUE_SIZE    = 20;
const int HTTP_PAYLOAD_MAX   = 512;

struct HttpQueueEntry {
  char payload[HTTP_PAYLOAD_MAX];
  bool used;
};

HttpQueueEntry    httpQueue[HTTP_QUEUE_SIZE];
int               httpQueueHead  = 0;
int               httpQueueTail  = 0;
int               httpQueueCount = 0;
unsigned long     lastHttpRetry  = 0;

// ── Enums de estado ───────────────────────────────────────────────────────────
enum class BleState     { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING };
enum class MeasureState { IDLE, WAIT_BATTERY, PHASE_START, PHASE_WAIT, COMPLETE, POST_WAIT };

BleState     bleState     = BleState::DISCONNECTED;
MeasureState measureState = MeasureState::IDLE;

// ── Estado de medição ─────────────────────────────────────────────────────────
char rawHex[NUM_SLOTS][50] = { { 0 } };
bool captured[NUM_SLOTS]   = { false };
char rawHex13[24]           = "";
bool captured13             = false;
bool capturedBP             = false;

int           readPhase         = -1;
int           phaseRetries      = 0;
unsigned long lastScanCmd       = 0;
unsigned long deviceInfoStart   = 0;
unsigned long postWaitStart     = 0;

// ── Informações do dispositivo ────────────────────────────────────────────────
char    deviceFirmware[32] = "";
char    deviceMacResp[20]  = "";
uint8_t batteryLevel       = 0;
bool    gotFirmware        = false;
bool    gotMacResp         = false;

// ── BLE globais ───────────────────────────────────────────────────────────────
BLEClient*               bleClient             = nullptr;
BLERemoteCharacteristic* txChar                = nullptr;
BLERemoteCharacteristic* rxChar                = nullptr;
volatile bool            bleLinkUp             = false;
bool                     bleStackInitialized   = false;
unsigned long            lastBleReconnectAttempt = 0;

// ── WiFi / ping ───────────────────────────────────────────────────────────────
unsigned long lastWifiRetry = 0;
unsigned long lastPingTime  = 0;

// ════════════════════════════════════════════════════════════════════════════
//  BLE CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

class BraceletClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    bleLinkUp = true;
    bleState  = BleState::CONNECTED;
    Serial.println("[BLE] Callback: conectado.");
  }
  void onDisconnect(BLEClient*) override {
    bleLinkUp = false;
    bleState  = BleState::DISCONNECTED;
    Serial.println("[BLE] Callback: desconectado.");
  }
};

BraceletClientCallbacks braceletCallbacks;

// ════════════════════════════════════════════════════════════════════════════
//  UTILITÁRIOS
// ════════════════════════════════════════════════════════════════════════════

uint8_t calcCrc(uint8_t* p, uint8_t len = 16) {
  uint16_t s = 0;
  for (int i = 0; i < len - 1; i++) s += p[i];
  return s & 0xFF;
}

uint8_t toBcd(int v) {
  return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void storeHex(char* dest, size_t destSize, uint8_t* data, size_t len) {
  char tmp[50] = { 0 };
  size_t maxBytes = len < 16 ? len : 16;
  for (size_t i = 0; i < maxBytes; i++) {
    sprintf(tmp + i * 3, "%02X ", data[i]);
  }
  size_t tl = strlen(tmp);
  if (tl > 0 && tmp[tl - 1] == ' ') tmp[tl - 1] = '\0';
  strncpy(dest, tmp, destSize - 1);
  dest[destSize - 1] = '\0';
}

bool parseHex16(const char* hex, uint8_t* out) {
  if (!hex || !out) return false;
  for (int i = 0; i < 16; i++) {
    unsigned int v = 0;
    if (sscanf(hex + i * 3, "%2X", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

uint32_t readU32LE(uint8_t* data, int offset) {
  return (uint32_t)data[offset]
       | ((uint32_t)data[offset + 1] << 8)
       | ((uint32_t)data[offset + 2] << 16)
       | ((uint32_t)data[offset + 3] << 24);
}

uint16_t readU16LE(uint8_t* data, int offset) {
  return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

// ════════════════════════════════════════════════════════════════════════════
//  FILA HTTP
// ════════════════════════════════════════════════════════════════════════════

bool enqueueHttp(const char* payload) {
  if (httpQueueCount >= HTTP_QUEUE_SIZE) {
    // Descarta o pacote mais antigo para abrir espaço
    httpQueue[httpQueueHead].used = false;
    httpQueueHead = (httpQueueHead + 1) % HTTP_QUEUE_SIZE;
    httpQueueCount--;
    Serial.println("[HTTP] Fila cheia — descartando pacote mais antigo.");
  }
  strncpy(httpQueue[httpQueueTail].payload, payload, HTTP_PAYLOAD_MAX - 1);
  httpQueue[httpQueueTail].payload[HTTP_PAYLOAD_MAX - 1] = '\0';
  httpQueue[httpQueueTail].used = true;
  httpQueueTail = (httpQueueTail + 1) % HTTP_QUEUE_SIZE;
  httpQueueCount++;
  return true;
}

void dequeueHttp() {
  if (httpQueueCount <= 0) return;
  httpQueue[httpQueueHead].used = false;
  httpQueueHead = (httpQueueHead + 1) % HTTP_QUEUE_SIZE;
  httpQueueCount--;
}

void enqueuePacket(const char* packetType, const char* hex,
                   const char* metricsJson = nullptr) {
  char payload[HTTP_PAYLOAD_MAX];
  if (metricsJson && strlen(metricsJson) > 0) {
    snprintf(payload, sizeof(payload),
             "{\"deviceMac\":\"%s\",\"packetType\":\"%s\",\"rawHex\":\"%s\","
             "\"source\":\"ESP32\",\"metrics\":%s}",
             DEVICE_MAC, packetType, hex, metricsJson);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"deviceMac\":\"%s\",\"packetType\":\"%s\",\"rawHex\":\"%s\","
             "\"source\":\"ESP32\"}",
             DEVICE_MAC, packetType, hex);
  }
  enqueueHttp(payload);
}

// ════════════════════════════════════════════════════════════════════════════
//  WIFI
// ════════════════════════════════════════════════════════════════════════════

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  unsigned long now = millis();
  if (now - lastWifiRetry < WIFI_RETRY_MS) return false;
  lastWifiRetry = now;

  Serial.printf("[WIFI] Reconectando (status=%d)...\n", WiFi.status());
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t = millis();
  while (millis() - t < 3000) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Reconectado. IP=%s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    delay(100);
  }
  Serial.println("[WIFI] Reconexão parcial — tentará novamente.");
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
//  BLE — ENVIO DE PACOTES
// ════════════════════════════════════════════════════════════════════════════

void sendPacket16(uint8_t* pkt) {
  if (!txChar) return;
  pkt[15] = calcCrc(pkt);
  txChar->writeValue(pkt, 16, true);
}

void sendCommand(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0) {
  uint8_t pkt[16] = { cmd, p1, p2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  sendPacket16(pkt);
}

void sendVariablePacket(uint8_t* data, size_t len) {
  if (!txChar || len == 0) return;
  txChar->writeValue(data, len, true);
}

void sendSetTime(uint8_t yy, uint8_t mo, uint8_t dd,
                 uint8_t hh, uint8_t mm, uint8_t ss) {
  uint8_t pkt[16] = { 0x01, yy, mo, dd, hh, mm, ss,
                      0, 0, 0, 0, 0, 0, 0, 0, 0 };
  sendPacket16(pkt);
}

void stopAllHealthModes() {
  sendCommand(0x28, 0x01, 0x00); delay(50);
  sendCommand(0x28, 0x02, 0x00); delay(50);
  sendCommand(0x28, 0x03, 0x00); delay(50);
  sendCommand(0x28, 0x04, 0x00); delay(50);
}

// ── Saúde ativa ───────────────────────────────────────────────────────────────
void startHRV()         { sendCommand(0x28, 0x01, 0x01); }
void stopHRV()          { sendCommand(0x28, 0x01, 0x00); }
void startHeartRate()   { sendCommand(0x28, 0x02, 0x01); }
void stopHeartRate()    { sendCommand(0x28, 0x02, 0x00); }
void startSpO2()        { sendCommand(0x28, 0x03, 0x01); }
void stopSpO2()         { sendCommand(0x28, 0x03, 0x00); }
void startTemperature() { sendCommand(0x28, 0x04, 0x01); }
void stopTemperature()  { sendCommand(0x28, 0x04, 0x00); }

// ── Atividade em tempo real ───────────────────────────────────────────────────
void startRealtimeActivity() { sendCommand(0x09, 0x01, 0x01); }
void stopRealtimeActivity()  { sendCommand(0x09, 0x00, 0x00); }

// ── Meta de passos ────────────────────────────────────────────────────────────
void setStepGoal(uint32_t goal) {
  uint8_t pkt[16] = {
    0x0B,
    (uint8_t)(goal & 0xFF),       (uint8_t)((goal >> 8) & 0xFF),
    (uint8_t)((goal >> 16) & 0xFF),(uint8_t)((goal >> 24) & 0xFF),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };
  sendPacket16(pkt);
}
void readStepGoal() { sendCommand(0x4B); }

// ── Sono ──────────────────────────────────────────────────────────────────────
void readLastSleep()            { sendCommand(0x53, 0x00, 0x00); }
void readSleepAt(uint16_t pos)  {
  sendCommand(0x53, 0x01, (uint8_t)(pos & 0xFF));
}
void continueSleep()            { sendCommand(0x53, 0x02, 0x00); }

// ── Históricos ────────────────────────────────────────────────────────────────
void readStepTotalHistory()  { sendCommand(0x51, 0x00, 0x00); }
void readStepDetailHistory() { sendCommand(0x52, 0x00, 0x00); }
void readHRHistory()         { sendCommand(0x54, 0x00, 0x00); }
void readHRSingleHistory()   { sendCommand(0x55, 0x00, 0x00); }
void readHRVHistory()        { sendCommand(0x56, 0x00, 0x00); }
void readSpO2ManualHistory() { sendCommand(0x60, 0x00, 0x00); }
void readSpO2AutoHistory()   { sendCommand(0x66, 0x00, 0x00); }
void readTempManualHistory() { sendCommand(0x62, 0x01, 0x00); }
void readTempAutoHistory()   { sendCommand(0x65, 0x01, 0x00); }
void readExerciseHistory()   { sendCommand(0x5C, 0x01, 0x00); }

// ── Exercícios ────────────────────────────────────────────────────────────────
void startSport(uint8_t sportType) { sendCommand(0x19, 0x01, sportType); }
void stopSport(uint8_t sportType)  { sendCommand(0x19, 0x04, sportType); }

// ── Dispositivo ───────────────────────────────────────────────────────────────
void readBattery()      { sendCommand(0x13, 0x99); }
void readMAC()          { sendCommand(0x22); }
void readFirmware()     { sendCommand(0x27); }
void readDeviceTime()   { sendCommand(0x41); }
void readPersonalInfo() { sendCommand(0x42); }

// ── Informações pessoais ──────────────────────────────────────────────────────
void setPersonalInfo(uint8_t gender, uint8_t age, uint8_t height,
                     uint8_t weight, uint8_t stepLen) {
  uint8_t pkt[16] = { 0x02, gender, age, height, weight, stepLen,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  sendPacket16(pkt);
}

// ── Configurações automáticas ─────────────────────────────────────────────────
void setAutoMeasure(uint8_t type, uint8_t mode,
                    uint8_t startH, uint8_t startM,
                    uint8_t endH,   uint8_t endM,
                    uint8_t weekMask, uint16_t intervalMin) {
  uint8_t pkt[16] = {
    0x2A, mode, toBcd(startH), toBcd(startM), toBcd(endH), toBcd(endM),
    weekMask, (uint8_t)(intervalMin & 0xFF), (uint8_t)(intervalMin >> 8),
    type, 0, 0, 0, 0, 0, 0
  };
  sendPacket16(pkt);
}
void readAutoConfig() { sendCommand(0x2B); }

// ── Notificações ──────────────────────────────────────────────────────────────
void sendNotification(uint8_t type, const char* message, const char* contact) {
  uint8_t buf[80] = { 0 };
  buf[0] = 0x4D;
  buf[1] = type;
  size_t msgLen = strlen(message);
  if (msgLen > 60) msgLen = 60;
  buf[2] = (uint8_t)msgLen;
  memcpy(buf + 3, message, msgLen);
  size_t contactLen = strlen(contact);
  if (contactLen > 15) contactLen = 15;
  buf[3 + msgLen] = (uint8_t)contactLen;
  memcpy(buf + 4 + msgLen, contact, contactLen);
  sendVariablePacket(buf, 4 + msgLen + contactLen);
}
void stopCallNotification() { sendCommand(0x4D, 0xFF, 0x00); }

// ── Vibração ──────────────────────────────────────────────────────────────────
void vibrate(uint8_t times) {
  if (times < 1) times = 1;
  if (times > 5) times = 5;
  sendCommand(0x36, times);
}

// ── Alarmes ───────────────────────────────────────────────────────────────────
void readAlarms() { sendCommand(0x57, 0x00, 0x00); }

// ── Lembrete sedentário ───────────────────────────────────────────────────────
void setSedentaryReminder(uint8_t startH, uint8_t startM,
                          uint8_t endH,   uint8_t endM,
                          uint8_t weekMask, uint8_t intervalMin,
                          uint8_t minSteps, bool enabled) {
  uint8_t pkt[16] = {
    0x25, toBcd(startH), toBcd(startM), toBcd(endH), toBcd(endM),
    weekMask, intervalMin, minSteps, (uint8_t)(enabled ? 1 : 0),
    0, 0, 0, 0, 0, 0, 0
  };
  sendPacket16(pkt);
}
void readSedentaryReminder() { sendCommand(0x26); }

// ════════════════════════════════════════════════════════════════════════════
//  BLE — HANDLERS DE PACOTE
// ════════════════════════════════════════════════════════════════════════════

void handleBatteryPacket(uint8_t* data, size_t len) {
  batteryLevel = data[1];
  captured13   = true;
  storeHex(rawHex13, sizeof(rawHex13), data, len < 8 ? len : 8);
  Serial.printf("[BLE] Bateria: %u%%\n", batteryLevel);

  char metricsJson[48];
  snprintf(metricsJson, sizeof(metricsJson), "{\"battery\":%u}", batteryLevel);
  enqueuePacket("0x13", rawHex13, metricsJson);
}

void handleMacPacket(uint8_t* data, size_t len) {
  if (len >= 7) {
    snprintf(deviceMacResp, sizeof(deviceMacResp),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             data[1], data[2], data[3], data[4], data[5], data[6]);
    gotMacResp = true;
    Serial.printf("[BLE] MAC: %s\n", deviceMacResp);
  }
  char hex[50];
  storeHex(hex, sizeof(hex), data, len);
  enqueuePacket("0x22", hex);
}

void handleFirmwarePacket(uint8_t* data, size_t len) {
  if (len >= 4) {
    snprintf(deviceFirmware, sizeof(deviceFirmware),
             "V%u.%u.%u", data[1], data[2], data[3]);
    gotFirmware = true;
    Serial.printf("[BLE] Firmware: %s\n", deviceFirmware);
  }
  char hex[50];
  storeHex(hex, sizeof(hex), data, len);
  char metricsJson[64];
  snprintf(metricsJson, sizeof(metricsJson),
           "{\"firmware\":\"%s\"}", deviceFirmware);
  enqueuePacket("0x27", hex, metricsJson);
}

void handleHealthPacket(uint8_t* data, size_t len) {
  if (len < 10) return;
  uint8_t  measureType = data[1];
  uint8_t  hr          = data[2];
  uint8_t  spo2        = data[3];
  uint8_t  hrv         = data[4];
  uint8_t  fat         = data[5];
  uint8_t  sys         = data[6];
  uint8_t  dia         = data[7];
  uint16_t tempRaw     = readU16LE(data, 8);
  float    temp        = tempRaw / 10.0f;

  // Roteamento para slots de medição ativos
  if (readPhase == 0 && measureType == 0x03) {
    if (!captured[SLOT_SPO2] && hr > 0 && spo2 > 0) {
      storeHex(rawHex[SLOT_SPO2], sizeof(rawHex[SLOT_SPO2]), data, len);
      captured[SLOT_SPO2] = true;
      Serial.printf("[BLE] SpO2 OK — BPM=%u SpO2=%u\n", hr, spo2);
    }
    return;
  }
  if (readPhase == 1 && measureType == 0x04) {
    if (!captured[SLOT_TEMP] && tempRaw > 0) {
      storeHex(rawHex[SLOT_TEMP], sizeof(rawHex[SLOT_TEMP]), data, len);
      captured[SLOT_TEMP] = true;
      Serial.printf("[BLE] Temp OK — %.1f°C\n", temp);
    }
    return;
  }
  if (readPhase == 2 && measureType == 0x02) {
    bool ready = sys > 0 && dia > 0
              && !(sys == 0x7B && dia == 0x49)
              && !(sys == 0x75 && dia == 0x44)
              && sys < 250 && dia < 200 && sys > dia;
    if (!captured[SLOT_BP] && ready) {
      storeHex(rawHex[SLOT_BP], sizeof(rawHex[SLOT_BP]), data, len);
      captured[SLOT_BP] = true;
      capturedBP        = true;
      Serial.printf("[BLE] Pressão OK — %u/%u mmHg\n", sys, dia);
    }
    return;
  }

  // Pacote avulso (fora de ciclo de medição)
  char hex[50];
  storeHex(hex, sizeof(hex), data, len);
  char metricsJson[256];
  snprintf(metricsJson, sizeof(metricsJson),
           "{\"type\":\"0x%02X\",\"heartRate\":%u,\"spO2\":%u,\"hrv\":%u,"
           "\"fatigue\":%u,\"systolic\":%u,\"diastolic\":%u,\"temperature\":%.1f}",
           measureType, hr, spo2, hrv, fat, sys, dia, temp);
  enqueuePacket("0x28", hex, metricsJson);
}

void handleRealtimeActivityPacket(uint8_t* data, size_t len) {
  if (len < 22) return;
  uint32_t steps     = readU32LE(data, 1);
  uint32_t calRaw    = readU32LE(data, 5);
  uint32_t distRaw   = readU32LE(data, 9);
  uint32_t activeMs  = readU32LE(data, 13);
  uint32_t intenseMs = readU32LE(data, 17);
  uint8_t  hr        = data[21];
  float    cal       = calRaw  / 100.0f;
  float    dist      = distRaw / 100.0f;
  uint16_t tempRaw   = (len >= 24) ? readU16LE(data, 22) : 0;
  uint8_t  spo2val   = (len >= 25) ? data[24] : 0;

  Serial.printf("[BLE] Atividade — Passos=%u Cal=%.1f Dist=%.1fm HR=%u\n",
                steps, cal, dist, hr);

  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char metricsJson[256];
  snprintf(metricsJson, sizeof(metricsJson),
           "{\"steps\":%u,\"calories\":%.1f,\"distance\":%.1f,"
           "\"activeTime\":%u,\"intenseTime\":%u,"
           "\"heartRate\":%u,\"temperature\":%.1f,\"spO2\":%u}",
           steps, cal, dist, activeMs, intenseMs, hr, tempRaw / 10.0f, spo2val);
  enqueuePacket("0x09", hex, metricsJson);
}

void handleSportRealtimePacket(uint8_t* data, size_t len) {
  if (len < 14) return;
  uint8_t  hr     = data[1];
  uint32_t steps  = readU32LE(data, 2);
  uint32_t calRaw = readU32LE(data, 6);
  uint32_t exTime = readU32LE(data, 10);

  Serial.printf("[BLE] Esporte — HR=%u Passos=%u Tempo=%us\n", hr, steps, exTime);

  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char metricsJson[128];
  snprintf(metricsJson, sizeof(metricsJson),
           "{\"heartRate\":%u,\"steps\":%u,\"calories\":%u,\"exerciseTime\":%u}",
           hr, steps, calRaw, exTime);
  enqueuePacket("0x18", hex, metricsJson);
}

void handleSleepPacket(uint8_t* data, size_t len) {
  if (len < 10) return;
  uint16_t recordId = readU16LE(data, 1);
  uint8_t  yy = data[3], mo = data[4], dd = data[5];
  uint8_t  hh = data[6], mm = data[7], ss = data[8];
  uint8_t  segLen   = data[9];
  int      sleepMin = segLen * 5;

  Serial.printf("[BLE] Sono — Record=%u Data=20%02u/%02u/%02u %02u:%02u SleepMin=%d\n",
                recordId, yy, mo, dd, hh, mm, sleepMin);

  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char metricsJson[128];
  snprintf(metricsJson, sizeof(metricsJson),
           "{\"recordId\":%u,\"date\":\"20%02u-%02u-%02u\","
           "\"time\":\"%02u:%02u:%02u\",\"sleepMinutes\":%d}",
           recordId, yy, mo, dd, hh, mm, ss, sleepMin);
  enqueuePacket("0x53", hex, metricsJson);
}

void handleHeartRateHistoryPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char ptype[8];
  snprintf(ptype, sizeof(ptype), "0x%02X", data[0]);
  enqueuePacket(ptype, hex);
}

void handleHrvHistoryPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  enqueuePacket("0x56", hex);
}

void handleSpo2HistoryPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char ptype[8];
  snprintf(ptype, sizeof(ptype), "0x%02X", data[0]);
  enqueuePacket(ptype, hex);
}

void handleTemperatureHistoryPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char ptype[8];
  snprintf(ptype, sizeof(ptype), "0x%02X", data[0]);
  enqueuePacket(ptype, hex);
}

void handleSportHistoryPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  enqueuePacket("0x5C", hex);
}

void handleRawPacket(uint8_t* data, size_t len) {
  char hex[50];
  storeHex(hex, sizeof(hex), data, len < 16 ? len : 16);
  char ptype[8];
  snprintf(ptype, sizeof(ptype), "0x%02X", data[0]);
  enqueuePacket(ptype, hex);
}

// ════════════════════════════════════════════════════════════════════════════
//  BLE — ROTEADOR DE NOTIFICAÇÕES
// ════════════════════════════════════════════════════════════════════════════

void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (!bleLinkUp || len < 2) return;

  switch (data[0]) {
    case 0x13: handleBatteryPacket(data, len);            break;
    case 0x22: handleMacPacket(data, len);                break;
    case 0x27: handleFirmwarePacket(data, len);           break;
    case 0x28: handleHealthPacket(data, len);             break;
    case 0x09: handleRealtimeActivityPacket(data, len);   break;
    case 0x18: handleSportRealtimePacket(data, len);      break;
    case 0x53: handleSleepPacket(data, len);              break;
    case 0x54:
    case 0x55: handleHeartRateHistoryPacket(data, len);   break;
    case 0x56: handleHrvHistoryPacket(data, len);         break;
    case 0x60:
    case 0x66: handleSpo2HistoryPacket(data, len);        break;
    case 0x62:
    case 0x65: handleTemperatureHistoryPacket(data, len); break;
    case 0x5C: handleSportHistoryPacket(data, len);       break;
    default:   handleRawPacket(data, len);                break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  BLE — GERENCIAMENTO DE CONEXÃO
// ════════════════════════════════════════════════════════════════════════════

void resetMeasurementState() {
  captured13   = false;
  capturedBP   = false;
  readPhase    = -1;
  phaseRetries = 0;
  gotFirmware  = false;
  gotMacResp   = false;
  memset(captured, 0, sizeof(captured));
  memset(rawHex,   0, sizeof(rawHex));
  rawHex13[0] = '\0';
  measureState = MeasureState::IDLE;
}

bool bindBraceletCharacteristics() {
  if (!bleClient || !bleClient->isConnected()) return false;
  BLERemoteService* svc = bleClient->getService(BLEUUID((uint16_t)0xFFF0));
  if (!svc) return false;
  txChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  rxChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF7));
  if (!txChar || !rxChar) return false;
  if (rxChar->canNotify()) rxChar->registerForNotify(notifyCallback);
  bleLinkUp = true;
  bleState  = BleState::CONNECTED;
  return true;
}

void releaseBleStack() {
  bleLinkUp = false;
  bleState  = BleState::DISCONNECTED;
  txChar = rxChar = nullptr;

  if (bleClient) {
    if (bleClient->isConnected()) {
      bleClient->disconnect();
      delay(250);
    }
    // Não usar delete — BLEDevice::deinit() libera o client (delete causa CORRUPT HEAP).
    bleClient = nullptr;
  }

  if (bleStackInitialized) {
    BLEDevice::deinit(true);
    bleStackInitialized = false;
    delay(800);
  }
}

bool startBleStack() {
  if (bleStackInitialized) return true;
  BLEDevice::init("");
  bleStackInitialized = true;
  delay(200);
  return true;
}

bool connectToBraceletOnce() {
  txChar = rxChar = nullptr;
  bleLinkUp = false;

  if (!bleStackInitialized) return false;

  bleClient = BLEDevice::createClient();
  if (!bleClient) return false;
  bleClient->setClientCallbacks(&braceletCallbacks);

  String mac = String(DEVICE_MAC);
  mac.toLowerCase();
  Serial.printf("[BLE] Conectando %s (timeout %ums)...\n",
                mac.c_str(), BLE_CONNECT_TIMEOUT_MS);

  if (!bleClient->connect(BLEAddress(mac.c_str()),
                          BLE_ADDR_TYPE_RANDOM, BLE_CONNECT_TIMEOUT_MS)) {
    Serial.println("[BLE] connect() falhou.");
    return false;
  }

  if (!bindBraceletCharacteristics()) {
    Serial.println("[BLE] Serviço/características não encontrados.");
    return false;
  }

  Serial.println("[BLE] Conectado e pronto.");
  return true;
}

bool connectToBracelet() {
  for (int attempt = 1; attempt <= BLE_CONNECT_MAX_ATTEMPTS; attempt++) {
    Serial.printf("[BLE] Tentativa %d/%d\n", attempt, BLE_CONNECT_MAX_ATTEMPTS);
    releaseBleStack();
    if (!startBleStack()) continue;
    if (connectToBraceletOnce()) return true;
    releaseBleStack();
    if (attempt < BLE_CONNECT_MAX_ATTEMPTS) {
      // Aproveita a espera para processar WiFi e fila HTTP
      unsigned long t = millis();
      while (millis() - t < BLE_RETRY_DELAY_MS) {
        ensureWifi();
        delay(200);
      }
    }
  }
  return false;
}

bool isBraceletConnected() {
  return bleClient && bleClient->isConnected() && txChar && rxChar && bleLinkUp;
}

bool ensureBraceletConnected() {
  if (isBraceletConnected()) return true;

  unsigned long now = millis();
  if (now - lastBleReconnectAttempt < BLE_RETRY_DELAY_MS) return false;
  lastBleReconnectAttempt = now;

  Serial.println("[BLE] Pulseira offline. Tentando reconectar...");
  bleState = BleState::RECONNECTING;

  // Reconexão limpa sem derrubar WiFi
  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
    delay(200);
  }
  bleClient = nullptr;
  txChar = rxChar = nullptr;
  bleLinkUp = false;

  if (bleStackInitialized) {
    BLEDevice::deinit(true);
    bleStackInitialized = false;
    delay(600);
  }

  if (!startBleStack() || !connectToBraceletOnce()) {
    bleState = BleState::DISCONNECTED;
    Serial.printf("[BLE] Reconexão falhou. Próxima tentativa em %lus.\n",
                  BLE_RETRY_DELAY_MS / 1000);
    return false;
  }

  resetMeasurementState();
  braceletInitSequence();
  return true;
}

void syncDeviceTime() {
  struct tm ti;
  if (!getLocalTime(&ti, 3000)) return;
  sendSetTime(toBcd(ti.tm_year % 100), toBcd(ti.tm_mon + 1), toBcd(ti.tm_mday),
              toBcd(ti.tm_hour), toBcd(ti.tm_min), toBcd(ti.tm_sec));
  delay(400);
}

void braceletInitSequence() {
  if (!isBraceletConnected()) return;
  stopAllHealthModes();
  delay(300);
  readBattery();
  lastScanCmd = millis();
  readMAC();     delay(200);
  readFirmware();delay(200);
  syncDeviceTime();
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTP — DIAGNÓSTICO E ENVIO
// ════════════════════════════════════════════════════════════════════════════

// Valida se o payload contém os campos obrigatórios da API.
// Retorna true se válido, false e loga o campo ausente se inválido.
bool validateJsonPayload(const char* payload) {
  const char* required[] = {
    "\"deviceMac\"", "\"packetType\"", "\"rawHex\"", "\"source\""
  };
  bool ok = true;
  for (int i = 0; i < 4; i++) {
    if (strstr(payload, required[i]) == nullptr) {
      Serial.printf("[HTTP][VALIDAÇÃO] Campo ausente: %s\n", required[i]);
      ok = false;
    }
  }
  // Avisa se metrics está presente mas metrics={} (objeto vazio)
  const char* mPos = strstr(payload, "\"metrics\"");
  if (mPos && strstr(mPos, "\"metrics\":{}") != nullptr) {
    Serial.println("[HTTP][VALIDAÇÃO] AVISO: metrics está vazio {}");
  }
  // Avisa rawHex vazio
  const char* rxPos = strstr(payload, "\"rawHex\":\"\"");
  if (rxPos) Serial.println("[HTTP][VALIDAÇÃO] AVISO: rawHex está vazio");
  return ok;
}

bool doPost(const char* payload) {
  // ── 1. WiFi ──────────────────────────────────────────────────────────────
  wl_status_t wifiStatus = (wl_status_t)WiFi.status();
  Serial.printf("[HTTP] WiFi status: %d (%s)\n",
                wifiStatus,
                wifiStatus == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");

  if (!ensureWifi()) {
    Serial.println("[HTTP] ERRO: WiFi indisponível — POST abortado.");
    return false;
  }

  // ── 2. Validação do payload ───────────────────────────────────────────────
  size_t payloadLen = strlen(payload);
  Serial.printf("[HTTP] POST %s\n", API_URL);
  Serial.printf("[HTTP] Payload (%u bytes):\n%s\n", payloadLen, payload);

  if (!validateJsonPayload(payload)) {
    Serial.println("[HTTP] ERRO: payload inválido — POST abortado.");
    return false;
  }

  // ── 3. Conexão SSL ────────────────────────────────────────────────────────
  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(30);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(20000);

  Serial.printf("[HTTP] Conectando a %s...\n", API_URL);
  if (!http.begin(sc, API_URL)) {
    Serial.println("[HTTP] ERRO: http.begin() falhou (DNS ou SSL).");
    return false;
  }
  Serial.println("[HTTP] http.begin() OK");

  http.addHeader("Content-Type", "application/json");

  // ── 4. POST ───────────────────────────────────────────────────────────────
  unsigned long t0 = millis();
  int code = http.POST((uint8_t*)payload, payloadLen);
  unsigned long elapsed = millis() - t0;

  // ── 5. Resultado ──────────────────────────────────────────────────────────
  Serial.printf("[HTTP] Status: %d  (em %lums)\n", code, elapsed);

  if (code <= 0) {
    // Erros negativos do HTTPClient
    switch (code) {
      case HTTPC_ERROR_CONNECTION_REFUSED:
        Serial.println("[HTTP] ERRO: conexão recusada pelo servidor."); break;
      case HTTPC_ERROR_SEND_HEADER_FAILED:
        Serial.println("[HTTP] ERRO: falha ao enviar headers."); break;
      case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
        Serial.println("[HTTP] ERRO: falha ao enviar payload."); break;
      case HTTPC_ERROR_NOT_CONNECTED:
        Serial.println("[HTTP] ERRO: não conectado."); break;
      case HTTPC_ERROR_CONNECTION_LOST:
        Serial.println("[HTTP] ERRO: conexão perdida durante envio."); break;
      case HTTPC_ERROR_READ_TIMEOUT:
        Serial.println("[HTTP] ERRO: timeout aguardando resposta da API."); break;
      default:
        Serial.printf("[HTTP] ERRO HTTPClient: %d\n", code); break;
    }
    Serial.printf("[HTTP] Payload que falhou:\n%s\n", payload);
    http.end();
    return false;
  }

  // Response body
  String responseBody = http.getString();
  if (responseBody.length() > 0) {
    Serial.printf("[HTTP] Response:\n%s\n", responseBody.c_str());
  }

  bool ok = (code == 200 || code == 201);
  if (ok) {
    Serial.println("[HTTP] OK — pacote aceito pela API.");
  } else {
    Serial.printf("[HTTP] FALHA — API rejeitou (HTTP %d).\n", code);
    Serial.printf("[HTTP] Payload rejeitado:\n%s\n", payload);
  }

  http.end();
  return ok;
}

// Verifica se a API está online e loga o resultado.
void checkApiHealth() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[API] OFFLINE (sem WiFi)");
    return;
  }
  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(15);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  String url = String(API_BASE) + "/health";
  Serial.printf("[API] GET %s\n", url.c_str());
  if (!http.begin(sc, url)) {
    Serial.println("[API] OFFLINE (begin falhou)");
    return;
  }
  int code = http.GET();
  String body = http.getString();
  if (code == 200) {
    Serial.printf("[API] ONLINE (HTTP %d) — %s\n", code, body.c_str());
  } else {
    Serial.printf("[API] OFFLINE (HTTP %d) — %s\n", code, body.c_str());
  }
  http.end();
}

void processHttpQueue() {
  if (httpQueueCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastHttpRetry < HTTP_RETRY_INTERVAL_MS) return;

  Serial.printf("[HTTP] Fila: %d pendentes — enviando...\n", httpQueueCount);
  const char* payload = httpQueue[httpQueueHead].payload;
  if (doPost(payload)) {
    dequeueHttp();
    lastHttpRetry = 0;
    if (httpQueueCount > 0)
      Serial.printf("[HTTP] Fila: %d restantes\n", httpQueueCount);
  } else {
    lastHttpRetry = now;
    Serial.printf("[HTTP] Fila: nova tentativa em %lus\n",
                  HTTP_RETRY_INTERVAL_MS / 1000);
  }
}

bool postBatteryPacket() {
  uint8_t pkt[8] = { 0 };
  for (int i = 0; i < 8; i++) {
    unsigned int v = 0;
    sscanf(rawHex13 + i * 3, "%2X", &v);
    pkt[i] = (uint8_t)v;
  }
  uint16_t s = 0;
  for (int i = 0; i < 7; i++) s += pkt[i];
  pkt[7] = s & 0xFF;

  char fixedHex[25] = { 0 };
  for (int i = 0; i < 8; i++) sprintf(fixedHex + i * 3, "%02X ", pkt[i]);
  fixedHex[23] = '\0';
  if (fixedHex[22] == ' ') fixedHex[22] = '\0';

  char payload[220];
  snprintf(payload, sizeof(payload),
           "{\"deviceMac\":\"%s\",\"packetType\":\"0x13\","
           "\"rawHex\":\"%s\",\"source\":\"ESP32\"}",
           DEVICE_MAC, fixedHex);
  enqueueHttp(payload);
  return true;
}

bool postVitals() {
  uint8_t pktSpo2[16] = { 0 }, pktTemp[16] = { 0 }, pktBp[16] = { 0 };
  bool hasSpo2 = captured[SLOT_SPO2] && parseHex16(rawHex[SLOT_SPO2], pktSpo2);
  bool hasTemp = captured[SLOT_TEMP] && parseHex16(rawHex[SLOT_TEMP], pktTemp);
  bool hasBp2  = captured[SLOT_BP]   && parseHex16(rawHex[SLOT_BP],   pktBp);

  uint8_t  bpm     = hasSpo2 ? pktSpo2[2] : 0;
  uint8_t  spo2    = hasSpo2 ? pktSpo2[3] : 0;
  uint16_t tempRaw = hasTemp
    ? ((uint16_t)pktTemp[8] | ((uint16_t)pktTemp[9] << 8))
    : 0;

  if (!hasSpo2 || !hasTemp || bpm == 0 || spo2 == 0 || tempRaw == 0) {
    Serial.printf(
      "[APP] Obrigatórios incompletos — BPM=%u SpO2=%u Temp=%.1f°C. Abortando POST.\n",
      bpm, spo2, tempRaw / 10.0f);
    return false;
  }

  uint8_t sys = hasBp2 ? pktBp[6] : 0;
  uint8_t dia = hasBp2 ? pktBp[7] : 0;

  uint8_t merged[16] = { 0 };
  merged[0]  = 0x28;
  merged[1]  = 0x02;
  merged[2]  = bpm;
  merged[3]  = spo2;
  merged[6]  = sys;
  merged[7]  = dia;
  merged[8]  = tempRaw & 0xFF;
  merged[9]  = (tempRaw >> 8) & 0xFF;
  merged[15] = calcCrc(merged);

  char mergedHex[50] = { 0 };
  storeHex(mergedHex, sizeof(mergedHex), merged, 16);

  Serial.printf("[APP] Vitais — BPM=%u SpO2=%u Temp=%.1f°C PA=%s\n",
                bpm, spo2, tempRaw / 10.0f,
                hasBp2 ? (String(sys) + "/" + String(dia) + " mmHg").c_str()
                       : "não medida");

  char payload[512];
  if (hasBp2) {
    snprintf(payload, sizeof(payload),
             "{\"deviceMac\":\"%s\",\"packetType\":\"0x28\",\"rawHex\":\"%s\","
             "\"source\":\"ESP32\",\"metrics\":{\"mode\":\"merged\","
             "\"type\":\"0x28\",\"modo_solicitado\":\"vitals\","
             "\"heartRate\":%u,\"spO2\":%u,\"temperature\":%.1f,"
             "\"bloodPressure\":\"%u/%u\"}}",
             DEVICE_MAC, mergedHex, bpm, spo2, tempRaw / 10.0f, sys, dia);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"deviceMac\":\"%s\",\"packetType\":\"0x28\",\"rawHex\":\"%s\","
             "\"source\":\"ESP32\",\"metrics\":{\"mode\":\"merged\","
             "\"type\":\"0x28\",\"modo_solicitado\":\"vitals\","
             "\"heartRate\":%u,\"spO2\":%u,\"temperature\":%.1f}}",
             DEVICE_MAC, mergedHex, bpm, spo2, tempRaw / 10.0f);
  }

  enqueueHttp(payload);
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  MÁQUINA DE ESTADOS — MEDIÇÃO
// ════════════════════════════════════════════════════════════════════════════

unsigned long timeoutForPhase(int phase) {
  if (phase == 0) return SPO2_TIMEOUT_MS;
  if (phase == 1) return TEMP_TIMEOUT_MS;
  if (phase == 2) return BP_TIMEOUT_MS;
  return 15000;
}

bool phaseGoalsMet(int phase) {
  if (phase == 2) return capturedBP;
  return captured[phase];
}

bool canAdvanceFromPhase(int phase) {
  if (phase == 0) return captured[SLOT_SPO2];
  if (phase == 1) return captured[SLOT_TEMP];
  if (phase == 2) return true;
  return false;
}

void startPhase(int phase) {
  phaseRetries = 0;
  stopAllHealthModes();
  delay(200);
  sendCommand(0x28, PHASE_CMD[phase], 0x01);
  Serial.printf("[BLE] Fase %d — %s\n", phase, PHASE_LABELS[phase]);
  lastScanCmd = millis();
}

void advancePhase() {
  sendCommand(0x28, PHASE_CMD[readPhase], 0x00);
  delay(100);
  readPhase++;
  phaseRetries = 0;
  if (readPhase < NUM_PHASES) startPhase(readPhase);
}

void processMeasurementStateMachine() {
  if (!isBraceletConnected()) return;

  switch (measureState) {

    case MeasureState::IDLE:
      resetMeasurementState();
      readBattery();
      lastScanCmd     = millis();
      deviceInfoStart = millis();
      measureState    = MeasureState::WAIT_BATTERY;
      break;

    case MeasureState::WAIT_BATTERY:
      if (captured13 || millis() - deviceInfoStart > BATTERY_TIMEOUT_MS) {
        if (!captured13) Serial.println("[BLE] Bateria não respondeu — seguindo.");
        readPhase    = 0;
        measureState = MeasureState::PHASE_START;
      }
      break;

    case MeasureState::PHASE_START:
      if (readPhase < NUM_PHASES) {
        startPhase(readPhase);
        measureState = MeasureState::PHASE_WAIT;
      } else {
        measureState = MeasureState::COMPLETE;
      }
      break;

    case MeasureState::PHASE_WAIT: {
      bool goalMet = phaseGoalsMet(readPhase);
      if (goalMet) {
        if (readPhase == NUM_PHASES - 1) {
          readPhase    = NUM_PHASES;
          measureState = MeasureState::COMPLETE;
        } else {
          advancePhase();
          measureState = (readPhase < NUM_PHASES)
                       ? MeasureState::PHASE_WAIT
                       : MeasureState::COMPLETE;
        }
        return;
      }

      if (millis() - lastScanCmd > timeoutForPhase(readPhase)) {
        phaseRetries++;
        Serial.printf("[BLE] Timeout %s (%d/%d)\n",
                      PHASE_LABELS[readPhase], phaseRetries, MAX_PHASE_RETRIES);

        if (phaseRetries >= MAX_PHASE_RETRIES) {
          if (!canAdvanceFromPhase(readPhase)) {
            // Fase obrigatória sem dados — reinicia ciclo após intervalo
            Serial.printf("[APP] Sinal obrigatório %s não capturado. Aguardando próximo ciclo.\n",
                          PHASE_LABELS[readPhase]);
            stopAllHealthModes();
            postWaitStart = millis();
            measureState  = MeasureState::POST_WAIT;
          } else {
            advancePhase();
            measureState = (readPhase < NUM_PHASES)
                         ? MeasureState::PHASE_WAIT
                         : MeasureState::COMPLETE;
          }
        } else {
          sendCommand(0x28, PHASE_CMD[readPhase], 0x01);
          lastScanCmd = millis();
        }
      }
      break;
    }

    case MeasureState::COMPLETE:
      stopAllHealthModes();
      if (captured13) postBatteryPacket();
      postVitals();
      Serial.printf("[APP] Ciclo completo — próxima medição em %lus\n",
                    MEASURE_INTERVAL_MS / 1000);
      postWaitStart = millis();
      measureState  = MeasureState::POST_WAIT;
      break;

    case MeasureState::POST_WAIT:
      if (millis() - postWaitStart >= MEASURE_INTERVAL_MS) {
        measureState = MeasureState::IDLE;
      }
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  PING PERIÓDICO
// ════════════════════════════════════════════════════════════════════════════

void pingServerPeriodically() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastPingTime < PING_INTERVAL_MS) return;
  lastPingTime = now;
  checkApiHealth();
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  memset(httpQueue, 0, sizeof(httpQueue));

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Conectado. IP=%s | RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  WiFi.setSleep(false);

  checkApiHealth();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  resetMeasurementState();
  while (!connectToBracelet()) {
    Serial.printf("[BLE] Pulseira indisponível. Nova tentativa em %lus...\n",
                  BLE_RETRY_DELAY_MS / 1000);
    ensureWifi();
    delay(BLE_RETRY_DELAY_MS);
  }
  braceletInitSequence();
  measureState = MeasureState::IDLE;
}

void loop() {
  ensureWifi();

  if (!ensureBraceletConnected()) {
    processHttpQueue();
    delay(100);
    return;
  }

  processMeasurementStateMachine();
  processHttpQueue();
  pingServerPeriodically();

  delay(50);
}
