
 #include <WiFi.h>
 #include <WiFiClientSecure.h>
 #include <HTTPClient.h>
 
 #include <BLEDevice.h>
 #include <BLEUtils.h>
 #include <BLEClient.h>
 #include <BLEAddress.h>
 
 #include "soc/soc.h"
 #include "soc/rtc_cntl_reg.h"
 
 // ===================== CONFIGURAÇÕES =====================
 
 const char* WIFI_SSID = "IoTs";
 const char* WIFI_PASS = "AneryIot158";
 
 const char* API_BASE = "https://bracelet-pn7r.onrender.com";
 const char* API_BATCH_URL = "https://bracelet-pn7r.onrender.com/bracelets/packets";
 
 // MAC atual da pulseira que você está usando
 const char* DEVICE_MAC = "ef:7a:0d:30:b3:fa";
 
 // UUIDs 2208A
 static BLEUUID SERVICE_UUID((uint16_t)0xFFF0);
 static BLEUUID TX_UUID((uint16_t)0xFFF6);  // ESP32 -> pulseira
 static BLEUUID RX_UUID((uint16_t)0xFFF7);  // pulseira -> ESP32
 
 // ===================== LOGS =====================
 
 #define LOG_BLE_RX 1
 #define LOG_BLE_TX 1
 #define LOG_HTTP 1
 #define LOG_QUEUE 1
 #define LOG_STATE 1
 
// ===================== INTERVALOS =====================

// Controle para não encher fila com 0x28/0x09 a cada segundo
const unsigned long RX_028_MIN_GAP_MS = 3000;   // 1 pacote 0x28 a cada 3s por subtipo
const unsigned long RX_009_MIN_GAP_MS = 15000;  // 1 pacote 0x09 a cada 15s
const unsigned long RX_OTHER_MIN_GAP_MS = 1500;

// Janelas menores de medição
const unsigned long BLE_COLLECT_WINDOW_MS = 70UL * 1000UL;
const unsigned long HEALTH_MEASURE_WINDOW_MS = 8000;

// Tempo final esperando resposta dos históricos antes de desligar BLE
const unsigned long HISTORY_RESPONSE_WAIT_MS = 6000;

// Intervalo entre comandos de histórico
const unsigned long HISTORY_COMMAND_GAP_MS = 700;
 
 // Wi-Fi / Render
 const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
 const unsigned long RENDER_PING_INTERVAL_MS = 8000;
 const unsigned long HTTP_RETRY_INTERVAL_MS = 10000;
 
 // ===================== FILA =====================
 
#define RAW_HEX_MAX_LEN 360
#define PACKET_TYPE_LEN 8
#define HTTP_QUEUE_SIZE 50
#define BATCH_MAX_PACKETS 50
#define BATCH_PAYLOAD_MAX_LEN 9000
 
 struct RawPacketItem {
   bool used;
   char packetType[PACKET_TYPE_LEN];
   char rawHex[RAW_HEX_MAX_LEN];
   unsigned long receivedAtMs;
 };
 
RawPacketItem httpQueue[HTTP_QUEUE_SIZE];
int queueCount = 0;

static char batchPayload[BATCH_PAYLOAD_MAX_LEN];
 
 // ===================== BLE GLOBAL =====================
 
 BLEClient* bleClient = nullptr;
 BLERemoteCharacteristic* txChar = nullptr;
 BLERemoteCharacteristic* rxChar = nullptr;
 
 volatile bool bleConnected = false;
 bool bleInitialized = false;
 
 unsigned long lastBleReconnectAttempt = 0;
 
 // ===================== ESTADOS =====================
 
 enum MainState {
   STATE_BLE_START,
   STATE_BLE_COLLECT,
   STATE_BLE_QUERY_HISTORY,
   STATE_BLE_SHUTDOWN,
   STATE_WIFI_START,
   STATE_RENDER_WAKE,
   STATE_HTTP_SEND_BATCH,
   STATE_WIFI_SHUTDOWN
 };
 
 MainState mainState = STATE_BLE_START;
 unsigned long stateStartedAt = 0;
 
 enum HealthCommandState {
   HEALTH_IDLE,
   HEALTH_START_HR,
   HEALTH_STOP_HR,
   HEALTH_START_SPO2,
   HEALTH_STOP_SPO2,
   HEALTH_START_TEMP,
   HEALTH_STOP_TEMP,
  HEALTH_START_HRV,
  HEALTH_STOP_HRV,
  HEALTH_DONE
};
 
 HealthCommandState healthState = HEALTH_IDLE;
 unsigned long healthStateStartedAt = 0;
 
 int historyIndex = 0;
 unsigned long lastHistoryCommandAt = 0;
 bool historyCommandsFinished = false;
 
 unsigned long lastWifiTryAt = 0;
 unsigned long lastRenderPingAt = 0;
 unsigned long lastHttpTryAt = 0;
 
 // ===================== UTILS =====================
 
 void changeState(MainState next) {
   mainState = next;
   stateStartedAt = millis();
 
 #if LOG_STATE
   Serial.print("[STATE] -> ");
   switch (next) {
     case STATE_BLE_START: Serial.println("BLE_START"); break;
     case STATE_BLE_COLLECT: Serial.println("BLE_COLLECT"); break;
     case STATE_BLE_QUERY_HISTORY: Serial.println("BLE_QUERY_HISTORY"); break;
     case STATE_BLE_SHUTDOWN: Serial.println("BLE_SHUTDOWN"); break;
     case STATE_WIFI_START: Serial.println("WIFI_START"); break;
     case STATE_RENDER_WAKE: Serial.println("RENDER_WAKE"); break;
     case STATE_HTTP_SEND_BATCH: Serial.println("HTTP_SEND_BATCH"); break;
     case STATE_WIFI_SHUTDOWN: Serial.println("WIFI_SHUTDOWN"); break;
   }
 #endif
 }
 
 uint8_t calcCrc(uint8_t* p, uint8_t len = 16) {
   uint16_t s = 0;
   for (int i = 0; i < len - 1; i++) {
     s += p[i];
   }
   return s & 0xFF;
 }
 
 void rawToHex(const uint8_t* data, size_t len, char* out, size_t outSize) {
   if (!out || outSize == 0) return;
 
   out[0] = '\0';
 
   size_t maxBytes = len;
   size_t maxByOutput = (outSize - 1) / 3;
 
   if (maxBytes > maxByOutput) {
     maxBytes = maxByOutput;
   }
 
   for (size_t i = 0; i < maxBytes; i++) {
     char tmp[4];
     snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
     strncat(out, tmp, outSize - strlen(out) - 1);
   }
 
   size_t l = strlen(out);
   if (l > 0 && out[l - 1] == ' ') {
     out[l - 1] = '\0';
   }
 }
 
 void packetTypeToString(uint8_t packetType, char* out, size_t outSize) {
   snprintf(out, outSize, "0x%02X", packetType);
 }
 
 bool isBleReady() {
   return bleClient && bleClient->isConnected() && txChar && rxChar && bleConnected;
 }
 
 // ===================== FILA =====================
 
void enqueueRawPacket(const char* packetType, const char* rawHex) {
  if (!packetType || !rawHex) return;

  if (queueCount >= HTTP_QUEUE_SIZE) {
#if LOG_QUEUE
    Serial.println("[QUEUE] Limite 50 atingido. Ignorando novo pacote até enviar batch.");
#endif
    return;
  }

  RawPacketItem& item = httpQueue[queueCount];
  item.used = true;

  strncpy(item.packetType, packetType, PACKET_TYPE_LEN - 1);
  item.packetType[PACKET_TYPE_LEN - 1] = '\0';

  strncpy(item.rawHex, rawHex, RAW_HEX_MAX_LEN - 1);
  item.rawHex[RAW_HEX_MAX_LEN - 1] = '\0';

  item.receivedAtMs = millis();

  queueCount++;

#if LOG_QUEUE
  Serial.printf("[QUEUE] + %s | fila=%d/%d\n", packetType, queueCount, HTTP_QUEUE_SIZE);
#endif
}
 
 void removeQueueItems(int count) {
   if (count <= 0 || queueCount <= 0) return;
 
   if (count >= queueCount) {
     queueCount = 0;
     return;
   }
 
   for (int i = count; i < queueCount; i++) {
     httpQueue[i - count] = httpQueue[i];
   }
 
   queueCount -= count;
 }
 
 // ===================== WIFI =====================
 
void wifiOff() {
  if (WiFi.getMode() == WIFI_OFF) {
    return;
  }

  Serial.println("[WIFI] Desligando Wi-Fi...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
}
 
 bool wifiStartOrEnsure() {
   if (WiFi.status() == WL_CONNECTED) {
     return true;
   }
 
   unsigned long now = millis();
 
   if (now - lastWifiTryAt < WIFI_RETRY_INTERVAL_MS) {
     return false;
   }
 
   lastWifiTryAt = now;
 
 #if LOG_STATE
   Serial.println("[WIFI] Ligando/conectando...");
 #endif
 
   WiFi.mode(WIFI_STA);
   WiFi.setSleep(false);
   WiFi.begin(WIFI_SSID, WIFI_PASS);
 
   unsigned long start = millis();
 
   while (millis() - start < 12000) {
     if (WiFi.status() == WL_CONNECTED) {
       Serial.printf("[WIFI] Conectado. IP=%s RSSI=%d\n",
                     WiFi.localIP().toString().c_str(),
                     WiFi.RSSI());
       return true;
     }
 
     delay(250);
   }
 
   Serial.printf("[WIFI] Ainda não conectou. Status=%d\n", WiFi.status());
   return false;
 }
 
 // ===================== HTTP =====================
 
 bool pingRenderHealth() {
   if (WiFi.status() != WL_CONNECTED) return false;
 
   WiFiClientSecure client;
   client.setInsecure();
   client.setHandshakeTimeout(30);
 
   HTTPClient http;
   http.setReuse(false);
   http.setTimeout(25000);
 
   String url = String(API_BASE) + "/health";
 
 #if LOG_HTTP
   Serial.println("[HTTP] GET /health...");
 #endif
 
   if (!http.begin(client, url)) {
 #if LOG_HTTP
     Serial.println("[HTTP] begin /health falhou.");
 #endif
     return false;
   }
 
   http.addHeader("Connection", "close");
 
   int code = http.GET();
   http.end();
 
 #if LOG_HTTP
   Serial.printf("[HTTP] /health -> %d\n", code);
 #endif
 
   return code >= 200 && code < 500;
 }
 
bool buildBatchPayload(int& itemsInBatch) {
  if (queueCount <= 0) return false;

  batchPayload[0] = '\0';
  itemsInBatch = 0;

  int written = snprintf(
    batchPayload,
    BATCH_PAYLOAD_MAX_LEN,
    "{\"deviceMac\":\"%s\",\"source\":\"ESP32\",\"packets\":[",
    DEVICE_MAC
  );

  if (written <= 0 || written >= BATCH_PAYLOAD_MAX_LEN) {
    return false;
  }

  int limit = queueCount;
  if (limit > BATCH_MAX_PACKETS) {
    limit = BATCH_MAX_PACKETS;
  }

  for (int i = 0; i < limit; i++) {
    char itemJson[520];

    snprintf(
      itemJson,
      sizeof(itemJson),
      "%s{\"packetType\":\"%s\",\"rawHex\":\"%s\",\"receivedAtMs\":%lu}",
      itemsInBatch > 0 ? "," : "",
      httpQueue[i].packetType,
      httpQueue[i].rawHex,
      httpQueue[i].receivedAtMs
    );

    if (strlen(batchPayload) + strlen(itemJson) + 4 >= BATCH_PAYLOAD_MAX_LEN) {
      break;
    }

    strcat(batchPayload, itemJson);
    itemsInBatch++;
  }

  if (itemsInBatch <= 0) return false;

  strcat(batchPayload, "]}");

  return true;
}
 
bool postBatchToApi() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (queueCount <= 0) return true;

  int itemsInBatch = 0;

  if (!buildBatchPayload(itemsInBatch)) {
    Serial.println("[HTTP] Falha ao montar batch payload.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(30000);

#if LOG_HTTP
  Serial.printf("[HTTP] POST batch | itens=%d | fila=%d\n", itemsInBatch, queueCount);
#endif

  if (!http.begin(client, API_BATCH_URL)) {
#if LOG_HTTP
    Serial.println("[HTTP] begin batch falhou.");
#endif
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int code = http.POST((uint8_t*)batchPayload, strlen(batchPayload));

#if LOG_HTTP
  Serial.printf("[HTTP] batch status=%d\n", code);
#endif

  if (code > 0) {
    String res = http.getString();
#if LOG_HTTP
    Serial.print("[HTTP] response size=");
    Serial.println(res.length());
#endif
  }

  http.end();

  if (code == 200 || code == 201 || code == 202) {
    removeQueueItems(itemsInBatch);
    Serial.printf("[HTTP] Batch OK. Removidos=%d | fila=%d\n", itemsInBatch, queueCount);
    return true;
  }

  Serial.println("[HTTP] Batch falhou. Mantendo fila.");
  return false;
}
 
 // ===================== BLE CALLBACKS =====================
 
 class BraceletCallbacks : public BLEClientCallbacks {
   void onConnect(BLEClient*) override {
     bleConnected = true;
     Serial.println("[BLE] Callback conectado.");
   }
 
   void onDisconnect(BLEClient*) override {
     bleConnected = false;
     txChar = nullptr;
     rxChar = nullptr;
     Serial.println("[BLE] Callback desconectado.");
   }
 };
 
 BraceletCallbacks braceletCallbacks;
 
void notifyCallback(
  BLERemoteCharacteristic* characteristic,
  uint8_t* data,
  size_t len,
  bool isNotify
) {
  if (!data || len == 0) return;

  unsigned long now = millis();

  static unsigned long last028BySubtype[8] = {0};
  static unsigned long last009At = 0;
  static unsigned long lastOtherByType[256] = {0};

  uint8_t type = data[0];

  // Limita volume do 0x28 por subtipo: HR, SpO2, Temp, HRV
  if (type == 0x28 && len > 1) {
    uint8_t subtype = data[1];
    if (subtype < 8) {
      if (now - last028BySubtype[subtype] < RX_028_MIN_GAP_MS) {
        return;
      }
      last028BySubtype[subtype] = now;
    }
  }

  // Limita 0x09, porque tempo real gera pacote demais
  else if (type == 0x09) {
    if (now - last009At < RX_009_MIN_GAP_MS) {
      return;
    }
    last009At = now;
  }

  // Limita duplicidade de outros pacotes, tipo 0x13, 0x22, 0x27, 0x41
  else {
    if (now - lastOtherByType[type] < RX_OTHER_MIN_GAP_MS) {
      return;
    }
    lastOtherByType[type] = now;
  }

  if (queueCount >= HTTP_QUEUE_SIZE) {
#if LOG_QUEUE
    Serial.println("[QUEUE] Cheia. RX ignorado.");
#endif
    return;
  }

  char rawHex[RAW_HEX_MAX_LEN];
  char packetType[PACKET_TYPE_LEN];

  rawToHex(data, len, rawHex, sizeof(rawHex));
  packetTypeToString(type, packetType, sizeof(packetType));

#if LOG_BLE_RX
  Serial.printf("[BLE RX] %s len=%u | fila=%d/%d\n", packetType, len, queueCount, HTTP_QUEUE_SIZE);
#endif

  enqueueRawPacket(packetType, rawHex);
}
 
 // ===================== BLE =====================
 
bool startBle() {
  if (bleInitialized) {
    return true;
  }

  Serial.println("[BLE] Inicializando BLE...");
  BLEDevice::init("ESP32_2208A_GATEWAY");
  bleInitialized = true;
  delay(500);

  return true;
}

void blePauseForWifi() {
  Serial.println("[BLE] Pausando BLE para usar Wi-Fi...");

  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
    delay(500);
  }

  bleConnected = false;
  txChar = nullptr;
  rxChar = nullptr;

  // NÃO chamar BLEDevice::deinit(true) aqui.
  // Manter a stack BLE inicializada evita falha ao criar client no próximo ciclo.
}

bool connectBracelet() {
  if (!startBle()) return false;

  bleConnected = false;
  txChar = nullptr;
  rxChar = nullptr;

  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
    delay(500);
  }

  if (!bleClient) {
    Serial.println("[BLE] Criando BLE client...");
    bleClient = BLEDevice::createClient();

    if (!bleClient) {
      Serial.println("[BLE] Falha ao criar BLE client. Tentando resetar stack BLE uma vez...");

      BLEDevice::deinit(true);
      bleInitialized = false;
      delay(1500);

      BLEDevice::init("ESP32_2208A_GATEWAY");
      bleInitialized = true;
      delay(800);

      bleClient = BLEDevice::createClient();

      if (!bleClient) {
        Serial.println("[BLE] Falha definitiva ao criar BLE client.");
        return false;
      }
    }

    bleClient->setClientCallbacks(&braceletCallbacks);
  }

  Serial.printf("[BLE] Conectando na pulseira %s...\n", DEVICE_MAC);

  BLEAddress address(DEVICE_MAC);

  bool connected = bleClient->connect(address, BLE_ADDR_TYPE_RANDOM);

  if (!connected) {
    Serial.println("[BLE] Falhou RANDOM. Tentando PUBLIC...");
    delay(500);
    connected = bleClient->connect(address, BLE_ADDR_TYPE_PUBLIC);
  }

  if (!connected) {
    Serial.println("[BLE] Falha ao conectar na pulseira.");
    bleConnected = false;
    txChar = nullptr;
    rxChar = nullptr;
    return false;
  }

  Serial.println("[BLE] Conectado. Procurando serviço FFF0...");

  BLERemoteService* service = bleClient->getService(SERVICE_UUID);
  if (!service) {
    Serial.println("[BLE] Serviço FFF0 não encontrado.");
    bleClient->disconnect();
    delay(500);
    bleConnected = false;
    txChar = nullptr;
    rxChar = nullptr;
    return false;
  }

  txChar = service->getCharacteristic(TX_UUID);
  rxChar = service->getCharacteristic(RX_UUID);

  if (!txChar || !rxChar) {
    Serial.println("[BLE] Características FFF6/FFF7 não encontradas.");
    bleClient->disconnect();
    delay(500);
    bleConnected = false;
    txChar = nullptr;
    rxChar = nullptr;
    return false;
  }

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
    Serial.println("[BLE] Notify registrado em RX FFF7.");
  } else {
    Serial.println("[BLE] RX FFF7 não suporta notify.");
  }

  bleConnected = true;

  Serial.println("[BLE] Pulseira pronta.");

  return true;
}
 
 bool ensureBraceletConnected() {
   if (isBleReady()) return true;
 
   unsigned long now = millis();
 
   if (now - lastBleReconnectAttempt < 10000) {
     return false;
   }
 
   lastBleReconnectAttempt = now;
 
   Serial.println("[BLE] Não conectado. Tentando reconectar...");
   return connectBracelet();
 }
 
 // ===================== COMANDOS BLE =====================
 
 void sendPacket16(uint8_t* pkt) {
   if (!isBleReady()) {
     Serial.println("[BLE TX] Não enviado: BLE não pronto.");
     return;
   }
 
   pkt[15] = calcCrc(pkt);
 
 #if LOG_BLE_TX
   char txHex[80];
   rawToHex(pkt, 16, txHex, sizeof(txHex));
   Serial.printf("[BLE TX] %s\n", txHex);
 #endif
 
   txChar->writeValue(pkt, 16, true);
 }
 
 void sendCommand(uint8_t cmd, uint8_t p1 = 0x00, uint8_t p2 = 0x00, uint8_t p3 = 0x00) {
   uint8_t pkt[16] = {
     cmd, p1, p2, p3,
     0, 0, 0, 0,
     0, 0, 0, 0,
     0, 0, 0, 0
   };
 
   sendPacket16(pkt);
 }
 
 // Saúde 0x28
 void startHealth(uint8_t type) {
   sendCommand(0x28, type, 0x01);
 }
 
 void stopHealth(uint8_t type) {
   sendCommand(0x28, type, 0x00);
 }
 
 void stopAllHealth() {
   if (!isBleReady()) return;
 
   stopHealth(0x01);  // HRV
   delay(50);
   stopHealth(0x02);  // HR
   delay(50);
   stopHealth(0x03);  // SpO2
   delay(50);
   stopHealth(0x04);  // Temperatura
   delay(50);
 }
 
 // Tempo real 0x09
 void startRealtime() {
   sendCommand(0x09, 0x01, 0x00);
 }
 
 void stopRealtime() {
   sendCommand(0x09, 0x00, 0x00);
 }
 
 // Dispositivo
 void readBattery() {
   sendCommand(0x13, 0x99);
 }
 
 void readMacAddress() {
   sendCommand(0x22);
 }
 
 void readFirmware() {
   sendCommand(0x27);
 }
 
 void readDeviceTime() {
   sendCommand(0x41);
 }
 
 // Históricos
 void readHistorySleep() {
   sendCommand(0x53, 0x00, 0x00, 0x00);
 }
 
 void readHistoryHeartRate() {
   sendCommand(0x54, 0x00, 0x00, 0x00);
 }
 
 void readHistorySingleHeartRate() {
   sendCommand(0x55, 0x00, 0x00, 0x00);
 }
 
 void readHistoryHrv() {
   sendCommand(0x56, 0x00, 0x00, 0x00);
 }
 
 void readHistorySpo2Manual() {
   sendCommand(0x60, 0x00, 0x00, 0x00);
 }
 
 void readHistorySpo2Auto() {
   sendCommand(0x66, 0x00, 0x00, 0x00);
 }
 
 void readHistoryTemperatureManual() {
   sendCommand(0x62, 0x01, 0x00, 0x00);
 }
 
 void readHistoryTemperatureAuto() {
   sendCommand(0x65, 0x01, 0x00, 0x00);
 }
 
 // ===================== COMANDOS INICIAIS BLE =====================
 
 void sendInitialBleCommands() {
   if (!isBleReady()) return;
 
   Serial.println("[APP] Comandos iniciais BLE...");
 
   readBattery();
   delay(300);
 
   readMacAddress();
   delay(300);
 
   readFirmware();
   delay(300);
 
   readDeviceTime();
   delay(300);
 
  // startRealtime();
  // delay(300);
 
   healthState = HEALTH_START_HR;
   healthStateStartedAt = millis();
 }
 
 // ===================== HEALTH SCHEDULER =====================
 
 void processHealthScheduler() {
   if (!isBleReady()) return;
 
   unsigned long now = millis();
 
   switch (healthState) {
     case HEALTH_IDLE:
       healthState = HEALTH_START_HR;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_START_HR:
       Serial.println("[APP] Start HR");
       startHealth(0x02);
       healthState = HEALTH_STOP_HR;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_HR:
       if (now - healthStateStartedAt >= HEALTH_MEASURE_WINDOW_MS) {
         Serial.println("[APP] Stop HR");
         stopHealth(0x02);
         healthState = HEALTH_START_SPO2;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_SPO2:
       Serial.println("[APP] Start SpO2");
       startHealth(0x03);
       healthState = HEALTH_STOP_SPO2;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_SPO2:
       if (now - healthStateStartedAt >= HEALTH_MEASURE_WINDOW_MS) {
         Serial.println("[APP] Stop SpO2");
         stopHealth(0x03);
         healthState = HEALTH_START_TEMP;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_TEMP:
       Serial.println("[APP] Start Temp");
       startHealth(0x04);
       healthState = HEALTH_STOP_TEMP;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_TEMP:
       if (now - healthStateStartedAt >= HEALTH_MEASURE_WINDOW_MS) {
         Serial.println("[APP] Stop Temp");
         stopHealth(0x04);
         healthState = HEALTH_START_HRV;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_HRV:
       Serial.println("[APP] Start HRV");
       startHealth(0x01);
       healthState = HEALTH_STOP_HRV;
       healthStateStartedAt = now;
       break;
 
    case HEALTH_STOP_HRV:
      if (now - healthStateStartedAt >= HEALTH_MEASURE_WINDOW_MS) {
        Serial.println("[APP] Stop HRV");
        stopHealth(0x01);

        Serial.println("[APP] Ciclo de saúde finalizado.");
        healthState = HEALTH_DONE;
        healthStateStartedAt = now;
      }
      break;

    case HEALTH_DONE:
      break;
  }
}
 
 // ===================== HISTÓRICOS =====================
 
 void resetHistoryQuery() {
   historyIndex = 0;
   lastHistoryCommandAt = 0;
   historyCommandsFinished = false;
 }
 
 void processHistoryQuery() {
   if (!isBleReady()) return;
 
   unsigned long now = millis();
 
   if (historyCommandsFinished) {
     return;
   }
 
   if (lastHistoryCommandAt != 0 && now - lastHistoryCommandAt < HISTORY_COMMAND_GAP_MS) {
     return;
   }
 
   lastHistoryCommandAt = now;
 
   Serial.printf("[APP] Consultando histórico index=%d\n", historyIndex);
 
   switch (historyIndex) {
     case 0:
       readHistorySleep();
       break;
 
     case 1:
       readHistoryHeartRate();
       break;
 
     case 2:
       readHistorySingleHeartRate();
       break;
 
     case 3:
       readHistoryHrv();
       break;
 
     case 4:
       readHistorySpo2Manual();
       break;
 
     case 5:
       readHistorySpo2Auto();
       break;
 
     case 6:
       readHistoryTemperatureManual();
       break;
 
     case 7:
       readHistoryTemperatureAuto();
       break;
 
     default:
       historyCommandsFinished = true;
       Serial.println("[APP] Comandos de histórico finalizados. Aguardando respostas...");
       break;
   }
 
   historyIndex++;
 }
 
 // ===================== SETUP =====================
 
 void setup() {
   WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
 
   Serial.begin(115200);
   delay(1000);
 
   Serial.println();
   Serial.println("==========================================");
   Serial.println("ESP32 2208A BLE COLLECT -> WIFI BATCH API");
   Serial.println("==========================================");
 
   wifiOff();
 
   changeState(STATE_BLE_START);
 }
 
 // ===================== LOOP =====================
 
 void loop() {
   unsigned long now = millis();
 
   switch (mainState) {
    case STATE_BLE_START: {
      static bool wifiAlreadyOffForBle = false;

      if (!wifiAlreadyOffForBle) {
        wifiOff();
        wifiAlreadyOffForBle = true;
        delay(800);
      }

      if (connectBracelet()) {
        wifiAlreadyOffForBle = false;

        sendInitialBleCommands();
        changeState(STATE_BLE_COLLECT);
      } else {
        Serial.println("[BLE] Falha no start. Tentando novamente em 10s...");
        delay(10000);
      }

      break;
    }
 
     case STATE_BLE_COLLECT: {
       ensureBraceletConnected();
 
       if (isBleReady()) {
         processHealthScheduler();
       }
 
       if (now - stateStartedAt >= BLE_COLLECT_WINDOW_MS) {
         Serial.printf("[APP] Janela BLE finalizada. Fila=%d\n", queueCount);
         resetHistoryQuery();
         changeState(STATE_BLE_QUERY_HISTORY);
       }
 
       break;
     }
 
     case STATE_BLE_QUERY_HISTORY: {
       ensureBraceletConnected();
 
       if (isBleReady()) {
         processHistoryQuery();
       }
 
       if (historyCommandsFinished && now - lastHistoryCommandAt >= HISTORY_RESPONSE_WAIT_MS) {
         Serial.printf("[APP] Histórico coletado. Fila=%d\n", queueCount);
         changeState(STATE_BLE_SHUTDOWN);
       }
 
       break;
     }
 
     case STATE_BLE_SHUTDOWN: {
       if (isBleReady()) {
         stopRealtime();
         delay(150);
         stopAllHealth();
         delay(300);
       }
 
      blePauseForWifi();
      delay(800);
      changeState(STATE_WIFI_START);
      break;
    }

    case STATE_WIFI_START: {
       if (wifiStartOrEnsure()) {
         changeState(STATE_RENDER_WAKE);
       }
 
       break;
     }
 
     case STATE_RENDER_WAKE: {
       if (WiFi.status() != WL_CONNECTED) {
         changeState(STATE_WIFI_START);
         break;
       }
 
       if (now - lastRenderPingAt >= RENDER_PING_INTERVAL_MS) {
         lastRenderPingAt = now;
 
         bool ok = pingRenderHealth();
 
         if (ok) {
           Serial.println("[HTTP] Render acordado.");
           changeState(STATE_HTTP_SEND_BATCH);
         } else {
           Serial.println("[HTTP] Render ainda não respondeu. Tentando de novo...");
         }
       }
 
       break;
     }
 
     case STATE_HTTP_SEND_BATCH: {
       if (WiFi.status() != WL_CONNECTED) {
         changeState(STATE_WIFI_START);
         break;
       }
 
       if (queueCount <= 0) {
         Serial.println("[HTTP] Fila vazia. Finalizando envio.");
         changeState(STATE_WIFI_SHUTDOWN);
         break;
       }
 
       if (now - lastHttpTryAt >= HTTP_RETRY_INTERVAL_MS) {
         lastHttpTryAt = now;
 
         bool ok = postBatchToApi();
 
         if (!ok) {
           Serial.println("[HTTP] Batch falhou. Voltando para wake Render.");
           changeState(STATE_RENDER_WAKE);
         }
       }
 
       break;
     }
 
    case STATE_WIFI_SHUTDOWN: {
      wifiOff();

      Serial.println("[APP] Ciclo completo. Voltando para BLE.");
      delay(1500);

      healthState = HEALTH_IDLE;
      lastBleReconnectAttempt = 0;
      lastRenderPingAt = 0;
      lastHttpTryAt = 0;

      changeState(STATE_BLE_START);
      break;
    }
   }
 
   delay(20);
 }