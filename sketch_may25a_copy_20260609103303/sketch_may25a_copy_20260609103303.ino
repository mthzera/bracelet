
 #include <WiFi.h>
 #include <WiFiClientSecure.h>
 #include <HTTPClient.h>
 
 #include <BLEDevice.h>
 #include <BLEUtils.h>
 #include <BLEClient.h>
 #include <BLEAddress.h>
 
 #include "soc/soc.h"
 #include "soc/rtc_cntl_reg.h"

 #include <time.h>
 #include <stdarg.h>
 #include <sys/time.h>
 #include "esp_bt.h"
 #include "esp_sntp.h"
 
 // ===================== CONFIGURAÇÕES =====================
 
 const char* WIFI_SSID = "IoTs";
 const char* WIFI_PASS = "AneryIot158";
 
 const char* API_HOST = "bracelet-pn7r.onrender.com";
 const char* API_BATCH_PATH = "/bracelets/packets";
 
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

 // Fuso horário: UTC-3 (Brasil)
 const long TIMEZONE_OFFSET_SEC = -3 * 3600;
 const int TIMEZONE_DST_SEC = 0;

 bool clockSynced = false;

 // Epoch mínimo plausível: 2024-01-01
 const time_t MIN_VALID_EPOCH_SEC = 1704067200;
 const uint64_t MIN_VALID_EPOCH_MS = 1704067200000ULL;

// ===================== INTERVALOS =====================

// Controle para não encher fila com pacotes repetidos
const unsigned long RX_028_MIN_GAP_MS = 3000;   // 1 pacote 0x28 a cada 3s por subtipo
const unsigned long RX_OTHER_MIN_GAP_MS = 1500;

// Medição: pulseira precisa de tempo com o comando ativo (PDF §33 — notify ~1/s)
const unsigned long HEALTH_MEASURE_WINDOW_MS = 12000;
const unsigned long HEALTH_HRV_WINDOW_MS = 18000;
const unsigned long HEALTH_BP_WINDOW_MS = 20000;
const unsigned long BLE_COLLECT_MAX_MS = 300UL * 1000UL;
const int HEALTH_MAX_RETRY_ROUNDS = 3;

// Tempo final esperando resposta dos históricos antes de desligar BLE
const unsigned long HISTORY_RESPONSE_WAIT_MS = 8000;

// Intervalo entre comandos de histórico
const unsigned long HISTORY_COMMAND_GAP_MS = 700;
 
 // Wi-Fi / Render
 const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
 const unsigned long WIFI_HTTPS_WARMUP_MS = 4000;
 const unsigned long RENDER_PING_INTERVAL_MS = 8000;
 const unsigned long HTTP_RETRY_INTERVAL_MS = 10000;
 const unsigned long HTTP_FAIL_RETRY_MS = 30000;
 const unsigned long HTTP_STUCK_RESTART_MS = 3UL * 60UL * 1000UL;
 const int RENDER_HEALTH_MAX_FAILS = 3;
 
 // ===================== FILA =====================
 
#define RAW_HEX_MAX_LEN 800
#define PACKET_TYPE_LEN 8
#define HTTP_QUEUE_SIZE 50
#define BATCH_MAX_PACKETS 20
#define BATCH_PAYLOAD_MAX_LEN 16000
 
 struct RawPacketItem {
   bool used;
   char packetType[PACKET_TYPE_LEN];
   char rawHex[RAW_HEX_MAX_LEN];
   unsigned long receivedAtMs;
   unsigned long receivedAtUptimeMs;
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
  HEALTH_START_BP,
  HEALTH_STOP_BP,
  HEALTH_DONE
};

struct CycleVitalsStatus {
  bool hr;
  bool spo2;
  bool temp;
  bool hrv;
  bool bp;
  uint8_t retryRound;
};

CycleVitalsStatus cycleVitals = {};
bool cycleReadyForUpload = false;

 HealthCommandState healthState = HEALTH_IDLE;
 unsigned long healthStateStartedAt = 0;
 
 int historyIndex = 0;
 unsigned long lastHistoryCommandAt = 0;
 bool historyCommandsFinished = false;
 
 unsigned long lastWifiTryAt = 0;
 unsigned long lastRenderPingAt = 0;
 unsigned long lastHttpTryAt = 0;
 int renderHealthFailCount = 0;
 int httpBatchFailCount = 0;
 unsigned long httpStuckSince = 0;
 const int HTTP_BATCH_MAX_FAILS = 2;
 
 // ===================== UTILS =====================

 void initClockTimezone() {
   configTime(TIMEZONE_OFFSET_SEC, TIMEZONE_DST_SEC, nullptr, nullptr);
 }

 uint8_t byteToDec(uint8_t value, bool bcd) {
   if (!bcd) return value;
   return ((value >> 4) & 0x0F) * 10 + (value & 0x0F);
 }

 bool isValidEpoch(time_t epoch) {
   return epoch >= MIN_VALID_EPOCH_SEC && epoch < 2000000000;
 }

 bool isValidDateTime(const struct tm& t) {
   int year = t.tm_year + 1900;
   if (year < 2024 || year > 2027) return false;
   if (t.tm_mon < 0 || t.tm_mon > 11) return false;
   if (t.tm_mday < 1 || t.tm_mday > 31) return false;
   if (t.tm_hour < 0 || t.tm_hour > 23) return false;
   if (t.tm_min < 0 || t.tm_min > 59) return false;
   if (t.tm_sec < 0 || t.tm_sec > 59) return false;
   return true;
 }

 bool applySystemTime(struct tm& t, const char* source) {
   if (!isValidDateTime(t)) return false;

   time_t epoch = mktime(&t);
   if (epoch <= 0 || !isValidEpoch(epoch)) return false;

   struct timeval tv = { epoch, 0 };
   settimeofday(&tv, nullptr);
   clockSynced = true;

   char timeBuf[32];
   strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &t);
   logPrintf("Relógio ajustado via %s: %s\n", source, timeBuf);
   return true;
 }

 uint64_t nowEpochMs() {
   struct timeval tv;
   if (gettimeofday(&tv, nullptr) != 0) return 0;
   if (!isValidEpoch(tv.tv_sec)) return 0;
   return ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000);
 }

 uint64_t resolvePacketEpochMs(unsigned long storedEpochMs, unsigned long receivedAtUptimeMs) {
   if (storedEpochMs >= MIN_VALID_EPOCH_MS) {
     return storedEpochMs;
   }

   uint64_t epochNow = nowEpochMs();
   if (epochNow == 0) return 0;

   unsigned long batchUptime = millis();
   if (batchUptime >= receivedAtUptimeMs) {
     unsigned long ageMs = batchUptime - receivedAtUptimeMs;
     if (epochNow > ageMs) {
       return epochNow - ageMs;
     }
   }

   return epochNow;
 }

 void formatLogTime(char* out, size_t outSize) {
   if (!out || outSize == 0) return;

   struct timeval tv;
   if (gettimeofday(&tv, nullptr) == 0 && isValidEpoch(tv.tv_sec)) {
     struct tm timeinfo;
     localtime_r(&tv.tv_sec, &timeinfo);
     strftime(out, outSize, "%Y-%m-%d %H:%M:%S", &timeinfo);
     return;
   }

   strncpy(out, "----/--/-- --:--:--", outSize);
   out[outSize - 1] = '\0';
 }

 void logPrefix() {
   char timeBuf[32];
   formatLogTime(timeBuf, sizeof(timeBuf));
   Serial.printf("[%s] ", timeBuf);
 }

 void logPrint(const char* msg) {
   logPrefix();
   Serial.print(msg);
 }

 void logPrintln() {
   logPrefix();
   Serial.println();
 }

 void logPrintln(const char* msg) {
   logPrefix();
   Serial.println(msg);
 }

 void logPrintln(unsigned long value) {
   logPrefix();
   Serial.println(value);
 }

 void logPrintf(const char* fmt, ...) {
   logPrefix();

   char buffer[320];
   va_list args;
   va_start(args, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, args);
   va_end(args);

   Serial.print(buffer);
 }

 bool syncTimeFromNtp() {
   if (WiFi.status() != WL_CONNECTED) return false;

   time_t before = time(nullptr);
   configTime(TIMEZONE_OFFSET_SEC, TIMEZONE_DST_SEC, "pool.ntp.org", "time.nist.gov");

   for (int i = 0; i < 40; i++) {
     delay(250);

     time_t now = time(nullptr);
     if (!isValidEpoch(now)) continue;

     if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
       clockSynced = true;
       struct tm timeinfo;
       localtime_r(&now, &timeinfo);
       char timeBuf[32];
       strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
       logPrintf("Relógio sincronizado via NTP: %s\n", timeBuf);
       return true;
     }

     if (before < MIN_VALID_EPOCH_SEC && now >= MIN_VALID_EPOCH_SEC) {
       clockSynced = true;
       struct tm timeinfo;
       localtime_r(&now, &timeinfo);
       char timeBuf[32];
       strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
       logPrintf("Relógio sincronizado via NTP: %s\n", timeBuf);
       return true;
     }
   }

   logPrintln("[APP] NTP não confirmou sincronização.");
   return false;
 }

 bool tryParseBraceletTime(const uint8_t* data, size_t len, int offset, bool bcd, struct tm& out) {
   if (!data || len < (size_t)(offset + 6)) return false;

   memset(&out, 0, sizeof(out));

   int year = byteToDec(data[offset], bcd);
   if (year < 100) {
     year += 2000;
   }

   out.tm_year = year - 1900;
   out.tm_mon = byteToDec(data[offset + 1], bcd) - 1;
   out.tm_mday = byteToDec(data[offset + 2], bcd);
   out.tm_hour = byteToDec(data[offset + 3], bcd);
   out.tm_min = byteToDec(data[offset + 4], bcd);
   out.tm_sec = byteToDec(data[offset + 5], bcd);

   return isValidDateTime(out);
 }

 void updateClockFromBraceletPacket(const uint8_t* data, size_t len) {
   if (!data || len < 7 || data[0] != 0x41) return;
   if (clockSynced) return;

   char rawHex[80];
   rawToHex(data, len < 16 ? len : 16, rawHex, sizeof(rawHex));

   // PDF §2: 0x41 AA BB CC DD EE FF ... com data/hora em BCD.
   struct tm parsed = {};
   if (!tryParseBraceletTime(data, len, 1, true, parsed)) {
     logPrintf("[APP] 0x41 ignorado (relógio da pulseira inválido): %s\n", rawHex);
     return;
   }

   applySystemTime(parsed, "pulseira 0x41");
 }

 void changeState(MainState next) {
   mainState = next;
   stateStartedAt = millis();

   if (next == STATE_RENDER_WAKE) {
     lastRenderPingAt = 0;
   }

   if (next == STATE_HTTP_SEND_BATCH) {
     lastHttpTryAt = 0;
   }

#if LOG_STATE
  switch (next) {
    case STATE_BLE_START: logPrintf("[STATE] -> BLE_START\n"); break;
    case STATE_BLE_COLLECT: logPrintf("[STATE] -> BLE_COLLECT\n"); break;
    case STATE_BLE_QUERY_HISTORY: logPrintf("[STATE] -> BLE_QUERY_HISTORY\n"); break;
    case STATE_BLE_SHUTDOWN: logPrintf("[STATE] -> BLE_SHUTDOWN\n"); break;
    case STATE_WIFI_START: logPrintf("[STATE] -> WIFI_START\n"); break;
    case STATE_RENDER_WAKE: logPrintf("[STATE] -> RENDER_WAKE\n"); break;
    case STATE_HTTP_SEND_BATCH: logPrintf("[STATE] -> HTTP_SEND_BATCH\n"); break;
    case STATE_WIFI_SHUTDOWN: logPrintf("[STATE] -> WIFI_SHUTDOWN\n"); break;
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

// Aceita só sinais vitais, sono e metadados do dispositivo.
// Ignora atividade: 0x09, 0x18, 0x51, 0x52, 0x5C (passos, calorias, exercícios).
bool isWantedPacketType(uint8_t type) {
  switch (type) {
    case 0x28:  // medição ativa (HR, SpO2, temp, HRV)
    case 0x53:  // sono
    case 0x54:  // histórico HR
    case 0x55:  // histórico HR pontual
    case 0x56:  // histórico HRV
    case 0x60:  // histórico SpO2 manual
    case 0x62:  // histórico temperatura manual
    case 0x65:  // histórico temperatura auto
    case 0x66:  // histórico SpO2 auto
    case 0x13:  // bateria
    case 0x22:  // MAC
    case 0x27:  // firmware
    case 0x41:  // relógio
      return true;
    default:
      return false;
  }
}

bool isBleReady() {
   return bleClient && bleClient->isConnected() && txChar && rxChar && bleConnected;
 }

bool isPlausibleBp(uint8_t systolic, uint8_t diastolic) {
  if (systolic == 0x7b && diastolic == 0x49) return false;
  if (systolic == 0x75 && diastolic == 0x44) return false;
  return systolic > 0 && diastolic > 0 && systolic < 250 && diastolic < 200 && systolic > diastolic;
}

bool hasMeaningfulTemp028(const uint8_t* data) {
  uint16_t tempBe = ((uint16_t)data[8] << 8) | data[9];
  uint16_t tempLe = ((uint16_t)data[9] << 8) | data[8];
  return tempBe > 0 || tempLe > 0;
}

void resetCycleVitals() {
  cycleVitals = {};
  cycleReadyForUpload = false;
}

void recordCycleVitalFromNotify(const uint8_t* data, size_t len) {
  if (!data || len < 2) return;

  if (data[0] == 0x28 && len >= 10) {
    switch (data[1]) {
      case 0x02:
        if (data[2] > 0) cycleVitals.hr = true;
        break;
      case 0x03:
        if (data[3] > 0) cycleVitals.spo2 = true;
        break;
      case 0x04:
        if (hasMeaningfulTemp028(data)) cycleVitals.temp = true;
        break;
      case 0x01:
        if (data[4] > 0) cycleVitals.hrv = true;
        break;
      case 0x05:
        if (isPlausibleBp(data[6], data[7])) cycleVitals.bp = true;
        break;
      default:
        break;
    }
    if (hasMeaningfulTemp028(data)) cycleVitals.temp = true;
    if (isPlausibleBp(data[6], data[7])) cycleVitals.bp = true;
    return;
  }

  if (data[0] == 0x56 && len >= 13) {
    size_t recordLen = (len % 16 == 0) ? 16 : 15;
    size_t start = len >= recordLen ? len - recordLen : 0;
    if (data[start + 9] > 0) cycleVitals.hrv = true;
  }
}

bool cycleVitalsComplete() {
  return cycleVitals.hr && cycleVitals.spo2 && cycleVitals.temp
      && cycleVitals.hrv && cycleVitals.bp;
}

void logCycleVitalsStatus(const char* prefix) {
  logPrintf(
    "%s HR=%s SpO2=%s Temp=%s HRV=%s BP=%s (rodada %u)\n",
    prefix,
    cycleVitals.hr ? "OK" : "--",
    cycleVitals.spo2 ? "OK" : "--",
    cycleVitals.temp ? "OK" : "--",
    cycleVitals.hrv ? "OK" : "--",
    cycleVitals.bp ? "OK" : "--",
    cycleVitals.retryRound
  );
}

HealthCommandState firstMissingHealthState() {
  if (!cycleVitals.hr) return HEALTH_START_HR;
  if (!cycleVitals.spo2) return HEALTH_START_SPO2;
  if (!cycleVitals.temp) return HEALTH_START_TEMP;
  if (!cycleVitals.hrv) return HEALTH_START_HRV;
  if (!cycleVitals.bp) return HEALTH_START_BP;
  return HEALTH_DONE;
}

unsigned long healthWindowForState(HealthCommandState state) {
  switch (state) {
    case HEALTH_STOP_HRV:
      return HEALTH_HRV_WINDOW_MS;
    case HEALTH_STOP_BP:
      return HEALTH_BP_WINDOW_MS;
    default:
      return HEALTH_MEASURE_WINDOW_MS;
  }
}

bool hasMeaningful028Data(const uint8_t* data, size_t len) {
  if (!data || len < 10 || data[0] != 0x28) return false;

  // PDF §33: [2]=HR, [3]=SpO2, [4]=HRV, [5]=fadiga, [8..9]=temp (/10)
  switch (data[1]) {
    case 0x01:
      return data[4] > 0 || data[5] > 0;
    case 0x02:
      return data[2] > 0;
    case 0x03:
      return data[3] > 0;
    case 0x04:
      return hasMeaningfulTemp028(data);
    case 0x05:
      return isPlausibleBp(data[6], data[7]);
    default:
      return data[2] > 0 || data[3] > 0 || data[4] > 0 || data[5] > 0
          || hasMeaningfulTemp028(data) || isPlausibleBp(data[6], data[7]);
  }
}
 
 // ===================== FILA =====================
 
void enqueueRawPacket(const char* packetType, const char* rawHex) {
  if (!packetType || !rawHex) return;

  if (queueCount >= HTTP_QUEUE_SIZE) {
#if LOG_QUEUE
    logPrintln("[QUEUE] Limite 50 atingido. Ignorando novo pacote até enviar batch.");
#endif
    return;
  }

  RawPacketItem& item = httpQueue[queueCount];
  item.used = true;

  strncpy(item.packetType, packetType, PACKET_TYPE_LEN - 1);
  item.packetType[PACKET_TYPE_LEN - 1] = '\0';

  strncpy(item.rawHex, rawHex, RAW_HEX_MAX_LEN - 1);
  item.rawHex[RAW_HEX_MAX_LEN - 1] = '\0';

  item.receivedAtUptimeMs = millis();

  uint64_t epochMs = nowEpochMs();
  item.receivedAtMs = epochMs > 0 ? (unsigned long)epochMs : 0;

  queueCount++;

#if LOG_QUEUE
  logPrintf("[QUEUE] + %s | fila=%d/%d\n", packetType, queueCount, HTTP_QUEUE_SIZE);
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

  logPrintln("[WIFI] Desligando Wi-Fi...");

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
   logPrintln("[WIFI] Ligando/conectando...");
 #endif
 
   WiFi.mode(WIFI_STA);
   WiFi.setSleep(false);
   WiFi.setAutoReconnect(true);
   WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE,
               IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
   WiFi.begin(WIFI_SSID, WIFI_PASS);
 
   unsigned long start = millis();
 
   while (millis() - start < 12000) {
     if (WiFi.status() == WL_CONNECTED) {
       logPrintf("[WIFI] Conectado. IP=%s RSSI=%d heap=%u\n",
                 WiFi.localIP().toString().c_str(),
                 WiFi.RSSI(),
                 ESP.getFreeHeap());
       syncTimeFromNtp();
       return true;
     }
 
     delay(250);
   }
 
   logPrintf("[WIFI] Ainda não conectou. Status=%d\n", WiFi.status());
   return false;
 }
 
 // ===================== HTTP =====================

 void configureSecureClient(WiFiClientSecure& client) {
   client.setInsecure();
   client.setHandshakeTimeout(45);
   client.setTimeout(45000);
 }

 bool resolveApiHost(IPAddress& outIp) {
   if (WiFi.hostByName(API_HOST, outIp)) {
     logPrintf("[HTTP] DNS OK -> %s\n", outIp.toString().c_str());
     return true;
   }

   logPrintln("[HTTP] DNS falhou para host da API.");
   return false;
 }

 void logHttpResult(const char* label, int code, HTTPClient& http) {
   if (code < 0) {
     logPrintf("[HTTP] %s -> %d (%s) heap=%u\n",
               label,
               code,
               http.errorToString(code).c_str(),
               ESP.getFreeHeap());
   } else {
     logPrintf("[HTTP] %s -> %d heap=%u\n", label, code, ESP.getFreeHeap());
   }
 }

 bool pingRenderHealth() {
   if (WiFi.status() != WL_CONNECTED) return false;

   IPAddress apiIp;
   if (!resolveApiHost(apiIp)) {
     return false;
   }

   WiFiClientSecure client;
   configureSecureClient(client);

   HTTPClient http;
   http.setReuse(false);
   http.setTimeout(45000);
   http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

 #if LOG_HTTP
   logPrintf("[HTTP] GET /health... heap=%u\n", ESP.getFreeHeap());
 #endif

   if (!http.begin(client, API_HOST, 443, "/health", true)) {
 #if LOG_HTTP
     logPrintln("[HTTP] begin /health falhou.");
 #endif
     return false;
   }

   http.addHeader("Connection", "close");
   http.addHeader("User-Agent", "ESP32-2208A-Gateway");

   int code = http.GET();
   logHttpResult("/health", code, http);
   http.end();

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
    char itemJson[2800];
    uint64_t packetEpochMs = resolvePacketEpochMs(
      httpQueue[i].receivedAtMs,
      httpQueue[i].receivedAtUptimeMs
    );

    if (packetEpochMs == 0) {
      logPrintf("[HTTP] Sem epoch válido para %s. Abortando batch.\n", httpQueue[i].packetType);
      return false;
    }

    snprintf(
      itemJson,
      sizeof(itemJson),
      "%s{\"packetType\":\"%s\",\"rawHex\":\"%s\",\"receivedAtMs\":%llu}",
      itemsInBatch > 0 ? "," : "",
      httpQueue[i].packetType,
      httpQueue[i].rawHex,
      (unsigned long long)packetEpochMs
    );

    if (strlen(batchPayload) + strlen(itemJson) + 4 >= BATCH_PAYLOAD_MAX_LEN) {
      break;
    }

    strcat(batchPayload, itemJson);
    itemsInBatch++;
  }

  if (itemsInBatch <= 0) return false;

  strcat(batchPayload, "]}");

#if LOG_HTTP
  logPrintf("[HTTP] Batch timestamps OK | epoch agora=%llu\n",
            (unsigned long long)nowEpochMs());
#endif

  return true;
}
 
bool postBatchToApi() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (queueCount <= 0) return true;

  if (!clockSynced) {
    syncTimeFromNtp();
  }

  if (!clockSynced || nowEpochMs() == 0) {
    logPrintln("[HTTP] Sem relógio válido para timestamps do batch.");
    return false;
  }

  int itemsInBatch = 0;

  if (!buildBatchPayload(itemsInBatch)) {
    logPrintln("[HTTP] Falha ao montar batch payload.");
    return false;
  }

  IPAddress apiIp;
  if (!resolveApiHost(apiIp)) {
    return false;
  }

  WiFiClientSecure client;
  configureSecureClient(client);

  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(45000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

#if LOG_HTTP
  logPrintf("[HTTP] POST batch | itens=%d | fila=%d heap=%u\n",
            itemsInBatch, queueCount, ESP.getFreeHeap());
#endif

  if (!http.begin(client, API_HOST, 443, API_BATCH_PATH, true)) {
#if LOG_HTTP
    logPrintln("[HTTP] begin batch falhou.");
#endif
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  http.addHeader("User-Agent", "ESP32-2208A-Gateway");

  int code = http.POST((uint8_t*)batchPayload, strlen(batchPayload));

  logHttpResult("batch", code, http);

  if (code > 0) {
    String res = http.getString();
#if LOG_HTTP
    logPrintf("[HTTP] response size=%u\n", res.length());
#endif
  }

  http.end();

  if (code == 200 || code == 201 || code == 202) {
    removeQueueItems(itemsInBatch);
    logPrintf("[HTTP] Batch OK. Removidos=%d | fila=%d\n", itemsInBatch, queueCount);
    return true;
  }

  logPrintln("[HTTP] Batch falhou. Mantendo fila.");
  return false;
}
 
 // ===================== BLE CALLBACKS =====================
 
 class BraceletCallbacks : public BLEClientCallbacks {
   void onConnect(BLEClient*) override {
     bleConnected = true;
     logPrintln("[BLE] Callback conectado.");
   }
 
   void onDisconnect(BLEClient*) override {
     bleConnected = false;
     txChar = nullptr;
     rxChar = nullptr;
     logPrintln("[BLE] Callback desconectado.");
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

  uint8_t type = data[0];

  if (type == 0x41) {
    updateClockFromBraceletPacket(data, len);
  }

  if (type == 0x28 || type == 0x56) {
    recordCycleVitalFromNotify(data, len);
  }

  if (!isWantedPacketType(type)) {
#if LOG_BLE_RX
    logPrintf("[BLE RX] 0x%02X ignorado (atividade/outro)\n", type);
#endif
    return;
  }

  unsigned long now = millis();

  static unsigned long last028BySubtype[8] = {0};
  static unsigned long lastOtherByType[256] = {0};

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

  // Limita duplicidade de outros pacotes, tipo 0x13, 0x22, 0x27, 0x41
  else {
    if (now - lastOtherByType[type] < RX_OTHER_MIN_GAP_MS) {
      return;
    }
    lastOtherByType[type] = now;
  }

  if (type == 0x28 && !hasMeaningful028Data(data, len)) {
#if LOG_BLE_RX
    logPrintf("[BLE RX] 0x28 ignorado AA=%02X BB=%02X CC=%02X [8..9]=%02X%02X\n",
              data[1], len > 2 ? data[2] : 0, len > 3 ? data[3] : 0,
              len > 8 ? data[8] : 0, len > 9 ? data[9] : 0);
#endif
    return;
  }

  if (queueCount >= HTTP_QUEUE_SIZE) {
#if LOG_QUEUE
    logPrintln("[QUEUE] Cheia. RX ignorado.");
#endif
    return;
  }

  char rawHex[RAW_HEX_MAX_LEN];
  char packetType[PACKET_TYPE_LEN];

  rawToHex(data, len, rawHex, sizeof(rawHex));
  packetTypeToString(type, packetType, sizeof(packetType));

#if LOG_BLE_RX
  logPrintf("[BLE RX] %s len=%u | fila=%d/%d\n", packetType, len, queueCount, HTTP_QUEUE_SIZE);
#endif

  enqueueRawPacket(packetType, rawHex);
}
 
 // ===================== BLE =====================
 
bool startBle() {
  if (bleInitialized) {
    return true;
  }

  logPrintln("[BLE] Inicializando BLE...");
  BLEDevice::init("ESP32_2208A_GATEWAY");
  bleInitialized = true;
  delay(500);

  return true;
}

void blePauseForWifi() {
  logPrintln("[BLE] Pausando BLE para usar Wi-Fi...");

  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
    delay(500);
  }

  bleConnected = false;
  txChar = nullptr;
  rxChar = nullptr;
  bleClient = nullptr;

  if (bleInitialized) {
    BLEDevice::deinit(false);
    bleInitialized = false;
    delay(1200);
    logPrintf("[BLE] Stack BLE pausada. heap=%u\n", ESP.getFreeHeap());
  }
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
    logPrintln("[BLE] Criando BLE client...");
    bleClient = BLEDevice::createClient();

    if (!bleClient) {
      logPrintln("[BLE] Falha ao criar BLE client. Tentando resetar stack BLE uma vez...");

      BLEDevice::deinit(true);
      bleInitialized = false;
      delay(1500);

      BLEDevice::init("ESP32_2208A_GATEWAY");
      bleInitialized = true;
      delay(800);

      bleClient = BLEDevice::createClient();

      if (!bleClient) {
        logPrintln("[BLE] Falha definitiva ao criar BLE client.");
        return false;
      }
    }

    bleClient->setClientCallbacks(&braceletCallbacks);
  }

  logPrintf("[BLE] Conectando na pulseira %s...\n", DEVICE_MAC);

  BLEAddress address(DEVICE_MAC);

  bool connected = bleClient->connect(address, BLE_ADDR_TYPE_RANDOM);

  if (!connected) {
    logPrintln("[BLE] Falhou RANDOM. Tentando PUBLIC...");
    delay(500);
    connected = bleClient->connect(address, BLE_ADDR_TYPE_PUBLIC);
  }

  if (!connected) {
    logPrintln("[BLE] Falha ao conectar na pulseira.");
    bleConnected = false;
    txChar = nullptr;
    rxChar = nullptr;
    return false;
  }

  logPrintln("[BLE] Conectado. Procurando serviço FFF0...");

  BLERemoteService* service = bleClient->getService(SERVICE_UUID);
  if (!service) {
    logPrintln("[BLE] Serviço FFF0 não encontrado.");
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
    logPrintln("[BLE] Características FFF6/FFF7 não encontradas.");
    bleClient->disconnect();
    delay(500);
    bleConnected = false;
    txChar = nullptr;
    rxChar = nullptr;
    return false;
  }

  if (rxChar->canNotify()) {
    rxChar->registerForNotify(notifyCallback);
    logPrintln("[BLE] Notify registrado em RX FFF7.");
  } else {
    logPrintln("[BLE] RX FFF7 não suporta notify.");
  }

  bleConnected = true;

  readDeviceTime();
  unsigned long waitClock = millis();
  while (!clockSynced && millis() - waitClock < 3000) {
    delay(50);
  }

  logPrintln("[BLE] Pulseira pronta.");

  return true;
}
 
 bool ensureBraceletConnected() {
   if (isBleReady()) return true;
 
   unsigned long now = millis();
 
   if (now - lastBleReconnectAttempt < 10000) {
     return false;
   }
 
   lastBleReconnectAttempt = now;
 
   logPrintln("[BLE] Não conectado. Tentando reconectar...");
   return connectBracelet();
 }
 
 // ===================== COMANDOS BLE =====================
 
 void sendPacket16(uint8_t* pkt) {
   if (!isBleReady()) {
     logPrintln("[BLE TX] Não enviado: BLE não pronto.");
     return;
   }
 
   pkt[15] = calcCrc(pkt);
 
 #if LOG_BLE_TX
   char txHex[80];
   rawToHex(pkt, 16, txHex, sizeof(txHex));
   logPrintf("[BLE TX] %s\n", txHex);
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
   stopHealth(0x05);  // Pressão arterial
   delay(50);
 }
 
 // Tempo real 0x09 — desabilitado (passos, calorias, distância)
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

   if (!clockSynced) {
     readDeviceTime();

     unsigned long waitClock = millis();
     while (!clockSynced && millis() - waitClock < 3000) {
       delay(50);
     }
   }

   logPrintln("[APP] Comandos iniciais BLE...");

   readBattery();
   delay(300);

   readMacAddress();
   delay(300);

   readFirmware();
   delay(300);

  resetCycleVitals();
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
       logPrintln("[APP] Start HR");
       startHealth(0x02);
       healthState = HEALTH_STOP_HR;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_HR:
       if (now - healthStateStartedAt >= healthWindowForState(HEALTH_STOP_HR)) {
         logPrintln("[APP] Stop HR");
         stopHealth(0x02);
         healthState = HEALTH_START_SPO2;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_SPO2:
       logPrintln("[APP] Start SpO2");
       startHealth(0x03);
       healthState = HEALTH_STOP_SPO2;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_SPO2:
       if (now - healthStateStartedAt >= healthWindowForState(HEALTH_STOP_SPO2)) {
         logPrintln("[APP] Stop SpO2");
         stopHealth(0x03);
         healthState = HEALTH_START_TEMP;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_TEMP:
       logPrintln("[APP] Start Temp");
       startHealth(0x04);
       healthState = HEALTH_STOP_TEMP;
       healthStateStartedAt = now;
       break;
 
     case HEALTH_STOP_TEMP:
       if (now - healthStateStartedAt >= healthWindowForState(HEALTH_STOP_TEMP)) {
         logPrintln("[APP] Stop Temp");
         stopHealth(0x04);
         healthState = HEALTH_START_HRV;
         healthStateStartedAt = now;
       }
       break;
 
     case HEALTH_START_HRV:
       logPrintln("[APP] Start HRV");
       startHealth(0x01);
       healthState = HEALTH_STOP_HRV;
       healthStateStartedAt = now;
       break;
 
    case HEALTH_STOP_HRV:
      if (now - healthStateStartedAt >= healthWindowForState(HEALTH_STOP_HRV)) {
        logPrintln("[APP] Stop HRV");
        stopHealth(0x01);
        healthState = HEALTH_START_BP;
        healthStateStartedAt = now;
      }
      break;

    case HEALTH_START_BP:
      logPrintln("[APP] Start BP");
      startHealth(0x05);
      healthState = HEALTH_STOP_BP;
      healthStateStartedAt = now;
      break;

    case HEALTH_STOP_BP:
      if (now - healthStateStartedAt >= healthWindowForState(HEALTH_STOP_BP)) {
        logPrintln("[APP] Stop BP");
        stopHealth(0x05);
        logCycleVitalsStatus("[APP] Fim da rodada:");

        if (cycleVitalsComplete()) {
          logPrintln("[APP] Ciclo de saúde completo.");
          healthState = HEALTH_DONE;
        } else if (cycleVitals.retryRound < HEALTH_MAX_RETRY_ROUNDS) {
          cycleVitals.retryRound++;
          logPrintf("[APP] Vitais incompletos — retentativa %u/%d\n",
                    cycleVitals.retryRound, HEALTH_MAX_RETRY_ROUNDS);
          healthState = firstMissingHealthState();
        } else {
          logPrintln("[APP] Vitais incompletos após retentativas.");
          healthState = HEALTH_DONE;
        }
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
 
   logPrintf("[APP] Consultando histórico index=%d\n", historyIndex);
 
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
       logPrintln("[APP] Comandos de histórico finalizados. Aguardando respostas...");
       break;
   }
 
   historyIndex++;
 }
 
 // ===================== SETUP =====================
 
 void setup() {
   WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
 
   Serial.begin(115200);
   delay(1000);

   initClockTimezone();

   logPrintln();
   logPrintln("==========================================");
   logPrintln("ESP32 2208A BLE COLLECT -> WIFI BATCH API");
   logPrintln("==========================================");

   esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
   logPrintf("[APP] Heap livre no boot=%u\n", ESP.getFreeHeap());

   wifiOff();

   changeState(STATE_BLE_START);
 }
 
 // ===================== LOOP =====================
 
 void loop() {
   unsigned long now = millis();
 
   switch (mainState) {
    case STATE_BLE_START: {
      static bool wifiAlreadyOffForBle = false;

      if (queueCount > 0) {
        logPrintf("[APP] Fila pendente=%d. Priorizando envio HTTP.\n", queueCount);
        if (bleInitialized) {
          blePauseForWifi();
          delay(1500);
        } else {
          wifiOff();
          delay(800);
        }
        wifiAlreadyOffForBle = false;
        changeState(STATE_WIFI_START);
        break;
      }

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
        logPrintln("[BLE] Falha no start. Tentando novamente em 10s...");
        delay(10000);
      }

      break;
    }
 
     case STATE_BLE_COLLECT: {
       ensureBraceletConnected();
 
       if (isBleReady()) {
         processHealthScheduler();
       }

       if (healthState == HEALTH_DONE && cycleVitalsComplete()) {
         logPrintf("[APP] Vitais completos. Fila=%d — coletando histórico\n", queueCount);
         resetHistoryQuery();
         changeState(STATE_BLE_QUERY_HISTORY);
       } else if (now - stateStartedAt >= BLE_COLLECT_MAX_MS) {
         logPrintln("[APP] Timeout absoluto de coleta BLE.");
         logCycleVitalsStatus("[APP]");
         if (cycleVitalsComplete()) {
           resetHistoryQuery();
           changeState(STATE_BLE_QUERY_HISTORY);
         } else {
           logPrintln("[APP] Sem vitais completos — reiniciando ciclo BLE (sem WiFi).");
           resetCycleVitals();
           healthState = HEALTH_IDLE;
           stateStartedAt = now;
         }
       }
 
       break;
     }
 
     case STATE_BLE_QUERY_HISTORY: {
       ensureBraceletConnected();
 
       if (isBleReady()) {
         processHistoryQuery();
       }
 
       if (historyCommandsFinished && now - lastHistoryCommandAt >= HISTORY_RESPONSE_WAIT_MS) {
         logPrintf("[APP] Histórico coletado. Fila=%d\n", queueCount);
         cycleReadyForUpload = cycleVitalsComplete();
         if (!cycleReadyForUpload) {
           logCycleVitalsStatus("[APP] Bloqueando WiFi — faltam:");
         }
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

      cycleReadyForUpload = cycleVitalsComplete();
      if (!cycleReadyForUpload) {
        logCycleVitalsStatus("[APP] Upload cancelado — faltam:");
        logPrintln("[APP] Nova coleta BLE (sem WiFi).");
        delay(1000);
        resetCycleVitals();
        healthState = HEALTH_IDLE;
        changeState(STATE_BLE_START);
        break;
      }

      blePauseForWifi();
      delay(1500);
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

       if (now - stateStartedAt < WIFI_HTTPS_WARMUP_MS) {
         break;
       }

       if (now - lastRenderPingAt >= RENDER_PING_INTERVAL_MS) {
         lastRenderPingAt = now;

         bool ok = pingRenderHealth();

         if (ok) {
           renderHealthFailCount = 0;
           httpBatchFailCount = 0;
           logPrintln("[HTTP] Render acordado.");
           changeState(STATE_HTTP_SEND_BATCH);
         } else {
           renderHealthFailCount++;

           logPrintf("[HTTP] Render ainda não respondeu. Tentativas=%d/%d\n",
                     renderHealthFailCount,
                     RENDER_HEALTH_MAX_FAILS);

           if (renderHealthFailCount >= RENDER_HEALTH_MAX_FAILS) {
             logPrintln("[HTTP] /health falhou demais. Tentando batch direto uma vez.");
             changeState(STATE_HTTP_SEND_BATCH);
           }
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
         logPrintln("[HTTP] Fila vazia. Finalizando envio.");
         changeState(STATE_WIFI_SHUTDOWN);
         break;
       }

       if (now - lastHttpTryAt >= HTTP_RETRY_INTERVAL_MS) {
         lastHttpTryAt = now;

         bool ok = postBatchToApi();

         if (ok) {
           httpBatchFailCount = 0;
           httpStuckSince = 0;

           if (queueCount <= 0) {
             logPrintln("[HTTP] Todos os batches enviados.");
             changeState(STATE_WIFI_SHUTDOWN);
           }

           break;
         }

         httpBatchFailCount++;

         logPrintf("[HTTP] Batch falhou. Tentativas=%d/%d\n",
                   httpBatchFailCount,
                   HTTP_BATCH_MAX_FAILS);

         if (httpBatchFailCount >= HTTP_BATCH_MAX_FAILS) {
           if (httpStuckSince == 0) {
             httpStuckSince = now;
           }

           if (now - httpStuckSince >= HTTP_STUCK_RESTART_MS) {
             logPrintf("[HTTP] Sem API há %lus. Reiniciando ESP32 (fila=%d será perdida).\n",
                       (now - httpStuckSince) / 1000UL,
                       queueCount);
             delay(1000);
             ESP.restart();
           }

           logPrintf("[HTTP] Falhou demais. Fila=%d — retry HTTP em %lus (sem nova coleta BLE).\n",
                     queueCount,
                     HTTP_FAIL_RETRY_MS / 1000UL);
           httpBatchFailCount = 0;
           lastHttpTryAt = now - HTTP_RETRY_INTERVAL_MS + HTTP_FAIL_RETRY_MS;
           changeState(STATE_RENDER_WAKE);
         } else {
           changeState(STATE_RENDER_WAKE);
         }
       }

       break;
     }
 
    case STATE_WIFI_SHUTDOWN: {
      wifiOff();

      logPrintln("[APP] Ciclo completo. Voltando para BLE.");
      delay(1500);

      resetCycleVitals();
      healthState = HEALTH_IDLE;
      lastBleReconnectAttempt = 0;
      lastRenderPingAt = 0;
      lastHttpTryAt = 0;
      renderHealthFailCount = 0;
      httpBatchFailCount = 0;
      httpStuckSince = 0;

      changeState(STATE_BLE_START);
      break;
    }
   }
 
   delay(20);
 }