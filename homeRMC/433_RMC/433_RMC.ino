#include <WiFi.h>
#include <PubSubClient.h>
#include <RCSwitch.h>

// ===================== WiFi =====================
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===================== MQTT =====================
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

// GW 스타일 토픽 (HTML과 동일해야 함)
const char* TOPIC_CMD    = "tswell/433home/cmd";     // HTML → ESP32
const char* TOPIC_STATUS = "tswell/433home/status";  // ESP32 → HTML
const char* TOPIC_LOG    = "tswell/433home/log";     // ESP32 → HTML

// ===================== 433MHz =====================
#define RX_PIN 13
#define TX_PIN 12
RCSwitch rf = RCSwitch();

// 송신 코드
struct RFCode {
  const char* name;
  unsigned long value;
  uint8_t bits;
  uint8_t protocol;
};

RFCode codes[] = {
  {"DOOR",  12427912, 24, 1},
  {"LIGHT",  8698436, 24, 1},
  {"SPK1",  15256641, 24, 1},
  {"SPK2",  15256642, 24, 1},
};

// ===================== Objects =====================
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ===================== Timers =====================
unsigned long lastStatusMs = 0;
const unsigned long STATUS_INTERVAL_MS = 1500; // HTML 상태 갱신 주기와 맞춤

// ===================== Helpers =====================
void publishLog(const String& msg) {
  Serial.println(msg);
  if (mqtt.connected()) mqtt.publish(TOPIC_LOG, msg.c_str(), false);
}

void publishStatus() {
  // wifi=1/0, mqtt=1/0, rssi 값
  int wifiOk = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  int mqttOk = (mqtt.connected()) ? 1 : 0;
  int rssi   = WiFi.RSSI();

  String json = String("{\"wifi\":") + wifiOk +
                ",\"mqtt\":" + mqttOk +
                ",\"rssi\":" + rssi + "}";

  if (mqtt.connected()) mqtt.publish(TOPIC_STATUS, json.c_str(), false);
}

bool sendRF(const String& cmd) {
  for (auto &c : codes) {
    if (cmd.equalsIgnoreCase(c.name)) {
      rf.setProtocol(c.protocol);

      // ✅ 안정적 송신을 위해 repeat 설정 (GW에서도 보통 이렇게 함)
      rf.setRepeatTransmit(12);
      rf.send(c.value, c.bits);

      String log = "RF TX " + cmd +
                   " [value:" + String(c.value) +
                   ", bits:" + String(c.bits) +
                   ", proto:" + String(c.protocol) + "]";
      publishLog(log);
      return true;
    }
  }
  return false;
}

// ===================== MQTT Callback =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t = String(topic);

  if (t == TOPIC_CMD) {
    publishLog("CMD RX: " + msg);

    if (msg.equalsIgnoreCase("reset")) {
      publishLog("RESET by MQTT");
      delay(250);
      ESP.restart();
      return;
    }

    bool ok = sendRF(msg);
    if (!ok) publishLog("Unknown CMD: " + msg);
  }
}

// ===================== WiFi Connect =====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(400);

    // 너무 오래 걸리면 재시도(현장 안정성)
    if (millis() - start > 20000) {
      Serial.println("\nWiFi reconnect...");
      WiFi.disconnect(true);
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.println(WiFi.RSSI());
}

// ===================== MQTT Connect =====================
void connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  while (!mqtt.connected()) {
    String cid = "ESP32-433-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("MQTT connecting...");
    if (mqtt.connect(cid.c_str())) {
      Serial.println("OK");
      mqtt.subscribe(TOPIC_CMD);
      publishLog("MQTT connected & subscribed: " + String(TOPIC_CMD));
      publishStatus();
    } else {
      Serial.print("FAIL rc=");
      Serial.println(mqtt.state());
      delay(1500);
    }
  }
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(200);

  // 433 RX/TX init
  rf.enableReceive(RX_PIN);
  rf.enableTransmit(TX_PIN);

  // WiFi + MQTT
  connectWiFi();
  connectMQTT();

  publishLog("433 Home Control Ready");
}

// ===================== Loop =====================
void loop() {
  // WiFi 유지
  if (WiFi.status() != WL_CONNECTED) {
    publishLog("WiFi lost → reconnect");
    connectWiFi();
  }

  // MQTT 유지
  if (!mqtt.connected()) {
    publishLog("MQTT lost → reconnect");
    connectMQTT();
  }
  mqtt.loop();

  // 상태 주기 발행
  if (millis() - lastStatusMs > STATUS_INTERVAL_MS) {
    lastStatusMs = millis();
    publishStatus();
  }

  // (옵션) 433 수신 로그도 MQTT로 올리기
  if (rf.available()) {
    unsigned long v = rf.getReceivedValue();
    if (v != 0) {
      int bits = rf.getReceivedBitlength();
      int proto = rf.getReceivedProtocol();
      int delayv = rf.getReceivedDelay();

      String rx = "RF RX [value:" + String(v) +
                  ", bits:" + String(bits) +
                  ", proto:" + String(proto) +
                  ", delay:" + String(delayv) + "]";
      publishLog(rx);
    }
    rf.resetAvailable();
  }
}
