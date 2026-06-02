#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>

// =====================
// CONFIGURAÇÃO GLOBAL
// =====================

Preferences prefs;
WebServer server(80);

String wifiSsid = "";
String wifiPass = "";
String apiUrl = "";
String deviceMac = "";

unsigned long scanTimeoutMs = 15000;
unsigned long batteryTimeoutMs = 5000;
unsigned long restartDelaySuccessMs = 30000;
unsigned long restartDelayErrorMs = 15000;

const char* CONFIG_AP_SSID = "ESP32_BRACELET_CONFIG";
const char* CONFIG_AP_PASS = "12345678";

// =====================
// BLE
// =====================

BLEClient* bleClient = nullptr;
BLERemoteCharacteristic* txChar = nullptr;
BLERemoteCharacteristic* rxChar = nullptr;

// Modos 0x28 (byte AA) + bateria 0x13
const uint8_t HEALTH_MODES[] = {0x01, 0x02, 0x03, 0x04, 0x05};
const char* MODE_LABELS[] = {"HRV", "Coracao", "Oxigenio", "Temperatura", "Pressao"};
const char* METRIC_NAMES[] = {"hrv", "heart", "spo2", "temperature", "blood_pressure"};
const int NUM_HEALTH = 5;

char rawHex28[NUM_HEALTH][50] = {{0}};
bool captured28[NUM_HEALTH] = {false};
char rawHex13[24] = "";
bool captured13 = false;

// -1 = bateria, 0..4 = scan 0x28, 5 = envio HTTP
int readPhase = -1;
unsigned long lastScanCmd = 0;

bool readingActive = false;
bool bleConnected = false;
bool sendingData = false;

bool scheduledRestart = false;
unsigned long scheduledRestartAt = 0;

String lastStatus = "idle";
String lastError = "";

// =====================
// HELPERS JSON SIMPLES
// Sem ArduinoJson para economizar espaço
// =====================

String getJsonValue(String body, String key) {
  String pattern = "\"" + key + "\"";
  int keyIndex = body.indexOf(pattern);
  if (keyIndex < 0) return "";

  int colonIndex = body.indexOf(":", keyIndex);
  if (colonIndex < 0) return "";

  int valueStart = colonIndex + 1;

  while (valueStart < body.length() && body[valueStart] == ' ') {
    valueStart++;
  }

  if (body[valueStart] == '"') {
    valueStart++;
    int valueEnd = body.indexOf("\"", valueStart);
    if (valueEnd < 0) return "";
    return body.substring(valueStart, valueEnd);
  }

  int valueEnd = body.indexOf(",", valueStart);
  if (valueEnd < 0) {
    valueEnd = body.indexOf("}", valueStart);
  }

  if (valueEnd < 0) return "";

  String value = body.substring(valueStart, valueEnd);
  value.trim();
  return value;
}

unsigned long getJsonULong(String body, String key, unsigned long defaultValue) {
  String value = getJsonValue(body, key);
  if (value.length() == 0) return defaultValue;
  return value.toInt();
}

String normalizeMac(String mac) {
  mac.trim();
  mac.toUpperCase();
  return mac;
}

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);

  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value[i];

    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out += c;
    }
  }

  return out;
}

void scheduleRestart(unsigned long delayMs) {
  scheduledRestartAt = millis() + delayMs;
  scheduledRestart = true;
  Serial.printf("[APP] Reinício agendado em %lu ms\n", delayMs);
}

// =====================
// PREFERENCES
// =====================

void loadConfig() {
  prefs.begin("bracelet", false);

  wifiSsid = prefs.getString("wifiSsid", "");
  wifiPass = prefs.getString("wifiPass", "");
  apiUrl = prefs.getString("apiUrl", "");
  deviceMac = prefs.getString("deviceMac", "");

  scanTimeoutMs = prefs.getULong("scanTimeout", 15000);
  restartDelaySuccessMs = prefs.getULong("okDelay", 30000);
  restartDelayErrorMs = prefs.getULong("errDelay", 15000);

  prefs.end();

  Serial.println("[CONFIG] Carregada:");
  Serial.println("wifiSsid: " + wifiSsid);
  Serial.println("apiUrl: " + apiUrl);
  Serial.println("deviceMac: " + deviceMac);
  Serial.printf("scanTimeoutMs: %lu\n", scanTimeoutMs);
}

void saveConfig() {
  prefs.begin("bracelet", false);

  prefs.putString("wifiSsid", wifiSsid);
  prefs.putString("wifiPass", wifiPass);
  prefs.putString("apiUrl", apiUrl);
  prefs.putString("deviceMac", deviceMac);

  prefs.putULong("scanTimeout", scanTimeoutMs);
  prefs.putULong("okDelay", restartDelaySuccessMs);
  prefs.putULong("errDelay", restartDelayErrorMs);

  prefs.end();

  Serial.println("[CONFIG] Salva na memória.");
}

void clearConfig() {
  prefs.begin("bracelet", false);
  prefs.clear();
  prefs.end();

  Serial.println("[CONFIG] Apagada.");
}

// =====================
// CRC
// =====================

uint8_t calcCrc(uint8_t* p) {
  uint16_t s = 0;
  for (int i = 0; i < 15; i++) {
    s += p[i];
  }
  return s & 0xFF;
}

// =====================
// BLE
// =====================

void sendCommand(uint8_t cmd, uint8_t p1 = 0, uint8_t p2 = 0) {
  if (!txChar) return;

  uint8_t pkt[16] = {
    cmd, p1, p2,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };

  pkt[15] = calcCrc(pkt);
  txChar->writeValue(pkt, 16, true);
}

void stopAllHealthModes() {
  for (int i = 0; i < NUM_HEALTH; i++) {
    sendCommand(0x28, HEALTH_MODES[i], 0x00);
    delay(40);
  }
}

int modeToSlot(uint8_t mode) {
  for (int i = 0; i < NUM_HEALTH; i++) {
    if (HEALTH_MODES[i] == mode) return i;
  }
  return -1;
}

bool isHealthPacketReady(uint8_t mode, uint8_t* data, size_t len) {
  if (len < 10) return false;

  switch (mode) {
    case 0x01:
      return data[4] > 0 || data[5] > 0;
    case 0x02:
      return data[2] > 0;
    case 0x03:
      return data[3] > 0;
    case 0x04: {
      uint16_t tempRaw = data[8] | (data[9] << 8);
      return tempRaw > 0;
    }
    case 0x05: {
      uint8_t sys = data[6];
      uint8_t dia = data[7];
      if (sys == 0x7B && dia == 0x49) return false;
      return sys > 0 && dia > 0;
    }
    default:
      return true;
  }
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

void notifyCallback(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (!readingActive) return;
  if (len < 2) return;

  if (data[0] == 0x13) {
    if (captured13) return;
    storeHex(rawHex13, sizeof(rawHex13), data, len);
    captured13 = true;
    Serial.printf("[BLE] Bateria capturada: %s\n", rawHex13);
    return;
  }

  if (data[0] != 0x28 || len < 10) return;

  uint8_t mode = data[1];
  int slot = modeToSlot(mode);
  if (slot < 0 || captured28[slot]) return;
  if (!isHealthPacketReady(mode, data, len)) return;

  storeHex(rawHex28[slot], sizeof(rawHex28[slot]), data, len);
  captured28[slot] = true;
  Serial.printf("[BLE] %s (0x%02X) capturado: %s\n", MODE_LABELS[slot], mode, rawHex28[slot]);
}

void startHealthScan(int index) {
  stopAllHealthModes();
  delay(120);
  sendCommand(0x28, HEALTH_MODES[index], 0x01);
  lastScanCmd = millis();
  lastStatus = "reading_" + String(METRIC_NAMES[index]);
  Serial.printf("[BLE] Iniciando %s (modo 0x%02X)...\n", MODE_LABELS[index], HEALTH_MODES[index]);
}

bool connectToBracelet() {
  if (deviceMac.length() == 0) {
    lastError = "deviceMac não configurado";
    return false;
  }

  Serial.println("[BLE] Inicializando BLE...");
  BLEDevice::init("");

  bleClient = BLEDevice::createClient();

  String macLower = deviceMac;
  macLower.toLowerCase();

  Serial.println("[BLE] Conectando na pulseira: " + macLower);

  bool connected = bleClient->connect(
    BLEAddress(macLower.c_str()),
    BLE_ADDR_TYPE_RANDOM
  );

  if (!connected) {
    lastError = "Falha ao conectar na pulseira";
    return false;
  }

  BLERemoteService* svc = bleClient->getService(BLEUUID((uint16_t)0xFFF0));

  if (!svc) {
    lastError = "Serviço BLE 0xFFF0 não encontrado";
    return false;
  }

  txChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF6));
  rxChar = svc->getCharacteristic(BLEUUID((uint16_t)0xFFF7));

  if (!txChar || !rxChar) {
    lastError = "Characteristics BLE não encontradas";
    return false;
  }

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
  }

  bleConnected = true;
  Serial.println("[BLE] Conectado com sucesso.");

  return true;
}

void bleStop() {
  Serial.println("[BLE] Parando BLE...");

  txChar = nullptr;
  rxChar = nullptr;

  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
  }

  bleClient = nullptr;
  bleConnected = false;

  BLEDevice::deinit(true);

  delay(500);

  Serial.printf("[BLE] Desligado. Heap livre: %u\n", ESP.getFreeHeap());
}

void resetReadingState() {
  for (int i = 0; i < NUM_HEALTH; i++) {
    memset(rawHex28[i], 0, sizeof(rawHex28[i]));
    captured28[i] = false;
  }

  memset(rawHex13, 0, sizeof(rawHex13));
  captured13 = false;

  readPhase = -1;
  lastScanCmd = 0;
  lastError = "";
}

// =====================
// HTTP POST PARA API EXTERNA
// =====================

bool postPacket(const char* packetType, const char* hex, const char* metricName) {
  if (apiUrl.length() == 0) {
    lastError = "apiUrl não configurada";
    return false;
  }

  char payload[350];

  snprintf(
    payload,
    sizeof(payload),
    "{"
      "\"deviceMac\":\"%s\","
      "\"packetType\":\"%s\","
      "\"metricName\":\"%s\","
      "\"rawHex\":\"%s\","
      "\"source\":\"ESP32\","
      "\"wifiSsid\":\"%s\","
      "\"ip\":\"%s\","
      "\"rssi\":%d,"
      "\"heapFree\":%u"
    "}",
    deviceMac.c_str(),
    packetType,
    metricName,
    hex,
    wifiSsid.c_str(),
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    ESP.getFreeHeap()
  );

  Serial.printf("[HTTP] POST -> %s\n", payload);

  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(45);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(30000);

  if (!http.begin(sc, apiUrl)) {
    lastError = "Falha no http.begin";
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)payload, strlen(payload));
  String responseBody = http.getString();

  if (code == 200) {
    Serial.printf("[HTTP] POST OK (%s)\n", metricName);
  } else if (code > 0) {
    Serial.printf("[HTTP] Status %d (%s): %s\n", code, metricName, responseBody.c_str());
    if (code == 422) {
      lastError = "Pacote salvo com erro de decode na API";
    } else {
      lastError = "HTTP " + String(code);
    }
  } else {
    lastError = http.errorToString(code);
    Serial.printf("[HTTP] Falha (%s): %d - %s\n", metricName, code, lastError.c_str());
  }

  http.end();

  return code == 200;
}

// =====================
// CONTROLE DE LEITURA
// =====================

void startReading() {
  if (readingActive) {
    Serial.println("[APP] Leitura já está ativa.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lastError = "Wi-Fi desconectado";
    lastStatus = "error";
    return;
  }

  if (apiUrl.length() == 0 || deviceMac.length() == 0) {
    lastError = "apiUrl ou deviceMac não configurados";
    lastStatus = "error";
    return;
  }

  resetReadingState();

  readingActive = true;
  lastStatus = "starting";

  if (!connectToBracelet()) {
    readingActive = false;
    lastStatus = "error";
    bleStop();
    return;
  }

  stopAllHealthModes();
  delay(200);

  sendCommand(0x13, 0x99);
  readPhase = -1;
  lastScanCmd = millis();
  lastStatus = "reading_battery";
  Serial.println("[APP] Ciclo completo: bateria -> HRV -> coração -> SpO2 -> temperatura -> pressão");
  Serial.println("[BLE] Solicitando bateria (0x13)...");
}

void stopReading() {
  readingActive = false;
  lastStatus = "stopped";

  if (bleConnected) {
    stopAllHealthModes();
    delay(100);
    bleStop();
  }

  Serial.println("[APP] Leitura cancelada.");
}

void readingLoop() {
  if (!readingActive) return;

  // Fase bateria (0x13)
  if (readPhase == -1) {
    if (!captured13) {
      if (millis() - lastScanCmd < batteryTimeoutMs) return;
      Serial.println("[BLE] Timeout bateria — seguindo sem 0x13.");
    }
    readPhase = 0;
    startHealthScan(0);
    return;
  }

  // Fases de saúde (0x28)
  if (readPhase >= 0 && readPhase < NUM_HEALTH) {
    if (!captured28[readPhase]) {
      if (millis() - lastScanCmd > scanTimeoutMs) {
        Serial.printf("[BLE] Timeout %s. Reenviando...\n", MODE_LABELS[readPhase]);
        sendCommand(0x28, HEALTH_MODES[readPhase], 0x01);
        lastScanCmd = millis();
      }
      return;
    }

    sendCommand(0x28, HEALTH_MODES[readPhase], 0x00);
    delay(120);
    readPhase++;

    if (readPhase < NUM_HEALTH) {
      startHealthScan(readPhase);
      return;
    }
  }

  Serial.println("[APP] Ciclo BLE completo. Enviando para API...");

  readingActive = false;
  sendingData = true;
  lastStatus = "sending";

  stopAllHealthModes();
  delay(100);
  bleStop();

  int sent = 0;
  int okCount = 0;

  if (captured13) {
    sent++;
    if (postPacket("0x13", rawHex13, "battery")) okCount++;
  } else {
    Serial.println("[APP] Bateria não capturada.");
  }

  for (int i = 0; i < NUM_HEALTH; i++) {
    if (!captured28[i]) {
      Serial.printf("[APP] %s (0x%02X) não capturado.\n", MODE_LABELS[i], HEALTH_MODES[i]);
      continue;
    }
    sent++;
    if (postPacket("0x28", rawHex28[i], METRIC_NAMES[i])) okCount++;
  }

  sendingData = false;

  Serial.printf("[APP] Enviados %d/%d pacotes.\n", okCount, sent);

  if (sent > 0 && okCount == sent) {
    lastStatus = "done";
    Serial.println("[APP] Dados enviados com sucesso.");
    scheduleRestart(restartDelaySuccessMs);
  } else {
    lastStatus = "error";
    if (sent == 0) lastError = "Nenhum dado capturado no ciclo";
    Serial.println("[APP] Falha ou captura incompleta.");
    scheduleRestart(restartDelayErrorMs);
  }
}

// =====================
// WEB SERVER LOCAL
// =====================

String buildStatusJson() {
  String json = "{";

  json += "\"status\":\"" + jsonEscape(lastStatus) + "\",";
  json += "\"error\":\"" + jsonEscape(lastError) + "\",";
  json += "\"readingActive\":" + String(readingActive ? "true" : "false") + ",";
  json += "\"sendingData\":" + String(sendingData ? "true" : "false") + ",";
  json += "\"bleConnected\":" + String(bleConnected ? "true" : "false") + ",";

  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifiSsid\":\"" + jsonEscape(wifiSsid) + "\",";
  json += "\"ip\":\"" + jsonEscape(WiFi.localIP().toString()) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";

  json += "\"apiUrl\":\"" + apiUrl + "\",";
  json += "\"deviceMac\":\"" + deviceMac + "\",";

  json += "\"scanTimeoutMs\":" + String(scanTimeoutMs) + ",";
  json += "\"heapFree\":" + String(ESP.getFreeHeap()) + ",";

  json += "\"batteryCaptured\":" + String(captured13 ? "true" : "false") + ",";
  json += "\"batteryRawHex\":\"" + jsonEscape(String(rawHex13)) + "\",";
  json += "\"readPhase\":" + String(readPhase) + ",";

  json += "\"hrvCaptured\":" + String(captured28[0] ? "true" : "false") + ",";
  json += "\"heartCaptured\":" + String(captured28[1] ? "true" : "false") + ",";
  json += "\"spo2Captured\":" + String(captured28[2] ? "true" : "false") + ",";
  json += "\"temperatureCaptured\":" + String(captured28[3] ? "true" : "false") + ",";
  json += "\"bloodPressureCaptured\":" + String(captured28[4] ? "true" : "false") + ",";

  json += "\"hrvRawHex\":\"" + jsonEscape(String(rawHex28[0])) + "\",";
  json += "\"heartRawHex\":\"" + jsonEscape(String(rawHex28[1])) + "\",";
  json += "\"spo2RawHex\":\"" + jsonEscape(String(rawHex28[2])) + "\",";
  json += "\"temperatureRawHex\":\"" + jsonEscape(String(rawHex28[3])) + "\",";
  json += "\"bloodPressureRawHex\":\"" + jsonEscape(String(rawHex28[4])) + "\"";

  json += "}";

  return json;
}

void handleStatus() {
  server.send(200, "application/json", buildStatusJson());
}

void handleConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"body JSON obrigatório\"}");
    return;
  }

  String body = server.arg("plain");

  String newWifiSsid = getJsonValue(body, "wifiSsid");
  restartDelayErrorMs = getJsonULong(body, "restartDelayErrorMs", restartDelayErrorMs);

  bool wifiChanged = newWifiSsid.length() > 0;

  saveConfig();

  if (wifiChanged) {
    server.send(
      200,
      "application/json",
      "{\"ok\":true,\"message\":\"Configuração salva. Reiniciando para aplicar Wi-Fi...\"}"
    );
    scheduleRestart(2000);
    return;
  }

  server.send(
    200,
    "application/json",
    "{\"ok\":true,\"message\":\"Configuração salva.\"}"
  );
}

void handleStart() {
  if (readingActive) {
    server.send(409, "application/json", "{\"error\":\"Leitura já está em andamento\"}");
    return;
  }

  startReading();

  if (lastStatus == "error") {
    server.send(500, "application/json", buildStatusJson());
    return;
  }

  server.send(200, "application/json", "{\"ok\":true,\"message\":\"Leitura iniciada\"}");
}

void handleStop() {
  stopReading();
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"Leitura parada\"}");
}

void handleResetConfig() {
  clearConfig();
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"Configuração apagada. Reiniciando...\"}");
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  server.send(
    200,
    "application/json",
    "{"
      "\"name\":\"ESP32 Bracelet Gateway\","
      "\"endpoints\":{"
        "\"GET /status\":\"estado atual\","
        "\"POST /config\":\"wifiSsid, wifiPass, apiUrl, deviceMac, scanTimeoutMs\","
        "\"POST /start\":\"inicia leitura BLE\","
        "\"POST /stop\":\"para leitura\","
        "\"POST /reset-config\":\"apaga NVS e reinicia\""
      "}"
    "}"
  );
}

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/reset-config", HTTP_POST, handleResetConfig);

  server.begin();

  Serial.println("[HTTP SERVER] Servidor iniciado na porta 80.");
  Serial.println("[HTTP SERVER] GET /status | POST /config /start /stop /reset-config");
}

// =====================
// WI-FI / AP CONFIG
// =====================

bool connectWifi() {
  if (wifiSsid.length() == 0) {
    Serial.println("[WIFI] SSID não configurado.");
    return false;
  }

  Serial.println("[WIFI] Conectando em: " + wifiSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.println("[WIFI] Conectado.");
    Serial.println("[WIFI] IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("[WIFI] Falha ao conectar.");
  return false;
}

void startConfigAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASS);

  IPAddress ip = WiFi.softAPIP();

  Serial.println("[AP] Modo configuração ativo.");
  Serial.println("[AP] SSID: " + String(CONFIG_AP_SSID));
  Serial.println("[AP] Senha: " + String(CONFIG_AP_PASS));
  Serial.println("[AP] IP: " + ip.toString());

  lastStatus = "config_ap";
}

// =====================
// SETUP / LOOP
// =====================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 Bracelet Gateway Dinâmico");
  Serial.println("================================");

  loadConfig();

  bool wifiOk = connectWifi();

  if (!wifiOk) {
    startConfigAP();
  } else {
    lastStatus = "idle";
  }

  setupServer();

  Serial.println("[APP] Pronto.");
}

void loop() {
  server.handleClient();
  readingLoop();

  if (scheduledRestart && millis() >= scheduledRestartAt) {
    Serial.println("[APP] Reiniciando ESP32...");
    ESP.restart();
  }

  delay(10);
}