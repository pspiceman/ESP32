#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
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

const char* TOPIC_IR_CMD       = "tswell/ir/cmd";
const char* TOPIC_IR_STATUS    = "tswell/ir/status";

const char* TOPIC_433_CMD      = "tswell/433home/cmd";
const char* TOPIC_433_STATUS   = "tswell/433home/status";
const char* TOPIC_433_LOG      = "tswell/433home/log";

// ===================== Objects =====================
WiFiClient espClient;
PubSubClient mqtt(espClient);
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);

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
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t MQTT_RETRY_INTERVAL_MS = 3000;
static const uint32_t BLE_STATUS_INTERVAL_MS = 1000;
static const uint32_t BLE_HEARTBEAT_INTERVAL_MS = 2000;
static const uint32_t BLE_FIRST_RECOVER_MS = 45000UL;   // no first connection for 45s -> soft recover
static const uint32_t BLE_HARD_RECOVER_MS = 300000UL;   // 5 min disconnected after a real link -> soft recover
static const uint32_t RESET_SETTLE_MS = 700;
static const uint32_t RESET_GUARD_MS  = 3000;

uint32_t nextWiFiTryMs = 0;
uint32_t nextMQTTTryMs = 0;
uint32_t last433StatusMs = 0;
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
  {"POWER2", NEC_LIKE, 0x55CCA2FF, 32, RAW_POWER2, 67, 38},
  {"VOL-2",  NEC_LIKE, 0x55CCA8FF, 32, RAW_VOLM2,  67, 38},
  {"VOL+2",  NEC_LIKE, 0x55CC90FF, 32, RAW_VOLP2,  67, 38},
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

void publishBleSnapshot(bool connected){
  bleStableState = connected;

  // The web UI treats ts > 1700000000 as Unix epoch seconds and filters stale retained snapshots.
  // ts=0 is intentionally used until NTP becomes valid, matching the existing UI's unknown/LWT path.
  const uint32_t ts = currentUnixTimestamp();

  char payload[80];
  snprintf(payload, sizeof(payload),
           "{\"ble\":%s,\"ts\":%lu}",
           connected ? "true" : "false",
           (unsigned long)ts);

  if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload, true);
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
    publishBleSnapshot(real);
    lastBleStatusMs = now;
    if(needHeartbeat) lastBleHeartbeatMs = now;

    Serial.printf("[BLE] state=%d lastRealAgo=%lu ms\n",
                  real ? 1 : 0,
                  (bleLastRealConnMs == 0) ? 0UL : (unsigned long)(now - bleLastRealConnMs));
  }
}

void bleHealthCheck(){
  static uint32_t lastRecoverAttemptMs = 0;
  static uint8_t recoverCount = 0;

  const uint32_t now = millis();
  if(bleKeyboard.isConnected()){
    bleLastRealConnMs = now;
    recoverCount = 0;
    return;
  }

  bool shouldRecover = false;
  const char* reason = "";

  if(bleLastRealConnMs == 0){
    if((now - bleBootMs) >= BLE_FIRST_RECOVER_MS){
      shouldRecover = true;
      reason = "no initial BLE connection";
    }
  } else if((now - bleLastRealConnMs) >= BLE_HARD_RECOVER_MS){
    shouldRecover = true;
    reason = "long BLE disconnect";
  }

  if(!shouldRecover) return;

  uint32_t backoffMs = 30000UL << (recoverCount < 4 ? recoverCount : 4);
  if((now - lastRecoverAttemptMs) < backoffMs) return;

  Serial.printf("[BLE] recover: %s (backoff=%lu ms)\n", reason, (unsigned long)backoffMs);
  publish433Logf("BLE recover: %s", reason);

  bleKeyboard.end();
  delay(80);
  bleKeyboard.begin();
  delay(120);
  bleKeyboard.setBatteryLevel(100);

  publishBleSnapshot(false);
  lastRecoverAttemptMs = now;
  if(recoverCount < 10) recoverCount++;
}

// ===================== Connectivity =====================
void startWiFiIfNeeded(){
  if(WiFi.status() == WL_CONNECTED) return;
  if(millis() < nextWiFiTryMs) return;

  nextWiFiTryMs = millis() + WIFI_RETRY_INTERVAL_MS;

  WiFi.mode(WIFI_STA);
#if defined(HAS_ESP_WIFI_H)
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] begin() retry (PS_NONE)");
}

void startMQTTIfNeeded(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(mqtt.connected()) return;
  if(millis() < nextMQTTTryMs) return;

  nextMQTTTryMs = millis() + MQTT_RETRY_INTERVAL_MS;

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  char cid[32];
  snprintf(cid, sizeof(cid), "ESP32-UNI-%08lX", (unsigned long)((uint32_t)ESP.getEfuseMac()));
  Serial.print("[MQTT] connect retry... ");

  if(mqtt.connect(cid, TOPIC_MIBOX_STATUS, 0, true, "{\"ble\":false,\"ts\":0}")){
    Serial.println("OK");
    mqtt.subscribe(TOPIC_MIBOX_CMD);
    mqtt.subscribe(TOPIC_IR_CMD);
    mqtt.subscribe(TOPIC_433_CMD);
    Serial.println("[MQTT] subscribed 3 topics");
    publish433Status();
    publishIrStatus("MQTT connected");
  } else {
    Serial.print("FAIL rc=");
    Serial.println(mqtt.state());
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
  else if (strcmp(msg, "mute") == 0 || strcmp(msg, "volmute") == 0 || strcmp(msg, "volume_mute") == 0 || strcmp(msg, "vol_mute") == 0)
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
  char msg[CMD_MAX_LEN];
  if(!dequeueCmd(rfQueue, msg, sizeof(msg))) return;

  trimInPlace(msg);

  publish433Logf("CMD RX: %s", msg);

  if(strcasecmp(msg, "reset") == 0){
    requestReset("433 cmd reset");
    return;
  }

  if(resetPending) return;

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

  rf.enableReceive(RX_PIN);
  rf.enableTransmit(TX_PIN);

  WiFi.mode(WIFI_STA);
#if defined(HAS_ESP_WIFI_H)
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] begin() (PS_NONE)");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(20);
  mqtt.setSocketTimeout(5);

  publish433Log("Universal Remote Booting");
}

void loop(){
  serviceNotify();

  startWiFiIfNeeded();
  serviceTimeSync();
  startMQTTIfNeeded();

  if(mqtt.connected()) mqtt.loop();

  publishBleStatus();
  bleHealthCheck();

  processBleCmdIfAny();
  processIrCmdIfAny();
  processRfCmdIfAny();
  serviceResetIfPending();

  if(millis() - last433StatusMs > 1500){
    last433StatusMs = millis();
    publish433Status();
  }

  delay(1);
}