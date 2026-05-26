/**
 * 2208A + ESP32 — envia pacotes BLE brutos para Bracelet API.
 * Protocolo: medição ativa 0x28 + tempo real 0x09.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>

const char* WIFI_SSID = "SEU_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA";

const char* API_HOST = "bracelet-api-miqueias.fly.dev";
const uint16_t API_PORT = 443;
const char* DEVICE_MAC = "E6:64:0D:30:D3:F9";

BLEClient* client;
BLERemoteCharacteristic* txChar;

volatile bool pendingHttp = false;
char pendingPayload[320];
uint8_t pendingPacketType = 0;

unsigned long lastHttpMs = 0;
const unsigned long INTERVALO_HTTP_09 = 3000;  // tempo real: no máx. a cada 3s
const unsigned long INTERVALO_HTTP_28 = 8000;  // medição ativa: no máx. a cada 8s

// Modos 0x28 — byte AA na resposta
const uint8_t MODE_HRV = 0x01;
const uint8_t MODE_HEART = 0x02;
const uint8_t MODE_OXYGEN = 0x03;
const uint8_t MODE_TEMP = 0x04;

uint8_t calcCrc(uint8_t* packet, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len - 1; i++) sum += packet[i];
  return sum & 0xFF;
}

void sendPacket(uint8_t* packet, size_t len) {
  if (len < 2) return;
  packet[len - 1] = calcCrc(packet, len);
  if (txChar) {
    txChar->writeValue(packet, len, true);
  }
}

/** Comando genérico 16 bytes (0x13, 0x22, 0x28, etc.). */
void sendCommand16(uint8_t command, uint8_t p1 = 0, uint8_t p2 = 0) {
  uint8_t packet[16] = {command, p1, p2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  sendPacket(packet, 16);
  Serial.printf("[BLE] TX 16B: %02X %02X %02X ...\n", command, p1, p2);
}

/** Liga medição ativa 0x28 — retorna ~1/s na notificação. */
void startActiveMeasurement(uint8_t mode) {
  sendCommand16(0x28, mode, 0x01);
  Serial.printf("[BLE] Medicao ativa ON modo 0x%02X\n", mode);
}

/** Desliga medição ativa 0x28. */
void stopActiveMeasurement(uint8_t mode) {
  sendCommand16(0x28, mode, 0x00);
  Serial.printf("[BLE] Medicao ativa OFF modo 0x%02X\n", mode);
}

/** Liga tempo real 0x09 — passos, calorias, HR, SpO2, temperatura automáticos. */
void enableRealtimeStream() {
  uint8_t packet[16] = {0x09, 0x01, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  sendPacket(packet, 16);
  Serial.println("[BLE] Tempo real 0x09 ON");
}

bool postToApiDirect(const char* payload) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(30);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(60000);

  if (!http.begin(secureClient, API_HOST, API_PORT, "/bracelets/packets", true)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST((uint8_t*)payload, strlen(payload));

  if (httpCode == 200) {
    Serial.println("[HTTP] POST OK");
  } else if (httpCode > 0) {
    Serial.printf("[HTTP] Status %d\n", httpCode);
  } else {
    Serial.printf("[HTTP] Erro %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
  }

  http.end();
  return httpCode == 200;
}

bool connectToBracelet();

bool postToApiWithBleOff(const char* payload) {
  BLEDevice::deinit(true);
  delay(1200);
  WiFi.setSleep(false);

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  }

  bool ok = postToApiDirect(payload);
  delay(400);
  connectToBracelet();
  return ok;
}

void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (WiFi.status() != WL_CONNECTED || len == 0 || pendingHttp) return;

  uint8_t pktType = data[0];
  unsigned long interval = (pktType == 0x09) ? INTERVALO_HTTP_09 : INTERVALO_HTTP_28;

  if (millis() - lastHttpMs < interval) return;
  lastHttpMs = millis();

  char rawHex[96] = {0};
  size_t maxBytes = len < 30 ? len : 30;
  for (size_t i = 0; i < maxBytes; i++) {
    sprintf(rawHex + (i * 3), "%02X ", data[i]);
  }
  if (maxBytes > 0) rawHex[(maxBytes * 3) - 1] = '\0';

  snprintf(pendingPayload, sizeof(pendingPayload),
           "{\"deviceMac\":\"%s\",\"packetType\":\"0x%02X\",\"rawHex\":\"%s\",\"source\":\"ESP32\"}",
           DEVICE_MAC, pktType, rawHex);

  Serial.printf("\n[BLE] RX 0x%02X (%u bytes) -> fila HTTP\n", pktType, (unsigned)len);
  pendingHttp = true;
}

bool connectToBracelet() {
  BLEDevice::init("");
  client = BLEDevice::createClient();

  if (!client->connect(BLEAddress(DEVICE_MAC), BLE_ADDR_TYPE_RANDOM)) {
    return false;
  }

  BLERemoteService* service = client->getService(BLEUUID((uint16_t)0xFFF0));
  if (!service) return false;

  txChar = service->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  BLERemoteCharacteristic* rxChar = service->getCharacteristic(BLEUUID((uint16_t)0xFFF7));
  if (!txChar || !rxChar) return false;

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
  }

  Serial.println("[BLE] Conectado.");
  return true;
}

void runActiveMeasurementCycle() {
  const uint8_t modes[] = {MODE_HEART, MODE_OXYGEN, MODE_HRV, MODE_TEMP};
  const char* names[] = {"Coracao", "Oxigenio", "HRV", "Temperatura"};

  for (int i = 0; i < 4; i++) {
    Serial.printf("[APP] Medindo %s (15s)...\n", names[i]);
    startActiveMeasurement(modes[i]);
    delay(15000);
    stopActiveMeasurement(modes[i]);
    delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Serial.println(WiFi.localIP());
  WiFi.setSleep(false);

  if (!connectToBracelet()) {
    Serial.println("[BLE] Falha conexao");
    return;
  }

  delay(500);
  sendCommand16(0x13, 0x99);
  delay(1500);
  sendCommand16(0x22);
  delay(1500);

  enableRealtimeStream();
  delay(1000);

  runActiveMeasurementCycle();

  enableRealtimeStream();
  Serial.println("[APP] Aguardando notificacoes (0x09 + 0x28)...");
}

void loop() {
  if (pendingHttp) {
    pendingHttp = false;
    postToApiWithBleOff(pendingPayload);
  }
  delay(50);
}
