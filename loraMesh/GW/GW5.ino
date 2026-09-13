// MINIMAL-STABLE variant based on the original GW5(1).ino.
// Communication timing/retry/sleep policy is intentionally left unchanged.
// Only critical halt guard + safe RESET ALL sequencing were added.
// LoRa Mesh stability v2: serialized end-to-end transactions, bounded attempts.
// Flash NR3_mesh_stability_v2 to ALL sensor Nodes and Routers with this GW.
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <string.h>
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


#define GW_ADDR 1
#define LORA_FREQ 922.0
#define LORA_TX_POWER 13
static const uint8_t NODES[]={2,3,4,50};
static const uint8_t NCOUNT=sizeof(NODES)/sizeof(NODES[0]);
static const uint8_t NORMAL_NODES[]={2,3,4};
static const uint8_t NORMAL_COUNT=sizeof(NORMAL_NODES)/sizeof(NORMAL_NODES[0]);
static const uint8_t ROUTER_ID=50;
// Scheduling targets, NOT hard realtime deadlines: RadioHead discovery blocks.
static const uint32_t NODE_SLOT_MS=10000UL;
static const uint32_t ROUTER_SLOT_MS=8000UL;
static const uint32_t RESPONSE_WAIT_MS=3500UL;
static const uint32_t TX_START_RESERVE_MS=5000UL;
static const uint32_t ROUTER_POLL_INTERVAL_MS=60000UL;
static const uint8_t MAX_ATTEMPTS=2;
static const uint32_t NODE_ALIVE_MS=70000UL, ROUTER_ALIVE_MS=130000UL;
enum : uint8_t { PKT_POLL=0x01, PKT_RESET=0x03, PKT_TELEM=0x81, PKT_SLEEP_GRANT=0x82 };
#pragma pack(push,1)
struct TelemetryPayload {uint16_t vbat_mV; int16_t t,h,vib; uint8_t led_state;};
#pragma pack(pop)
static_assert(sizeof(TelemetryPayload)==9,"Telemetry layout mismatch");
struct NodeState {
  TelemetryPayload p={};
  bool seen=false,dirty=false;
  uint32_t lastOkMs=0,rxCount=0,maxGapMs=0;
  uint16_t seq=0;
  int16_t rssi=-999;
  uint8_t hops=0,failCount=0;
};
struct LedPending {bool active=false; uint8_t target=0;};
NodeState state[NCOUNT];
LedPending led[NCOUNT];
RH_RF95 rf95(PIN_LORA_SS,PIN_LORA_DIO0);
RHMesh manager(rf95,GW_ADDR);
WiFiClient espClient;
PubSubClient mqtt(espClient);
static uint16_t nextSeq=0;
static int activeIdx=-1;
static uint16_t activeSeq=0;
static bool activeReceived=false;
static uint32_t lastRouterPollMs=0;
static bool routerPolled=false;
static uint32_t lastWifiAttemptMs=0,lastMqttAttemptMs=0,lastRssiMs=0;
static bool wifiAttempted=false,mqttAttempted=false;
// Web/MQTT reset request. "reset" resets Sensor Nodes -> Router -> GW.
// Kept as a queued action so a button press never reboots the GW in the middle
// of a LoRa transaction.
static bool resetAllRequested=false;
static bool resetGwOnlyRequested=false;

static int idxOf(uint8_t id) {
  for(uint8_t i=0;i<NCOUNT;i++) if(NODES[i]==id) return i;
  return -1;
}
static bool beforeDeadline(uint32_t now,uint32_t deadline) {
  return (int32_t)(deadline-now)>0;
}
static void mqttCb(char* topic,byte* payload,unsigned int length) {
  char msg[16];
  if(length>=sizeof(msg)) return;
  memcpy(msg,payload,length); msg[length]=0;
  if(strcmp(topic,TOPIC_GW_CMD)==0) {
    // Existing Web RESET button may keep publishing payload "reset".
    // It now means RESET ALL. "reset_all" is accepted as an alias.
    if(strcmp(msg,"reset")==0 || strcmp(msg,"reset_all")==0) {
      resetAllRequested=true;
      Serial.println("[RESET] ALL queued; execute at next cycle boundary");
    } else if(strcmp(msg,"reset_gw")==0) {
      resetGwOnlyRequested=true;
      Serial.println("[RESET] GW-only queued");
    }
    return;
  }
  if(strcmp(msg,"0")!=0 && strcmp(msg,"1")!=0) return;
  // Exact topic matching avoids truncating/aliasing malformed numeric IDs.
  for(uint8_t i=0;i<NCOUNT;i++) {
    if(NODES[i]>=50) continue;
    char expected[64];
    snprintf(expected,sizeof(expected),"tswell/lora/node/%u/led",NODES[i]);
    if(strcmp(topic,expected)!=0) continue;
    led[i].active=true; led[i].target=(msg[0]=='1');
    // Keep fixed order: sleeping nodes receive LED commands on their next slot.
    Serial.printf("[LED QUEUED] node=%u target=%u; next cycle priority\n",
                  NODES[i],led[i].target);
    break;
  }
}
static void receiveOnce() {
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN],len=sizeof(buf),from=0,hops=0;
  if(!manager.recvfromAck(buf,&len,&from,NULL,NULL,NULL,&hops)) return;
  if(len!=12 || buf[0]!=PKT_TELEM) return;
  const int i=idxOf(from);
  if(i<0) return;
  const uint16_t seq=(uint16_t)buf[10] | ((uint16_t)buf[11]<<8);
  // Only accept this target's outstanding transaction. Late old packets cannot
  // complete a different poll or refresh the web as if they were new samples.
  if(i!=activeIdx || seq!=activeSeq || activeReceived) {
    Serial.printf("[LATE/DUP] node=%u seq=%u active=%u\n",from,seq,activeSeq);
    return;
  }
  TelemetryPayload p;
  memcpy(&p,buf+1,sizeof(p));
  if(p.led_state>1) return;
  const uint32_t now=millis();
  NodeState &s=state[i];
  const uint32_t gap=s.seen ? (uint32_t)(now-s.lastOkMs) : 0;
  if(gap>s.maxGapMs) s.maxGapMs=gap;
  s.p=p; s.lastOkMs=now; s.seq=seq; s.seen=true; s.dirty=true;
  s.rxCount++; s.failCount=0; s.rssi=rf95.lastRssi(); s.hops=hops;
  activeReceived=true;
  if(led[i].active && led[i].target==p.led_state) led[i].active=false;
  Serial.printf("[E2E OK] node=%u seq=%u hops=%u lastLinkRssi=%d gapMs=%lu maxGapMs=%lu\n",
    from,seq,hops,s.rssi,(unsigned long)gap,(unsigned long)s.maxGapMs);
}
static void listenFor(uint32_t ms,bool stopOnReply) {
  const uint32_t start=millis();
  while((uint32_t)(millis()-start)<ms) {
    receiveOnce();
    if(stopOnReply && activeReceived) break;
    delay(1);
  }
}
static void publishPending() {
  if(!mqtt.connected()) return;
  for(uint8_t i=0;i<NCOUNT;i++) {
    NodeState &s=state[i];
    if(!s.dirty) continue;
    // Never replay an old queued sample as fresh after a network outage.
    if((uint32_t)(millis()-s.lastOkMs)>5000UL) {s.dirty=false; continue;}
    char topic[64],payload[256],ledTopic[64],ledValue[2];
    snprintf(topic,sizeof(topic),TOPIC_TELEM_FMT,NODES[i]);
    snprintf(payload,sizeof(payload),
      "{\"node\":%u,\"vbat\":%.3f,\"t\":%d,\"h\":%d,\"vib\":%d,\"led\":%u,\"rssi\":%d,\"seq\":%u,\"hops\":%u}",
      NODES[i],s.p.vbat_mV/1000.0f,s.p.t,s.p.h,s.p.vib,s.p.led_state,s.rssi,s.seq,s.hops);
    if(mqtt.publish(topic,payload,true)) {
      s.dirty=false;
      snprintf(ledTopic,sizeof(ledTopic),TOPIC_LEDSTATE_FMT,NODES[i]);
      ledValue[0]=s.p.led_state?'1':'0'; ledValue[1]=0;
      mqtt.publish(ledTopic,ledValue,true);
    }
  }
}
// Called only outside a radio transaction: no MQTT/socket work between
// relayed POLL and the corresponding Telemetry.
static void serviceNetwork(bool allowConnect) {
  uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED) {
    if(allowConnect && (!wifiAttempted || (uint32_t)(now-lastWifiAttemptMs)>=15000UL)) {
      wifiAttempted=true; lastWifiAttemptMs=now;
      WiFi.begin(WIFI_SSID,WIFI_PASSWORD); // no 9-second blocking wait loop
    }
    return;
  }
  if(!mqtt.connected()) {
    if(!allowConnect || (mqttAttempted && (uint32_t)(now-lastMqttAttemptMs)<15000UL)) return;
    mqttAttempted=true; lastMqttAttemptMs=now;
    String cid="tswell-gw-"+String((uint32_t)ESP.getEfuseMac(),HEX);
    if(mqtt.connect(cid.c_str())) {
      mqtt.subscribe(TOPIC_LED_SUB); mqtt.subscribe(TOPIC_GW_CMD);
      Serial.println("[MQTT] connected");
    }
  }
  if(!mqtt.connected()) return;
  mqtt.loop();
  publishPending();
  now=millis();
  if((uint32_t)(now-lastRssiMs)>=3000UL) {
    lastRssiMs=now;
    char value[16]; snprintf(value,sizeof(value),"%d",WiFi.RSSI());
    mqtt.publish(TOPIC_WIFI_RSSI,value,true);
  }
}
// Send a reset packet in the device's NORMAL slot. Keeping the original slot
// timing is important because Sensor Nodes may still be sleeping from the last
// grant. Sensors are reset first; the Router is reset last so it can relay all
// Sensor reset packets.
static bool sendResetInSlot(uint8_t id,uint32_t slotMs) {
  const uint32_t start=millis(),deadline=start+slotMs;
  uint8_t cmd[2]={PKT_RESET,0xA5};
  bool ok=false;

  for(uint8_t attempt=0;attempt<MAX_ATTEMPTS;attempt++) {
    const uint32_t now=millis();
    if(!beforeDeadline(now,deadline) || (uint32_t)(deadline-now)<TX_START_RESERVE_MS) break;
    const uint8_t err=manager.sendtoWait(cmd,sizeof(cmd),id);
    Serial.printf("[RESET TX] node=%u try=%u nextHopErr=%u\n",id,attempt+1,err);
    if(err==RH_ROUTER_ERROR_NONE) { ok=true; break; }
    listenFor(200,false);
  }

  // Preserve the same slot cadence as normal polling so later sleeping Nodes
  // wake at the expected time.
  uint32_t lastService=millis();
  while(beforeDeadline(millis(),deadline)) {
    receiveOnce();
    if((uint32_t)(millis()-lastService)>=250UL) {
      lastService=millis(); serviceNetwork(false);
    }
    delay(1);
  }
  return ok;
}

static void performResetAll() {
  resetAllRequested=false;
  Serial.println("[RESET ALL] Sensors -> Router -> GW");

  // 1) Sensor Nodes first. Their original 10 s slot cadence is preserved.
  for(uint8_t n=0;n<NORMAL_COUNT;n++) {
    sendResetInSlot(NORMAL_NODES[n],NODE_SLOT_MS);
  }

  // 2) Router last: it must remain alive while Sensor reset packets are relayed.
  sendResetInSlot(ROUTER_ID,ROUTER_SLOT_MS);

  // 3) Finally reboot the Gateway.
  Serial.println("[RESET ALL] reboot GW");
  Serial.flush();
  delay(300);
  ESP.restart();
}

static bool pollDevice(uint8_t id,uint32_t slotMs) {
  const int i=idxOf(id);
  if(i<0) return false;
  const uint32_t start=millis(),deadline=start+slotMs;
  activeIdx=i; activeSeq=++nextSeq; activeReceived=false;
  uint8_t req[4]={PKT_POLL,led[i].active?led[i].target:(uint8_t)0xFF,
                 (uint8_t)activeSeq,(uint8_t)(activeSeq>>8)};
  for(uint8_t attempt=0;attempt<MAX_ATTEMPTS;attempt++) {
    const uint32_t now=millis();
    if(!beforeDeadline(now,deadline) || (uint32_t)(deadline-now)<TX_START_RESERVE_MS) break;
    // Drain queued route failures/late telemetry before transmitting again.
    receiveOnce();
    if(activeReceived) break;
    const uint32_t txStart=millis();
    const uint8_t err=manager.sendtoWait(req,sizeof(req),id);
    Serial.printf("[POLL] node=%u seq=%u try=%u nextHopErr=%u txMs=%lu\n",
      id,activeSeq,attempt+1,err,(unsigned long)(millis()-txStart));
    // ALWAYS listen even when the first-hop ACK was lost. Do not divide the
    // end-to-end wait by a burst count. Do not issue concurrent transactions.
    listenFor(RESPONSE_WAIT_MS,true);
    if(activeReceived) break;
    listenFor(200,false);
  }
  if(!activeReceived) {
    // A response just after the retry loop still counts within the slot.
    while(beforeDeadline(millis(),deadline) && !activeReceived) {
      receiveOnce(); delay(1);
    }
  }
  const bool got=activeReceived;
  if(!got && state[i].failCount<255) state[i].failCount++;
  // After two unsuccessful slots, relearn this destination next round.
  // Do not repeatedly clear every route or reset the radio.
  if(!got && state[i].failCount>=2) manager.deleteRouteTo(id);
  // Only confirmed sensor reports authorize sleep. With three 10 s slots,
  // at least 20 s of other slots precede the next visit to this node.
  // Node anchors the 16 s deadline to its first POLL, never to grant arrival.
  if(got && id<50 && NORMAL_COUNT>=3 && NODE_SLOT_MS>=10000UL &&
     manager.getRouteTo(id)) {
    listenFor(200,false); // Allow telemetry forwarding/ACK turnaround.
    uint8_t grant[5]={PKT_SLEEP_GRANT,(uint8_t)activeSeq,
                     (uint8_t)(activeSeq>>8),128,0};
    uint8_t err=manager.sendtoWait(grant,sizeof(grant),id);
    Serial.printf("[SLEEP GRANT] node=%u seq=%u nextHopErr=%u\n",id,activeSeq,err);
  }
  activeIdx=-1;
  serviceNetwork(false);
  // Preserve ~10-second normal slots on healthy links; no unbounded RESCUE.
  uint32_t lastService=millis();
  while(beforeDeadline(millis(),deadline)) {
    receiveOnce();
    if((uint32_t)(millis()-lastService)>=250UL) {
      lastService=millis(); serviceNetwork(false);
    }
    delay(1);
  }
  Serial.printf("[SLOT] node=%u ok=%u elapsedMs=%lu fails=%u\n",
    id,got,(unsigned long)(millis()-start),state[i].failCount);
  return got;
}
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,OUTPUT); digitalWrite(LED_PIN,LOW);
  pinMode(PIN_LORA_RST,OUTPUT);
  digitalWrite(PIN_LORA_RST,LOW); delay(20);
  digitalWrite(PIN_LORA_RST,HIGH); delay(50);
  SPI.begin(PIN_VSPI_SCK,PIN_VSPI_MISO,PIN_VSPI_MOSI,PIN_LORA_SS);
  if(!manager.init()) {
    // Critical-only safeguard: never remain permanently halted on one bad
    // radio startup. Reboot and let the normal boot path try again.
    Serial.println("[FATAL] radio init failed -> reboot in 3 s");
    Serial.flush();
    delay(3000);
    ESP.restart();
  }
  rf95.setFrequency(LORA_FREQ);
  rf95.setTxPower(LORA_TX_POWER,false);
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  manager.setTimeout(400); manager.setRetries(2);
  manager.setMaxHops(5); manager.setIsaRouter(true);
  nextSeq=(uint16_t)esp_random();
  WiFi.mode(WIFI_STA);
  mqtt.setServer(MQTT_BROKER,MQTT_PORT); mqtt.setCallback(mqttCb);
  mqtt.setSocketTimeout(1);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(384);
  serviceNetwork(true);
  Serial.println("[GW v3] fixed slots; end-to-end sleep grants; startup guard 20 s");
  // A reset may occur while nodes still hold grants from the previous boot.
  const uint32_t bootGuard=millis();
  while((uint32_t)(millis()-bootGuard)<20000UL) {
    receiveOnce(); serviceNetwork(true); delay(10);
  }
}
void loop() {
  const uint32_t cycleStart=millis();
  serviceNetwork(true);

  // Execute resets only between cycles, never in the middle of POLL/TELEMETRY.
  if(resetGwOnlyRequested) {
    resetGwOnlyRequested=false;
    Serial.println("[RESET] GW-only reboot");
    Serial.flush(); delay(200); ESP.restart();
  }
  if(resetAllRequested) performResetAll();

  for(uint8_t n=0;n<NORMAL_COUNT;n++) {
    pollDevice(NORMAL_NODES[n],NODE_SLOT_MS);
    serviceNetwork(true);
  }
  if(!routerPolled || (uint32_t)(millis()-lastRouterPollMs)>=ROUTER_POLL_INTERVAL_MS) {
    pollDevice(ROUTER_ID,ROUTER_SLOT_MS);
    lastRouterPollMs=millis(); routerPolled=true;
  }
  Serial.printf("[CYCLE] elapsedMs=%lu\n",(unsigned long)(millis()-cycleStart));
}
