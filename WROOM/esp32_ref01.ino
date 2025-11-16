// https://chatgpt.com/c/691914ca-a6a8-8324-9982-2b9db981f606
// esp32_ref_mqtt_history.ino
// ESP32 Web Sensor Dashboard + HiveMQ
//  - 현재값: esp32wr/status (MQTT, retained)
//  - 1분 평균: esp32wr/history/YYYYMMDDHHMM (MQTT, retained)
//  - 더 이상 ESP32 내부에 히스토리 배열을 저장하지 않음.

/*
  - LED 핀: D27
  - BOOT 키: GPIO0 (내부 풀업)
  - 센서: AHT25(AHTxx) + SCD42(SensirionI2cScd4x)
  - 시리얼 포맷:
    AHT:24.0C 78% | SCD:28.9C 53% CO2:1071ppm | LED:OFF | RSSI: -47 [dBm]
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>
#include <esp_system.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SensirionI2cScd4x.h>
#include <AHTxx.h>
#include <time.h>  // NTP 시간 사용

// 핀
#define LED_PIN   27
#define BOOT_KEY   0

// Wi-Fi 기본 설정 =======================
#define WIFI_SSID "Backhome"
#define WIFI_PASS "1700note"

// --------------------------------------------------------
// AP 설정 페이지 HTML (필요 시 유지)
// --------------------------------------------------------
const char PAGE_INDEX[] PROGMEM =
  "<!DOCTYPE html><html lang='ko'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>ESP32 WiFi 설정</title>"
  "<style>"
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
  "margin:0;padding:24px;background:#020617;color:#e5e7eb}"
  "label{display:block;margin-top:12px;font-size:0.9rem}"
  "input{width:100%;padding:8px;margin-top:4px;border-radius:8px;"
  "border:1px solid #4b5563;background:#020617;color:#e5e7eb}"
  "button{margin-top:16px;padding:10px 16px;border-radius:999px;border:none;"
  "background:#2563eb;color:white;font-weight:600;cursor:pointer}"
  "button:hover{background:#1d4ed8}"
  "body>div{background:#020617;border-radius:12px;border:1px solid #1f2937;"
  "padding:16px;max-width:360px;margin:0 auto}"
  "h1{margin:0 0 10px}"
  "small{color:#9fb2d7}"
  "</style></head><body><div>"
  "<h1>ESP32 WiFi 설정</h1>"
  "<p>연결할 공유기의 SSID와 비밀번호를 입력한 뒤 저장하세요."
  " 저장 후 자동으로 재부팅합니다.</p>"
  "<form method='POST' action='/save'>"
  "<label>SSID<br><input name='ssid' required></label>"
  "<label>비밀번호<br><input type='password' name='pass'></label>"
  "<button type='submit'>저장</button>"
  "</form></div></body></html>";

const char PAGE_SAVED[] PROGMEM =
  "<!DOCTYPE html><html lang='ko'><head>"
  "<meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>저장 완료</title>"
  "<style>"
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
  "margin:0;padding:24px;background:#020617;color:#e5e7eb;text-align:center}"
  "h3{font-size:1.1rem}"
  "</style></head><body>"
  "<h3>WiFi 설정이 저장되었습니다. 2초 후 재부팅합니다…</h3>";

// ✅ HiveMQ Cloud 정보
const char* mqtt_server = "6c56aefe0ddb4d57977c735f5070abe8.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;     // TLS 포트
const char* mqtt_user   = "mqtt_ESP32";
const char* mqtt_pass   = "gomqtt_ESP32";

// MQTT 토픽
const char* TOPIC_STATUS      = "esp32wr/status";       // 현재값
const char* TOPIC_LED         = "esp32wr/led";          // LED 제어
const char* TOPIC_HISTORY_BASE= "esp32wr/history";      // 1분 평균 히스토리 베이스

// ===== Web =====
WebServer server(80);
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ===== Sensors / State =====
SensirionI2cScd4x scd4x;
AHTxx aht(AHTXX_ADDRESS_X38, AHT2x_SENSOR);
bool ledState = false;

static inline bool finite_f(float v){ return !isnan(v) && isfinite(v); }

// ===== Current sensor values =====
float g_aht_t = NAN, g_aht_h = NAN;
float g_scd_t = NAN, g_scd_h = NAN;
float g_co2   = NAN;
int   g_rssi  = 0;

// 누적 평균용
uint32_t lastAvgMs = 0;
float sum_aht_t=0, sum_aht_h=0, sum_scd_t=0, sum_scd_h=0, sum_co2=0;
int sample_count = 0;

// Serial command buffer
String serialBuf;

// ===== AP 모드 / 버튼 관리 =====
bool apMode = false;
uint32_t apPressStart = 0;
uint32_t apBlinkMs = 0;
bool apLedState = false;
// 5초 이상 누르면 AP 진입
const uint32_t AP_HOLD_MS         = 5000;
// AP 진입 전(길게 누르는 중) LED 깜빡임 간격
const uint32_t AP_BLINK_BEFORE_MS = 300;
// AP 모드 진입 후 LED 깜빡임 간격
const uint32_t AP_BLINK_AFTER_MS  = 200;

// ===== MQTT 관리 =====
uint32_t lastMqttReconnect = 0;
const uint32_t MQTT_RECONNECT_INTERVAL = 10000; // 10초

// ---- 함수 선언 ----
void setLed(bool on);
String jsonStatus();
String buildLine();
void startStationMode();
void enterApMode();
void checkApButton();
void publishStatus();
void publishHistoryMinute(float at, float ah, float st, float sh, float co2);

void handleApIndex();
void handleApSave();

void setupTime();

// ======================================================
//  시간 관련 (NTP) 헬퍼
// ======================================================
void setupTime() {
  // 한국 시간(UTC+9)
  configTime(9*3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("[Time] NTP sync requested");
}

// ISO 문자열 (YYYY-MM-DDTHH:MM) 생성
String currentIsoMinute() {
  time_t now = time(nullptr);
  if (now < 100000) {
    // 아직 NTP 동기화 전이면 부팅 기준 분 단위로 대체
    unsigned long mins = millis() / 60000;
    return String("boot+") + String(mins);
  }
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &timeinfo);
  return String(buf);
}

// 토픽용 키 (YYYYMMDDHHMM)
String currentMinuteKey() {
  time_t now = time(nullptr);
  if (now < 100000) {
    unsigned long mins = millis() / 60000;
    char buf[16];
    snprintf(buf, sizeof(buf), "boot%lu", mins);
    return String(buf);
  }
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%Y%m%d%H%M", &timeinfo);
  return String(buf);
}

// ======================================================
//  Sensors
// ======================================================
void readAHT() {
  static bool inited = false;
  if (!inited) {
    Wire.begin();
    delay(20);
    aht.begin();
    inited = true;
  }
  float t = aht.readTemperature();
  float h = aht.readHumidity();
  if (finite_f(t)) g_aht_t = t;
  if (finite_f(h)) g_aht_h = h;
}

void beginSCD() {
  scd4x.begin(Wire, 0x62);
  delay(10);
  scd4x.stopPeriodicMeasurement();
  delay(10);

  uint16_t error;
  char errorMessage[64];

  error = scd4x.startPeriodicMeasurement();
  if (error) {
    errorToString(error, errorMessage, sizeof(errorMessage));
    Serial.print("SCD4x start error: "); Serial.println(errorMessage);
  } else {
    Serial.println("SCD4x periodic measurement started.");
  }
}

void readSCD() {
  uint16_t error;
  char errorMessage[64];
  bool dataReady = false;
  error = scd4x.getDataReadyStatus(dataReady);
  if (error) {
    errorToString(error, errorMessage, sizeof(errorMessage));
    Serial.print("SCD4x DR status err: "); Serial.println(errorMessage);
    return;
  }
  if (!dataReady) return;

  uint16_t co2;
  float temperature, humidity;
  error = scd4x.readMeasurement(co2, temperature, humidity);
  if (error) {
    errorToString(error, errorMessage, sizeof(errorMessage));
    Serial.print("SCD4x read error: "); Serial.println(errorMessage);
    return;
  }
  g_co2   = (float)co2;
  g_scd_t = temperature;
  g_scd_h = humidity;
}

// ======================================================
//  LED 
// ======================================================
void setLed(bool on) {
  ledState = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ======================================================
//  JSON status (현재값) – MQTT + /api/status용 공용
// ======================================================
String jsonStatus() {
  StaticJsonDocument<512> doc;

  doc["aht_t"] = finite_f(g_aht_t) ? g_aht_t : JsonVariant();
  doc["aht_h"] = finite_f(g_aht_h) ? g_aht_h : JsonVariant();
  doc["scd_t"] = finite_f(g_scd_t) ? g_scd_t : JsonVariant();
  doc["scd_h"] = finite_f(g_scd_h) ? g_scd_h : JsonVariant();
  doc["co2"]   = finite_f(g_co2)   ? g_co2   : JsonVariant();
  doc["led"]   = ledState ? "on" : "off";
  doc["rssi"]  = g_rssi;
  doc["ip"]    = WiFi.localIP().toString();

  String s;
  serializeJson(doc, s);
  return s;
}

// ======================================================
//  한 줄 포맷 (시리얼)
// ======================================================
String buildLine() {
  String line;
  line.reserve(120);
  line += "AHT:";
  if (finite_f(g_aht_t)) {
    line += String(g_aht_t, 1);
    line += "C ";
  } else {
    line += "NaN ";
  }
  if (finite_f(g_aht_h)) {
    line += String((int)round(g_aht_h));
    line += "%";
  } else {
    line += "NaN%";
  }

  line += " | SCD:";
  if (finite_f(g_scd_t)) {
    line += String(g_scd_t, 1);
    line += "C ";
  } else {
    line += "NaN ";
  }
  if (finite_f(g_scd_h)) {
    line += String((int)round(g_scd_h));
    line += "% ";
  } else {
    line += "NaN% ";
  }
  line += "CO2:";
  if (finite_f(g_co2)) {
    line += String((int)round(g_co2));
    line += "ppm";
  } else {
    line += "NaN ppm";
  }

  line += " | LED:";
  line += (ledState ? "ON" : "OFF");
  line += " | RSSI: ";
  line += String(g_rssi);
  line += " [dBm]";
  return line;
}

// ======================================================
//  Web 응답 헬퍼
// ======================================================
void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  addCORS();
  server.send(204);
}

void handleRoot(){
  addCORS();
  if (apMode) {
    server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
  } else {
    server.send(200, "text/plain; charset=utf-8",
                "ESP32 sensor board. Use MQTT (HiveMQ dashboard) or /api/status.");
  }
}

// AP 모드 페이지
void handleApIndex(){
  addCORS();
  server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
}

void handleApSave(){
  addCORS();
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  Serial.println("[AP] WiFi 설정 수신");
  Serial.print("  SSID: "); Serial.println(ssid);
  Serial.print("  PASS: "); Serial.println(pass);

  // 필요하면 NVS에 저장
  server.send_P(200, "text/html; charset=utf-8", PAGE_SAVED);

  delay(2000);
  ESP.restart();
}

// 상태 / 한 줄
void handleStatus() {
  addCORS();
  server.send(200, "application/json; charset=utf-8", jsonStatus());
}

void handleLineHttp() {
  addCORS();
  server.send(200, "text/plain; charset=utf-8", buildLine());
}

// ======================================================
//  LED HTTP 핸들러 (옵션 – 써도 되고 안 써도 됨)
// ======================================================
void handleLedAny() {
  addCORS();
  String path = server.uri();
  path.toLowerCase();

  bool on = ledState;
  bool hasCmd = false;

  if (path.endsWith("/led/on") || path == "/led/on") {
    on = true; hasCmd = true;
  } else if (path.endsWith("/led/off") || path == "/led/off") {
    on = false; hasCmd = true;
  } else if (server.hasArg("state")) {
    String s = server.arg("state");
    s.toLowerCase();
    if (s == "on" || s == "1" || s == "true") { on = true; hasCmd = true; }
    else if (s == "off" || s == "0" || s == "false") { on = false; hasCmd = true; }
  }

  if (hasCmd) setLed(on);

  StaticJsonDocument<128> doc;
  doc["ok"]  = hasCmd;
  doc["led"] = ledState ? "on" : "off";

  String resp;
  serializeJson(doc, resp);
  server.send(200, "application/json; charset=utf-8", resp);
}

// ======================================================
//  Wi-Fi: STA 모드 시작
// ======================================================
void startStationMode() {
  apMode = false;
  Serial.println("\n[WiFi] STA mode start");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.isConnected()) {
    Serial.print("Wi-Fi connected, IP="); Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connect failed.");
  }
}

// ======================================
// AP 모드 전환 (BOOT 5초 이상)
// ======================================
void enterApMode() {
  if (apMode) return;
  apMode = true;

  Serial.println("\n=== Entering AP mode (BOOT long press) ===");

  mqtt.disconnect();
  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_AP);
  const char* apSsid = "ESP32-Setup";
  bool ok = WiFi.softAP(apSsid);
  IPAddress ip = WiFi.softAPIP();

  Serial.print("AP SSID: ");  Serial.println(apSsid);
  Serial.print("AP start: "); Serial.println(ok ? "OK" : "FAIL");
  Serial.print("AP IP: ");    Serial.println(ip);

  apBlinkMs = millis();
  apLedState = false;
}

// BOOT 키 상태 감시
void checkApButton() {
  uint32_t now = millis();
  int level = digitalRead(BOOT_KEY);

  if (!apMode) {
    if (level == LOW) {
      if (apPressStart == 0) apPressStart = now;

      uint32_t held = now - apPressStart;
      if (held >= AP_BLINK_BEFORE_MS && !apMode) {
        if (now - apBlinkMs >= AP_BLINK_BEFORE_MS) {
          apBlinkMs = now;
          apLedState = !apLedState;
          digitalWrite(LED_PIN, apLedState ? HIGH : LOW);
        }
      }
      if (held >= AP_HOLD_MS) {
        enterApMode();
      }
    } else {
      apPressStart = 0;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  } else {
    if (now - apBlinkMs >= AP_BLINK_AFTER_MS) {
      apBlinkMs = now;
      apLedState = !apLedState;
      digitalWrite(LED_PIN, apLedState ? HIGH : LOW);
    }
  }
}

// ======================================================
//  MQTT
// ======================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < length; ++i) msg += (char)payload[i];
  msg.trim();
  msg.toLowerCase();

  if (t == TOPIC_LED) {
    bool on = false;
    if (msg == "on" || msg == "1" || msg == "true") on = true;
    else if (msg == "off" || msg == "0" || msg == "false") on = false;
    setLed(on);
    Serial.print("[MQTT] LED command: ");
    Serial.println(on ? "ON" : "OFF");
  }
}

void ensureMqtt() {
  if (apMode) return;
  if (!WiFi.isConnected()) return;

  mqtt.loop();
  if (mqtt.connected()) return;

  uint32_t now = millis();
  if (now - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) return;
  lastMqttReconnect = now;

  Serial.print("[MQTT] Connecting to ");
  Serial.print(mqtt_server); Serial.print(":"); Serial.println(mqtt_port);

  if (!mqtt.connect("esp32wr", mqtt_user, mqtt_pass)) {
    Serial.print("[MQTT] connect failed, rc="); Serial.println(mqtt.state());
    return;
  }
  Serial.println("[MQTT] connected.");
  mqtt.subscribe(TOPIC_LED);
}

// ======================================================
//  MQTT 상태 전송 (현재값)
// ======================================================
void publishStatus() {
  if (!mqtt.connected() || apMode) return;

  StaticJsonDocument<256> doc;
  doc["aht_t"] = finite_f(g_aht_t) ? g_aht_t : JsonVariant();
  doc["aht_h"] = finite_f(g_aht_h) ? g_aht_h : JsonVariant();
  doc["scd_t"] = finite_f(g_scd_t) ? g_scd_t : JsonVariant();
  doc["scd_h"] = finite_f(g_scd_h) ? g_scd_h : JsonVariant();
  doc["co2"]   = finite_f(g_co2)   ? g_co2   : JsonVariant();
  doc["led"]   = ledState ? "on" : "off";
  doc["rssi"]  = g_rssi;
  doc["ip"]    = WiFi.localIP().toString();

  String s;
  serializeJson(doc, s);
  if (!mqtt.publish(TOPIC_STATUS, s.c_str(), true)) {
    Serial.println("[MQTT] status publish failed");
  }
}

// ======================================================
//  1분 평균 히스토리 전송 (MQTT, retained)
// ======================================================
void publishHistoryMinute(float at, float ah, float st, float sh, float co2) {
  if (!mqtt.connected() || apMode) return;

  StaticJsonDocument<256> doc;
  doc["ts"]    = currentIsoMinute();
  doc["aht_t"] = finite_f(at) ? at : JsonVariant();
  doc["aht_h"] = finite_f(ah) ? ah : JsonVariant();
  doc["scd_t"] = finite_f(st) ? st : JsonVariant();
  doc["scd_h"] = finite_f(sh) ? sh : JsonVariant();
  doc["co2"]   = finite_f(co2)? co2: JsonVariant();
  doc["led"]   = ledState ? "on" : "off";

  String s;
  serializeJson(doc, s);

  String topic = String(TOPIC_HISTORY_BASE) + "/" + currentMinuteKey();
  if (!mqtt.publish(topic.c_str(), s.c_str(), true)) {
    Serial.println("[MQTT] history publish failed");
  } else {
    Serial.print("[MQTT] history published to ");
    Serial.println(topic);
  }
}

// ======================================================
//  시리얼 명령 처리
// ======================================================
void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialBuf.length() > 0) {
        String cmd = serialBuf;
        serialBuf = "";
        cmd.trim();
        cmd.toLowerCase();
        if (cmd == "on") setLed(true);
        else if (cmd == "off") setLed(false);
        else if (cmd == "ap") enterApMode();
        else if (cmd == "status") {
          Serial.println(jsonStatus());
        }
      }
    } else {
      serialBuf += c;
    }
  }
}

// ======================================================
//  setup / loop
// ======================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BOOT_KEY, INPUT_PULLUP);

  Wire.begin();
  delay(20);
  aht.begin();
  beginSCD();

  startStationMode();
  setupTime();   // NTP 시간 동기화 시도

  // Web 라우트 (필요 최소만 유지)
  server.on("/",            HTTP_GET,      handleRoot);
  server.on("/",            HTTP_OPTIONS,  handleOptions);

  server.on("/ap",          HTTP_GET,      handleApIndex);
  server.on("/ap",          HTTP_OPTIONS,  handleOptions);
  server.on("/save",        HTTP_POST,     handleApSave);
  server.on("/save",        HTTP_OPTIONS,  handleOptions);

  server.on("/api/status",  HTTP_GET,      handleStatus);
  server.on("/api/status",  HTTP_OPTIONS,  handleOptions);
  server.on("/status",      HTTP_GET,      handleStatus);
  server.on("/status",      HTTP_OPTIONS,  handleOptions);

  server.on("/api/line",    HTTP_GET,      handleLineHttp);
  server.on("/api/line",    HTTP_OPTIONS,  handleOptions);
  server.on("/line",        HTTP_GET,      handleLineHttp);
  server.on("/line",        HTTP_OPTIONS,  handleOptions);

  // LED HTTP (선택)
  server.on("/api/led",     HTTP_ANY,      handleLedAny);
  server.on("/api/led",     HTTP_OPTIONS,  handleOptions);
  server.on("/led/on",      HTTP_ANY,      handleLedAny);
  server.on("/led/on",      HTTP_OPTIONS,  handleOptions);
  server.on("/led/off",     HTTP_ANY,      handleLedAny);
  server.on("/led/off",     HTTP_OPTIONS,  handleOptions);

  server.begin();
  Serial.println("HTTP server started.");

  espClient.setInsecure(); // TLS 인증서 검증 생략
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
}

void loop() {
  server.handleClient();
  handleSerialCommands();
  checkApButton();
  ensureMqtt();

  uint32_t now = millis();

  // 3초마다 센서 갱신 + 평균 누적
  static uint32_t lastReadMs = 0;
  if (now - lastReadMs >= 3000) {
    lastReadMs = now;
    readAHT();
    readSCD();
    if (WiFi.isConnected()) g_rssi = WiFi.RSSI();

    if (finite_f(g_aht_t)) sum_aht_t += g_aht_t;
    if (finite_f(g_aht_h)) sum_aht_h += g_aht_h;
    if (finite_f(g_scd_t)) sum_scd_t += g_scd_t;
    if (finite_f(g_scd_h)) sum_scd_h += g_scd_h;
    if (finite_f(g_co2))   sum_co2   += g_co2;
    sample_count++;
  }

  // 1분마다 평균 계산 → MQTT history 토픽에 publish (retained)
  if (now - lastAvgMs >= 60000) {
    lastAvgMs = now;
    float avg_aht_t = (sample_count>0) ? (sum_aht_t / sample_count) : NAN;
    float avg_aht_h = (sample_count>0) ? (sum_aht_h / sample_count) : NAN;
    float avg_scd_t = (sample_count>0) ? (sum_scd_t / sample_count) : NAN;
    float avg_scd_h = (sample_count>0) ? (sum_scd_h / sample_count) : NAN;
    float avg_co2   = (sample_count>0) ? (sum_co2   / sample_count) : NAN;

    publishHistoryMinute(avg_aht_t, avg_aht_h, avg_scd_t, avg_scd_h, avg_co2);

    sum_aht_t=sum_aht_h=sum_scd_t=sum_scd_h=sum_co2=0;
    sample_count=0;
  }

  // 3초마다 시리얼 + MQTT 상태 전송
  static uint32_t lastLineMs = 0;
  if (now - lastLineMs >= 3000) {
    lastLineMs = now;
    Serial.println(buildLine());
    publishStatus();
  }
}
