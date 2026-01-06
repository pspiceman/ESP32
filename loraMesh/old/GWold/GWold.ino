// ====================== GATEWAY (NO-RF-TUNING + LED-HOLD + JITTER BURST + SLOT FIXED 10s) ======================
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// ===== Board Select =====
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

// ===== WiFi =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===== MQTT =====
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

static const char* TOPIC_LED_SUB      = "tswell/lora/node/+/led";
static const char* TOPIC_TELEM_FMT    = "tswell/lora/node/%u/telemetry";
static const char* TOPIC_LEDSTATE_FMT = "tswell/lora/node/%u/led_state";
static const char* TOPIC_GW_CMD       = "tswell/lora/gateway/cmd";
static const char* TOPIC_WIFI_RSSI    = "tswell/lora/gateway/wifi_rssi";

// ===== Mesh / Nodes =====
#define GW_ADDR 1
static const uint8_t NODES[] = {2,3,4,50};
static const uint8_t NCOUNT  = sizeof(NODES)/sizeof(NODES[0]);
#define ROUTER_ID 50

// ===== Timing =====
#define SLOT_MS         10000UL
#define RX_WAIT_MS      2200   // ✅ 약간 증가 (노드 Confirm Telemetry 수신 확률↑)
#define RX_WAIT_LED_MS  3500   // ✅ LED pending시 RX 대기 더 길게
#define POLL_BACKOFF_MS 160

// ===== Beacon =====
#define BEACON_REPEAT   3
#define BEACON_GAP_MS   160

// ===== LED pending =====
struct LedPending{
  bool active=false;
  uint8_t target=0;
  uint32_t queuedMs=0;
};
LedPending ledPend[256];

// ===== FAST =====
static volatile bool fastCycleReq=false;
static volatile uint8_t fastNodeId=0;
static volatile bool pollAbortReq=false;

// ===== LoRa pins =====
#define LORA_MOSI PIN_VSPI_MOSI
#define LORA_MISO PIN_VSPI_MISO
#define LORA_SCK  PIN_VSPI_SCK
#define LORA_NSS  PIN_LORA_SS
#define LORA_RST  PIN_LORA_RST
#define LORA_DIO0 PIN_LORA_DIO0
#define LED_COMM  LED_PIN

#define LORA_FREQ 920.0
#define USE_SET_TXPOWER  1
#define LORA_TX_POWER    13

RH_RF95 rf95(LORA_NSS, LORA_DIO0);
RHMesh  manager(rf95, GW_ADDR);

// ===== Protocol =====
enum : uint8_t {
  PKT_POLL   = 0x01,
  PKT_TELEM  = 0x81,
  PKT_BEACON = 0xB1
};

#pragma pack(push,1)
struct TelemetryPayload{
  uint16_t vbat_mV;
  int16_t  t;
  int16_t  h;
  int16_t  vib;
  uint8_t  led_state;
};
#pragma pack(pop)

#pragma pack(push,1)
struct BeaconPayload{
  uint16_t cycleId;
  uint8_t  startIdx;
};
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

// ===== MQTT =====
WiFiClient espClient;
PubSubClient mqtt(espClient);

static inline void wdtYield(){ delay(1); }

static void blinkComm(uint8_t n=1){
  for(uint8_t i=0;i<n;i++){
    digitalWrite(LED_COMM,HIGH); delay(20);
    digitalWrite(LED_COMM,LOW);  delay(20);
  }
}

static int idxOf(uint8_t id){
  for(int i=0;i<NCOUNT;i++) if(NODES[i]==id) return i;
  return -1;
}
static inline bool isRouterId(uint8_t id){ return id==ROUTER_ID; }

// =====================================================
// WiFi / MQTT
// =====================================================
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
  } else Serial.println("WiFi FAIL");
}

static void mqttCb(char* topic, byte* payload, unsigned int length){
  String tpc(topic), msg;
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();

  if(tpc == TOPIC_GW_CMD){
    if(msg=="reset"){ delay(50); ESP.restart(); }
    return;
  }

  int p1=tpc.indexOf("/node/"), p2=tpc.indexOf("/led");
  if(p1<0||p2<0) return;

  uint8_t nodeId=(uint8_t)tpc.substring(p1+6,p2).toInt();
  if(idxOf(nodeId)<0) return;

  uint8_t stt = (msg.toInt()!=0)?1:0;

  ledPend[nodeId].active=true;
  ledPend[nodeId].target=stt;
  ledPend[nodeId].queuedMs=millis();

  fastCycleReq=true;
  fastNodeId=nodeId;
  pollAbortReq=true;

  Serial.printf("[LED QUEUE] node %u target=%u (FAST + ABORT POLL)\n", nodeId, stt);
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

static void publishWifiRssi(){
  if(!mqtt.connected() || WiFi.status()!=WL_CONNECTED) return;
  int rssi = WiFi.RSSI();
  char buf[16]; snprintf(buf,sizeof(buf),"%d",rssi);
  mqtt.publish(TOPIC_WIFI_RSSI, buf, true);
}

// =====================================================
// LED_STATE publish
// =====================================================
static void publishLedState(uint8_t nodeId, uint8_t ledState){
  if(!mqtt.connected()) return;

  char tLed[64];
  snprintf(tLed, sizeof(tLed), TOPIC_LEDSTATE_FMT, nodeId);

  char v[2];
  v[0] = ledState ? '1' : '0';
  v[1] = 0;

  mqtt.publish(tLed, v, true);
}

static void publishTele(uint8_t nodeId, const NodeState& s){
  if(!mqtt.connected()) return;

  char topic[64];
  snprintf(topic, sizeof(topic), TOPIC_TELEM_FMT, nodeId);

  char payload[220];
  snprintf(payload, sizeof(payload),
    "{\"node\":%u,\"vbat\":%.3f,\"t\":%d,\"h\":%d,\"vib\":%d,\"led\":%u,\"rssi\":%d}",
    nodeId, s.vbat, s.t, s.h, s.vib, s.led, s.rssi
  );

  mqtt.publish(topic, payload, true);
  publishLedState(nodeId, s.led);
}

static void printLineNode(uint8_t nodeId, const NodeState& s){
  uint32_t ageSec = s.ok ? (millis()-s.lastOkMs)/1000 : 0;
  const char* label = isRouterId(nodeId) ? "Router" : "Node  ";
  Serial.printf("%s %3u ->  %5.2f | %3d | %3d | %3d |  %u  | %4d | %4lu | %s\n",
    label,nodeId,s.vbat,s.t,s.h,s.vib,s.led,s.rssi,(unsigned long)ageSec,s.ok?"YES":"NO");
}

// =====================================================
// RX Telemetry
// =====================================================
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
        TelemetryPayload p;
        memcpy(&p, buf+1, sizeof(p));

        int i=idxOf(from);
        if(i>=0){
          stNode[i].ok=true;
          stNode[i].lastOkMs=millis();
          stNode[i].rssi=rssi;
          stNode[i].vbat=p.vbat_mV/1000.0f;
          stNode[i].t=p.t;
          stNode[i].h=p.h;
          stNode[i].vib=p.vib;
          stNode[i].led=p.led_state;

          publishTele(from, stNode[i]);

          // ✅ LED 완료 판단
          if(ledPend[from].active && ledPend[from].target == p.led_state){
            Serial.printf("[LED DONE][TELEM] node %u applied=%u\n", from, p.led_state);
            ledPend[from].active=false;
            publishLedState(from, p.led_state);
          }

          gotAny=true;
        }
      }
    }

    mqtt.loop();
    wdtYield();
    delay(2);
  }

  return gotAny;
}

// =====================================================
// Beacon (유지)
// =====================================================
static void sendBeacon(uint16_t cycleId, uint8_t startIdx){
  uint8_t out[1+sizeof(BeaconPayload)];
  out[0]=PKT_BEACON;
  BeaconPayload b{cycleId, startIdx};
  memcpy(out+1, &b, sizeof(b));

  for(int i=0;i<BEACON_REPEAT;i++){
    rf95.waitCAD(); wdtYield();
    manager.sendtoWait(out, sizeof(out), RH_BROADCAST_ADDRESS);
    wdtYield();
    delay(BEACON_GAP_MS);
    mqtt.loop();
  }
}

// =====================================================
// ✅ SAFE POLL BURST + JITTER + LED모드 강도 증가
// =====================================================
static uint8_t sendPollSafeBurst(uint8_t nodeId, uint8_t* req, uint8_t len, bool isLedMode){
  // ✅ LED mode면 burst 더 강하게
  uint8_t burstCount = isLedMode ? 9 : 5;
  uint8_t err = RH_ROUTER_ERROR_NONE;

  // ✅ 첫 1회는 sendtoWait
  rf95.waitCAD(); wdtYield();
  err = manager.sendtoWait(req, len, nodeId);
  wdtYield(); mqtt.loop();

  // ✅ 나머지 burst는 NO WAIT
  for(uint8_t i=1;i<burstCount;i++){
    // ✅ 지터 (노드 RX window 랜덤이므로 GW도 랜덤화하면 겹칠 확률↑)
    delay(isLedMode ? random(55, 85) : random(60, 95));

    rf95.waitCAD(); wdtYield();
    manager.sendto(req, len, nodeId);
    wdtYield(); mqtt.loop();
  }
  return err;
}

// =====================================================
// POLL slot
// =====================================================
static bool pollInSlot(uint8_t nodeId, uint32_t slotDeadline){
  bool got=false;

  while(millis()<slotDeadline){

    if(pollAbortReq){
      if(!(fastCycleReq && nodeId==fastNodeId)){
        Serial.printf("[POLL ABORT] break slot node %u (fast=%u)\n", nodeId, fastNodeId);
        break;
      }
    }

    uint8_t req[2]={PKT_POLL, 0xFF};

    bool isLedMode = false;
    if(ledPend[nodeId].active){
      req[1]=ledPend[nodeId].target;
      isLedMode = true;
      Serial.printf("[LED CMD in POLL] node %u target=%u\n", nodeId, req[1]);
    }

    // ✅ burst 송신
    uint8_t err = sendPollSafeBurst(nodeId, req, sizeof(req), isLedMode);

    // ✅ LED mode면 RX 대기 더 길게
    uint32_t rxWait = isLedMode ? RX_WAIT_LED_MS : RX_WAIT_MS;

    if(err==RH_ROUTER_ERROR_NONE){
      if(recvTele(rxWait)){
        got=true;

        // ✅ LED pending이 끝났으면 slot에서 즉시 종료
        if(!ledPend[nodeId].active) break;
      }
    }

    // ✅ LED mode에서는 backoff를 줄여서 더 촘촘하게 재시도
    uint32_t backoff = isLedMode ? 90 : POLL_BACKOFF_MS;

    uint32_t t1=millis();
    while(millis()-t1<backoff){
      mqtt.loop();
      recvTele(25);

      if(pollAbortReq){
        if(!(fastCycleReq && nodeId==fastNodeId)){
          break;
        }
      }

      if(millis()>=slotDeadline) break;
      wdtYield();
      delay(2);
    }
  }

  int i=idxOf(nodeId);
  if(i>=0 && !got){
    if(!ledPend[nodeId].active){
      stNode[i].ok=false;
      stNode[i].rssi=-999;
    }
  }

  return got;
}

// =====================================================
// Setup / Loop
// =====================================================
void setup(){
  pinMode(LED_COMM, OUTPUT);
  digitalWrite(LED_COMM, LOW);

  Serial.begin(115200);
  delay(200);

  // LoRa reset
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, HIGH);
  digitalWrite(LORA_RST, LOW); delay(20);
  digitalWrite(LORA_RST, HIGH); delay(50);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  if(!manager.init()){
    Serial.println("RHMesh init failed");
    while(1){ blinkComm(2); delay(200); }
  }

  // ✅ RF 튜닝 제거 (노드 기본값과 동일)
  rf95.setFrequency(LORA_FREQ);

#if USE_SET_TXPOWER
  rf95.setTxPower(LORA_TX_POWER,false);
#endif

  ensureWifi();
  ensureMqtt();

  randomSeed(esp_random());
  Serial.println("GW: FINAL STABLE (LED-HOLD + JITTER BURST + NO RF TUNING)");
}

void loop(){
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  recvTele(20);

  static uint32_t lastRssiPubMs=0;
  if(millis()-lastRssiPubMs>=3000){
    lastRssiPubMs=millis();
    publishWifiRssi();
  }

  static uint16_t cycleId=0;
  cycleId++;

  uint8_t startIdx=0;
  if(fastCycleReq){
    int idx=idxOf(fastNodeId);
    if(idx>=0) startIdx=(uint8_t)idx;

    pollAbortReq=false;
    fastCycleReq=false;
  }

  sendBeacon(cycleId, startIdx);

  for(uint8_t k=0;k<NCOUNT;k++){

    if(pollAbortReq){
      Serial.println("[POLL ABORT] stop slot loop -> restart cycle for FAST");
      break;
    }

    uint8_t i=(startIdx + k) % NCOUNT;
    uint8_t nodeId=NODES[i];

    uint32_t slotStart = millis();
    uint32_t deadline  = slotStart + SLOT_MS;

    pollInSlot(nodeId, deadline);

    while(millis() < deadline){
      mqtt.loop();
      recvTele(20);
      delay(2);
    }

    printLineNode(nodeId, stNode[i]);

    mqtt.loop();
    recvTele(10);

    if(millis()-lastRssiPubMs>=3000){
      lastRssiPubMs=millis();
      publishWifiRssi();
    }
  }
}
