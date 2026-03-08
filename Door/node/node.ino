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
const char* ca_cert =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

/***************** MQTT 토픽 *****************/
const char* TOPIC_NODE_LUX_VALUE       = "tswell/node1/lux/value";
const char* TOPIC_NODE_LUX_STATE       = "tswell/node1/lux/state";
const char* TOPIC_NODE_STATUS          = "tswell/node1/status";
const char* TOPIC_NODE_LED_CMD         = "tswell/node1/cmd/led";
const char* TOPIC_NODE_RESET_CMD       = "tswell/node1/cmd/reset";
const char* TOPIC_NODE_RSSI2           = "tswell/node1/rssi2";
const char* TOPIC_NODE_THRESHOLD_SET   = "tswell/node1/config/threshold/set";
const char* TOPIC_NODE_THRESHOLD_STATE = "tswell/node1/config/threshold";

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

/***************** 타이밍 *****************/
unsigned long lastSendTime              = 0;
const unsigned long SEND_INTERVAL_MS    = 1000;   // 2초 -> 1초로 단축하여 웹 타임아웃 흔들림 감소

unsigned long lastWiFiRetryTime         = 0;
const unsigned long WIFI_RETRY_MS       = 10000;

unsigned long lastMQTTRetryTime         = 0;
const unsigned long MQTT_RETRY_MS       = 3000;

unsigned long pulseUntil                = 0;
unsigned long lastMqttLoopTime          = 0;
const unsigned long MQTT_LOOP_GAP_MS    = 10;

/***************** 상태 *****************/
int luxThreshold = 10;
bool wifiStarted = false;
bool mqttWasConnected = false;
bool sensorValid = false;
int  lastLuxInt = 0;
String lastDoorState = "CLOSE";
long lastWifiRssi = 0;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

/***************** 선언 *****************/
void setupLoRa();
void startWiFi();
void serviceWiFi();
void serviceMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void receiveLoRa();
void updateSensorCache();
void publishCachedData(bool publishLoRa, bool publishMqtt, bool forceStatus);
void handleCommand(const String& cmd);
void servicePulse();
void publishThresholdState();
void publishStatus(const char* statusMsg, bool retain = true);

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

void startWiFi() {
  Serial.print("[WiFi] 연결 시도: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  wifiStarted = true;
  lastWiFiRetryTime = millis();
}

void serviceWiFi() {
  wl_status_t st = WiFi.status();

  static wl_status_t prevStatus = WL_IDLE_STATUS;
  if (st != prevStatus) {
    prevStatus = st;
    if (st == WL_CONNECTED) {
      Serial.print("[WiFi] 연결됨, IP: ");
      Serial.println(WiFi.localIP());
      lastWifiRssi = WiFi.RSSI();
      publishStatus("WIFI_CONNECTED");
    } else {
      Serial.print("[WiFi] 상태 변경: ");
      Serial.println((int)st);
    }
  }

  if (st == WL_CONNECTED) return;

  unsigned long now = millis();
  if (!wifiStarted || now - lastWiFiRetryTime >= WIFI_RETRY_MS) {
    Serial.println("[WiFi] 재연결 시도");
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.begin(ssid, password);
    wifiStarted = true;
    lastWiFiRetryTime = now;
  }
}

void publishStatus(const char* statusMsg, bool retain) {
  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_NODE_STATUS, statusMsg, retain);
  }
}

void publishThresholdState() {
  if (!mqttClient.connected()) return;

  char buf[8];
  snprintf(buf, sizeof(buf), "%d", luxThreshold);
  mqttClient.publish(TOPIC_NODE_THRESHOLD_STATE, buf, true);
}

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
    if (msg == "PULSE" || msg == "LED_PULSE") {
      handleCommand("LED_PULSE");
    }
  }
  else if (t == TOPIC_NODE_RESET_CMD) {
    if (msg == "SOFT_RESET" || msg == "RESET_NODE") {
      handleCommand("RESET_NODE");
    }
  }
  else if (t == TOPIC_NODE_THRESHOLD_SET) {
    int v = msg.toInt();
    if (v < 0 || v > 9999) {
      Serial.println("[CFG] 잘못된 임계값 수신 (0~9999 허용)");
      return;
    }

    if (v != luxThreshold) {
      luxThreshold = v;
      Serial.print("[CFG] luxThreshold 업데이트: ");
      Serial.println(luxThreshold);
      if (sensorValid) {
        lastDoorState = (lastLuxInt <= luxThreshold) ? "CLOSE" : "OPEN";
      }
      publishThresholdState();
      publishCachedData(false, true, true);
    }
  }
}

void serviceMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    mqttWasConnected = false;
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMQTTRetryTime >= MQTT_RETRY_MS) {
      lastMQTTRetryTime = now;

      Serial.print("[MQTT] 연결 시도...");
      if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println(" 성공");

        mqttClient.subscribe(TOPIC_NODE_LED_CMD);
        mqttClient.subscribe(TOPIC_NODE_RESET_CMD);
        mqttClient.subscribe(TOPIC_NODE_THRESHOLD_SET);

        mqttWasConnected = true;
        mqttClient.publish(TOPIC_NODE_STATUS, "NODE_ONLINE", true);
        publishThresholdState();

        // 재연결 직후 웹이 즉시 retained 값을 받도록 캐시값 즉시 publish
        if (!sensorValid) {
          updateSensorCache();
        }
        publishCachedData(false, true, true);
      } else {
        Serial.print(" 실패, rc=");
        Serial.println(mqttClient.state());
        mqttWasConnected = false;
      }
    }
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttLoopTime >= MQTT_LOOP_GAP_MS) {
    lastMqttLoopTime = now;
    mqttClient.loop();
  }
}

void handleCommand(const String& cmd) {
  Serial.print("[CMD] ");
  Serial.println(cmd);

  if (cmd == "LED_PULSE") {
    digitalWrite(LED_PIN, HIGH);
    pulseUntil = millis() + 2000;
    Serial.println("[ACT] LED_PIN HIGH (2초 펄스 시작)");
  }
  else if (cmd == "RESET_NODE") {
    Serial.println("[RESET] Node 소프트 리셋");
    delay(100);
    ESP.restart();
  }
}

void servicePulse() {
  if (pulseUntil != 0 && (long)(millis() - pulseUntil) >= 0) {
    digitalWrite(LED_PIN, LOW);
    pulseUntil = 0;
    Serial.println("[ACT] LED_PIN LOW (펄스 종료)");
  }
}

void receiveLoRa() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }
  incoming.trim();

  long rssi = LoRa.packetRssi();

  Serial.print("[LoRa] 수신: ");
  Serial.print(incoming);
  Serial.print(" (RSSI=");
  Serial.print(rssi);
  Serial.println(" dBm)");

  if (incoming == "LED_PULSE" || incoming == "PULSE") {
    handleCommand("LED_PULSE");
  }
  else if (incoming == "RESET_NODE" || incoming == "SOFT_RESET") {
    handleCommand("RESET_NODE");
  }
}

void updateSensorCache() {
  float lux = lightMeter.readLightLevel();
  if (lux < 0.0f) {
    Serial.println("[SENSOR] BH1750 읽기 오류");
    sensorValid = false;
    if (mqttClient.connected()) {
      mqttClient.publish(TOPIC_NODE_STATUS, "BH1750_READ_FAIL", true);
    }
    return;
  }

  sensorValid = true;
  lastLuxInt = (int)(lux + 0.5f);
  lastDoorState = (lastLuxInt <= luxThreshold) ? "CLOSE" : "OPEN";
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiRssi = WiFi.RSSI();
  }
}

void publishCachedData(bool publishLoRa, bool publishMqtt, bool forceStatus) {
  if (!sensorValid) return;

  String luxStr = String(lastLuxInt);
  String payload = luxStr + "," + lastDoorState;

  if (publishLoRa) {
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket();
  }

  if (publishMqtt && mqttClient.connected()) {
    mqttClient.publish(TOPIC_NODE_LUX_VALUE, luxStr.c_str(), true);
    mqttClient.publish(TOPIC_NODE_LUX_STATE, lastDoorState.c_str(), true);

    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", lastWifiRssi);
    mqttClient.publish(TOPIC_NODE_RSSI2, buf, true);

    if (forceStatus) {
      mqttClient.publish(TOPIC_NODE_STATUS, "DATA_OK", true);
    }
  }

  Serial.print("[TX] ");
  if (publishLoRa) Serial.print("LoRa ");
  if (publishMqtt && mqttClient.connected()) Serial.print("MQTT ");
  Serial.print(": ");
  Serial.print(payload);
  Serial.print(" (Threshold=");
  Serial.print(luxThreshold);
  Serial.println(" lx)");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Node] ESP32 + LoRa + BH1750 + WiFi(Kiet) + MQTT + Threshold CFG");
  Serial.println("[Mode] WiFi 직결 + LoRa 독립 동작 / 상태 안정화 버전");

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

  wifiClient.setCACert(ca_cert);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  updateSensorCache();
  startWiFi();
}

void loop() {
  receiveLoRa();
  servicePulse();
  serviceWiFi();
  serviceMQTT();

  unsigned long now = millis();
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    updateSensorCache();
    publishCachedData(true, true, true);
  }
}
