#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEAddress.h>

#include "esp_sleep.h"

// ============================================================
// CONFIG
// ============================================================

const char* WIFI_SSID = "IoTs";
const char* WIFI_PASS = "altana@iot";

const char* API_BASE_URL = "https://bracelet-pn7r.onrender.com";
const char* API_PATH = "/bracelets/packets";

const char* DEVICE_MAC = "e6:64:0d:30:d3:f9";

static BLEUUID SERVICE_UUID((uint16_t)0xFFF0);
static BLEUUID TX_UUID((uint16_t)0xFFF6); // ESP32 -> pulseira
static BLEUUID RX_UUID((uint16_t)0xFFF7); // pulseira -> ESP32

const char* PACKETS_FILE = "/packets.ndjson";
const char* VITALS_SNAPSHOT_FILE = "/vitals_snapshot.json";

// ============================================================
// BOOTS
// ============================================================
// bootStep 0 = configura automáticos no padrão do app JC Vital
// bootStep 1 = lê históricos automáticos + medições oportunistas e salva SNAPSHOT_VITALS
// bootStep 2 = envia somente o snapshot para API

RTC_DATA_ATTR int bootStep = 0;
RTC_DATA_ATTR uint32_t bootCounter = 0;
RTC_DATA_ATTR bool autoAlreadyConfigured = false;

// Guarda fingerprints dos últimos dados aceitos entre deep sleeps.
// Serve para evitar reenviar HRV/fadiga/pressão cacheados como se fossem medições novas.
RTC_DATA_ATTR uint32_t lastAcceptedHrvRawHash = 0;
RTC_DATA_ATTR uint32_t lastAcceptedBpRawHash = 0;
RTC_DATA_ATTR uint8_t lastAcceptedHrv = 0;
RTC_DATA_ATTR uint8_t lastAcceptedFatigue = 0;
RTC_DATA_ATTR uint8_t lastAcceptedSystolic = 0;
RTC_DATA_ATTR uint8_t lastAcceptedDiastolic = 0;
RTC_DATA_ATTR bool hasLastAcceptedHrv = false;
RTC_DATA_ATTR bool hasLastAcceptedFatigue = false;
RTC_DATA_ATTR bool hasLastAcceptedBp = false;

// ============================================================
// MODO RÁPIDO PARA TESTE
// ============================================================
// true  = ciclo ~3–4 min, medições ativas sempre, sem suprimir repetidos
// false = produção (~15 min entre coletas, lógica app-like 2x/hora)

const bool FAST_TEST_MODE = true;

// Intervalo entre snapshots enviados para a API.
const uint64_t COLLECT_SLEEP_SECONDS = FAST_TEST_MODE ? 180ULL : 900ULL;
const uint64_t BOOT0_FIRST_WAIT_SECONDS = FAST_TEST_MODE ? 90ULL : COLLECT_SLEEP_SECONDS;
const uint64_t SHORT_SLEEP_SECONDS = FAST_TEST_MODE ? 3ULL : 5ULL;

// Delays de leitura de histórico BLE (cada comando 0x54/0x56/…)
const unsigned long HIST_DELAY_MS = FAST_TEST_MODE ? 1800UL : 3500UL;
const unsigned long HIST_DELAY_LONG_MS = FAST_TEST_MODE ? 2200UL : 4500UL;
const unsigned long HIST_DELAY_SHORT_MS = FAST_TEST_MODE ? 1400UL : 3000UL;
const unsigned long STOP_ACTIVE_DELAY_MS = FAST_TEST_MODE ? 400UL : 1000UL;
const unsigned long GAP_BETWEEN_ACTIVE_MODES_MS = FAST_TEST_MODE ? 600UL : 1200UL;

// ============================================================
// ESTRATÉGIA DE MEDIÇÃO
// ============================================================

// LÓGICA APP-LIKE:
// - Configura a pulseira com os mínimos reais vistos no JC Vital.
// - Deixa a pulseira medir sozinha durante 30 minutos.
// - No BOOT 1, lê os históricos/últimos registros como o app faz.
// - Faz medições ativas só como complemento/fallback.
// - HRV/stress vem principalmente do 0x56. Pressão é oportunista.

const bool SEND_ONLY_SNAPSHOT_TO_API = true;
const bool SAVE_0X28_DEBUG_RAW = false;
const bool READ_HISTORY_DEBUG = false;
const bool SAVE_HISTORY_RAW_DEBUG = false;
const bool USE_HISTORY_AS_FALLBACK = true;

// Se true, reconfigura os automáticos sempre que passar pelo BOOT 0.
// Em produção deixe false: o app configura uma vez e deixa a pulseira trabalhar.
const bool FORCE_AUTO_CONFIG_EVERY_BOOT0 = false;

// Complementos ativos. O principal agora é histórico automático.
const bool USE_ACTIVE_HEART_BP_REFRESH = true;
const bool USE_ACTIVE_SPO2_IF_MISSING = true;
const bool USE_ACTIVE_TEMP_IF_MISSING = true;
const bool TRY_ACTIVE_HRV_DEBUG = true;

// Automático fica ligado para gerar histórico recente durante o sono.
// Isso ajuda SpO2 e principalmente HRV/fadiga, que podem demorar ou vir melhor pelo histórico.
const bool AUTO_CONFIG_ENABLED = true;
const bool AUTO_DISABLE_ON_BOOT0 = false;

// Mínimos observados no app JC Vital.
const uint16_t AUTO_BPM_INTERVAL_MIN = 15;
const uint16_t AUTO_SPO2_INTERVAL_MIN = 30;
const uint16_t AUTO_TEMP_INTERVAL_MIN = 15;
const uint16_t AUTO_HRV_INTERVAL_MIN = 15;

// Tempos das medições ativas.
const unsigned long ACTIVE_WARMUP_MS = FAST_TEST_MODE ? 2000UL : 5000UL;
const unsigned long ACTIVE_HEART_BP_MS = FAST_TEST_MODE ? 18000UL : 30000UL;
const unsigned long ACTIVE_SPO2_MS = FAST_TEST_MODE ? 35000UL : 90000UL;
const unsigned long ACTIVE_TEMP_MS = FAST_TEST_MODE ? 10000UL : 15000UL;
const unsigned long ACTIVE_HRV_MS = FAST_TEST_MODE ? 25000UL : 60000UL;

// Para não parar no primeiro pacote válido isolado.
const unsigned long ACTIVE_STABLE_AFTER_TARGET_MS = FAST_TEST_MODE ? 2000UL : 5000UL;
const uint8_t MIN_BPM_SAMPLES = FAST_TEST_MODE ? 1 : 2;
const uint8_t MIN_BP_SAMPLES = FAST_TEST_MODE ? 1 : 2;
const uint8_t MIN_SPO2_SAMPLES = FAST_TEST_MODE ? 1 : 2;
const uint8_t MIN_TEMP_SAMPLES = FAST_TEST_MODE ? 1 : 2;
const uint8_t MIN_HRV_SAMPLES = 1;
const uint8_t MIN_FATIGUE_SAMPLES = 1;

// Logs e debug
const unsigned long PRINT_028_EVERY_MS = 3000UL;
const unsigned long SAVE_028_EVERY_MS = 5000UL;

unsigned long lastPrint028 = 0;
unsigned long lastSave028 = 0;

// ============================================================
// SNAPSHOT
// ============================================================

struct VitalsSnapshot {
  bool hasBpm;
  uint8_t bpm;
  const char* bpmSource;
  uint32_t bpmAtMs;
  uint16_t bpmSamples;

  bool hasSpo2;
  uint8_t spo2;
  const char* spo2Source;
  uint32_t spo2AtMs;
  uint16_t spo2Samples;

  bool hasTemperature;
  float temperature;
  const char* temperatureSource;
  uint32_t temperatureAtMs;
  uint16_t temperatureSamples;

  bool hasHrv;
  uint8_t hrv;
  const char* hrvSource;
  uint32_t hrvAtMs;
  uint16_t hrvSamples;

  bool hasFatigue;
  uint8_t fatigue;
  const char* fatigueSource;
  uint32_t fatigueAtMs;
  uint16_t fatigueSamples;

  bool hasBloodPressure;
  uint8_t systolic;
  uint8_t diastolic;
  const char* bpSource;
  const char* bpQuality;
  uint32_t bpAtMs;
  uint16_t bpSamples;

  bool hasSleep;
  uint16_t sleepRecordId;
  char sleepDate[11];
  char sleepTime[9];
  uint16_t sleepMinutes;
  const char* sleepSource;
  uint16_t sleepSamples;

  uint32_t createdAtMs;
};

VitalsSnapshot snapshot;

// Fingerprints selecionados neste ciclo.
uint32_t selectedHrvRawHash = 0;
uint32_t selectedBpRawHash = 0;
uint32_t selectedPacket09RawHash = 0;

uint32_t measurementSessionId = 0;
uint32_t measurementSessionStartedAtMs = 0;
uint32_t currentActiveModeStartedAtMs = 0;
uint32_t currentActiveModeAcceptAfterMs = 0;
uint8_t currentActiveMode = 0;
const char* currentActiveModeLabel = "NONE";
bool captureFreshActiveEnabled = false;

// ============================================================
// BLE
// ============================================================

BLEClient* client = nullptr;
BLERemoteCharacteristic* txChar = nullptr;
BLERemoteCharacteristic* rxChar = nullptr;

bool bleReady = false;
bool captureHistoryEnabled = false;

// ============================================================
// AUTO CONFIG
// ============================================================

#define AUTO_MODE_OFF 0x00
#define AUTO_MODE_INTERVAL 0x02
#define AUTO_DAYS_ALL 0x7F

int autoMode[5] = { -1, -1, -1, -1, -1 };
int autoInterval[5] = { -1, -1, -1, -1, -1 };

// ============================================================
// UTILS
// ============================================================

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

uint32_t fnv1aHashString(const String& value) {
  uint32_t hash = 2166136261UL;

  for (size_t i = 0; i < value.length(); i++) {
    hash ^= (uint8_t)value[i];
    hash *= 16777619UL;
  }

  return hash;
}

void printHeap(const char* label) {
  Serial.print("[MEM] ");
  Serial.print(label);
  Serial.print(" freeHeap=");
  Serial.println(ESP.getFreeHeap());
}

const char* activeModeName(uint8_t mode) {
  if (mode == 0x01) return "HRV_FATIGUE";
  if (mode == 0x02) return "HEART_BP";
  if (mode == 0x03) return "SPO2";
  if (mode == 0x04) return "TEMP";
  return "UNKNOWN";
}

const char* activeModeSource(uint8_t mode) {
  if (mode == 0x01) return "0x28_ACTIVE_FRESH_HRV_FATIGUE";
  if (mode == 0x02) return "0x28_ACTIVE_FRESH_HEART_BP";
  if (mode == 0x03) return "0x28_ACTIVE_FRESH_SPO2";
  if (mode == 0x04) return "0x28_ACTIVE_FRESH_TEMP";
  return "0x28_ACTIVE_FRESH";
}

const char* measurementStrategyLabel() {
  return FAST_TEST_MODE ? "FAST_TEST_FULL" : "APP_LIKE_DEBUG_15MIN";
}

const char* apiSourceLabel() {
  return FAST_TEST_MODE ? "ESP32_FAST_TEST" : "ESP32_APP_LIKE_DEBUG_15MIN";
}

// ============================================================
// SLEEP
// ============================================================

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

// ============================================================
// SPIFFS
// ============================================================

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

  if (SPIFFS.exists(VITALS_SNAPSHOT_FILE)) {
    SPIFFS.remove(VITALS_SNAPSHOT_FILE);
  }

  File f = SPIFFS.open(PACKETS_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("[FS] Falha ao criar arquivo de pacotes.");
    return;
  }

  f.close();
  Serial.println("[FS] Arquivo de pacotes limpo.");
}

void appendRawPacketToFile(const char* packetType, const String& rawHex, uint32_t receivedAtMs) {
  if (SEND_ONLY_SNAPSHOT_TO_API) {
    return;
  }

  File f = SPIFFS.open(PACKETS_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("[FS] Falha ao abrir arquivo para append.");
    return;
  }

  f.print("{\"packetType\":\"");
  f.print(packetType);
  f.print("\",\"rawHex\":\"");
  f.print(rawHex);
  f.print("\",\"receivedAtMs\":");
  f.print(receivedAtMs);
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

  if (SEND_ONLY_SNAPSHOT_TO_API || !SAVE_0X28_DEBUG_RAW) {
    return;
  }

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

// ============================================================
// VALIDAÇÃO E SNAPSHOT
// ============================================================

void resetSnapshot() {
  snapshot.hasBpm = false;
  snapshot.bpm = 0;
  snapshot.bpmSource = "missing";
  snapshot.bpmAtMs = 0;
  snapshot.bpmSamples = 0;

  snapshot.hasSpo2 = false;
  snapshot.spo2 = 0;
  snapshot.spo2Source = "missing";
  snapshot.spo2AtMs = 0;
  snapshot.spo2Samples = 0;

  snapshot.hasTemperature = false;
  snapshot.temperature = 0;
  snapshot.temperatureSource = "missing";
  snapshot.temperatureAtMs = 0;
  snapshot.temperatureSamples = 0;

  snapshot.hasHrv = false;
  snapshot.hrv = 0;
  snapshot.hrvSource = "missing";
  snapshot.hrvAtMs = 0;
  snapshot.hrvSamples = 0;

  snapshot.hasFatigue = false;
  snapshot.fatigue = 0;
  snapshot.fatigueSource = "missing";
  snapshot.fatigueAtMs = 0;
  snapshot.fatigueSamples = 0;

  snapshot.hasBloodPressure = false;
  snapshot.systolic = 0;
  snapshot.diastolic = 0;
  snapshot.bpSource = "missing";
  snapshot.bpQuality = "missing";
  snapshot.bpAtMs = 0;
  snapshot.bpSamples = 0;

  snapshot.hasSleep = false;
  snapshot.sleepRecordId = 0;
  snapshot.sleepDate[0] = '\0';
  snapshot.sleepTime[0] = '\0';
  snapshot.sleepMinutes = 0;
  snapshot.sleepSource = "missing";
  snapshot.sleepSamples = 0;

  snapshot.createdAtMs = millis();

  selectedHrvRawHash = 0;
  selectedBpRawHash = 0;
  selectedPacket09RawHash = 0;
}

bool validBpm(uint8_t bpm) {
  return bpm >= 35 && bpm <= 220;
}

bool validSpo2(uint8_t spo2) {
  return spo2 >= 50 && spo2 <= 100;
}

bool validTemperature(float temp) {
  return temp >= 30.0 && temp <= 43.0;
}

bool validHrv(uint8_t hrv) {
  return hrv > 0 && hrv <= 200;
}

bool validFatigue(uint8_t fatigue) {
  return fatigue >= 1 && fatigue <= 100;
}

bool validBloodPressure(uint8_t sys, uint8_t dia) {
  if (sys < 70 || sys > 220) return false;
  if (dia < 40 || dia > 140) return false;
  if (sys <= dia) return false;
  if ((sys - dia) < 15) return false;
  return true;
}

bool validSleepMinutes(uint16_t minutes) {
  return minutes >= 5 && minutes <= 24 * 60;
}

uint8_t bcdToDec(uint8_t b) {
  return ((b >> 4) * 10) + (b & 0x0F);
}

float parseTempLittleEndian(uint8_t low, uint8_t high) {
  uint16_t raw = ((uint16_t)high << 8) | low;
  return raw / 10.0;
}

void updateSnapshotBpm(uint8_t bpm, const char* source) {
  if (!validBpm(bpm)) return;

  snapshot.hasBpm = true;
  snapshot.bpm = bpm;
  snapshot.bpmSource = source;
  snapshot.bpmAtMs = millis();
  snapshot.bpmSamples++;
}

void updateSnapshotSpo2(uint8_t spo2, const char* source) {
  if (!validSpo2(spo2)) return;

  snapshot.hasSpo2 = true;
  snapshot.spo2 = spo2;
  snapshot.spo2Source = source;
  snapshot.spo2AtMs = millis();
  snapshot.spo2Samples++;
}

void updateSnapshotTemperature(float temp, const char* source) {
  if (!validTemperature(temp)) return;

  snapshot.hasTemperature = true;
  snapshot.temperature = temp;
  snapshot.temperatureSource = source;
  snapshot.temperatureAtMs = millis();
  snapshot.temperatureSamples++;
}

void updateSnapshotHrv(uint8_t hrv, const char* source) {
  if (!validHrv(hrv)) return;

  snapshot.hasHrv = true;
  snapshot.hrv = hrv;
  snapshot.hrvSource = source;
  snapshot.hrvAtMs = millis();
  snapshot.hrvSamples++;
}

void updateSnapshotFatigue(uint8_t fatigue, const char* source) {
  if (!validFatigue(fatigue)) return;

  snapshot.hasFatigue = true;
  snapshot.fatigue = fatigue;
  snapshot.fatigueSource = source;
  snapshot.fatigueAtMs = millis();
  snapshot.fatigueSamples++;
}

void updateSnapshotBloodPressure(uint8_t sys, uint8_t dia, const char* source, const char* quality) {
  if (!validBloodPressure(sys, dia)) return;

  snapshot.hasBloodPressure = true;
  snapshot.systolic = sys;
  snapshot.diastolic = dia;
  snapshot.bpSource = source;
  snapshot.bpQuality = quality;
  snapshot.bpAtMs = millis();
  snapshot.bpSamples++;
}

void updateSnapshotSleep(uint16_t recordId, const char* date, const char* time, uint16_t sleepMinutes, const char* source) {
  if (!validSleepMinutes(sleepMinutes)) return;

  snapshot.hasSleep = true;
  snapshot.sleepRecordId = recordId;
  strncpy(snapshot.sleepDate, date, sizeof(snapshot.sleepDate) - 1);
  snapshot.sleepDate[sizeof(snapshot.sleepDate) - 1] = '\0';
  strncpy(snapshot.sleepTime, time, sizeof(snapshot.sleepTime) - 1);
  snapshot.sleepTime[sizeof(snapshot.sleepTime) - 1] = '\0';
  snapshot.sleepMinutes = sleepMinutes;
  snapshot.sleepSource = source;
  snapshot.sleepSamples++;
}

bool snapshotComplete() {
  return snapshot.hasBpm &&
         snapshot.hasSpo2 &&
         snapshot.hasTemperature &&
         snapshot.hasHrv &&
         snapshot.hasFatigue &&
         snapshot.hasBloodPressure;
}

void finalizeSnapshotFreshness() {
  // Em teste rápido, envia todos os valores coletados (sem nullar repetidos).
  if (FAST_TEST_MODE) {
    if (snapshot.hasHrv) {
      if (selectedHrvRawHash != 0) lastAcceptedHrvRawHash = selectedHrvRawHash;
      lastAcceptedHrv = snapshot.hrv;
      hasLastAcceptedHrv = true;
    }
    if (snapshot.hasFatigue) {
      lastAcceptedFatigue = snapshot.fatigue;
      hasLastAcceptedFatigue = true;
    }
    if (snapshot.hasBloodPressure) {
      if (selectedBpRawHash != 0) lastAcceptedBpRawHash = selectedBpRawHash;
      lastAcceptedSystolic = snapshot.systolic;
      lastAcceptedDiastolic = snapshot.diastolic;
      hasLastAcceptedBp = true;
    }
    return;
  }

  // Evita transformar valor cacheado/repetido em nova medição.
  // A API também deve fazer essa checagem, mas o INO já envia null quando detectar repetição clara.

  if (snapshot.hasHrv) {
    bool sameRaw = selectedHrvRawHash != 0 && selectedHrvRawHash == lastAcceptedHrvRawHash;
    bool sameValue = hasLastAcceptedHrv && snapshot.hrv == lastAcceptedHrv;

    if (sameRaw || sameValue) {
      Serial.println("[STALE] HRV repetido. Enviando como null neste snapshot.");
      snapshot.hasHrv = false;
      snapshot.hrv = 0;
      snapshot.hrvSource = sameRaw ? "stale_duplicate_raw" : "stale_duplicate_value";
      snapshot.hrvSamples = 0;
    } else {
      if (selectedHrvRawHash != 0) lastAcceptedHrvRawHash = selectedHrvRawHash;
      lastAcceptedHrv = snapshot.hrv;
      hasLastAcceptedHrv = true;
    }
  }

  if (snapshot.hasFatigue) {
    bool sameRaw = selectedHrvRawHash != 0 && selectedHrvRawHash == lastAcceptedHrvRawHash;
    bool sameValue = hasLastAcceptedFatigue && snapshot.fatigue == lastAcceptedFatigue;

    if (sameRaw || sameValue) {
      Serial.println("[STALE] Fadiga repetida. Enviando como null neste snapshot.");
      snapshot.hasFatigue = false;
      snapshot.fatigue = 0;
      snapshot.fatigueSource = sameRaw ? "stale_duplicate_raw" : "stale_duplicate_value";
      snapshot.fatigueSamples = 0;
    } else {
      if (selectedHrvRawHash != 0) lastAcceptedHrvRawHash = selectedHrvRawHash;
      lastAcceptedFatigue = snapshot.fatigue;
      hasLastAcceptedFatigue = true;
    }
  }

  if (snapshot.hasBloodPressure) {
    bool sameRaw = selectedBpRawHash != 0 && selectedBpRawHash == lastAcceptedBpRawHash;
    bool sameValue = hasLastAcceptedBp &&
                     snapshot.systolic == lastAcceptedSystolic &&
                     snapshot.diastolic == lastAcceptedDiastolic;

    if (sameRaw || sameValue) {
      Serial.println("[STALE] Pressão repetida. Enviando como null neste snapshot.");
      snapshot.hasBloodPressure = false;
      snapshot.systolic = 0;
      snapshot.diastolic = 0;
      snapshot.bpSource = sameRaw ? "stale_duplicate_raw" : "stale_duplicate_value";
      snapshot.bpQuality = "stale_duplicate";
      snapshot.bpSamples = 0;
    } else {
      lastAcceptedBpRawHash = selectedBpRawHash;
      lastAcceptedSystolic = snapshot.systolic;
      lastAcceptedDiastolic = snapshot.diastolic;
      hasLastAcceptedBp = true;
    }
  }
}

void printSnapshotDebug() {
  Serial.println("========== SNAPSHOT DEBUG ==========");
  Serial.print("sessionId=");
  Serial.println(measurementSessionId);
  Serial.print("strategy=");
  Serial.print(measurementStrategyLabel());
  Serial.print(" freshOnly=false complete=");
  Serial.println(snapshotComplete() ? "true" : "false");

  Serial.print("bpm=");
  if (snapshot.hasBpm) Serial.print(snapshot.bpm); else Serial.print("null");
  Serial.print(" source=");
  Serial.print(snapshot.bpmSource);
  Serial.print(" samples=");
  Serial.println(snapshot.bpmSamples);

  Serial.print("spo2=");
  if (snapshot.hasSpo2) Serial.print(snapshot.spo2); else Serial.print("null");
  Serial.print(" source=");
  Serial.print(snapshot.spo2Source);
  Serial.print(" samples=");
  Serial.println(snapshot.spo2Samples);

  Serial.print("temp=");
  if (snapshot.hasTemperature) Serial.print(snapshot.temperature, 1); else Serial.print("null");
  Serial.print(" source=");
  Serial.print(snapshot.temperatureSource);
  Serial.print(" samples=");
  Serial.println(snapshot.temperatureSamples);

  Serial.print("hrv=");
  if (snapshot.hasHrv) Serial.print(snapshot.hrv); else Serial.print("null");
  Serial.print(" source=");
  Serial.print(snapshot.hrvSource);
  Serial.print(" samples=");
  Serial.println(snapshot.hrvSamples);

  Serial.print("fatigue=");
  if (snapshot.hasFatigue) Serial.print(snapshot.fatigue); else Serial.print("null");
  Serial.print(" source=");
  Serial.print(snapshot.fatigueSource);
  Serial.print(" samples=");
  Serial.println(snapshot.fatigueSamples);

  Serial.print("bp=");
  if (snapshot.hasBloodPressure) {
    Serial.print(snapshot.systolic);
    Serial.print("/");
    Serial.print(snapshot.diastolic);
  } else {
    Serial.print("null");
  }
  Serial.print(" source=");
  Serial.print(snapshot.bpSource);
  Serial.print(" bp samples=");
  Serial.println(snapshot.bpSamples);

  Serial.print("sleep=");
  if (snapshot.hasSleep) {
    Serial.print(snapshot.sleepMinutes);
    Serial.print("min ");
    Serial.print(snapshot.sleepDate);
    Serial.print(" ");
    Serial.print(snapshot.sleepTime);
  } else {
    Serial.print("null");
  }
  Serial.print(" source=");
  Serial.println(snapshot.sleepSource);

  Serial.print("rawHash.hrvFatigue=");
  Serial.println(selectedHrvRawHash);
  Serial.print("rawHash.bloodPressure=");
  Serial.println(selectedBpRawHash);
  Serial.print("rawHash.packet09=");
  Serial.println(selectedPacket09RawHash);

  Serial.println("====================================");
}

void saveVitalsSnapshotToFile() {
  File f = SPIFFS.open(VITALS_SNAPSHOT_FILE, FILE_WRITE);

  if (!f) {
    Serial.println("[SNAPSHOT] Falha ao abrir arquivo de snapshot.");
    return;
  }

  f.print("{");
  f.print("\"packetType\":\"SNAPSHOT_VITALS\",");
  f.print("\"receivedAtMs\":");
  f.print(millis());
  f.print(",");

  f.print("\"bootCounter\":");
  f.print(bootCounter);
  f.print(",");

  f.print("\"measurementSessionId\":");
  f.print(measurementSessionId);
  f.print(",");

  f.print("\"measurementSessionStartedAtMs\":");
  f.print(measurementSessionStartedAtMs);
  f.print(",");

  f.print("\"measurementStrategy\":\"");
  f.print(measurementStrategyLabel());
  f.print("\",");
  f.print("\"testMode\":");
  f.print(FAST_TEST_MODE ? "true" : "false");
  f.print(",\"freshOnly\":false,");

  f.print("\"autoConfig\":{");
  f.print("\"heartRateMin\":"); f.print(AUTO_BPM_INTERVAL_MIN);
  f.print(",\"spo2Min\":"); f.print(AUTO_SPO2_INTERVAL_MIN);
  f.print(",\"temperatureMin\":"); f.print(AUTO_TEMP_INTERVAL_MIN);
  f.print(",\"hrvStressMin\":"); f.print(AUTO_HRV_INTERVAL_MIN);
  f.print("},");

  f.print("\"metrics\":{");

  f.print("\"bpm\":");
  if (snapshot.hasBpm) f.print(snapshot.bpm); else f.print("null");

  f.print(",\"spo2\":");
  if (snapshot.hasSpo2) f.print(snapshot.spo2); else f.print("null");

  f.print(",\"temperature\":");
  if (snapshot.hasTemperature) f.print(snapshot.temperature, 1); else f.print("null");

  f.print(",\"hrv\":");
  if (snapshot.hasHrv) f.print(snapshot.hrv); else f.print("null");

  f.print(",\"fatigue\":");
  if (snapshot.hasFatigue) f.print(snapshot.fatigue); else f.print("null");

  f.print(",\"bloodPressureSystolic\":");
  if (snapshot.hasBloodPressure) f.print(snapshot.systolic); else f.print("null");

  f.print(",\"bloodPressureDiastolic\":");
  if (snapshot.hasBloodPressure) f.print(snapshot.diastolic); else f.print("null");

  f.print(",\"sleepMinutes\":");
  if (snapshot.hasSleep) f.print(snapshot.sleepMinutes); else f.print("null");

  f.print(",\"sleepDate\":");
  if (snapshot.hasSleep) { f.print("\""); f.print(snapshot.sleepDate); f.print("\""); } else f.print("null");

  f.print(",\"sleepTime\":");
  if (snapshot.hasSleep) { f.print("\""); f.print(snapshot.sleepTime); f.print("\""); } else f.print("null");

  f.print(",\"sleepRecordId\":");
  if (snapshot.hasSleep) f.print(snapshot.sleepRecordId); else f.print("null");

  f.print("},");

  f.print("\"sources\":{");
  f.print("\"bpm\":\"");
  f.print(snapshot.bpmSource);
  f.print("\",");
  f.print("\"spo2\":\"");
  f.print(snapshot.spo2Source);
  f.print("\",");
  f.print("\"temperature\":\"");
  f.print(snapshot.temperatureSource);
  f.print("\",");
  f.print("\"hrv\":\"");
  f.print(snapshot.hrvSource);
  f.print("\",");
  f.print("\"fatigue\":\"");
  f.print(snapshot.fatigueSource);
  f.print("\",");
  f.print("\"bloodPressure\":\"");
  f.print(snapshot.bpSource);
  f.print("\",");
  f.print("\"sleep\":\"");
  f.print(snapshot.sleepSource);
  f.print("\"");
  f.print("},");

  f.print("\"sampleCounts\":{");
  f.print("\"bpm\":");
  f.print(snapshot.bpmSamples);
  f.print(",\"spo2\":");
  f.print(snapshot.spo2Samples);
  f.print(",\"temperature\":");
  f.print(snapshot.temperatureSamples);
  f.print(",\"hrv\":");
  f.print(snapshot.hrvSamples);
  f.print(",\"fatigue\":");
  f.print(snapshot.fatigueSamples);
  f.print(",\"bloodPressure\":");
  f.print(snapshot.bpSamples);
  f.print(",\"sleep\":");
  f.print(snapshot.sleepSamples);
  f.print("},");

  f.print("\"selectedRawHashes\":{");
  f.print("\"hrvFatigue\":");
  f.print(selectedHrvRawHash);
  f.print(",\"bloodPressure\":");
  f.print(selectedBpRawHash);
  f.print(",\"packet09\":");
  f.print(selectedPacket09RawHash);
  f.print("},");

  f.print("\"quality\":{");
  f.print("\"snapshotComplete\":");
  f.print(snapshotComplete() ? "true" : "false");
  f.print(",\"bloodPressure\":\"");
  f.print(snapshot.bpQuality);
  f.print("\",");
  f.print("\"freshOnly\":false");
  f.print("}");

  f.println("}");

  f.close();

  Serial.print("[SNAPSHOT] Salvo. complete=");
  Serial.println(snapshotComplete() ? "true" : "false");
}

void appendSnapshotToPacketsFile() {
  File src = SPIFFS.open(VITALS_SNAPSHOT_FILE, FILE_READ);
  if (!src) {
    Serial.println("[SNAPSHOT] Snapshot não encontrado para anexar.");
    return;
  }

  String line = src.readStringUntil('\n');
  line.trim();
  src.close();

  if (line.length() == 0) {
    Serial.println("[SNAPSHOT] Snapshot vazio.");
    return;
  }

  File dst = SPIFFS.open(PACKETS_FILE, FILE_APPEND);
  if (!dst) {
    Serial.println("[SNAPSHOT] Falha ao anexar snapshot no packets file.");
    return;
  }

  dst.println(line);
  dst.close();

  Serial.println("[SNAPSHOT] Snapshot anexado ao packets.ndjson.");
}

// ============================================================
// BLE SEND
// ============================================================

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

void startActive028(uint8_t mode) {
  // 0x28 mode 0x01 HRV/fadiga | 0x02 HR/BP | 0x03 SpO2 | 0x04 temp
  sendCmd(0x28, mode, 0x01);
}

void stopActive028(uint8_t mode) {
  sendCmd(0x28, mode, 0x00);
}

// ============================================================
// HISTÓRICOS OPCIONAIS
// ============================================================

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

void readHistorySleep() {
  sendCmd(0x53, 0x00, 0x00, 0x00);
}

// ============================================================
// AUTO CONFIG OPCIONAL
// ============================================================

void readAutoConfig(uint8_t type) {
  sendCmd(0x2B, type, 0x00, 0x00);
}

void setAutoConfigRaw(uint8_t type, uint8_t mode, uint16_t intervalMin) {
  uint8_t pkt[16] = {
    0x2A,
    mode,
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
  Serial.print(" modo=");
  Serial.print(mode);
  Serial.print(" intervalo=");
  Serial.print(intervalMin);
  Serial.println("min");

  sendRaw16(pkt);
}

void setAutoConfig(uint8_t type, uint16_t intervalMin) {
  setAutoConfigRaw(type, AUTO_MODE_INTERVAL, intervalMin);
}

void disableAutoConfig(uint8_t type) {
  setAutoConfigRaw(type, AUTO_MODE_OFF, 0);
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

  if (autoMode[type] == AUTO_MODE_INTERVAL && autoInterval[type] == intervalMin) {
    Serial.print("[AUTO] Tipo ");
    Serial.print(type);
    Serial.println(" já está correto.");
    return;
  }

  setAutoConfig(type, intervalMin);
  delay(1500);
}

void ensureAutoConfigs() {
  Serial.println("[AUTO] Conferindo configurações automáticas...");

  clearAutoStatus();

  ensureAutoConfigForType(0x01, AUTO_BPM_INTERVAL_MIN);
  delay(700);
  ensureAutoConfigForType(0x02, AUTO_SPO2_INTERVAL_MIN);
  delay(700);
  ensureAutoConfigForType(0x03, AUTO_TEMP_INTERVAL_MIN);
  delay(700);
  ensureAutoConfigForType(0x04, AUTO_HRV_INTERVAL_MIN);
  delay(700);

  Serial.println("[AUTO] Configurações automáticas verificadas.");
}

void disableAutoConfigs() {
  Serial.println("[AUTO] Desligando medições automáticas...");
  disableAutoConfig(0x01);
  delay(700);
  disableAutoConfig(0x02);
  delay(700);
  disableAutoConfig(0x03);
  delay(700);
  disableAutoConfig(0x04);
  delay(700);
  Serial.println("[AUTO] Automático desligado.");
}

// ============================================================
// PARSE 0x28 ATIVO
// ============================================================

void handleActivePacket028(uint8_t* data, size_t len, String raw) {
  if (len < 10) {
    Serial.println("[0x28] Pacote curto, ignorado.");
    return;
  }

  if (!captureFreshActiveEnabled) {
    return;
  }

  unsigned long now = millis();

  // Garante que o snapshot só aceite amostras depois do START ativo e depois do warmup.
  if (now < currentActiveModeAcceptAfterMs) {
    return;
  }

  uint8_t packetMode = data[1];
  uint8_t bpm = data[2];
  uint8_t spo2 = data[3];
  uint8_t hrv = data[4];
  uint8_t fadiga = data[5];
  uint8_t pressaoAlta = data[6];
  uint8_t pressaoBaixa = data[7];
  float temp = parseTempLittleEndian(data[8], data[9]);

  // Evita aceitar notificação atrasada de outro modo.
  // Ex.: iniciou SpO2, mas ainda chegou um 0x28 do modo HR/BP anterior.
  if (packetMode != currentActiveMode) {
    Serial.print("[0x28 ACTIVE] Ignorado por modo diferente. collecting=0x");
    Serial.print(currentActiveMode, HEX);
    Serial.print(" packetMode=0x");
    Serial.println(packetMode, HEX);
    return;
  }

  const char* source = activeModeSource(currentActiveMode);

  // Usa somente o campo esperado do modo ativo atual.
  // Isso evita que um valor antigo/cacheado de outro campo sobrescreva o snapshot.
  if (currentActiveMode == 0x02) {
    // HEART/BP: usa BPM e pressão. Pressão continua marcada como estimada.
    updateSnapshotBpm(bpm, source);

    if (validBloodPressure(pressaoAlta, pressaoBaixa)) {
      selectedBpRawHash = fnv1aHashString(raw);
      updateSnapshotBloodPressure(pressaoAlta, pressaoBaixa, source, "estimated");
    }
  } else if (currentActiveMode == 0x03) {
    // SpO2 ativo
    updateSnapshotSpo2(spo2, source);
  } else if (currentActiveMode == 0x04) {
    // Temperatura ativa
    updateSnapshotTemperature(temp, source);
  } else if (currentActiveMode == 0x01) {
    // HRV/fadiga ativo
    selectedHrvRawHash = fnv1aHashString(raw);
    updateSnapshotHrv(hrv, source);
    updateSnapshotFatigue(fadiga, source);
  }

  if (now - lastPrint028 >= PRINT_028_EVERY_MS) {
    lastPrint028 = now;

    Serial.print("[0x28 ACTIVE FRESH] collecting=");
    Serial.print(currentActiveModeLabel);
    Serial.print(" packetMode=0x");
    Serial.print(packetMode, HEX);
    Serial.print(" bpm=");
    Serial.print(bpm);
    Serial.print(" spo2=");
    Serial.print(spo2);
    Serial.print(" temp=");
    Serial.print(temp, 1);
    Serial.print(" hrv=");
    Serial.print(hrv);
    Serial.print(" fadiga=");
    Serial.print(fadiga);
    Serial.print(" bp=");
    Serial.print(pressaoAlta);
    Serial.print("/");
    Serial.print(pressaoBaixa);
    Serial.print(" bpValid=");
    Serial.print(validBloodPressure(pressaoAlta, pressaoBaixa) ? "true" : "false");
    Serial.print(" raw=");
    Serial.println(raw);
  }

  if (SAVE_0X28_DEBUG_RAW && now - lastSave028 >= SAVE_028_EVERY_MS) {
    lastSave028 = now;
    appendMetricsPacket028ToFile(raw, packetMode, bpm, spo2, hrv, fadiga, pressaoAlta, pressaoBaixa, temp, millis());
  }
}

// ============================================================
// PARSE HISTÓRICO OPCIONAL
// ============================================================

void handleHistoryPacket(uint8_t type, uint8_t* data, size_t len, String raw) {
  if (!USE_HISTORY_AS_FALLBACK) {
    return;
  }

  // Fallback só preenche campo ausente. Nunca sobrescreve medição ativa fresca.
  if (len < 10) {
    Serial.println("[HIST PARSE] Pacote curto.");
    return;
  }

  if (type == 0x54 && !snapshot.hasBpm) {
    for (int i = (int)len - 1; i >= 9; i--) {
      uint8_t bpm = data[i];
      if (validBpm(bpm)) {
        updateSnapshotBpm(bpm, "0x54_AUTO_HISTORY_FALLBACK");
        break;
      }
    }
    return;
  }

  if (type == 0x56 && (!snapshot.hasHrv || !snapshot.hasFatigue)) {
    bool found = false;

    for (int offset = 0; offset + 12 < (int)len; offset += 15) {
      if (data[offset] != 0x56) continue;

      uint8_t hrv = data[offset + 9];
      uint8_t fatigue = data[offset + 12];

      if (validHrv(hrv) && validFatigue(fatigue)) {
        selectedHrvRawHash = fnv1aHashString(raw);
        if (!snapshot.hasHrv) updateSnapshotHrv(hrv, "0x56_AUTO_HISTORY_FALLBACK");
        if (!snapshot.hasFatigue) updateSnapshotFatigue(fatigue, "0x56_AUTO_HISTORY_FALLBACK");
        found = true;
      }
    }

    if (!found && len >= 13) {
      uint8_t hrv = data[9];
      uint8_t fatigue = data[12];
      if (validHrv(hrv) && validFatigue(fatigue)) {
        selectedHrvRawHash = fnv1aHashString(raw);
        if (!snapshot.hasHrv) updateSnapshotHrv(hrv, "0x56_AUTO_HISTORY_FALLBACK");
        if (!snapshot.hasFatigue) updateSnapshotFatigue(fatigue, "0x56_AUTO_HISTORY_FALLBACK");
      }
    }
    return;
  }

  if ((type == 0x60 || type == 0x66) && !snapshot.hasSpo2) {
    const char* source = (type == 0x66) ? "0x66_AUTO_HISTORY_FALLBACK" : "0x60_AUTO_HISTORY_FALLBACK";
    for (int i = (int)len - 1; i >= 9; i--) {
      uint8_t spo2 = data[i];
      if (validSpo2(spo2)) {
        updateSnapshotSpo2(spo2, source);
        break;
      }
    }
    return;
  }

  if ((type == 0x62 || type == 0x65) && !snapshot.hasTemperature) {
    const char* source = (type == 0x65) ? "0x65_AUTO_HISTORY_FALLBACK" : "0x62_AUTO_HISTORY_FALLBACK";
    for (int i = (int)len - 2; i >= 9; i--) {
      float temp = parseTempLittleEndian(data[i], data[i + 1]);
      if (validTemperature(temp)) {
        updateSnapshotTemperature(temp, source);
        break;
      }
    }
    return;
  }

  if (type == 0x53 && !snapshot.hasSleep) {
    uint16_t recordId = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    char date[11];
    snprintf(date, sizeof(date), "20%02u-%02u-%02u", bcdToDec(data[3]), bcdToDec(data[4]), bcdToDec(data[5]));
    char time[9];
    snprintf(time, sizeof(time), "%02u:%02u:%02u", bcdToDec(data[6]), bcdToDec(data[7]), bcdToDec(data[8]));
    uint16_t sleepMinutes = (uint16_t)data[9] * 5;
    updateSnapshotSleep(recordId, date, time, sleepMinutes, "0x53_SLEEP_HISTORY");
    return;
  }
}

// ============================================================
// PARSE/DEBUG PACOTE 0x09 - pacote live que o app parece usar
// ============================================================

void handlePacket09(uint8_t* data, size_t len, String raw) {
  selectedPacket09RawHash = fnv1aHashString(raw);

  Serial.print("[0x09 DEBUG] len=");
  Serial.print(len);
  Serial.print(" raw=");
  Serial.println(raw);

  Serial.print("[0x09 BYTES]");
  for (size_t i = 0; i < len; i++) {
    Serial.print(" [");
    Serial.print(i);
    Serial.print("]=");
    if (data[i] < 16) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();

  if (len >= 24) {
    uint8_t possibleBpm = data[21];
    float possibleTemp = parseTempLittleEndian(data[22], data[23]);

    Serial.print("[0x09 PARSE] possibleBpm=");
    Serial.print(possibleBpm);
    Serial.print(" possibleTemp=");
    Serial.println(possibleTemp, 1);

    if (validBpm(possibleBpm) && !snapshot.hasBpm) {
      updateSnapshotBpm(possibleBpm, "0x09_LIVE_PACKET");
    }

    if (validTemperature(possibleTemp) && !snapshot.hasTemperature) {
      updateSnapshotTemperature(possibleTemp, "0x09_LIVE_PACKET");
    }
  }

  // Exploratório: ajuda a localizar HRV/Stress/pressão comparando com o app.
  if (len >= 10) {
    Serial.print("[0x09 CANDIDATES] ");
    for (size_t i = 0; i < len; i++) {
      uint8_t v = data[i];
      if (v >= 1 && v <= 220) {
        Serial.print("i");
        Serial.print(i);
        Serial.print("=");
        Serial.print(v);
        Serial.print(" ");
      }
    }
    Serial.println();
  }
}

// ============================================================
// NOTIFY
// ============================================================

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

  if (type == 0x09) {
    handlePacket09(data, len, rawStr);
    return;
  }

  if (type == 0x54 || type == 0x53 || type == 0x56 || type == 0x60 || type == 0x62 || type == 0x65 || type == 0x66) {
    String packetType = packetTypeString(type);

    Serial.print("[HIST] ");
    Serial.print(packetType);
    Serial.print(" len=");
    Serial.print(len);
    Serial.print(" rawLen=");
    Serial.println(rawStr.length());

    if (type == 0x56) {
      Serial.print("[HIST RAW] ");
      Serial.print(packetType);
      Serial.print(" raw=");
      Serial.println(rawStr);
    }

    if (captureHistoryEnabled) {
      if (SAVE_HISTORY_RAW_DEBUG || READ_HISTORY_DEBUG) {
        appendRawPacketToFile(packetType.c_str(), rawStr, millis());
      }
      handleHistoryPacket(type, data, len, rawStr);
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

// ============================================================
// CONNECT / DISCONNECT BLE
// ============================================================

bool connectBle() {
  Serial.println("[BLE] Iniciando...");
  printHeap("antes BLE");

  BLEDevice::init("ESP32_ACTIVE_FRESH");

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
  captureFreshActiveEnabled = false;

  bleReady = false;
  txChar = nullptr;
  rxChar = nullptr;

  if (client && client->isConnected()) {
    Serial.println("[BLE] Disconnect client...");
    client->disconnect();
    delay(1000);
  }

  // Não usar delete client nem BLEDevice::deinit(true): antes causava CORRUPT HEAP.
  // O deep sleep limpa o estado do BLE no próximo boot.

  Serial.println("[BLE] Desconectado. Não vou liberar heap manualmente.");
  printHeap("depois BLE disconnect");
}

// ============================================================
// BOOT 0: INTERVALO / AUTO OPCIONAL
// ============================================================

void configureAutoThenSleep() {
  Serial.println("[MODE] BOOT 0 - CONFIGURAR AUTOMÁTICO APP-LIKE");

  if (!AUTO_CONFIG_ENABLED && !AUTO_DISABLE_ON_BOOT0) {
    Serial.println("[AUTO] Ignorado. Indo direto para coleta.");
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }

  if (AUTO_CONFIG_ENABLED && autoAlreadyConfigured && !FORCE_AUTO_CONFIG_EVERY_BOOT0) {
    Serial.println("[AUTO] Já configurado nesta sessão RTC. Não vou reconfigurar.");
    Serial.print("[AUTO] Dormindo ");
    Serial.print(COLLECT_SLEEP_SECONDS);
    Serial.println("s antes da próxima coleta.");
    goToDeepSleep(COLLECT_SLEEP_SECONDS, 1);
  }

  if (!connectBle()) {
    Serial.println("[MODE] Falha BLE no boot 0. Dormindo curto e tentando de novo.");
    bleOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 0);
  }

  if (AUTO_DISABLE_ON_BOOT0) {
    disableAutoConfigs();
    autoAlreadyConfigured = false;
  } else if (AUTO_CONFIG_ENABLED) {
    ensureAutoConfigs();
    autoAlreadyConfigured = true;
    Serial.println("[AUTO] Configurado no padrão do app JC Vital:");
    Serial.print("[AUTO] HR=");
    Serial.print(AUTO_BPM_INTERVAL_MIN);
    Serial.print("min | SpO2=");
    Serial.print(AUTO_SPO2_INTERVAL_MIN);
    Serial.print("min | HRV/Stress=");
    Serial.print(AUTO_HRV_INTERVAL_MIN);
    Serial.print("min | Temp=");
    Serial.print(AUTO_TEMP_INTERVAL_MIN);
    Serial.println("min");
  }

  bootStep = 1;
  bleOff();

  Serial.print("[AUTO] Dormindo ");
  Serial.print(BOOT0_FIRST_WAIT_SECONDS);
  Serial.println("s antes da primeira coleta.");
  goToDeepSleep(BOOT0_FIRST_WAIT_SECONDS, 1);
}

// ============================================================
// BOOT 1: MEDIÇÃO ATIVA FRESCA
// ============================================================

bool activeModeTargetReached(uint8_t mode) {
  if (mode == 0x02) {
    // Pressão é oportunista: não seguramos o ciclo esperando BP.
    return snapshot.bpmSamples >= MIN_BPM_SAMPLES;
  }

  if (mode == 0x03) {
    return snapshot.spo2Samples >= MIN_SPO2_SAMPLES;
  }

  if (mode == 0x04) {
    return snapshot.temperatureSamples >= MIN_TEMP_SAMPLES;
  }

  if (mode == 0x01) {
    return snapshot.hrvSamples >= MIN_HRV_SAMPLES && snapshot.fatigueSamples >= MIN_FATIGUE_SAMPLES;
  }

  return false;
}

void collectActiveMode(uint8_t mode, const char* label, unsigned long durationMs) {
  Serial.print("[FRESH] START modo=");
  Serial.print(label);
  Serial.print(" 0x");
  Serial.print(mode, HEX);
  Serial.print(" durationMs=");
  Serial.println(durationMs);

  currentActiveMode = mode;
  currentActiveModeLabel = label;
  currentActiveModeStartedAtMs = millis();
  currentActiveModeAcceptAfterMs = currentActiveModeStartedAtMs + ACTIVE_WARMUP_MS;
  captureFreshActiveEnabled = true;
  lastPrint028 = 0;
  lastSave028 = 0;

  startActive028(mode);

  unsigned long start = millis();
  unsigned long targetReachedAt = 0;

  while (millis() - start < durationMs) {
    delay(500);

    if (millis() >= currentActiveModeAcceptAfterMs && activeModeTargetReached(mode)) {
      if (targetReachedAt == 0) {
        targetReachedAt = millis();
        Serial.print("[FRESH] Alvo encontrado em modo=");
        Serial.print(label);
        Serial.println(". Mantendo alguns segundos para estabilizar.");
      }

      if (millis() - targetReachedAt >= ACTIVE_STABLE_AFTER_TARGET_MS) {
        Serial.print("[FRESH] Alvo estável. Encerrando modo=");
        Serial.println(label);
        break;
      }
    }
  }

  stopActive028(mode);
  delay(STOP_ACTIVE_DELAY_MS);

  captureFreshActiveEnabled = false;

  Serial.print("[FRESH] STOP modo=");
  Serial.println(label);
}


void readHistoriesForSnapshot() {
  Serial.println("[HIST] Lendo últimos registros automáticos/manuais para montar snapshot...");

  captureHistoryEnabled = true;

  Serial.println("[HIST] BPM 0x54...");
  readHistoryBpm();
  delay(HIST_DELAY_MS);

  Serial.println("[HIST] HRV/fadiga 0x56 latest AA=0...");
  sendCmd(0x56, 0x00, 0x00, 0x00);
  delay(HIST_DELAY_LONG_MS);

  if (FAST_TEST_MODE) {
    if (!snapshot.hasHrv || !snapshot.hasFatigue) {
      Serial.println("[HIST] HRV/fadiga 0x56 position AA=1 pos=0...");
      sendCmd(0x56, 0x01, 0x00, 0x00);
      delay(HIST_DELAY_MS);
    }
  } else {
    Serial.println("[HIST] HRV/fadiga 0x56 position AA=1 pos=0...");
    sendCmd(0x56, 0x01, 0x00, 0x00);
    delay(HIST_DELAY_LONG_MS);

    Serial.println("[HIST] HRV/fadiga 0x56 position AA=1 pos=1...");
    sendCmd(0x56, 0x01, 0x01, 0x00);
    delay(HIST_DELAY_LONG_MS);

    Serial.println("[HIST] HRV/fadiga 0x56 next AA=2 #1...");
    sendCmd(0x56, 0x02, 0x00, 0x00);
    delay(HIST_DELAY_SHORT_MS);

    Serial.println("[HIST] HRV/fadiga 0x56 next AA=2 #2...");
    sendCmd(0x56, 0x02, 0x00, 0x00);
    delay(HIST_DELAY_SHORT_MS);

    Serial.println("[HIST] HRV/fadiga 0x56 next AA=2 #3...");
    sendCmd(0x56, 0x02, 0x00, 0x00);
    delay(HIST_DELAY_SHORT_MS);
  }

  Serial.println("[HIST] SpO2 automático 0x66...");
  readHistorySpo2Auto();
  delay(HIST_DELAY_LONG_MS);

  if (!FAST_TEST_MODE || !snapshot.hasSpo2) {
    Serial.println("[HIST] SpO2 manual 0x60...");
    readHistorySpo2Manual();
    delay(HIST_DELAY_MS);
  }

  Serial.println("[HIST] Temperatura automática 0x65...");
  readHistoryTempAuto();
  delay(HIST_DELAY_MS);

  if (!FAST_TEST_MODE || !snapshot.hasTemperature) {
    Serial.println("[HIST] Temperatura manual 0x62...");
    readHistoryTempManual();
    delay(HIST_DELAY_MS);
  }

  Serial.println("[HIST] Sono 0x53...");
  readHistorySleep();
  delay(HIST_DELAY_MS);

  captureHistoryEnabled = false;

  Serial.println("[HIST] Leitura de históricos finalizada.");
}

void collectFreshActiveVitals() {
  Serial.println(FAST_TEST_MODE
    ? "[MODE] BOOT 1 - TESTE RÁPIDO (histórico + ativo completo)"
    : "[MODE] BOOT 1 - SYNC APP-LIKE 2 POR HORA");

  clearPacketsFile();
  resetSnapshot();

  measurementSessionId = bootCounter;
  measurementSessionStartedAtMs = millis();

  if (!connectBle()) {
    Serial.println("[MODE] Falha BLE ao coletar snapshot.");
    bleOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }

  // 1) Históricos: BPM, HRV, SpO2, temp, sono.
  readHistoriesForSnapshot();

  // 2) Medições ativas — em teste rápido sempre roda tudo para dados frescos.
  if (USE_ACTIVE_HEART_BP_REFRESH || FAST_TEST_MODE) {
    collectActiveMode(0x02, "HEART_BP_OPPORTUNISTIC", ACTIVE_HEART_BP_MS);
    delay(GAP_BETWEEN_ACTIVE_MODES_MS);
  }

  if (FAST_TEST_MODE || (USE_ACTIVE_SPO2_IF_MISSING && snapshot.spo2Samples < MIN_SPO2_SAMPLES)) {
    collectActiveMode(0x03, FAST_TEST_MODE ? "SPO2_ACTIVE" : "SPO2_FALLBACK", ACTIVE_SPO2_MS);
    delay(GAP_BETWEEN_ACTIVE_MODES_MS);
  }

  if (FAST_TEST_MODE || (USE_ACTIVE_TEMP_IF_MISSING && !snapshot.hasTemperature)) {
    collectActiveMode(0x04, FAST_TEST_MODE ? "TEMP_ACTIVE" : "TEMP_FALLBACK", ACTIVE_TEMP_MS);
    delay(GAP_BETWEEN_ACTIVE_MODES_MS);
  }

  if (FAST_TEST_MODE || (TRY_ACTIVE_HRV_DEBUG && (!snapshot.hasHrv || !snapshot.hasFatigue))) {
    collectActiveMode(0x01, FAST_TEST_MODE ? "HRV_FATIGUE_ACTIVE" : "HRV_FATIGUE_DEBUG", ACTIVE_HRV_MS);
    delay(GAP_BETWEEN_ACTIVE_MODES_MS);
  }

  // Segunda tentativa do 0x56 se HRV/fadiga ainda faltando.
  if (!snapshot.hasHrv || !snapshot.hasFatigue) {
    captureHistoryEnabled = true;
    Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=0...");
    sendCmd(0x56, 0x00, 0x00, 0x00);
    delay(HIST_DELAY_LONG_MS);

    if (!FAST_TEST_MODE) {
      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=1 pos=0...");
      sendCmd(0x56, 0x01, 0x00, 0x00);
      delay(HIST_DELAY_LONG_MS);

      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=1 pos=1...");
      sendCmd(0x56, 0x01, 0x01, 0x00);
      delay(HIST_DELAY_LONG_MS);

      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=2...");
      sendCmd(0x56, 0x02, 0x00, 0x00);
      delay(HIST_DELAY_SHORT_MS);
    }

    captureHistoryEnabled = false;
  }

  // Sono: segunda leitura se ainda não veio no histórico.
  if (!snapshot.hasSleep) {
    captureHistoryEnabled = true;
    Serial.println("[HIST] Segunda tentativa sono 0x53...");
    readHistorySleep();
    delay(HIST_DELAY_MS);
    captureHistoryEnabled = false;
  }

  finalizeSnapshotFreshness();
  printSnapshotDebug();
  saveVitalsSnapshotToFile();
  appendSnapshotToPacketsFile();

  int lines = countPacketLines();
  Serial.print("[FS] Total de pacotes salvos em arquivo=");
  Serial.println(lines);

  bleOff();

  // Não liga Wi-Fi neste mesmo boot.
  // Acorda em boot limpo só para HTTP.
  goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
}

// ============================================================
// WIFI / HTTP
// ============================================================

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

  const int maxAttempts = FAST_TEST_MODE ? 4 : 6;
  const unsigned long retryDelayMs = FAST_TEST_MODE ? 4000UL : 7000UL;

  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    Serial.print("[HTTP] Wake attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(maxAttempts);

    if (pingHealth()) {
      Serial.println("[HTTP] API OK.");
      return true;
    }

    delay(retryDelayMs);
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

  // Na estratégia nova, normalmente só há 1 pacote: SNAPSHOT_VITALS.
  const int LOCAL_BATCH_SIZE = 1;

  int sent = 0;
  int batchCount = 0;
  bool firstPacket = true;

  String body;
  body.reserve(5000);

  body = "{";
  body += "\"deviceMac\":\"";
  body += DEVICE_MAC;
  body += "\",";
  body += "\"source\":\"";
  body += apiSourceLabel();
  body += "\",";
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
      body += "\"source\":\"";
  body += apiSourceLabel();
  body += "\",";
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
  Serial.println("[MODE] BOOT 2 - ENVIAR API");

  if (!connectWifi()) {
    wifiOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
  }

  bool apiOk = waitApiAwake();

  if (!apiOk) {
    Serial.println("[HTTP] API indisponível. Mantendo arquivo e tentando no próximo boot 2.");
    wifiOff();
    goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
  }

  bool sentOk = sendPacketsFileToApi();

  if (sentOk) {
    Serial.println("[HTTP] Enviado com sucesso. Limpando arquivo.");
    clearPacketsFile();
  } else {
    Serial.println("[HTTP] Envio falhou. Arquivo será mantido.");
  }

  wifiOff();

  if (sentOk) {
    Serial.println("[SLEEP] Envio OK. Voltando para BOOT 0 para iniciar próximo ciclo remoto.");
    goToDeepSleep(SHORT_SLEEP_SECONDS, 0);
  }

  Serial.println("[SLEEP] Envio falhou. Tentando reenviar no BOOT 2.");
  goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  bootCounter++;

  Serial.println();
  Serial.println("========================================");
  Serial.println(FAST_TEST_MODE
    ? "ESP32 FAST TEST - ciclo ~3-4 min | BOOT 0=AUTO | 1=COLETA | 2=API"
    : "ESP32 APP-LIKE 15MIN - BLE/WIFI ISOLADOS");
  Serial.println("BOOT 0=AUTO WAIT | BOOT 1=SYNC+DEBUG | BOOT 2=API");
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
    collectFreshActiveVitals();
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
