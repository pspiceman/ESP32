#include <WiFi.h>
#include <PubSubClient.h>
#include <BleKeyboard.h>

// ===== WiFi =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===== MQTT =====
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// ===== MQTT TOPIC =====
const char* TOPIC_CMD    = "tswell/mibox3/cmd";
const char* TOPIC_STATUS = "tswell/mibox3/status";

BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====== 키 탭 ======
static inline void tapKey(uint8_t key, uint16_t ms=45){
  bleKeyboard.press(key);
  delay(ms);
  bleKeyboard.release(key);
  delay(25);
}

// ====== Media RAW 전송 ======
static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t ms=80){
  MediaKeyReport r = { b0, b1 };
  bleKeyboard.press(r);
  delay(ms);
  bleKeyboard.release(r);
  delay(40);
}

// hex 문자열 -> uint16_t
uint16_t hexToU16(String h){
  h.replace("0x",""); h.replace("0X","");
  h.trim();
  return (uint16_t) strtoul(h.c_str(), nullptr, 16);
}

void mqttCallback(char* topic, byte* payload, unsigned int length){
  String msg;
  msg.reserve(length);
  for (unsigned int i=0;i<length;i++) msg += (char)payload[i];
  msg.trim();

  Serial.printf("[MQTT] %s\n", msg.c_str());

  if(!bleKeyboard.isConnected()){
    Serial.println("[BLE] NOT CONNECTED -> ignore");
    return;
  }

  // ===== 기본 리모컨 =====
  if      (msg=="up")    tapKey(KEY_UP_ARROW);
  else if (msg=="down")  tapKey(KEY_DOWN_ARROW);
  else if (msg=="left")  tapKey(KEY_LEFT_ARROW);
  else if (msg=="right") tapKey(KEY_RIGHT_ARROW);

  else if (msg=="volup")   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (msg=="voldown") bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else if (msg=="mute")    bleKeyboard.write(KEY_MEDIA_MUTE);

  // ✅ OK (원본처럼 ok1만 사용)
  else if (msg=="ok1"){
    Serial.println("[KEY] OK (ok1)");
    tapKey(KEY_RETURN);
  }

  // ✅ RESET
  else if (msg=="reset"){
    Serial.println("[SYS] RESET");
    ESP.restart();
  }

  // ✅ HOME/BACK 포함 RAW 처리 (MB:)
  // 🔥 핵심: MiBox는 바이트 순서를 반대로 먹는 경우가 많아서 lsb/msb로 보냄
  else if (msg.startsWith("MB:")){
    uint16_t v = hexToU16(msg.substring(3));
    uint8_t msb = (v>>8)&0xFF;
    uint8_t lsb = v & 0xFF;

    Serial.printf("[RAW] MB v=0x%04X send(%02X,%02X)  (REV)\n", v, lsb, msb);
    tapMediaRaw(lsb, msb);   // ✅ 여기만 변경됨 (기존: tapMediaRaw(msb,lsb))
  }

  else{
    Serial.println("[WARN] Unknown cmd");
  }
}

void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] connecting");
  while(WiFi.status()!=WL_CONNECTED){
    delay(400); Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] IP=");
  Serial.println(WiFi.localIP());
}

void reconnectMQTT(){
  while(!mqtt.connected()){
    Serial.print("[MQTT] connecting... ");
    String cid = "ESP32_MIBOX3_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if(mqtt.connect(cid.c_str())){
      Serial.println("OK");
      mqtt.subscribe(TOPIC_CMD);
      Serial.print("[MQTT] subscribed: ");
      Serial.println(TOPIC_CMD);
    }else{
      Serial.print("FAIL rc=");
      Serial.print(mqtt.state());
      Serial.println(" retry 2s");
      delay(2000);
    }
  }
}

// ✅ BLE 상태 publish (1초)
void publishBleStatus(){
  static uint32_t last = 0;
  if(millis() - last < 1000) return;
  last = millis();

  bool ble = bleKeyboard.isConnected();
  String payload = String("{\"ble\":") + (ble ? "true" : "false") + "}";
  mqtt.publish(TOPIC_STATUS, payload.c_str(), true);
}

void setup(){
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== MiBox3 BLE Remote (MB reverse fix) ===");

  bleKeyboard.begin();
  Serial.println("[BLE] begin() -> pair on MiBox3");

  connectWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);

  reconnectMQTT();
}

void loop(){
  if(!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  publishBleStatus();
}
