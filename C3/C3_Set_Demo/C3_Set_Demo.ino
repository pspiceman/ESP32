// 
// ===== C3_Set_Hivemq.ino =====
// ESP32-C3 + broker.hivemq.com 데모 + AP(설정 모드)

#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>

// 1) 기본 Wi-Fi (저장값 없을 때 사용하는 디폴트)
#define WIFI_SSID_DEFAULT "Backhome"
#define WIFI_PASS_DEFAULT "1700note"

// 2) 데모 MQTT 브로커 설정
// ===== MQTT 설정 =====
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;

#define LED_PIN    8   // LED 출력 핀 (active-low: LOW=ON)
#define AP_KEY_PIN 9   // AP 모드 버튼 (내부 풀업, 눌리면 LOW)

// MQTT 토픽
const char* TOPIC_STATUS = "esp32c3/status";
const char* TOPIC_LED    = "esp32c3/led";

const unsigned long STATUS_INTERVAL_MS = 3000;

// ===== NVS WiFi 설정 저장용 =====
const char* PREF_NS = "wifi_cfg";

Preferences prefs;
WebServer   server(80);

WiFiClient   netClient;      // 데모 브로커 → 일반 TCP 클라이언트
PubSubClient mqtt(netClient);

String wifiSsid;
String wifiPass;

unsigned long lastStatusMs = 0;
bool ledState = false;      // 논리 LED 상태 (true=켜짐)

// ===== 함수 선언 =====
void loadWiFiConfig();
void saveWiFiConfig(const String& ssid, const String& pass);
void clearWiFiConfig();

void connectWiFi();
void connectMQTT();
void publishStatus();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void handleRoot();
void handleSave();
void runAPPortal();
void handleAPLongPress();

// --------------------------------------------------------
// AP 설정 페이지 HTML
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
  "<p><small>MQTT 데모 브로커: broker.hivemq.com:1883</small></p>"
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
  "<h3>WiFi 설정이 저장되었습니다. 2초 후 재부팅합니다…</h3>"
  "</body></html>";

// --------------------------------------------------------
// WiFi 설정 NVS 로드/저장/삭제
// --------------------------------------------------------
void loadWiFiConfig() {
  prefs.begin(PREF_NS, true);
  if (prefs.isKey("ssid")) {
    wifiSsid = prefs.getString("ssid", WIFI_SSID_DEFAULT);
    wifiPass = prefs.getString("pass", WIFI_PASS_DEFAULT);
  } else {
    wifiSsid = WIFI_SSID_DEFAULT;
    wifiPass = WIFI_PASS_DEFAULT;
  }
  prefs.end();
}

void saveWiFiConfig(const String& ssid, const String& pass) {
  prefs.begin(PREF_NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearWiFiConfig() {
  prefs.begin(PREF_NS, false);
  prefs.clear();
  prefs.end();
}

// --------------------------------------------------------
// AP 모드 HTTP 핸들러
// --------------------------------------------------------
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", PAGE_INDEX);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    saveWiFiConfig(ssid, pass);
    server.send(200, "text/html; charset=utf-8", PAGE_SAVED);
    delay(2000);
    ESP.restart();
  } else {
    server.send(405, "text/plain", "Method Not Allowed");
  }
}

// --------------------------------------------------------
// setup
// --------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // active-low → HIGH=OFF
  ledState = false;

  pinMode(AP_KEY_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("=== C3_Set_Hivemq / broker.hivemq.com 데모 + AP ===");

  loadWiFiConfig();
  Serial.print("[WiFi] SSID = ");
  Serial.println(wifiSsid);

  WiFi.mode(WIFI_STA);
  connectWiFi();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();
}

// --------------------------------------------------------
// loop
// --------------------------------------------------------
void loop() {
  // AP 버튼 3초 길게 누르면 AP 설정 모드
  handleAPLongPress();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    publishStatus();
  }
}

// --------------------------------------------------------
// AP-Mode 버튼 길게 눌림 감지
// --------------------------------------------------------
void handleAPLongPress() {
  static bool          lastPressed = false;
  static unsigned long pressedAt   = 0;
  static bool          apTriggered = false;

  bool pressed = (digitalRead(AP_KEY_PIN) == LOW);  // LOW = 눌림

  if (pressed && !lastPressed) {
    pressedAt   = millis();
    apTriggered = false;
    Serial.println("[AP] 버튼 눌림 시작");
  }

  if (pressed && !apTriggered) {
    unsigned long held = millis() - pressedAt;
    if (held >= 3000) {   // 3초 이상 길게 누르면 AP 모드
      apTriggered = true;
      Serial.println("[AP] 3초 길게 눌림 → AP 모드 진입");
      runAPPortal();
    }
  }

  if (!pressed && lastPressed) {
    Serial.println("[AP] 버튼에서 손 뗌");
  }

  lastPressed = pressed;
}

// --------------------------------------------------------
// WiFi 연결
// --------------------------------------------------------
void connectWiFi() {
  Serial.print("[WiFi] 연결 시도: ");
  Serial.println(wifiSsid);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] 연결 성공, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] 연결 실패 (AP 버튼으로 설정 변경 가능)");
  }
}

// --------------------------------------------------------
// MQTT 연결 (broker.hivemq.com / no TLS, no auth)
// --------------------------------------------------------
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.println("[MQTT] broker.hivemq.com 연결 시도...");
    String clientId = "demo-" + String((uint32_t)esp_random(), HEX);

    bool ok = mqtt.connect(clientId.c_str());   // username/password 없음
    if (ok) {
      Serial.println("[MQTT] 연결 성공");
      mqtt.subscribe(TOPIC_LED);
      Serial.print("[MQTT] 구독: ");
      Serial.println(TOPIC_LED);

      publishStatus();  // 연결 직후 한 번 보내기
    } else {
      Serial.print("[MQTT] 실패, rc=");
      Serial.println(mqtt.state());
      delay(3000);
    }
  }
}

// --------------------------------------------------------
// 상태 Publish (랜덤 숫자 + LED 상태)
// --------------------------------------------------------
void publishStatus() {
  if (!mqtt.connected()) return;

  static bool seeded = false;
  if (!seeded) {
    randomSeed(esp_random());
    seeded = true;
  }

  int temp  = random(20, 33);     // 20~32℃
  int hum   = random(40, 81);     // 40~80%
  int co2   = random(380, 1200);  // 380~1199 ppm
  int solar = random(0, 1001);    // 0~1000
  long rssi = WiFi.RSSI();

  String payload = "{";
  payload += "\"temp\":"  + String(temp)  + ",";
  payload += "\"hum\":"   + String(hum)   + ",";
  payload += "\"co2\":"   + String(co2)   + ",";
  payload += "\"solar\":" + String(solar) + ",";
  payload += "\"led\":"   + String(ledState ? "true" : "false") + ",";
  payload += "\"rssi\":"  + String(rssi)  + ",";
  payload += "\"alive\":true";
  payload += "}";

  if (mqtt.publish(TOPIC_STATUS, payload.c_str())) {
    Serial.print("[PUB] ");
    Serial.println(payload);
  } else {
    Serial.println("[PUB] 전송 실패");
  }
}

// --------------------------------------------------------
// MQTT 콜백 (LED 제어, active-low)
// --------------------------------------------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("[MQTT] 수신 ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(msg);

  if (String(topic) == TOPIC_LED) {
    if (msg.indexOf("LED_ON") >= 0) {
      ledState = true;
      digitalWrite(LED_PIN, LOW);   // LOW = ON
      Serial.println("[LED] ON (LOW)");
    } else if (msg.indexOf("LED_OFF") >= 0) {
      ledState = false;
      digitalWrite(LED_PIN, HIGH);  // HIGH = OFF
      Serial.println("[LED] OFF (HIGH)");
    }
  }
}

// --------------------------------------------------------
// AP 포털 (WiFi 설정 페이지 + AP SSID 생성)
// --------------------------------------------------------
void runAPPortal() {
  Serial.println("[AP] Wi-Fi 설정 초기화 및 AP 모드 진입");

  clearWiFiConfig();
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP);

  String apName = String("ESP32-Setup-") +
                  String((uint32_t)ESP.getEfuseMac(), HEX).substring(2);
  WiFi.softAP(apName.c_str());
  IPAddress ip = WiFi.softAPIP();

  Serial.print("[AP] SSID = ");
  Serial.println(apName);
  Serial.print("[AP] IP   = ");
  Serial.println(ip);

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();

  unsigned long t = 0;
  bool blink = false;

  while (true) {
    server.handleClient();

    if (millis() - t > 800) {
      t = millis();
      blink = !blink;
      digitalWrite(LED_PIN, blink ? LOW : HIGH); // AP 모드: LED 깜빡임
    }

    delay(10);
  }
}
