/**
 * 2208A + ESP32 — protocolo PDF "2208A API V1" §33
 * v3 — foco em 4 sinais vitais: BPM, SpO2, Temperatura, Pressão (opcional)
 *
 * Fases:
 *   Fase 0 → 0x03 SpO2  (captura BPM + SpO2)
 *   Fase 1 → 0x04 Temp  (captura temperatura)
 *   Fase 2 → 0x02 BP    (captura pressão — opcional, não bloqueia envio)
 *
 * Melhorias:
 *  - ensureWifi(): reconecta se WiFi cair durante ciclo BLE
 *  - ensureBraceletConnected(): reconecta BLE com retry e timeout (não trava)
 *  - POST com 3 tentativas (Render wake-up)
 *  - wakeUpServer() + ping /health no intervalo entre medições
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
 
 // ── Configurações ────────────────────────────────────────────────────────────
 const char* WIFI_SSID = "IoTs";
 const char* WIFI_PASS = "AneryIot158";
 const char* API_URL = "https://bracelet-pn7r.onrender.com/bracelets/packets";
 const char* API_BASE = "https://bracelet-pn7r.onrender.com";
 const char* DEVICE_MAC = "e6:64:0d:30:d3:f9";
 
 // ── Intervalos ───────────────────────────────────────────────────────────────
 const unsigned long MEASURE_INTERVAL_MS = 1UL * 60UL * 1000UL;  // 5 min
 const unsigned long PING_INTERVAL_MS = 30UL * 1000UL;
 
 // ── Timeouts BLE ─────────────────────────────────────────────────────────────
 const unsigned long SPO2_WARMUP_MS = 15000;
 const unsigned long TEMP_WARMUP_MS = 5000;
 const unsigned long BP_WARMUP_MS = 15000;
 const unsigned long SPO2_TIMEOUT_MS = 50000;
 const unsigned long TEMP_TIMEOUT_MS = 30000;
 const unsigned long BP_TIMEOUT_MS = 90000;
const unsigned long BATTERY_TIMEOUT_MS = 6000;
const int MAX_PHASE_RETRIES = 3;
const int BLE_CONNECT_MAX_ATTEMPTS = 6;
const uint32_t BLE_CONNECT_TIMEOUT_MS = 15000;
const unsigned long BLE_RETRY_DELAY_MS = 10000;
 
 // ── Fases ────────────────────────────────────────────────────────────────────
 const uint8_t PHASE_CMD[] = { 0x03, 0x04, 0x02 };
 const char* PHASE_LABELS[] = { "SpO2+BPM", "Temperatura", "Pressao" };
 const int NUM_PHASES = 3;
 
 // ── Slots de captura ─────────────────────────────────────────────────────────
 const int SLOT_SPO2 = 0;
 const int SLOT_TEMP = 1;
 const int SLOT_BP = 2;
 const int NUM_SLOTS = 3;
 
 char rawHex[NUM_SLOTS][50] = { { 0 } };
 bool captured[NUM_SLOTS] = { false };
 char rawHex13[24] = "";
 bool captured13 = false;
 bool capturedBP = false;
 
 int readPhase = -1;
 int phaseRetries = 0;
 unsigned long lastScanCmd = 0;
 
 struct Vitals {
   uint8_t bpm;
   uint8_t spo2;
   uint8_t sys;
   uint8_t dia;
   uint16_t tempRaw;
 };
 
BLEClient* bleClient = nullptr;
BLERemoteCharacteristic* txChar = nullptr;
BLERemoteCharacteristic* rxChar = nullptr;
volatile bool bleLinkUp = false;
bool bleStackInitialized = false;
unsigned long lastBleReconnectAttempt = 0;

class BraceletClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override {
    bleLinkUp = true;
    Serial.println("[BLE] Callback: conectado.");
  }
  void onDisconnect(BLEClient*) override {
    bleLinkUp = false;
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
 
 // ════════════════════════════════════════════════════════════════════════════
 //  WIFI
 // ════════════════════════════════════════════════════════════════════════════
 
 bool ensureWifi() {
   if (WiFi.status() == WL_CONNECTED) return true;
   Serial.printf("[WIFI] Desconectado (status=%d). Reconectando...\n", WiFi.status());
   WiFi.disconnect();
   delay(200);
   WiFi.begin(WIFI_SSID, WIFI_PASS);
   for (int i = 0; i < 20; i++) {
     if (WiFi.status() == WL_CONNECTED) {
       Serial.printf("[WIFI] Reconectado. IP=%s\n", WiFi.localIP().toString().c_str());
       return true;
     }
     delay(500);
     Serial.print(".");
   }
   Serial.println("[WIFI] Falha ao reconectar.");
   return false;
 }
 
 // ════════════════════════════════════════════════════════════════════════════
 //  BLE
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
 
 void sendSetTime(uint8_t yy, uint8_t mo, uint8_t dd, uint8_t hh, uint8_t mm, uint8_t ss) {
   uint8_t pkt[16] = { 0x01, yy, mo, dd, hh, mm, ss, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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
 
 bool isPressureReady(uint8_t* data, size_t len) {
   if (len < 8) return false;
   uint8_t sys = data[6], dia = data[7];
   if (sys == 0 || dia == 0) return false;
   if (sys == 0x7B && dia == 0x49) return false;
   if (sys == 0x75 && dia == 0x44) return false;
   return sys < 250 && dia < 200 && sys > dia;
 }
 
 void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
   if (!bleLinkUp || len < 2) return;
 
   if (data[0] == 0x13) {
     if (captured13 || readPhase != -1) return;
     storeHex(rawHex13, sizeof(rawHex13), data, len);
     captured13 = true;
     Serial.printf("[BLE] Bateria: %u%%\n", data[1]);
     return;
   }
 
   if (data[0] != 0x28 || len < 10) return;
   if (readPhase < 0 || readPhase >= NUM_PHASES) return;
 
   if (readPhase == 0 && data[1] == 0x03) {
     if (captured[SLOT_SPO2]) return;
     if (data[2] == 0 || data[3] == 0) return;
     storeHex(rawHex[SLOT_SPO2], sizeof(rawHex[SLOT_SPO2]), data, len);
     captured[SLOT_SPO2] = true;
     Serial.printf("[BLE] SpO2 OK — BPM=%u SpO2=%u\n", data[2], data[3]);
     return;
   }
 
   if (readPhase == 1 && data[1] == 0x04) {
     if (captured[SLOT_TEMP]) return;
     uint16_t tempRaw = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
     if (tempRaw == 0) return;
     storeHex(rawHex[SLOT_TEMP], sizeof(rawHex[SLOT_TEMP]), data, len);
     captured[SLOT_TEMP] = true;
     Serial.printf("[BLE] Temp OK — %.1f°C\n", tempRaw / 10.0f);
     return;
   }
 
   if (readPhase == 2 && data[1] == 0x02) {
     if (!captured[SLOT_BP] && data[2] > 0 && !isPressureReady(data, len)) return;
     if (!isPressureReady(data, len)) return;
     storeHex(rawHex[SLOT_BP], sizeof(rawHex[SLOT_BP]), data, len);
     captured[SLOT_BP] = true;
     capturedBP = true;
     Serial.printf("[BLE] Pressão OK — %u/%u mmHg\n", data[6], data[7]);
     return;
   }
 }
 
void resetMeasurementState() {
  captured13 = false;
  capturedBP = false;
  readPhase = -1;
  phaseRetries = 0;
  memset(captured, 0, sizeof(captured));
  memset(rawHex, 0, sizeof(rawHex));
  rawHex13[0] = '\0';
}

bool bindBraceletCharacteristics() {
  if (!bleClient || !bleClient->isConnected()) return false;

  BLERemoteService* svc = bleClient->getService(BLEUUID((uint16_t)0xFFF0));
  if (!svc) return false;

  txChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  rxChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF7));
  if (!txChar || !rxChar) return false;

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
  }
  bleLinkUp = true;
  return true;
}

void releaseBleStack() {
  bleLinkUp = false;
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
  Serial.printf("[BLE] Conectando %s (timeout %ums)...\n", mac.c_str(), BLE_CONNECT_TIMEOUT_MS);

  if (!bleClient->connect(BLEAddress(mac.c_str()), BLE_ADDR_TYPE_RANDOM, BLE_CONNECT_TIMEOUT_MS)) {
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
    if (attempt < BLE_CONNECT_MAX_ATTEMPTS) delay(BLE_RETRY_DELAY_MS);
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
  if (!connectToBracelet()) {
    Serial.printf("[BLE] Reconexão falhou. Próxima tentativa em %lus.\n",
                  BLE_RETRY_DELAY_MS / 1000);
    return false;
  }

  resetMeasurementState();
  braceletInitSequence();
  return true;
}

void bleStop() {
  releaseBleStack();
}
 
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
   delay(300);
 
   sendCommand(0x28, PHASE_CMD[phase], 0x01);
   Serial.printf("[BLE] Fase %d — %s\n", phase, PHASE_LABELS[phase]);
 
   if (phase == 0) delay(SPO2_WARMUP_MS);
   else if (phase == 1) delay(TEMP_WARMUP_MS);
   else if (phase == 2) delay(BP_WARMUP_MS);
 
   lastScanCmd = millis();
 }
 
 void advancePhase() {
   sendCommand(0x28, PHASE_CMD[readPhase], 0x00);
   delay(120);
   readPhase++;
   phaseRetries = 0;
   if (readPhase < NUM_PHASES) startPhase(readPhase);
 }
 
 void syncDeviceTime() {
   configTime(0, 0, "pool.ntp.org", "time.nist.gov");
   struct tm ti;
   if (!getLocalTime(&ti, 8000)) return;
   sendSetTime(toBcd(ti.tm_year % 100), toBcd(ti.tm_mon + 1), toBcd(ti.tm_mday),
               toBcd(ti.tm_hour), toBcd(ti.tm_min), toBcd(ti.tm_sec));
   delay(400);
 }
 
void braceletInitSequence() {
  if (!isBraceletConnected()) return;

  stopAllHealthModes();
   delay(300);
 
   sendCommand(0x13, 0x99);
   lastScanCmd = millis();
   sendCommand(0x22);
   delay(600);
 
   syncDeviceTime();
 }
 
 // ════════════════════════════════════════════════════════════════════════════
 //  HTTP
 // ════════════════════════════════════════════════════════════════════════════
 
 void wakeUpServer() {
   if (!ensureWifi()) return;
   WiFiClientSecure sc;
   sc.setInsecure();
   sc.setHandshakeTimeout(90);
   HTTPClient http;
   http.setReuse(false);
   http.setTimeout(60000);
   String url = String(API_BASE) + "/health";
   if (http.begin(sc, url)) {
     http.GET();
     http.end();
   }
   delay(2000);
 }
 
 bool doPost(const char* payload) {
   if (!ensureWifi()) return false;
 
   WiFiClientSecure sc;
   sc.setInsecure();
   sc.setHandshakeTimeout(90);
   HTTPClient http;
   http.setReuse(false);
   http.setTimeout(60000);
 
   if (!http.begin(sc, API_URL)) return false;
   http.addHeader("Content-Type", "application/json");
   int code = http.POST((uint8_t*)payload, strlen(payload));
   bool ok = (code == 200 || code == 201);
   Serial.printf("[HTTP] %s (HTTP %d)\n", ok ? "OK" : "ERRO", code);
   http.end();
   return ok;
 }
 
 bool postPacket(const char* packetType, const char* hex, int retries = 3) {
   char payload[220];
   snprintf(payload, sizeof(payload),
            "{\"deviceMac\":\"%s\",\"packetType\":\"%s\",\"rawHex\":\"%s\",\"source\":\"ESP32\"}",
            DEVICE_MAC, packetType, hex);
 
   for (int i = 0; i < retries; i++) {
     if (doPost(payload)) return true;
     if (i < retries - 1) delay(5000);
   }
   Serial.printf("[HTTP] POST %s falhou (%d tentativas)\n", packetType, retries);
   return false;
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
 
   return postPacket("0x13", fixedHex);
 }
 
 bool postVitals() {
   uint8_t pktSpo2[16] = { 0 }, pktTemp[16] = { 0 }, pktBp[16] = { 0 };
   bool hasSpo2 = captured[SLOT_SPO2] && parseHex16(rawHex[SLOT_SPO2], pktSpo2);
   bool hasTemp = captured[SLOT_TEMP] && parseHex16(rawHex[SLOT_TEMP], pktTemp);
   bool hasBp2 = captured[SLOT_BP] && parseHex16(rawHex[SLOT_BP], pktBp);
 
   uint8_t bpm = hasSpo2 ? pktSpo2[2] : 0;
   uint8_t spo2 = hasSpo2 ? pktSpo2[3] : 0;
   uint16_t tempRaw = hasTemp
     ? (uint16_t)pktTemp[8] | ((uint16_t)pktTemp[9] << 8)
     : 0;
 
   if (!hasSpo2 || !hasTemp || bpm == 0 || spo2 == 0 || tempRaw == 0) {
     Serial.printf(
       "[APP] Obrigatórios incompletos — BPM=%u SpO2=%u Temp=%.1f°C. Abortando POST.\n",
       bpm, spo2, tempRaw / 10.0f
     );
     return false;
   }
 
   uint8_t sys = hasBp2 ? pktBp[6] : 0;
   uint8_t dia = hasBp2 ? pktBp[7] : 0;
 
   uint8_t merged[16] = { 0 };
   merged[0] = 0x28;
   merged[1] = 0x02;
   merged[2] = bpm;
   merged[3] = spo2;
   merged[6] = sys;
   merged[7] = dia;
   merged[8] = tempRaw & 0xFF;
   merged[9] = (tempRaw >> 8) & 0xFF;
   merged[15] = calcCrc(merged);
 
   char mergedHex[50] = { 0 };
   storeHex(mergedHex, sizeof(mergedHex), merged, 16);
 
   Serial.printf("[APP] Enviando — BPM=%u SpO2=%u Temp=%.1f°C PA=%s\n",
                 bpm, spo2, tempRaw / 10.0f,
                 hasBp2 ? (String(sys) + "/" + String(dia) + " mmHg").c_str() : "não medida");
 
   char payload[512];
   if (hasBp2) {
     snprintf(payload, sizeof(payload),
              "{\"deviceMac\":\"%s\",\"packetType\":\"0x28\",\"rawHex\":\"%s\",\"source\":\"ESP32\","
              "\"metrics\":{\"mode\":\"merged\",\"type\":\"0x28\",\"modo_solicitado\":\"vitals\","
              "\"heartRate\":%u,\"spO2\":%u,\"temperature\":%.1f,"
              "\"bloodPressure\":\"%u/%u\"}}",
              DEVICE_MAC, mergedHex, bpm, spo2, tempRaw / 10.0f, sys, dia);
   } else {
     snprintf(payload, sizeof(payload),
              "{\"deviceMac\":\"%s\",\"packetType\":\"0x28\",\"rawHex\":\"%s\",\"source\":\"ESP32\","
              "\"metrics\":{\"mode\":\"merged\",\"type\":\"0x28\",\"modo_solicitado\":\"vitals\","
              "\"heartRate\":%u,\"spO2\":%u,\"temperature\":%.1f}}",
              DEVICE_MAC, mergedHex, bpm, spo2, tempRaw / 10.0f);
   }
 
   for (int i = 0; i < 3; i++) {
     if (doPost(payload)) return true;
     if (i < 2) delay(5000);
   }
   Serial.println("[HTTP] POST vitals falhou (3 tentativas)");
   return false;
 }
 
 // ════════════════════════════════════════════════════════════════════════════
 //  SETUP & LOOP
 // ════════════════════════════════════════════════════════════════════════════
 
 void setup() {
   WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
 
   Serial.begin(115200);
   delay(1000);
 
   WiFi.begin(WIFI_SSID, WIFI_PASS);
   while (WiFi.status() != WL_CONNECTED) {
     delay(500);
     Serial.print(".");
   }
   Serial.printf("\n[WIFI] Conectado. IP=%s | RSSI=%d dBm\n",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
   WiFi.setSleep(false);
 
  resetMeasurementState();
  while (!connectToBracelet()) {
    Serial.printf("[BLE] Pulseira indisponível. Nova tentativa em %lus...\n",
                  BLE_RETRY_DELAY_MS / 1000);
    delay(BLE_RETRY_DELAY_MS);
  }
  braceletInitSequence();
}
 
void loop() {
  if (!ensureBraceletConnected()) {
    delay(500);
    return;
  }

  if (readPhase == -1) {
     if (!captured13 && millis() - lastScanCmd < BATTERY_TIMEOUT_MS) {
       delay(100);
       return;
     }
     if (!captured13) Serial.println("[BLE] Bateria não respondeu — seguindo.");
     readPhase = 0;
     startPhase(0);
     return;
   }
 
   if (readPhase >= 0 && readPhase < NUM_PHASES) {
     bool goalMet = phaseGoalsMet(readPhase);
 
     if (!goalMet) {
       if (millis() - lastScanCmd > timeoutForPhase(readPhase)) {
         phaseRetries++;
         Serial.printf("[BLE] Timeout %s (%d/%d)\n",
                       PHASE_LABELS[readPhase], phaseRetries, MAX_PHASE_RETRIES);
 
         if (phaseRetries >= MAX_PHASE_RETRIES) {
           if (canAdvanceFromPhase(readPhase)) {
            if (readPhase < 2 && !captured[readPhase]) {
              Serial.printf("[APP] Sinal obrigatório %s não capturado. Reconectando BLE...\n",
                            PHASE_LABELS[readPhase]);
              bleStop();
              delay(2000);
              lastBleReconnectAttempt = 0;
              if (!ensureBraceletConnected()) return;
              return;
            }
             advancePhase();
           } else {
             sendCommand(0x28, PHASE_CMD[readPhase], 0x01);
             lastScanCmd = millis();
             phaseRetries = 0;
           }
         } else {
           sendCommand(0x28, PHASE_CMD[readPhase], 0x01);
           lastScanCmd = millis();
         }
       }
       delay(100);
       return;
     }
 
     if (readPhase == 2) {
       readPhase = NUM_PHASES;
     } else {
       advancePhase();
     }
     return;
   }
 
   stopAllHealthModes();
   delay(100);
   bleStop();
 
   wakeUpServer();
 
   int sent = 0, okCount = 0;
 
   if (captured13) {
     sent++;
     if (postBatteryPacket()) okCount++;
   }
 
   sent++;
   if (postVitals()) okCount++;
 
   Serial.printf("[APP] Enviados %d/%d — próxima medição em %lus\n",
                 okCount, sent, MEASURE_INTERVAL_MS / 1000);
 
   unsigned long cycleStart = millis();
   while (millis() - cycleStart < MEASURE_INTERVAL_MS) {
     unsigned long remaining = MEASURE_INTERVAL_MS - (millis() - cycleStart);
 
     if (ensureWifi()) {
       WiFiClientSecure sc;
       sc.setInsecure();
       sc.setHandshakeTimeout(30);
       HTTPClient http;
       http.setReuse(false);
       http.setTimeout(15000);
       String url = String(API_BASE) + "/health";
       if (http.begin(sc, url)) {
         int code = http.GET();
         if (code != 200) Serial.printf("[PING] /health → %d\n", code);
         http.end();
       }
     }
 
     unsigned long sleepMs = min(PING_INTERVAL_MS, remaining);
     unsigned long sleepEnd = millis() + sleepMs;
     while (millis() < sleepEnd) delay(1000);
   }
 
   Serial.println("[APP] Reiniciando ciclo...");
   ESP.restart();
 }
 