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

// =====================================================
// 사용자 옵션
// =====================================================
#define USE_SCD4X 0         // 1: SCD4x 사용, 0: 랜덤값
#define SERIAL_VERBOSE 1    // 1: 로그 많이 출력, 0: 최소 출력

// 5초 주기(원하면 3000으로 변경)
const unsigned long SAMPLE_INTERVAL_MS = 5000;  // 3초로 바꾸려면 3000

// =====================================================
// SCD4x
// =====================================================
#if USE_SCD4X
#include <SensirionI2cScd4x.h>
SensirionI2cScd4x scd4x;
bool scd4xStarted = false;
#endif

// =====================================================
// 보드/핀 설정
// =====================================================
#define BOARD_ESP32C3

#ifdef BOARD_ESP32
const int LED_PIN = 2;              // 외부LED(GPIO15:Active HIGH), 내부(2:Active HIGH)
const bool LED_ACTIVE_HIGH = true;  // true(Active HIGH), false(Active LOW)
const int AP_KEY_PIN = 0;           // AP 전환 버튼
#elif defined(BOARD_ESP32C3)
const int LED_PIN = 8;              // 외부LED(GPIO10:Active HIGH), 내부(8:Active LOW)
const bool LED_ACTIVE_HIGH = false; // true(Active HIGH), false(Active LOW)
const int AP_KEY_PIN = 9;           // AP 전환 버튼
#else
#error "지원되지 않는 보드입니다."
#endif

void setLedRaw(bool on) {
  if (LED_ACTIVE_HIGH) digitalWrite(LED_PIN, on ? HIGH : LOW);
  else digitalWrite(LED_PIN, on ? LOW : HIGH);
}

// =====================================================
// MQTT
// =====================================================
const char* MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// 토픽
const char* TOPIC_STATUS = "esp32c3/status";
const char* TOPIC_LED    = "esp32c3/led";
const char* TOPIC_RESET  = "esp32c3/reset";
const char* TOPIC_ALARM  = "esp32c3/config/alarm";

const char* TOPIC_HISTORY_META = "esp32c3/history/meta";
// 버킷 토픽: esp32c3/history/bucket/<idx>

// =====================================================
// Preferences 네임스페이스
// =====================================================
const char* PREF_WIFI_NS    = "wifi";
const char* PREF_ALARM_NS   = "alarm";
const char* PREF_HISTORY_NS = "history";

const char* WIFI_SSID_DEFAULT = "Backhome";
const char* WIFI_PASS_DEFAULT = "1700note";

// =====================================================
// 전역 객체
// =====================================================
WiFiClient netClient;
PubSubClient mqtt(netClient);
WebServer server(80);

Preferences prefsWifi;
Preferences prefsAlarm;
Preferences prefsHistory;

// =====================================================
// 상태 변수
// =====================================================
String wifiSsid;
String wifiPass;

unsigned long lastSampleMs = 0;

// 논리 LED 상태
bool ledLogicalState = false;

// 통신 Blink 상태 머신(80ms)
bool blinkActive = false;
unsigned long blinkUntilMs = 0;

// AP 모드
bool apMode = false;
unsigned long apPressStartMs = 0;
bool apLastPressed = false;

// 센서 캐시
float g_temp = NAN;
float g_hum  = NAN;
float g_co2  = NAN;
int   g_solar = 0;

// 알람 JSON
String g_alarmJson;

// =====================================================
// 히스토리 버킷 구조
// =====================================================
struct MinuteBucket {
  uint64_t startMs;

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

  uint8_t led;   // 1: OFF, 2: ON
  uint8_t _pad[7];
};

const size_t MAX_BUCKETS = 60;
MinuteBucket g_buckets[MAX_BUCKETS];
uint8_t g_bucketCount = 0;

// =====================================================
// AP 포털 페이지
// =====================================================
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
AP키 3초 이상 누르면 LED가 빠르게 깜빡이고 AP모드로 전환됩니다.<br>
WiFi 설정 후 저장하면 2초 후 재부팅됩니다.
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

// =====================================================
// 함수 선언
// =====================================================
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

void beginBlink(uint16_t ms = 80);
void updateBlink();
void ledFastBlink(uint8_t times, uint16_t msOnOff);

uint64_t nowEpochMs();

// =====================================================
// 유틸
// =====================================================
uint64_t nowEpochMs() {
  time_t nowSec = time(nullptr);
  if (nowSec <= 0) return 0;
  return (uint64_t)nowSec * 1000ULL;
}

void beginBlink(uint16_t ms) {
  blinkActive = true;
  blinkUntilMs = millis() + ms;
  setLedRaw(true);
}

void updateBlink() {
  if (!blinkActive) return;
  if ((long)(millis() - blinkUntilMs) >= 0) {
    blinkActive = false;
    setLedRaw(ledLogicalState);
  }
}

void ledFastBlink(uint8_t times, uint16_t msOnOff) {
  bool prev = ledLogicalState;
  for (uint8_t i = 0; i < times; i++) {
    setLedRaw(true);  delay(msOnOff);
    setLedRaw(false); delay(msOnOff);
  }
  setLedRaw(prev);
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(800);

#if SERIAL_VERBOSE
  Serial.println();
  Serial.println(F("=== ESP32-C3 MQTT + AP + History (최종 정리본) ==="));
#endif

  pinMode(LED_PIN, OUTPUT);
  setLedRaw(false);

  pinMode(AP_KEY_PIN, INPUT_PULLUP);

  randomSeed(esp_random());

#if USE_SCD4X
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

  // 부팅 시 알람/히스토리 재전송
  publishAlarmConfig();
  publishHistoryAll();

#if SERIAL_VERBOSE
  Serial.println(F("=== setup 완료 ==="));
#endif
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  handleAPLongPress();

  if (apMode) {
    // AP 모드: LED 100ms 점멸 + 웹 서버
    static unsigned long lastBlinkMs = 0;
    static bool s = false;
    if (millis() - lastBlinkMs >= 100) {
      s = !s;
      setLedRaw(s);
      lastBlinkMs = millis();
    }
    server.handleClient();
    delay(5);
    return;
  }

  // MQTT 유지
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  updateBlink();

  unsigned long now = millis();
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;

    float temp = NAN, hum = NAN, co2 = NAN;
    int solar = 0;

    bool ok = readSensors(temp, hum, co2, solar);
    if (ok) {
      g_temp = temp;
      g_hum = hum;
      g_co2 = co2;
    }
    g_solar = solar;

    publishStatus();
    addSampleToHistory(g_temp, g_hum, g_co2, g_solar, WiFi.RSSI(), ledLogicalState);
  }
}

// =====================================================
// WiFi / AP
// =====================================================
void connectWiFi() {
  wifiSsid = prefsWifi.getString("ssid", WIFI_SSID_DEFAULT);
  wifiPass = prefsWifi.getString("pass", WIFI_PASS_DEFAULT);

#if SERIAL_VERBOSE
  Serial.printf("WiFi STA 연결 시도: SSID=%s\n", wifiSsid.c_str());
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
#if SERIAL_VERBOSE
    Serial.print(".");
#endif
    attempts++;
  }
#if SERIAL_VERBOSE
  Serial.println();
#endif

  if (WiFi.status() == WL_CONNECTED) {
#if SERIAL_VERBOSE
    Serial.print("WiFi 연결 성공, IP: ");
    Serial.println(WiFi.localIP());
#endif
    ledFastBlink(3, 60);
  } else {
#if SERIAL_VERBOSE
    Serial.println("WiFi 연결 실패, AP 모드로 전환");
#endif
    startAPMode("wifi-connect-fail");
  }
}

void startAPMode(const char* reason) {
  if (apMode) return;

#if SERIAL_VERBOSE
  Serial.printf("\n*** AP 모드 진입 (%s) ***\n", reason);
#endif

  if (mqtt.connected()) mqtt.disconnect();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(250);

  apMode = true;

  WiFi.mode(WIFI_AP);
  delay(250);

  const char* apSsid = "ESP32C3-SETUP";
  bool ok = WiFi.softAP(apSsid, NULL, 1, 0, 4);

#if SERIAL_VERBOSE
  if (!ok) Serial.println("AP 시작 실패");
  else {
    Serial.print("AP 시작 성공: ");
    Serial.print(apSsid);
    Serial.print(" IP: ");
    Serial.println(WiFi.softAPIP());
  }
#endif

  server.stop();

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PAGE_INDEX);
  });

  server.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i) json += ",";
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

#if SERIAL_VERBOSE
    Serial.print("새 WiFi 저장: ");
    Serial.print(ssid);
    Serial.print(" / ");
    Serial.println(pass);
#endif

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

  if (pressed && !apLastPressed) apPressStartMs = now;

  if (pressed) {
    if (apPressStartMs != 0 && (now - apPressStartMs >= 3000)) {
      startAPMode("button-long-press");
    }
  } else {
    apPressStartMs = 0;
  }

  apLastPressed = pressed;
}

// =====================================================
// MQTT
// =====================================================
void connectMQTT() {
  if (apMode) return;

  while (!mqtt.connected() && !apMode) {
    uint64_t chipid = ESP.getEfuseMac();
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "esp32c3-%04X%08X",
             (uint16_t)(chipid >> 32), (uint32_t)chipid);

#if SERIAL_VERBOSE
    Serial.print("MQTT 연결 시도: ");
    Serial.println(clientId);
#endif

    if (mqtt.connect(clientId)) {
#if SERIAL_VERBOSE
      Serial.println("MQTT 연결 성공");
#endif
      bool s1 = mqtt.subscribe(TOPIC_LED);
      bool s2 = mqtt.subscribe(TOPIC_RESET);
      bool s3 = mqtt.subscribe(TOPIC_ALARM);

#if SERIAL_VERBOSE
      Serial.printf("SUB LED=%d RESET=%d ALARM=%d\n", s1, s2, s3);
#endif
      // ✅ C3는 history/# 구독하지 않음 (자기 에코 제거)
    } else {
#if SERIAL_VERBOSE
      Serial.print("MQTT 연결 실패, rc=");
      Serial.println(mqtt.state());
#endif
      delay(1500);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

#if SERIAL_VERBOSE
  Serial.printf("MQTT 수신 [%s]: [%s]\n", topic, msg.c_str());
#endif

  // LED 제어
  if (strcmp(topic, TOPIC_LED) == 0) {
    if (msg.equalsIgnoreCase("LED_ON")) {
      ledLogicalState = true;
      setLedRaw(true);
#if SERIAL_VERBOSE
      Serial.println("LED => ON");
#endif
    } else if (msg.equalsIgnoreCase("LED_OFF")) {
      ledLogicalState = false;
      setLedRaw(false);
#if SERIAL_VERBOSE
      Serial.println("LED => OFF");
#endif
    }
    return;
  }

  // Soft Reset
  if (strcmp(topic, TOPIC_RESET) == 0) {
    if (msg.equalsIgnoreCase("SOFT_RESET")) {
#if SERIAL_VERBOSE
      Serial.println("SOFT_RESET 수신 → 빠른 블링크 후 재시작");
#endif
      ledFastBlink(10, 80);
      ESP.restart();
    }
    return;
  }

  // 알람 설정
  if (strcmp(topic, TOPIC_ALARM) == 0) {
    // ✅ 동일 알람이면 무시 → 도배 방지
    if (msg == g_alarmJson) return;

    g_alarmJson = msg;
    saveAlarmToPreferences();

#if SERIAL_VERBOSE
    Serial.println("알람 설정 수신 & 저장");
#endif

    // ✅ 여기서 publishAlarmConfig() 호출하지 않음 (에코 루프 방지)
    return;
  }
}

// =====================================================
// SCD4x / 랜덤 센서
// =====================================================
void initScd4x() {
#if USE_SCD4X
#if SERIAL_VERBOSE
  Serial.println("SCD4x 사용: 센서 초기화");
#endif
  scd4x.begin(Wire, 0x62);

  scd4x.stopPeriodicMeasurement();
  int16_t error = scd4x.startPeriodicMeasurement();

  if (error) {
    scd4xStarted = false;
#if SERIAL_VERBOSE
    Serial.print("SCD4x startPeriodicMeasurement 에러: ");
    Serial.println(error);
#endif
  } else {
    scd4xStarted = true;
#if SERIAL_VERBOSE
    Serial.println("SCD4x 측정 시작");
#endif
  }
#else
#if SERIAL_VERBOSE
  Serial.println("SCD4x 비사용: 랜덤 센서 모드");
#endif
#endif
}

bool readSensors(float& temp, float& hum, float& co2, int& solar) {
#if USE_SCD4X
  if (scd4xStarted) {
    uint16_t co2Raw;
    float temperature, humidity;
    int16_t error = scd4x.readMeasurement(co2Raw, temperature, humidity);
    if (!error && co2Raw != 0) {
      temp = temperature;
      hum  = humidity;
      co2  = (float)co2Raw;
      solar = random(400, 1200);
      return true;
    }
  }
#endif

  temp  = random(200, 301) / 10.0f;
  hum   = random(300, 701) / 10.0f;
  co2   = (float)random(400, 2001);
  solar = random(400, 1200);
  return true;
}

// =====================================================
// STATUS publish
// =====================================================
void publishStatus() {
  if (!mqtt.connected() || apMode) return;

  String json;
  json.reserve(256);

  json += "{";
  json += "\"alive\":true";
  if (!isnan(g_temp)) { json += ",\"temp\":"; json += String(g_temp, 2); }
  if (!isnan(g_hum))  { json += ",\"hum\":";  json += String(g_hum, 2); }
  if (!isnan(g_co2))  { json += ",\"co2\":";  json += String(g_co2, 1); }

  json += ",\"solar\":";
  json += String(g_solar);

  json += ",\"led\":";
  json += (ledLogicalState ? "true" : "false");

  json += ",\"rssi\":";
  json += String(WiFi.RSSI());
  json += "}";

#if SERIAL_VERBOSE
  Serial.print("STATUS publish: ");
  Serial.println(json);
#endif

  mqtt.publish(TOPIC_STATUS, json.c_str(), true);

  // 통신 시 80ms Blink (delay 없이)
  beginBlink(80);
}

// =====================================================
// 시간 동기화
// =====================================================
void ensureTime() {
  configTime(9 * 3600, 0, "pool.ntp.org", "time.nist.gov");

#if SERIAL_VERBOSE
  Serial.println("NTP 시간 동기화 시도...");
#endif

  time_t now = 0;
  uint8_t retry = 0;
  while (now < 8 * 3600 && retry < 10) {
    delay(900);
    now = time(nullptr);
    retry++;
  }

#if SERIAL_VERBOSE
  if (now > 0) {
    struct tm t;
    localtime_r(&now, &t);
    Serial.printf("현재 시간: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    Serial.println("시간 동기화 실패 (epoch=0)");
  }
#endif
}

// =====================================================
// HISTORY
// =====================================================
// 구조:
// - 5초 샘플은 RAM 누적만 수행
// - 새 1분 버킷 생성 시에만 Preferences 저장(1분 1회)
// - MQTT 전송은 meta + 마지막 버킷만 갱신
void addSampleToHistory(float temp, float hum, float co2, int solar, float rssi, bool ledOn) {
  time_t nowSec = time(nullptr);
  if (nowSec <= 0) return;

  uint32_t bucketStartSec = nowSec - (nowSec % 60);
  uint64_t bucketStartMs = (uint64_t)bucketStartSec * 1000ULL;

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

#if SERIAL_VERBOSE
    Serial.printf("새 히스토리 버킷 생성: startSec=%lu, bucketCount=%u\n",
                  (unsigned long)bucketStartSec, g_bucketCount);
#endif
  }

  if (!isnan(temp)) { bucket->tempSum += temp; bucket->tempCount++; }
  if (!isnan(hum))  { bucket->humSum  += hum;  bucket->humCount++; }
  if (!isnan(co2))  { bucket->co2Sum  += co2;  bucket->co2Count++; }
  bucket->solarSum += (float)solar; bucket->solarCount++;
  bucket->rssiSum  += rssi;         bucket->rssiCount++;

  bucket->led = ledOn ? 2 : 1;

  // ✅ 1분에 1회만 저장
  if (newBucket) {
    saveHistoryToPreferences();
  }

  if (!mqtt.connected() || apMode) return;

  if (newBucket) publishHistoryMeta();
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
      if (g_buckets[i].startMs == 0) invalid = true;
    } else {
      invalid = true;
    }
  }

  if (invalid) {
#if SERIAL_VERBOSE
    Serial.println("히스토리 데이터 이상 감지 → 초기화");
#endif
    prefsHistory.clear();
    g_bucketCount = 0;
    memset(g_buckets, 0, sizeof(g_buckets));
  }

#if SERIAL_VERBOSE
  Serial.printf("히스토리 로드 완료: %u buckets\n", g_bucketCount);
#endif
}

void saveHistoryToPreferences() {
  prefsHistory.putUChar("count", g_bucketCount);
  for (uint8_t i = 0; i < g_bucketCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "b%u", i);
    prefsHistory.putBytes(key, &g_buckets[i], sizeof(MinuteBucket));
  }

#if SERIAL_VERBOSE
  Serial.printf("히스토리 저장 (bucket=%u)\n", g_bucketCount);
#endif
}

void publishHistoryMeta() {
  if (!mqtt.connected() || apMode) return;

  String meta;
  meta.reserve(80);
  meta += "{\"version\":2,\"count\":";
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
  json += String((unsigned long long)b.startMs);

  if (b.tempCount > 0) { json += ",\"temp\":"; json += String(b.tempSum / b.tempCount, 2); }
  if (b.humCount  > 0) { json += ",\"hum\":";  json += String(b.humSum  / b.humCount, 2); }
  if (b.co2Count  > 0) { json += ",\"co2\":";  json += String(b.co2Sum  / b.co2Count, 1); }
  if (b.solarCount> 0) { json += ",\"solar\":";json += String(b.solarSum/ b.solarCount, 0); }
  if (b.rssiCount > 0) { json += ",\"rssi\":"; json += String(b.rssiSum / b.rssiCount, 0); }

  if (b.led == 1) json += ",\"led\":\"OFF\"";
  else if (b.led == 2) json += ",\"led\":\"ON\"";

  json += "}";

  mqtt.publish(topic.c_str(), json.c_str(), true);
}

void publishHistoryAll() {
  if (!mqtt.connected() || apMode) return;

  publishHistoryMeta();

  for (uint8_t i = 0; i < g_bucketCount; i++) publishHistoryBucket(i);

  // 남은 버킷 삭제
  for (uint8_t i = g_bucketCount; i < MAX_BUCKETS; i++) {
    String topic = "esp32c3/history/bucket/";
    topic += String(i);
    mqtt.publish(topic.c_str(), "", true);
  }

#if SERIAL_VERBOSE
  Serial.printf("HISTORY publish all (count=%u)\n", g_bucketCount);
#endif
}

// =====================================================
// ALARM
// =====================================================
void loadAlarmFromPreferences() {
  g_alarmJson = prefsAlarm.getString("alarm", "");
#if SERIAL_VERBOSE
  if (g_alarmJson.length() > 0) {
    Serial.println("알람 설정 로드:");
    Serial.println(g_alarmJson);
  } else {
    Serial.println("저장된 알람 설정 없음");
  }
#endif
}

void saveAlarmToPreferences() {
  prefsAlarm.putString("alarm", g_alarmJson);
#if SERIAL_VERBOSE
  Serial.println("알람 설정 저장 완료");
#endif
}

// 부팅 시에만 재전송(원래 요구사항: 저장 및 재전송)
void publishAlarmConfig() {
  if (!mqtt.connected() || apMode) return;
  if (g_alarmJson.length() == 0) return;

  mqtt.publish(TOPIC_ALARM, g_alarmJson.c_str(), true);

#if SERIAL_VERBOSE
  Serial.println("알람 설정 MQTT 전송(부팅 동기화)");
#endif
}
