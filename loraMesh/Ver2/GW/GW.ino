// ====================== GATEWAY ======================
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// ===== Board Select (한 가지만 1로 설정) =====
#define BOARD_ESP32C3 1
#define BOARD_ESP32   0

#if (BOARD_ESP32C3 + BOARD_ESP32) != 1
#error "BOARD_ESP32C3 또는 BOARD_ESP32 중 하나만 1로 설정하세요."
#endif

#if BOARD_ESP32C3
const int LED_PIN    = 10;
static const uint8_t PIN_LORA_SS   = 7;
static const uint8_t PIN_LORA_RST  = 2;
static const uint8_t PIN_LORA_DIO0 = 3;
static const int PIN_VSPI_SCK  = 4;
static const int PIN_VSPI_MISO = 5;
static const int PIN_VSPI_MOSI = 6;
#elif BOARD_ESP32
const int LED_PIN    = 15;
static const uint8_t PIN_LORA_SS   = 5;
static const uint8_t PIN_LORA_RST  = 17;
static const uint8_t PIN_LORA_DIO0 = 16;
static const int PIN_VSPI_SCK  = 18;
static const int PIN_VSPI_MISO = 19;
static const int PIN_VSPI_MOSI = 23;
#endif

// ===== WiFi(Default) =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===== MQTT =====
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// ===== MQTT Topics =====
static const char* TOPIC_LED_SUB   = "tswell/lora/node/+/led";
static const char* TOPIC_TELEM_FMT = "tswell/lora/node/%u/telemetry";
static const char* TOPIC_GW_CMD    = "tswell/lora/gateway/cmd";

// ✅ (추가) WiFi RSSI publish 토픽
static const char* TOPIC_WIFI_RSSI = "tswell/lora/gateway/wifi_rssi";

// ===== Addresses =====
#define GW_ADDR         1
#define BROADCAST_ADDR  255

// ✅ 테스트용: Node 2,3,4 + Router 50
static const uint8_t NODES[] = {2,3,4,50};
static const uint8_t NCOUNT = sizeof(NODES)/sizeof(NODES[0]);

#define ROUTER_ID       50

// ===== Timing =====
#define SLOT_MS         10000UL
#define PERIOD_MS       (SLOT_MS * NCOUNT)

#define BEACON_REPEAT   3
#define BEACON_GAP_MS   200

#define RX_WAIT_MS 2000
#define POLL_BACKOFF_MS 160

#define NODE_OK_TIMEOUT_MS    25000UL
#define ROUTER_OK_TIMEOUT_MS  120000UL

// ===== LED 안정화 =====
struct LedPending {
  bool active = false;
  uint8_t target = 0;
  uint32_t lastTryMs = 0;
  uint8_t retries = 0;
};
LedPending ledPend[256];

const uint8_t  LED_MAX_RETRY      = 6;
const uint32_t LED_RETRY_GAP_MS   = 900;

// ===== SX1276 =====
#define LORA_MOSI PIN_VSPI_MOSI
#define LORA_MISO PIN_VSPI_MISO
#define LORA_SCK  PIN_VSPI_SCK
#define LORA_NSS  PIN_LORA_SS
#define LORA_RST  PIN_LORA_RST
#define LORA_DIO0 PIN_LORA_DIO0
#define LED_COMM  LED_PIN

#define LORA_FREQ 920.0
#define LORA_TX_POWER 13

RH_RF95 rf95(LORA_NSS, LORA_DIO0);
RHMesh  manager(rf95, GW_ADDR);

// ===== Protocol =====
enum : uint8_t {
  PKT_POLL    = 0x01,
  PKT_LED_SET = 0x02,
  PKT_TELEM   = 0x81,
  PKT_LED_ACK = 0x82,
  PKT_BEACON  = 0xB1
};

#pragma pack(push, 1)
struct TelemetryPayload {
  uint16_t vbat_mV;
  int16_t t;
  int16_t h;
  int16_t vib;
  uint8_t led_state;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct BeaconPayload { uint16_t cycleId; };
#pragma pack(pop)

struct NodeState{
  bool ok=false;
  uint32_t lastOkMs=0;
  int16_t rssi=-999;
  float vbat=0;
  int16_t t=0,h=0,vib=0;
  uint8_t led=0;
};

NodeState stNode[NCOUNT];

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ------------------------------------------------------------
// ✅ ESP32 WDT 방지: 아주 짧게 task 양보
static inline void wdtYield(){
  delay(1);
}

// ------------------------------------------------------------
static void blinkComm(uint8_t n=1){
  for(uint8_t i=0;i<n;i++){
    digitalWrite(LED_COMM,HIGH); delay(25);
    digitalWrite(LED_COMM,LOW);  delay(25);
  }
}

static int idxOf(uint8_t id){
  for(int i=0;i<NCOUNT;i++) if(NODES[i]==id) return i;
  return -1;
}
static inline bool isRouterId(uint8_t id){
  return (id == ROUTER_ID);
}

static void ensureWifi(){
  if(WiFi.status()==WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  for(int i=0;i<30 && WiFi.status()!=WL_CONNECTED;i++){
    delay(300);
    wdtYield();
    Serial.print(".");
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.print("WiFi OK: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi FAIL (continue without MQTT)");
  }
}

static void mqttCb(char* topic, byte* payload, unsigned int length){
  String tpc(topic), msg;
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();

  if(tpc == TOPIC_GW_CMD){
    Serial.printf("[GW CMD] %s\n", msg.c_str());
    if(msg == "reset"){
      Serial.println("[GW CMD] ESP.restart()");
      delay(50);
      ESP.restart();
    }
    return;
  }

  int p1=tpc.indexOf("/node/"), p2=tpc.indexOf("/led");
  if(p1<0||p2<0) return;

  uint8_t nodeId=(uint8_t)tpc.substring(p1+6,p2).toInt();
  if(idxOf(nodeId)<0) return;

  uint8_t stt = (msg.toInt()!=0)?1:0;

  ledPend[nodeId].active    = true;
  ledPend[nodeId].target    = stt;
  ledPend[nodeId].lastTryMs = 0;
  ledPend[nodeId].retries   = 0;

  Serial.printf("[LED QUEUE] node %u target=%u\n", nodeId, stt);
}

static void ensureMqtt(){
  if(WiFi.status()!=WL_CONNECTED) return;
  if(mqtt.connected()) return;

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCb);
  String cid="tswell-gw-"+String((uint32_t)ESP.getEfuseMac(),HEX);

  if(mqtt.connect(cid.c_str())){
    Serial.println("MQTT connected");
    mqtt.subscribe(TOPIC_LED_SUB);
    mqtt.subscribe(TOPIC_GW_CMD);
    Serial.println("MQTT subscribed: node led + gateway cmd");
  }
}

// ✅ (추가) WiFi RSSI publish
static void publishWifiRssi(){
  if(!mqtt.connected()) return;
  if(WiFi.status()!=WL_CONNECTED) return;

  int rssi = WiFi.RSSI(); // 예: -62
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", rssi);

  // retain=true → 웹 새로고침해도 바로 값 표시
  mqtt.publish(TOPIC_WIFI_RSSI, buf, true);
}

// publish telemetry
static void publishTele(uint8_t nodeId, const NodeState& s){
  if(!mqtt.connected()) return;
  char topic[64];
  snprintf(topic,sizeof(topic),TOPIC_TELEM_FMT,nodeId);
  char payload[220];
  snprintf(payload,sizeof(payload),
    "{\"node\":%u,\"vbat\":%.3f,\"t\":%d,\"h\":%d,\"vib\":%d,\"led\":%u,\"rssi\":%d}",
    nodeId, s.vbat, s.t, s.h, s.vib, s.led, s.rssi);
  mqtt.publish(topic,payload);
}

static void printLineNode(uint8_t nodeId, const NodeState& s){
  uint32_t ageSec = s.ok ? (millis()-s.lastOkMs)/1000 : 0;
  const char* label = isRouterId(nodeId) ? "Router" : "Node  ";

  Serial.printf("%s %3u ->  %5.2f | %3d | %3d | %3d |  %u  | %4d | %4lu | %s\n",
    label, nodeId, s.vbat, s.t, s.h, s.vib, s.led, s.rssi,
    (unsigned long)ageSec, s.ok?"YES":"NO");
}

static bool recvTele(uint32_t waitMs){
  uint32_t t0=millis();
  bool gotAny=false;

  while(millis()-t0<waitMs){
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
    uint8_t len=sizeof(buf);
    uint8_t from;

    if(manager.recvfromAck(buf,&len,&from)){
      blinkComm(1);
      int16_t rssi=rf95.lastRssi();

      if(len==1+sizeof(TelemetryPayload) && buf[0]==PKT_TELEM){
        TelemetryPayload p; memcpy(&p,buf+1,sizeof(p));

        int i=idxOf(from);
        if(i>=0){
          stNode[i].ok=true;
          stNode[i].lastOkMs=millis();
          stNode[i].rssi=rssi;
          stNode[i].vbat=p.vbat_mV/1000.0f;
          stNode[i].t=p.t; stNode[i].h=p.h; stNode[i].vib=p.vib;
          stNode[i].led=p.led_state;
          publishTele(from, stNode[i]);
          gotAny=true;
        }
      }

      if(len>=2 && buf[0]==PKT_LED_ACK){
        uint8_t ackState = buf[1] ? 1 : 0;

        int i = idxOf(from);
        if(i>=0){
          stNode[i].ok = true;
          stNode[i].lastOkMs = millis();
          stNode[i].rssi = rssi;
          stNode[i].led  = ackState;

          if(ledPend[from].active && ledPend[from].target == ackState){
            Serial.printf("[LED ACK] node %u ack=%u -> DONE\n", from, ackState);
            ledPend[from].active = false;
          }else{
            Serial.printf("[LED ACK] node %u ack=%u\n", from, ackState);
          }

          publishTele(from, stNode[i]);
        }
        gotAny = true;
      }
    }

    mqtt.loop();
    wdtYield();
    delay(2);
  }
  return gotAny;
}

static void processLedPending(){
  uint32_t now = millis();

  for(uint8_t i=0;i<NCOUNT;i++){
    uint8_t nodeId = NODES[i];
    if(!ledPend[nodeId].active) continue;

    if(stNode[i].ok && stNode[i].led == ledPend[nodeId].target){
      Serial.printf("[LED DONE] node %u applied=%u\n", nodeId, stNode[i].led);
      ledPend[nodeId].active = false;
      continue;
    }

    if(ledPend[nodeId].lastTryMs != 0 && (now - ledPend[nodeId].lastTryMs < LED_RETRY_GAP_MS)) continue;

    if(ledPend[nodeId].retries >= LED_MAX_RETRY){
      Serial.printf("[LED FAIL] node %u timeout target=%u\n", nodeId, ledPend[nodeId].target);
      ledPend[nodeId].active = false;
      continue;
    }

    ledPend[nodeId].lastTryMs = now;

    uint8_t out[2] = {PKT_LED_SET, ledPend[nodeId].target};
    rf95.waitCAD();
    wdtYield();
    uint8_t err = manager.sendtoWait(out, sizeof(out), nodeId);
    wdtYield();

    ledPend[nodeId].retries++;

    Serial.printf("[LED TRY] node %u -> %u (try=%u err=%u)\n",
                  nodeId, ledPend[nodeId].target, ledPend[nodeId].retries, err);

    recvTele(180);
  }
}

static void sendBeacon(uint16_t cycleId){
  uint8_t out[1+sizeof(BeaconPayload)];
  out[0]=PKT_BEACON;
  BeaconPayload b{cycleId};
  memcpy(out+1,&b,sizeof(b));

  for(int i=0;i<BEACON_REPEAT;i++){
    rf95.waitCAD();
    wdtYield();
    rf95.send(out,sizeof(out));
    rf95.waitPacketSent();
    wdtYield();
    delay(BEACON_GAP_MS);
    mqtt.loop();
  }
}

static bool pollInSlot(uint8_t nodeId, uint32_t slotDeadline){
  uint8_t req[1]={PKT_POLL};
  bool got=false;

  while(millis()<slotDeadline){
    rf95.waitCAD();
    wdtYield();
    uint8_t err=manager.sendtoWait(req,sizeof(req),nodeId);
    wdtYield();
    mqtt.loop();

    processLedPending();

    if(err==RH_ROUTER_ERROR_NONE){
      if(recvTele(RX_WAIT_MS)){ got=true; break; }
    }

    uint32_t t0=millis();
    while(millis()-t0<POLL_BACKOFF_MS){
      mqtt.loop();
      recvTele(25);
      processLedPending();
      if(millis()>=slotDeadline) break;
      wdtYield();
      delay(2);
    }
  }

  int i=idxOf(nodeId);
  if(i>=0 && !got){
    stNode[i].ok=false;
    stNode[i].rssi=-999;
  }
  return got;
}

static void applyTimeouts(){
  uint32_t now = millis();
  for(int i=0;i<NCOUNT;i++){
    uint8_t id = NODES[i];
    uint32_t tout = isRouterId(id) ? ROUTER_OK_TIMEOUT_MS : NODE_OK_TIMEOUT_MS;

    if(stNode[i].ok && (now - stNode[i].lastOkMs > tout)){
      stNode[i].ok=false;
      stNode[i].rssi=-999;
    }
  }
}

void setup(){
  pinMode(LED_COMM, OUTPUT);
  digitalWrite(LED_COMM, LOW);

  Serial.begin(115200);
  delay(200);

  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, HIGH);
  digitalWrite(LORA_RST, LOW); delay(20);
  digitalWrite(LORA_RST, HIGH); delay(50);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  if(!manager.init()){
    Serial.println("RHMesh init failed");
    while(1){ blinkComm(2); delay(200); }
  }

  rf95.setFrequency(LORA_FREQ);
  rf95.setTxPower(LORA_TX_POWER,false);

  ensureWifi();
  ensureMqtt();

  Serial.println("Gateway: WiFi RSSI publish enabled");
}

void loop(){
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  recvTele(20);
  processLedPending();

  // ✅ (추가) 3초마다 WiFi RSSI publish
  static uint32_t lastRssiPubMs = 0;
  if(millis() - lastRssiPubMs >= 3000){
    lastRssiPubMs = millis();
    publishWifiRssi();
  }

  static uint16_t cycleId=0;
  cycleId++;
  uint32_t cycleStart=millis();

  sendBeacon(cycleId);

  for(uint8_t i=0;i<NCOUNT;i++){
    uint8_t nodeId=NODES[i];
    uint32_t slotStart = cycleStart + SLOT_MS*i;
    uint32_t slotEnd   = slotStart + SLOT_MS;

    while(millis()<slotStart){
      mqtt.loop();
      recvTele(20);
      processLedPending();

      // ✅ (추가) 대기 중에도 RSSI publish 유지
      if(millis() - lastRssiPubMs >= 3000){
        lastRssiPubMs = millis();
        publishWifiRssi();
      }

      wdtYield();
      delay(2);
    }

    pollInSlot(nodeId, slotEnd);

    applyTimeouts();
    printLineNode(nodeId, stNode[i]);
  }

  applyTimeouts();

  while(millis() < cycleStart + PERIOD_MS){
    mqtt.loop();
    recvTele(25);
    processLedPending();
    applyTimeouts();

    // ✅ (추가) 사이클 대기 중에도 RSSI publish 유지
    if(millis() - lastRssiPubMs >= 3000){
      lastRssiPubMs = millis();
      publishWifiRssi();
    }

    wdtYield();
    delay(5);
  }
}
