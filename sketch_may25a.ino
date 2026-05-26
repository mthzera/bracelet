#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>

const char* WIFI_SSID  = "IoTs";
const char* WIFI_PASS  = "AneryIot158";
const char* API_URL    = "https://bracelet-pn7r.onrender.com/bracelets/packets";
const char* DEVICE_MAC = "E6:64:0D:30:D3:F9";

BLEClient*               bleClient = nullptr;
BLERemoteCharacteristic* txChar    = nullptr;
BLERemoteCharacteristic* rxChar    = nullptr;

// dois slots: [0] = coração (0x02), [1] = SpO2 (0x03)
char  rawHex[2][50] = {"", ""};
bool  captured[2]   = {false, false};
int   currentSlot   = 0; // qual scan está ativo agora

const unsigned long SCAN_TIMEOUT_MS = 12000;
unsigned long lastScanCmd = 0;

// ── CRC ───────────────────────────────────────────────────────────────────────

uint8_t calcCrc(uint8_t* p) {
  uint16_t s = 0;
  for (int i = 0; i < 15; i++) s += p[i];
  return s & 0xFF;
}

// ── BLE ───────────────────────────────────────────────────────────────────────

void sendCommand(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0) {
  if (!txChar) return;
  uint8_t pkt[16] = {cmd, p1, p2, 0,0,0,0,0,0,0,0,0,0,0,0,0};
  pkt[15] = calcCrc(pkt);
  txChar->writeValue(pkt, 16, true);
}

void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 10 || data[0] != 0x28) return;

  uint8_t mode = data[1];
  int slot = -1;
  if      (mode == 0x02) slot = 0; // coração
  else if (mode == 0x03 && data[3] > 0) slot = 1; // SpO2 (só aceita se tiver valor)
  if (slot < 0 || captured[slot]) return;

  char tmp[50] = {0};
  for (size_t i = 0; i < len && i < 16; i++) sprintf(tmp + i * 3, "%02X ", data[i]);
  size_t tl = strlen(tmp);
  if (tl > 0 && tmp[tl - 1] == ' ') tmp[tl - 1] = '\0';

  strncpy(rawHex[slot], tmp, sizeof(rawHex[slot]) - 1);
  captured[slot] = true;
  Serial.printf("[BLE] Slot %d capturado: %s\n", slot, rawHex[slot]);
}

bool connectToBracelet() {
  bleClient = BLEDevice::createClient();
  if (!bleClient->connect(BLEAddress("e6:64:0d:30:d3:f9"), BLE_ADDR_TYPE_RANDOM)) return false;
  BLERemoteService* svc = bleClient->getService(BLEUUID((uint16_t)0xFFF0));
  if (!svc) return false;
  txChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  rxChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF7));
  if (!txChar || !rxChar) return false;
  if (rxChar->canNotify()) rxChar->registerForNotify(notifyCallback);
  return true;
}

void bleStop() {
  txChar    = nullptr;
  rxChar    = nullptr;
  bleClient = nullptr;
  BLEDevice::deinit(true);
  delay(500);
  Serial.printf("[BLE] Desligado. Heap livre: %u\n", ESP.getFreeHeap());
}

// ── HTTP ──────────────────────────────────────────────────────────────────────

bool postPacket(const char* packetType, const char* hex) {
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"deviceMac\":\"%s\",\"packetType\":\"%s\","
    "\"rawHex\":\"%s\",\"source\":\"ESP32\"}",
    DEVICE_MAC, packetType, hex);

  Serial.printf("[HTTP] -> %s\n", payload);

  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(45);

  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(sc, API_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)payload, strlen(payload));
  bool ok = code > 0;
  if (ok) Serial.printf("[HTTP] OK %d\n", code);
  else    Serial.printf("[HTTP] FALHA %d (%s)\n", code, http.errorToString(code).c_str());
  http.end();
  return ok;
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\n[WIFI] %s\n", WiFi.localIP().toString().c_str());

  BLEDevice::init("");
  if (!connectToBracelet()) {
    Serial.println("[BLE] Falha. Reiniciando...");
    delay(3000); ESP.restart();
  }
  Serial.println("[BLE] OK");

  // inicia scan de coração primeiro
  currentSlot = 0;
  sendCommand(0x28, 0x03, 0x00); // garante SpO2 desligado
  delay(100);
  sendCommand(0x28, 0x02, 0x01); // liga coração
  lastScanCmd = millis();
}

void loop() {
  // fase 0: aguarda coração
  if (currentSlot == 0) {
    if (!captured[0]) {
      if (millis() - lastScanCmd > SCAN_TIMEOUT_MS) {
        Serial.println("[BLE] Timeout coração. Reenviando...");
        sendCommand(0x28, 0x02, 0x01);
        lastScanCmd = millis();
      }
      delay(100);
      return;
    }
    // coração capturado → passa para SpO2
    currentSlot = 1;
    sendCommand(0x28, 0x02, 0x00); // desliga coração
    delay(100);
    sendCommand(0x28, 0x03, 0x01); // liga SpO2
    lastScanCmd = millis();
    Serial.println("[BLE] Iniciando scan SpO2...");
  }

  // fase 1: aguarda SpO2
  if (currentSlot == 1) {
    if (!captured[1]) {
      if (millis() - lastScanCmd > SCAN_TIMEOUT_MS) {
        Serial.println("[BLE] Timeout SpO2. Reenviando...");
        sendCommand(0x28, 0x03, 0x01);
        lastScanCmd = millis();
      }
      delay(100);
      return;
    }
  }

  // ambos capturados → desliga BLE e envia
  Serial.println("[APP] Dados completos. Desligando BLE para envio...");
  bleStop();

  Serial.printf("[HEAP] livre antes do POST: %u\n", ESP.getFreeHeap());
  bool ok0 = postPacket("0x28", rawHex[0]); // coração
  bool ok1 = postPacket("0x28", rawHex[1]); // SpO2

  if (ok0 && ok1) {
    Serial.println("[APP] Tudo enviado. Reiniciando em 30s...");
    delay(30000);
  } else {
    Serial.println("[APP] Falha em algum envio. Reiniciando em 15s...");
    delay(15000);
  }
  ESP.restart();
}
