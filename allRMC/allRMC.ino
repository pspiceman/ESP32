/*
  allRMC (uniRMC + myHome) merged for ESP32
  - BLE keyboard (MiBox)
  - IR send
  - 433MHz (RCSwitch)
  - Tuya Cloud control (HTTPS + HMAC)
  - MQTT

  IMPORTANT:
    - 웹(HTTP 서버/웹UI) 미포함 (별도 운용)
    - Arduino IDE 자동 프로토타입 꼬임 방지:
      -> 타입/전역변수 최상단 + 함수 프로토타입 수동 선언
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include <esp_bt.h>
#include <esp_gap_ble_api.h>

#include <ArduinoJson.h>
#include <BleKeyboard.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#include <RCSwitch.h>

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ============================================================================
// 1) TYPES (MUST BE BEFORE ANY FUNCTION DECLARATIONS/DEFINITIONS)
// ============================================================================

// ----- myHome types -----
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

struct Cmd { int idx; bool on; uint32_t enq_ms; };

// ----- BLE action queue types -----
enum BleActKind : uint8_t { BLE_ACT_KEY=1, BLE_ACT_MEDIA=2 };

struct BleAct {
  uint8_t kind;
  uint8_t key;
  uint8_t b0;
  uint8_t b1;
  uint16_t holdMs;
};

// ----- 433 table -----
struct RFCode {
  const char* name;
  unsigned long value;
  uint8_t bits;
  uint8_t protocol;
};

// ----- IR table -----
struct IRCode {
  const char* name;
  decode_type_t protocol;
  uint64_t value;
  uint16_t bits;
  const uint16_t* raw;
  uint16_t raw_len;
  uint16_t khz;
};

// ============================================================================
// 2) CONFIG / GLOBALS (ALSO BEFORE FUNCTIONS)
// ============================================================================

// WiFi
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// MQTT
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// uniRMC topics
const char* TOPIC_MIBOX_CMD    = "tswell/mibox3/cmd";
const char* TOPIC_MIBOX_STATUS = "tswell/mibox3/status";
const char* TOPIC_IR_CMD       = "tswell/ir/cmd";
const char* TOPIC_IR_STATUS    = "tswell/ir/status";
const char* TOPIC_433_CMD      = "tswell/433home/cmd";
const char* TOPIC_433_STATUS   = "tswell/433home/status";
const char* TOPIC_433_LOG      = "tswell/433home/log";

// myHome root
const char* TOPIC_ROOT = "pspiceman/myhome";

// mDNS
const char* MDNS_NAME = "myhome";

// Clients
WiFiClient espClient;
PubSubClient mqtt(espClient);

// BLE
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);

// BLE action queue storage
static const uint8_t BLE_ACT_Q_LEN = 8;
static BleAct bleActQ[BLE_ACT_Q_LEN];
static uint8_t bleQHead=0, bleQTail=0;
static bool bleQFull=false;

// IR pins
const uint16_t IR_RECV_PIN     = 27; // (unused, send-only)
const uint16_t IR_SEND_PIN     = 14;
const uint16_t NOTIFY_LED_PIN  = 15;

// IR objects
IRrecv irrecv(IR_RECV_PIN);
decode_results results;
IRsend irsend(IR_SEND_PIN);

// notify
bool notifyActive=false;
uint32_t notifyOffAt=0;

// 433
#define RX_PIN 13
#define TX_PIN 12
RCSwitch rf = RCSwitch();

RFCode rfCodes[] = {
  {"DOOR",   12427912, 24, 1},
  {"LIGHT",   8698436, 24, 1},
  {"SPK1",  15256641, 24, 1},
  {"SPK2",  15256642, 24, 1},
};
const int RF_COUNT = sizeof(rfCodes) / sizeof(rfCodes[0]);

// IR RAW (NEC_LIKE)
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

IRCode irCodes[] = {
  {"POWER1", NEC,      0x20DF10EF, 32, nullptr,    0,  0},
  {"VOL-1",  NEC,      0x20DFC03F, 32, nullptr,    0,  0},
  {"VOL+1",  NEC,      0x20DF40BF, 32, nullptr,    0,  0},
  {"POWER2", NEC_LIKE, 0x55CCA2FF, 32, RAW_POWER2, 67, 38},
  {"VOL-2",  NEC_LIKE, 0x55CCA8FF, 32, RAW_VOLM2,  67, 38},
  {"VOL+2",  NEC_LIKE, 0x55CC90FF, 32, RAW_VOLP2,  67, 38},
};
const int IR_COUNT = sizeof(irCodes) / sizeof(irCodes[0]);

// myHome Tuya config
String TUYA_ENDPOINT  = "https://openapi.tuyaus.com";
String TUYA_CLIENT_ID = "4q9fg8hve79wxevhgtc4";
String TUYA_SECRET    = "32a2fc7ed7924b3ea0a1baccbdabfc64";

// devices
DeviceItem DEVICES[] = {
  {"myDesk",   "myDesk",   "eb622bc2147e960df5e3ez", "switch_1", false, true, false, 0, ""},
  {"floor",    "장판",     "eb778d36a45024d8d0ymiq", "switch_1", false, true, false, 0, ""},
  {"tablet",   "테블릿",   "ebf7ba55e51699856boo3j", "switch_1", false, true, false, 0, ""},
  {"server",   "서버",     "eb07edb5fbddf33dc9xjhb", "switch_1", false, true, false, 0, ""},
  {"water",    "정수기",   "ebe0b0f4ff09a6297e5an3", "switch_1", false, true, false, 0, ""},
};
const int DEVICE_COUNT = sizeof(DEVICES) / sizeof(DEVICES[0]);

// Tuya token state
String tuya_access_token = "";
unsigned long token_expire_epoch = 0;

// myHome topics
String g_nodeId;
String g_topicAnnounce;
String g_topicDevices;
String g_topicCmdSet;

// timers
unsigned long g_lastAnnounceMs = 0;
const unsigned long ANNOUNCE_INTERVAL_MS = 15000;
unsigned long g_lastDevPublishMs = 0;
const unsigned long DEV_PUBLISH_INTERVAL_MS = 12000;

const unsigned long STATUS_POLL_INTERVAL_MS = 1800;
unsigned long g_lastStatusPollMs = 0;
int g_pollIdx = 0;

unsigned long g_lastTokenCheckMs = 0;
const unsigned long TOKEN_WARMUP_INTERVAL_MS = 30000;

// connection retries
unsigned long nextWiFiTryMs = 0;
unsigned long nextMQTTTryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 3000;

// init flags
bool didTimeStart=false;
bool didMDNS=false;
bool didTuyaBootSync=false;

// BLE status/health
unsigned long lastBleStatusMs = 0;
unsigned long lastBleHeartbeatMs = 0;
unsigned long bleLastOkMs = 0;
const unsigned long BLE_DISCONNECT_GRACE_MS = 12000;
bool bleStableState = false;

const unsigned long BLE_HARD_RECOVER_MS = 5UL * 60UL * 1000UL;
unsigned long bleLastRealConnMs = 0;

// uniRMC pending 1-deep
volatile bool pendingBleCmd = false;
String pendingBleMsg;

volatile bool pendingIrCmd = false;
String pendingIrMsgJson;

volatile bool pendingRfCmd = false;
String pendingRfMsg;

// ===== Tuya Task (low priority) =====
static TaskHandle_t tuyaTaskHandle = nullptr;
static QueueHandle_t tuyaQueue = nullptr;

struct TuyaJob {
  int idx;
  bool on;
  uint32_t enq_ms;
};

// ============================================================================
// 3) FUNCTION PROTOTYPES (MANUAL, TO PREVENT ARDUINO AUTO-PROTOTYPE ISSUES)
// ============================================================================
static inline bool bleQEmpty();
static bool bleEnqueue(const BleAct& a);
static bool bleDequeue(BleAct& out);
static inline void tapKey(uint8_t key, uint16_t holdMs=45);
static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t holdMs=80);
static void serviceBleActions();
static uint16_t hexToU16(String h);

static void blinkNotify(uint16_t ms=70);
static void serviceNotify();
static void publishIrStatus(const String& msg);

static void publish433Log(const String& msg);
static void publish433Status();

static void tuyaTask(void* pv);

static DeviceItem* findDeviceByKey(const String& key, int &idxOut);

static String sha256_hex(const String& data);
static String hmac_sha256_hex_upper(const String& key, const String& msg);
static String epochMs13();
static String makeStringToSign(const String& method, const String& pathWithQuery, const String& body);
static String makeTuyaSign(bool isTokenApi, const String& method, const String& pathWithQuery,
                           const String& body, const String& t_ms, const String& nonce);
static bool tuyaRequest(bool isTokenApi, const String& method, const String& pathWithQuery,
                        const String& body, int& httpCodeOut, String& respOut);
static bool tuyaEnsureToken();
static bool tuyaSetSwitch(const char* device_id, const char* codeStr, bool on, String& outResp);
static bool tuyaGetSwitchStatus(const char* device_id, const char* codeStr, bool &valOut, bool &onlineOut);

static String makeNodeId();
static void buildTopics();
static bool timeIsSynced();
static void publishAnnounce();
static void publishDevices(bool retained);
static void syncAllOnce();
static void pollOneDevice();

static void startWiFiIfNeeded();
static void startTimeIfPossible();
static void startMDNSIfPossible();
static void publishBleStatus();
static void bleHealthCheck();
static void startMQTTIfNeeded();

static void processBleCmdIfAny();
static void processIrCmdIfAny();
static void processRfCmdIfAny();

static void handleMyHomeCmdSet(const String& payload);
static void mqttCallback(char* topic, byte* payload, unsigned int length);

// ============================================================================
// 4) FUNCTION IMPLEMENTATIONS
// ============================================================================

// ----- BLE queue helpers -----
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

static inline void tapKey(uint8_t key, uint16_t holdMs){
  if(!bleKeyboard.isConnected()) return;
  BleAct a{BLE_ACT_KEY, key, 0,0, holdMs};
  bleEnqueue(a);
}

static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t holdMs){
  if(!bleKeyboard.isConnected()) return;
  BleAct a{BLE_ACT_MEDIA, 0, b0, b1, holdMs};
  bleEnqueue(a);
}

static void serviceBleActions(){
  static bool active=false;
  static BleAct cur{};
  static uint8_t stage=0;
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

  uint16_t gap = (cur.kind==BLE_ACT_KEY) ? 25 : 40;
  if((uint32_t)(now - t0) < (uint32_t)gap) return;
  active=false;
}

static uint16_t hexToU16(String h){
  h.replace("0x",""); h.replace("0X","");
  h.trim();
  return (uint16_t) strtoul(h.c_str(), nullptr, 16);
}

// ----- Notify -----
static void blinkNotify(uint16_t ms){
  digitalWrite(NOTIFY_LED_PIN, HIGH);
  notifyOffAt = millis() + ms;
  notifyActive = true;
}

static void serviceNotify(){
  if(!notifyActive) return;
  if((int32_t)(millis() - notifyOffAt) >= 0){
    digitalWrite(NOTIFY_LED_PIN, LOW);
    notifyActive = false;
  }
}

// ----- IR -----
static void publishIrStatus(const String& msg){
  if(mqtt.connected()) mqtt.publish(TOPIC_IR_STATUS, msg.c_str(), false);
}

// ----- 433 -----
static void publish433Log(const String& msg){
  Serial.println(msg);
  if(mqtt.connected()) mqtt.publish(TOPIC_433_LOG, msg.c_str(), false);
}

static void publish433Status(){
  int wifiOk = (WiFi.status()==WL_CONNECTED)?1:0;
  int mqttOk = (mqtt.connected())?1:0;
  int rssi = (WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-999;
  String json = String("{\"wifi\":") + wifiOk + ",\"mqtt\":" + mqttOk + ",\"rssi\":" + rssi + "}";
  if(mqtt.connected()) mqtt.publish(TOPIC_433_STATUS, json.c_str(), false);
}

// // ----- myHome find device -----
static DeviceItem* findDeviceByKey(const String& key, int &idxOut) {
  for (int i=0;i<DEVICE_COUNT;i++){
    if (key == DEVICES[i].key) { idxOut=i; return &DEVICES[i]; }
  }
  idxOut=-1;
  return nullptr;
}

// ----- Tuya crypto -----
static String sha256_hex(const String& data) {
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

static String hmac_sha256_hex_upper(const String& key, const String& msg) {
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

static String epochMs13() {
  time_t nowSec=time(nullptr);
  uint64_t ms=(uint64_t)nowSec*1000ULL+(uint64_t)(millis()%1000);
  return String((unsigned long long)ms);
}

static String makeStringToSign(const String& method, const String& pathWithQuery, const String& body) {
  return method + "\n" + sha256_hex(body) + "\n\n" + pathWithQuery;
}

static String makeTuyaSign(bool isTokenApi,
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

static bool tuyaRequest(bool isTokenApi,
                        const String& method,
                        const String& pathWithQuery,
                        const String& body,
                        int& httpCodeOut,
                        String& respOut) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = TUYA_ENDPOINT + pathWithQuery;

  if(!https.begin(client,url)){
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
  else { https.end(); return false; }

  httpCodeOut=httpCode;

  if(httpCode<0){
    respOut="";
    https.end();
    return false;
  }

  respOut=https.getString();
  https.end();
  return true;
}

static bool tuyaEnsureToken() {
  unsigned long nowSec=(unsigned long)time(nullptr);
  if(tuya_access_token.length()>0 && token_expire_epoch>nowSec+60) return true;

  int code=0; String resp;
  bool okReq=tuyaRequest(true,"GET","/v1.0/token?grant_type=1","",code,resp);
  if(!okReq || !(code>=200 && code<300)) return false;

  DynamicJsonDocument doc(8192);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok) return false;

  if(!(doc["success"]|false)) return false;

  JsonObject result=doc["result"].as<JsonObject>();
  const char* at=result["access_token"];
  if(!at || String(at).isEmpty()) return false;

  tuya_access_token=at;
  int expire=result["expire_time"]|result["expire"]|7200;
  token_expire_epoch=nowSec+expire;
  return true;
}

static bool tuyaSetSwitch(const char* device_id, const char* codeStr, bool on, String& outResp) {
  if(!tuyaEnsureToken()){ outResp="token_failed"; return false; }

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
  return (doc["success"]|false);
}

static bool tuyaGetSwitchStatus(const char* device_id, const char* codeStr, bool &valOut, bool &onlineOut){
  onlineOut = false;
  if(!tuyaEnsureToken()) return false;

  String path = String("/v1.0/iot-03/devices/") + device_id + "/status";
  int httpCode=0; String resp;
  bool okReq=tuyaRequest(false,"GET",path,"",httpCode,resp);
  if(!okReq) return false;

  DynamicJsonDocument doc(8192);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok) return false;

  if(!(doc["success"]|false)) return false;
  onlineOut = true;

  JsonArray result = doc["result"].as<JsonArray>();
  for(JsonObject st : result){
    const char* code = st["code"] | "";
    if(String(code) == String(codeStr)){
      valOut = st["value"] | false;
      return true;
    }
  }
  return false;
}
// ===== Tuya Task (low priority; keeps BLE stable) =====
static void tuyaTask(void* pv) {
  for(;;) {
    TuyaJob job;
    if (tuyaQueue == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (xQueueReceive(tuyaQueue, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Wait for NTP time sync (Tuya signatures require valid epoch ms)
    while (!timeIsSynced()) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (job.idx < 0 || job.idx >= DEVICE_COUNT) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // BLE 보호: 처리 전/중에 자주 양보
    if (bleKeyboard.isConnected()) {
      vTaskDelay(pdMS_TO_TICKS(150));
    }

    DeviceItem &d = DEVICES[job.idx];

    if (!tuyaEnsureToken()) {
      d.pending = false;
      d.online_cache = false;
      d.last_err = "token_failed";
      publishDevices(true);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    String tuyaResp;
    uint32_t t0 = millis();
    bool ok = tuyaSetSwitch(d.device_id, d.code, job.on, tuyaResp);
    uint32_t t1 = millis();
    d.last_cmd_ms = (t1 - t0);

    if (ok) {
      d.state_cache = job.on;   // optimistic
      d.pending = true;
      d.online_cache = true;
      d.last_err = "";
    } else {
      d.pending = false;
      d.online_cache = false;
      d.last_err = "set_failed";
    }

    publishDevices(true);

    // 연속 TLS 호출 완화
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}



// ----- myHome topics -----
static String makeNodeId(){
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[40];
  snprintf(buf,sizeof(buf),"node-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}

static void buildTopics(){
  g_topicAnnounce = String(TOPIC_ROOT) + "/announce";
  g_topicDevices  = String(TOPIC_ROOT) + "/" + g_nodeId + "/devices";
  g_topicCmdSet   = String(TOPIC_ROOT) + "/" + g_nodeId + "/cmd/set";
}

static bool timeIsSynced(){
  time_t nowSec=time(nullptr);
  return (nowSec > 1700000000);
}

static void publishAnnounce(){
  if(!mqtt.connected()) return;
  DynamicJsonDocument doc(512);
  doc["node"]=g_nodeId;
  doc["ip"]=WiFi.localIP().toString();
  doc["mdns"]=String(MDNS_NAME)+".local";
  doc["rssi"]=WiFi.RSSI();
  doc["t"]=(unsigned long)time(nullptr);
  String out; serializeJson(doc,out);
  mqtt.publish(g_topicAnnounce.c_str(), out.c_str(), true);
}

static void publishDevices(bool retained){
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

static void syncAllOnce(){
  bool changed=false;
  for(int i=0;i<DEVICE_COUNT;i++){
    bool v=false, online=false;
    bool ok = tuyaGetSwitchStatus(DEVICES[i].device_id, DEVICES[i].code, v, online);
    if(ok){
      if(DEVICES[i].state_cache != v) changed=true;
      DEVICES[i].state_cache = v;
      DEVICES[i].online_cache = online;
      DEVICES[i].last_err = "";
      DEVICES[i].pending = false;
    }else{
      DEVICES[i].online_cache = false;
      DEVICES[i].last_err = "status_failed";
    }
    delay(80);
  }
  if(changed) publishDevices(true);
}

static void pollOneDevice(){
  int i = g_pollIdx % DEVICE_COUNT;
  g_pollIdx = (g_pollIdx + 1) % DEVICE_COUNT;

  bool v=false, online=false;
  bool ok = tuyaGetSwitchStatus(DEVICES[i].device_id, DEVICES[i].code, v, online);

  bool changed=false;
  if(ok){
    if(DEVICES[i].state_cache != v) changed=true;
    DEVICES[i].state_cache = v;
    DEVICES[i].online_cache = online;
    DEVICES[i].last_err = "";
    DEVICES[i].pending = false;
  }else{
    DEVICES[i].online_cache = false;
    DEVICES[i].last_err = "status_failed";
  }

  if(changed) publishDevices(true);
}

// ----- WiFi/MQTT connect helpers -----
static void startWiFiIfNeeded(){
  if(WiFi.status() == WL_CONNECTED) return;
  if(millis() < nextWiFiTryMs) return;

  nextWiFiTryMs = millis() + WIFI_RETRY_INTERVAL_MS;

  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void startTimeIfPossible(){
  if(didTimeStart) return;
  if(WiFi.status() != WL_CONNECTED) return;

  configTime(9*3600,0,"pool.ntp.org","time.nist.gov");
  didTimeStart = true;
}

static void startMDNSIfPossible(){
  if(didMDNS) return;
  if(WiFi.status() != WL_CONNECTED) return;
  if(!MDNS.begin(MDNS_NAME)) return;
  didMDNS = true;
}

static void publishBleStatus(){
  if(millis() - lastBleStatusMs < 1000) return;
  lastBleStatusMs = millis();

  const unsigned long nowMs = millis();
  bool real = bleKeyboard.isConnected();
  if(real) bleLastOkMs = nowMs;

  bool eff = real;
  if(!eff && bleLastOkMs > 0 && (nowMs - bleLastOkMs) < BLE_DISCONNECT_GRACE_MS){
    eff = true;
  }

  if(eff != bleStableState){
    bleStableState = eff;
    uint32_t ts = (uint32_t)(millis()/1000);
    String payload = String("{\"ble\":") + (bleStableState?"true":"false") + ",\"ts\":" + ts + "}";
    if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload.c_str(), true);
  }

  if(millis() - lastBleHeartbeatMs >= 2000){
    lastBleHeartbeatMs = millis();
    uint32_t ts = (uint32_t)(millis()/1000);
    String payload = String("{\"ble\":") + (bleStableState?"true":"false") + ",\"ts\":" + ts + "}";
    if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload.c_str(), true);
  }
}

static void bleHealthCheck(){
  static uint32_t lastRecoverAttemptMs = 0;
  static uint8_t recoverCount = 0;

  if(bleKeyboard.isConnected()){
    bleLastRealConnMs = millis();
    recoverCount = 0;
    return;
  }
  if(bleLastRealConnMs == 0) return;

  const uint32_t now = millis();
  if((now - bleLastRealConnMs) < BLE_HARD_RECOVER_MS) return;

  uint32_t backoffMs = 60000UL << (recoverCount < 4 ? recoverCount : 4);
  if(now - lastRecoverAttemptMs < backoffMs) return;

  bleKeyboard.end();
  bleKeyboard.begin();

  lastRecoverAttemptMs = now;
  if(recoverCount < 10) recoverCount++;
}

// ----- uniRMC processors -----
static void processBleCmdIfAny(){
  if(!pendingBleCmd) return;
  pendingBleCmd = false;

  String msg = pendingBleMsg; msg.trim();
  if(!bleKeyboard.isConnected()) return;

  if      (msg=="up")    tapKey(KEY_UP_ARROW);
  else if (msg=="down")  tapKey(KEY_DOWN_ARROW);
  else if (msg=="left")  tapKey(KEY_LEFT_ARROW);
  else if (msg=="right") tapKey(KEY_RIGHT_ARROW);
  else if (msg=="ok1")   tapKey(KEY_RETURN);
  else if (msg=="volup")   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (msg=="voldown") bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else if (msg=="mute")    bleKeyboard.write(KEY_MEDIA_MUTE);
  else if (msg=="reset")   ESP.restart();
  else if (msg.startsWith("MB:")){
    uint16_t v = hexToU16(msg.substring(3));
    uint8_t msb = (v>>8)&0xFF;
    uint8_t lsb = v & 0xFF;
    tapMediaRaw(lsb, msb);
  } else return;

  blinkNotify();
}

static void processIrCmdIfAny(){
  if(!pendingIrCmd) return;
  pendingIrCmd = false;

  String msg = pendingIrMsgJson; msg.trim();

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc, msg)){
    publishIrStatus("JSON parse failed");
    return;
  }
  const char* cmd = doc["cmd"];
  if(!cmd){ publishIrStatus("missing cmd"); return; }

  bool ok=false;
  for(int i=0;i<IR_COUNT;i++){
    if(strcmp(irCodes[i].name, cmd)==0){
      if(irCodes[i].raw && irCodes[i].raw_len){
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

static void processRfCmdIfAny(){
  if(!pendingRfCmd) return;
  pendingRfCmd = false;

  String msg = pendingRfMsg; msg.trim();
  publish433Log("CMD RX: " + msg);

  if(msg.equalsIgnoreCase("reset")){
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
      ok=true;
      blinkNotify();
      break;
    }
  }
  if(!ok) publish433Log("Unknown CMD: " + msg);
}

// ----- myHome cmd handler -----
static void handleMyHomeCmdSet(const String& payload){
  DynamicJsonDocument doc(256);
  if(deserializeJson(doc, payload) != DeserializationError::Ok) return;

  String key = (const char*)(doc["key"] | "");
  bool on = doc["on"] | false;

  int idx=-1;
  DeviceItem* d=findDeviceByKey(key, idx);
  if(!d) return;

  // optimistic UI update
  d->state_cache = on;
  d->pending = true;
  d->online_cache = true;
  d->last_err = "";
  publishDevices(true);

  // enqueue to TuyaTask (do not run HTTPS in main loop)
  if (tuyaQueue) {
    TuyaJob job{idx, on, (uint32_t)millis()};
    if (xQueueSend(tuyaQueue, &job, 0) != pdTRUE) {
      // drop oldest then retry
      TuyaJob dummy;
      xQueueReceive(tuyaQueue, &dummy, 0);
      xQueueSend(tuyaQueue, &job, 0);
    }
  }
}



// ----- MQTT callback -----
static void mqttCallback(char* topic, byte* payload, unsigned int length){
  String t(topic);
  String msg; msg.reserve(length+2);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();

  if(t == TOPIC_MIBOX_CMD){ pendingBleMsg = msg; pendingBleCmd = true; return; }
  if(t == TOPIC_IR_CMD)   { pendingIrMsgJson = msg; pendingIrCmd = true; return; }
  if(t == TOPIC_433_CMD)  { pendingRfMsg = msg; pendingRfCmd = true; return; }
  if(t == g_topicCmdSet)  { handleMyHomeCmdSet(msg); return; }
}

// ----- MQTT connect -----
static void startMQTTIfNeeded(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(mqtt.connected()) return;
  if(millis() < nextMQTTTryMs) return;

  nextMQTTTryMs = millis() + MQTT_RETRY_INTERVAL_MS;

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  String cid = "ESP32-MERGE-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  if(mqtt.connect(cid.c_str(), TOPIC_MIBOX_STATUS, 0, true, "{\"ble\":false,\"ts\":0}")){
    mqtt.subscribe(TOPIC_MIBOX_CMD);
    mqtt.subscribe(TOPIC_IR_CMD);
    mqtt.subscribe(TOPIC_433_CMD);
    mqtt.subscribe(g_topicCmdSet.c_str(), 0);

    publish433Status();
    publishIrStatus("MQTT connected");
    publishAnnounce();
    publishDevices(true);
  }
}

// ============================================================================
// 5) ARDUINO setup/loop
// ============================================================================
unsigned long last433StatusMs = 0;

void setup(){
  Serial.begin(115200);
  delay(200);

  // BLE 상시 연결 목적: WiFi/BLE 공존에서 BT 우선
  esp_coex_preference_set(ESP_COEX_PREFER_BT);

  pinMode(NOTIFY_LED_PIN, OUTPUT);
  digitalWrite(NOTIFY_LED_PIN, LOW);

  // BLE init
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  delay(150);
  bleKeyboard.begin();
  delay(200);
  bleKeyboard.setBatteryLevel(100);

  // IR send
  irsend.begin();

  // 433
  rf.enableReceive(RX_PIN);
  rf.enableTransmit(TX_PIN);

  // WiFi
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // myHome topics
  g_nodeId = makeNodeId();
  buildTopics();

  // MQTT
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(4096);

  // TuyaTask 큐/태스크 생성 (Core0 / low priority)
  tuyaQueue = xQueueCreate(6, sizeof(TuyaJob));
  xTaskCreatePinnedToCore(tuyaTask, "TuyaTask", 8192, nullptr, 1, &tuyaTaskHandle, 0);

  publish433Log("Boot: allRMC merged (BLE priority + TuyaTask, no web).");
}

void loop(){
  serviceNotify();
  serviceBleActions();

  startWiFiIfNeeded();
  startTimeIfPossible();
  startMDNSIfPossible();
  startMQTTIfNeeded();

  if(mqtt.connected()) mqtt.loop();

  publishBleStatus();
  bleHealthCheck();

  processBleCmdIfAny();
  processIrCmdIfAny();
  processRfCmdIfAny();

  if(millis()-last433StatusMs>1500){
    last433StatusMs=millis();
    publish433Status();
  }

  unsigned long nowMs = millis();

  if(mqtt.connected() && (nowMs - g_lastAnnounceMs > ANNOUNCE_INTERVAL_MS)){
    g_lastAnnounceMs = nowMs;
    publishAnnounce();
  }
  if(mqtt.connected() && (nowMs - g_lastDevPublishMs > DEV_PUBLISH_INTERVAL_MS)){
    g_lastDevPublishMs = nowMs;
    publishDevices(true);
  }

  // Tuya: no boot sync (request-driven). Mark ready after time sync.
  if(!didTuyaBootSync && timeIsSynced()){
    didTuyaBootSync = true;
  }

  // token warmup disabled (request-driven)

  // Tuya polling disabled (request-driven)


  delay(1);
}
