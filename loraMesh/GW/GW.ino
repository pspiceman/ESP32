// ====================== GATEWAY (FIXED LOOP 2->3->4->50 + LED PRIORITY ONCE + RESCUE POLL) ======================
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

// ✅ 고정 순환 노드 목록: 2→3→4→50 반복
static const uint8_t NODES[] = {2,3,4,50};
static const uint8_t NCOUNT  = sizeof(NODES)/sizeof(NODES[0]);
#define ROUTER_ID 50

// ===== Timing =====
#define SLOT_MS         10000UL
#define RX_WAIT_MS      2200
#define RX_WAIT_LED_MS  3500
#define POLL_BACKOFF_MS 160

// ===== Beacon =====
#define BEACON_REPEAT   3
#define BEACON_GAP_MS   160

// ===== RESCUE 설정 =====
static const uint32_t RESCUE_AGE_MS     = 60000UL;  // 60초 이상 미응답이면 rescue
static const uint8_t  RESCUE_FAIL_TH   = 3;         // 연속 3회 실패 시 rescue
static const uint8_t  RESCUE_TRY_COUNT = 3;         // rescue 3회 시도
static const uint32_t RESCUE_RXWAIT_MS = 4800;      // rescue는 RX 더 길게
static const uint32_t RESCUE_GAP_MS    = 220;       // rescue 재시도 간격

// ===== Router Alive Timeout (표시용) =====
static const uint32_t ROUTER_ALIVE_MS   = 130000UL; // Router는 130초 이내 마지막 수신이면 YES
static const uint32_t NODE_SLOT_OK_HOLD_MS = 1500UL; // Node 슬롯 OK로 인정하는 lastOk fresh 기준(보조)

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
static bool fastUsedOnce=false;

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
  bool ok=false;                 // 최근 수신 성공 플래그
  uint32_t lastOkMs=0;           // 마지막 수신 시간(ms)
  int16_t rssi=-999;
  float vbat=0;
  int16_t t=0,h=0,vib=0;
  uint8_t led=0;

  // ✅ 내부 로직용 (출력에서는 제거)
  uint8_t failCount=0;
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

  // topic: tswell/lora/node/<id>/led
  int p1=tpc.indexOf("/node/"), p2=tpc.indexOf("/led");
  if(p1<0||p2<0) return;

  uint8_t nodeId=(uint8_t)tpc.substring(p1+6,p2).toInt();
  if(idxOf(nodeId)<0) return;

  uint8_t stt = (msg.toInt()!=0)?1:0;

  ledPend[nodeId].active=true;
  ledPend[nodeId].target=stt;
  ledPend[nodeId].queuedMs=millis();

  // ✅ LED 우선 처리: 현재 폴링 중단 + 해당 노드로 FAST
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
// Publish
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

// =====================================================
// ✅ 출력 정책 (failCount 제거)
// - Node: 이번 슬롯에서 받았으면 YES / 못 받았으면 NO
// - Router: 최근 ROUTER_ALIVE_MS 이내 lastOk면 YES, 아니면 NO (정책상 60초 응답)
// =====================================================
static void printLineNode(uint8_t nodeId, const NodeState& s, bool slotOk){
  uint32_t ageSec = (s.lastOkMs>0) ? (millis()-s.lastOkMs)/1000 : 999999;
  const char* label = isRouterId(nodeId) ? "Router" : "Node  ";

  bool showYes;
  if(isRouterId(nodeId)){
    showYes = (s.lastOkMs>0) && ((millis()-s.lastOkMs) < ROUTER_ALIVE_MS);
  }else{
    showYes = slotOk;
  }

  Serial.printf("%s %3u ->  %5.2f | %3d | %3d | %3d |  %u  | %4d | %4lu | %s\n",
    label,nodeId,s.vbat,s.t,s.h,s.vib,s.led,s.rssi,(unsigned long)ageSec,
    showYes ? "YES" : "NO");
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

          // ✅ 성공했으니 failCount reset
          stNode[i].failCount = 0;

          publishTele(from, stNode[i]);

          // ✅ LED 완료 판단
          if(ledPend[from].active && ledPend[from].target == p.led_state){
            Serial.printf("[LED DONE][TELEM] node %u applied=%u\n", from, p.led_state);
            ledPend[from].active=false;
            publishLedState(from, p.led_state);
            pollAbortReq = false;
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
// Beacon
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
// SEND POLL (Burst + Jitter)
// =====================================================
static uint8_t sendPollSafeBurst(uint8_t nodeId, uint8_t* req, uint8_t len, bool isLedMode){
  uint8_t burstCount = isLedMode ? 9 : 5;
  uint8_t err = RH_ROUTER_ERROR_NONE;

  rf95.waitCAD(); wdtYield();
  err = manager.sendtoWait(req, len, nodeId);
  wdtYield(); mqtt.loop();

  for(uint8_t i=1;i<burstCount;i++){
    delay(isLedMode ? random(55, 85) : random(60, 95));
    rf95.waitCAD(); wdtYield();
    manager.sendto(req, len, nodeId);
    wdtYield(); mqtt.loop();
  }
  return err;
}

// =====================================================
// ✅ RESCUE POLL
// =====================================================
static void rescuePoll(uint8_t nodeId){
  int i = idxOf(nodeId);
  if(i < 0) return;

  Serial.printf("\n[RESCUE START] node %u (age=%lus fail=%u)\n",
                nodeId,
                (unsigned long)((millis()-stNode[i].lastOkMs)/1000),
                stNode[i].failCount);

  uint8_t req[2] = {PKT_POLL, 0xFF};

  for(uint8_t r=0; r<RESCUE_TRY_COUNT; r++){
    sendPollSafeBurst(nodeId, req, sizeof(req), true);
    recvTele(RESCUE_RXWAIT_MS);

    if(stNode[i].ok && (millis() - stNode[i].lastOkMs < 2000UL)){
      Serial.printf("[RESCUE OK] node %u\n\n", nodeId);
      stNode[i].failCount = 0;
      return;
    }

    delay(RESCUE_GAP_MS);
    mqtt.loop();
  }

  Serial.printf("[RESCUE FAIL] node %u\n\n", nodeId);
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

    uint8_t err = sendPollSafeBurst(nodeId, req, sizeof(req), isLedMode);
    uint32_t rxWait = isLedMode ? RX_WAIT_LED_MS : RX_WAIT_MS;

    if(err==RH_ROUTER_ERROR_NONE){
      if(recvTele(rxWait)){
        got=true;
        if(!ledPend[nodeId].active) break;
      }
    }

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

  // =====================================================
  // ✅ 성공/실패 기록
  //  - Router는 60초 응답 정책 → 슬롯 미수신을 fail 처리하지 않음
  // =====================================================
  int i=idxOf(nodeId);
  if(i>=0){
    if(got){
      stNode[i].failCount = 0;
      stNode[i].ok = true;
    }else{
      if(isRouterId(nodeId)){
        // Router miss는 상태 변화 없음
        return got;
      }

      // Node만 failCount 증가 (LED pending 아닐 때)
      if(!ledPend[nodeId].active){
        if(stNode[i].failCount < 255) stNode[i].failCount++;
        stNode[i].ok=false;
        stNode[i].rssi=-999;
      }
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

  rf95.setFrequency(LORA_FREQ);

#if USE_SET_TXPOWER
  rf95.setTxPower(LORA_TX_POWER,false);
#endif

  ensureWifi();
  ensureMqtt();

  randomSeed(esp_random());
  Serial.println("GW: FINAL (FIXED LOOP 2->3->4->50, LED PRIORITY ONCE, RESCUE POLL, SLOT YES/NO)");
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

  // =====================================================
  // startIdx 결정 (FAST는 딱 1 cycle만 적용하고 반드시 복귀)
  // =====================================================
  uint8_t startIdx = 0; // 기본은 항상 2번 노드부터 (고정 순환)

  if(fastCycleReq && !fastUsedOnce){
    int idx = idxOf(fastNodeId);
    if(idx >= 0) startIdx = (uint8_t)idx;

    pollAbortReq = false;
    fastCycleReq = false;
    fastUsedOnce = true;
    Serial.printf("[FAST APPLY ONCE] startIdx=%u (node %u)\n", startIdx, NODES[startIdx]);
  } else {
    fastUsedOnce = false;
  }

  sendBeacon(cycleId, startIdx);

  // =====================================================
  // ✅ 메인 폴링 루프
  // =====================================================
  for(uint8_t k=0;k<NCOUNT;k++){

    if(pollAbortReq){
      Serial.println("[POLL ABORT] stop slot loop -> restart cycle for FAST");
      break;
    }

    uint8_t i=(startIdx + k) % NCOUNT;
    uint8_t nodeId=NODES[i];

    uint32_t slotStart = millis();
    uint32_t deadline  = slotStart + SLOT_MS;

    // ✅ 이번 슬롯 수신 성공 여부
    bool slotOk = pollInSlot(nodeId, deadline);

    // =====================================================
    // ✅ Rescue 조건 체크 (Router 제외)
    // =====================================================
    if(!isRouterId(nodeId)){
      uint32_t age = (stNode[i].lastOkMs>0) ? (millis()-stNode[i].lastOkMs) : 99999999UL;

      bool needRescue = (age > RESCUE_AGE_MS) || (stNode[i].failCount >= RESCUE_FAIL_TH);
      if(needRescue){
        rescuePoll(nodeId);
      }
    }

    while(millis() < deadline){
      mqtt.loop();
      recvTele(20);
      delay(2);
      if(pollAbortReq) break;
    }

    // ✅ 출력: failCount 제거 + slotOk 기반 YES/NO
    printLineNode(nodeId, stNode[i], slotOk);

    mqtt.loop();
    recvTele(10);

    if(millis()-lastRssiPubMs>=3000){
      lastRssiPubMs=millis();
      publishWifiRssi();
    }

    if(pollAbortReq) break;
  }
}
