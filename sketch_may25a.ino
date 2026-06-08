/**
 * 2208A + ESP32 — protocolo PDF "2208A API V1" §33
 *
 * Medição ativa 0x28 AA BB (AA = tipo, BB = 1 liga / 0 desliga):
 *   0x01 HRV   0x02 Coração (+ pressão FF/GG)   0x03 SpO2   0x04 Temperatura
 *
 * Resposta ~1/s: 0x28 AA BB CC DD EE FF GG ...
 *   BB=HR  CC=SpO2  DD=HRV  EE=fadiga  FF=sis  GG=dia
 *
 * Pressão arterial: NÃO tem modo separado no PDF — vem em FF/GG durante 0x02.
 * HRV (tempo real): TX 28 01 01 ... CRC=2A → notify DD=HRV, EE=fadiga
 * HRV (histórico): TX 56 00 ... CRC=56 → D1=HRV, D4=fadiga
 */
 #include <WiFi.h>
 #include <WiFiClientSecure.h>
 #include <HTTPClient.h>
 #include <BLEDevice.h>
 #include <BLEUtils.h>
 #include <BLEClient.h>
 #include <time.h>
 
 const char* WIFI_SSID  = "IoTs";
 const char* WIFI_PASS  = "AneryIot158";
 const char* API_URL    = "https://bracelet-pn7r.onrender.com/bracelets/packets";
 const char* DEVICE_MAC = "E6:64:0D:30:D3:F9";
 
 const unsigned long HEART_BP_TIMEOUT_MS = 120000;
 const unsigned long SPO2_TIMEOUT_MS = 50000;
 const unsigned long SPO2_WARMUP_MS = 15000;
 const unsigned long HRV_TIMEOUT_MS = 90000;
 const unsigned long HRV_HIST_TIMEOUT_MS = 8000;
 const unsigned long BATTERY_TIMEOUT_MS = 6000;
 const int MAX_PHASE_RETRIES = 5;
 
const int SLOT_CORACAO = 0;
const int SLOT_SPO2 = 1;
const int SLOT_HRV = 2;
const int NUM_SLOTS = 3;

bool capturedPressure = false;
 
 // Fases de comando (0x28 AA) — pressão usa a mesma fase 0x02
 const uint8_t PHASE_CMD_MODES[] = {0x02, 0x03, 0x01};
 const char*   PHASE_LABELS[]    = {"Coracao+Pressao", "Oxigenio", "HRV"};
 const int     NUM_PHASES        = 3;
 
 int phaseRetries = 0;
 bool hrvQueryHistory = false;
 
 BLEClient*               bleClient = nullptr;
 BLERemoteCharacteristic* txChar    = nullptr;
 BLERemoteCharacteristic* rxChar    = nullptr;
 
 const char* SLOT_LABELS[] = {"Coracao", "Oxigenio", "HRV"};
 
 char rawHex28[NUM_SLOTS][50] = {{0}};
 bool captured28[NUM_SLOTS] = {false};
 char rawHex56[50] = "";
 bool captured56 = false;
 char rawHex13[24] = "";
 bool captured13 = false;
 
 int readPhase = -1;
 unsigned long lastScanCmd = 0;
 
 uint8_t calcCrc(uint8_t* p) {
   uint16_t s = 0;
   for (int i = 0; i < 15; i++) s += p[i];
   return s & 0xFF;
 }
 
 uint8_t toBcd(int v) {
   return (uint8_t)(((v / 10) << 4) | (v % 10));
 }
 
 void logTxPacket(uint8_t* pkt) {
   Serial.printf(
     "[BLE] TX: %02X %02X %02X ... CRC=%02X\n",
     pkt[0], pkt[1], pkt[2], pkt[15]
   );
 }
 
 void sendPacket16(uint8_t* pkt) {
   if (!txChar) return;
   pkt[15] = calcCrc(pkt);
   txChar->writeValue(pkt, 16, true);
 }
 
 void sendCommand(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0) {
   uint8_t pkt[16] = {cmd, p1, p2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   sendPacket16(pkt);
 }
 
 /** PDF §1: 0x01 AA BB CC DD EE FF — data/hora em BCD. */
 void sendSetTime(uint8_t yearBcd, uint8_t monBcd, uint8_t dayBcd, uint8_t hourBcd, uint8_t minBcd, uint8_t secBcd) {
   uint8_t pkt[16] = {0x01, yearBcd, monBcd, dayBcd, hourBcd, minBcd, secBcd, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   sendPacket16(pkt);
 }
 
 void stopAllHealthModes() {
   sendCommand(0x28, 0x01, 0x00);
   delay(50);
   sendCommand(0x28, 0x02, 0x00);
   delay(50);
   sendCommand(0x28, 0x03, 0x00);
   delay(50);
   sendCommand(0x28, 0x04, 0x00);
   delay(50);
 }
 
 void disableRealtimeStream() {
   sendCommand(0x09, 0x00, 0x00);
 }
 
bool isCalibrationBp(uint8_t sys, uint8_t dia) {
  return sys == 0x7B && dia == 0x49;
}

/** Valor fixo 0x75/0x44 (117/68) que aparece em todo notify — não é PA real. */
bool isStaleBraceletBp(uint8_t sys, uint8_t dia) {
  return sys == 0x75 && dia == 0x44;
}

bool isPressureReady(uint8_t* data, size_t len) {
  if (len < 8) return false;
  uint8_t sys = data[6];
  uint8_t dia = data[7];
  if (sys == 0 || dia == 0) return false;
  if (isCalibrationBp(sys, dia)) return false;
  if (isStaleBraceletBp(sys, dia)) return false;
  return sys < 250 && dia < 200 && sys > dia;
}
 
 bool isHrvActiveReady(uint8_t* data, size_t len) {
   if (len < 6 || data[0] != 0x28 || data[1] != 0x01) return false;
   return data[4] > 0 || data[5] > 0;
 }
 
bool phaseGoalsMet(int phase) {
  if (phase == 0) return captured28[SLOT_CORACAO];
  if (phase == 1) return captured28[SLOT_SPO2];
  if (phase == 2) return captured28[SLOT_HRV];
  return false;
}
 
 bool phaseMinimumMet(int phase) {
   if (phase == 0) return captured28[SLOT_CORACAO];
   return phaseGoalsMet(phase);
 }
 
 void storeHex(char* dest, size_t destSize, uint8_t* data, size_t len) {
   char tmp[50] = {0};
   size_t maxBytes = len < 16 ? len : 16;
 
   for (size_t i = 0; i < maxBytes; i++) {
     sprintf(tmp + i * 3, "%02X ", data[i]);
   }
 
   size_t tl = strlen(tmp);
   if (tl > 0 && tmp[tl - 1] == ' ') tmp[tl - 1] = '\0';
 
   strncpy(dest, tmp, destSize - 1);
 }
 
 bool synthesizeHrvFrom56(uint8_t* data, size_t len, char* dest, size_t destSize) {
   if (len < 13 || data[0] != 0x56) return false;
 
   uint8_t hrv = data[9];
   uint8_t fatigue = data[12];
   if (hrv == 0 && fatigue == 0) return false;
 
   uint8_t pkt[16] = {0x28, 0x01, 0, 0, hrv, fatigue, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   pkt[15] = calcCrc(pkt);
   storeHex(dest, destSize, pkt, 16);
   return true;
 }
 
bool parseHex16(const char* hex, uint8_t* out) {
  if (!hex || !out) return false;
  for (int i = 0; i < 16; i++) {
    unsigned int byteVal = 0;
    if (sscanf(hex + i * 3, "%2X", &byteVal) != 1) return false;
    out[i] = (uint8_t)byteVal;
  }
  return true;
}

void captureSlot(int slot, uint8_t* data, size_t len) {
  storeHex(rawHex28[slot], sizeof(rawHex28[slot]), data, len);
  captured28[slot] = true;
  Serial.printf("[BLE] %s OK: %s\n", SLOT_LABELS[slot], rawHex28[slot]);
}

void patchHeartSlotPressure(uint8_t sys, uint8_t dia) {
  if (!captured28[SLOT_CORACAO]) return;

  uint8_t pkt[16];
  if (!parseHex16(rawHex28[SLOT_CORACAO], pkt)) return;

  pkt[6] = sys;
  pkt[7] = dia;
  pkt[15] = calcCrc(pkt);
  storeHex(rawHex28[SLOT_CORACAO], sizeof(rawHex28[SLOT_CORACAO]), pkt, 16);
  capturedPressure = true;

  Serial.printf("[BLE] Pressão mesclada no slot coração: %u/%u mmHg\n", sys, dia);
}
 
 void captureHrvFrom56(uint8_t* data, size_t len) {
   if (len < 13 || data[0] != 0x56) return;
   if (data[9] == 0 && (len <= 12 || data[12] == 0)) return;
 
   storeHex(rawHex56, sizeof(rawHex56), data, len);
   captured56 = true;
 
   synthesizeHrvFrom56(data, len, rawHex28[SLOT_HRV], sizeof(rawHex28[SLOT_HRV]));
   captured28[SLOT_HRV] = true;
 
   Serial.printf(
     "[BLE] HRV histórico 0x56: D1=%u D4=%u | raw: %s\n",
     data[9],
     len > 12 ? data[12] : 0,
     rawHex56
   );
 }
 
 /** Resposta 0x28 modo 0x01 — DD=HRV, EE=fadiga (PDF §33). */
 void handleHrvPacket(uint8_t* data, size_t len) {
   if (len < 10 || data[0] != 0x28 || data[1] != 0x01) return;
 
   if (!captured28[SLOT_HRV] && (data[4] > 0 || data[5] > 0)) {
     captureSlot(SLOT_HRV, data, len);
     Serial.printf("[BLE] HRV tempo real: DD=%u ms, EE=%u (fadiga)\n", data[4], data[5]);
     return;
   }
 
   if (!captured28[SLOT_HRV]) {
     Serial.printf(
       "[BLE] HRV 0x01 aguardando... DD=%u EE=%u (precisa >0) HR=%u\n",
       data[4], data[5], data[2]
     );
   }
 }
 
 void handleHeartBpPacket(uint8_t* data, size_t len) {
   if (len < 10 || data[0] != 0x28 || data[1] != 0x02) return;
 
   if (!captured28[SLOT_CORACAO] && data[2] > 0) {
     captureSlot(SLOT_CORACAO, data, len);
     Serial.println("[BLE] Pressão: mantenha o braço parado ~60–90s (FF/GG no PDF §33)...");
   }
 
  if (!capturedPressure && isPressureReady(data, len)) {
    patchHeartSlotPressure(data[6], data[7]);
  }

  if (!captured28[SLOT_CORACAO] || !capturedPressure) {
     Serial.printf(
       "[BLE] 0x02 aguardando... HR=%u sis=%u dia=%u\n",
       data[2], data[6], data[7]
     );
   }
 }
 
 bool isPacketReadyForPhase(int phase, uint8_t* data, size_t len) {
   if (phase == 2 && hrvQueryHistory) {
     if (len < 13 || data[0] != 0x56) return false;
     char tmp[50];
     return synthesizeHrvFrom56(data, len, tmp, sizeof(tmp));
   }
 
   if (len < 10 || data[0] != 0x28) return false;
 
   switch (phase) {
     case 1:
       return data[1] == 0x03 && data[3] > 0;
     case 2:
       return isHrvActiveReady(data, len);
     default:
       return false;
   }
 }
 
 void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
   if (len < 2) return;
 
   if (data[0] == 0x13) {
     if (readPhase != -1 || captured13) return;
     storeHex(rawHex13, sizeof(rawHex13), data, len);
     captured13 = true;
     Serial.printf("[BLE] Bateria: %u%% | raw: %s\n", len > 1 ? data[1] : 0, rawHex13);
     return;
   }
 
   if (readPhase < 0 || readPhase >= NUM_PHASES) return;
 
   if (readPhase == 0 && data[0] == 0x28) {
     handleHeartBpPacket(data, len);
     return;
   }
 
   if (readPhase == 2 && hrvQueryHistory && data[0] == 0x56) {
     if (captured28[SLOT_HRV]) return;
     captureHrvFrom56(data, len);
     return;
   }
 
   if (readPhase == 2 && !hrvQueryHistory && data[0] == 0x28) {
     handleHrvPacket(data, len);
     return;
   }
 
   if (readPhase == 1) {
     if (captured28[SLOT_SPO2] || data[0] != 0x28 || len < 10) return;
     if (!isPacketReadyForPhase(1, data, len)) {
       Serial.printf(
         "[BLE] Oxigenio aguardando... SpO2=%u HR=%u\n",
         data[3], data[2]
       );
       return;
     }
     captureSlot(SLOT_SPO2, data, len);
   }
 }
 
 bool connectToBracelet() {
   bleClient = BLEDevice::createClient();
 
   String macLower = String(DEVICE_MAC);
   macLower.toLowerCase();
 
   if (!bleClient->connect(BLEAddress(macLower.c_str()), BLE_ADDR_TYPE_RANDOM)) {
     return false;
   }
 
   BLERemoteService* svc = bleClient->getService(BLEUUID((uint16_t)0xFFF0));
   if (!svc) return false;
 
   txChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
   rxChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF7));
   if (!txChar || !rxChar) return false;
 
   if (rxChar->canNotify()) {
     rxChar->registerForNotify(notifyCallback);
   }
 
   return true;
 }
 
 void bleStop() {
   if (bleClient && bleClient->isConnected()) {
     bleClient->disconnect();
   }
   txChar = nullptr;
   rxChar = nullptr;
   bleClient = nullptr;
   BLEDevice::deinit(true);
   delay(500);
 }
 
 unsigned long timeoutForPhase(int phase) {
   if (phase == 0) return HEART_BP_TIMEOUT_MS;
   if (phase == 1) return SPO2_TIMEOUT_MS;
   if (phase == 2) return hrvQueryHistory ? HRV_HIST_TIMEOUT_MS : HRV_TIMEOUT_MS;
   return 15000;
 }
 
 void syncDeviceTime() {
   configTime(0, 0, "pool.ntp.org", "time.nist.gov");
   struct tm ti;
   if (!getLocalTime(&ti, 8000)) {
     Serial.println("[BLE] Relógio NTP indisponível — pulando 0x01");
     return;
   }
   sendSetTime(
     toBcd(ti.tm_year % 100),
     toBcd(ti.tm_mon + 1),
     toBcd(ti.tm_mday),
     toBcd(ti.tm_hour),
     toBcd(ti.tm_min),
     toBcd(ti.tm_sec)
   );
   Serial.println("[BLE] 0x01 — hora sincronizada na pulseira");
   delay(400);
 }
 
 void startHrvHistoryRead() {
   hrvQueryHistory = true;
   phaseRetries = 0;
   sendCommand(0x28, 0x01, 0x00);
   delay(150);
   Serial.println("[BLE] 0x56 — último HRV/fadiga gravado na pulseira");
   sendCommand(0x56, 0x00);
   lastScanCmd = millis();
 }
 
 void startHealthScan(int phase) {
   phaseRetries = 0;
   hrvQueryHistory = false;
   stopAllHealthModes();
   disableRealtimeStream();
   delay(300);
 
   uint8_t cmdMode = PHASE_CMD_MODES[phase];
   sendCommand(0x28, cmdMode, 0x01);
   Serial.printf("[BLE] 0x28 modo 0x%02X ON (%s)\n", cmdMode, PHASE_LABELS[phase]);
 
   if (phase == 0) {
     sendCommand(0x36, 0x01);
     Serial.println("[BLE] PDF §33: pressão vem em FF/GG durante modo 0x02 (coração).");
     Serial.println("[BLE] Deixe o braço parado até 2 min após o HR aparecer.");
     delay(3000);
   } else if (phase == 1) {
     Serial.printf("[BLE] SpO2: aguarde %lus...\n", SPO2_WARMUP_MS / 1000);
     delay(SPO2_WARMUP_MS);
   } else if (phase == 2) {
     Serial.println("[BLE] HRV: enviando 28 01 01 ... (CRC=0x2A)");
     uint8_t demo[16] = {0x28, 0x01, 0x01, 0};
     demo[15] = calcCrc(demo);
     logTxPacket(demo);
     Serial.println("[BLE] Resposta esperada: 28 01 BB CC DD EE ... (DD=HRV, EE=fadiga)");
     delay(5000);
   }
 
   lastScanCmd = millis();
 }
 
 void advancePhase() {
   if (readPhase >= 0 && readPhase < NUM_PHASES && !hrvQueryHistory) {
     sendCommand(0x28, PHASE_CMD_MODES[readPhase], 0x00);
     delay(120);
   }
   hrvQueryHistory = false;
   readPhase++;
   phaseRetries = 0;
 
   if (readPhase < NUM_PHASES) {
     startHealthScan(readPhase);
   }
 }
 
 void skipCurrentPhase() {
   if (readPhase == 2 && !hrvQueryHistory && !captured28[SLOT_HRV]) {
     Serial.println("[BLE] HRV ativo vazio — tentando 0x56...");
     startHrvHistoryRead();
     return;
   }
 
  if (readPhase == 0 && captured28[SLOT_CORACAO] && !capturedPressure) {
    Serial.println("[BLE] Pressão não veio em FF/GG — seguindo sem pressão.");
   } else {
     Serial.printf("[BLE] %s pulado.\n", PHASE_LABELS[readPhase]);
   }
   advancePhase();
 }
 
typedef struct {
  uint8_t heartRate;
  uint8_t spo2;
  uint8_t hrv;
  uint8_t fatigue;
  uint8_t sys;
  uint8_t dia;
  uint16_t tempRaw;
} VitalsMerge;

void absorbPacket(VitalsMerge* m, uint8_t* data) {
  uint16_t tempRaw = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
  if (tempRaw > 0) m->tempRaw = tempRaw;

  switch (data[1]) {
    case 0x01:
      if (data[4] > 0) m->hrv = data[4];
      if (data[5] > 0) m->fatigue = data[5];
      break;
    case 0x02:
      if (data[2] > 0) m->heartRate = data[2];
      if (isPressureReady(data, 16)) {
        m->sys = data[6];
        m->dia = data[7];
      }
      break;
    case 0x03:
      if (data[3] > 0) m->spo2 = data[3];
      if (data[2] > 0) m->heartRate = data[2];
      break;
    default:
      if (data[2] > 0) m->heartRate = data[2];
      if (data[3] > 0) m->spo2 = data[3];
      break;
  }
}

bool buildMergedPacket(uint8_t* pkt, VitalsMerge* out) {
  VitalsMerge merged = {0};
  bool any = false;

  for (int i = 0; i < NUM_SLOTS; i++) {
    if (!captured28[i]) continue;
    uint8_t slotPkt[16];
    if (!parseHex16(rawHex28[i], slotPkt)) continue;
    absorbPacket(&merged, slotPkt);
    any = true;
  }

  if (!any) return false;

  memset(pkt, 0, 16);
  pkt[0] = 0x28;
  pkt[1] = 0x02;
  pkt[2] = merged.heartRate;
  pkt[3] = merged.spo2;
  pkt[4] = merged.hrv;
  pkt[5] = merged.fatigue;
  pkt[6] = merged.sys;
  pkt[7] = merged.dia;
  pkt[8] = merged.tempRaw & 0xFF;
  pkt[9] = (merged.tempRaw >> 8) & 0xFF;
  pkt[15] = calcCrc(pkt);
  *out = merged;
  return true;
}

bool postPacket(const char* packetType, const char* hex) {
  char payload[220];
  snprintf(
    payload,
    sizeof(payload),
    "{\"deviceMac\":\"%s\",\"packetType\":\"%s\",\"rawHex\":\"%s\",\"source\":\"ESP32\"}",
    DEVICE_MAC,
    packetType,
    hex
  );

  Serial.printf("[HTTP] POST %s\n", packetType);
 
   WiFiClientSecure sc;
   sc.setInsecure();
   sc.setHandshakeTimeout(45);
 
   HTTPClient http;
   http.setReuse(false);
   http.setTimeout(30000);
 
   if (!http.begin(sc, API_URL)) return false;
 
   http.addHeader("Content-Type", "application/json");
   int code = http.POST((uint8_t*)payload, strlen(payload));
 
   bool ok = (code == 200);
   Serial.printf("[HTTP] %s (%d)\n", ok ? "OK" : "erro", code);
  http.end();
  return ok;
}

bool postMergedHealthPacket(const char* hex, VitalsMerge* m) {
  char bp[20] = "";
  if (m->sys > 0 && m->dia > 0) {
    snprintf(bp, sizeof(bp), "%u/%u", m->sys, m->dia);
  }

  char payload[512];
  snprintf(
    payload,
    sizeof(payload),
    "{\"deviceMac\":\"%s\",\"packetType\":\"0x28\",\"rawHex\":\"%s\",\"source\":\"ESP32\","
    "\"metrics\":{\"mode\":\"merged\",\"type\":\"0x28\",\"modo_solicitado\":\"completo\","
    "\"heartRate\":%u,\"spO2\":%u,\"hrv\":%u,\"fatigue\":%u,"
    "\"bloodPressure\":\"%s\",\"temperature\":%.1f}}",
    DEVICE_MAC,
    hex,
    m->heartRate,
    m->spo2,
    m->hrv,
    m->fatigue,
    bp,
    m->tempRaw / 10.0f
  );

  Serial.println("[HTTP] POST 0x28 merged");

  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(45);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(30000);

  if (!http.begin(sc, API_URL)) return false;

  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)payload, strlen(payload));

  bool ok = (code == 200);
  Serial.printf("[HTTP] merged %s (%d)\n", ok ? "OK" : "erro", code);
  http.end();
  return ok;
}

void braceletInitSequence() {
  captured13 = false;
  capturedPressure = false;
  readPhase = -1;
  hrvQueryHistory = false;
  memset(captured28, 0, sizeof(captured28));
 
   stopAllHealthModes();
   delay(300);
 
   Serial.println("[BLE] 0x13/0x99 — bateria");
   sendCommand(0x13, 0x99);
   lastScanCmd = millis();
 
   Serial.println("[BLE] 0x22 — MAC");
   sendCommand(0x22);
   delay(600);
 
   syncDeviceTime();
 }
 
 void setup() {
   Serial.begin(115200);
   delay(1000);
 
   WiFi.begin(WIFI_SSID, WIFI_PASS);
   while (WiFi.status() != WL_CONNECTED) {
     delay(500);
     Serial.print(".");
   }
   Serial.printf("\n[WIFI] %s\n", WiFi.localIP().toString().c_str());
   WiFi.setSleep(false);
 
   BLEDevice::init("");
   if (!connectToBracelet()) {
     Serial.println("[BLE] Falha conexão. Reiniciando...");
     delay(3000);
     ESP.restart();
   }
   Serial.println("[BLE] Conectado.");
 
   braceletInitSequence();
 }
 
 void loop() {
   if (readPhase == -1) {
     if (!captured13) {
       if (millis() - lastScanCmd < BATTERY_TIMEOUT_MS) {
         delay(100);
         return;
       }
       Serial.println("[BLE] Bateria não veio — seguindo.");
     }
 
     readPhase = 0;
     startHealthScan(0);
     return;
   }
 
   if (readPhase >= 0 && readPhase < NUM_PHASES) {
     if (!phaseGoalsMet(readPhase)) {
       if (millis() - lastScanCmd > timeoutForPhase(readPhase)) {
         phaseRetries++;
         Serial.printf(
           "[BLE] Timeout %s (%d/%d)\n",
           hrvQueryHistory ? "HRV-0x56" : PHASE_LABELS[readPhase],
           phaseRetries,
           MAX_PHASE_RETRIES
         );
 
         if (phaseRetries >= MAX_PHASE_RETRIES) {
           if (phaseMinimumMet(readPhase)) {
             advancePhase();
           } else {
             skipCurrentPhase();
           }
         } else if (hrvQueryHistory) {
           sendCommand(0x56, 0x00);
           lastScanCmd = millis();
         } else {
           sendCommand(0x28, PHASE_CMD_MODES[readPhase], 0x01);
           lastScanCmd = millis();
         }
       }
       delay(100);
       return;
     }
 
     advancePhase();
     return;
   }
 
   Serial.println("[APP] Ciclo completo. Enviando...");
   stopAllHealthModes();
   delay(100);
   bleStop();
 
   int sent = 0;
   int okCount = 0;
 
   if (captured13) {
     sent++;
     if (postPacket("0x13", rawHex13)) okCount++;
   }
 
   if (captured56) {
     sent++;
     if (postPacket("0x56", rawHex56)) okCount++;
   }
 
  bool hasHealthSlot = captured28[SLOT_CORACAO] || captured28[SLOT_SPO2] || captured28[SLOT_HRV];
  if (hasHealthSlot) {
    uint8_t mergedPkt[16];
    VitalsMerge merged;
    char mergedHex[50] = {0};

    if (buildMergedPacket(mergedPkt, &merged)) {
      storeHex(mergedHex, sizeof(mergedHex), mergedPkt, 16);
      Serial.printf("[APP] Pacote mesclado: %s\n", mergedHex);
      sent++;
      if (postMergedHealthPacket(mergedHex, &merged)) okCount++;
    } else {
      Serial.println("[APP] Falha ao montar pacote mesclado.");
    }
  } else {
    Serial.println("[APP] Nenhum slot de saúde capturado.");
  }
 
   Serial.printf("[APP] %d/%d enviados.\n", okCount, sent);
   delay(okCount == sent ? 30000 : 15000);
   ESP.restart();
 }
 