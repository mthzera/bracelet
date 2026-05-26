#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>

const char* WIFI_SSID = "iPhone de Matheus";
const char* WIFI_PASSWORD = "Umdois34!";

// URL de Produção do Render (HTTPS)
const char* API_URL = "https://bracelet-pn7r.onrender.com/bracelets/packets";

BLEClient* client;
BLERemoteCharacteristic* txChar;
BLERemoteCharacteristic* rxChar;

// Temporizadores de ciclo
unsigned long ultimaBateriaRequest = 0;
const unsigned long INTERVALO_BATERIA = 300000; // 5 minutos

bool medirSaturacaoAgora = false;

unsigned long ultimoEnvioHttp = 0;
const unsigned long INTERVALO_ENVIO_HTTP = 15000; // Envia para o Render a cada 15 segundos
const char* HEALTH_URL = "https://bracelet-pn7r.onrender.com/health";

enum ScanStage { SCAN_HEART, SCAN_SPO2, SCAN_REALTIME, SCAN_DONE };
ScanStage currentStage = SCAN_HEART;
bool heartCaptured = false;
bool spo2Captured = false;
bool realtimeCaptured = false;
bool cacheReadyToSend = false;
bool sendingCache = false;
int consecutiveFailures = 0;
unsigned long failureCooldownUntil = 0;
unsigned long stageCommandTimer = 0;
bool stageCommandIssued = false;
WiFiClientSecure secureClient;
bool secureClientConfigured = false;
const int MAX_SEND_RETRIES = 3;
const unsigned long RETRY_DELAY_MS = 2000;
const unsigned long FAILURE_COOLDOWN_STEP_MS = 10000;
const unsigned long FAILURE_COOLDOWN_MAX_MS = 60000;

void recordFailure() {
  consecutiveFailures++;
  unsigned long cooldown = consecutiveFailures * FAILURE_COOLDOWN_STEP_MS;
  if (cooldown > FAILURE_COOLDOWN_MAX_MS) {
    cooldown = FAILURE_COOLDOWN_MAX_MS;
  }
  failureCooldownUntil = millis() + cooldown;
}

bool ensureRenderAvailable() {
  if (!secureClientConfigured) {
    secureClient.setInsecure();
    secureClient.setHandshakeTimeout(60);
    secureClientConfigured = true;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(15000);

  if (!http.begin(secureClient, HEALTH_URL)) {
    Serial.println("[HTTP] Falha ao iniciar health check");
    return false;
  }

  int httpCode = http.GET();
  bool healthy = httpCode > 0 && httpCode == 200;
  Serial.printf(
      "[HTTP] Health check %s (%d)\n",
      healthy ? "OK" : "falhou",
      httpCode);
  if (!healthy) {
    Serial.printf("[HTTP] Health error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  return healthy;
}

// Estrutura de Cache Global Estável para Produção
struct MetricsCache {
  uint8_t packetType = 0;
  char rawHex[64] = "00";
  uint8_t heartRate = 0;
  uint8_t spO2 = 0;
  uint8_t hrv = 0;
  uint8_t fatigue = 0;
  char bloodPressure[16] = "0/0";
  float temperature = 0.0;
  uint32_t steps = 0;
  float calories = 0.0;
  float distance = 0.0;
} cache;

void resetScanState() {
  currentStage = SCAN_HEART;
  heartCaptured = false;
  spo2Captured = false;
  realtimeCaptured = false;
  cacheReadyToSend = false;
  stageCommandIssued = false;
  stageCommandTimer = 0;
  medirSaturacaoAgora = false;
  cache.packetType = 0;
  cache.rawHex[0] = '\0';
}

uint8_t calcCrc(uint8_t* packet) {
  uint16_t sum = 0;
  for (int i = 0; i < 15; i++) sum += packet[i];
  return sum & 0xFF;
}

void sendCommand(uint8_t command, uint8_t param1 = 0, uint8_t param2 = 0) {
  uint8_t packet[16] = {command, param1, param2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  packet[15] = calcCrc(packet);

  if (txChar) {
    txChar->writeValue(packet, 16, true);
    Serial.printf("[BLE] Comando 0x%02X enviado.\n", command);
  }
}

void gerenciarAlternanciaSensores() {
  if (currentStage == SCAN_DONE) {
    return;
  }

  unsigned long now = millis();
  switch (currentStage) {
    case SCAN_HEART:
      if (!stageCommandIssued || now - stageCommandTimer > 7000) {
        if (txChar) {
          Serial.println("\n[APP] [SCAN] -> Captura de batimentos (BPM)");
          sendCommand(0x28, 0x03, 0x00);
          delay(50);
          sendCommand(0x28, 0x02, 0x01);
        }
        stageCommandIssued = true;
        stageCommandTimer = now;
      }
      if (heartCaptured) {
        currentStage = SCAN_SPO2;
        stageCommandIssued = false;
        stageCommandTimer = 0;
        medirSaturacaoAgora = true;
      }
      break;
    case SCAN_SPO2:
      if (!stageCommandIssued || now - stageCommandTimer > 7000) {
        if (txChar) {
          Serial.println("\n[APP] [SCAN] -> Captura de saturação (SpO2)");
          sendCommand(0x28, 0x02, 0x00);
          delay(50);
          sendCommand(0x28, 0x03, 0x01);
        }
        stageCommandIssued = true;
        stageCommandTimer = now;
      }
      if (spo2Captured) {
        currentStage = SCAN_REALTIME;
        stageCommandIssued = false;
        stageCommandTimer = 0;
        medirSaturacaoAgora = false;
      }
      break;
    case SCAN_REALTIME:
      if (!stageCommandIssued || now - stageCommandTimer > 7000) {
        if (txChar) {
          Serial.println("\n[APP] [SCAN] -> Captura real-time (passos/metrics)");
          sendCommand(0x09, 0x01, 0x01);
        }
        stageCommandIssued = true;
        stageCommandTimer = now;
      }
      if (realtimeCaptured) {
        currentStage = SCAN_DONE;
        cacheReadyToSend = true;
        medirSaturacaoAgora = false;
      }
      break;
    default:
      break;
  }
}

void notifyCallback(BLERemoteCharacteristic* charac, uint8_t* data, size_t len, bool isNotify) {
  if (len == 0) return;

  cache.packetType = data[0];

  char tempHex[64] = {0};
  for (size_t i = 0; i < len && i < 16; i++) {
    sprintf(tempHex + (i * 3), "%02X ", data[i]);
  }
  if (len > 0) tempHex[(len * 3) - 1] = '\0';
  strcpy(cache.rawHex, tempHex);

  if (cache.packetType == 0x28) {
    if (data[1] == 0x03 && data[3] > 0) {
      cache.spO2 = data[3]; // Atualiza a Saturação do Sangue real quando ela for lida
      spo2Captured = true;
    } else if (data[1] == 0x02) {
      cache.heartRate = data[2]; // Atualiza os batimentos
      heartCaptured = true;
    }
    cache.hrv = data[4];
    cache.fatigue = data[5];
    snprintf(cache.bloodPressure, sizeof(cache.bloodPressure), "%d/%d", data[6], data[7]);
    cache.temperature = ((data[8] << 8) | data[9]) / 10.0;
  } else if (cache.packetType == 0x09) {
    cache.steps = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
    cache.calories = (data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24)) / 100.0;
    cache.distance = (data[9] | (data[10] << 8) | (data[11] << 16) | (data[12] << 24)) / 100.0;
    if (data[21] > 0) cache.heartRate = data[21];
    if (data[24] > 0) cache.spO2 = data[24];
    realtimeCaptured = true;
  }
}

bool sendPayloadToRender(const char* payloadFinal) {
  if (!secureClientConfigured) {
    secureClient.setInsecure();
    secureClient.setHandshakeTimeout(60);
    secureClientConfigured = true;
  }

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(30000);

  if (!http.begin(secureClient, API_URL)) {
    Serial.println("[HTTP] Falha ao iniciar conexão HTTP");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  size_t payloadLength = strlen(payloadFinal);
  for (int attempt = 1; attempt <= MAX_SEND_RETRIES; ++attempt) {
    int httpCode = http.POST((uint8_t*)payloadFinal, payloadLength);
    if (httpCode > 0) {
      Serial.printf("[HTTP] Sucesso Produção! Status Render: %d\n", httpCode);
      Serial.println(http.getString());
      http.end();
      return true;
    }

    Serial.printf(
        "[HTTP] Erro de barramento: %d (%s) tentativa %d\n",
        httpCode,
        http.errorToString(httpCode).c_str(),
        attempt);
    delay(RETRY_DELAY_MS * attempt);
  }

  http.end();
  return false;
}

void transmitirCacheParaAPI() {
  if (!cacheReadyToSend || sendingCache || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (failureCooldownUntil > millis()) {
    return;
  }

  if (millis() - ultimoEnvioHttp < INTERVALO_ENVIO_HTTP && consecutiveFailures == 0) {
    return;
  }

  if (!ensureRenderAvailable()) {
    Serial.println("[HTTP] Health check falhou. Cancelando envio.");
    recordFailure();
    return;
  }

  ultimoEnvioHttp = millis();
  sendingCache = true;

  if (cache.packetType == 0) {
    sendingCache = false;
    return;
  }

  Serial.println("\n[HTTP] Preparando rádio para envio HTTPS seguro...");
  if (txChar) {
    sendCommand(0x28, 0x02, 0x00);
    sendCommand(0x28, 0x03, 0x00);
  }
  if (rxChar) rxChar->registerForNotify(NULL);
  delay(300);

  char metricsJson[256];
  const char* modoSolicitado = medirSaturacaoAgora ? "Saturacao (SpO2)" : "Coracao (BPM)";

  if (cache.packetType == 0x28) {
    snprintf(metricsJson, sizeof(metricsJson),
             "{\"mode\":\"on_demand\",\"type\":\"0x%02X\",\"modo_solicitado\":\"%s\",\"heartRate\":%d,\"spO2\":%d,\"hrv\":%d,\"fatigue\":%d,\"bloodPressure\":\"%s\",\"temperature\":%.1f}",
             cache.packetType, modoSolicitado, cache.heartRate, cache.spO2, cache.hrv, cache.fatigue, cache.bloodPressure, cache.temperature);
  } else if (cache.packetType == 0x09) {
    snprintf(metricsJson, sizeof(metricsJson),
             "{\"mode\":\"real_time\",\"steps\":%u,\"calories\":%.2f,\"distance\":%.2f,\"heartRate\":%d,\"temperature\":%.1f,\"spO2\":%d}",
             cache.steps, cache.calories, cache.distance, cache.heartRate, cache.temperature, cache.spO2);
  } else {
    snprintf(metricsJson, sizeof(metricsJson), "{\"mode\":\"status\",\"packet\":\"0x%02X\"}", cache.packetType);
  }

  size_t rawHexLen = strlen(cache.rawHex);
  if (rawHexLen > 0 && cache.rawHex[rawHexLen - 1] == ' ') {
    cache.rawHex[rawHexLen - 1] = '\0';
  }

  char payloadFinal[512];
  snprintf(payloadFinal, sizeof(payloadFinal),
           "{\"deviceMac\":\"E6:64:0D:30:D3:F9\",\"packetType\":\"0x%02X\",\"rawHex\":\"%s\",\"source\":\"ESP32\",\"metrics\":%s}",
           cache.packetType, cache.rawHex, metricsJson);

  Serial.println("[HTTP] Despachando payload estável para o Render...");
  Serial.println(payloadFinal);

  bool success = sendPayloadToRender(payloadFinal);

  if (success) {
    consecutiveFailures = 0;
    failureCooldownUntil = 0;
    resetScanState();
  } else {
    recordFailure();
  }

  Serial.println("[BLE] Restaurando monitoramento de saúde...");
  delay(100);
  if (rxChar) rxChar->registerForNotify(notifyCallback);

  delay(200);
  if (txChar) {
    if (medirSaturacaoAgora) {
      sendCommand(0x28, 0x03, 0x01);
    } else {
      sendCommand(0x28, 0x02, 0x01);
    }
    delay(200);
    sendCommand(0x09, 0x01, 0x01);
  }

  sendingCache = false;
}

bool connectToBracelet() {
  Serial.println("[BLE] Inicializando Hardware...");
  client = BLEDevice::createClient();

  if (!client->connect(BLEAddress("e6:64:0d:30:d3:f9"), BLE_ADDR_TYPE_RANDOM)) {
    return false;
  }

  BLERemoteService* service = client->getService(BLEUUID((uint16_t)0xFFF0));
  if (!service) return false;

  txChar = service->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  rxChar = service->getCharacteristic(BLEUUID((uint16_t)0xFFF7));

  if (!txChar || !rxChar) return false;

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] Canal de Notificações conectado!");
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n=================================");
  Serial.println("     ESP32 PRODUÇÃO COMPLETO     ");
  Serial.println("=================================");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\n[WIFI] Conectado! IP: ");
  Serial.println(WiFi.localIP());

  BLEDevice::init("");

  resetScanState();

  if (connectToBracelet()) {
    delay(1000);
    sendCommand(0x13, 0x99); // Battery
    delay(1000);
    sendCommand(0x09, 0x01, 0x01); // Real-time
    delay(1000);
    
    ultimoEnvioHttp = millis();
    sendCommand(0x28, 0x02, 0x01); // Inicia coletando batimentos
    Serial.println("[APP] Varredura iniciada.");
  } else {
    Serial.println("[APP] Falha ao parear com a pulseira.");
  }
}

void loop() {
  // 1. Garante o chaveamento de sensores em segundo plano
  gerenciarAlternanciaSensores();

  // 2. Transmite o cache estável para o Render abrindo uma janela limpa de rádio
  transmitirCacheParaAPI();

  // 3. Busca nível de bateria da pulseira de forma passiva (5 minutos)
  if (millis() - ultimaBateriaRequest > INTERVALO_BATERIA) {
    ultimaBateriaRequest = millis();
    if (txChar) { 
      sendCommand(0x13, 0x99);
    }
  }

  delay(10);
}