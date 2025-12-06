// GW_1116.ino  (ESP32 + SX1276 + RHMesh + Web UI + MQTT)
// - Mesh 기능 유지
// - Web UI: 노드 추가/삭제/저장 + RESET(ESP 소프트 리셋)
// - Node 순서 NVS 저장/로드 (Preferences)
// - OFFLINE 노드(데이터 없음)는 Node 번호 RED 표시
// - MQTT: HiveMQ Public Broker 연동
// - WiFi: 기본 SSID/패스워드로 STA 접속, 실패 시 AP 모드(오픈 AP)
// - AP 모드: LED 빠른 Blink, LoRa 폴링/재초기화 정지
// - SOFT RESET 시 LED 빠른 Blink 후 재부팅

#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <RH_RF95.h>
#include <RHMesh.h>
#include <Preferences.h>
#include <PubSubClient.h>

// ===== WiFi(Default) =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===== Board Select (한 가지만 1로 설정) =====
#define BOARD_ESP32C3 0
#define BOARD_ESP32   1

#if (BOARD_ESP32C3 + BOARD_ESP32) != 1
#error "BOARD_ESP32C3 또는 BOARD_ESP32 중 하나만 1로 설정하세요."
#endif

#if BOARD_ESP32C3
// 통신/상태 LED(Active LOW) + AP 모드 전환 버튼a
const int LED_PIN    = 8;  // 통신/상태 LED (Active LOW)
const int AP_KEY_PIN = 9;  // AP 모드 전환 버튼 (내부 풀업, 버튼은 GND로)

// ESP32-C3 핀 맵
static const uint8_t PIN_LORA_SS   = 7;
static const uint8_t PIN_LORA_RST  = 2; 
static const uint8_t PIN_LORA_DIO0 = 3;
// LED는 LED_PIN 사용
static const int PIN_VSPI_SCK  = 4;
static const int PIN_VSPI_MISO = 5;
static const int PIN_VSPI_MOSI = 6;

#elif BOARD_ESP32
// 통신/상태 LED(Active HIGH) + AP 모드 전환 버튼
const int LED_PIN    = 2;  // 통신/상태 LED (Active HIGH), Module(2 Pin)
const int AP_KEY_PIN = 0;  // AP 모드 전환 버튼 (내부 풀업)

// ESP32 핀 맵
static const uint8_t PIN_LORA_SS   = 15;
static const uint8_t PIN_LORA_RST  = 2;
static const uint8_t PIN_LORA_DIO0 = 4;
// LED는 LED_PIN 사용
static const int PIN_VSPI_SCK  = 18;
static const int PIN_VSPI_MISO = 19;
static const int PIN_VSPI_MOSI = 23;
#endif

// ===== MQTT (HiveMQ Public Demo) =====
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// ===== LoRa / RHMesh =====
#define GATEWAY_ADDRESS 1
static const float   RF_FREQ_MHZ   = 915.0;
static const uint8_t TX_POWER_DBM  = 17;
RH_RF95 rf95(PIN_LORA_SS, PIN_LORA_DIO0);
RHMesh  mesh(rf95, GATEWAY_ADDRESS);
WebServer server(80);

// ===== Nodes =====
static const size_t MAX_NODES = 32;
uint8_t nodes[MAX_NODES] = {2,3,4,5};
size_t  nodesCount       = 4;
const uint32_t PER_NODE_GAP_MS = 1200;
size_t pollIdx=0;
unsigned long nextPollAt=0;

struct NodeState {
  uint8_t addr, failStreak;
  uint16_t baseTimeout;
  bool hasData;
  float lastBat, lastTemp;
  int lastRH;
  int lastCO2;
  bool naTemp, naRH, naCO2;
  int lastLED;
  int16_t lastRSSI;
  unsigned long lastOkMs;
};
NodeState stateTbl[MAX_NODES];

// ===== 안정화 관련 =====
static unsigned long lastAnyRadioActivityMs = 0;
static unsigned long lastGoodRxMs = 0;
static int sweepAllFailCount = 0;
static int sweepFailCounter  = 0;

static const unsigned long RADIO_KICK_GAP_MS  = 20000UL;
static const unsigned long MESH_REINIT_GAP_MS = 300000UL;
static const int           ALLFAIL_REINIT_SWEEPS = 3;

// ===== NVS =====
Preferences prefs;       // Node 순서용
Preferences wifiPrefs;   // WiFi 설정용

bool savedOrderActive = false;   // true면 저장된 순서를 사용

// ===== WiFi 설정/AP 모드 플래그 =====
bool g_apMode = false;          // true면 AP 설정 모드
unsigned long g_apLedLast = 0;
bool g_apLedState = false;

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
WiFi 설정 후 저장하면 재부팅되어 정상 모드로 동작합니다.
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

// ===== 유틸 =====
static inline void pumpWeb(){ server.handleClient(); delay(1); }

static inline void waitMs(uint32_t ms){
  uint32_t t=millis()+ms;
  while((int32_t)(t-millis())>0) pumpWeb();
}
// 보드별 LED ON/OFF 헬퍼
#if BOARD_ESP32C3
inline void ledOn()  { digitalWrite(LED_PIN, LOW); }
inline void ledOff() { digitalWrite(LED_PIN, HIGH); }
#elif BOARD_ESP32
inline void ledOn()  { digitalWrite(LED_PIN, HIGH); }
inline void ledOff() { digitalWrite(LED_PIN, LOW); }
#endif


// 통신용 LED Blink (80ms) - LED_PIN 사용
static inline void blinkComm(){
  ledOn();                 // LED ON
  waitMs(80);                  // 80ms 동안 켜두기
  ledOff();                // LED OFF
}

// ★ 소프트 리셋 알림용 빠른 Blink
void blinkResetNotice(uint8_t times = 8){
  for(uint8_t i=0; i<times; ++i){
    ledOn();                // ON
    delay(80);
    ledOff();               // OFF
    delay(80);
  }
}

static inline void loraReset(){
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_RST, LOW);
  waitMs(10);
  digitalWrite(PIN_LORA_RST, HIGH);
  waitMs(10);
}

static inline String ipToString(IPAddress ip){
  return String(ip[0])+"."+String(ip[1])+"."+String(ip[2])+"."+String(ip[3]);
}

// ===== WiFi 설정 NVS 헬퍼 =====
void loadWifiConfig(String& ssid, String& pass){
  wifiPrefs.begin("wificfg", true);
  ssid = wifiPrefs.getString("ssid", WIFI_SSID);
  pass = wifiPrefs.getString("pass", WIFI_PASSWORD);
  wifiPrefs.end();
}

void saveWifiConfig(const String& ssid, const String& pass){
  wifiPrefs.begin("wificfg", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
}

// ===== AP 모드 진입 =====
void startApMode(){
  if(g_apMode) return;
  g_apMode = true;

  Serial.println("[WiFi] Entering AP config mode");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);                 // AP + 스캔 가능
  WiFi.softAP("ESP32GW-SETUP");           // 비번 없는 오픈 AP
  Serial.print("[WiFi] AP SSID: ESP32GW-SETUP  IP: ");
  Serial.println(WiFi.softAPIP());
}

// ===== Node 헬퍼 =====
void initStateAt(size_t idx, uint8_t addr){
  stateTbl[idx] = {addr,0,1500,false,0,0,0,0,true,true,true,0,0,0};
}

int findNodeIndex(uint8_t addr){
  for(size_t i=0;i<nodesCount;i++) if(nodes[i]==addr) return (int)i;
  return -1;
}

bool addNode(uint8_t addr){
  if(nodesCount>=MAX_NODES) return false;
  if(findNodeIndex(addr)>=0) return false;
  nodes[nodesCount]=addr;
  initStateAt(nodesCount, addr);
  nodesCount++;
  return true;
}

bool delNode(uint8_t addr){
  int i=findNodeIndex(addr); if(i<0) return false;
  size_t last = nodesCount-1;
  if((size_t)i != last){
    nodes[i]=nodes[last];
    stateTbl[i]=stateTbl[last];
  }
  nodesCount--;
  if(pollIdx>=nodesCount) pollIdx=0;
  return true;
}

NodeState* getState(uint8_t a){
  for(size_t i=0;i<nodesCount;i++) if(stateTbl[i].addr==a) return &stateTbl[i];
  return nullptr;
}

// ===== NVS: Node 순서 저장/로드 =====
void loadNodeOrder(){
  prefs.begin("nodeorder", true);
  uint8_t cnt = prefs.getUChar("cnt", 0);

  if(cnt > 0 && cnt <= MAX_NODES){
    savedOrderActive = true;
    nodesCount = cnt;
    for(size_t i=0;i<nodesCount;i++){
      char key[4];
      sprintf(key, "n%u", (unsigned)i);   // "n0","n1",...
      uint8_t def = (uint8_t)(2+i);
      nodes[i] = prefs.getUChar(key, def);
    }
    Serial.printf("[NVS] loaded node order, cnt=%u\n", cnt);
  }else{
    savedOrderActive = false;
    nodesCount = 4; // 기본 2,3,4,5
    Serial.println("[NVS] no saved node order, use default");
  }
  prefs.end();
}

void saveNodeOrder(){
  prefs.begin("nodeorder", false);
  prefs.putUChar("cnt", (uint8_t)nodesCount);
  for(size_t i=0;i<nodesCount;i++){
    char key[4];
    sprintf(key, "n%u", (unsigned)i);
    prefs.putUChar(key, nodes[i]);
  }
  prefs.end();
  savedOrderActive = true;
  Serial.println("[NVS] node order saved");
}

// ===== LoRa Drain =====
void drainRx(uint16_t ms=3){
  uint32_t t=millis()+ms;
  while((int32_t)(t-millis())>0){
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    uint8_t from=0;
    if(mesh.recvfromAckTimeout(buf,&len,1,&from)){
      blinkComm();
      lastAnyRadioActivityMs = millis();
    }
    pumpWeb();
  }
}

// ===== 파싱 헬퍼 =====
int findKeyPos(const String& s, const char* key){
  int pos=-1, start=0;
  while(true){
    pos=s.indexOf(key,start);
    if(pos<0) return -1;
    char prev = (pos==0) ? '\0' : s.charAt(pos-1);
    if(pos==0 || prev==',' || prev==' ') return pos;
    start=pos+1;
  }
}

String getFieldAt(const String& s, int keyPos){
  int eq=s.indexOf('=',keyPos); if(eq<0) return "";
  int e1=s.indexOf(',',eq+1), e2=s.indexOf(' ',eq+1);
  int e=(e1<0)?e2:(e2<0?e1:min(e1,e2)); if(e<0) e=s.length();
  return s.substring(eq+1,e);
}

// ===== Mesh send + wait =====
bool sendAndWait(uint8_t to, const char* payload, String& resp,
                 int16_t& rssi, uint16_t timeoutMs)
{
  drainRx(2);
  mesh.sendtoWait((uint8_t*)payload, strlen(payload), to);
  blinkComm();
  lastAnyRadioActivityMs = millis();

  unsigned long ddl = millis() + timeoutMs;
  while((long)(ddl - millis()) > 0){
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    uint8_t from=0;
    if(mesh.recvfromAckTimeout(buf,&len,25,&from) && from==to){
      blinkComm();
      lastAnyRadioActivityMs = millis();
      lastGoodRxMs = millis();
      resp = "";
      resp.reserve(len+1);
      resp.concat((const char*)buf, len);
      rssi = rf95.lastRssi();
      return !resp.isEmpty();
    }
    pumpWeb();
  }
  return false;
}

// ===== MQTT publish =====
void mqttPublishNode(uint8_t addr){
  NodeState* ns = getState(addr);
  if(!ns || !ns->hasData) return;
  if(WiFi.status()!=WL_CONNECTED || !mqtt.connected()) return;

  char topic[40];
  snprintf(topic,sizeof(topic),"lora/node/%d/data",addr);

  String json;
  json.reserve(160);
  json += "{\"node\":"; json += addr;
  json += ",\"bat\":";  json += String(ns->lastBat,1);

  if(ns->naTemp) json += ",\"t\":null";
  else { json += ",\"t\":"; json += String(ns->lastTemp,1); }

  if(ns->naRH) json += ",\"rh\":null";
  else { json += ",\"rh\":"; json += ns->lastRH; }

  if(ns->naCO2) json += ",\"co2\":null";
  else { json += ",\"co2\":"; json += ns->lastCO2; }

  json += ",\"led\":";  json += ns->lastLED;
  json += ",\"rssi\":"; json += ns->lastRSSI;
  json += ",\"status\":\"OK\"}";

  Serial.printf("[MQTT] PUB %s -> %s\n", topic, json.c_str());
  mqtt.publish(topic, json.c_str());
}

void mqttPublishAll(){
  if(WiFi.status()!=WL_CONNECTED || !mqtt.connected()) return;

  String json;
  json.reserve(1024);
  json = "[";

  bool first = true;
  for(size_t i=0;i<nodesCount;i++){
    NodeState& ns = stateTbl[i];
    if(!first) json += ",";
    first = false;

    json += "{\"addr\":"; json += ns.addr;
    if(ns.hasData){
      json += ",\"bat\":"; json += String(ns.lastBat,1);
      if(ns.naTemp) json += ",\"t\":null"; else { json+=",\"t\":"; json+=String(ns.lastTemp,1); }
      if(ns.naRH)   json += ",\"rh\":null"; else { json+=",\"rh\":"; json+=ns.lastRH; }
      if(ns.naCO2)  json += ",\"co2\":null"; else { json+=",\"co2\":"; json+=ns.lastCO2; }
      json += ",\"led\":";  json += ns.lastLED;
      json += ",\"rssi\":"; json += ns.lastRSSI;
      long s = (ns.lastOkMs>0)?((millis()-ns.lastOkMs)/1000):-1;
      json += ",\"last_ok_s\":"; json += s;
      json += ",\"status\":\"OK\"}";
    }else{
      json += ",\"bat\":null,\"t\":null,\"rh\":null,\"co2\":null,\"led\":null,"
              "\"rssi\":null,\"last_ok_s\":-1,\"status\":\"INIT/FAIL\"}";
    }
  }
  json += "]";
  Serial.printf("[MQTT] PUB lora/nodes -> %s\n", json.c_str());
  mqtt.publish("lora/nodes", json.c_str());
}

// ===== 응답 파싱 + 저장 =====
bool parseAndStore(uint8_t addr, const String& resp, int16_t rssi){
  NodeState* ns=getState(addr); if(!ns) return false;
  int bPos=findKeyPos(resp,"BAT=");
  int tPos=findKeyPos(resp,"T=");
  int hPos=findKeyPos(resp,"RH=");
  int cPos=findKeyPos(resp,"CO2=");
  int lPos=findKeyPos(resp,"LED=");
  if(bPos<0||tPos<0||hPos<0||cPos<0||lPos<0) return false;

  String batS=getFieldAt(resp,bPos);
  String tS  =getFieldAt(resp,tPos);
  String rhS =getFieldAt(resp,hPos);
  String coS =getFieldAt(resp,cPos);
  String ledS=getFieldAt(resp,lPos);

  ns->lastBat = batS.toFloat();
  if(tS=="NA"){ ns->naTemp=true;  ns->lastTemp=0; }
  else        { ns->naTemp=false; ns->lastTemp=tS.toFloat(); }

  if(rhS=="NA"){ ns->naRH=true;  ns->lastRH=0; }
  else         { ns->naRH=false; ns->lastRH=rhS.toInt(); }

  if(coS=="NA"){ ns->naCO2=true; ns->lastCO2=0; }
  else         { ns->naCO2=false;ns->lastCO2=coS.toInt(); }

  ns->lastLED = ledS.toInt();
  ns->lastRSSI= rssi;
  ns->lastOkMs= millis();
  ns->hasData = true;
  ns->failStreak=0;

  mqttPublishNode(addr);
  mqttPublishAll();
  return true;
}

// ===== Poll Once =====
bool pollOnce(uint8_t addr){
  NodeState* ns=getState(addr); if(!ns) return false;
  uint16_t t0=ns->baseTimeout + ns->failStreak*220; if(t0>3200) t0=3200;

  drainRx(5);

  String resp; int16_t rssi=0; bool ok=false;
  ok=sendAndWait(addr,"GET",resp,rssi,t0);
  if(!ok){
    waitMs(40);
    ok=sendAndWait(addr,"GET",resp,rssi,(uint16_t)(t0+300));
  }

  if(ok){
    sweepFailCounter = 0;
    if(parseAndStore(addr, resp, rssi)) return true;
  }

  ns->failStreak = ns->failStreak<10 ? ns->failStreak+1 : 10;
  sweepFailCounter++;
  Serial.printf("NODE=%d : COMM_FAIL\n", addr);
  return false;
}

// ===== LED 제어 =====
bool setNodeLed(uint8_t addr, int want){
  NodeState* ns=getState(addr); if(!ns) return false;
  String resp; resp.reserve(40); int16_t rssi=0;
  for(uint8_t a=0;a<3;++a){
    drainRx(5);
    bool ok=sendAndWait(addr, (String("SETL:")+String(want)).c_str(),
                         resp, rssi,
                         (uint16_t)(ns->baseTimeout + ns->failStreak*160));
    if(ok && resp.startsWith("ACK=")){
      int fb=resp.substring(4).toInt();
      ns->hasData=true; ns->lastLED=fb; ns->lastRSSI=rssi; ns->lastOkMs=millis(); ns->failStreak=0;
      mqttPublishNode(addr); mqttPublishAll();
      return (fb==want);
    }
    ok=sendAndWait(addr,"GET",resp,rssi,
                   (uint16_t)(ns->baseTimeout + ns->failStreak*160 + 220));
    if(ok){
      int lPos=findKeyPos(resp,"LED=");
      if(lPos>=0){
        int led=getFieldAt(resp,lPos).toInt();
        ns->hasData=true; ns->lastLED=led; ns->lastRSSI=rssi; ns->lastOkMs=millis();
        mqttPublishNode(addr); mqttPublishAll();
        if(led==want){ ns->failStreak=0; return true; }
      }
    }
    waitMs(90);
  }
  ns->failStreak = ns->failStreak<10 ? ns->failStreak+1 : 10;
  return false;
}

// ===== Serial parser =====
static char serBuf[64]; static size_t serLen=0;

static void printHelp(){
  Serial.println("Commands:");
  Serial.println("  NODE<addr>_LED_ON / NODE<addr>_LED_OFF");
  Serial.println("  GET <addr> , STATUS , HELP");
}

static void handleSerialLine(char* line){
  char* p=line; while(*p==' '||*p=='\t')++p; size_t n=strlen(p);
  while(n>0&&(p[n-1]==' '||p[n-1]=='\t')) p[--n]='\0';
  for(size_t i=0;i<n;i++) if(p[i]>='a'&&p[i]<='z') p[i]-=32; if(!*p) return;

  if(!strncmp(p,"NODE",4)){
    size_t i=4; int addr=0; bool dig=false;
    while(p[i]>='0'&&p[i]<='9'){ dig=true; addr=addr*10+(p[i]-'0'); i++; }
    if(!dig || p[i]!='_'){ Serial.println("Usage: NODE<addr>_LED_ON / NODE<addr>_LED_OFF"); return; }
    const char* suf=p+i; int want=-1;
    if(!strcmp(suf,"_LED_ON")) want=1;
    if(!strcmp(suf,"_LED_OFF")) want=0;
    if(want<0){ Serial.println("Usage: NODE<addr>_LED_ON / NODE<addr>_LED_OFF"); return; }
    bool ok=setNodeLed((uint8_t)addr,want);
    Serial.printf("LED %s: %s\n", want?"ON":"OFF", ok?"OK":"FAIL");
    return;
  }

  char* cmd=strtok(p,(char*)" \t,"); if(!cmd) return;
  if(!strcmp(cmd,"HELP")){ printHelp(); return; }
  if(!strcmp(cmd,"GET")){
    char* a1=strtok(nullptr,(char*)" \t,"); if(!a1){ Serial.println("Usage: GET <addr>"); return; }
    int addr=atoi(a1); (void)pollOnce((uint8_t)addr); return;
  }
  if(!strcmp(cmd,"STATUS")){
    for(size_t i=0;i<nodesCount;i++) (void)pollOnce(nodes[i]);
    return;
  }
  Serial.println("Unknown command. Type HELP.");
}

// ===== Web 핸들러 =====
void handleRoot(){
  // AP 모드일 때는 설정 페이지, 아니면 간단 텍스트
  if(g_apMode){
    server.send_P(200, "text/html", PAGE_INDEX);
  }else{
    server.send(200, "text/plain", "LoRa Mesh Gateway");
  }
}

// 웹에서 RESET 버튼을 누르면 소프트 리셋
void handleReset(){
  server.send(200,"text/plain","OK");   // 응답
  delay(200);
  blinkResetNotice();                   // ★ 리셋 알림용 빠른 Blink
  ESP.restart();                        // 소프트 리셋
}

void handleSaveOrder(){
  saveNodeOrder();
  server.send(200,"application/json","{\"ok\":true}");
}

// /scan : 주변 AP 목록 JSON 반환
void handleScan(){
  int n = WiFi.scanNetworks();
  String json = "[";
  for(int i=0;i<n;i++){
    if(i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// /save : SSID/비번 저장 후 재부팅
void handleSaveWifi(){
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  saveWifiConfig(ssid, pass);
  server.send_P(200, "text/html", PAGE_SAVED);
  delay(2000);
  ESP.restart();
}

// /metrics: 저장된 순서가 있으면 그대로, 없으면 노드번호 정렬
void handleMetrics(){
  String json; json.reserve(1024);
  json="{\"ip\":\""+ipToString(WiFi.localIP())+"\",\"uptime_s\":"+String(millis()/1000)+",\"nodes\":[";

  uint8_t order[MAX_NODES];
  for(size_t i=0;i<nodesCount;i++) order[i]=nodes[i];

  if(!savedOrderActive){
    for(size_t i=0;i<nodesCount;i++){
      for(size_t j=i+1;j<nodesCount;j++){
        if(order[j] < order[i]){
          uint8_t tmp=order[i]; order[i]=order[j]; order[j]=tmp;
        }
      }
    }
  }

  for(size_t idx=0; idx<nodesCount; idx++){
    if(idx) json+=',';
    uint8_t addr = order[idx];
    NodeState* nsPtr = getState(addr);

    json+="{\"addr\":"+String(addr);
    if(nsPtr && nsPtr->hasData){
      NodeState& ns = *nsPtr;
      json+=",\"bat\":"+String(ns.lastBat,1);
      if(ns.naTemp) json+=",\"t\":null"; else json+=",\"t\":"+String(ns.lastTemp,1);
      if(ns.naRH)   json+=",\"rh\":null"; else json+=",\"rh\":"+String(ns.lastRH);
      if(ns.naCO2)  json+=",\"co2\":null"; else json+=",\"co2\":"+String(ns.lastCO2);
      json+=",\"led\":"+String(ns.lastLED)+
            ",\"rssi\":"+String(ns.lastRSSI);
      long s=(ns.lastOkMs>0)?((millis()-ns.lastOkMs)/1000):-1;
      json+=",\"last_ok_s\":"+String(s)+",\"status\":\"OK\"}";
    }else{
      json+=",\"bat\":null,\"t\":null,\"rh\":null,\"co2\":null,"
           "\"led\":null,\"rssi\":null,\"last_ok_s\":-1,"
           "\"status\":\"INIT/FAIL\"}";
    }
  }
  json+="]}";
  server.send(200, "application/json", json);
}

void handleCmd(){
  if(!server.hasArg("node")||!server.hasArg("led")){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"missing params\"}");
    return;
  }
  int addr=server.arg("node").toInt();
  int led =server.arg("led").toInt();
  if(addr<1 || addr>255){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}");
    return;
  }
  bool ok=setNodeLed((uint8_t)addr, led?1:0);
  server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}"));
}

void handleAddNode(){
  if(!server.hasArg("addr")){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"missing addr\"}");
    return;
  }
  int a = server.arg("addr").toInt();
  if(a<1 || a>255){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}");
    return;
  }
  if(findNodeIndex((uint8_t)a)>=0){
    server.send(200,"application/json","{\"ok\":false,\"err\":\"already exists\"}");
    return;
  }
  if(!addNode((uint8_t)a)){
    server.send(200,"application/json","{\"ok\":false,\"err\":\"cannot add\"}");
    return;
  }
  server.send(200,"application/json","{\"ok\":true}");
}

void handleDelNode(){
  if(!server.hasArg("addr")){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"missing addr\"}");
    return;
  }
  int a = server.arg("addr").toInt();
  if(a<1 || a>255){
    server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}");
    return;
  }
  if(!delNode((uint8_t)a)){
    server.send(200,"application/json","{\"ok\":false,\"err\":\"not found\"}");
    return;
  }
  server.send(200,"application/json","{\"ok\":true}");
}

void notFound(){ server.send(404,"text/plain","404"); }

// ===== 안정화 =====
void softRadioKick(){
  rf95.sleep(); waitMs(4);
  rf95.setModeIdle(); waitMs(3);
  rf95.setFrequency(RF_FREQ_MHZ);
  rf95.setTxPower(TX_POWER_DBM, false);
  rf95.setModeRx();
}

bool meshReinit(){
  loraReset();
  SPI.end(); waitMs(2);
  SPI.begin(PIN_VSPI_SCK,PIN_VSPI_MISO,PIN_VSPI_MOSI,PIN_LORA_SS);
  if(!mesh.init()) return false;
  rf95.setFrequency(RF_FREQ_MHZ);
  rf95.setTxPower(TX_POWER_DBM, false);
  mesh.setRetries(3);
  mesh.setTimeout(1200);
  return true;
}

// ===== MQTT =====
void mqttEnsureConnected(){
  if(WiFi.status()!=WL_CONNECTED) return;
  if(mqtt.connected()) return;

  while(!mqtt.connected()){
    Serial.print("[MQTT] Connecting...");
    if(mqtt.connect("ESP32GW")){
      Serial.println("connected");
      mqtt.subscribe("lora/node/+/cmd");
      mqtt.subscribe("lora/gateway/cmd");   // HTML과 연계된 게이트웨이 RESET 명령
    }else{
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" retry in 1s");
      delay(1000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int len){
  String t(topic);

  // 1) 게이트웨이 소프트 리셋 명령 (mqtt.html에서 전송)
  if (t == "lora/gateway/cmd") {
    String msg;
    for (unsigned int i = 0; i < len; ++i) msg += (char)payload[i];
    msg.trim();
    msg.toLowerCase();

    Serial.printf("[MQTT] GW CMD: \"%s\"\n", msg.c_str());
    if (msg == "reset" || msg == "reboot" || msg == "restart") {
      Serial.println("[MQTT] GW RESET via MQTT command");
      delay(200);
      blinkResetNotice();      // ★ MQTT 기반 RESET 시도 전 LED 빠른 Blink
      ESP.restart();
    }
    return;
  }

  // 2) 기존: 노드 LED 제어 명령
  if(t.startsWith("lora/node/") && t.endsWith("/cmd")){
    int node = t.substring(10, t.length()-4).toInt();
    if(len==0) return;
    int val = payload[0]-'0';
    Serial.printf("[MQTT] LED CMD via MQTT: node=%d val=%d\n", node, val);
    setNodeLed((uint8_t)node, val?1:0);
  }
}

void mqttTestOnce(){
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    Serial.println("[MQTT-TEST] WiFi or MQTT not ready");
    return;
  }
  const char* topic   = "lora/node/99/data";
  const char* payload = "{\"node\":99,\"bat\":3.3,\"t\":25.0,\"rh\":50,"
                        "\"co2\":600,\"led\":0,\"rssi\":-42,\"status\":\"OK\"}";
  Serial.printf("[MQTT-TEST] PUB %s -> %s\n", topic, payload);
  bool ok = mqtt.publish(topic, payload);
  Serial.printf("[MQTT-TEST] publish result = %s\n", ok ? "OK" : "FAIL");
}

// ===== setup / loop =====
void setup(){
  Serial.begin(115200);
  pinMode(LED_PIN,OUTPUT);
  ledOff();

  // AP 버튼: 내부 풀업 사용, 버튼은 GND로
  pinMode(AP_KEY_PIN, INPUT_PULLUP);

  // NVS에서 노드 순서 로드 (없으면 기본 2,3,4,5)
  loadNodeOrder();
  for(size_t i=0;i<nodesCount;i++) initStateAt(i, nodes[i]);

  // WiFi 설정 로드 (NVS에 없으면 디폴트 SSID/PASS)
  String cfgSsid, cfgPass;
  loadWifiConfig(cfgSsid, cfgPass);

  // WiFi 초기 모드 설정
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // 부팅 시 AP 키가 눌려 있으면 바로 AP 모드 진입
  bool apBootPressed = (digitalRead(AP_KEY_PIN) == LOW); // PULLUP이므로 LOW = 눌림
  if(apBootPressed){
    Serial.println("[WiFi] AP key pressed at boot -> AP mode");
    startApMode();
  }else{
    // DHCP 자동 접속
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    Serial.print("[WiFi] Connecting");
    unsigned long wifiStart = millis();
    while(WiFi.status()!=WL_CONNECTED && (millis() - wifiStart) < 10000){
      Serial.print(".");
      delay(300);
    }
    Serial.println();

    if(WiFi.status()==WL_CONNECTED){
      Serial.print("[WiFi] Connected: "); Serial.println(WiFi.localIP());
    }else{
      Serial.println("[WiFi] Connect timeout (>10s), switch to AP mode");
      startApMode();
    }
  }

  server.on("/",handleRoot);
  server.on("/metrics",HTTP_GET,handleMetrics);
  server.on("/cmd",HTTP_POST,handleCmd);
  server.on("/nodes/add",HTTP_POST,handleAddNode);
  server.on("/nodes/del",HTTP_POST,handleDelNode);
  server.on("/nodes/save",HTTP_POST,handleSaveOrder);
  server.on("/reset",HTTP_POST,handleReset);   // 웹에서 RESET -> 소프트 리셋

  // AP 포털용 엔드포인트
  server.on("/scan",HTTP_GET,handleScan);
  server.on("/save",HTTP_POST,handleSaveWifi);

  server.onNotFound(notFound);
  server.begin();
  Serial.println("[WEB] Server started");

  loraReset();
  SPI.begin(PIN_VSPI_SCK,PIN_VSPI_MISO,PIN_VSPI_MOSI,PIN_LORA_SS);
  if(!mesh.init()){
    Serial.println("[LoRa] Mesh init FAILED");
    for(;;){
      digitalWrite(LED_PIN,!digitalRead(LED_PIN));
      delay(100);
    }
  }
  rf95.setFrequency(RF_FREQ_MHZ);
  rf95.setTxPower(TX_POWER_DBM, false);
  mesh.setRetries(3);
  mesh.setTimeout(1200);

  randomSeed(esp_random());
  nextPollAt = millis()+600;

  lastAnyRadioActivityMs = millis();
  lastGoodRxMs = millis();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqttEnsureConnected();
  mqttTestOnce();

  Serial.println();
  Serial.println("Commands:\n  NODE<addr>_LED_ON / NODE<addr>_LED_OFF\n  GET <addr> , STATUS , HELP");
}

void loop(){
  pumpWeb();

  // AP 키(3s 롱프레스) 감지
  static bool apKeyPrev = false;
  static unsigned long apKeyDownMs = 0;
  bool apPressed = (digitalRead(AP_KEY_PIN) == LOW); // PULLUP → GND 누르면 LOW

  if(apPressed && !apKeyPrev){
    apKeyDownMs = millis();
  }
  if(!apPressed && apKeyPrev){
    apKeyDownMs = 0;
  }
  if(!g_apMode && apPressed && apKeyDownMs && (millis() - apKeyDownMs) >= 3000){
    Serial.println("[WiFi] AP key long-press -> AP mode");
    startApMode();
  }
  apKeyPrev = apPressed;

  // AP 모드일 때 LED 빠르게 Blink (재부팅까지 유지)
  if(g_apMode){
    if(millis() - g_apLedLast >= 150){   // 약 150ms 주기 토글
      g_apLedLast = millis();
      g_apLedState = !g_apLedState;
      if(g_apLedState) ledOn(); else ledOff();
    }
  }

  if(WiFi.status()==WL_CONNECTED){
    if(!mqtt.connected()) mqttEnsureConnected();
    mqtt.loop();
  }

  if(rf95.available()){ blinkComm(); lastAnyRadioActivityMs = millis(); }

  while(Serial.available()){
    char c=Serial.read();
    if(c=='\r'||c=='\n'){
      if(serLen){
        serBuf[serLen]='\0';
        handleSerialLine(serBuf);
        serLen=0;
      }
    }else if(serLen<sizeof(serBuf)-1){
      serBuf[serLen++]=c;
    }
  }

  // ===== LoRa 폴링/재초기화: AP 모드가 아닐 때만 동작 =====
  if(!g_apMode){
    unsigned long now=millis();
    if(nodesCount>0 && (long)(now-nextPollAt)>=0){
      if(pollIdx==0) sweepFailCounter = 0;
      uint8_t addr=nodes[pollIdx];
      pollOnce(addr);

      pollIdx=(pollIdx+1)%nodesCount;
      if(pollIdx==0){
        if(sweepFailCounter >= (int)nodesCount){
          sweepAllFailCount++;
          Serial.printf("[SWEEP] all-fail: %d\n", sweepAllFailCount);
        }else sweepAllFailCount = 0;
      }
      nextPollAt=now+PER_NODE_GAP_MS;
    }

    if ((millis() - lastAnyRadioActivityMs) > RADIO_KICK_GAP_MS) {
      softRadioKick();
      lastAnyRadioActivityMs = millis();
    }
    if ((millis() - lastGoodRxMs) > MESH_REINIT_GAP_MS ||
        sweepAllFailCount >= ALLFAIL_REINIT_SWEEPS){
      Serial.println("[LoRa] Reinit");
      if(meshReinit()){
        sweepAllFailCount = 0;
        lastGoodRxMs = millis();
        lastAnyRadioActivityMs = lastGoodRxMs;
        blinkComm();
      }
    }
  }
}
