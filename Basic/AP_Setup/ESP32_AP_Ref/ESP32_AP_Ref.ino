/***************************************************************
 *  ESP32 WiFi AP Portal - Silent Version (No Serial Output)
 *
 *  ✔ 모든 Serial.print 제거 → 완전 무소음 모드
 *  ✔ 모든 ESP32 계열(C3/S3/WROOM) 호환
 *  ✔ LED/AP_KEY 핀은 설정부에서 자유 변경 가능
 *  ✔ AP 포털(SSID 스캔 + 저장 + 재부팅) 포함
 *  ✔ AP 버튼 3초 → AP 모드 자동 진입
 *  ✔ STA 실패 시 AP 자동 복귀
 *
 ***************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

/***************************************************************
 * 📌 PIN 설정(여기만 수정하면 모든 보드 적용 가능)
 ***************************************************************/
#define LED_PIN   2     // Active LOW LED
#define AP_KEY    0     // 버튼(INPUT_PULLUP) — 눌렀을 때 LOW

/***************************************************************/
#define WIFI_SSID "Backhome"
#define WIFI_PASS "1700note"

#define AP_SSID       "ESP32_SETUP"
#define AP_HOLD_TIME  3000
#define STA_TIMEOUT   10000

Preferences pref;
WebServer server(80);

/***************************************************************/
enum LedMode {
  LED_OFF,
  LED_AP_FAST,
  LED_STA_SLOW
};

LedMode ledMode = LED_OFF;

String ssid_saved, pass_saved;

unsigned long pressStart = 0;
unsigned long lastAPBlink = 0;
unsigned long lastSTABlink = 0;
unsigned long staStart = 0;
unsigned long lastStatus = 0;
unsigned long rebootAt = 0;

bool needReboot = false;
bool apMode = false;
bool wifiConnecting = false;


/***************************************************************/
String getExternalIP() {
  HTTPClient http;
  http.begin("http://api.ipify.org");
  if (http.GET() == 200) return http.getString();
  return "N/A";
}


/***************************************************************/
void startAPMode() {
  apMode = true;
  wifiConnecting = false;
  ledMode = LED_AP_FAST;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);

  server.begin();
}


/***************************************************************/
void handleLED() {
  unsigned long now = millis();
  static bool s = false;

  switch (ledMode) {

    case LED_AP_FAST:
      if (now - lastAPBlink >= 80) {
        s = !s;
        digitalWrite(LED_PIN, s ? LOW : HIGH);
        lastAPBlink = now;
      }
      break;

    case LED_STA_SLOW:
      if (now - lastSTABlink >= 3000) {
        s = !s;
        digitalWrite(LED_PIN, s ? LOW : HIGH);
        lastSTABlink = now;
      }
      break;

    default:
      digitalWrite(LED_PIN, HIGH);
  }
}


/***************************************************************/
void handleAPKey() {
  unsigned long now = millis();

  if (digitalRead(AP_KEY) == LOW) {

    if (pressStart == 0) pressStart = now;

    if (!apMode && (now - pressStart >= AP_HOLD_TIME)) {
      startAPMode();
    }
  }
  else {
    pressStart = 0;
  }
}


/***************************************************************/
void beginSTA(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnecting = true;
  staStart = millis();
  ledMode = LED_OFF;
}


/***************************************************************/
void scanAP() {
  int n = WiFi.scanNetworks();
  String json = "[";

  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + WiFi.RSSI(i) + "}";
  }

  json += "]";
  server.send(200, "application/json", json);
}


/***************************************************************
// HTML — WiFi 설정 페이지
***************************************************************/
const char PAGE_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang='ko'><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>ESP32 WiFi 설정</title>
<style>
body{font-family:-apple-system,'Segoe UI',sans-serif;background:#020617;color:#e5e7eb;padding:24px;margin:0;}
label{margin-top:12px;display:block}
input,select{width:100%;padding:8px;margin-top:4px;border:1px solid #4b5563;border-radius:8px;background:#020617;color:#e5e7eb;}
button{margin-top:16px;padding:10px;background:#2563eb;border-radius:999px;color:white;font-weight:700;}
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
<select id='ssidList' onchange='setSSID()'><option>스캔 중…</option></select>
<form method='POST' action='/save'>
<label>SSID<input id='ssid' name='ssid' required></label>
<label>비밀번호<input type='password' name='pass'></label>
<button type='submit'>저장</button>
</form>
</div></body></html>
)rawliteral";


/***************************************************************
 * HTML — 저장 완료 페이지
 ***************************************************************/
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


/***************************************************************/
void setupWeb() {

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=UTF-8", PAGE_INDEX);
  });

  server.on("/scan", HTTP_GET, scanAP);

  server.on("/save", HTTP_POST, []() {

    pref.putString("ssid", server.arg("ssid"));
    pref.putString("pass", server.arg("pass"));

    server.send(200, "text/html; charset=UTF-8", PAGE_SAVED);

    rebootAt = millis() + 2000;
    needReboot = true;
  });
}


/***************************************************************/
void setup() {

  pinMode(LED_PIN, OUTPUT);
  pinMode(AP_KEY, INPUT_PULLUP);
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


/***************************************************************/
void loop() {

  handleAPKey();
  handleLED();
  server.handleClient();

  if (needReboot && millis() >= rebootAt)
    ESP.restart();

  if (apMode) return;

  int st = WiFi.status();

  if (wifiConnecting && st == WL_CONNECTED) {
    wifiConnecting = false;
    ledMode = LED_STA_SLOW;
  }

  if (wifiConnecting && (millis() - staStart >= STA_TIMEOUT)) {
    wifiConnecting = false;
    startAPMode();
  }

  if (!wifiConnecting && st == WL_CONNECTED) {
    if (millis() - lastStatus >= 3000) {
      lastStatus = millis();
    }
  }
}
