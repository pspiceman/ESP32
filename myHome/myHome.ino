#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

// ===================== WiFi =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===================== Tuya Cloud (Western America) =====================
String TUYA_ENDPOINT  = "https://openapi.tuyaus.com";
String TUYA_CLIENT_ID = "4q9fg8hve79wxevhgtc4";
String TUYA_SECRET    = "32a2fc7ed7924b3ea0a1baccbdabfc64";

// ===================== mDNS =====================
const char* MDNS_NAME = "myhome"; // http://myhome.local

// ===================== MQTT (Public broker) =====================
// ESP32 -> TCP 1883
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// Root topic (keep yours to avoid collisions)
String TOPIC_ROOT = "pspiceman/myhome"; // change if you want

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

// ===================== Tuya token/cache =====================
String tuya_access_token = "";
unsigned long token_expire_epoch = 0;
int g_lastTuyaHttp = 0;
String g_lastTuyaResp = "";
String g_lastTuyaErr  = "";

unsigned long g_lastTokenCheckMs = 0;
const unsigned long TOKEN_WARMUP_INTERVAL_MS = 30000;

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
bool qPop(Cmd &out){
  if(qEmpty()) return false;
  out=q[qh]; qh=(qh+1)%12; return true;
}

// ===================== MQTT objects =====================
WiFiClient espClient;
PubSubClient mqtt(espClient);

String chipIdHex(){
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lx%08lx", (uint32_t)(mac>>32), (uint32_t)mac);
  return String(buf);
}
String DEVICE_NODE; // set in setup()

// topic helpers
String tAnnounce(){ return TOPIC_ROOT + "/announce"; }
String tNodeRoot(){ return TOPIC_ROOT + "/" + DEVICE_NODE; }
String tOnline(){ return tNodeRoot() + "/online"; }
String tDevices(){ return tNodeRoot() + "/devices"; }
String tState(const String& key){ return tNodeRoot() + "/state/" + key; }
String tCmd(const String& key){ return tNodeRoot() + "/cmd/" + key; }
String tCmdAll(){ return tNodeRoot() + "/cmd/#"; }
String tLog(){ return tNodeRoot() + "/log"; }

// ===================== helpers =====================
DeviceItem* findDeviceByKey(const String& key, int &idxOut){
  for(int i=0;i<DEVICE_COUNT;i++){
    if(key==DEVICES[i].key){ idxOut=i; return &DEVICES[i]; }
  }
  idxOut=-1; return nullptr;
}

void publishLog(const String& msg){
  Serial.println(msg);
  if(mqtt.connected()) mqtt.publish(tLog().c_str(), msg.c_str(), false);
}

void publishDevices(bool retained=true){
  if(!mqtt.connected()) return;
  DynamicJsonDocument doc(2048);
  doc["node"]=DEVICE_NODE;
  doc["ip"]=WiFi.localIP().toString();
  doc["mdns"]=String(MDNS_NAME)+".local";
  JsonArray arr = doc.createNestedArray("devices");
  for(int i=0;i<DEVICE_COUNT;i++){
    JsonObject d = arr.createNestedObject();
    d["key"]=DEVICES[i].key;
    d["name"]=DEVICES[i].name_kr;
    d["state"]=DEVICES[i].state_cache;
    d["online"]=DEVICES[i].online_cache;
    d["pending"]=DEVICES[i].pending;
    if(DEVICES[i].last_err.length()) d["err"]=DEVICES[i].last_err;
  }
  String out; serializeJson(doc,out);
  mqtt.publish(tDevices().c_str(), out.c_str(), retained);
}

void publishState(int idx, bool retained=true){
  if(!mqtt.connected()) return;
  DeviceItem &d=DEVICES[idx];
  DynamicJsonDocument doc(256);
  doc["key"]=d.key;
  doc["state"]=d.state_cache;
  doc["online"]=d.online_cache;
  doc["pending"]=d.pending;
  if(d.last_err.length()) doc["err"]=d.last_err;
  String out; serializeJson(doc,out);
  mqtt.publish(tState(d.key).c_str(), out.c_str(), retained);
}

void publishAnnounce(){
  if(!mqtt.connected()) return;
  DynamicJsonDocument doc(512);
  doc["node"]=DEVICE_NODE;
  doc["ip"]=WiFi.localIP().toString();
  doc["mdns"]=String(MDNS_NAME)+".local";
  doc["rssi"]=WiFi.RSSI();
  doc["t"]=(unsigned long)time(nullptr);
  String out; serializeJson(doc,out);
  mqtt.publish(tAnnounce().c_str(), out.c_str(), true); // retained
  mqtt.publish(tOnline().c_str(), "online", true);      // retained
}

// ===================== crypto (Tuya) =====================
String sha256_hex(const String& data) {
  unsigned char hash[32];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  mbedtls_sha256_update(&sha, (const unsigned char*)data.c_str(), data.length());
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);

  static const char* hex = "0123456789abcdef";
  String out; out.reserve(64);
  for (int i=0;i<32;i++){ out += hex[(hash[i]>>4)&0xF]; out += hex[hash[i]&0xF]; }
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

  static const char* hex = "0123456789ABCDEF";
  String out; out.reserve(64);
  for (int i=0;i<32;i++){ out += hex[(hmacResult[i]>>4)&0xF]; out += hex[hmacResult[i]&0xF]; }
  return out;
}

String epochMs13() {
  time_t nowSec = time(nullptr);
  uint64_t ms = (uint64_t)nowSec * 1000ULL + (uint64_t)(millis() % 1000);
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
  g_lastTuyaHttp = 0;
  g_lastTuyaResp = "";
  g_lastTuyaErr  = "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = TUYA_ENDPOINT + pathWithQuery;

  if (!https.begin(client, url)) {
    g_lastTuyaErr = "https.begin() failed";
    httpCodeOut = 0;
    respOut = "";
    return false;
  }

  String t_ms = epochMs13();
  String nonce = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX);
  String sign = makeTuyaSign(isTokenApi, method, pathWithQuery, body, t_ms, nonce);

  https.addHeader("client_id", TUYA_CLIENT_ID);
  https.addHeader("t", t_ms);
  https.addHeader("nonce", nonce);
  https.addHeader("sign_method", "HMAC-SHA256");
  https.addHeader("sign", sign);
  https.addHeader("Content-Type", "application/json");
  if (!isTokenApi) https.addHeader("access_token", tuya_access_token);

  int httpCode = 0;
  if (method == "GET") httpCode = https.GET();
  else if (method == "POST") httpCode = https.POST((uint8_t*)body.c_str(), body.length());
  else { https.end(); g_lastTuyaErr="unsupported method"; return false; }

  httpCodeOut = httpCode;

  if (httpCode < 0) {
    g_lastTuyaErr = String("HTTPClient error: ") + https.errorToString(httpCode);
    respOut = "";
    https.end();
    return false;
  }

  respOut = https.getString();
  https.end();

  g_lastTuyaHttp = httpCode;
  g_lastTuyaResp = respOut;
  return true;
}

bool tuyaEnsureToken() {
  unsigned long nowSec = (unsigned long)time(nullptr);
  if (tuya_access_token.length() > 0 && token_expire_epoch > nowSec + 60) return true;

  int code = 0;
  String resp;
  bool okReq = tuyaRequest(true, "GET", "/v1.0/token?grant_type=1", "", code, resp);
  if (!okReq || !(code >= 200 && code < 300)) { g_lastTuyaErr="token request failed"; return false; }

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, resp) != DeserializationError::Ok) { g_lastTuyaErr="token JSON parse failed"; return false; }

  bool success = doc["success"] | false;
  if (!success) {
    g_lastTuyaErr = String("token success=false, msg=") + (const char*)(doc["msg"] | "");
    return false;
  }

  JsonObject result = doc["result"].as<JsonObject>();
  const char* at = result["access_token"];
  if (!at || String(at).isEmpty()) { g_lastTuyaErr="token missing access_token"; return false; }

  tuya_access_token = at;
  int expire = result["expire_time"] | result["expire"] | 7200;
  token_expire_epoch = nowSec + expire;
  return true;
}

bool tuyaSetSwitch(const char* device_id, const char* codeStr, bool on, String& outResp) {
  if (!tuyaEnsureToken()) { outResp = String("token_failed: ") + g_lastTuyaErr; return false; }

  String path = String("/v1.0/iot-03/devices/") + device_id + "/commands";

  DynamicJsonDocument bodyDoc(512);
  JsonArray commands = bodyDoc.createNestedArray("commands");
  JsonObject cmd = commands.createNestedObject();
  cmd["code"] = codeStr;
  cmd["value"] = on;

  String body; serializeJson(bodyDoc, body);

  int httpCode = 0;
  String resp;
  bool okReq = tuyaRequest(false, "POST", path, body, httpCode, resp);
  outResp = resp;
  if (!okReq) return false;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;
  bool success = doc["success"] | false;
  if (!success) g_lastTuyaErr = String("cmd fail: ") + (const char*)(doc["msg"] | "");
  return success;
}

// ===================== MQTT callback =====================
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String t(topic);
  String msg; msg.reserve(length);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];

  // expect: <root>/<node>/cmd/<key>
  String prefix = tNodeRoot() + "/cmd/";
  if(!t.startsWith(prefix)) return;

  String key = t.substring(prefix.length());

  bool on=false; bool parsed=false;
  msg.trim();
  if(msg=="1"||msg=="0"){ on=(msg=="1"); parsed=true; }
  else {
    DynamicJsonDocument doc(256);
    if(deserializeJson(doc,msg)==DeserializationError::Ok){
      if(doc.containsKey("on")){ on = doc["on"]; parsed=true; }
      else if(doc.containsKey("state")){ on = doc["state"]; parsed=true; }
    }
  }
  if(!parsed) return;

  int idx=-1;
  DeviceItem* d = findDeviceByKey(key, idx);
  if(!d) return;

  d->pending=true;
  d->last_err="";
  d->state_cache = on;     // optimistic
  publishState(idx, true);
  publishDevices(true);

  qPushOrReplace(idx, on);
  publishLog("CMD RX: " + key + " -> " + (on?"ON":"OFF"));
}

// ===================== connect helpers =====================
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi connecting");
  unsigned long start=millis();
  while(WiFi.status()!=WL_CONNECTED){
    delay(400);
    Serial.print(".");
    if(millis()-start>20000){
      Serial.println("\nWiFi reconnect...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start=millis();
    }
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
}

void setupTimeNTP(){
  configTime(9*3600, 0, "pool.ntp.org", "time.nist.gov");
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
  MDNS.addService("http","tcp",80);
  Serial.print("mDNS started: http://"); Serial.print(MDNS_NAME); Serial.println(".local");
}

void connectMQTT(){
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  while(!mqtt.connected()){
    String cid = "ESP32-MYHOME-" + chipIdHex();
    Serial.print("[MQTT] connecting...");
    // LWT online topic
    if(mqtt.connect(cid.c_str(), nullptr, nullptr, tOnline().c_str(), 1, true, "offline")){
      Serial.println("OK");
      mqtt.subscribe(tCmdAll().c_str(), 1);
      publishAnnounce();
      publishDevices(true);
      publishLog("MQTT connected & subscribed: " + tCmdAll());
    } else {
      Serial.print("FAIL rc="); Serial.println(mqtt.state());
      delay(1500);
    }
  }
}

// ===================== setup/loop =====================
void setup(){
  Serial.begin(115200);
  delay(200);

  DEVICE_NODE = "node-" + chipIdHex().substring(0, 10); // stable id
  Serial.println("Node: " + DEVICE_NODE);

  connectWiFi();
  setupTimeNTP();
  setupMDNS();

  connectMQTT();
}

void loop(){
  // keep WiFi
  if(WiFi.status()!=WL_CONNECTED){
    publishLog("WiFi lost -> reconnect");
    connectWiFi();
  }

  // keep MQTT
  if(!mqtt.connected()){
    Serial.println("MQTT lost -> reconnect");
    connectMQTT();
  }
  mqtt.loop();

  // token warmup
  unsigned long nowMs = millis();
  if(nowMs - g_lastTokenCheckMs > TOKEN_WARMUP_INTERVAL_MS){
    g_lastTokenCheckMs = nowMs;
    unsigned long nowSec = (unsigned long)time(nullptr);
    if(tuya_access_token.length()==0 || token_expire_epoch < nowSec + 180){
      tuyaEnsureToken();
    }
    publishAnnounce();
  }

  // process queue
  if(!qBusy && !qEmpty()){
    qBusy=true;
    Cmd c; qPop(c);
    if(c.idx>=0 && c.idx<DEVICE_COUNT){
      DeviceItem &d = DEVICES[c.idx];
      String tuyaResp;
      uint32_t t0=millis();
      bool ok = tuyaSetSwitch(d.device_id, d.code, c.on, tuyaResp);
      uint32_t t1=millis();
      d.last_cmd_ms = (t1-t0);
      d.pending=false;

      if(ok){
        d.state_cache = c.on;
        d.online_cache = true;
        d.last_err="";
        publishLog("SET OK: " + String(d.key) + " -> " + (c.on?"ON":"OFF"));
      } else {
        d.online_cache = false;
        d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "set_failed";
        publishLog("SET ERR: " + String(d.key) + " :: " + d.last_err);
      }
      publishState(c.idx, true);
      publishDevices(true);
    }
    qBusy=false;
  }
}
