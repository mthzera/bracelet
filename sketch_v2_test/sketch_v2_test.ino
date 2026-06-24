#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLEAddress.h>

#include "esp_sleep.h"
#include <esp_system.h>
#include "soc/rtc_cntl_reg.h"

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
// BOOTS (teste: sem boot 0 — pulseira configurada pelo app JC Vital)
// ============================================================
// bootStep 1 = coleta BLE + medições ativas + SNAPSHOT_VITALS
// bootStep 2 = envia snapshot para API

RTC_DATA_ATTR int bootStep = 1;
RTC_DATA_ATTR uint32_t bootCounter = 0;

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
RTC_DATA_ATTR uint8_t brownoutSkipBoot0Count = 0;

// ============================================================
// ARQUIVO DE TESTE EXPERIMENTAL (sketch_v2_test)
// Hipóteses a testar:
//  1) Reset de estado da pulseira antes de medir (stop all modes)
//  2) Sincronização de tempo via 0x01 logo após conectar
//  3) Leitura de histórico 0x56 ANTES das medições ativas (HRV histórico como prio)
//  4) Sequência JC Vital: 0x02 (BPM+temp) → 0x03 (SpO2) → 0x01 (HRV+PA)
//  5) Maior warmup antes de aceitar dados do modo 0x01
//  6) Log verboso de todos os pacotes 0x28 durante modo HRV para diagnóstico
// ============================================================

const bool FAST_TEST_MODE = true;

// Intervalo entre ciclos após envio OK para a API.
const uint64_t COLLECT_SLEEP_SECONDS = FAST_TEST_MODE ? 60ULL : 900ULL;
const uint64_t SHORT_SLEEP_SECONDS = FAST_TEST_MODE ? 5ULL : 5ULL;

// Sequência JC Vital: 0x02 → 0x03 → 0x01
// Timeouts do teste — 0x01 com mais tempo pois HRV pode demorar 85s+
const unsigned long TEST_BPM_TEMP_MS  = 45000UL;   // 0x02 primeiro — rápido
const unsigned long TEST_SPO2_MS      = 180000UL;  // 0x03
const unsigned long TEST_HRV_FATIGUE_BP_MS = 240000UL; // 0x01 — mais tempo (era 180s)

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

// TEST: logs verbosos ligados para diagnóstico do modo 0x01 (HRV)
const bool VERBOSE_BLE_LOGS = true;
const uint32_t MIN_FREE_HEAP_BYTES = 32000;
const unsigned long PRINT_028_EVERY_MS = VERBOSE_BLE_LOGS ? 3000UL : 0UL;
const unsigned long SAVE_028_EVERY_MS = 5000UL;

unsigned long lastPrint028 = 0;
unsigned long lastSave028 = 0;
unsigned long lastReceived028Ms = 0; // último pacote 0x28 recebido (para detectar silêncio)

// ============================================================
// SNAPSHOT
// ============================================================

struct SleepStageSegment {
  uint8_t stageByte;
  uint16_t minutes;
};

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
  char sleepEndTime[9];
  uint16_t sleepMinutes;
  uint16_t sleepInBedMinutes;
  uint8_t sleepQuality;
  uint8_t sleepSegmentCount;
  SleepStageSegment sleepSegments[64];
  struct {
    uint16_t awake;
    uint16_t rem;
    uint16_t light;
    uint16_t deep;
    uint16_t nap;
    uint16_t unknown;
  } sleepTotals;
  const char* sleepSource;
  uint16_t sleepSamples;

  bool hasBattery;
  uint8_t battery;
  uint32_t batteryAtMs;

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

uint32_t fnv1aHashBytes(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261UL;

  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }

  return hash;
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

void saveVitalsSnapshotToFile();

bool heapCriticallyLow() {
  return ESP.getFreeHeap() < MIN_FREE_HEAP_BYTES;
}

void saveCollectionCheckpoint(const char* reason) {
  if (measurementSessionStartedAtMs == 0) return;
  saveVitalsSnapshotToFile();
  Serial.print("[CHK] Snapshot parcial salvo (");
  Serial.print(reason);
  Serial.print(") heap=");
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
// SLEEP / POWER
// ============================================================

void disableBrownoutDetector() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
}

void restoreBrownoutDetector() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);
}

// Boot 1 usa BLE — desliga BOD cedo para evitar loop de reset em cabo USB fraco.
void preparePowerForBleBoot() {
  WiFi.mode(WIFI_OFF);
  setCpuFrequencyMhz(80);
  disableBrownoutDetector();
  delay(brownoutSkipBoot0Count > 0 ? 2000 : 800);
  Serial.println("[PWR] CPU 80MHz, brownout off, pronto para BLE.");
}

void goToDeepSleep(uint64_t seconds, int nextStep) {
  Serial.print("[SLEEP] Próximo step=");
  Serial.print(nextStep);
  Serial.print(" dormindo por ");
  Serial.print(seconds);
  Serial.println("s");

  bootStep = nextStep;

  Serial.flush();

  restoreBrownoutDetector();
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
  snapshot.sleepEndTime[0] = '\0';
  snapshot.sleepMinutes = 0;
  snapshot.sleepInBedMinutes = 0;
  snapshot.sleepQuality = 0;
  snapshot.sleepSegmentCount = 0;
  memset(&snapshot.sleepTotals, 0, sizeof(snapshot.sleepTotals));
  snapshot.sleepSource = "missing";
  snapshot.sleepSamples = 0;

  snapshot.hasBattery = false;
  snapshot.battery = 0;
  snapshot.batteryAtMs = 0;

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

void logVitalFirstFound(const char* label, const char* detail) {
  if (measurementSessionStartedAtMs == 0) return;

  unsigned long sessionMs = millis() - measurementSessionStartedAtMs;
  unsigned long modeMs = (currentActiveModeStartedAtMs > 0)
    ? (millis() - currentActiveModeStartedAtMs)
    : 0;

  Serial.print("[TIMING] ");
  Serial.print(label);
  Serial.print(" ");
  Serial.print(detail);
  Serial.print(" | sessão +");
  Serial.print(sessionMs);
  Serial.print("ms (");
  Serial.print(sessionMs / 1000.0, 1);
  Serial.print("s)");

  if (captureFreshActiveEnabled && currentActiveModeLabel) {
    Serial.print(" | modo +");
    Serial.print(modeMs);
    Serial.print("ms (");
    Serial.print(modeMs / 1000.0, 1);
    Serial.print("s) [");
    Serial.print(currentActiveModeLabel);
    Serial.print("]");
  }

  Serial.println();
}

void updateSnapshotBpm(uint8_t bpm, const char* source) {
  if (!validBpm(bpm)) return;

  bool first = !snapshot.hasBpm;
  snapshot.hasBpm = true;
  snapshot.bpm = bpm;
  snapshot.bpmSource = source;
  snapshot.bpmAtMs = millis();
  snapshot.bpmSamples++;
  if (first) {
    char buf[24];
    snprintf(buf, sizeof(buf), "bpm=%u", bpm);
    logVitalFirstFound("BPM", buf);
  }
}

void updateSnapshotSpo2(uint8_t spo2, const char* source) {
  if (!validSpo2(spo2)) return;

  bool first = !snapshot.hasSpo2;
  snapshot.hasSpo2 = true;
  snapshot.spo2 = spo2;
  snapshot.spo2Source = source;
  snapshot.spo2AtMs = millis();
  snapshot.spo2Samples++;
  if (first) {
    char buf[24];
    snprintf(buf, sizeof(buf), "spo2=%u%%", spo2);
    logVitalFirstFound("SpO2", buf);
  }
}

void updateSnapshotTemperature(float temp, const char* source) {
  if (!validTemperature(temp)) return;

  bool first = !snapshot.hasTemperature;
  snapshot.hasTemperature = true;
  snapshot.temperature = temp;
  snapshot.temperatureSource = source;
  snapshot.temperatureAtMs = millis();
  snapshot.temperatureSamples++;
  if (first) {
    char buf[24];
    snprintf(buf, sizeof(buf), "temp=%.1fC", temp);
    logVitalFirstFound("TEMP", buf);
  }
}

void updateSnapshotHrv(uint8_t hrv, const char* source) {
  if (!validHrv(hrv)) return;

  bool first = !snapshot.hasHrv;
  snapshot.hasHrv = true;
  snapshot.hrv = hrv;
  snapshot.hrvSource = source;
  snapshot.hrvAtMs = millis();
  snapshot.hrvSamples++;
  if (first) {
    char buf[24];
    snprintf(buf, sizeof(buf), "hrv=%u", hrv);
    logVitalFirstFound("HRV", buf);
  }
}

void updateSnapshotFatigue(uint8_t fatigue, const char* source) {
  if (!validFatigue(fatigue)) return;

  bool first = !snapshot.hasFatigue;
  snapshot.hasFatigue = true;
  snapshot.fatigue = fatigue;
  snapshot.fatigueSource = source;
  snapshot.fatigueAtMs = millis();
  snapshot.fatigueSamples++;
  if (first) {
    char buf[24];
    snprintf(buf, sizeof(buf), "fadiga=%u", fatigue);
    logVitalFirstFound("FADIGA", buf);
  }
}

void updateSnapshotBloodPressure(uint8_t sys, uint8_t dia, const char* source, const char* quality) {
  if (!validBloodPressure(sys, dia)) return;

  bool first = !snapshot.hasBloodPressure;
  snapshot.hasBloodPressure = true;
  snapshot.systolic = sys;
  snapshot.diastolic = dia;
  snapshot.bpSource = source;
  snapshot.bpQuality = quality;
  snapshot.bpAtMs = millis();
  snapshot.bpSamples++;
  if (first) {
    char buf[32];
    snprintf(buf, sizeof(buf), "pa=%u/%u", sys, dia);
    logVitalFirstFound("PRESSAO", buf);
  }
}

const char* sleepStageName(uint8_t stageByte) {
  if (stageByte == 0x01) return "deep";
  if (stageByte == 0x02) return "light";
  if (stageByte == 0x03) return "rem";
  if (stageByte == 0x04) return "awake";
  if (stageByte == 0x05) return "nap";
  return "unknown";
}

void computeSleepEndDateTime(const char* date, const char* time, uint16_t inBedMinutes) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  sscanf(date, "%d-%d-%d", &y, &mo, &d);
  sscanf(time, "%d:%d:%d", &h, &mi, &s);

  uint32_t totalMin = (uint32_t)h * 60UL + (uint32_t)mi + (uint32_t)inBedMinutes;
  int day = d;
  int month = mo;
  int year = y % 100;

  while (totalMin >= 24UL * 60UL) {
    totalMin -= 24UL * 60UL;
    day++;
    int daysInMonth = 31;
    if (month == 4 || month == 6 || month == 9 || month == 11) daysInMonth = 30;
    else if (month == 2) daysInMonth = 28;
    if (day > daysInMonth) { day = 1; month++; if (month > 12) { month = 1; year++; } }
  }

  int eh = (int)(totalMin / 60UL);
  int em = (int)(totalMin % 60UL);
  snprintf(snapshot.sleepEndTime, sizeof(snapshot.sleepEndTime), "%02d:%02d:%02d", eh, em, s);
}

size_t sleepRecordLength(uint8_t* data, size_t len, size_t offset) {
  // Protocolo 2208A: record = 10 bytes header + LEN bytes de segmentos (data[offset+9] = LEN)
  if (offset + 10 > len) return 0;
  uint8_t segLen = data[offset + 9];
  size_t recordLen = 10 + segLen;
  if (offset + recordLen > len) recordLen = len - offset;
  return recordLen;
}

bool parseSleepRecordAt(uint8_t* data, size_t len, size_t offset) {
  // Protocolo 2208A (doc seção 24):
  // [0]=0x53 [1-2]=ID [3-8]=YY MM DD HH mm SS (BCD) [9]=LEN
  // [10...(10+LEN-1)]: cada byte = estágio de 5 minutos (1=deep,2=light,3=rem,4=awake,5=nap)
  if (offset + 10 > len || data[offset] != 0x53) return false;

  uint16_t recordId = (uint16_t)data[offset + 1] | ((uint16_t)data[offset + 2] << 8);
  char date[11];
  snprintf(date, sizeof(date), "20%02u-%02u-%02u",
           bcdToDec(data[offset + 3]), bcdToDec(data[offset + 4]), bcdToDec(data[offset + 5]));
  char time[9];
  snprintf(time, sizeof(time), "%02u:%02u:%02u",
           bcdToDec(data[offset + 6]), bcdToDec(data[offset + 7]), bcdToDec(data[offset + 8]));

  uint8_t segCount = data[offset + 9];
  uint8_t available = (offset + 10 + segCount <= len) ? segCount : (uint8_t)(len - offset - 10);

  uint16_t sleepMinutes = 0;
  uint16_t inBedMinutes = (uint16_t)available * 5;
  uint16_t totalsAwake = 0, totalsRem = 0, totalsLight = 0, totalsDeep = 0, totalsNap = 0, totalsUnknown = 0;

  SleepStageSegment segments[64];
  uint8_t segmentCount = 0;
  uint8_t currentStage = 0xFF;
  uint16_t currentDur = 0;

  for (uint8_t i = 0; i < available; i++) {
    uint8_t stage = data[offset + 10 + i];

    if (stage == 0x01) { totalsDeep += 5; sleepMinutes += 5; }
    else if (stage == 0x02) { totalsLight += 5; sleepMinutes += 5; }
    else if (stage == 0x03) { totalsRem += 5; sleepMinutes += 5; }
    else if (stage == 0x04) { totalsAwake += 5; }
    else if (stage == 0x05) { totalsNap += 5; sleepMinutes += 5; }
    else { totalsUnknown += 5; sleepMinutes += 5; }

    if (stage == currentStage) {
      currentDur += 5;
    } else {
      if (currentDur > 0 && segmentCount < 64) {
        segments[segmentCount].stageByte = currentStage;
        segments[segmentCount].minutes = currentDur;
        segmentCount++;
      }
      currentStage = stage;
      currentDur = 5;
    }
  }
  if (currentDur > 0 && segmentCount < 64) {
    segments[segmentCount].stageByte = currentStage;
    segments[segmentCount].minutes = currentDur;
    segmentCount++;
  }

  // Fallback: sem segmentos válidos, usa LEN*5 como total
  if (sleepMinutes == 0 && available == 0 && segCount > 0) {
    sleepMinutes = (uint16_t)segCount * 5;
    inBedMinutes = sleepMinutes;
  }

  if (!validSleepMinutes(sleepMinutes > 0 ? sleepMinutes : inBedMinutes)) return false;
  if (sleepMinutes == 0) sleepMinutes = inBedMinutes;

  if (snapshot.hasSleep && sleepMinutes <= snapshot.sleepMinutes) return false;

  snapshot.hasSleep = true;
  snapshot.sleepRecordId = recordId;
  strncpy(snapshot.sleepDate, date, sizeof(snapshot.sleepDate) - 1);
  snapshot.sleepDate[sizeof(snapshot.sleepDate) - 1] = '\0';
  strncpy(snapshot.sleepTime, time, sizeof(snapshot.sleepTime) - 1);
  snapshot.sleepTime[sizeof(snapshot.sleepTime) - 1] = '\0';
  snapshot.sleepMinutes = sleepMinutes;
  snapshot.sleepInBedMinutes = inBedMinutes > 0 ? inBedMinutes : sleepMinutes;
  snapshot.sleepQuality = 0;
  snapshot.sleepSegmentCount = segmentCount;
  for (uint8_t i = 0; i < segmentCount; i++) snapshot.sleepSegments[i] = segments[i];
  snapshot.sleepTotals.awake = totalsAwake;
  snapshot.sleepTotals.rem = totalsRem;
  snapshot.sleepTotals.light = totalsLight;
  snapshot.sleepTotals.deep = totalsDeep;
  snapshot.sleepTotals.nap = totalsNap;
  snapshot.sleepTotals.unknown = totalsUnknown;
  computeSleepEndDateTime(snapshot.sleepDate, snapshot.sleepTime, snapshot.sleepInBedMinutes);
  snapshot.sleepSource = "0x53_SLEEP_HISTORY";
  snapshot.sleepSamples++;

  Serial.print("[SLEEP] recordId=");
  Serial.print(recordId);
  Serial.print(" date="); Serial.print(date);
  Serial.print(" time="); Serial.print(time);
  Serial.print(" sleepMin="); Serial.print(sleepMinutes);
  Serial.print(" inBed="); Serial.print(snapshot.sleepInBedMinutes);
  Serial.print(" segs="); Serial.print(segmentCount);
  Serial.print(" deep="); Serial.print(totalsDeep);
  Serial.print(" light="); Serial.print(totalsLight);
  Serial.print(" rem="); Serial.print(totalsRem);
  Serial.print(" awake="); Serial.println(totalsAwake);

  return true;
}

void parseSleepBuffer(uint8_t* data, size_t len) {
  for (size_t offset = 0; offset + 10 < len; ) {
    if (data[offset] != 0x53) { offset++; continue; }
    parseSleepRecordAt(data, len, offset);
    size_t recordLen = sleepRecordLength(data, len, offset);
    if (recordLen == 0) break;
    offset += recordLen;
  }
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

void updateSnapshotBattery(uint8_t bat) {
  if (bat == 0 || bat > 100) return;

  snapshot.hasBattery = true;
  snapshot.battery = bat;
  snapshot.batteryAtMs = millis();
}

bool snapshotComplete() {
  return snapshot.hasBpm &&
         snapshot.hasSpo2 &&
         snapshot.hasTemperature &&
         snapshot.hasHrv &&
         snapshot.hasFatigue &&
         snapshot.hasBloodPressure;
}

void printSnapshotMissing() {
  if (!snapshot.hasHrv) Serial.println("  - HRV");
  if (!snapshot.hasFatigue) Serial.println("  - fadiga");
  if (!snapshot.hasBloodPressure) Serial.println("  - pressão arterial");
  if (!snapshot.hasSpo2) Serial.println("  - SpO2");
  if (!snapshot.hasBpm) Serial.println("  - BPM");
  if (!snapshot.hasTemperature) Serial.println("  - temperatura");
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
  f.print(",\"bpmAtMs\":");
  if (snapshot.hasBpm) f.print(snapshot.bpmAtMs); else f.print("null");

  f.print(",\"spo2\":");
  if (snapshot.hasSpo2) f.print(snapshot.spo2); else f.print("null");
  f.print(",\"spo2AtMs\":");
  if (snapshot.hasSpo2) f.print(snapshot.spo2AtMs); else f.print("null");

  f.print(",\"temperature\":");
  if (snapshot.hasTemperature) f.print(snapshot.temperature, 1); else f.print("null");
  f.print(",\"temperatureAtMs\":");
  if (snapshot.hasTemperature) f.print(snapshot.temperatureAtMs); else f.print("null");

  f.print(",\"hrv\":");
  if (snapshot.hasHrv) f.print(snapshot.hrv); else f.print("null");
  f.print(",\"hrvAtMs\":");
  if (snapshot.hasHrv) f.print(snapshot.hrvAtMs); else f.print("null");

  f.print(",\"fatigue\":");
  if (snapshot.hasFatigue) f.print(snapshot.fatigue); else f.print("null");
  f.print(",\"fatigueAtMs\":");
  if (snapshot.hasFatigue) f.print(snapshot.fatigueAtMs); else f.print("null");

  f.print(",\"bloodPressureSystolic\":");
  if (snapshot.hasBloodPressure) f.print(snapshot.systolic); else f.print("null");
  f.print(",\"bloodPressureDiastolic\":");
  if (snapshot.hasBloodPressure) f.print(snapshot.diastolic); else f.print("null");
  f.print(",\"bloodPressureAtMs\":");
  if (snapshot.hasBloodPressure) f.print(snapshot.bpAtMs); else f.print("null");

  f.print(",\"battery\":");
  if (snapshot.hasBattery) f.print(snapshot.battery); else f.print("null");
  f.print(",\"batteryAtMs\":");
  if (snapshot.hasBattery) f.print(snapshot.batteryAtMs); else f.print("null");

  f.print(",\"sleepMinutes\":");
  if (snapshot.hasSleep) f.print(snapshot.sleepMinutes); else f.print("null");

  f.print(",\"sleepDate\":");
  if (snapshot.hasSleep) { f.print("\""); f.print(snapshot.sleepDate); f.print("\""); } else f.print("null");

  f.print(",\"sleepTime\":");
  if (snapshot.hasSleep) { f.print("\""); f.print(snapshot.sleepTime); f.print("\""); } else f.print("null");

  f.print(",\"sleepRecordId\":");
  if (snapshot.hasSleep) f.print(snapshot.sleepRecordId); else f.print("null");

  f.print(",\"sleepDetail\":");
  if (!snapshot.hasSleep) {
    f.print("null");
  } else {
    f.print("{");
    f.print("\"recordId\":"); f.print(snapshot.sleepRecordId);
    f.print(",\"date\":\""); f.print(snapshot.sleepDate); f.print("\"");
    f.print(",\"startTime\":\""); f.print(snapshot.sleepTime); f.print("\"");
    f.print(",\"endTime\":\""); f.print(snapshot.sleepEndTime); f.print("\"");
    f.print(",\"sleepMinutes\":"); f.print(snapshot.sleepMinutes);
    f.print(",\"inBedMinutes\":"); f.print(snapshot.sleepInBedMinutes);
    if (snapshot.sleepQuality > 0) { f.print(",\"quality\":"); f.print(snapshot.sleepQuality); }
    f.print(",\"totals\":{");
    f.print("\"awake\":"); f.print(snapshot.sleepTotals.awake);
    f.print(",\"rem\":"); f.print(snapshot.sleepTotals.rem);
    f.print(",\"light\":"); f.print(snapshot.sleepTotals.light);
    f.print(",\"deep\":"); f.print(snapshot.sleepTotals.deep);
    f.print(",\"nap\":"); f.print(snapshot.sleepTotals.nap);
    f.print(",\"unknown\":"); f.print(snapshot.sleepTotals.unknown);
    f.print("},\"segments\":[");
    for (uint8_t i = 0; i < snapshot.sleepSegmentCount; i++) {
      if (i > 0) f.print(",");
      f.print("{\"stage\":\"");
      f.print(sleepStageName(snapshot.sleepSegments[i].stageByte));
      f.print("\",\"minutes\":"); f.print(snapshot.sleepSegments[i].minutes);
      f.print("}");
    }
    f.print("]}");
  }

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

// Liga o stream realtime 0x09 (HR, temp, SpO2 automático a cada mudança).
// Mantém ativo durante medição SpO2 pois data[24]=VV=blood oxygen.
void enableRealtimeStream() {
  Serial.println("[RT] Ligando stream 0x09 (HR+temp+SpO2 realtime)...");
  uint8_t pkt[16] = {0x09, 0x01, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  sendRaw16(pkt);
}

void disableRealtimeStream() {
  uint8_t pkt[16] = {0x09, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  sendRaw16(pkt);
}

// Reinicia o sensor SpO2 quando ele fica sem responder (lights off).
void kickActiveSpo2Sensor(uint8_t kickNumber) {
  Serial.print("[SPO2] Sensor sem resposta — reiniciando (kick ");
  Serial.print(kickNumber);
  Serial.println(")");
  stopActive028(0x03);
  delay(2000);
  startActive028(0x03);
}

// Envia 0x01 com data/hora atual para sincronizar o relógio da pulseira.
// Necessário para timestamps corretos no histórico (doc 2208A seção 1).
void setBraceletTime() {
  // Usa tempo relativo ao boot — sem RTC real, manda um horário fixo de referência.
  // Manda 00:00:00 do dia 01/01/25 como baseline (só para a pulseira ter um clock válido).
  uint8_t pkt[16] = {
    0x01,
    0x25, // year BCD 25
    0x01, // month
    0x01, // day
    0x08, // hour 08:00:00 (horário padrão de referência)
    0x00, // minute
    0x00, // second
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  Serial.println("[INIT] Sincronizando relógio da pulseira (0x01)...");
  sendRaw16(pkt);
  delay(500);
}

// Para todas as medições ativas para garantir estado limpo antes de começar.
// Evita que a pulseira fique em modo travado após reboot rápido.
void stopAllActiveModes() {
  Serial.println("[INIT] Parando todos os modos ativos (reset de estado)...");
  sendCmd(0x28, 0x01, 0x00); delay(300);
  sendCmd(0x28, 0x02, 0x00); delay(300);
  sendCmd(0x28, 0x03, 0x00); delay(300);
  sendCmd(0x28, 0x04, 0x00); delay(300);
  Serial.println("[INIT] Reset de estado concluído.");
}

void stopActive028(uint8_t mode) {
  sendCmd(0x28, mode, 0x00);
}

// ============================================================
// HISTÓRICOS OPCIONAIS
// ============================================================

void readBattery() {
  // Comando 0x13 AA=0x99: solicita leitura da bateria (doc 2208A seção 9)
  sendCmd(0x13, 0x99, 0x00, 0x00);
}

void readBatteryFromService() {
  if (!client || !client->isConnected()) return;

  BLERemoteService* battSvc = client->getService(BLEUUID((uint16_t)0x180F));
  if (!battSvc) {
    Serial.println("[BATT] Serviço 0x180F não encontrado.");
    return;
  }

  BLERemoteCharacteristic* battChar = battSvc->getCharacteristic(BLEUUID((uint16_t)0x2A19));
  if (!battChar) {
    Serial.println("[BATT] Característica 0x2A19 não encontrada.");
    return;
  }

  String val = battChar->readValue();
  if (val.length() > 0) {
    uint8_t bat = (uint8_t)val[0];
    Serial.print("[BATT] BLE Battery Service 0x180F=");
    Serial.print(bat);
    Serial.println("%");
    updateSnapshotBattery(bat);
  } else {
    Serial.println("[BATT] Leitura 0x180F vazia.");
  }
}

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

void handleActivePacket028(uint8_t* data, size_t len) {
  if (len < 10) {
    Serial.println("[0x28] Pacote curto, ignorado.");
    return;
  }

  unsigned long now = millis();
  lastReceived028Ms = now; // atualiza sempre que chega qualquer 0x28

  if (!captureFreshActiveEnabled) {
    return;
  }

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
    if (VERBOSE_BLE_LOGS) {
      Serial.print("[0x28 ACTIVE] Ignorado por modo diferente. collecting=0x");
      Serial.print(currentActiveMode, HEX);
      Serial.print(" packetMode=0x");
      Serial.println(packetMode, HEX);
    }
    return;
  }

  const char* source = activeModeSource(currentActiveMode);
  uint32_t rawHash = fnv1aHashBytes(data, len);

  // Usa somente o campo esperado do modo ativo atual.
  // Isso evita que um valor antigo/cacheado de outro campo sobrescreva o snapshot.
  if (currentActiveMode == 0x02) {
    // HEART/BP: no app, quando mede BPM ele costuma trazer temperatura junto.
    updateSnapshotBpm(bpm, source);
    updateSnapshotTemperature(temp, source);

    if (validBloodPressure(pressaoAlta, pressaoBaixa)) {
      selectedBpRawHash = rawHash;
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
    selectedHrvRawHash = rawHash;
    updateSnapshotHrv(hrv, source);
    updateSnapshotFatigue(fadiga, source);

    // No JC Vital, ao medir HRV a pulseira costuma atualizar também pressão.
    if (validBloodPressure(pressaoAlta, pressaoBaixa)) {
      selectedBpRawHash = rawHash;
      updateSnapshotBloodPressure(pressaoAlta, pressaoBaixa, source, "estimated");
    }
  }

  if (VERBOSE_BLE_LOGS && PRINT_028_EVERY_MS > 0 && now - lastPrint028 >= PRINT_028_EVERY_MS) {
    lastPrint028 = now;

    Serial.print("[0x28] mode=0x"); Serial.print(packetMode, HEX);
    Serial.print(" bpm="); Serial.print(bpm);
    Serial.print(" spo2="); Serial.print(spo2);
    Serial.print(" temp="); Serial.print(temp, 1);
    Serial.print(" hrv="); Serial.print(hrv);
    Serial.print(" fadiga="); Serial.print(fadiga);
    Serial.print(" bp="); Serial.print(pressaoAlta); Serial.print("/"); Serial.print(pressaoBaixa);
    Serial.print(" bpOK="); Serial.print(validBloodPressure(pressaoAlta, pressaoBaixa) ? "Y" : "N");
    // Log o HRV bruto sempre no modo 0x01 para diagnóstico
    if (currentActiveMode == 0x01) {
      unsigned long modeMs = now - currentActiveModeStartedAtMs;
      Serial.print(" modeT="); Serial.print(modeMs / 1000); Serial.print("s");
      Serial.print(" hrvValid="); Serial.print(validHrv(hrv) ? "Y" : "N");
    }
    Serial.println();
  }

  if (SAVE_0X28_DEBUG_RAW && now - lastSave028 >= SAVE_028_EVERY_MS) {
    lastSave028 = now;
    String raw = toHex(data, len);
    appendMetricsPacket028ToFile(raw, packetMode, bpm, spo2, hrv, fadiga, pressaoAlta, pressaoBaixa, temp, millis());
  }
}

// ============================================================
// PARSE HISTÓRICO OPCIONAL
// ============================================================

void handleHistoryPacket(uint8_t type, uint8_t* data, size_t len) {
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
        selectedHrvRawHash = fnv1aHashBytes(data, len);
        if (!snapshot.hasHrv) updateSnapshotHrv(hrv, "0x56_AUTO_HISTORY_FALLBACK");
        if (!snapshot.hasFatigue) updateSnapshotFatigue(fatigue, "0x56_AUTO_HISTORY_FALLBACK");
        found = true;
      }
    }

    if (!found && len >= 13) {
      uint8_t hrv = data[9];
      uint8_t fatigue = data[12];
      if (validHrv(hrv) && validFatigue(fatigue)) {
        selectedHrvRawHash = fnv1aHashBytes(data, len);
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

  if (type == 0x53) {
    parseSleepBuffer(data, len);
    return;
  }
}

// ============================================================
// PARSE/DEBUG PACOTE 0x09 - pacote live que o app parece usar
// ============================================================

void handlePacket09(uint8_t* data, size_t len) {
  selectedPacket09RawHash = fnv1aHashBytes(data, len);

  if (VERBOSE_BLE_LOGS) {
    String raw = toHex(data, len);
    Serial.print("[0x09 DEBUG] len=");
    Serial.print(len);
    Serial.print(" raw=");
    Serial.println(raw);
  }

  // Protocolo 2208A doc seção 6 (0x09 reply):
  // [21]=UU heart rate | [22-23]=T1 T2 temperature | [24]=VV blood oxygen
  if (len >= 24) {
    uint8_t possibleBpm = data[21];
    float possibleTemp = parseTempLittleEndian(data[22], data[23]);

    if (validBpm(possibleBpm) && !snapshot.hasBpm) {
      updateSnapshotBpm(possibleBpm, "0x09_LIVE_PACKET");
    }

    if (validTemperature(possibleTemp) && !snapshot.hasTemperature) {
      updateSnapshotTemperature(possibleTemp, "0x09_LIVE_PACKET");
    }
  }

  // [24] = VV blood oxygen (confirmado doc 2208A)
  if (len >= 25) {
    uint8_t possibleSpo2 = data[24];
    if (validSpo2(possibleSpo2) && !snapshot.hasSpo2) {
      Serial.print("[0x09 SPO2] spo2=");
      Serial.print(possibleSpo2);
      Serial.println("% via pacote realtime 0x09");
      updateSnapshotSpo2(possibleSpo2, "0x09_LIVE_PACKET");
    }
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

  if (type == 0x13) {
    // Resposta ao comando 0x13 0x99 — data[1] = porcentagem de bateria (0-100)
    if (len >= 2) {
      Serial.print("[BATT 0x13] bateria=");
      Serial.print(data[1]);
      Serial.println("%");
      if (data[1] <= 100) {
        updateSnapshotBattery(data[1]);
      }
    }
    return;
  }

  if (type == 0x28) {
    handleActivePacket028(data, len);
    return;
  }

  if (type == 0x2B) {
    if (VERBOSE_BLE_LOGS) {
      Serial.print("[AUTO CFG] len=");
      Serial.println(len);
    }
    handleAutoConfigPacket(data, len, "");
    return;
  }

  if (type == 0x2A) {
    if (VERBOSE_BLE_LOGS) {
      Serial.println("[AUTO SET RESP]");
    }
    return;
  }

  // 0x09: durante SpO2 (modo 0x03) processa sempre — data[24]=VV=SpO2.
  // Em outros modos: só processa se ainda faltar BPM/temp (evita flood de heap).
  if (type == 0x09) {
    bool needSpo2 = (currentActiveMode == 0x03 && !snapshot.hasSpo2);
    bool needBpmTemp = (!snapshot.hasBpm || !snapshot.hasTemperature);
    if (!needSpo2 && !needBpmTemp && captureFreshActiveEnabled) {
      return;
    }
    handlePacket09(data, len);
    return;
  }

  if (type == 0x54 || type == 0x53 || type == 0x56 || type == 0x60 || type == 0x62 || type == 0x65 || type == 0x66) {
    if (!captureHistoryEnabled) {
      return;
    }

    if (VERBOSE_BLE_LOGS) {
      Serial.print("[HIST] 0x");
      Serial.print(type, HEX);
      Serial.print(" len=");
      Serial.println(len);
    }

    if (SAVE_HISTORY_RAW_DEBUG || READ_HISTORY_DEBUG) {
      String rawStr = toHex(data, len);
      char typeBuf[5];
      snprintf(typeBuf, sizeof(typeBuf), "0x%02X", type);
      appendRawPacketToFile(typeBuf, rawStr, millis());
    }

    handleHistoryPacket(type, data, len);
    return;
  }

  if (VERBOSE_BLE_LOGS) {
    Serial.print("[BLE RX] type=0x");
    Serial.print(type, HEX);
    Serial.print(" len=");
    Serial.println(len);
  }
}

// ============================================================
// CONNECT / DISCONNECT BLE
// ============================================================

void prepareForBleRadio() {
  preparePowerForBleBoot();
}

bool connectBle() {
  Serial.println("[BLE] Iniciando...");
  Serial.print("[BLE] MAC configurado (pulseira)=");
  Serial.println(DEVICE_MAC);
  printHeap("antes BLE");

  prepareForBleRadio();

  BLEDevice::init("ESP32_ACTIVE_FRESH");
  BLEDevice::setPower(ESP_PWR_LVL_N9);

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

  Serial.print("[BLE] Conectado — MAC da pulseira=");
  Serial.println(client->getPeerAddress().toString().c_str());

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
// BOOT 1: MEDIÇÃO ATIVA FRESCA
// ============================================================

// Deltas de amostras por modo ativo (evita encerrar por dados do histórico).
uint16_t bpmSamplesAtModeStart = 0;
uint16_t spo2SamplesAtModeStart = 0;
uint16_t tempSamplesAtModeStart = 0;
uint16_t hrvSamplesAtModeStart = 0;
uint16_t fatigueSamplesAtModeStart = 0;
uint16_t bpSamplesAtModeStart = 0;

bool fastTestHrvFatigueBpTargetReached() {
  return snapshot.hasHrv &&
         snapshot.hasFatigue &&
         snapshot.hasBloodPressure &&
         (snapshot.hrvSamples - hrvSamplesAtModeStart) >= MIN_HRV_SAMPLES &&
         (snapshot.fatigueSamples - fatigueSamplesAtModeStart) >= MIN_FATIGUE_SAMPLES &&
         (snapshot.bpSamples - bpSamplesAtModeStart) >= MIN_BP_SAMPLES;
}

bool fastTestBpmTempTargetReached() {
  return snapshot.hasBpm &&
         snapshot.hasTemperature &&
         (snapshot.bpmSamples - bpmSamplesAtModeStart) >= MIN_BPM_SAMPLES &&
         (snapshot.temperatureSamples - tempSamplesAtModeStart) >= MIN_TEMP_SAMPLES;
}

bool activeModeTargetReached(uint8_t mode) {
  if (mode == 0x02) {
    if (FAST_TEST_MODE) return fastTestBpmTempTargetReached();
    return snapshot.bpmSamples >= MIN_BPM_SAMPLES;
  }

  if (mode == 0x03) {
    if (FAST_TEST_MODE) return snapshot.hasSpo2;
    return snapshot.spo2Samples >= MIN_SPO2_SAMPLES;
  }

  if (mode == 0x04) {
    return snapshot.temperatureSamples >= MIN_TEMP_SAMPLES;
  }

  if (mode == 0x01) {
    if (FAST_TEST_MODE) return fastTestHrvFatigueBpTargetReached();
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

  bpmSamplesAtModeStart = snapshot.bpmSamples;
  spo2SamplesAtModeStart = snapshot.spo2Samples;
  tempSamplesAtModeStart = snapshot.temperatureSamples;
  hrvSamplesAtModeStart = snapshot.hrvSamples;
  fatigueSamplesAtModeStart = snapshot.fatigueSamples;
  bpSamplesAtModeStart = snapshot.bpSamples;
  lastReceived028Ms = millis(); // reinicia contador de silêncio

  // Para modo SpO2: liga realtime 0x09 como fonte adicional (data[24]=VV=SpO2)
  if (mode == 0x03) {
    enableRealtimeStream();
    delay(500);
  }

  startActive028(mode);

  unsigned long start = millis();
  unsigned long targetReachedAt = 0;
  unsigned long lastSpo2KickMs = 0;
  uint8_t spo2KickCount = 0;

  while (millis() - start < durationMs) {
    delay(500);

    if (heapCriticallyLow()) {
      Serial.println("[MEM] Heap crítico — encerrando modo ativo para evitar travamento.");
      saveCollectionCheckpoint("heap_baixo");
      break;
    }

    // Kick SpO2 baseado em SILÊNCIO — só kika se os pacotes 0x28 pararem de chegar.
    // Se spo2=0 mas pacotes chegam normalmente → sensor está medindo (paciente lento) → NÃO kika.
    // Se pacotes pararam há 12s → sensor apagou → kika para religar.
    // Isso garante que pacientes que demoram mais recebem o tempo total sem interrupção.
    if (mode == 0x03 && !snapshot.hasSpo2 && spo2KickCount < 3) {
      unsigned long silenceMs = millis() - lastReceived028Ms;
      if (silenceMs > 12000) {
        Serial.print("[SPO2] Silêncio de ");
        Serial.print(silenceMs / 1000);
        Serial.println("s detectado — sensor provavelmente apagou.");
        kickActiveSpo2Sensor(spo2KickCount + 1);
        lastSpo2KickMs = millis();
        lastReceived028Ms = millis(); // reseta contador após kick
        spo2KickCount++;
      }
    }

    bool reached = false;
    if (mode == 0x02) {
      if (FAST_TEST_MODE) {
        reached = fastTestBpmTempTargetReached();
      } else {
        reached = (snapshot.bpmSamples - bpmSamplesAtModeStart) >= MIN_BPM_SAMPLES;
      }
    } else if (mode == 0x03) {
      reached = (snapshot.spo2Samples - spo2SamplesAtModeStart) >= MIN_SPO2_SAMPLES;
      if (FAST_TEST_MODE) reached = reached && snapshot.hasSpo2;
    } else if (mode == 0x04) {
      reached = (snapshot.temperatureSamples - tempSamplesAtModeStart) >= MIN_TEMP_SAMPLES;
    } else if (mode == 0x01) {
      if (FAST_TEST_MODE) {
        reached = fastTestHrvFatigueBpTargetReached();
      } else {
        reached =
          (snapshot.hrvSamples - hrvSamplesAtModeStart) >= MIN_HRV_SAMPLES &&
          (snapshot.fatigueSamples - fatigueSamplesAtModeStart) >= MIN_FATIGUE_SAMPLES;
      }
    }

    if (millis() >= currentActiveModeAcceptAfterMs && reached) {
      if (targetReachedAt == 0) {
        targetReachedAt = millis();
        unsigned long modeMs = millis() - currentActiveModeStartedAtMs;
        unsigned long sessionMs = millis() - measurementSessionStartedAtMs;
        Serial.print("[FRESH] Alvo encontrado em modo=");
        Serial.print(label);
        Serial.print(" | modo +");
        Serial.print(modeMs);
        Serial.print("ms (");
        Serial.print(modeMs / 1000.0, 1);
        Serial.print("s) | sessão +");
        Serial.print(sessionMs);
        Serial.print("ms (");
        Serial.print(sessionMs / 1000.0, 1);
        Serial.println("s). Mantendo alguns segundos para estabilizar.");
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

  // Desliga realtime 0x09 se estava ativo no modo SpO2
  if (mode == 0x03) {
    disableRealtimeStream();
    delay(300);
  }

  captureFreshActiveEnabled = false;

  Serial.print("[FRESH] STOP modo=");
  Serial.println(label);
  printHeap("fim modo ativo");
  saveCollectionCheckpoint(label);
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

void logBpmTempCheck() {
  Serial.println("[CHECK] BPM + temperatura — sem ligar sensor 0x02");

  unsigned long sessionMs = millis() - measurementSessionStartedAtMs;

  if (snapshot.hasBpm) {
    unsigned long at = snapshot.bpmAtMs - measurementSessionStartedAtMs;
    Serial.print("[TIMING] BPM=");
    Serial.print(snapshot.bpm);
    Serial.print(" presente | detectado há +");
    Serial.print(at);
    Serial.print("ms (");
    Serial.print(at / 1000.0, 1);
    Serial.print("s) na sessão | agora +");
    Serial.print(sessionMs);
    Serial.println("ms");
  } else {
    Serial.println("[CHECK] BPM ausente — não vou ligar modo 0x02.");
  }

  if (snapshot.hasTemperature) {
    unsigned long at = snapshot.temperatureAtMs - measurementSessionStartedAtMs;
    Serial.print("[TIMING] TEMP=");
    Serial.print(snapshot.temperature, 1);
    Serial.print("C presente | detectado há +");
    Serial.print(at);
    Serial.print("ms (");
    Serial.print(at / 1000.0, 1);
    Serial.print("s) na sessão | agora +");
    Serial.print(sessionMs);
    Serial.println("ms");
  } else {
    Serial.println("[CHECK] Temperatura ausente — não vou ligar modo 0x02.");
  }
}

void printTimingLine(const char* name, bool has, uint32_t atMs) {
  Serial.print("[TIMING] ");
  Serial.print(name);
  Serial.print(": ");
  if (has && atMs >= measurementSessionStartedAtMs) {
    unsigned long rel = atMs - measurementSessionStartedAtMs;
    Serial.print("+");
    Serial.print(rel);
    Serial.print("ms (");
    Serial.print(rel / 1000.0, 1);
    Serial.println("s)");
  } else if (has) {
    Serial.println("presente (antes da sessão)");
  } else {
    Serial.println("ausente");
  }
}

void printTimingSummary() {
  if (measurementSessionStartedAtMs == 0) return;

  unsigned long totalMs = millis() - measurementSessionStartedAtMs;

  Serial.println();
  Serial.println("========== RESUMO TIMING (sessão) ==========");
  Serial.print("[TIMING] Duração total da coleta: +");
  Serial.print(totalMs);
  Serial.print("ms (");
  Serial.print(totalMs / 1000.0, 1);
  Serial.println("s)");

  printTimingLine("HRV", snapshot.hasHrv, snapshot.hrvAtMs);
  printTimingLine("FADIGA", snapshot.hasFatigue, snapshot.fatigueAtMs);
  printTimingLine("PRESSAO", snapshot.hasBloodPressure, snapshot.bpAtMs);
  printTimingLine("SpO2", snapshot.hasSpo2, snapshot.spo2AtMs);
  printTimingLine("BPM", snapshot.hasBpm, snapshot.bpmAtMs);
  printTimingLine("TEMP", snapshot.hasTemperature, snapshot.temperatureAtMs);
  Serial.println("==========================================");
  Serial.println();
}

void collectFreshActiveVitals() {
  Serial.println("[MODE] BOOT 1 - TEST EXPERIMENTAL v2_test");
  Serial.println("[MODE] Sequência: reset → tempo → histórico → 0x02 → 0x03 → 0x01");

  if (!connectBle()) {
    Serial.println("[MODE] Falha BLE ao coletar snapshot.");
    bleOff();
    uint64_t retrySec = brownoutSkipBoot0Count > 0 ? 15ULL : SHORT_SLEEP_SECONDS;
    goToDeepSleep(retrySec, 1);
  }

  // ── HIPÓTESE 1: reset de estado antes de qualquer medição ──
  stopAllActiveModes();
  delay(1000);

  // ── HIPÓTESE 2: sincroniza relógio da pulseira (necessário para histórico) ──
  setBraceletTime();

  clearPacketsFile();
  resetSnapshot();

  // ── Bateria ──
  Serial.println("[BATT] Tentando Battery Service BLE (0x180F)...");
  readBatteryFromService();
  if (!snapshot.hasBattery) {
    Serial.println("[BATT] 0x180F falhou, tentando comando 0x13 0x99...");
    readBattery();
    delay(1500);
  }

  measurementSessionId = bootCounter;
  measurementSessionStartedAtMs = millis();
  Serial.print("[TIMING] Sessão de coleta iniciada em millis()=");
  Serial.println(measurementSessionStartedAtMs);

  // ── HIPÓTESE 3: lê histórico 0x56 (HRV) ANTES das medições ativas ──
  // Se tiver HRV recente no histórico, economiza 85s+ de medição ativa
  Serial.println("[TEST] Lendo histórico 0x56 antes das medições ativas...");
  captureHistoryEnabled = true;
  sendCmd(0x56, 0x00, 0x00, 0x00);
  delay(HIST_DELAY_LONG_MS);
  sendCmd(0x56, 0x01, 0x00, 0x00);
  delay(HIST_DELAY_MS);
  readHistoryBpm();
  delay(HIST_DELAY_MS);
  readHistorySpo2Auto();
  delay(HIST_DELAY_MS);
  readHistoryTempAuto();
  delay(HIST_DELAY_MS);
  readHistorySleep();
  delay(HIST_DELAY_MS);
  captureHistoryEnabled = false;

  Serial.print("[TEST] Após histórico: hrv=");
  Serial.print(snapshot.hasHrv ? String(snapshot.hrv) : "null");
  Serial.print(" bpm="); Serial.print(snapshot.hasBpm ? String(snapshot.bpm) : "null");
  Serial.print(" spo2="); Serial.println(snapshot.hasSpo2 ? String(snapshot.spo2) : "null");

  if (FAST_TEST_MODE) {
    // ── HIPÓTESE 4: sequência JC Vital — 0x02 primeiro (aquece sensor) ──
    // 0x02: BPM + temp (rápido, ~10s)
    collectActiveMode(0x02, "TEST_BPM_TEMP", TEST_BPM_TEMP_MS);
    delay(GAP_BETWEEN_ACTIVE_MODES_MS);

    // 0x03: SpO2
    if (!snapshot.hasSpo2) {
      collectActiveMode(0x03, "TEST_SPO2", TEST_SPO2_MS);
      delay(GAP_BETWEEN_ACTIVE_MODES_MS);
    } else {
      Serial.println("[TEST] SpO2 já obtido do histórico — pulando modo 0x03.");
    }

    // 0x01: HRV+fadiga+PA — ÚLTIMO, sensor já aquecido
    // HIPÓTESE 5: maior warmup (ACTIVE_WARMUP_MS já estava em 2s, mantém)
    collectActiveMode(0x01, "TEST_HRV_FATIGUE_BP", TEST_HRV_FATIGUE_BP_MS);

    // Retry de histórico caso 0x01 não entregou HRV
    if (!snapshot.hasHrv || !snapshot.hasFatigue) {
      Serial.println("[TEST] HRV ainda ausente após modo ativo — tentando histórico de novo...");
      captureHistoryEnabled = true;
      sendCmd(0x56, 0x00, 0x00, 0x00);
      delay(HIST_DELAY_LONG_MS);
      sendCmd(0x56, 0x01, 0x00, 0x00);
      delay(HIST_DELAY_LONG_MS);
      captureHistoryEnabled = false;
      Serial.print("[TEST] Pós retry histórico: hrv=");
      Serial.println(snapshot.hasHrv ? String(snapshot.hrv) : "null");
    }
  } else {
    if (USE_ACTIVE_HEART_BP_REFRESH) {
      collectActiveMode(0x02, "HEART_BP_OPPORTUNISTIC", ACTIVE_HEART_BP_MS);
      delay(GAP_BETWEEN_ACTIVE_MODES_MS);
    }

    if (USE_ACTIVE_SPO2_IF_MISSING && snapshot.spo2Samples < MIN_SPO2_SAMPLES) {
      collectActiveMode(0x03, "SPO2_FALLBACK", ACTIVE_SPO2_MS);
      delay(GAP_BETWEEN_ACTIVE_MODES_MS);
    }

    if (USE_ACTIVE_TEMP_IF_MISSING && !snapshot.hasTemperature) {
      collectActiveMode(0x04, "TEMP_FALLBACK", ACTIVE_TEMP_MS);
      delay(GAP_BETWEEN_ACTIVE_MODES_MS);
    }

    if (TRY_ACTIVE_HRV_DEBUG && (!snapshot.hasHrv || !snapshot.hasFatigue)) {
      collectActiveMode(0x01, "HRV_FATIGUE_DEBUG", ACTIVE_HRV_MS);
      delay(GAP_BETWEEN_ACTIVE_MODES_MS);
    }
  }

  // Em teste rápido não faz fallback de histórico (evita flood 0x53/0x56 e heap).
  if (!FAST_TEST_MODE) {
    if (!snapshot.hasHrv || !snapshot.hasFatigue) {
      captureHistoryEnabled = true;
      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=0...");
      sendCmd(0x56, 0x00, 0x00, 0x00);
      delay(HIST_DELAY_LONG_MS);

      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=1 pos=0...");
      sendCmd(0x56, 0x01, 0x00, 0x00);
      delay(HIST_DELAY_LONG_MS);

      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=1 pos=1...");
      sendCmd(0x56, 0x01, 0x01, 0x00);
      delay(HIST_DELAY_LONG_MS);

      Serial.println("[HIST] Segunda tentativa HRV/fadiga 0x56 AA=2...");
      sendCmd(0x56, 0x02, 0x00, 0x00);
      delay(HIST_DELAY_SHORT_MS);

      captureHistoryEnabled = false;
    }

    if (!snapshot.hasSleep) {
      captureHistoryEnabled = true;
      Serial.println("[HIST] Segunda tentativa sono 0x53...");
      readHistorySleep();
      delay(HIST_DELAY_MS);
      captureHistoryEnabled = false;
    }
  }

  finalizeSnapshotFreshness();
  printTimingSummary();
  printSnapshotDebug();

  if (!snapshotComplete()) {
    Serial.println("[MODE] Snapshot INCOMPLETO — não grava nem envia para API. Faltam:");
    printSnapshotMissing();
    saveCollectionCheckpoint("incompleto");
    bleOff();
    Serial.println("[MODE] Nova tentativa de coleta em breve (boot 1).");
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
    return;
  }

  Serial.println("[MODE] Snapshot completo — OK para enviar API.");
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

  if (countPacketLines() == 0) {
    Serial.println("[HTTP] Sem pacotes (snapshot incompleto ou não gravado) — voltando para coleta.");
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
    return;
  }

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
    Serial.print("[SLEEP] Envio OK. Dormindo ");
    Serial.print(COLLECT_SLEEP_SECONDS);
    Serial.println("s e indo para boot 1 (coleta).");
    goToDeepSleep(COLLECT_SLEEP_SECONDS, 1);
  }

  Serial.println("[SLEEP] Envio falhou. Tentando reenviar no BOOT 2.");
  goToDeepSleep(SHORT_SLEEP_SECONDS, 2);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  bootCounter++;

  esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_BROWNOUT) {
    brownoutSkipBoot0Count++;
  } else if (resetReason != ESP_RST_DEEPSLEEP) {
    brownoutSkipBoot0Count = 0;
  }

  // Boot 1 = BLE: desliga BOD antes de SPIFFS/Serial pesado (evita loop infinito).
  if (bootStep == 1 || resetReason == ESP_RST_BROWNOUT) {
    preparePowerForBleBoot();
  }

  if (resetReason == ESP_RST_BROWNOUT) {
    Serial.print("[BOD] Brownout detectado (#");
    Serial.print(brownoutSkipBoot0Count);
    Serial.println("). BOD desligado — use cabo USB curto/fonte ≥500mA.");
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println(FAST_TEST_MODE
    ? "ESP32 FAST TEST - mede → API → 5s → mede | BOOT 1=COLETA | 2=API"
    : "ESP32 APP-LIKE 15MIN - BLE/WIFI ISOLADOS");
  Serial.println("BOOT 1=COLETA | BOOT 2=API (sem boot 0)");
  Serial.print("[BLE] MAC pulseira (config)=");
  Serial.println(DEVICE_MAC);
  Serial.print("[BOOT] bootCounter=");
  Serial.println(bootCounter);
  Serial.print("[BOOT] bootStep=");
  Serial.println(bootStep);
  Serial.println("========================================");

  printHeap("inicio");

  WiFi.mode(WIFI_OFF);

  if (!initStorage()) {
    Serial.println("[FATAL] SPIFFS falhou. Dormindo curto.");
    goToDeepSleep(SHORT_SLEEP_SECONDS, bootStep == 2 ? 2 : 1);
  }

  if (bootStep == 0) {
    Serial.println("[BOOT] bootStep 0 obsoleto (firmware antigo) — forçando boot 1.");
    bootStep = 1;
  }

  if (bootStep == 1) {
    collectFreshActiveVitals();
  } else if (bootStep == 2) {
    sendFileThenSleep();
  } else {
    Serial.println("[BOOT] bootStep inválido. Voltando para 1.");
    bootStep = 1;
    goToDeepSleep(SHORT_SLEEP_SECONDS, 1);
  }
}

void loop() {
  // Não usa loop.
  // Tudo roda no setup e depois entra em deep sleep.
}
