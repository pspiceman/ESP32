/*
https://platform.tuya.com/
baekdc@naver.com -> 1700note//
*/
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
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

struct DeviceItem {
  const char* key;
  const char* name_kr;
  const char* device_id;
  const char* code;       // "switch_1"
  bool  state_cache;
  bool  online_cache;
  bool  pending;
  uint32_t last_ms;
  String last_err;
};

DeviceItem DEVICES[] = {
  {"myDesk", "myDesk", "eb622bc2147e960df5e3ez", "switch_1", false, true, false, 0, ""},
  {"floor",  "장판",   "eb778d36a45024d8d0ymiq", "switch_1", false, true, false, 0, ""},
  {"tablet", "테블릿", "ebf7ba55e51699856boo3j", "switch_1", false, true, false, 0, ""},
  {"server", "서버",   "eb07edb5fbddf33dc9xjhb", "switch_1", false, true, false, 0, ""},
  {"water",  "정수기", "ebe0b0f4ff09a6297e5an3", "switch_1", false, true, false, 0, ""},
};
const int DEVICE_COUNT = sizeof(DEVICES) / sizeof(DEVICES[0]);

String tuya_access_token = "";
unsigned long token_expire_epoch = 0;

WebServer server(80);

// debug
int g_lastTuyaHttp = 0;
String g_lastTuyaResp = "";
String g_lastTuyaErr  = "";

// token warmup
unsigned long g_lastTokenCheckMs = 0;
const unsigned long TOKEN_WARMUP_INTERVAL_MS = 30000;

// ===================== Queue (command) =====================
struct Cmd { int idx; bool on; uint32_t enq_ms; };
Cmd q[12];
int qh=0, qt=0;
bool qBusy = false;

bool qEmpty(){ return qh==qt; }
bool qFull(){ return ((qt+1)%12)==qh; }

void qPushOrReplace(int idx, bool on){
  for(int i=qh; i!=qt; i=(i+1)%12){
    if(q[i].idx == idx){
      q[i].on = on;
      q[i].enq_ms = millis();
      return;
    }
  }
  if(qFull()) qh = (qh+1)%12;
  q[qt] = { idx, on, (uint32_t)millis() };
  qt = (qt+1)%12;
}

bool qPop(Cmd &out){
  if(qEmpty()) return false;
  out = q[qh];
  qh = (qh+1)%12;
  return true;
}

// ===================== Status sync =====================
unsigned long g_lastStatusPollMs = 0;
const unsigned long STATUS_POLL_INTERVAL_MS = 1200;
int g_statusIdx = 0;

bool g_forceStatus[DEVICE_COUNT] = {false};
unsigned long g_forceDueMs[DEVICE_COUNT] = {0};

// ---- CORS / PNA ----
void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Max-Age", "86400");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}
void handleOptions() { addCORS(); server.send(200); }

// ---- crypto ----
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
  for (int i=0;i<32;i++){
    out += hex[(hash[i]>>4)&0xF];
    out += hex[hash[i]&0xF];
  }
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
  for (int i=0;i<32;i++){
    out += hex[(hmacResult[i]>>4)&0xF];
    out += hex[hmacResult[i]&0xF];
  }
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

bool tuyaGetSwitchStatus(const char* device_id, const char* codeStr, bool &outOn, bool &outOnline, String &outErr){
  outErr = "";
  if (!tuyaEnsureToken()) { outErr = String("token_failed: ") + g_lastTuyaErr; return false; }

  String path = String("/v1.0/iot-03/devices/") + device_id + "/status";

  int httpCode = 0;
  String resp;
  bool okReq = tuyaRequest(false, "GET", path, "", httpCode, resp);
  if (!okReq) { outErr = g_lastTuyaErr; return false; }

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, resp) != DeserializationError::Ok) { outErr = "status_json_parse_fail"; return false; }

  bool success = doc["success"] | false;
  if (!success) { outErr = (const char*)(doc["msg"] | "status_fail"); return false; }

  outOnline = true;

  JsonArray result = doc["result"].as<JsonArray>();
  for (JsonObject it : result){
    const char* code = it["code"];
    if (code && String(code) == codeStr){
      if (it["value"].is<bool>()) outOn = it["value"].as<bool>();
      else outOn = (it["value"].as<int>() != 0);
      return true;
    }
  }
  outErr = "code_not_found";
  return false;
}

DeviceItem* findDeviceByKey(const String& key, int &idxOut) {
  for (int i=0;i<DEVICE_COUNT;i++){
    if (key == DEVICES[i].key) { idxOut=i; return &DEVICES[i]; }
  }
  idxOut=-1;
  return nullptr;
}

// ---- handlers ----
void handleHealth(){
  addCORS();
  DynamicJsonDocument doc(1024);
  doc["ok"] = true;
  doc["ip"] = WiFi.localIP().toString();
  doc["mdns"] = String(MDNS_NAME) + ".local";
  doc["rssi"] = WiFi.RSSI();
  doc["q"] = (qt - qh + 12) % 12;
  time_t now = time(nullptr);
  doc["epoch"] = (unsigned long)now;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleDevices(){
  addCORS();
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.createNestedArray("devices");
  for (int i=0;i<DEVICE_COUNT;i++){
    JsonObject d = arr.createNestedObject();
    d["key"] = DEVICES[i].key;
    d["name"]= DEVICES[i].name_kr;
    d["state"]= DEVICES[i].state_cache;
    d["online"]= DEVICES[i].online_cache;
    d["pending"]= DEVICES[i].pending;
    d["last_ms"]= DEVICES[i].last_ms;
    if (DEVICES[i].last_err.length()) d["err"] = DEVICES[i].last_err;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// /api/set: 큐에 넣고 즉시 응답
void handleSet(){
  addCORS();
  if (!server.hasArg("key") || !server.hasArg("on")){
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"missing key/on\"}");
    return;
  }
  String key = server.arg("key");
  bool on = (server.arg("on")=="1" || server.arg("on")=="true");

  int idx=-1;
  DeviceItem* t = findDeviceByKey(key, idx);
  if (!t){
    server.send(404, "application/json", "{\"ok\":false,\"msg\":\"unknown device\"}");
    return;
  }

  // UI 즉시 반영 (optimistic)
  t->state_cache = on;
  t->pending = true;
  t->online_cache = true;
  t->last_err = "";

  qPushOrReplace(idx, on);

  DynamicJsonDocument doc(768);
  doc["ok"] = true;
  doc["queued"] = true;
  doc["key"] = key;
  doc["on"] = on;
  doc["q"] = (qt - qh + 12) % 12;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleTuyaLast(){
  addCORS();
  DynamicJsonDocument doc(8192);
  doc["last_http"] = g_lastTuyaHttp;
  doc["last_err"] = g_lastTuyaErr;
  doc["last_resp"] = g_lastTuyaResp;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---- WiFi / time / mdns ----
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void setupTimeNTP(){
  configTime(9*3600, 0, "pool.ntp.org", "time.nist.gov"); // KST
  Serial.print("NTP sync");
  for (int i=0;i<30;i++){
    time_t nowSec = time(nullptr);
    if (nowSec > 1700000000) { Serial.println(". OK"); return; }
    delay(300); Serial.print(".");
  }
  Serial.println(" (not synced, continue)");
}

void setupMDNS(){
  if (!MDNS.begin(MDNS_NAME)) { Serial.println("mDNS start FAILED"); return; }
  MDNS.addService("http","tcp",80);
  Serial.print("mDNS started: http://"); Serial.print(MDNS_NAME); Serial.println(".local");
}

void doStatusSyncOne(int idx){
  DeviceItem &d = DEVICES[idx];
  if (d.pending) return;

  bool on=false, online=true;
  String err;
  uint32_t t0 = millis();
  bool ok = tuyaGetSwitchStatus(d.device_id, d.code, on, online, err);
  uint32_t t1 = millis();
  d.last_ms = (t1 - t0);

  if (ok){
    d.state_cache = on;
    d.online_cache = true;
    d.last_err = "";
  } else {
    d.online_cache = false;
    d.last_err = err;
  }
}

void setup(){
  Serial.begin(115200);
  delay(200);

  connectWiFi();
  setupTimeNTP();
  setupMDNS();

  server.on("/api/health", HTTP_GET, handleHealth);
  server.on("/api/health", HTTP_OPTIONS, handleOptions);

  server.on("/api/devices", HTTP_GET, handleDevices);
  server.on("/api/devices", HTTP_OPTIONS, handleOptions);

  server.on("/api/set", HTTP_GET, handleSet);
  server.on("/api/set", HTTP_OPTIONS, handleOptions);

  server.on("/api/tuya_last", HTTP_GET, handleTuyaLast);
  server.on("/api/tuya_last", HTTP_OPTIONS, handleOptions);

  server.onNotFound([](){
    if (server.method() == HTTP_OPTIONS) handleOptions();
    else { addCORS(); server.send(404, "text/plain", "Not Found"); }
  });

  server.begin();
  Serial.println("Web server started.");
}

void loop(){
  server.handleClient();

  // 토큰 워밍업
  unsigned long now = millis();
  if (now - g_lastTokenCheckMs > TOKEN_WARMUP_INTERVAL_MS) {
    g_lastTokenCheckMs = now;
    unsigned long nowSec = (unsigned long)time(nullptr);
    if (tuya_access_token.length() == 0 || token_expire_epoch < nowSec + 180) {
      tuyaEnsureToken();
    }
  }

  // 1) 명령 큐 처리 (한 번에 1개)
  if (!qBusy && !qEmpty()){
    qBusy = true;
    Cmd c; qPop(c);

    if (c.idx >= 0 && c.idx < DEVICE_COUNT){
      DeviceItem &d = DEVICES[c.idx];
      String tuyaResp;

      uint32_t t0 = millis();
      bool ok = tuyaSetSwitch(d.device_id, d.code, c.on, tuyaResp);
      uint32_t t1 = millis();
      d.last_ms = (t1 - t0);

      if (ok){
        d.state_cache = c.on;
        d.pending = false;
        d.online_cache = true;
        d.last_err = "";

        // ✅ 명령 직후 빠른 동기화(200ms 뒤 한번)
        g_forceStatus[c.idx] = true;
        g_forceDueMs[c.idx] = millis() + 200;
      } else {
        d.pending = false;
        d.online_cache = false;
        d.last_err = g_lastTuyaErr.length()? g_lastTuyaErr : "set_failed";
      }
    }
    qBusy = false;
  }

  // 2) 명령 직후 강제 status sync
  if (!qBusy && qEmpty()){
    for(int i=0;i<DEVICE_COUNT;i++){
      if (g_forceStatus[i] && (long)(millis() - g_forceDueMs[i]) >= 0){
        g_forceStatus[i] = false;
        doStatusSyncOne(i);
        break;
      }
    }
  }

  // 3) 라운드로빈 status 폴링 (실시간 표시 안정화)
  if (!qBusy && qEmpty() && (now - g_lastStatusPollMs > STATUS_POLL_INTERVAL_MS)) {
    g_lastStatusPollMs = now;
    int idx = g_statusIdx;
    g_statusIdx = (g_statusIdx + 1) % DEVICE_COUNT;
    doStatusSyncOne(idx);
  }
}
