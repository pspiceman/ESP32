#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <BH1750.h>

/***************** WiFi 설정 *****************/
const char* ssid     = "Kiet";
const char* password = "1700note";

/***************** MQTT (HiveMQ Cloud) *****************/
const char*    MQTT_BROKER = "51892257f9da45da963ae82069913afc.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT   = 8883;

const char*    MQTT_USER   = "hivemq.webclient.1765336525937";
const char*    MQTT_PASS   = "&x%m0CB4IS9X1Adgy:a?";

String         clientId    = "ESP32-Node-" + String((uint32_t)ESP.getEfuseMac(), HEX);

/***************** Root CA *****************/
const char* ca_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"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\n" \
"-----END CERTIFICATE-----\n";

/***************** MQTT 토픽 *****************/
// 센서/상태
const char* TOPIC_NODE_LUX_VALUE   = "tswell/node1/lux/value";
const char* TOPIC_NODE_LUX_STATE   = "tswell/node1/lux/state";
const char* TOPIC_NODE_STATUS      = "tswell/node1/status";
const char* TOPIC_NODE_LED_CMD     = "tswell/node1/cmd/led";
const char* TOPIC_NODE_RESET_CMD   = "tswell/node1/cmd/reset";
const char* TOPIC_NODE_RSSI2       = "tswell/node1/rssi2";

// ✅ 임계값: set / state 분리
const char* TOPIC_NODE_THRESHOLD_SET   = "tswell/node1/config/threshold/set";   // 웹 → Node
const char* TOPIC_NODE_THRESHOLD_STATE = "tswell/node1/config/threshold";       // Node → 웹 (retain)

/***************** LoRa 핀 *****************/
#define LORA_SCK  18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SS   5
#define LORA_RST  17
#define LORA_DIO0 16

#define LORA_BAND 920.9E6

/***************** 제어 & 센서 *****************/
#define LED_PIN   15

#define I2C_SDA   21
#define I2C_SCL   22

BH1750 lightMeter;

/***************** 전송 주기 및 임계값 *****************/
unsigned long lastSendTime        = 0;
const unsigned long SEND_INTERVAL = 2000; // 2초

int luxThreshold = 10;     // 기본값
WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

/***************** 선언 *****************/
void setupLoRa();
void setupWiFi();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void receiveLoRa();
void sendSensorData();
void handleCommand(const String& cmd);

/***************** LoRa 초기화 *****************/
void setupLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  Serial.print("LoRa 초기화 중...");
  if (!LoRa.begin(LORA_BAND)) {
    Serial.println(" 실패!");
    while (1) {
      Serial.println("LoRa 초기화 실패, 재부팅 필요");
      delay(2000);
    }
  }
  LoRa.enableCrc();
  Serial.println(" 성공!");
}

/***************** WiFi 연결 *****************/
void setupWiFi() {
  delay(10);
  Serial.println();
  Serial.print("WiFi 연결 중: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi 연결 완료, IP: ");
  Serial.println(WiFi.localIP());
}

/***************** MQTT 콜백 *****************/
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  Serial.print("[MQTT] 수신 - Topic: ");
  Serial.print(t);
  Serial.print(" / Payload: ");
  Serial.println(msg);

  if (t == TOPIC_NODE_LED_CMD) {
    handleCommand("LED_PULSE");
  }
  else if (t == TOPIC_NODE_RESET_CMD) {
    handleCommand("RESET_NODE");
  }
  else if (t == TOPIC_NODE_THRESHOLD_SET) {
    // ✅ 웹에서 보낸 임계값 (0~9999)
    int v = msg.toInt();
    if (v < 0 || v > 9999) {
      Serial.println("[CFG] 잘못된 임계값 수신 (0~9999 허용)");
      return;
    }

    if (v != luxThreshold) {
      luxThreshold = v;
      Serial.print("[CFG] luxThreshold 업데이트: ");
      Serial.println(luxThreshold);

      // Node가 상태 토픽으로 현재값 publish (retain)
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", luxThreshold);
      mqttClient.publish(TOPIC_NODE_THRESHOLD_STATE, buf, true);
    }
  }
}

/***************** MQTT 재연결 *****************/
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("MQTT 연결 시도...");

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("성공!");

      mqttClient.subscribe(TOPIC_NODE_LED_CMD);
      mqttClient.subscribe(TOPIC_NODE_RESET_CMD);
      mqttClient.subscribe(TOPIC_NODE_THRESHOLD_SET);   // ✅ set 토픽만 구독

      mqttClient.publish(TOPIC_NODE_STATUS, "NODE_ONLINE", true);

      // 현재 임계값을 상태 토픽으로 한 번 올리고(retain),
      // 이후에는 변경될 때만 갱신
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", luxThreshold);
      mqttClient.publish(TOPIC_NODE_THRESHOLD_STATE, buf, true);
    } else {
      Serial.print("실패, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" 5초 후 재시도");
      delay(5000);
    }
  }
}

/***************** 공용 명령 처리 *****************/
void handleCommand(const String& cmd) {
  Serial.print("명령 처리: ");
  Serial.println(cmd);

  if (cmd == "LED_PULSE") {
    digitalWrite(LED_PIN, HIGH);
    delay(1000);
    digitalWrite(LED_PIN, LOW);
  }
  else if (cmd == "RESET_NODE") {
    Serial.println("[RESET] Node 소프트 리셋");
    digitalWrite(LED_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PIN, LOW);
    delay(300);
    digitalWrite(LED_PIN, HIGH);
    delay(300);
    digitalWrite(LED_PIN, LOW);
    delay(200);
    ESP.restart();
  }
}

/***************** LoRa 수신 *****************/
void receiveLoRa() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }

    long rssi = LoRa.packetRssi();
    Serial.print("LoRa 수신: ");
    Serial.print(incoming);
    Serial.print(" (RSSI=");
    Serial.print(rssi);
    Serial.println(" dBm)");

    handleCommand(incoming);
  }
}

/***************** 센서 전송 *****************/
void sendSensorData() {
  float lux = lightMeter.readLightLevel();
  if (lux < 0.0f) {
    Serial.println("BH1750 읽기 오류");
    return;
  }

  int luxInt = (int)(lux + 0.5f);
  String state = (luxInt <= luxThreshold) ? "CLOSE" : "OPEN";

  String luxStr  = String(luxInt);
  String payload = luxStr + "," + state;

  // LoRa → Gateway
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();

  // MQTT → 브로커
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_NODE_LUX_VALUE, luxStr.c_str(), true);
    mqttClient.publish(TOPIC_NODE_LUX_STATE, state.c_str(), true);
    mqttClient.publish(TOPIC_NODE_STATUS, "DATA_OK", true);

    long rssi2 = WiFi.RSSI();
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", rssi2);
    mqttClient.publish(TOPIC_NODE_RSSI2, buf, true);
  }

  Serial.print("LoRa & MQTT 전송: ");
  Serial.print(payload);
  Serial.print("  (Threshold=");
  Serial.print(luxThreshold);
  Serial.println(" lx)");
}

/***************** Setup *****************/
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Node] ESP32 + LoRa + BH1750 + WiFi(Kiet) + MQTT + Threshold CFG");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.print("BH1750 초기화 중...");
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println(" 성공!");
  } else {
    Serial.println(" 실패! 배선/주소 확인 필요");
  }

  setupLoRa();
  setupWiFi();

  wifiClient.setCACert(ca_cert);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

/***************** Loop *****************/
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  receiveLoRa();

  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL) {
    lastSendTime = now;
    sendSensorData();
  }
}
