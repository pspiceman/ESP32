#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

// ===================== WiFi =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===================== MQTT (Public) =====================
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;
const char*    TOPIC_ROOT  = "pspiceman/myhome";

// ===================== Tuya Cloud (Western America) =====================
String TUYA_ENDPOINT  = "https://openapi.tuyaus.com";
String TUYA_CLIENT_ID = "4q9fg8hve79wxevhgtc4";
String TUYA_SECRET    = "32a2fc7ed7924b3ea0a1baccbdabfc64";

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
WiFiClient espClient;
PubSubClient mqtt(espClient);

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
// Tuya rate-limit/지연 고려: "한 번에 전부" 대신 Round-robin으로 1개씩 폴링 권장
const unsigned long STATUS_POLL_INTERVAL_MS = 1800; // 1.8s마다 기기 1개씩
unsigned long g_lastStatusPollMs = 0;
int g_pollIdx = 0;
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
  mqtt.publish(g_topicAnnounce.c_str(), out.c_str(), false);
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

void mqttCallback(char* topic, byte* payload, unsigned int length){
  String t(topic);
  String p; p.reserve(length+2);
  for(unsigned int i=0;i<length;i++) p+=(char)payload[i];

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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
    delay(80); // 너무 빠른 연속호출 방지
  }
  if(changed) publishDevices(true);
}

// 주기적으로 1개씩 상태 갱신 (Round-robin)
void pollOneDevice(){
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
    // pending은 "명령중 표시"라서, 실제상태가 반영된 시점에 끄는게 UX 좋음
    DEVICES[i].pending = false;
  }else{
    DEVICES[i].online_cache = false;
    DEVICES[i].last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "status_failed";
    // 실패했다고 pending을 바로 false로 바꾸진 않음(명령중일 수도 있어서)
  }

  if(changed) publishDevices(true);
}

// ===================== Arduino =====================
void setup(){
  Serial.begin(115200);
  delay(200);

  connectWiFi();
  setupTimeNTP();
  setupMDNS();

  g_nodeId = makeNodeId();
  buildTopics();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(4096);

  ensureMQTT();

  // ✅ 부팅 직후 Tuya 실제 상태 1회 동기화
  syncAllOnce();
  g_bootSynced = true;
  publishDevices(true);
}

void loop(){
  ensureMQTT();
  mqtt.loop();

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

  // ✅ Tuya status poll (실시간 동기화 핵심)
  if(nowMs - g_lastStatusPollMs > STATUS_POLL_INTERVAL_MS){
    g_lastStatusPollMs = nowMs;
    pollOneDevice();
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
      }else{
        d.pending = false;
        d.online_cache = false;
        d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "set_failed";
      }

      publishDevices(true); // UI 즉시 갱신
    }

    qBusy = false;
  }
}
