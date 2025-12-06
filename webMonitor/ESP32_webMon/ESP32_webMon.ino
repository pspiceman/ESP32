// 기능:
//  1. 5초 간격 센서 측정 + MQTT 전송 (esp32c3/status)
//  2. LED 제어 (esp32c3/led : LED_ON / LED_OFF)
//  3. Soft Reset (esp32c3/reset : SOFT_RESET → LED 빠른 깜빡임 후 ESP.restart())
//  4. 통신 시 LED(10) 순간 Blink(약 80ms)
//  5. 알람 설정(esp32c3/config/alarm)을 ESP32 Preferences에 저장 및 재전송
//  6. 1분 평균 60개 이력(테이블/그래프용) ESP32 Preferences에 저장 및 재전송 (esp32c3/history)
//  7. 기본 WiFi: Backhome / 1700note (저장값 없을 때)
//  8. AP모드: AP_KEY_PIN(9)을 3초 이상 누르면 LED(10) 빠른 Blink → AP 모드 전환 → WiFi 설정 웹 → 재시작
//     - 오픈 AP SSID: ESP32C3-SETUP (비밀번호 없음)
//     - /       : WiFi 스캔/선택, SSID/암호 입력 페이지
//     - /scan   : JSON으로 AP 목록
//     - /save   : 저장 후 2초 뒤 재부팅
//  9. MQTT broker: broker.hivemq.com
// 10. 브라우저 저장기능은 사용하지 않고, 이력은 ESP32 Preferences 기준으로만 유지

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>

// ---------- SCD4x 사용/비사용 옵션 ----------
// 1 → SCD4x 실제 센서 사용
// 0 → 센서 비사용(온도/습도/CO2 랜덤값 유지 = 현재 동작)
#define USE_SCD4X 0

#if USE_SCD4X
#include <SensirionI2cScd4x.h>
SensirionI2cScd4x scd4x;
bool scd4xStarted = false;
#endif

// ---------- 핀 정의 (ESP32 보드에 맞게 사용) ----------
#define BOARD_ESP32  // 혹은 BOARD_ESP32

// -----------------------------
// 보드별 핀 정의
// -----------------------------
#ifdef BOARD_ESP32
const int LED_PIN = 2;  // 통신/상태 LED
const bool LED_ACTIVE_HIGH = true;  // Active HIGH
const int AP_KEY_PIN = 0;  // AP 모드 전환 버튼

#elif defined(BOARD_ESP32C3)
const int LED_PIN = 10;              // 통신/상태 LED
const bool LED_ACTIVE_HIGH = true;   // Active HIGH
const int AP_KEY_PIN = 9;            // AP 모드 전환 버튼

#else
#error "지원되지 않는 보드입니다."
#endif

// 논리적 LED On/Off → 실제 핀 레벨로 변환
void setLed(bool on) {
  if (LED_ACTIVE_HIGH) {
    digitalWrite(LED_PIN, on ? HIGH : LOW);
  } else {
    digitalWrite(LED_PIN, on ? LOW : HIGH);
  }
}

// ---------- MQTT ----------
const char* MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// ---------- 토픽 ----------
const char* TOPIC_STATUS = "esp32c3/status";
const char* TOPIC_LED = "esp32c3/led";
const char* TOPIC_RESET = "esp32c3/reset";
const char* TOPIC_ALARM = "esp32c3/config/alarm";

const char* TOPIC_HISTORY_META = "esp32c3/history/meta";

// ---------- 주기 ----------
const unsigned long SAMPLE_INTERVAL_MS = 5000;  // 5s

// ---------- Preferences 네임스페이스 ----------
const char* PREF_WIFI_NS = "wifi";
const char* PREF_ALARM_NS = "alarm";
const char* PREF_HISTORY_NS = "history";

// WiFi 기본값
const char* WIFI_SSID_DEFAULT = "Backhome";
const char* WIFI_PASS_DEFAULT = "1700note";

// ---------- 전역 ----------
WiFiClient netClient;
PubSubClient mqtt(netClient);
WebServer server(80);

Preferences prefsWifi;
Preferences prefsAlarm;
Preferences prefsHistory;

String wifiSsid;
String wifiPass;

unsigned long lastSampleMs = 0;
bool ledLogicalState = false;  // MQTT LED_ON/OFF 상태
bool apMode = false;           // AP 포털 모드 여부

unsigned long apPressStartMs = 0;
bool apLastPressed = false;

// 상태 캐시
float g_temp = NAN;
float g_hum = NAN;
float g_co2 = NAN;
int g_solar = 0;

// 알람 설정 JSON
String g_alarmJson;

// ---------- 히스토리 버킷 ----------
struct MinuteBucket {
  uint32_t startMs;  // 버킷 시작 시각(ms, epoch 기반)

  float tempSum;
  float humSum;
  float co2Sum;
  float solarSum;
  float rssiSum;

  uint16_t tempCount;
  uint16_t humCount;
  uint16_t co2Count;
  uint16_t solarCount;
  uint16_t rssiCount;

  uint8_t led;  // 0: unknown, 1: OFF, 2: ON
  uint8_t _pad[3];
};

const size_t MAX_BUCKETS = 60;
MinuteBucket g_buckets[MAX_BUCKETS];
uint8_t g_bucketCount = 0;

// ---------- AP 포털 페이지 ----------
const char PAGE_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='ko'><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>ESP32 WiFi 설정</title>
<style>
body{font-family:-apple-system,'Segoe UI',sans-serif;background:#020617;color:#e5e7eb;padding:24px;margin:0;}
label{margin-top:12px;display:block}
input,select{width:100%;padding:8px;margin-top:4px;border:1px solid #4b5563;border-radius:8px;background:#020617;color:#e5e7eb;}
button{margin-top:16px;padding:10px;background:#2563eb;border-radius:999px;color:white;font-weight:700;border:none;cursor:pointer;}
body>div{max-width:360px;margin:auto;border:1px solid #1f2937;padding:20px;border-radius:12px;}
</style>
<script>
async function scan(){
  const r = await fetch('/scan');
  const list = await r.json();
  const sel = document.getElementById('ssidList');
  sel.innerHTML = "<option value=''>SSID 선택…</option>";
  list.forEach(ap=>{
    const o=document.createElement('option');
    o.value=ap.ssid;
    o.innerText=ap.ssid+" ("+ap.rssi+" dBm)";
    sel.appendChild(o);
  });
}
function setSSID(){
  const v=document.getElementById('ssidList').value;
  if(v) document.getElementById('ssid').value=v;
}
window.onload = scan;
</script>
</head>
<body><div>
<h2 style='font-size:1.6rem;font-weight:700;'>WiFi 설정</h2>
<label>WiFi 목록 (자동 스캔)</label>
<select id='ssidList' onchange='setSSID()'>
  <option value=''>스캔 중…</option>
</select>
<label>SSID</label>
<input id='ssid' name='ssid' placeholder='SSID 입력'>
<label>비밀번호</label>
<input id='pass' name='pass' type='password' placeholder='비밀번호 (없으면 비워두기)'>
<form method='POST' action='/save'>
  <input type='hidden' id='ssidHidden' name='ssid'>
  <input type='hidden' id='passHidden' name='pass'>
  <button type='submit' onclick="
    document.getElementById('ssidHidden').value=document.getElementById('ssid').value;
    document.getElementById('passHidden').value=document.getElementById('pass').value;
  ">저장 & 재부팅</button>
</form>
<p style='font-size:0.85rem;color:#9ca3af;margin-top:12px;'>
AP키 3초 이상 누르면 LED(10)이 빠르게 깜빡이고 AP모드로 전환됩니다.<br>
WiFi 설정 후 저장하면 2초 후 재부팅되어 정상 모드로 동작합니다.
</p>
</div></body></html>
)rawliteral";

const char PAGE_SAVED[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='ko'>
<head><meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{display:flex;justify-content:center;align-items:center;height:100vh;
background:#020617;color:#e5e7eb;font-family:-apple-system,'Segoe UI';text-align:center;}
h2{font-size:1.8rem;font-weight:700;}
</style></head>
<body>
<h2>WiFi 설정이 저장되었습니다.<br>2초 후 재부팅합니다…</h2>
</body></html>
)rawliteral";

// ---------- 프로토타입 ----------
void startAPMode(const char* reason);
void handleAPLongPress();
void connectWiFi();
void ensureTime();

void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void initScd4x();
bool readSensors(float& temp, float& hum, float& co2, int& solar);

void publishStatus();

void addSampleToHistory(float temp, float hum, float co2, int solar, float rssi, bool ledOn);
void loadHistoryFromPreferences();
void saveHistoryToPreferences();
void publishHistoryMeta();
void publishHistoryBucket(uint8_t idx);
void publishHistoryAll();

void loadAlarmFromPreferences();
void saveAlarmToPreferences();
void publishAlarmConfig();

void ledBlinkOnce(uint16_t ms = 80);
void ledFastBlink(uint8_t times, uint16_t msOnOff);

// ===================== SETUP ==========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("=== ESP32-C3 SCD4x + HiveMQ 데모 시작 ==="));

  pinMode(LED_PIN, OUTPUT);
  setLed(false);  // LED “꺼짐” 상태로 시작

  pinMode(AP_KEY_PIN, INPUT_PULLUP);

  randomSeed(esp_random());

#if USE_SCD4X
  // I2C 초기화 (SCD4x 사용 시에만)
  Wire.begin();
  Wire.setClock(100000);
#endif

  prefsWifi.begin(PREF_WIFI_NS, false);
  prefsAlarm.begin(PREF_ALARM_NS, false);
  prefsHistory.begin(PREF_HISTORY_NS, false);

  connectWiFi();
  ensureTime();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  initScd4x();
  loadAlarmFromPreferences();
  loadHistoryFromPreferences();

  publishAlarmConfig();
  publishHistoryAll();  // 부팅 시 한번만 전체 히스토리 publish

  Serial.println(F("=== setup 완료 ==="));
}

// ===================== LOOP ==========================
void loop() {
  handleAPLongPress();

  if (apMode) {
    // AP 모드: LED 100ms 빠른 깜빡임 지속 + 포털 처리
    static unsigned long lastBlinkMs = 0;
    static bool apBlinkState = false;
    unsigned long nowBlink = millis();
    if (nowBlink - lastBlinkMs >= 100) {
      apBlinkState = !apBlinkState;
      setLed(apBlinkState);
      lastBlinkMs = nowBlink;
    }
    server.handleClient();
    delay(10);
    return;
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;

    float temp = NAN;
    float hum = NAN;
    float co2 = NAN;
    int solar = 0;

    bool ok = readSensors(temp, hum, co2, solar);
    if (ok) {
      g_temp = temp;
      g_hum = hum;
      g_co2 = co2;
    }
    g_solar = solar;  // 일사량은 항상 갱신 (랜덤 또는 센서)

    publishStatus();  // ★ 여기서만 LED 80ms 블링크
    addSampleToHistory(g_temp, g_hum, g_co2, g_solar, WiFi.RSSI(), ledLogicalState);
  }
}

// ===================== WiFi & AP ==========================

void connectWiFi() {
  wifiSsid = prefsWifi.getString("ssid", WIFI_SSID_DEFAULT);
  wifiPass = prefsWifi.getString("pass", WIFI_PASS_DEFAULT);

  Serial.printf("WiFi STA 연결 시도: SSID=%s\n", wifiSsid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi 연결 성공, IP: ");
    Serial.println(WiFi.localIP());
    ledFastBlink(3, 60);
  } else {
    Serial.println("WiFi 연결 실패, AP 모드로 전환");
    startAPMode("wifi-connect-fail");
  }
}

void startAPMode(const char* reason) {
  if (apMode) return;

  Serial.printf("\n*** AP 모드 진입 (%s) ***\n", reason);

  if (mqtt.connected()) mqtt.disconnect();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(300);

  apMode = true;

  WiFi.mode(WIFI_AP);
  delay(300);

  const char* apSsid = "ESP32C3-SETUP";
  bool ok = WiFi.softAP(apSsid, NULL, 1, 0, 4);

  if (!ok) {
    Serial.println("AP 시작 실패 (softAP() false)");
  } else {
    IPAddress ip = WiFi.softAPIP();
    Serial.print("AP 시작 성공: ");
    Serial.print(apSsid);
    Serial.print("  IP: ");
    Serial.println(ip);
  }

  server.stop();

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PAGE_INDEX);
  });

  server.on("/scan", HTTP_GET, []() {
    Serial.println("AP 스캔 요청 수신");
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"";
      json += WiFi.SSID(i);
      json += "\",\"rssi\":";
      json += WiFi.RSSI(i);
      json += "}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/save", HTTP_POST, []() {
    if (!server.hasArg("ssid")) {
      server.send(400, "text/plain", "ssid required");
      return;
    }
    String ssid = server.arg("ssid");
    String pass = server.hasArg("pass") ? server.arg("pass") : "";

    prefsWifi.putString("ssid", ssid);
    prefsWifi.putString("pass", pass);

    Serial.print("새 WiFi 저장: ");
    Serial.print(ssid);
    Serial.print(" / ");
    Serial.println(pass);

    server.send_P(200, "text/html", PAGE_SAVED);
    delay(2000);
    ESP.restart();
  });

  server.begin();
}

void handleAPLongPress() {
  if (apMode) {
    apLastPressed = (digitalRead(AP_KEY_PIN) == LOW);
    return;
  }

  bool pressed = (digitalRead(AP_KEY_PIN) == LOW);
  unsigned long now = millis();

  if (pressed && !apLastPressed) {
    apPressStartMs = now;
  }

  if (pressed) {
    if (apPressStartMs != 0 && (now - apPressStartMs >= 3000)) {
      startAPMode("button-long-press");
    }
  } else {
    apPressStartMs = 0;
  }
  apLastPressed = pressed;
}

// ===================== MQTT ==========================

void connectMQTT() {
  if (apMode) return;

  while (!mqtt.connected() && !apMode) {
    String clientId = "esp32c3-";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.print("MQTT 연결 시도: ");
    Serial.println(clientId);

    if (mqtt.connect(clientId.c_str())) {
      Serial.println("MQTT 연결 성공");
      mqtt.subscribe(TOPIC_LED);
      mqtt.subscribe(TOPIC_RESET);
      mqtt.subscribe(TOPIC_ALARM);
      mqtt.subscribe("esp32c3/history/#");
    } else {
      Serial.print("MQTT 연결 실패, rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("MQTT 수신 [%s]: %s\n", topic, msg.c_str());
  // LED 깜빡임은 STATUS publish 시에만 수행

  if(strcmp(topic, TOPIC_LED) == 0) {
    if (msg == "LED_ON") {
      ledLogicalState = true;
      setLed(true);
    } else if (msg == "LED_OFF") {
      ledLogicalState = false;
      setLed(false);
    }
    return;
  }

  if (strcmp(topic, TOPIC_RESET) == 0) {
    if (msg == "SOFT_RESET") {
      Serial.println("SOFT_RESET 명령 수신 → 빠른 블링크 후 재시작");
      ledFastBlink(10, 80);
      ESP.restart();
    }
    return;
  }

  if (strcmp(topic, TOPIC_ALARM) == 0) {
    g_alarmJson = msg;
    saveAlarmToPreferences();
    Serial.println("알람 설정 수신 & 저장");
    return;
  }
}

// ===================== SCD4x ==========================

void initScd4x() {
#if USE_SCD4X
  Serial.println("SCD4x 사용: 센서 초기화");

  // 라이브러리 시그니처: begin(TwoWire&, uint8_t)
  scd4x.begin(Wire, 0x62);

  int16_t error;

  // 혹시 이전 측정이 돌고 있으면 정지(에러코드는 굳이 체크 안 함)
  error = scd4x.stopPeriodicMeasurement();
  (void)error;

  error = scd4x.startPeriodicMeasurement();
  if (error) {
    scd4xStarted = false;
    Serial.print("SCD4x startPeriodicMeasurement 에러 코드: ");
    Serial.println(error);
  } else {
    scd4xStarted = true;
    Serial.println("SCD4x 측정 시작");
  }
#else
  Serial.println("SCD4x 제거: 온도/습도/CO2 랜덤 값 사용 모드");
#endif
}

bool readSensors(float& temp, float& hum, float& co2, int& solar) {
#if USE_SCD4X
  if (scd4xStarted) {
    int16_t  error;
    uint16_t co2Raw;
    float    temperature;
    float    humidity;

    // 라이브러리 시그니처: readMeasurement(uint16_t&, float&, float&)
    error = scd4x.readMeasurement(co2Raw, temperature, humidity);
    if (!error && co2Raw != 0) {
      temp  = temperature;
      hum   = humidity;
      co2   = (float)co2Raw;
      solar = random(400, 1200);  // 별도 일사 센서 없으므로 랜덤 유지
      return true;
    } else {
      Serial.print("SCD4x readMeasurement 에러/무효: ");
      Serial.println(error);
    }
  }
#endif

  // === 기본(또는 센서 실패/비사용) 랜덤 모드 ===
  // 온도 : 20.0 ~ 30.0 ℃
  // 습도 : 30.0 ~ 70.0 %
  // CO2  : 400 ~ 2000 ppm
  // solar: 400 ~ 1200 (기존과 동일, 단위는 임의)

  temp = random(200, 301) / 10.0f;  // 20.0 ~ 30.0
  hum  = random(300, 701) / 10.0f;  // 30.0 ~ 70.0
  co2  = (float)random(400, 2001);  // 400 ~ 2000
  solar = random(400, 1200);        // 기존과 동일

  return true;  // 항상 측정 성공으로 처리
}

// ===================== STATUS ==========================

void publishStatus() {
  if (!mqtt.connected() || apMode) return;

  String json;
  json.reserve(256);

  json += "{";
  json += "\"alive\":true";

  if (!isnan(g_temp)) {
    json += ",\"temp\":";
    json += String(g_temp, 2);
  }
  if (!isnan(g_hum)) {
    json += ",\"hum\":";
    json += String(g_hum, 2);
  }
  if (!isnan(g_co2)) {
    json += ",\"co2\":";
    json += String(g_co2, 1);
  }
  json += ",\"solar\":";
  json += String(g_solar);

  json += ",\"led\":";
  json += (ledLogicalState ? "true" : "false");

  json += ",\"rssi\":";
  json += String(WiFi.RSSI());

  json += "}";

  Serial.print("STATUS publish: ");
  Serial.println(json);

  mqtt.publish(TOPIC_STATUS, json.c_str(), true);

  // ★ 5초마다 80ms Blink
  ledBlinkOnce();
}

// ===================== 시간 동기화 ==========================

void ensureTime() {
  configTime(9 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP 시간 동기화 시도...");

  time_t now = 0;
  uint8_t retry = 0;
  while (now < 8 * 3600 && retry < 10) {
    delay(1000);
    now = time(nullptr);
    retry++;
  }
  if (now > 0) {
    struct tm t;
    localtime_r(&now, &t);
    Serial.printf("현재 시간: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    Serial.println("시간 동기화 실패");
  }
}

// ===================== 히스토리 ==========================

void addSampleToHistory(float temp, float hum, float co2, int solar, float rssi, bool ledOn) {
  time_t nowSec = time(nullptr);
  if (nowSec <= 0) return;

  uint32_t bucketStartSec = nowSec - (nowSec % 60);
  uint32_t bucketStartMs = bucketStartSec * 1000UL;

  MinuteBucket* bucket = nullptr;
  bool newBucket = false;

  if (g_bucketCount > 0 && g_buckets[g_bucketCount - 1].startMs == bucketStartMs) {
    bucket = &g_buckets[g_bucketCount - 1];
  } else {
    if (g_bucketCount == MAX_BUCKETS) {
      for (uint8_t i = 1; i < g_bucketCount; i++) {
        g_buckets[i - 1] = g_buckets[i];
      }
      g_bucketCount--;
    }
    bucket = &g_buckets[g_bucketCount++];
    memset(bucket, 0, sizeof(MinuteBucket));
    bucket->startMs = bucketStartMs;
    newBucket = true;

    Serial.printf("새 히스토리 버킷 생성: startSec=%lu, bucketCount=%u\n",
                  (unsigned long)bucketStartSec, g_bucketCount);
  }

  if (!isnan(temp)) {
    bucket->tempSum += temp;
    bucket->tempCount++;
  }
  if (!isnan(hum)) {
    bucket->humSum += hum;
    bucket->humCount++;
  }
  if (!isnan(co2)) {
    bucket->co2Sum += co2;
    bucket->co2Count++;
  }
  if (!isnan((float)solar)) {
    bucket->solarSum += solar;
    bucket->solarCount++;
  }
  if (!isnan(rssi)) {
    bucket->rssiSum += rssi;
    bucket->rssiCount++;
  }

  bucket->led = ledOn ? 2 : 1;  // 2=ON, 1=OFF

  saveHistoryToPreferences();

  if (!mqtt.connected() || apMode) return;

  // 새 버킷이 생긴 경우에만 meta 갱신
  if (newBucket) {
    publishHistoryMeta();
  }
  // 마지막 버킷만 갱신해서 전송
  publishHistoryBucket(g_bucketCount - 1);
}

void loadHistoryFromPreferences() {
  g_bucketCount = prefsHistory.getUChar("count", 0);
  if (g_bucketCount > MAX_BUCKETS) g_bucketCount = MAX_BUCKETS;

  bool invalid = false;

  for (uint8_t i = 0; i < g_bucketCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "b%u", i);
    size_t len = prefsHistory.getBytesLength(key);
    if (len == sizeof(MinuteBucket)) {
      prefsHistory.getBytes(key, &g_buckets[i], sizeof(MinuteBucket));
      if (g_buckets[i].startMs == 0) {
        invalid = true;
      }
    } else {
      invalid = true;
    }
  }

  if (invalid) {
    Serial.println("히스토리 포맷 변경 감지 → 전체 초기화");
    prefsHistory.clear();
    g_bucketCount = 0;
    memset(g_buckets, 0, sizeof(g_buckets));
  }

  Serial.printf("히스토리 로드 완료: %u buckets\n", g_bucketCount);
}

void saveHistoryToPreferences() {
  prefsHistory.putUChar("count", g_bucketCount);
  for (uint8_t i = 0; i < g_bucketCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "b%u", i);
    prefsHistory.putBytes(key, &g_buckets[i], sizeof(MinuteBucket));
  }
  Serial.printf("히스토리 저장 (bucket=%u)\n", g_bucketCount);
}

void publishHistoryMeta() {
  if (!mqtt.connected() || apMode) return;

  String meta;
  meta.reserve(64);
  meta += "{\"version\":1,\"count\":";
  meta += String(g_bucketCount);
  meta += "}";

  mqtt.publish(TOPIC_HISTORY_META, meta.c_str(), true);
}

void publishHistoryBucket(uint8_t idx) {
  if (!mqtt.connected() || apMode) return;
  if (idx >= MAX_BUCKETS) return;

  String topic = "esp32c3/history/bucket/";  
  topic += String(idx);

  if (idx >= g_bucketCount) {
    // 사용 안 하는 인덱스는 빈 메시지로 삭제
    mqtt.publish(topic.c_str(), "", true);
    return;
  }

  MinuteBucket& b = g_buckets[idx];

  if (b.startMs == 0) {
    mqtt.publish(topic.c_str(), "", true);
    return;
  }

  String json;
  json.reserve(256);
  json += "{";
  json += "\"start\":";
  json += String(b.startMs);

  if (b.tempCount > 0) {
    json += ",\"temp\":";
    json += String(b.tempSum / b.tempCount, 2);
  }
  if (b.humCount > 0) {
    json += ",\"hum\":";
    json += String(b.humSum / b.humCount, 2);
  }
  if (b.co2Count > 0) {
    json += ",\"co2\":";
    json += String(b.co2Sum / b.co2Count, 1);
  }
  if (b.solarCount > 0) {
    json += ",\"solar\":";
    json += String(b.solarSum / b.solarCount, 0);
  }
  if (b.rssiCount > 0) {
    json += ",\"rssi\":";
    json += String(b.rssiSum / b.rssiCount, 0);
  }

  if (b.led == 1) {
    json += ",\"led\":\"OFF\"";
  } else if (b.led == 2) {
    json += ",\"led\":\"ON\"";
  }

  json += "}";

  mqtt.publish(topic.c_str(), json.c_str(), true);
}

void publishHistoryAll() {
  if (!mqtt.connected() || apMode) return;

  publishHistoryMeta();

  // 이미 사용 중인 버킷들만 전송
  for (uint8_t i = 0; i < g_bucketCount; i++) {
    publishHistoryBucket(i);
  }

  // 나머지 인덱스는 비우기
  for (uint8_t i = g_bucketCount; i < MAX_BUCKETS; i++) {
    String topic = "esp32c3/history/bucket/";
    topic += String(i);
    mqtt.publish(topic.c_str(), "", true);
  }

  Serial.printf("HISTORY publish all (count=%u)\n", g_bucketCount);
}

// ===================== 알람 ==========================

void loadAlarmFromPreferences() {
  g_alarmJson = prefsAlarm.getString("alarm", "");
  if (g_alarmJson.length() > 0) {
    Serial.println("알람 설정 로드:");
    Serial.println(g_alarmJson);
  } else {
    Serial.println("저장된 알람 설정 없음");
  }
}

void saveAlarmToPreferences() {
  prefsAlarm.putString("alarm", g_alarmJson);
  Serial.println("알람 설정 저장 완료");
}

void publishAlarmConfig() {
  if (!mqtt.connected() || apMode) return;
  if (g_alarmJson.length() == 0) return;

  mqtt.publish(TOPIC_ALARM, g_alarmJson.c_str(), true);
  Serial.println("알람 설정 MQTT 전송");
}

// ===================== LED 유틸 ==========================

void ledBlinkOnce(uint16_t ms) {
  setLed(true);
  delay(ms);
  setLed(ledLogicalState);  // MQTT가 기억하는 상태로 복귀
}

void ledFastBlink(uint8_t times, uint16_t msOnOff) {
  for (uint8_t i = 0; i < times; i++) {
    setLed(true);
    delay(msOnOff);
    setLed(false);
    delay(msOnOff);
  }
}
