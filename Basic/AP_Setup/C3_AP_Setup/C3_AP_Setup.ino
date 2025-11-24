#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

#define LED_PIN 8
#define AP_KEY 9

#define WIFI_SSID "Backhome"
#define WIFI_PASS "1700note"

#define AP_SSID "ESP32_SETUP"
#define AP_KEY_HOLD_TIME 3000
#define STA_TIMEOUT 10000

Preferences pref;
WebServer server(80);

enum LedMode {
  LED_OFF,
  LED_AP_FAST,
  LED_STA_SLOW
};

LedMode ledMode = LED_OFF;

String ssid_saved, pass_saved;

unsigned long pressStart = 0;
unsigned long lastAPBlink = 0, lastSTABlink = 0, lastStatusPrint = 0;
unsigned long staConnectStart = 0;
unsigned long rebootAt = 0;

bool needReboot = false;
bool apMode = false;
bool wifiConnecting = false;


// ========================================================
// 외부 IP 조회
// ========================================================
String getExternalIP() {
  HTTPClient http;
  http.begin("http://api.ipify.org");
  int r = http.GET();
  if (r == 200) return http.getString();
  return "N/A";
}


// ========================================================
// AP 모드 시작 함수
// ========================================================
void startAPMode() {
  apMode = true;
  wifiConnecting = false;
  ledMode = LED_AP_FAST;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);

  server.begin();

  Serial.println("[AP] Mode Activated");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}


// ========================================================
// LED 제어 (비블로킹)
// ========================================================
void handleLED() {
  unsigned long now = millis();
  static bool state = false;

  switch (ledMode) {

    case LED_AP_FAST:
      if (now - lastAPBlink >= 80) {
        state = !state;
        digitalWrite(LED_PIN, state ? LOW : HIGH);
        lastAPBlink = now;
      }
      break;

    case LED_STA_SLOW:
      {
        unsigned long dt = (now - lastSTABlink) % 3000;  // 3초 주기

        // dt < 500이면 ON, 아니면 OFF
        digitalWrite(LED_PIN, (dt < 50) ? LOW : HIGH);

        break;
      }

    default:
      digitalWrite(LED_PIN, HIGH);
  }
}


// ========================================================
// AP 버튼 처리 (3초 유지 감지)
// ========================================================
void handleAPKey() {
  unsigned long now = millis();

  if (digitalRead(AP_KEY) == LOW) {

    if (pressStart == 0) pressStart = now;

    if (!apMode && (now - pressStart >= AP_KEY_HOLD_TIME)) {
      startAPMode();
    }
  }
  else {
    pressStart = 0;
  }
}


// ========================================================
// STA 접속 시작
// ========================================================
void beginSTA(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  wifiConnecting = true;
  staConnectStart = millis();
  ledMode = LED_OFF;

  Serial.printf("[STA] Connecting → %s\n", ssid.c_str());
}


// ========================================================
// SSID 스캔 API
// ========================================================
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";

  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }

  json += "]";
  server.send(200, "application/json", json);
}


// ========================================================
// HTML 페이지
// ========================================================
const char PAGE_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='ko'><head>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<title>ESP32 WiFi 설정</title>
<style>
body{font-family:-apple-system,'Segoe UI',sans-serif;background:#020617;color:#e5e7eb;padding:24px;margin:0;}
label{margin-top:12px;display:block}
input,select{width:100%;padding:8px;margin-top:4px;border:1px solid #4b5563;border-radius:8px;background:#020617;color:#e5e7eb;}
button{margin-top:16px;padding:10px 16px;border:none;border-radius:999px;background:#2563eb;color:white;font-weight:700;}
body>div{max-width:360px;margin:auto;border:1px solid #1f2937;padding:20px;border-radius:12px;}
</style>
<script>
async function scan(){
  const res = await fetch('/scan');
  const list = await res.json();
  const sel = document.getElementById('ssidList');
  sel.innerHTML = "<option value=''>SSID 선택…</option>";
  list.forEach(ap=>{
    const opt=document.createElement('option');
    opt.value=ap.ssid;
    opt.innerText=ap.ssid+" ("+ap.rssi+" dBm)";
    sel.appendChild(opt);
  });
}
function setSSID(){
  const v=document.getElementById('ssidList').value;
  if(v) document.getElementById('ssid').value=v;
}
window.onload=scan;
</script>
</head>
<body><div>
<h2 style="font-size:1.6rem;font-weight:700;">WiFi 설정</h2>
<label>WiFi 목록 (자동 스캔)</label>
<select id="ssidList" onchange="setSSID()"><option>스캔 중…</option></select>
<form method="POST" action="/save">
<label>SSID<input id="ssid" name="ssid" required></label>
<label>비밀번호<input type="password" name="pass"></label>
<button type="submit">저장</button>
</form>
</div></body></html>
)rawliteral";


const char PAGE_SAVED[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='ko'><head>
<meta charset="UTF-8">
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{display:flex;justify-content:center;align-items:center;height:100vh;margin:0;background:#020617;color:#e5e7eb;font-family:-apple-system,'Segoe UI',sans-serif;text-align:center;}
h2{font-size:1.8rem;font-weight:700;line-height:1.5;}
</style>
</head>
<body><h2>WiFi 설정이 저장되었습니다.<br>2초 후 재부팅합니다…</h2></body></html>
)rawliteral";


// ========================================================
// WebServer 설정
// ========================================================
void setupWeb() {

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=UTF-8", PAGE_INDEX);
  });

  server.on("/scan", HTTP_GET, handleScan);

  server.on("/save", HTTP_POST, []() {
    pref.putString("ssid", server.arg("ssid"));
    pref.putString("pass", server.arg("pass"));

    server.send(200, "text/html; charset=UTF-8", PAGE_SAVED);

    rebootAt = millis() + 2000;
    needReboot = true;
  });
}


// ========================================================
// SETUP
// ========================================================
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(AP_KEY, INPUT);

  digitalWrite(LED_PIN, HIGH);

  pref.begin("wifi", false);
  ssid_saved = pref.getString("ssid", "");
  pass_saved = pref.getString("pass", "");

  setupWeb();

  unsigned long t0 = millis();
  while (millis() - t0 < 600) {
    handleAPKey();
    handleLED();
  }

  if (!apMode) {
    if (ssid_saved.length())
      beginSTA(ssid_saved, pass_saved);
    else
      beginSTA(WIFI_SSID, WIFI_PASS);
  }
}


// ========================================================
// LOOP
// ========================================================
void loop() {

  handleAPKey();
  handleLED();
  server.handleClient();

  if (needReboot && millis() >= rebootAt)
    ESP.restart();

  if (apMode) return;

  int status = WiFi.status();

  // STA 성공
  if (wifiConnecting && status == WL_CONNECTED) {
    wifiConnecting = false;
    ledMode = LED_STA_SLOW;
  }

  // STA 실패 → AP 모드로 자동 전환
  if (wifiConnecting && (millis() - staConnectStart >= STA_TIMEOUT)) {
    wifiConnecting = false;
    startAPMode();
  }

  // STA 상태 출력
  if (!wifiConnecting && status == WL_CONNECTED) {
    if (millis() - lastStatusPrint >= 3000) {

      Serial.println("\n===== WiFi STATUS =====");
      Serial.print("외부 IP : "); Serial.println(getExternalIP());
      Serial.print("내부 IP : "); Serial.println(WiFi.localIP());
      Serial.print("SSID    : "); Serial.println(WiFi.SSID());
      Serial.print("RSSI    : ");
      Serial.print(WiFi.RSSI());
      Serial.println(" [dBm]");
      Serial.println("========================");

      lastStatusPrint = millis();
    }
  }
}
