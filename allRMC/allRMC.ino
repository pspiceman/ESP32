/*
  COMBINED: myHome (Tuya cloud switch control) + uniRMC (BLE/IR/433 gateway) on ONE ESP32
  - Single WiFi + single MQTT client (PubSubClient)
  - MQTT callback dispatches to both modules
  - WiFi power save mode set to WIFI_PS_MIN_MODEM for BLE stability (Arduino-ESP32 core 2.0.17 style)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include <esp_bt.h>
#include <esp_gap_ble_api.h>

// -----------------------------------------------------------------------------
// NOTE (Arduino build order):
// - Keep this sketch as the ONLY .ino file in the folder, OR put shared types in a .h
// - Arduino concatenates multiple .ino files and may place functions before types.
// These explicit forward declarations help the Arduino preprocessor.
// -----------------------------------------------------------------------------
struct BleAct;
struct Cmd;
struct DeviceItem;

// Forward declarations (to avoid Arduino auto-prototype edge cases)
static bool bleEnqueue(const BleAct& a);
static bool bleDequeue(BleAct& out);
bool qPop(Cmd& out);
DeviceItem* findDeviceByKey(const String& key, int &idxOut);


// uniRMC deps
#include <BleKeyboard.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <RCSwitch.h>

// myHome deps
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
static volatile bool bleConnected = false;   // BLE connection state


// ===================== Shared WiFi / MQTT =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// Single shared MQTT client
WiFiClient espClient;
PubSubClient mqtt(espClient);

// Forward declarations for dispatch callbacks
void mhMqttCallback(char* topic, byte* payload, unsigned int length);
void urMqttCallback(char* topic, byte* payload, unsigned int length);

// Unified callback
void mqttCallback(char* topic, byte* payload, unsigned int length){
  // myHome handles: .../cmd/set, .../cmd/refresh, .../cmd/pollall
  mhMqttCallback(topic, payload, length);
  // uniRMC handles: tswell/mibox3/cmd, tswell/ir/cmd, tswell/433home/cmd
  urMqttCallback(topic, payload, length);
}

/*
  TSWell Universal Remote Gateway (ESP32)  [Arduino-ESP32 core 2.0.17 안정화]
  - BLE Keyboard (MiBox3)
  - IR Remote (send only)
  - 433MHz (RCSwitch)
  - MQTT (broker.hivemq.com:1883)

  핵심 수정 (2.0.17에서 abort 방지)
    - WiFi+BLE 동시 사용 시 WiFi Modem Sleep(PS)을 반드시 켜야 함
    - WiFi.setSleep(false) 금지
    - esp_wifi_set_ps(WIFI_PS_MIN_MODEM) 사용

  Topics
    - tswell/mibox3/cmd     (BLE key / MB: raw)
    - tswell/mibox3/status  (BLE status JSON, retain)
    - tswell/ir/cmd         (JSON {cmd})
    - tswell/ir/status      (text status)
    - tswell/433home/cmd    (plain text cmd)
    - tswell/433home/status (JSON status)
    - tswell/433home/log    (text lo

  NOTE:
    - IR 라이브러리: IRremoteESP8266
    - 433 라이브러리: RCSwitch
    - BLE: BleKeyboard..
*/


// ===== BLE =====

// ===== IR =====

// ===== 433 =====

// ---- Forward declarations (avoid Arduino auto-prototype quirks) ----
void publish433Status();
void publishBleStatus();
void publishIrStatus(const String& msg);
void blinkNotify(uint16_t ms=70);
void serviceNotify();
void serviceBleActions();
void bleHealthCheck();


// ===================== WiFi =====================

// ===================== MQTT =====================

// ===================== Topics =====================
// BLE MiBox
const char* TOPIC_MIBOX_CMD    = "tswell/mibox3/cmd";
const char* TOPIC_MIBOX_STATUS = "tswell/mibox3/status";

// IR
const char* TOPIC_IR_CMD    = "tswell/ir/cmd";
const char* TOPIC_IR_STATUS = "tswell/ir/status";

// 433
const char* TOPIC_433_CMD    = "tswell/433home/cmd";
const char* TOPIC_433_STATUS = "tswell/433home/status";
const char* TOPIC_433_LOG    = "tswell/433home/log";

// ===================== BLE Keyboard =====================
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);
static bool ur_bleStarted = false;

// Start BLE only when needed (keeps heap for myHome HTTPS/Tuya)
static void urEnsureBleStarted(){
  if(ur_bleStarted) return;

  // WiFi+BLE stability (Arduino-ESP32 2.0.17 / IDF): enable modem sleep
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  delay(150);
  bleKeyboard.begin();
  delay(300);
  bleKeyboard.setBatteryLevel(100);

  ur_bleStarted = true;
  Serial.println("[BLE] started (lazy)");
}

// BLE helpers (loop에서만 호출)  [NON-BLOCKING]
enum BleActKind : uint8_t { BLE_ACT_KEY=1, BLE_ACT_MEDIA=2 };
struct BleAct {
  uint8_t kind;
  uint8_t key;
  uint8_t b0;
  uint8_t b1;
  uint16_t holdMs;
};

static const uint8_t BLE_ACT_Q_LEN = 8;
BleAct bleActQ[BLE_ACT_Q_LEN];
uint8_t bleQHead=0, bleQTail=0;
bool bleQFull=false;

static inline bool bleQEmpty(){ return (bleQHead==bleQTail) && !bleQFull; }

static bool bleEnqueue(const BleAct& a){
  if(bleQFull) return false;
  bleActQ[bleQTail] = a;
  bleQTail = (uint8_t)((bleQTail + 1) % BLE_ACT_Q_LEN);
  if(bleQTail == bleQHead) bleQFull = true;
  return true;
}

static bool bleDequeue(BleAct& out){
  if(bleQEmpty()) return false;
  out = bleActQ[bleQHead];
  bleQHead = (uint8_t)((bleQHead + 1) % BLE_ACT_Q_LEN);
  bleQFull = false;
  return true;
}

// Queue tap (returns immediately)
static inline void tapKey(uint8_t key, uint16_t holdMs=45){
  // 연결 안 된 상태면 큐 적체/유실이 혼재될 수 있어 드롭(로그는 필요 시 추가)
  if(!bleKeyboard.isConnected()) return;
  BleAct a{BLE_ACT_KEY, key, 0,0, holdMs};
  bleEnqueue(a);
}

static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t holdMs=80){
  if(!bleKeyboard.isConnected()) return;
  BleAct a{BLE_ACT_MEDIA, 0, b0, b1, holdMs};
  bleEnqueue(a);
}

// Drives BLE actions without delay()
void serviceBleActions(){
  if(!ur_bleStarted) return;
  static bool active=false;
  static BleAct cur{};
  static uint8_t stage=0; // 0=press,1=hold,2=gap
  static uint32_t t0=0;

  uint32_t now = millis();

  if(!active){
    if(!bleDequeue(cur)) return;
    active=true; stage=0; t0=now;
  }

  if(stage==0){
    if(cur.kind==BLE_ACT_KEY){
      bleKeyboard.press(cur.key);
    }else{
      MediaKeyReport r = { cur.b0, cur.b1 };
      bleKeyboard.press(r);
    }
    t0 = now;
    stage = 1;
    return;
  }

  if(stage==1){
    if((uint32_t)(now - t0) < (uint32_t)cur.holdMs) return;
    if(cur.kind==BLE_ACT_KEY){
      bleKeyboard.release(cur.key);
    }else{
      MediaKeyReport r = { cur.b0, cur.b1 };
      bleKeyboard.release(r);
    }
    t0 = now;
    stage = 2;
    return;
  }

  // stage==2 : inter-action gap
  uint16_t gap = (cur.kind==BLE_ACT_KEY) ? 25 : 40;
  if((uint32_t)(now - t0) < (uint32_t)gap) return;
  active=false;
}

uint16_t hexToU16(String h){
  h.replace("0x",""); h.replace("0X","");
  h.trim();
  return (uint16_t) strtoul(h.c_str(), nullptr, 16);
}


// ===================== IR =====================
// Pins
const uint16_t IR_RECV_PIN     = 27;  // receiver not used now (send-only)
const uint16_t IR_SEND_PIN     = 14;
const uint16_t NOTIFY_LED_PIN  = 15; // notify LED


// Non-blocking notify LED
bool notifyActive=false;
uint32_t notifyOffAt=0;
IRrecv irrecv(IR_RECV_PIN);
decode_results results;
IRsend irsend(IR_SEND_PIN);

// ---- NEC_LIKE RAW (POWER2 / VOL-2 / VOL+2) ----
static const uint16_t RAW_POWER2[67] = {
  9068, 4468,  608, 530,  604, 1670,  604, 532,  604, 1670,  576, 554,  608, 1666,  606, 532,
  606, 1670,  602, 1662,  610, 1664,  610, 530,  604, 530,  606, 1644,  628, 1666,  608, 532,
  604, 528,  606, 1664,  610, 528,  608, 1664,  608, 532,  604, 526,  608, 526,  610, 1664,
  606, 526,  610, 1666,  604, 1662,  610, 1662,  608, 1662,  610, 1662,  612, 1664,  606, 1662,
  608, 1670,  584
};

static const uint16_t RAW_VOLM2[67] = {
  9068, 4466,  612, 526,  610, 1666,  608, 528,  608, 1664,  608, 528,  610, 1666,  608, 530,
  602, 1666,  608, 1662,  612, 1664,  608, 536,  600, 540,  596, 1666,  608, 1666,  608, 526,
  610, 526,  610, 1662,  610, 526,  612, 1662,  610, 532,  604, 1662,  612, 528,  606, 528,
  608, 528,  608, 1662,  610, 1664,  608, 1666,  608, 1666,  608, 1644,  630, 1664,  608, 1648,
  626, 1664,  610
};

static const uint16_t RAW_VOLP2[67] = {
  9066, 4448,  628, 528,  608, 1662,  612, 528,  606, 1666,  608, 528,  608, 1662,  610, 532,
  604, 1664,  608, 1666,  612, 1662,  608, 526,  608, 510,  626, 1662,  612, 1664,  608, 528,
  606, 528,  610, 1662,  610, 528,  606, 528,  610, 1668,  602, 528,  608, 528,  608, 532,
  604, 536,  598, 1670,  606, 1662,  612, 1666,  606, 1664,  610, 1644,  630, 1666,  606, 1666,
  608, 1664,  610
};

// Stored IR codes
struct IRCode {
  const char* name;
  decode_type_t protocol;
  uint64_t value;
  uint16_t bits;

  // RAW send support (optional)
  const uint16_t* raw;
  uint16_t raw_len;
  uint16_t khz;
};

IRCode irCodes[] = {
  {"POWER1", NEC,      0x20DF10EF, 32, nullptr,    0,  0},
  {"VOL-1",  NEC,      0x20DFC03F, 32, nullptr,    0,  0},
  {"VOL+1",  NEC,      0x20DF40BF, 32, nullptr,    0,  0},

  // NEC_LIKE: send RAW once (67)
  {"POWER2", NEC_LIKE, 0x55CCA2FF, 32, RAW_POWER2, 67, 38},
  {"VOL-2",  NEC_LIKE, 0x55CCA8FF, 32, RAW_VOLM2,  67, 38},
  {"VOL+2",  NEC_LIKE, 0x55CC90FF, 32, RAW_VOLP2,  67, 38},
};
const int IR_COUNT = sizeof(irCodes) / sizeof(irCodes[0]);

// ===================== 433 =====================
#define RX_PIN 13
#define TX_PIN 12
RCSwitch rf = RCSwitch();

struct RFCode {
  const char* name;
  unsigned long value;
  uint8_t bits;
  uint8_t protocol;
};

RFCode rfCodes[] = {
  {"DOOR",   12427912, 24, 1},
  {"LIGHT",   8698436, 24, 1},
  {"SPK1",  15256641, 24, 1},
  {"SPK2",  15256642, 24, 1},
};
const int RF_COUNT = sizeof(rfCodes) / sizeof(rfCodes[0]);

// ===================== MQTT Client =====================

// ===================== Timers =====================
unsigned long lastBleStatusMs = 0;
unsigned long lastBleHeartbeatMs = 0;
unsigned long bleLastOkMs = 0;
const unsigned long BLE_DISCONNECT_GRACE_MS = 12000;
bool bleStableState = false;

unsigned long last433StatusMs = 0;

// ===================== Non-blocking reconnect =====================
unsigned long nextWiFiTryMs = 0;
unsigned long nextMQTTTryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 3000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 3000;

// ===================== BLE Health (optional hard recover) =====================
const unsigned long BLE_HARD_RECOVER_MS = 5UL * 60UL * 1000UL;
unsigned long bleLastRealConnMs = 0;

// ===================== Pending command queues (1-deep) =====================
volatile bool pendingBleCmd = false;
String pendingBleMsg;

volatile bool pendingIrCmd = false;
String pendingIrMsgJson;

volatile bool pendingRfCmd = false;
String pendingRfMsg;

// ===================== Utils =====================
void publish433Log(const String& msg){
  Serial.println(msg);
  if(mqtt.connected()) mqtt.publish(TOPIC_433_LOG, msg.c_str(), false);
}

void publish433Status(){
  int wifiOk = (WiFi.status()==WL_CONNECTED)?1:0;
  int mqttOk = (mqtt.connected())?1:0;
  int rssi = (WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-999;
  String json = String("{\"wifi\":") + wifiOk + ",\"mqtt\":" + mqttOk + ",\"rssi\":" + rssi + "}";
  if(mqtt.connected()) mqtt.publish(TOPIC_433_STATUS, json.c_str(), false);
}

void publishIrStatus(const String& msg){
  if(mqtt.connected()) mqtt.publish(TOPIC_IR_STATUS, msg.c_str(), false);
}

void blinkNotify(uint16_t ms){
  digitalWrite(NOTIFY_LED_PIN, HIGH);
  notifyOffAt = millis() + ms;
  notifyActive = true;
}

void serviceNotify(){
  if(!notifyActive) return;
  if((int32_t)(millis() - notifyOffAt) >= 0){
    digitalWrite(NOTIFY_LED_PIN, LOW);
    notifyActive = false;
  }
}

// BLE status publish (흔들림 최소화)
void publishBleStatus(){
  if(millis() - lastBleStatusMs < 1000) return;
  lastBleStatusMs = millis();

  // If BLE hasn't been started yet, report disabled (so UI knows it's ready on-demand)
  if(!ur_bleStarted){
    StaticJsonDocument<192> doc;
    doc["enabled"] = false;
    doc["connected"] = false;
    doc["name"] = "MiBox3";
    String out; serializeJson(doc, out);
    mqtt.publish(TOPIC_MIBOX_STATUS, out.c_str(), true);
    return;
  }

  const unsigned long nowMs = millis();
  bool real = bleKeyboard.isConnected();
  if(real) bleLastOkMs = nowMs;

  if(real != bleConnected){
    bleConnected = real;
    Serial.printf("[BLE] connected=%d\n", (int)bleConnected);
  }

  StaticJsonDocument<256> doc;
  doc["enabled"] = true;
  doc["connected"] = bleConnected;
  doc["last_ok_ms"] = (bleLastOkMs>0)? (nowMs - bleLastOkMs) : -1;
  doc["name"] = "MiBox3";
  String out; serializeJson(doc, out);
  mqtt.publish(TOPIC_MIBOX_STATUS, out.c_str(), true);
}


void bleHealthCheck(){
  if(!ur_bleStarted) return;
  // Connected -> reset counters
  static uint32_t lastRecoverAttemptMs = 0;
  static uint8_t recoverCount = 0;

  if(bleKeyboard.isConnected()){
    bleLastRealConnMs = millis();
    recoverCount = 0;
    return;
  }

  // never connected yet -> don't do anything
  if(bleLastRealConnMs == 0) return;

  const uint32_t now = millis();
  if((now - bleLastRealConnMs) < BLE_HARD_RECOVER_MS) return;

  // Backoff: 1min,2,4,8,16... (cap at 16min)
  uint32_t backoffMs = 60000UL << (recoverCount < 4 ? recoverCount : 4);
  if(now - lastRecoverAttemptMs < backoffMs) return;

  Serial.printf("[BLE] long disconnect (%lus) -> soft recover (end/begin), backoff=%lus\n",
                (unsigned long)((now - bleLastRealConnMs)/1000UL),
                (unsigned long)(backoffMs/1000UL));

  // Soft reset BLE stack (preferred over ESP.restart)
  bleKeyboard.end();
  bleKeyboard.begin();

  lastRecoverAttemptMs = now;
  if(recoverCount < 10) recoverCount++;
}


// ===================== WiFi/MQTT tick connect (NON-BLOCKING) =====================
void startWiFiIfNeeded(){
  if(WiFi.status() == WL_CONNECTED) return;
  if(millis() < nextWiFiTryMs) return;

  nextWiFiTryMs = millis() + WIFI_RETRY_INTERVAL_MS;

  WiFi.mode(WIFI_STA);

  // core 2.0.17: WiFi+BT 동시 사용 안정/abort 방지
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] begin() retry (PS_MIN_MODEM)");
}

void startMQTTIfNeeded(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(mqtt.connected()) return;
  if(millis() < nextMQTTTryMs) return;

  nextMQTTTryMs = millis() + MQTT_RETRY_INTERVAL_MS;


  String cid = "ESP32-UNI-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] connect retry... ");

  if(mqtt.connect(cid.c_str(), TOPIC_MIBOX_STATUS, 0, true, "{\"ble\":false,\"ts\":0}")){
    Serial.println("OK");
    mqtt.subscribe(TOPIC_MIBOX_CMD);
    mqtt.subscribe(TOPIC_IR_CMD);
    mqtt.subscribe(TOPIC_433_CMD);
    Serial.println("[MQTT] subscribed 3 topics");
    publish433Status();
    publishIrStatus("MQTT connected");
  } else {
    Serial.print("FAIL rc="); Serial.println(mqtt.state());
  }
}

// ===================== Command processors (RUN IN LOOP) =====================
void processBleCmdIfAny(){
  if(!pendingBleCmd) return;
  urEnsureBleStarted();
  pendingBleCmd = false;

  String msg = pendingBleMsg;
  msg.trim();

  Serial.printf("[MiBox][RUN] %s\n", msg.c_str());

  if(!bleKeyboard.isConnected()){
    Serial.println("[BLE] NOT CONNECTED -> ignore");
    return;
  }

  if      (msg=="up")    tapKey(KEY_UP_ARROW);
  else if (msg=="down")  tapKey(KEY_DOWN_ARROW);
  else if (msg=="left")  tapKey(KEY_LEFT_ARROW);
  else if (msg=="right") tapKey(KEY_RIGHT_ARROW);

  else if (msg=="volup")   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (msg=="voldown") bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else if (msg=="mute")    bleKeyboard.write(KEY_MEDIA_MUTE);

  else if (msg=="ok1")     tapKey(KEY_RETURN);

  else if (msg=="reset")   ESP.restart();

  else if (msg.startsWith("MB:")){
    uint16_t v = hexToU16(msg.substring(3));
    uint8_t msb = (v>>8)&0xFF;
    uint8_t lsb = v & 0xFF;
    tapMediaRaw(lsb, msb);
  } else {
    Serial.println("[MiBox] Unknown cmd");
    return;
  }

  blinkNotify();
}

void processIrCmdIfAny(){
  if(!pendingIrCmd) return;
  pendingIrCmd = false;

  String msg = pendingIrMsgJson;
  msg.trim();

  Serial.printf("[IR][RUN] %s\n", msg.c_str());

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
      // RAW 우선 송신(있으면 sendRaw), 없으면 기존 send 유지
      if(irCodes[i].raw && irCodes[i].raw_len > 0){
        irsend.sendRaw(irCodes[i].raw, irCodes[i].raw_len, irCodes[i].khz);
      } else {
        irsend.send(irCodes[i].protocol, irCodes[i].value, irCodes[i].bits);
      }
      ok=true;
      break;
    }
  }

  if(ok) blinkNotify();
  publishIrStatus(ok ? String("IR sent: ")+cmd : String("Unknown cmd: ")+cmd);
}

void processRfCmdIfAny(){
  if(!pendingRfCmd) return;
  pendingRfCmd = false;

  String msg = pendingRfMsg;
  msg.trim();

  publish433Log("CMD RX: " + msg);

  if(msg.equalsIgnoreCase("reset")){
    publish433Log("RESET by MQTT");
    delay(250);
    ESP.restart();
    return;
  }

  bool ok=false;
  for(int i=0;i<RF_COUNT;i++){
    if(msg.equalsIgnoreCase(rfCodes[i].name)){
      rf.setProtocol(rfCodes[i].protocol);
      rf.setRepeatTransmit(12);
      rf.send(rfCodes[i].value, rfCodes[i].bits);

      publish433Log("RF TX " + msg +
                    " [value:" + String(rfCodes[i].value) +
                    ", bits:" + String(rfCodes[i].bits) +
                    ", proto:" + String(rfCodes[i].protocol) + "]");

      ok=true;
      blinkNotify();
      break;
    }
  }
  if(!ok) publish433Log("Unknown CMD: " + msg);
}

// ===================== MQTT Callback (STORE ONLY) =====================
void urMqttCallback(char* topic, byte* payload, unsigned int length){
  String msg; msg.reserve(length);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();
  String t(topic);

  if(t == TOPIC_MIBOX_CMD){
    Serial.printf("[MQTT][MiBox] %s\n", msg.c_str());
    pendingBleMsg = msg;
    pendingBleCmd = true;
    return;
  }

  if(t == TOPIC_IR_CMD){
    Serial.printf("[MQTT][IR] %s\n", msg.c_str());
    pendingIrMsgJson = msg;
    pendingIrCmd = true;
    return;
  }

  if(t == TOPIC_433_CMD){
    pendingRfMsg = msg;
    pendingRfCmd = true;
    return;
  }
}

// ===================== Setup/Loop =====================


// ===================== WiFi =====================

// ===================== MQTT (Public) =====================
const char*    TOPIC_ROOT  = "pspiceman/myhome";

// ===================== Tuya Cloud (Western America) =====================
String TUYA_ENDPOINT  = "https://openapi.tuyaus.com";
String TUYA_CLIENT_ID = "s7t34utavncj9mmqhdc3";
String TUYA_SECRET    = "790ce42d601a4672b778e72004ed1752";

// ===================== mDNS =====================
const char* MDNS_NAME = "myhome"; // myhome.local

// ===================== Devices =====================
struct DeviceItem {
  const char* key;
  const char* name_kr;
  const char* device_id;
  const char* code;       // "switch_1"
  bool  state_cache;
  bool  online_cache;
  bool  pending;
  uint32_t last_cmd_ms;
  String last_err;
};

DeviceItem DEVICES[] = {
  {"myDesk",   "myDesk",   "eb622bc2147e960df5e3ez", "switch_1", false, true, false, 0, ""},
  {"floor",    "장판",     "eb778d36a45024d8d0ymiq", "switch_1", false, true, false, 0, ""},
  {"tablet",   "테블릿",   "ebf7ba55e51699856boo3j", "switch_1", false, true, false, 0, ""},
  {"server",   "서버",     "eb07edb5fbddf33dc9xjhb", "switch_1", false, true, false, 0, ""},
  {"water",    "정수기",   "ebe0b0f4ff09a6297e5an3", "switch_1", false, true, false, 0, ""},
};
const int DEVICE_COUNT = sizeof(DEVICES) / sizeof(DEVICES[0]);

// ===================== Tuya token =====================
String tuya_access_token = "";
unsigned long token_expire_epoch = 0;

// debug
int g_lastTuyaHttp = 0;
String g_lastTuyaResp = "";
String g_lastTuyaErr  = "";

// token warmup
unsigned long g_lastTokenCheckMs = 0;
const unsigned long TOKEN_WARMUP_INTERVAL_MS = 30000;

// ===================== MQTT runtime =====================

String g_nodeId;
String g_topicAnnounce;
String g_topicDevices;
String g_topicLog;
String g_topicCmdAll;

unsigned long g_lastAnnounceMs = 0;
const unsigned long ANNOUNCE_INTERVAL_MS = 15000;

unsigned long g_lastDevPublishMs = 0;
const unsigned long DEV_PUBLISH_INTERVAL_MS = 12000;

// ===================== REALTIME SYNC (Tuya status poll) =====================
// ✅ Quota 절감을 위해 "상시 폴링"을 끄고, 필요할 때만(웹 오픈/새로고침/기기 제어 직후) 폴링합니다.
//
// - 웹(대시보드)에서 새로고침 시: MQTT로  .../cmd/refresh  를 publish 하면 전체 상태를 1회 동기화
// - 기기 제어(/cmd/set) 후: 해당 기기만 1~3회 확인 폴링(지연 반영 대비) 후 pending 해제
//
// (Round-robin 1개씩 폴링 유지: 한 번에 몰아서 호출하지 않도록 간격을 둠)
const unsigned long ONDEMAND_POLL_GAP_MS = 750; // on-demand 폴링 간격(기기 1개 호출 간) (quota 안정형) // on-demand 폴링 간격(기기 1개 호출 간)
unsigned long g_lastOnDemandPollMs = 0;
int g_pollIdx = 0;
int g_pollBurstRemaining = 0; // >0이면 on-demand로 DEVICE_COUNT 만큼 순차 폴링

// /cmd/set 이후 확인 폴링(해당 기기만)
int g_verifyIdx = -1;
unsigned long g_verifyDueMs = 0;
uint8_t g_verifyTry = 0;
const unsigned long VERIFY_DELAYS_MS[3] = {800, 1500, 2500};

bool g_bootSynced = false;

// ===================== Queue =====================
struct Cmd { int idx; bool on; uint32_t enq_ms; };
Cmd q[12];
int qh=0, qt=0;
bool qBusy=false;

bool qEmpty(){ return qh==qt; }
bool qFull(){ return ((qt+1)%12)==qh; }

void qPushOrReplace(int idx, bool on){
  for(int i=qh; i!=qt; i=(i+1)%12){
    if(q[i].idx==idx){ q[i].on=on; q[i].enq_ms=millis(); return; }
  }
  if(qFull()) qh=(qh+1)%12;
  q[qt]={idx,on,(uint32_t)millis()};
  qt=(qt+1)%12;
}
bool qPop(Cmd& out){
  if(qEmpty()) return false;
  out=q[qh];
  qh=(qh+1)%12;
  return true;
}

// ===================== Helpers =====================
DeviceItem* findDeviceByKey(const String& key, int &idxOut) {
  for (int i=0;i<DEVICE_COUNT;i++){
    if (key == DEVICES[i].key) { idxOut=i; return &DEVICES[i]; }
  }
  idxOut=-1;
  return nullptr;
}

// ===================== Crypto =====================
String sha256_hex(const String& data) {
  unsigned char hash[32];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  mbedtls_sha256_update(&sha, (const unsigned char*)data.c_str(), data.length());
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);

  static const char* hex="0123456789abcdef";
  String out; out.reserve(64);
  for(int i=0;i<32;i++){ out+=hex[(hash[i]>>4)&0xF]; out+=hex[hash[i]&0xF]; }
  return out;
}

String hmac_sha256_hex_upper(const String& key, const String& msg) {
  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  static const char* hex="0123456789ABCDEF";
  String out; out.reserve(64);
  for(int i=0;i<32;i++){ out+=hex[(hmacResult[i]>>4)&0xF]; out+=hex[hmacResult[i]&0xF]; }
  return out;
}

String epochMs13() {
  time_t nowSec=time(nullptr);
  uint64_t ms=(uint64_t)nowSec*1000ULL+(uint64_t)(millis()%1000);
  return String((unsigned long long)ms);
}

String makeStringToSign(const String& method, const String& pathWithQuery, const String& body) {
  return method + "\n" + sha256_hex(body) + "\n\n" + pathWithQuery;
}

String makeTuyaSign(bool isTokenApi,
                    const String& method,
                    const String& pathWithQuery,
                    const String& body,
                    const String& t_ms,
                    const String& nonce) {
  String sts = makeStringToSign(method, pathWithQuery, body);
  String base = isTokenApi
    ? (TUYA_CLIENT_ID + t_ms + nonce + sts)
    : (TUYA_CLIENT_ID + tuya_access_token + t_ms + nonce + sts);
  return hmac_sha256_hex_upper(TUYA_SECRET, base);
}

bool tuyaRequest(bool isTokenApi,
                 const String& method,
                 const String& pathWithQuery,
                 const String& body,
                 int& httpCodeOut,
                 String& respOut) {
  g_lastTuyaHttp=0; g_lastTuyaResp=""; g_lastTuyaErr="";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = TUYA_ENDPOINT + pathWithQuery;

  if(!https.begin(client,url)){
    g_lastTuyaErr="https.begin() failed";
    httpCodeOut=0; respOut="";
    return false;
  }

  String t_ms=epochMs13();
  String nonce=String((uint32_t)esp_random(),HEX)+String((uint32_t)esp_random(),HEX);
  String sign=makeTuyaSign(isTokenApi, method, pathWithQuery, body, t_ms, nonce);

  https.addHeader("client_id", TUYA_CLIENT_ID);
  https.addHeader("t", t_ms);
  https.addHeader("nonce", nonce);
  https.addHeader("sign_method", "HMAC-SHA256");
  https.addHeader("sign", sign);
  https.addHeader("Content-Type", "application/json");
  if(!isTokenApi) https.addHeader("access_token", tuya_access_token);

  int httpCode=0;
  if(method=="GET") httpCode=https.GET();
  else if(method=="POST") httpCode=https.POST((uint8_t*)body.c_str(), body.length());
  else { https.end(); g_lastTuyaErr="unsupported method"; return false; }

  httpCodeOut=httpCode;

  if(httpCode<0){
    g_lastTuyaErr=String("HTTPClient error: ")+https.errorToString(httpCode);
    respOut="";
    https.end();
    return false;
  }

  respOut=https.getString();
  https.end();

  g_lastTuyaHttp=httpCode;
  g_lastTuyaResp=respOut;
  return true;
}

bool tuyaEnsureToken() {
  unsigned long nowSec=(unsigned long)time(nullptr);
  if(tuya_access_token.length()>0 && token_expire_epoch>nowSec+60) return true;

  int code=0; String resp;
  bool okReq=tuyaRequest(true,"GET","/v1.0/token?grant_type=1","",code,resp);
  if(!okReq || !(code>=200 && code<300)){ g_lastTuyaErr="token request failed"; return false; }

  DynamicJsonDocument doc(8192);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok){ g_lastTuyaErr="token JSON parse failed"; return false; }

  bool success=doc["success"]|false;
  if(!success){
    g_lastTuyaErr=String("token success=false, msg=")+(const char*)(doc["msg"]|"");
    return false;
  }

  JsonObject result=doc["result"].as<JsonObject>();
  const char* at=result["access_token"];
  if(!at || String(at).isEmpty()){ g_lastTuyaErr="token missing access_token"; return false; }

  tuya_access_token=at;
  int expire=result["expire_time"]|result["expire"]|7200;
  token_expire_epoch=nowSec+expire;
  return true;
}

bool tuyaSetSwitch(const char* device_id, const char* codeStr, bool on, String& outResp) {
  if(!tuyaEnsureToken()){ outResp=String("token_failed: ")+g_lastTuyaErr; return false; }

  String path=String("/v1.0/iot-03/devices/")+device_id+"/commands";

  DynamicJsonDocument bodyDoc(512);
  JsonArray commands=bodyDoc.createNestedArray("commands");
  JsonObject cmd=commands.createNestedObject();
  cmd["code"]=codeStr;
  cmd["value"]=on;

  String body; serializeJson(bodyDoc, body);

  int httpCode=0; String resp;
  bool okReq=tuyaRequest(false,"POST",path,body,httpCode,resp);
  outResp=resp;
  if(!okReq) return false;

  DynamicJsonDocument doc(4096);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok) return false;
  bool success=doc["success"]|false;
  if(!success) g_lastTuyaErr=String("cmd fail: ")+(const char*)(doc["msg"]|"");
  return success;
}

// ✅ Tuya 실제 상태 읽기 (switch_1)
// GET /v1.0/iot-03/devices/{device_id}/status :contentReference[oaicite:1]{index=1}
bool tuyaGetSwitchStatus(const char* device_id, const char* codeStr, bool &valOut, bool &onlineOut){
  onlineOut = false;
  if(!tuyaEnsureToken()) { g_lastTuyaErr = String("token_failed: ") + g_lastTuyaErr; return false; }

  String path = String("/v1.0/iot-03/devices/") + device_id + "/status";
  int httpCode=0; String resp;
  bool okReq=tuyaRequest(false,"GET",path,"",httpCode,resp);
  if(!okReq) return false;

  DynamicJsonDocument doc(8192);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok){ g_lastTuyaErr="status JSON parse failed"; return false; }

  bool success = doc["success"] | false;
  if(!success){
    g_lastTuyaErr = String("status fail: ") + (const char*)(doc["msg"] | "");
    return false;
  }

  // success=true면 일단 온라인으로 판단 (더 정확히 하려면 device detail API 추가 가능)
  onlineOut = true;

  JsonArray result = doc["result"].as<JsonArray>();
  for(JsonObject st : result){
    const char* code = st["code"] | "";
    if(String(code) == String(codeStr)){
      valOut = st["value"] | false;
      return true;
    }
  }

  g_lastTuyaErr = "status code not found";
  return false;
}

// ===================== MQTT topics =====================
String makeNodeId(){
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[40];
  snprintf(buf,sizeof(buf),"node-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}

void buildTopics(){
  g_topicAnnounce = String(TOPIC_ROOT) + "/announce";
  g_topicDevices  = String(TOPIC_ROOT) + "/" + g_nodeId + "/devices";
  g_topicLog      = String(TOPIC_ROOT) + "/" + g_nodeId + "/log";
  g_topicCmdAll   = String(TOPIC_ROOT) + "/" + g_nodeId + "/cmd/#";
}

void publishAnnounce(){
  if(!mqtt.connected()) return;
  DynamicJsonDocument doc(512);
  doc["node"]=g_nodeId;
  doc["ip"]=WiFi.localIP().toString();
  doc["mdns"]=String(MDNS_NAME)+".local";
  doc["rssi"]=WiFi.RSSI();
  doc["t"]=(unsigned long)time(nullptr);
  String out; serializeJson(doc,out);
  mqtt.publish(g_topicAnnounce.c_str(), out.c_str(), true);  // retained=true
}

void publishDevices(bool retained){
  if(!mqtt.connected()) return;
  DynamicJsonDocument doc(4096);
  JsonArray arr=doc.createNestedArray("devices");
  for(int i=0;i<DEVICE_COUNT;i++){
    JsonObject d=arr.createNestedObject();
    d["key"]=DEVICES[i].key;
    d["name"]=DEVICES[i].name_kr;
    d["state"]=DEVICES[i].state_cache;
    d["online"]=DEVICES[i].online_cache;
    d["pending"]=DEVICES[i].pending;
    if(DEVICES[i].last_err.length()) d["err"]=DEVICES[i].last_err;
  }
  String out; serializeJson(doc,out);
  mqtt.publish(g_topicDevices.c_str(), out.c_str(), retained);
}


// ===================== On-demand poll triggers =====================
void requestPollAll(){
  g_pollBurstRemaining = DEVICE_COUNT;
  // g_pollIdx는 Round-robin 유지. 전체 동기화를 "항상 0부터" 하고 싶으면 아래 줄을 켜세요.
  // g_pollIdx = 0;
  g_lastOnDemandPollMs = 0;
}

void scheduleVerify(int idx){
  g_verifyIdx = idx;
  g_verifyTry = 0;
  g_verifyDueMs = millis() + VERIFY_DELAYS_MS[0];
}

void mhMqttCallback(char* topic, byte* payload, unsigned int length){
  String t(topic);
  String p; p.reserve(length+2);
  for(unsigned int i=0;i<length;i++) p+=(char)payload[i];

  // 웹 오픈/새로고침 등: 전체 상태 1회 동기화 트리거
  if(t.endsWith("/cmd/refresh") || t.endsWith("/cmd/pollall")){
    requestPollAll();
    return;
  }

  if(!t.endsWith("/cmd/set")) return;

  DynamicJsonDocument doc(256);
  if(deserializeJson(doc,p)!=DeserializationError::Ok) return;

  String key = (const char*)(doc["key"] | "");
  bool on = doc["on"] | false;

  int idx=-1;
  DeviceItem* d=findDeviceByKey(key, idx);
  if(!d) return;

  // optimistic UI + queue
  d->state_cache = on;
  d->pending = true;
  d->online_cache = true;
  d->last_err = "";
  qPushOrReplace(idx, on);

  publishDevices(true);
}

// ===================== WiFi/time/mdns =====================
void connectWiFi(){
  WiFi.mode(WIFI_STA);  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while(WiFi.status()!=WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void setupTimeNTP(){
  configTime(9*3600,0,"pool.ntp.org","time.nist.gov");
  Serial.print("NTP sync");
  for(int i=0;i<30;i++){
    time_t nowSec=time(nullptr);
    if(nowSec>1700000000){ Serial.println(". OK"); return; }
    delay(300); Serial.print(".");
  }
  Serial.println(" (not synced, continue)");
}

void setupMDNS(){
  if(!MDNS.begin(MDNS_NAME)){ Serial.println("mDNS start FAILED"); return; }
  Serial.print("mDNS started: http://"); Serial.print(MDNS_NAME); Serial.println(".local");
}

// ===================== MQTT connect =====================
bool ensureMQTT(){
  if(mqtt.connected()) return true;

  String cid = g_nodeId + "-" + String((uint32_t)esp_random(), HEX);
  Serial.print("[MQTT] connecting...");
  if(mqtt.connect(cid.c_str())){
    Serial.println("OK");
    mqtt.subscribe(g_topicCmdAll.c_str(), 0);
    // UniRMC topics
    mqtt.subscribe(TOPIC_MIBOX_CMD, 0);
    mqtt.subscribe(TOPIC_IR_CMD, 0);
    mqtt.subscribe(TOPIC_433_CMD, 0);


    publishAnnounce();
    publishDevices(true);
    delay(200);
    publishDevices(true);

    g_lastAnnounceMs = millis();
    g_lastDevPublishMs = millis();
    return true;
  }
  Serial.println("FAIL");
  return false;
}

// ===================== REAL SYNC =====================

// 부팅 직후 전 기기 1회 동기화
void syncAllOnce(){
  bool changed=false;
  for(int i=0;i<DEVICE_COUNT;i++){
    bool v=false, online=false;
    bool ok = tuyaGetSwitchStatus(DEVICES[i].device_id, DEVICES[i].code, v, online);
    if(ok){
      if(DEVICES[i].state_cache != v) changed=true;
      DEVICES[i].state_cache = v;
      DEVICES[i].online_cache = online;
      DEVICES[i].last_err = "";
    }else{
      DEVICES[i].online_cache = false;
      DEVICES[i].last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "status_failed";
    }
    delay(250); // 너무 빠른 연속호출 방지 (quota 안정형)
  }
  if(changed) publishDevices(true);
}

// 주기적으로 1개씩 상태 갱신 (Round-robin)
// (on-demand에서도 동일 함수 사용)
void pollOneDevice(){
  int i = g_pollIdx % DEVICE_COUNT;
  g_pollIdx = (g_pollIdx + 1) % DEVICE_COUNT;

  DeviceItem &d = DEVICES[i];

  bool v=false, online=false;
  bool ok = tuyaGetSwitchStatus(d.device_id, d.code, v, online);

  bool changed=false;
  String prevErr = d.last_err;
  bool prevState = d.state_cache;
  bool prevOnline = d.online_cache;
  bool prevPending = d.pending;

  if(ok){
    d.state_cache = v;
    d.online_cache = online;
    d.last_err = "";
    // 실제 상태가 읽히면 pending 해제(명령 직후 확인 폴링 포함)
    d.pending = false;
  }else{
    d.online_cache = false;
    d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "status_failed";
    // 실패했다고 pending을 바로 false로 바꾸진 않음(명령중일 수도 있어서)
  }

  if(prevState != d.state_cache) changed=true;
  if(prevOnline != d.online_cache) changed=true;
  if(prevPending != d.pending) changed=true;
  if(prevErr != d.last_err) changed=true;

  // 상태 변화(또는 pending/err 변화) 시 retained 업데이트
  if(changed) publishDevices(true);
}


// ===================== Arduino =====================


// ===================== Combined Setup/Loop =====================
void setup(){
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== COMBINED Gateway: uniRMC + myHome ===");

  // ----- myHome priority: WiFi/time/mDNS/MQTT first -----
// NOTE: myHome uses HTTPS(Tuya). BLE init can reduce free heap and break TLS,
// so we DEFER BLE start until a BLE command is actually requested.

// ----- shared WiFi -----
  connectWiFi();      // (patched) uses WIFI_PS_MIN_MODEM internally
  setupTimeNTP();
  setupMDNS();

  // ----- myHome topics -----
  g_nodeId = makeNodeId();
  buildTopics();

  // ----- shared MQTT -----
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(4096);

  ensureMQTT();

  // myHome: boot-time sync once
  syncAllOnce();
  g_bootSynced = true;
  publishDevices(true);

  // ----- uniRMC init (IR/433 only; BLE deferred) -----
  pinMode(NOTIFY_LED_PIN, OUTPUT);
  digitalWrite(NOTIFY_LED_PIN, LOW);

  // IR/RF can be ready immediately (low memory cost)
  irsend.begin(); // send-only
  rf.enableReceive(RX_PIN);
  rf.enableTransmit(TX_PIN);

  // BLE is started lazily when a BLE command arrives:
  //   urEnsureBleStarted();

  // uniRMC: boot log (will publish if MQTT already connected)
  publish433Log("Combined Boot: uniRMC + myHome (shared MQTT, PS_MIN_MODEM)");
}

void loop(){
  // 0) keep MQTT up (connect + subscribe)
  ensureMQTT();

  // 1) service MQTT network pump
  if(mqtt.connected()){
    mqtt.loop();
  }

  // 2) run myHome services (priority)
// 3) run myHome services (no mqtt.loop here)
  
  unsigned long nowMs = millis();

  // announce
  if(mqtt.connected() && (nowMs - g_lastAnnounceMs > ANNOUNCE_INTERVAL_MS)){
    g_lastAnnounceMs = nowMs;
    publishAnnounce();
  }

  // periodic snapshot (보험용)
  if(mqtt.connected() && (nowMs - g_lastDevPublishMs > DEV_PUBLISH_INTERVAL_MS)){
    g_lastDevPublishMs = nowMs;
    publishDevices(true);
  }

  // token warmup
  if(nowMs - g_lastTokenCheckMs > TOKEN_WARMUP_INTERVAL_MS){
    g_lastTokenCheckMs = nowMs;
    unsigned long nowSec=(unsigned long)time(nullptr);
    if(tuya_access_token.length()==0 || token_expire_epoch < nowSec + 180){
      tuyaEnsureToken();
    }
  }

  // ✅ On-demand Tuya poll (웹 오픈/새로고침/요청 시에만)
  if(g_pollBurstRemaining > 0 && (nowMs - g_lastOnDemandPollMs) >= ONDEMAND_POLL_GAP_MS){
    g_lastOnDemandPollMs = nowMs;

    // pollOneDevice() 내부에서 변화가 있으면 retained publish를 수행합니다.
    pollOneDevice();
    g_pollBurstRemaining--;

    // refresh 요청에서 "변화가 없어도" UI가 최소 1회 응답을 받도록,
    // burst가 끝나는 시점에만 강제 publish 1회 수행합니다(트래픽/쿼터 안정).
    if(g_pollBurstRemaining <= 0){
      publishDevices(true);
    }
  }

  // ✅ /cmd/set 이후 확인 폴링(해당 기기만, 최대 3회)
  if(g_verifyIdx >= 0 && nowMs >= g_verifyDueMs){
    DeviceItem &d = DEVICES[g_verifyIdx];
    bool v=false, online=false;
    bool ok = tuyaGetSwitchStatus(d.device_id, d.code, v, online);
    if(ok){
      d.state_cache = v;
      d.online_cache = online;
      d.last_err = "";
      d.pending = false;
      g_verifyIdx = -1;
      publishDevices(true);
    }else{
      g_verifyTry++;
      if(g_verifyTry >= 3){
        // 더 이상 재시도하지 않음
        d.pending = false;
        d.online_cache = false;
        d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "verify_failed";
        g_verifyIdx = -1;
        publishDevices(true);
      }else{
        g_verifyDueMs = nowMs + VERIFY_DELAYS_MS[g_verifyTry];
      }
    }
  }

  // process queued cmd (non-blocking)
  if(!qBusy && !qEmpty()){
    qBusy = true;
    Cmd c; qPop(c);

    if(c.idx >= 0 && c.idx < DEVICE_COUNT){
      DeviceItem &d = DEVICES[c.idx];
      String tuyaResp;

      uint32_t t0 = millis();
      bool ok = tuyaSetSwitch(d.device_id, d.code, c.on, tuyaResp);
      uint32_t t1 = millis();
      d.last_cmd_ms = (t1 - t0);

      if(ok){
        // optimistic 유지. 최종 확정은 poll이 해줌(투야 반영 지연 대비)
        d.pending = true;
        d.online_cache = true;
        d.last_err = "";
        // ✅ 제어 직후 해당 기기만 확인 폴링(Quota 최소화)
        scheduleVerify(c.idx);
      }else{
        d.pending = false;
        d.online_cache = false;
        d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "set_failed";
      }

      publishDevices(true); // UI 즉시 갱신
    }

    qBusy = false;
  }

  

  // 3) run uniRMC services (secondary)
// 2) run uniRMC services (no WiFi/MQTT connect here)
  
  // Non-blocking service tasks
  serviceNotify();
  serviceBleActions();
  // 1) Non-blocking reconnect ticks


  // 3) BLE status publish + health check
  publishBleStatus();
  bleHealthCheck();

  // 4) Process queued commands
  processBleCmdIfAny();
  processIrCmdIfAny();
  processRfCmdIfAny();

  // 5) 433 status
  if(millis()-last433StatusMs>1500){
    last433StatusMs=millis();
    publish433Status();
  }


  

  delay(1);
}