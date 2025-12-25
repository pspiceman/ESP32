#include <WiFi.h>
#include <WiFiClientSecure.h>   // ✅ TLS용
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>

/***************** WiFi 설정 *****************/
const char* ssid     = "Backhome";
const char* password = "1700note";

/***************** MQTT (HiveMQ Cloud) *****************/
// HiveMQ Cloud 호스트/포트
const char*    MQTT_BROKER = "51892257f9da45da963ae82069913afc.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT   = 8883;   // 🔒 TLS 포트

// HiveMQ WebClient 계정 정보
const char*    MQTT_USER   = "hivemq.webclient.1765336525937";
const char*    MQTT_PASS   = "&x%m0CB4IS9X1Adgy:a?";

// 클라이언트 ID (MAC 기반으로 생성)
String         clientId    = "ESP32-Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);

/***************** Root CA (ISRG Root X1 / HiveMQ Cloud 예제) *****************/
const char* ca_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"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\n" \
"-----END CERTIFICATE-----\n";

/***************** MQTT 토픽 *****************/
const char* TOPIC_LUX_VALUE   = "tswell/lux/value";
const char* TOPIC_LUX_STATE   = "tswell/lux/state";
const char* TOPIC_RSSI1       = "tswell/rssi1";       // Node-Gateway RSSI
const char* TOPIC_RSSI2       = "tswell/rssi2";       // Gateway-AP RSSI
const char* TOPIC_STATUS      = "tswell/status";      // 장비 상태
const char* TOPIC_LED_CMD     = "tswell/cmd/led";     // 웹 → Gateway → Node (LED 제어)
const char* TOPIC_RESET_CMD   = "tswell/cmd/reset";   // 웹 → Gateway → Node+Gateway 리셋

/***************** LoRa 핀 *****************/
#define LORA_SCK  18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SS   5
#define LORA_RST  17
#define LORA_DIO0 16

#define LORA_BAND 920.9E6   // 사용 주파수에 맞게 Node와 동일하게 설정

WiFiClientSecure espClient;       // ✅ TLS 클라이언트
PubSubClient mqttClient(espClient);

/***************** 함수 선언 *****************/
void setupWiFi();
void setupLoRa();
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishRSSI(long rssi1, long rssi2);
void softResetSequence();

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

/***************** LoRa 설정 *****************/
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
  LoRa.enableCrc();   // ✅ CRC 사용 권장
  Serial.println(" 성공!");
}

/***************** MQTT 콜백 (메시지 수신 시) *****************/
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

  // LED 제어 명령
  if (t == TOPIC_LED_CMD) {
    if (msg == "PULSE") {
      LoRa.beginPacket();
      LoRa.print("LED_PULSE");
      LoRa.endPacket();
      Serial.println("Node로 LED_PULSE 전송 완료");
    }
  }
  // RESET 명령
  else if (t == TOPIC_RESET_CMD) {
    if (msg == "SOFT_RESET") {
      softResetSequence();
    }
  }
}

/***************** Gateway + Node Soft Reset 시퀀스 *****************/
void softResetSequence() {
  Serial.println("[RESET] Soft reset sequence 시작: Node → Gateway");

  // 1) Node에 RESET 명령 전송
  LoRa.beginPacket();
  LoRa.print("RESET_NODE");   // ⚠️ Node 코드에서 이 문자열 처리해서 실제 리셋하도록 구현 필요
  LoRa.endPacket();
  Serial.println("[RESET] Node로 RESET_NODE 전송 완료");

  // 상태 MQTT 발행
  mqttClient.publish(TOPIC_STATUS, "SOFT_RESET_NODE_SENT", true);

  // Node가 리셋될 시간을 약간 기다린 후 Gateway 리셋
  delay(2000);
  mqttClient.publish(TOPIC_STATUS, "SOFT_RESET_GATEWAY", true);
  Serial.println("[RESET] Gateway 소프트 리셋 (ESP.restart)");

  delay(200);
  ESP.restart();
}

/***************** MQTT 재연결 *****************/
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("MQTT 연결 시도...");

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("성공!");
      mqttClient.subscribe(TOPIC_LED_CMD);
      mqttClient.subscribe(TOPIC_RESET_CMD);   // ✅ RESET 토픽도 구독
      mqttClient.publish(TOPIC_STATUS, "GATEWAY_ONLINE", true);
    } else {
      Serial.print("실패, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" 5초 후 재시도");
      delay(5000);
    }
  }
}

/***************** RSSI 발행 *****************/
void publishRSSI(long rssi1, long rssi2) {
  char buf[16];

  snprintf(buf, sizeof(buf), "%ld", rssi1);
  mqttClient.publish(TOPIC_RSSI1, buf, true);

  snprintf(buf, sizeof(buf), "%ld", rssi2);
  mqttClient.publish(TOPIC_RSSI2, buf, true);
}

/***************** Setup *****************/
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Gateway] ESP32 + LoRa(SX1276) + MQTT(HiveMQ Cloud TLS + CA + RESET)");

  setupWiFi();
  setupLoRa();

  // Root CA 등록
  espClient.setCACert(ca_cert);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

/***************** Loop *****************/
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // LoRa 패킷 수신
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }

    long rssi1 = LoRa.packetRssi();
    long rssi2 = WiFi.RSSI();

    // lux,state 형식만 처리 (노이즈/바이너리 패킷은 무시)
    int commaIndex = incoming.indexOf(',');
    if (commaIndex <= 0) {
      // 필요하면 raw 로그 찍기
      // Serial.print("LoRa (raw): "); Serial.println(incoming);
      return;
    }

    Serial.print("LoRa 수신: ");
    Serial.print(incoming);
    Serial.print(" / RSSI1=");
    Serial.print(rssi1);
    Serial.print(" dBm, RSSI2=");
    Serial.print(rssi2);
    Serial.println(" dBm");

    String luxStr   = incoming.substring(0, commaIndex);
    String stateStr = incoming.substring(commaIndex + 1);
    luxStr.trim();
    stateStr.trim();

    mqttClient.publish(TOPIC_LUX_VALUE, luxStr.c_str(), true);
    mqttClient.publish(TOPIC_LUX_STATE, stateStr.c_str(), true);
    publishRSSI(rssi1, rssi2);
    mqttClient.publish(TOPIC_STATUS, "DATA_OK", true);
  }
}
