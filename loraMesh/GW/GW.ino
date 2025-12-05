// https://chatgpt.com/c/68d36481-0044-8323-b702-5463d8e8c391
// GW.ino (ESP32 + SX1276 + RHMesh) — Final (no alert popups)
// - Web UI: 상단 시계, 모바일 최적화, Status 실패시 빨간 '-', LED 토글 버튼
// - BAT < 3.0V 시 Node 번호 빨간색 표시
// - 실패 시 JS alert 팝업 제거 (console.error만)
// - Serial: NODE<addr>_LED_ON / NODE<addr>_LED_OFF, GET <addr>, STATUS, HELP
// - Mesh 안정화: soft kick / 주기적 reinit

#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// ===== WiFi =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"
// DHCP 사용 시 아래 주석
IPAddress local_IP(192,168,31,200);
IPAddress gateway(192,168,31,1);
IPAddress subnet(255,255,255,0);
IPAddress primaryDNS(8,8,8,8);
IPAddress secondaryDNS(8,8,4,4);

// ===== LoRa / RHMesh =====
#define GATEWAY_ADDRESS 1
static const float   RF_FREQ_MHZ   = 915.0;
static const uint8_t TX_POWER_DBM  = 17;

// ESP32 <-> RFM95 (VSPI)
static const uint8_t PIN_LORA_SS   = 15;
static const uint8_t PIN_LORA_RST  = 2;
static const uint8_t PIN_LORA_DIO0 = 4;
static const int     PIN_LED_COMM  = 27;
static const int PIN_VSPI_SCK=18, PIN_VSPI_MISO=19, PIN_VSPI_MOSI=23;

RH_RF95 rf95(PIN_LORA_SS, PIN_LORA_DIO0);
RHMesh  mesh(rf95, GATEWAY_ADDRESS);
WebServer server(80);

// ===== Nodes =====
static const size_t MAX_NODES = 32;
uint8_t nodes[MAX_NODES] = {2,3,4,5};
size_t  nodesCount       = 4;
const uint32_t PER_NODE_GAP_MS = 1200;
size_t pollIdx=0; unsigned long nextPollAt=0;

// ===== 상태 =====
struct NodeState {
  uint8_t addr, failStreak; uint16_t baseTimeout; bool hasData;
  float lastBat, lastTemp; int lastRH; int lastCO2;
  bool naTemp, naRH, naCO2;
  int lastLED; int16_t lastRSSI; unsigned long lastOkMs;
};
NodeState stateTbl[MAX_NODES];

// ===== 안정화 타이머/임계 =====
static unsigned long lastAnyRadioActivityMs = 0;
static unsigned long lastGoodRxMs = 0;
static int sweepAllFailCount = 0;
static int sweepFailCounter  = 0;
static const unsigned long RADIO_KICK_GAP_MS = 20000UL;
static const unsigned long MESH_REINIT_GAP_MS= 300000UL;
static const int           ALLFAIL_REINIT_SWEEPS = 3;

// ===== 유틸 =====
static inline void pumpWeb(){ server.handleClient(); delay(1); }
static inline void waitMs(uint32_t ms){ uint32_t t=millis()+ms; while((int32_t)(t-millis())>0) pumpWeb(); }
static inline void blinkComm(){ digitalWrite(PIN_LED_COMM,HIGH); waitMs(2); digitalWrite(PIN_LED_COMM,LOW); }
static inline void loraReset(){ pinMode(PIN_LORA_RST,OUTPUT); digitalWrite(PIN_LORA_RST,LOW); waitMs(10); digitalWrite(PIN_LORA_RST,HIGH); waitMs(10); }
static inline String ipToString(IPAddress ip){ return String(ip[0])+"."+String(ip[1])+"."+String(ip[2])+"."+String(ip[3]); }

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
  nodes[nodesCount]=addr; initStateAt(nodesCount, addr); nodesCount++; return true;
}
bool delNode(uint8_t addr){
  int i=findNodeIndex(addr); if(i<0) return false;
  size_t last = nodesCount-1;
  if((size_t)i != last){ uint8_t tn=nodes[last]; nodes[i]=tn; NodeState ts=stateTbl[last]; stateTbl[i]=ts; }
  nodesCount--; if(pollIdx>=nodesCount) pollIdx=0; return true;
}
NodeState* getState(uint8_t a){ for(size_t i=0;i<nodesCount;i++) if(stateTbl[i].addr==a) return &stateTbl[i]; return nullptr; }

void drainRx(uint16_t ms=3){
  uint32_t t=millis()+ms; uint8_t b[RH_MESH_MAX_MESSAGE_LEN], l, f;
  while((int32_t)(t-millis())>0){
    l=sizeof(b);
    if(mesh.recvfromAckTimeout(b,&l,1,&f)) { blinkComm(); lastAnyRadioActivityMs = millis(); }
    pumpWeb();
  }
}

// 파싱 헬퍼
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

bool sendAndWait(uint8_t to, const char* payload, String& resp, int16_t& rssi, uint16_t timeoutMs){
  drainRx(2);
  mesh.sendtoWait((uint8_t*)payload, strlen(payload), to);
  blinkComm(); lastAnyRadioActivityMs = millis();

  unsigned long ddl=millis()+timeoutMs;
  while((long)(ddl-millis())>0){
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN]; uint8_t len=sizeof(buf); uint8_t from=0;
    if(mesh.recvfromAckTimeout(buf,&len,25,&from) && from==to){
      blinkComm(); lastAnyRadioActivityMs = millis(); lastGoodRxMs = millis();
      resp=""; resp.reserve(len+1); resp.concat((const char*)buf,len);
      rssi=rf95.lastRssi(); return !resp.isEmpty();
    }
    pumpWeb();
  }
  return false;
}

bool parseAndStore(uint8_t addr, const String& resp, int16_t rssi){
  NodeState* ns=getState(addr); if(!ns) return false;
  int bPos=findKeyPos(resp,"BAT="), tPos=findKeyPos(resp,"T="), hPos=findKeyPos(resp,"RH="), cPos=findKeyPos(resp,"CO2="), lPos=findKeyPos(resp,"LED=");
  if(bPos<0||tPos<0||hPos<0||cPos<0||lPos<0) return false;

  String batS=getFieldAt(resp,bPos), tS=getFieldAt(resp,tPos), rhS=getFieldAt(resp,hPos), coS=getFieldAt(resp,cPos), ledS=getFieldAt(resp,lPos);

  ns->lastBat = batS.toFloat();
  if(tS=="NA"){ ns->naTemp=true;  ns->lastTemp=0; } else { ns->naTemp=false; ns->lastTemp=tS.toFloat(); }
  if(rhS=="NA"){ ns->naRH=true;   ns->lastRH=0;   } else { ns->naRH=false;  ns->lastRH=rhS.toInt(); }
  if(coS=="NA"){ ns->naCO2=true;  ns->lastCO2=0;  } else { ns->naCO2=false; ns->lastCO2=coS.toInt(); }
  ns->lastLED = ledS.toInt();
  ns->lastRSSI= rssi;
  ns->lastOkMs= millis();
  ns->hasData = true;
  ns->failStreak=0;

  char tbuf[16], rhbuf[16], cbuf[16];
  if(ns->naTemp) strcpy(tbuf,"NA"); else dtostrf(ns->lastTemp,0,1,tbuf);
  if(ns->naRH)   strcpy(rhbuf,"NA"); else snprintf(rhbuf,sizeof(rhbuf),"%d",ns->lastRH);
  if(ns->naCO2)  strcpy(cbuf,"NA");  else snprintf(cbuf,sizeof(cbuf),"%d",ns->lastCO2);

  Serial.printf("NODE=%d : BAT=%.1f,T=%s,RH=%s,CO2=%s,LED=%d,RSSI=%d\n",
                addr, ns->lastBat, tbuf, rhbuf, cbuf, ns->lastLED, ns->lastRSSI);
  return true;
}

bool pollOnce(uint8_t addr){
  NodeState* ns=getState(addr); if(!ns) return false;
  uint16_t t0=ns->baseTimeout + ns->failStreak*220; if(t0>3200) t0=3200;

  drainRx(5);

  String resp; int16_t rssi=0; bool ok=false;
  ok=sendAndWait(addr,"GET",resp,rssi,t0);
  if(!ok){ waitMs(40); ok=sendAndWait(addr,"GET",resp,rssi,(uint16_t)(t0+300)); }

  if(ok){ sweepFailCounter = 0; if(parseAndStore(addr, resp, rssi)) return true; }

  ns->failStreak = ns->failStreak<10 ? ns->failStreak+1 : 10;
  sweepFailCounter++;
  Serial.printf("NODE=%d : COMM_FAIL\n", addr);
  return false;
}

bool setNodeLed(uint8_t addr, int want){
  NodeState* ns=getState(addr); if(!ns) return false;
  String resp; resp.reserve(40); int16_t rssi=0;
  for(uint8_t a=0;a<3;++a){
    drainRx(5);
    bool ok=sendAndWait(addr, (String("SETL:")+String(want)).c_str(), resp, rssi, (uint16_t)(ns->baseTimeout + ns->failStreak*160));
    if(ok && resp.startsWith("ACK=")){
      int fb=resp.substring(4).toInt();
      ns->hasData=true; ns->lastLED=fb; ns->lastRSSI=rssi; ns->lastOkMs=millis(); ns->failStreak=0;
      return (fb==want);
    }
    ok=sendAndWait(addr,"GET",resp,rssi,(uint16_t)(ns->baseTimeout + ns->failStreak*160 + 220));
    if(ok){
      int lPos=findKeyPos(resp,"LED=");
      if(lPos>=0){
        int led=getFieldAt(resp,lPos).toInt();
        ns->hasData=true; ns->lastLED=led; ns->lastRSSI=rssi; ns->lastOkMs=millis();
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
  Serial.println("  NODE<addr>_LED_ON / NODE<addr>_LED_OFF  (e.g., NODE2_LED_ON)");
  Serial.println("  GET <addr> , STATUS , HELP");
}
static void handleSerialLine(char* line){
  char* p=line; while(*p==' '||*p=='\t')++p; size_t n=strlen(p);
  while(n>0&&(p[n-1]==' '||p[n-1]=='\t')) p[--n]='\0';
  for(size_t i=0;i<n;i++) if(p[i]>='a'&&p[i]<='z') p[i]-=32; if(!*p) return;

  if(!strncmp(p,"NODE",4)){
    size_t i=4; int addr=0; bool dig=false; while(p[i]>='0'&&p[i]<='9'){ dig=true; addr=addr*10+(p[i]-'0'); i++; }
    if(!dig || p[i]!='_'){ Serial.println("Usage: NODE<addr>_LED_ON / NODE<addr>_LED_OFF"); return; }
    const char* suf=p+i; int want=-1;
    if(!strcmp(suf,"_LED_ON")) want=1; if(!strcmp(suf,"_LED_OFF")) want=0;
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
    for(size_t i=0;i<nodesCount;i++) (void)pollOnce(nodes[i]); return;
  }
  Serial.println("Unknown command. Type HELP.");
}

// ===== Web UI =====
const char* HTML_INDEX = R"HTML(
<!doctype html><html lang="ko"><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1"/>
<title>LoRa Mesh Monitor</title>
<style>
:root{ --bg:#0b1020; --card:#131a33; --muted:#a8b4d4; --text:#e9eef5; --line:#223; --accent:#2a345e; --accentH:#364178; --okbg:#19381f; --okfg:#7CFF9E; --failbg:#3a1620; --failfg:#ffa3b6; --low:#ff5a6b; }
*{box-sizing:border-box}
body{font-family:system-ui,Segoe UI,Arial,sans-serif; padding:14px; background:var(--bg); color:var(--text); margin:0;}
.header{display:flex; align-items:flex-end; justify-content:space-between; margin:0 0 10px}
h1{font-size:19px; margin:0}
.clock{font-size:11.5px; color:var(--muted); opacity:.9}
.row{display:flex; gap:8px; align-items:center; margin:0 0 10px}
.card{background:var(--card); border-radius:12px; padding:10px; box-shadow:0 2px 10px rgba(0,0,0,.35)}
table{width:100%; border-collapse:collapse; font-size:13px}
th,td{padding:8px 6px; border-bottom:1px solid var(--line)}
th{text-align:left; color:var(--muted); font-weight:600}
td.num, th.num{text-align:center}
.badge{padding:2px 8px; border-radius:12px; font-size:12px; display:inline-block}
.ok{background:var(--okbg); color:var(--okfg)}
.fail{background:var(--failbg); color:var(--failfg)}
.lowbat{color:var(--low); font-weight:700;} /* BAT<3.0V이면 Node 번호 빨간색 */
.btn{padding:6px 10px; border-radius:8px; border:none; cursor:pointer; background:var(--accent); color:var(--text); font-size:12.5px}
.btn:hover{background:var(--accentH)}
.small{color:var(--muted); font-size:11.5px}
input[type=number]{width:100px; padding:6px 8px; border-radius:8px; border:1px solid var(--line); background:#0e1630; color:var(--text); font-size:12.5px}
@media (max-width:420px){
  body{padding:10px}
  .header{margin-bottom:8px}
  h1{font-size:18px}
  .clock{font-size:11px}
  .row{gap:6px; margin-bottom:8px}
  .card{padding:10px; border-radius:10px}
  table{font-size:12px}
  th,td{padding:7px 5px}
  .btn{padding:6px 8px; font-size:12px}
  input[type=number]{width:82px; font-size:12px}
  th:nth-child(10), td:nth-child(10){white-space:nowrap}
}
</style>
</head><body>

<div class="header">
  <h1>LoRa Mesh Monitor</h1>
  <div id="clock" class="clock">--</div>
</div>

<div class="card">
  <div class="row">
    <div class="small" id="wifi" style="flex:1"></div>
    <input id="addr" type="number" min="1" max="255" placeholder="addr"/>
    <button class="btn" onclick="addNode()">추가</button>
    <button class="btn" onclick="delNode()">삭제</button>
  </div>
  <table>
    <thead>
      <tr>
        <th class="num">Node</th><th class="num">BAT (V)</th><th class="num">T (°C)</th>
        <th class="num">RH (%)</th><th class="num">CO2 (ppm)</th><th class="num">LED</th>
        <th class="num">RSSI</th><th class="num">Last OK (s)</th><th>Status</th><th>Control</th><th>노드</th>
      </tr>
    </thead>
    <tbody id="tbody"></tbody>
  </table>
</div>

<script>
function updateClock(){
  const now = new Date();
  const pad = n => n.toString().padStart(2,'0');
  const y = now.getFullYear();
  const m = pad(now.getMonth()+1);
  const d = pad(now.getDate());
  const hh = pad(now.getHours());
  const mm = pad(now.getMinutes());
  const ss = pad(now.getSeconds());
  document.getElementById('clock').textContent = `${y}-${m}-${d} ${hh}:${mm}:${ss}`;
}

async function fetchMetrics(){
  try{
    const r = await fetch('/metrics',{cache:'no-store'});
    const j = await r.json();
    document.getElementById('wifi').textContent =
      'ESP32 IP: '+ j.ip +' | Uptime: '+ j.uptime_s +'s | Nodes: '+ (j.nodes?.length ?? 0);
    const tb = document.getElementById('tbody'); tb.innerHTML='';
    j.nodes.forEach(n=>{
      const tr = document.createElement('tr');
      const statusOk = n.status==='OK';
      const statusCell = statusOk ? '<span class="badge ok">OK</span>' : '<span class="badge fail">-</span>';
      const tTxt  = (n.t===null)? 'NA' : n.t.toFixed(1);
      const rhTxt = (n.rh===null)? 'NA' : n.rh.toString();
      const cTxt  = (n.co2===null)? 'NA' : n.co2.toString();
      const ledTxt = (n.led===0||n.led===1)? n.led : '-';
      const toggleBtn = `<button class="btn" onclick="toggleLed(${n.addr},${(n.led===0||n.led===1)?n.led:'null'})">ON/OFF</button>`;
      const low = (n.bat!==null && typeof n.bat==='number' && n.bat < 3.0);
      const nodeCell = `<td class="num ${low?'lowbat':''}">${n.addr}</td>`;

      tr.innerHTML = `
        ${nodeCell}
        <td class="num">${(n.bat??'-')}</td>
        <td class="num">${tTxt}</td>
        <td class="num">${rhTxt}</td>
        <td class="num">${cTxt}</td>
        <td class="num">${ledTxt}</td>
        <td class="num">${(n.rssi??'-')}</td>
        <td class="num">${(n.last_ok_s??'-')}</td>
        <td>${statusCell}</td>
        <td>${toggleBtn}</td>
        <td><button class="btn" onclick="delNode(${n.addr})">삭제</button></td>`;
      tb.appendChild(tr);
    });
  }catch(e){ console.error(e); }
}

async function toggleLed(addr,current){
  const want = (current===0)?1:((current===1)?0:1);
  try{
    const r=await fetch('/cmd?node='+addr+'&led='+want,{method:'POST'});
    const j=await r.json();
    if(!j.ok) console.error('LED toggle failed for', addr);
    fetchMetrics();
  }catch(e){ console.error('toggleLed request error', e); }
}
async function addNode(){
  const a = Number(document.getElementById('addr').value);
  if(!a || a<1 || a>255){ console.error('bad addr'); return; }
  try{
    const r = await fetch('/nodes/add?addr='+a,{method:'POST'});
    const j = await r.json();
    if(!j.ok) console.error('addNode failed', j.err || '');
    fetchMetrics();
  }catch(e){ console.error('addNode request error', e); }
}
async function delNode(addr){
  const a = (typeof addr==='number') ? addr : Number(document.getElementById('addr').value);
  if(!a || a<1 || a>255){ console.error('bad addr'); return; }
  try{
    const r = await fetch('/nodes/del?addr='+a,{method:'POST'});
    const j = await r.json();
    if(!j.ok) console.error('delNode failed', j.err || '');
    fetchMetrics();
  }catch(e){ console.error('delNode request error', e); }
}
setInterval(fetchMetrics,1500); fetchMetrics();
setInterval(updateClock,1000); updateClock();
</script></body></html>
)HTML";

void sendJsonOkWithNodeList(){
  String json; json.reserve(256);
  json = "{\"ok\":true,\"list\":[";
  for(size_t i=0;i<nodesCount;i++){ if(i) json+=','; json += String(nodes[i]); }
  json += "]}";
  server.send(200,"application/json",json);
}
void handleRoot(){ server.send(200, "text/html; charset=utf-8", HTML_INDEX); }

void handleMetrics(){
  String json; json.reserve(1024);
  json="{\"ip\":\""+ipToString(WiFi.localIP())+"\",\"uptime_s\":"+String(millis()/1000)+",\"nodes\":[";
  for(size_t i=0;i<nodesCount;i++){
    if(i) json+=',';
    NodeState& ns=stateTbl[i];
    if(ns.hasData){
      long s=(ns.lastOkMs>0)?((millis()-ns.lastOkMs)/1000):-1;
      json+="{\"addr\":"+String(ns.addr)+
            ",\"bat\":"+String(ns.lastBat,1);
      if(ns.naTemp) json+=",\"t\":null"; else json+=",\"t\":"+String(ns.lastTemp,1);
      if(ns.naRH)   json+=",\"rh\":null"; else json+=",\"rh\":"+String(ns.lastRH);
      if(ns.naCO2)  json+=",\"co2\":null"; else json+=",\"co2\":"+String(ns.lastCO2);
      json+=",\"led\":"+String(ns.lastLED)+
            ",\"rssi\":"+String(ns.lastRSSI)+
            ",\"last_ok_s\":"+String(s)+
            ",\"status\":\"OK\"}";
    }else{
      json+="{\"addr\":"+String(ns.addr)+",\"bat\":null,\"t\":null,\"rh\":null,\"co2\":null,\"led\":null,\"rssi\":null,\"last_ok_s\":-1,\"status\":\"INIT/FAIL\"}";
    }
  }
  json+="]}";
  server.send(200, "application/json", json);
}

void handleCmd(){
  if(!server.hasArg("node")||!server.hasArg("led")){ server.send(400,"application/json","{\"ok\":false,\"err\":\"missing params\"}"); return; }
  int addr=server.arg("node").toInt(); int led=server.arg("led").toInt();
  if(addr<1 || addr>255){ server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}"); return; }
  bool ok=setNodeLed((uint8_t)addr, led?1:0);
  server.send(200,"application/json",String("{\"ok\":")+(ok?"true}":"false}"));
}
void handleAddNode(){
  if(!server.hasArg("addr")){ server.send(400,"application/json","{\"ok\":false,\"err\":\"missing addr\"}"); return; }
  int a = server.arg("addr").toInt();
  if(a<1 || a>255){ server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}"); return; }
  if(findNodeIndex((uint8_t)a)>=0){ server.send(200,"application/json","{\"ok\":false,\"err\":\"already exists\"}"); return; }
  if(!addNode((uint8_t)a)){ server.send(200,"application/json","{\"ok\":false,\"err\":\"cannot add\"}"); return; }
  sendJsonOkWithNodeList();
}
void handleDelNode(){
  if(!server.hasArg("addr")){ server.send(400,"application/json","{\"ok\":false,\"err\":\"missing addr\"}"); return; }
  int a = server.arg("addr").toInt();
  if(a<1 || a>255){ server.send(400,"application/json","{\"ok\":false,\"err\":\"bad addr\"}"); return; }
  if(!delNode((uint8_t)a)){ server.send(200,"application/json","{\"ok\":false,\"err\":\"not found\"}"); return; }
  sendJsonOkWithNodeList();
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

// ===== Setup / Loop =====
void setup(){
  Serial.begin(115200);
  pinMode(PIN_LED_COMM,OUTPUT); digitalWrite(PIN_LED_COMM,LOW);
  for(size_t i=0;i<nodesCount;i++) initStateAt(i, nodes[i]);

  if (!WiFi.config(local_IP,gateway,subnet,primaryDNS,secondaryDNS)) {
    Serial.println("[WiFi] STA Failed to configure");
  }
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while(WiFi.status()!=WL_CONNECTED){ Serial.print("."); delay(300); }
  Serial.println(); Serial.print("[WiFi] Connected: "); Serial.println(WiFi.localIP());

  server.on("/",handleRoot);
  server.on("/metrics",HTTP_GET,handleMetrics);
  server.on("/cmd",HTTP_POST,handleCmd);
  server.on("/nodes/add",HTTP_POST,handleAddNode);
  server.on("/nodes/del",HTTP_POST,handleDelNode);
  server.onNotFound(notFound);
  server.begin();
  Serial.println("[WEB] Server started");

  loraReset();
  SPI.begin(PIN_VSPI_SCK,PIN_VSPI_MISO,PIN_VSPI_MOSI,PIN_LORA_SS);
  if(!mesh.init()){ for(;;){ digitalWrite(PIN_LED_COMM,!digitalRead(PIN_LED_COMM)); delay(100);} }
  rf95.setFrequency(RF_FREQ_MHZ);
  rf95.setTxPower(TX_POWER_DBM, false);
  mesh.setRetries(3); mesh.setTimeout(1200);

  randomSeed(esp_random());
  nextPollAt = millis()+600;

  lastAnyRadioActivityMs = millis();
  lastGoodRxMs = millis();

  Serial.println();
  Serial.println("Commands:\n  NODE<addr>_LED_ON / NODE<addr>_LED_OFF\n  GET <addr> , STATUS , HELP");
}

void loop(){
  pumpWeb();
  if(rf95.available()){ blinkComm(); lastAnyRadioActivityMs = millis(); }

  // 간단한 시리얼 라인 처리
  static char sbuf[64]; static size_t slen=0;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\r'||c=='\n'){ if(slen){ sbuf[slen]='\0'; handleSerialLine(sbuf); slen=0; } }
    else if(slen<sizeof(sbuf)-1) sbuf[slen++]=c;
  }

  unsigned long now=millis();
  if(nodesCount>0 && (long)(now-nextPollAt)>=0){
    if(pollIdx==0) sweepFailCounter = 0;
    uint8_t addr=nodes[pollIdx];
    (void)pollOnce(addr);

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
  if ((millis() - lastGoodRxMs) > MESH_REINIT_GAP_MS || sweepAllFailCount >= ALLFAIL_REINIT_SWEEPS){
    Serial.println("[LoRa] Reinit");
    if(meshReinit()){
      sweepAllFailCount = 0;
      lastGoodRxMs = millis();
      lastAnyRadioActivityMs = lastGoodRxMs;
      blinkComm();
    }
  }
}
