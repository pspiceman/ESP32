#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

// ESP32 family compatibility:
// Some ESP32 variants/cores, especially BLE-only targets such as ESP32-C3/S3/C6,
// do not expose esp_bt.h or Classic-BT memory-release APIs. Guard these includes
// so the sketch still compiles when BLEKeyboard is supported.
#if defined(ESP32) && __has_include("esp_wifi.h")
  #include "esp_wifi.h"
  #define HAS_ESP_WIFI_H 1
#endif

#if defined(ESP32) && __has_include(<esp_bt.h>)
  #include <esp_bt.h>
  #define HAS_ESP_BT_H 1
#endif

#if defined(ESP32) && __has_include(<esp_gap_ble_api.h>)
  #include <esp_gap_ble_api.h>
  #define HAS_ESP_GAP_BLE_API_H 1
#endif

#include <BleKeyboard.h>

// Fallback USB HID key codes used by BleKeyboard/Keyboard-style libraries.
// Some library/core combinations do not expose KEY_ESC or KEY_HOME names.
#ifndef KEY_ESC
  #define KEY_ESC 0xB1
#endif
#ifndef KEY_HOME
  #define KEY_HOME 0xD2
#endif

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#include <RCSwitch.h>

// ===================== WiFi =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===================== MQTT =====================
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// ===================== Time / NTP =====================
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
static const uint32_t VALID_EPOCH_MIN = 1700000000UL;
bool ntpConfigured = false;

// ===================== Topics =====================
const char* TOPIC_MIBOX_CMD    = "tswell/mibox3/cmd";
const char* TOPIC_MIBOX_STATUS = "tswell/mibox3/status";
const char* TOPIC_GATEWAY_STATUS = "tswell/gateway/status";  // MQTT online/offline LWT (separate from BLE status)

const char* TOPIC_IR_CMD       = "tswell/ir/cmd";
const char* TOPIC_IR_STATUS    = "tswell/ir/status";

const char* TOPIC_433_CMD      = "tswell/433home/cmd";
const char* TOPIC_433_STATUS   = "tswell/433home/status";
const char* TOPIC_433_LOG      = "tswell/433home/log";
const char* TOPIC_SCHEDULE_CMD = "tswell/schedule/cmd";
const char* TOPIC_SCHEDULE_STATUS = "tswell/schedule/status";

// ===================== Objects =====================
WiFiClient espClient;
PubSubClient mqtt(espClient);
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);
Preferences schedulePrefs;

// ===================== IR =====================
const uint16_t IR_RECV_PIN    = 27;   // receiver not used now
const uint16_t IR_SEND_PIN    = 14;
const uint16_t NOTIFY_LED_PIN = 15;

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
IRsend irsend(IR_SEND_PIN);

// ===================== 433 =====================
#define RX_PIN 13
#define TX_PIN 12
RCSwitch rf = RCSwitch();

// ===================== Timing / intervals =====================
static const uint32_t WIFI_HARD_RESTART_MS = 90000UL;  // trust ESP32 auto-reconnect first; hard restart only after 90 s
static const uint32_t MQTT_RETRY_INTERVAL_MS = 3000;
static const uint32_t BLE_STATUS_INTERVAL_MS = 1000;
static const uint32_t BLE_HEARTBEAT_INTERVAL_MS = 10000;
static const uint32_t RESET_SETTLE_MS = 700;
static const uint32_t RESET_GUARD_MS  = 3000;

uint32_t wifiDisconnectedSinceMs = 0;
uint32_t lastWiFiHardRestartMs = 0;
wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
uint32_t nextMQTTTryMs = 0;
bool lastMQTTConnected = false;
uint32_t mqttConnectCount = 0;
uint32_t mqttDisconnectedSinceMs = 0;
uint32_t last433StatusMs = 0;
uint32_t lastGatewayStatusMs = 0;
uint32_t lastBleStatusMs = 0;
uint32_t lastBleHeartbeatMs = 0;
bool bleStableState = false;
uint32_t bleBootMs = 0;
uint32_t bleLastRealConnMs = 0;
bool resetPending = false;
uint32_t resetAtMs = 0;
uint32_t lastResetRequestMs = 0;
char resetReason[64] = "";

// Notify LED timer
bool notifyActive = false;
uint32_t notifyOffAt = 0;

// ===================== IR raw codes =====================
static const uint16_t RAW_POWER2[67] = {
  9068,4468,608,530,604,1670,604,532,604,1670,576,554,608,1666,606,532,606,1670,
  602,1662,610,1664,610,530,604,530,606,1644,628,1666,608,532,604,528,606,1664,
  610,528,608,1664,608,532,604,526,608,526,610,1664,606,526,610,1666,604,1662,
  610,1662,608,1662,610,1662,612,1664,606,1662,608,1670,584
};

static const uint16_t RAW_VOLM2[67]  = {
  9068,4466,612,526,610,1666,608,528,608,1664,608,528,610,1666,608,530,602,1666,
  608,1662,612,1664,608,536,600,540,596,1666,608,1666,608,526,610,526,610,1662,
  610,526,612,1662,610,532,604,1662,612,528,606,528,608,528,608,1662,610,1664,
  608,1666,608,1666,608,1644,630,1664,608,1648,626,1664,610
};

static const uint16_t RAW_VOLP2[67]  = {
  9066,4448,628,528,608,1662,612,528,606,1666,608,528,608,1662,610,532,604,1664,
  608,1666,612,1662,608,526,608,510,626,1662,612,1664,608,528,606,528,610,1662,
  610,528,606,528,610,1668,602,528,608,528,608,532,604,536,598,1670,606,1662,
  612,1666,606,1664,610,1644,630,1666,606,1666,608,1664,610
};

struct IRCode {
  const char* name;
  decode_type_t protocol;
  uint64_t value;
  uint16_t bits;
  const uint16_t* raw;
  uint16_t raw_len;
  uint16_t khz;
};

IRCode irCodes[] = {
  {"POWER1", NEC,      0x20DF10EF, 32, nullptr,    0,  0},
  {"VOL-1",  NEC,      0x20DFC03F, 32, nullptr,    0,  0},
  {"VOL+1",  NEC,      0x20DF40BF, 32, nullptr,    0,  0},
  {"POWER2", NEC,      0xB24D3BC4, 32, nullptr,    0,  0},
  {"VOL-2",  NEC,      0xB24D817E, 32, nullptr,    0,  0},
  {"VOL+2",  NEC,      0xB24D01FE, 32, nullptr,    0,  0},
};
const int IR_COUNT = sizeof(irCodes) / sizeof(irCodes[0]);

// ===================== RF codes =====================
struct RFCode {
  const char* name;
  unsigned long value;
  uint8_t bits;
  uint8_t protocol;
};

RFCode rfCodes[] = {
  {"DOOR",  12427912, 24, 1},
  {"LIGHT",  8698436, 24, 1},
  {"SPK1",  15256641, 24, 1},
  {"SPK2",  15256642, 24, 1},
};
const int RF_COUNT = sizeof(rfCodes) / sizeof(rfCodes[0]);

// ===================== Pending command queues =====================
// Ring buffers prevent rapid button presses from overwriting each other.
static const uint8_t CMD_QUEUE_DEPTH = 12;
static const size_t CMD_MAX_LEN = 192;

struct CommandQueue {
  char items[CMD_QUEUE_DEPTH][CMD_MAX_LEN];
  uint8_t head = 0;
  uint8_t tail = 0;
  uint8_t count = 0;
  uint32_t dropped = 0;
};

CommandQueue bleQueue;
CommandQueue irQueue;
CommandQueue rfQueue;

// ===================== Daily schedules (persistent NVS) =====================
enum ScheduleKind : uint8_t { SCHED_BLE = 0, SCHED_RF = 1, SCHED_IR = 2 };

struct DailySchedule {
  const char* id;
  ScheduleKind kind;
  const char* cmd;
  bool enabled;
  uint8_t hour;       // 0..23, Korea time
  uint8_t minute;     // 0..59
  uint32_t lastRunDate; // YYYYMMDD, persisted to prevent duplicate run after reboot
};

DailySchedule dailySchedules[] = {
  {"mi_voldown", SCHED_BLE, "voldown", false, 0, 0, 0},
  {"mi_power",   SCHED_BLE, "power",   false, 0, 0, 0},
  {"mi_volup",   SCHED_BLE, "volup",   false, 0, 0, 0},
  {"rf_light",   SCHED_RF,  "LIGHT",   false, 0, 0, 0},
  {"rf_door",    SCHED_RF,  "DOOR",    false, 0, 0, 0},
  {"ir_power1",  SCHED_IR,  "POWER1",  false, 0, 0, 0},
  {"ir_power2",  SCHED_IR,  "POWER2",  false, 0, 0, 0},
};
const uint8_t DAILY_SCHEDULE_COUNT = sizeof(dailySchedules) / sizeof(dailySchedules[0]);
uint32_t lastScheduleCheckMs = 0;

// Scheduler forward declarations (keeps this sketch independent of Arduino auto-prototype quirks).
void loadSchedules();
void publishScheduleStatus();
void handleScheduleCommand(const char* msg);
void serviceDailySchedules();

// ===================== Helpers =====================
void trimInPlace(char* s){
  if(!s) return;

  size_t len = strlen(s);
  while(len > 0 && isspace((unsigned char)s[len - 1])){
    s[--len] = '\0';
  }

  char* start = s;
  while(*start && isspace((unsigned char)*start)) start++;
  if(start != s){
    memmove(s, start, strlen(start) + 1);
  }
}

void lowerInPlace(char* s){
  if(!s) return;
  for(; *s; s++) *s = (char)tolower((unsigned char)*s);
}

void copyPayloadToCString(const byte* payload, unsigned int length, char* out, size_t outSize){
  if(!out || outSize == 0) return;
  size_t n = length;
  if(n >= outSize) n = outSize - 1;
  memcpy(out, payload, n);
  out[n] = '\0';
  trimInPlace(out);
}

bool enqueueCmd(CommandQueue& q, const char* msg){
  if(!msg || !msg[0]) return false;

  if(q.count >= CMD_QUEUE_DEPTH){
    q.head = (q.head + 1) % CMD_QUEUE_DEPTH;
    q.count--;
    q.dropped++;
  }

  strncpy(q.items[q.tail], msg, CMD_MAX_LEN - 1);
  q.items[q.tail][CMD_MAX_LEN - 1] = '\0';
  q.tail = (q.tail + 1) % CMD_QUEUE_DEPTH;
  q.count++;
  return true;
}

bool dequeueCmd(CommandQueue& q, char* out, size_t outSize){
  if(q.count == 0 || !out || outSize == 0) return false;

  strncpy(out, q.items[q.head], outSize - 1);
  out[outSize - 1] = '\0';
  q.head = (q.head + 1) % CMD_QUEUE_DEPTH;
  q.count--;
  return true;
}

uint16_t hexToU16(const char* h){
  if(!h) return 0;
  while(*h && isspace((unsigned char)*h)) h++;
  if(h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
  return (uint16_t)strtoul(h, nullptr, 16);
}

bool validEpochTime(){
  time_t now = time(nullptr);
  return now > (time_t)VALID_EPOCH_MIN;
}

uint32_t currentUnixTimestamp(){
  time_t now = time(nullptr);
  if(now > (time_t)VALID_EPOCH_MIN) return (uint32_t)now;
  return 0;
}

void serviceTimeSync(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(ntpConfigured) return;

  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  ntpConfigured = true;
  Serial.println("[TIME] NTP configTime() requested");
}

static inline void tapKey(uint8_t key, uint16_t holdMs = 55){
  if(!bleKeyboard.isConnected()) return;
  bleKeyboard.press(key);
  delay(holdMs);
  bleKeyboard.release(key);
  delay(15);
}

static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t holdMs = 85){
  if(!bleKeyboard.isConnected()) return;
  MediaKeyReport r = { b0, b1 };
  bleKeyboard.press(r);
  delay(holdMs);
  bleKeyboard.release(r);
  delay(20);
}

void blinkNotify(uint16_t ms = 70){
  digitalWrite(NOTIFY_LED_PIN, HIGH);
  notifyOffAt = millis() + ms;
  notifyActive = true;
}

void serviceNotify(){
  if(notifyActive && (int32_t)(millis() - notifyOffAt) >= 0){
    digitalWrite(NOTIFY_LED_PIN, LOW);
    notifyActive = false;
  }
}

// ===================== Status publishers =====================
void publish433Status(){
  int wifiOk = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  int mqttOk = mqtt.connected() ? 1 : 0;
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -999;

  char json[80];
  snprintf(json, sizeof(json),
           "{\"wifi\":%d,\"mqtt\":%d,\"rssi\":%d}",
           wifiOk, mqttOk, rssi);

  if(mqtt.connected()) mqtt.publish(TOPIC_433_STATUS, json, false);
}

void publishIrStatus(const char* msg){
  if(mqtt.connected()) mqtt.publish(TOPIC_IR_STATUS, msg ? msg : "", false);
}

void publishIrStatusf(const char* fmt, ...){
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  publishIrStatus(buf);
}

void publish433Log(const char* msg){
  Serial.println(msg ? msg : "");
  if(mqtt.connected()) mqtt.publish(TOPIC_433_LOG, msg ? msg : "", false);
}

void publish433Logf(const char* fmt, ...){
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  publish433Log(buf);
}

void publishBleSnapshot(bool connected, bool retain = true){
  bleStableState = connected;

  // The web UI treats ts > 1700000000 as Unix epoch seconds and filters stale retained snapshots.
  // ts=0 is intentionally used until NTP becomes valid, matching the existing UI's unknown/LWT path.
  const uint32_t ts = currentUnixTimestamp();

  char payload[80];
  snprintf(payload, sizeof(payload),
           "{\"ble\":%s,\"ts\":%lu}",
           connected ? "true" : "false",
           (unsigned long)ts);

  if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload, retain);
}

void publishGatewayOnline(bool retain = true){
  if(!mqtt.connected()) return;

  const uint32_t ts = currentUnixTimestamp();
  const bool bleOk = bleKeyboard.isConnected();
  char payload[220];
  snprintf(payload, sizeof(payload),
           "{\"online\":true,\"ts\":%lu,\"rssi\":%d,\"ble\":%s,\"uptime\":%lu,\"heap\":%lu,\"mqttConnects\":%lu}",
           (unsigned long)ts,
           (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -999,
           bleOk ? "true" : "false",
           (unsigned long)millis(),
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)mqttConnectCount);
  mqtt.publish(TOPIC_GATEWAY_STATUS, payload, retain);
}

void logConnectivityEdges(){
  wl_status_t ws = WiFi.status();
  if(ws != lastWiFiStatus){
    Serial.printf("[WiFi] status %d -> %d", (int)lastWiFiStatus, (int)ws);
    if(ws == WL_CONNECTED){
      Serial.printf(" IP=%s RSSI=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    Serial.println();
    lastWiFiStatus = ws;
  }

  bool mc = mqtt.connected();
  if(mc != lastMQTTConnected){
    Serial.printf("[MQTT] connected=%d state=%d\n", mc ? 1 : 0, mqtt.state());
    lastMQTTConnected = mc;
  }
}

void requestReset(const char* reason){
  uint32_t now = millis();
  if(resetPending){
    publish433Log("RESET already pending");
    return;
  }
  if((now - lastResetRequestMs) < RESET_GUARD_MS){
    publish433Log("RESET ignored (guard window)");
    return;
  }
  lastResetRequestMs = now;
  resetPending = true;
  resetAtMs = now + RESET_SETTLE_MS;
  strncpy(resetReason, reason ? reason : "unknown", sizeof(resetReason) - 1);
  resetReason[sizeof(resetReason) - 1] = '\0';

  publish433Logf("RESET scheduled: %s", resetReason);
  publishBleSnapshot(false);
  publish433Status();
  if(mqtt.connected()) mqtt.loop();
}

void serviceResetIfPending(){
  if(!resetPending) return;
  if((int32_t)(millis() - resetAtMs) < 0) return;

  publish433Logf("RESET now: %s", resetReason);
  publishBleSnapshot(false);
  publish433Status();

  uint32_t flushUntil = millis() + 120;
  while(mqtt.connected() && (int32_t)(millis() - flushUntil) < 0){
    mqtt.loop();
    delay(2);
  }

  if(mqtt.connected()) mqtt.disconnect();
  delay(40);
  ESP.restart();
}

void publishBleStatus(){
  const uint32_t now = millis();
  bool real = bleKeyboard.isConnected();

  if(real) bleLastRealConnMs = now;

  bool needEdgePublish = (real != bleStableState);
  bool needHeartbeat = (now - lastBleHeartbeatMs) >= BLE_HEARTBEAT_INTERVAL_MS;
  bool needRateTick = (now - lastBleStatusMs) >= BLE_STATUS_INTERVAL_MS;

  if(needEdgePublish || (needHeartbeat && needRateTick)){
    // Retain only real state changes. Periodic heartbeats are non-retained to
    // avoid repeatedly rewriting retained records on the public MQTT broker.
    publishBleSnapshot(real, needEdgePublish);
    lastBleStatusMs = now;
    if(needHeartbeat) lastBleHeartbeatMs = now;

    Serial.printf("[BLE] state=%d lastRealAgo=%lu ms\n",
                  real ? 1 : 0,
                  (bleLastRealConnMs == 0) ? 0UL : (unsigned long)(now - bleLastRealConnMs));
  }
}

void bleHealthCheck(){
  // MiBox power-off / BLE disconnect is normal. Do not re-create the BLE stack.
  // The BLE library keeps advertising after disconnect, while IR/433/Wi-Fi/MQTT
  // must continue running independently.
  if(bleKeyboard.isConnected()){
    bleLastRealConnMs = millis();
  }
}

// ===================== Connectivity =====================
void startWiFiIfNeeded(){
  const uint32_t now = millis();
  const wl_status_t st = WiFi.status();

  if(st == WL_CONNECTED){
    wifiDisconnectedSinceMs = 0;
    return;
  }

  if(wifiDisconnectedSinceMs == 0){
    wifiDisconnectedSinceMs = now;
    Serial.println("[WiFi] disconnected -> waiting for ESP32 auto-reconnect");
    return;
  }

  // IMPORTANT: WiFi.setAutoReconnect(true) is already enabled. Do not also call
  // WiFi.reconnect() every few seconds; repeated forced associations can disturb
  // BLE/Wi-Fi coexistence. Only do a full Wi-Fi restart after a sustained outage.
  if((now - wifiDisconnectedSinceMs) < WIFI_HARD_RESTART_MS) return;
  if(lastWiFiHardRestartMs && (now - lastWiFiHardRestartMs) < WIFI_HARD_RESTART_MS) return;

  Serial.printf("[WiFi] offline %lu ms -> one hard restart\n",
                (unsigned long)(now - wifiDisconnectedSinceMs));
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#if defined(HAS_ESP_WIFI_H)
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiHardRestartMs = now;
  wifiDisconnectedSinceMs = now;
}

void makeMqttClientId(char* out, size_t outSize){
  const uint64_t mac = ESP.getEfuseMac();
  const uint16_t hi = (uint16_t)(mac >> 32);
  const uint32_t lo = (uint32_t)mac;
  snprintf(out, outSize, "ESP32-UNI-%04X%08lX", hi, (unsigned long)lo);
}

void startMQTTIfNeeded(){
  if(WiFi.status() != WL_CONNECTED){
    mqttDisconnectedSinceMs = 0;
    return;
  }
  if(mqtt.connected()){
    mqttDisconnectedSinceMs = 0;
    return;
  }

  const uint32_t now = millis();
  if(mqttDisconnectedSinceMs == 0) mqttDisconnectedSinceMs = now;
  if((int32_t)(now - nextMQTTTryMs) < 0) return;

  nextMQTTTryMs = now + MQTT_RETRY_INTERVAL_MS;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  // Clear any half-open TCP socket before a fresh MQTT session.
  espClient.stop();

  char cid[40];
  makeMqttClientId(cid, sizeof(cid));
  Serial.printf("[MQTT] connect retry cid=%s... ", cid);

  // LWT belongs to gateway online/offline state, not to the MiBox BLE state topic.
  // This prevents a short MQTT loss from overwriting the retained BLE state with false.
  if(mqtt.connect(cid, TOPIC_GATEWAY_STATUS, 0, true, "{\"online\":false,\"ts\":0}")){
    const uint32_t outageMs = mqttDisconnectedSinceMs ? (now - mqttDisconnectedSinceMs) : 0;
    mqttConnectCount++;
    mqttDisconnectedSinceMs = 0;
    Serial.printf("OK (connect #%lu, outage=%lu ms)\n",
                  (unsigned long)mqttConnectCount, (unsigned long)outageMs);
    mqtt.subscribe(TOPIC_MIBOX_CMD);
    mqtt.subscribe(TOPIC_IR_CMD);
    mqtt.subscribe(TOPIC_433_CMD);
    mqtt.subscribe(TOPIC_SCHEDULE_CMD);
    Serial.println("[MQTT] subscribed 4 topics");

    publishGatewayOnline();
    publishBleSnapshot(bleKeyboard.isConnected());
    publish433Status();
    publishIrStatus("MQTT connected");
    publishScheduleStatus();
  } else {
    Serial.print("FAIL rc=");
    Serial.println(mqtt.state());
  }
}

// ===================== Persistent daily schedule =====================
void scheduleKey(char prefix, uint8_t index, char* out, size_t outSize){
  snprintf(out, outSize, "%c%u", prefix, (unsigned)index);
}

void saveScheduleSlot(uint8_t index){
  if(index >= DAILY_SCHEDULE_COUNT) return;
  char key[8];
  scheduleKey('e', index, key, sizeof(key)); schedulePrefs.putBool(key, dailySchedules[index].enabled);
  scheduleKey('h', index, key, sizeof(key)); schedulePrefs.putUChar(key, dailySchedules[index].hour);
  scheduleKey('m', index, key, sizeof(key)); schedulePrefs.putUChar(key, dailySchedules[index].minute);
  scheduleKey('d', index, key, sizeof(key)); schedulePrefs.putUInt(key, dailySchedules[index].lastRunDate);
}

void loadSchedules(){
  if(!schedulePrefs.begin("unirmcSched", false)){
    Serial.println("[SCHED] Preferences begin failed");
    return;
  }

  for(uint8_t i=0;i<DAILY_SCHEDULE_COUNT;i++){
    char key[8];
    scheduleKey('e', i, key, sizeof(key)); dailySchedules[i].enabled = schedulePrefs.getBool(key, false);
    scheduleKey('h', i, key, sizeof(key)); dailySchedules[i].hour = schedulePrefs.getUChar(key, 0);
    scheduleKey('m', i, key, sizeof(key)); dailySchedules[i].minute = schedulePrefs.getUChar(key, 0);
    scheduleKey('d', i, key, sizeof(key)); dailySchedules[i].lastRunDate = schedulePrefs.getUInt(key, 0);

    if(dailySchedules[i].hour > 23) dailySchedules[i].hour = 0;
    if(dailySchedules[i].minute > 59) dailySchedules[i].minute = 0;
  }

  Serial.println("[SCHED] persistent schedules loaded");
}

int findScheduleIndex(const char* id){
  if(!id) return -1;
  for(uint8_t i=0;i<DAILY_SCHEDULE_COUNT;i++){
    if(strcmp(dailySchedules[i].id, id) == 0) return (int)i;
  }
  return -1;
}

void publishScheduleStatus(){
  if(!mqtt.connected()) return;

  StaticJsonDocument<512> doc;
  JsonArray items = doc.createNestedArray("items");
  for(uint8_t i=0;i<DAILY_SCHEDULE_COUNT;i++){
    JsonObject o = items.createNestedObject();
    o["id"] = dailySchedules[i].id;
    o["e"] = dailySchedules[i].enabled ? 1 : 0;
    o["h"] = dailySchedules[i].hour;
    o["m"] = dailySchedules[i].minute;
  }

  char payload[430];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if(n > 0 && n < sizeof(payload)){
    mqtt.publish(TOPIC_SCHEDULE_STATUS, payload, true);
  }
}

void handleScheduleCommand(const char* msg){
  StaticJsonDocument<256> doc;
  if(deserializeJson(doc, msg)){
    Serial.println("[SCHED] JSON parse failed");
    return;
  }

  const char* op = doc["op"] | "";
  if(strcmp(op, "get") == 0){
    publishScheduleStatus();
    return;
  }

  const char* id = doc["id"] | "";
  int idx = findScheduleIndex(id);
  if(idx < 0){
    Serial.printf("[SCHED] unknown id: %s\n", id);
    return;
  }

  if(strcmp(op, "cancel") == 0){
    dailySchedules[idx].enabled = false;
    dailySchedules[idx].lastRunDate = 0;
    saveScheduleSlot((uint8_t)idx);
    Serial.printf("[SCHED] cancel %s\n", dailySchedules[idx].id);
    publishScheduleStatus();
    return;
  }

  if(strcmp(op, "set") == 0){
    int hour = doc["hour"] | -1;
    int minute = doc["minute"] | -1;
    if(hour < 0 || hour > 23 || minute < 0 || minute > 59){
      Serial.printf("[SCHED] invalid time for %s\n", dailySchedules[idx].id);
      return;
    }

    dailySchedules[idx].hour = (uint8_t)hour;
    dailySchedules[idx].minute = (uint8_t)minute;
    dailySchedules[idx].enabled = true;
    dailySchedules[idx].lastRunDate = 0;
    saveScheduleSlot((uint8_t)idx);
    Serial.printf("[SCHED] set %s -> %02d:%02d KST daily\n", dailySchedules[idx].id, hour, minute);
    publishScheduleStatus();
  }
}

bool getKoreaLocalTime(struct tm* out){
  if(!out || !validEpochTime()) return false;
  time_t now = time(nullptr);
  now += 9 * 60 * 60; // KST = UTC+9, no DST
  gmtime_r(&now, out);
  return true;
}

uint32_t koreaDateKey(const struct tm& t){
  return (uint32_t)(t.tm_year + 1900) * 10000UL +
         (uint32_t)(t.tm_mon + 1) * 100UL +
         (uint32_t)t.tm_mday;
}

void runScheduledAction(uint8_t index, uint32_t dateKey){
  if(index >= DAILY_SCHEDULE_COUNT) return;
  DailySchedule& s = dailySchedules[index];

  // Mark and persist first so a reboot in the same minute cannot execute twice.
  s.lastRunDate = dateKey;
  saveScheduleSlot(index);

  if(s.kind == SCHED_BLE){
    if(!bleKeyboard.isConnected()){
      Serial.printf("[SCHED] %s skipped: BLE not connected\n", s.id);
      publish433Logf("SCHED %s skipped: BLE OFF", s.id);
      return;
    }
    enqueueCmd(bleQueue, s.cmd);
  }else if(s.kind == SCHED_RF){
    enqueueCmd(rfQueue, s.cmd);
  }else if(s.kind == SCHED_IR){
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"cmd\":\"%s\"}", s.cmd);
    enqueueCmd(irQueue, payload);
  }

  Serial.printf("[SCHED] RUN %s (%s)\n", s.id, s.cmd);
  publish433Logf("SCHED RUN %s", s.id);
}

void serviceDailySchedules(){
  const uint32_t nowMs = millis();
  if((nowMs - lastScheduleCheckMs) < 500) return;
  lastScheduleCheckMs = nowMs;

  struct tm local;
  if(!getKoreaLocalTime(&local)) return;
  const uint32_t today = koreaDateKey(local);

  for(uint8_t i=0;i<DAILY_SCHEDULE_COUNT;i++){
    DailySchedule& s = dailySchedules[i];
    if(!s.enabled) continue;
    if(s.lastRunDate == today) continue;
    if(local.tm_hour == s.hour && local.tm_min == s.minute){
      runScheduledAction(i, today);
    }
  }
}

// ===================== Command processors =====================
void processBleCmdIfAny(){
  if(resetPending) return;

  char msg[CMD_MAX_LEN];
  if(!dequeueCmd(bleQueue, msg, sizeof(msg))) return;

  trimInPlace(msg);
  lowerInPlace(msg);

  Serial.printf("[MiBox][RUN] %s\n", msg);

  if(strcmp(msg, "reset") == 0){
    requestReset("MiBox cmd reset");
    return;
  }

  if(!bleKeyboard.isConnected()){
    Serial.println("[BLE] NOT CONNECTED -> ignore");
    publishBleSnapshot(false);
    return;
  }

  if      (strcmp(msg, "up") == 0)    tapKey(KEY_UP_ARROW);
  else if (strcmp(msg, "down") == 0)  tapKey(KEY_DOWN_ARROW);
  else if (strcmp(msg, "left") == 0)  tapKey(KEY_LEFT_ARROW);
  else if (strcmp(msg, "right") == 0) tapKey(KEY_RIGHT_ARROW);

  // MiBox / Android TV BLE keys.
  // VOL+/VOL-/MUTE are restored to the exact original working method from uniRMC(14).ino.
  // Extra aliases are accepted for panel-style command labels.
  else if (strcmp(msg, "volup") == 0 || strcmp(msg, "vol+") == 0 || strcmp(msg, "volumeup") == 0 || strcmp(msg, "volume_up") == 0 || strcmp(msg, "volume+") == 0)
    bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (strcmp(msg, "voldown") == 0 || strcmp(msg, "vol-") == 0 || strcmp(msg, "volumedown") == 0 || strcmp(msg, "volume_down") == 0 || strcmp(msg, "volume-") == 0)
    bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  // Xiaomi Mi Box S 3rd Gen POWER key.
  // IMPORTANT: the supplied BleKeyboard POWER patch changes media bit 4
  // from Consumer Mute (0xE2) to Consumer Power (0x30).
  // KEY_MEDIA_MUTE is therefore intentionally used as the bit-4 carrier here.
  else if (strcmp(msg, "power") == 0 || strcmp(msg, "pwr") == 0 || strcmp(msg, "powerkey") == 0)
    bleKeyboard.write(KEY_MEDIA_MUTE);

  // BACK/HOME are sent as normal keyboard HID keys, which Android TV/MiBox usually maps correctly.
  // Raw consumer usages are still available through mb:0224 and mb:0223 if needed.
  else if (strcmp(msg, "back") == 0 || strcmp(msg, "return") == 0 || strcmp(msg, "prev") == 0)
    tapKey(KEY_ESC);
  else if (strcmp(msg, "home") == 0 || strcmp(msg, "homepage") == 0 || strcmp(msg, "launcher") == 0)
    tapKey(KEY_HOME);

  else if (strcmp(msg, "ok1") == 0 || strcmp(msg, "ok") == 0 || strcmp(msg, "enter") == 0) tapKey(KEY_RETURN);

  else if (strncmp(msg, "mb:", 3) == 0){
    uint16_t v = hexToU16(msg + 3);
    uint8_t msb = (v >> 8) & 0xFF;
    uint8_t lsb = v & 0xFF;
    tapMediaRaw(lsb, msb);
  } else {
    Serial.println("[MiBox] Unknown cmd");
    return;
  }

  blinkNotify();
}

void processIrCmdIfAny(){
  if(resetPending) return;

  char msg[CMD_MAX_LEN];
  if(!dequeueCmd(irQueue, msg, sizeof(msg))) return;

  trimInPlace(msg);

  Serial.printf("[IR][RUN] %s\n", msg);

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc, msg)){
    publishIrStatus("JSON parse failed");
    return;
  }

  const char* cmd = doc["cmd"];
  if(!cmd){
    publishIrStatus("Invalid JSON: missing cmd");
    return;
  }

  bool ok=false;
  for(int i=0;i<IR_COUNT;i++){
    if(strcmp(irCodes[i].name, cmd)==0){
      if(irCodes[i].raw && irCodes[i].raw_len>0){
        irsend.sendRaw(irCodes[i].raw, irCodes[i].raw_len, irCodes[i].khz);
      }else{
        irsend.send(irCodes[i].protocol, irCodes[i].value, irCodes[i].bits);
      }
      ok=true;
      break;
    }
  }

  if(ok) blinkNotify();
  publishIrStatusf(ok ? "IR sent: %s" : "Unknown cmd: %s", cmd);
}

void processRfCmdIfAny(){
  // Match BLE/IR behavior: while reset is pending, leave queued RF commands untouched.
  if(resetPending) return;

  char msg[CMD_MAX_LEN];
  if(!dequeueCmd(rfQueue, msg, sizeof(msg))) return;

  trimInPlace(msg);

  publish433Logf("CMD RX: %s", msg);

  if(strcasecmp(msg, "reset") == 0){
    requestReset("433 cmd reset");
    return;
  }

  bool ok=false;
  for(int i=0;i<RF_COUNT;i++){
    if(strcasecmp(msg, rfCodes[i].name) == 0){
      rf.setProtocol(rfCodes[i].protocol);
      rf.setRepeatTransmit(12);
      rf.send(rfCodes[i].value, rfCodes[i].bits);

      publish433Logf("RF TX %s [value:%lu, bits:%u, proto:%u]",
                     msg,
                     rfCodes[i].value,
                     rfCodes[i].bits,
                     rfCodes[i].protocol);

      ok=true;
      blinkNotify();
      break;
    }
  }

  if(!ok) publish433Logf("Unknown CMD: %s", msg);
}

// ===================== MQTT callback =====================
void mqttCallback(char* topic, byte* payload, unsigned int length){
  char msg[CMD_MAX_LEN];
  copyPayloadToCString(payload, length, msg, sizeof(msg));
  if(!msg[0]) return;

  if(strcmp(topic, TOPIC_SCHEDULE_CMD)==0){
    handleScheduleCommand(msg);
    return;
  }

  if(strcmp(topic, TOPIC_MIBOX_CMD)==0){
    uint32_t droppedBefore = bleQueue.dropped;
    enqueueCmd(bleQueue, msg);
    Serial.printf("[MQTT][MiBox] %s%s\n", msg, (bleQueue.dropped != droppedBefore) ? " (queue full: dropped oldest)" : "");
    return;
  }

  if(strcmp(topic, TOPIC_IR_CMD)==0){
    uint32_t droppedBefore = irQueue.dropped;
    enqueueCmd(irQueue, msg);
    Serial.printf("[MQTT][IR] %s%s\n", msg, (irQueue.dropped != droppedBefore) ? " (queue full: dropped oldest)" : "");
    return;
  }

  if(strcmp(topic, TOPIC_433_CMD)==0){
    uint32_t droppedBefore = rfQueue.dropped;
    enqueueCmd(rfQueue, msg);
    Serial.printf("[MQTT][433] %s%s\n", msg, (rfQueue.dropped != droppedBefore) ? " (queue full: dropped oldest)" : "");
    return;
  }
}

// ===================== Setup / Loop =====================
void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== TSWell Universal Remote Gateway ===");

  loadSchedules();

  pinMode(NOTIFY_LED_PIN, OUTPUT);
  digitalWrite(NOTIFY_LED_PIN, LOW);

  // Original ESP32 only: release Classic BT memory.
  // Skip this on BLE-only ESP32 variants where esp_bt.h is not available.
#if defined(HAS_ESP_BT_H) && defined(CONFIG_IDF_TARGET_ESP32)
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif

  // Increase BLE TX power when the ESP-IDF BLE GAP API is available.
#if defined(HAS_ESP_GAP_BLE_API_H)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
#endif

  delay(150);
  bleBootMs = millis();
  bleKeyboard.begin();
  delay(300);
  bleKeyboard.setBatteryLevel(100);
  Serial.println("[BLE] begin() + optional btmem_release/tx_power");

  irsend.begin();

  // 433 RX is not used in this firmware. Keep RX disabled to avoid unnecessary interrupts.
  rf.enableTransmit(TX_PIN);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#if defined(HAS_ESP_WIFI_H)
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiDisconnectedSinceMs = millis();
  lastWiFiStatus = WiFi.status();
  Serial.println("[WiFi] begin() (auto-reconnect, PS_NONE)");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(768);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);

  publish433Log("Universal Remote Booting");
}

void loop(){
  serviceNotify();

  startWiFiIfNeeded();
  serviceTimeSync();
  serviceDailySchedules();
  startMQTTIfNeeded();

  if(mqtt.connected()) mqtt.loop();
  logConnectivityEdges();

  publishBleStatus();
  bleHealthCheck();

  // BLE availability must never delay IR / 433 processing.
  processIrCmdIfAny();
  processRfCmdIfAny();
  processBleCmdIfAny();
  serviceResetIfPending();

  if(millis() - last433StatusMs > 5000){
    last433StatusMs = millis();
    publish433Status();
  }
  if(millis() - lastGatewayStatusMs > 10000){
    lastGatewayStatusMs = millis();
    // Heartbeat only; retained gateway state is written on MQTT connect.
    publishGatewayOnline(false);
  }

  delay(1);
}