/*
  TSWell Universal Remote Gateway (ESP32)  [Arduino-ESP32 core 2.0.17 안정화]
  - BLE Keyboard (MiBox3)
  - IR Remote (learn/send)
  - 433MHz (RCSwitch)
  - MQTT (broker.hivemq.com:1883)

  핵심 수정 (2.0.17에서 abort 방지)
    - WiFi+BLE 동시 사용 시 WiFi Modem Sleep(PS)을 반드시 켜야 함
    - WiFi.setSleep(false) 금지
    - esp_wifi_set_ps(WIFI_PS_MIN_MODEM) 사용

  Topics
    - tswell/mibox3/cmd     (BLE key / MB: raw)
    - tswell/mibox3/status  (BLE status JSON, retain)
    - tswell/ir/cmd         (JSON {cmd})
    - tswell/ir/status      (text status)
    - tswell/433home/cmd    (plain text cmd)
    - tswell/433home/status (JSON status)
    - tswell/433home/log    (text log)

  NOTE:
    - IR 라이브러리: IRremoteESP8266
    - 433 라이브러리: RCSwitch
    - BLE: BleKeyboard
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_wifi.h"          // ✅ core 2.0.17: esp_wifi_set_ps
#include <esp_bt.h>
#include <esp_gap_ble_api.h>

// ===== BLE =====
#include <BleKeyboard.h>

// ===== IR =====
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ===== 433 =====
#include <RCSwitch.h>

// ===================== WiFi =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===================== MQTT =====================
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// ===================== Topics =====================
// BLE MiBox
const char* TOPIC_MIBOX_CMD    = "tswell/mibox3/cmd";
const char* TOPIC_MIBOX_STATUS = "tswell/mibox3/status";

// IR
const char* TOPIC_IR_CMD    = "tswell/ir/cmd";
const char* TOPIC_IR_STATUS = "tswell/ir/status";

// 433
const char* TOPIC_433_CMD    = "tswell/433home/cmd";
const char* TOPIC_433_STATUS = "tswell/433home/status";
const char* TOPIC_433_LOG    = "tswell/433home/log";

// ===================== BLE Keyboard =====================
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);

// BLE helpers (loop에서만 호출)
static inline void tapKey(uint8_t key, uint16_t ms=45){
  bleKeyboard.press(key);
  delay(ms);
  bleKeyboard.release(key);
  delay(25);
}
static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t ms=80){
  MediaKeyReport r = { b0, b1 };
  bleKeyboard.press(r);
  delay(ms);
  bleKeyboard.release(r);
  delay(40);
}
uint16_t hexToU16(String h){
  h.replace("0x",""); h.replace("0X","");
  h.trim();
  return (uint16_t) strtoul(h.c_str(), nullptr, 16);
}

// ===================== IR =====================
// Pins
const uint16_t IR_RECV_PIN     = 27;
const uint16_t IR_SEND_PIN     = 14;
const uint16_t NOTIFY_LED_PIN  = 15; // notify LED

IRrecv irrecv(IR_RECV_PIN);
decode_results results;
IRsend irsend(IR_SEND_PIN);

// Learning Mode (Serial에서 L/S로 전환)
bool learningMode = true;

// Stored IR codes
struct IRCode {
  const char* name;
  decode_type_t protocol;
  uint64_t value;
  uint16_t bits;
};

IRCode irCodes[] = {
  {"POWER1", NEC,      0x20DF10EF, 32},
  {"VOL-1",  NEC,      0x20DFC03F, 32},
  {"VOL+1",  NEC,      0x20DF40BF, 32},

  {"POWER2", NEC_LIKE, 0x55CCA2FF, 32},
  {"VOL-2",  NEC_LIKE, 0x55CCA8FF, 32},
  {"VOL+2",  NEC_LIKE, 0x55CC90FF, 32},
};
const int IR_COUNT = sizeof(irCodes) / sizeof(irCodes[0]);

// ===================== 433 =====================
#define RX_PIN 13
#define TX_PIN 12
RCSwitch rf = RCSwitch();

struct RFCode {
  const char* name;
  unsigned long value;
  uint8_t bits;
  uint8_t protocol;
};

RFCode rfCodes[] = {
  {"DOOR",   12427912, 24, 1},
  {"LIGHT",   8698436, 24, 1},
  {"SPK1",  15256641, 24, 1},
  {"SPK2",  15256642, 24, 1},
};
const int RF_COUNT = sizeof(rfCodes) / sizeof(rfCodes[0]);

// ===================== MQTT Client =====================
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ===================== Timers =====================
unsigned long lastBleStatusMs = 0;
unsigned long lastBleHeartbeatMs = 0;
unsigned long bleLastOkMs = 0;
const unsigned long BLE_DISCONNECT_GRACE_MS = 12000;
bool bleStableState = false;

unsigned long last433StatusMs = 0;

// ===================== Non-blocking reconnect =====================
unsigned long nextWiFiTryMs = 0;
unsigned long nextMQTTTryMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 3000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 3000;

// ===================== BLE Health (optional hard recover) =====================
const unsigned long BLE_HARD_RECOVER_MS = 5UL * 60UL * 1000UL;
unsigned long bleLastRealConnMs = 0;

// ===================== Pending command queues (1-deep) =====================
volatile bool pendingBleCmd = false;
String pendingBleMsg;

volatile bool pendingIrCmd = false;
String pendingIrMsgJson;

volatile bool pendingRfCmd = false;
String pendingRfMsg;

// ===================== Utils =====================
void publish433Log(const String& msg){
  Serial.println(msg);
  if(mqtt.connected()) mqtt.publish(TOPIC_433_LOG, msg.c_str(), false);
}

void publish433Status(){
  int wifiOk = (WiFi.status()==WL_CONNECTED)?1:0;
  int mqttOk = (mqtt.connected())?1:0;
  int rssi = (WiFi.status()==WL_CONNECTED)?WiFi.RSSI():-999;
  String json = String("{\"wifi\":") + wifiOk + ",\"mqtt\":" + mqttOk + ",\"rssi\":" + rssi + "}";
  if(mqtt.connected()) mqtt.publish(TOPIC_433_STATUS, json.c_str(), false);
}

void publishIrStatus(const String& msg){
  if(mqtt.connected()) mqtt.publish(TOPIC_IR_STATUS, msg.c_str(), false);
}

void blinkNotify(uint16_t ms=70){
  digitalWrite(NOTIFY_LED_PIN, HIGH);
  delay(ms);
  digitalWrite(NOTIFY_LED_PIN, LOW);
}

// ✅ BLE 상태 publish (흔들림 최소화)
void publishBleStatus(){
  if(millis() - lastBleStatusMs < 1000) return;
  lastBleStatusMs = millis();

  const unsigned long nowMs = millis();
  bool real = bleKeyboard.isConnected();
  if(real) bleLastOkMs = nowMs;

  bool eff = real;
  if(!eff && bleLastOkMs > 0 && (nowMs - bleLastOkMs) < BLE_DISCONNECT_GRACE_MS){
    eff = true;
  }

  if(eff != bleStableState){
    bleStableState = eff;
    uint32_t ts = (uint32_t)(millis()/1000);
    String payload = String("{\"ble\":") + (bleStableState?"true":"false") + ",\"ts\":" + ts + "}";
    if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload.c_str(), true);

    Serial.printf("[BLE] real=%d eff=%d lastOkAgo=%lu ms\n",
                  real?1:0, eff?1:0,
                  (bleLastOkMs==0)?0UL:(unsigned long)(nowMs - bleLastOkMs));
  }

  if(millis() - lastBleHeartbeatMs >= 2000){
    lastBleHeartbeatMs = millis();
    uint32_t ts = (uint32_t)(millis()/1000);
    String payload = String("{\"ble\":") + (bleStableState?"true":"false") + ",\"ts\":" + ts + "}";
    if(mqtt.connected()) mqtt.publish(TOPIC_MIBOX_STATUS, payload.c_str(), true);
  }
}

void bleHealthCheck(){
  if(bleKeyboard.isConnected()){
    bleLastRealConnMs = millis();
  }
  if(bleLastRealConnMs > 0 && (millis() - bleLastRealConnMs) > BLE_HARD_RECOVER_MS){
    Serial.println("[BLE] long disconnect -> restart ESP");
    ESP.restart();
  }
}

// ===================== WiFi/MQTT tick connect (NON-BLOCKING) =====================
void startWiFiIfNeeded(){
  if(WiFi.status() == WL_CONNECTED) return;
  if(millis() < nextWiFiTryMs) return;

  nextWiFiTryMs = millis() + WIFI_RETRY_INTERVAL_MS;

  WiFi.mode(WIFI_STA);

  // ✅ core 2.0.17: WiFi+BT 동시 사용 안정/abort 방지
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] begin() retry (PS_MIN_MODEM)");
}

void startMQTTIfNeeded(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(mqtt.connected()) return;
  if(millis() < nextMQTTTryMs) return;

  nextMQTTTryMs = millis() + MQTT_RETRY_INTERVAL_MS;

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  String cid = "ESP32-UNI-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("[MQTT] connect retry... ");

  if(mqtt.connect(cid.c_str(), TOPIC_MIBOX_STATUS, 0, true, "{\"ble\":false,\"ts\":0}")){
    Serial.println("OK");
    mqtt.subscribe(TOPIC_MIBOX_CMD);
    mqtt.subscribe(TOPIC_IR_CMD);
    mqtt.subscribe(TOPIC_433_CMD);
    Serial.println("[MQTT] subscribed 3 topics");
    publish433Status();
    publishIrStatus("MQTT connected");
  } else {
    Serial.print("FAIL rc="); Serial.println(mqtt.state());
  }
}

// ===================== Command processors (RUN IN LOOP) =====================
void processBleCmdIfAny(){
  if(!pendingBleCmd) return;
  pendingBleCmd = false;

  String msg = pendingBleMsg;
  msg.trim();

  Serial.printf("[MiBox][RUN] %s\n", msg.c_str());

  if(!bleKeyboard.isConnected()){
    Serial.println("[BLE] NOT CONNECTED -> ignore");
    return;
  }

  if      (msg=="up")    tapKey(KEY_UP_ARROW);
  else if (msg=="down")  tapKey(KEY_DOWN_ARROW);
  else if (msg=="left")  tapKey(KEY_LEFT_ARROW);
  else if (msg=="right") tapKey(KEY_RIGHT_ARROW);

  else if (msg=="volup")   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (msg=="voldown") bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else if (msg=="mute")    bleKeyboard.write(KEY_MEDIA_MUTE);

  else if (msg=="ok1")     tapKey(KEY_RETURN);

  else if (msg=="reset")   ESP.restart();

  else if (msg.startsWith("MB:")){
    uint16_t v = hexToU16(msg.substring(3));
    uint8_t msb = (v>>8)&0xFF;
    uint8_t lsb = v & 0xFF;
    tapMediaRaw(lsb, msb);
  } else {
    Serial.println("[MiBox] Unknown cmd");
    return;
  }

  blinkNotify();
}

void processIrCmdIfAny(){
  if(!pendingIrCmd) return;
  pendingIrCmd = false;

  String msg = pendingIrMsgJson;
  msg.trim();

  Serial.printf("[IR][RUN] %s\n", msg.c_str());

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc, msg)){
    publishIrStatus("JSON parse failed");
    return;
  }

  const char* cmd = doc["cmd"];
  if(!cmd){
    publishIrStatus("Invalid JSON: missing cmd");
    return;
  }

  if(learningMode){
    publishIrStatus("Blocked: Learning Mode ON");
    return;
  }

  bool ok=false;
  for(int i=0;i<IR_COUNT;i++){
    if(strcmp(irCodes[i].name, cmd)==0){
      irsend.send(irCodes[i].protocol, irCodes[i].value, irCodes[i].bits);
      ok=true;
      break;
    }
  }

  if(ok) blinkNotify();
  publishIrStatus(ok ? String("IR sent: ")+cmd : String("Unknown cmd: ")+cmd);
}

void processRfCmdIfAny(){
  if(!pendingRfCmd) return;
  pendingRfCmd = false;

  String msg = pendingRfMsg;
  msg.trim();

  publish433Log("CMD RX: " + msg);

  if(msg.equalsIgnoreCase("reset")){
    publish433Log("RESET by MQTT");
    delay(250);
    ESP.restart();
    return;
  }

  bool ok=false;
  for(int i=0;i<RF_COUNT;i++){
    if(msg.equalsIgnoreCase(rfCodes[i].name)){
      rf.setProtocol(rfCodes[i].protocol);
      rf.setRepeatTransmit(12);
      rf.send(rfCodes[i].value, rfCodes[i].bits);

      publish433Log("RF TX " + msg +
                    " [value:" + String(rfCodes[i].value) +
                    ", bits:" + String(rfCodes[i].bits) +
                    ", proto:" + String(rfCodes[i].protocol) + "]");

      ok=true;
      blinkNotify();
      break;
    }
  }
  if(!ok) publish433Log("Unknown CMD: " + msg);
}

// ===================== MQTT Callback (STORE ONLY) =====================
void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg; msg.reserve(length);
  for(unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();
  String t(topic);

  if(t == TOPIC_MIBOX_CMD){
    Serial.printf("[MQTT][MiBox] %s\n", msg.c_str());
    pendingBleMsg = msg;
    pendingBleCmd = true;
    return;
  }

  if(t == TOPIC_IR_CMD){
    Serial.printf("[MQTT][IR] %s\n", msg.c_str());
    pendingIrMsgJson = msg;
    pendingIrCmd = true;
    return;
  }

  if(t == TOPIC_433_CMD){
    pendingRfMsg = msg;
    pendingRfCmd = true;
    return;
  }
}

// ===================== Setup/Loop =====================
void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== TSWell Universal Remote Gateway ===");

  pinMode(NOTIFY_LED_PIN, OUTPUT);
  digitalWrite(NOTIFY_LED_PIN, LOW);

  // BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  delay(150);
  bleKeyboard.begin();
  delay(300);
  bleKeyboard.setBatteryLevel(100);
  Serial.println("[BLE] begin() + btmem_release + tx_power");

  // IR
  irsend.begin();
  irrecv.enableIRIn();

  // 433
  rf.enableReceive(RX_PIN);
  rf.enableTransmit(TX_PIN);

  // WiFi (non-blocking start)
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // ✅ 중요: abort 방지
  // (선택) 간섭 줄이고 싶으면 TX power 약간 낮추기
  // WiFi.setTxPower(WIFI_POWER_11dBm);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] begin() (PS_MIN_MODEM)");

  // MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  publish433Log("Universal Remote Booting (2.0.17 safe, PS_MIN_MODEM, queued commands)");
}

void loop(){
  // 1) Non-blocking reconnect ticks
  startWiFiIfNeeded();
  startMQTTIfNeeded();

  // 2) MQTT loop
  if(mqtt.connected()){
    mqtt.loop();
  }

  // 3) BLE status publish + health check
  publishBleStatus();
  bleHealthCheck();

  // 4) Process queued commands
  processBleCmdIfAny();
  processIrCmdIfAny();
  processRfCmdIfAny();

  // 5) 433 status
  if(millis()-last433StatusMs>1500){
    last433StatusMs=millis();
    publish433Status();
  }

  // 6) IR learning print (serial only)
  if(learningMode){
    if(irrecv.decode(&results)){
      Serial.println("---- IR RECEIVED (LEARNING MODE) ----");
      Serial.printf("Protocol : %s\n", typeToString(results.decode_type).c_str());
      Serial.printf("Bits     : %d\n", results.bits);
      Serial.printf("Value    : 0x%llX\n", (unsigned long long)results.value);
      Serial.println("-------------------------------------\n");
      irrecv.resume();
    }
  }

  // 7) Serial mode switch
  if(Serial.available()){
    char c=Serial.read();
    if(c=='L'||c=='l'){ learningMode=true;  publishIrStatus("Mode: Learning"); }
    if(c=='S'||c=='s'){ learningMode=false; publishIrStatus("Mode: Send"); }
  }

  delay(1);
}
