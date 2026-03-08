#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>

/***************** WiFi 설정 *****************/
const char* ssid     = "Backhome";
const char* password = "1700note";

/***************** MQTT (HiveMQ Cloud) *****************/
const char*    MQTT_BROKER = "51892257f9da45da963ae82069913afc.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT   = 8883;
const char*    MQTT_USER   = "hivemq.webclient.1765336525937";
const char*    MQTT_PASS   = "&x%m0CB4IS9X1Adgy:a?";
String         clientId    = "ESP32-Gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);

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
const char* TOPIC_LUX_VALUE = "tswell/lux/value";
const char* TOPIC_LUX_STATE = "tswell/lux/state";
const char* TOPIC_RSSI1     = "tswell/rssi1";
const char* TOPIC_RSSI2     = "tswell/rssi2";
const char* TOPIC_STATUS    = "tswell/status";
const char* TOPIC_LED_CMD   = "tswell/cmd/led";
const char* TOPIC_RESET_CMD = "tswell/cmd/reset";

/***************** LoRa 핀 *****************/
#define LORA_SCK  18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_SS   5
#define LORA_RST  17
#define LORA_DIO0 16
#define LORA_BAND 920.9E6

/***************** 타이밍 *****************/
unsigned long lastWiFiRetryTime   = 0;
const unsigned long WIFI_RETRY_MS = 10000;

unsigned long lastMQTTRetryTime   = 0;
const unsigned long MQTT_RETRY_MS = 5000;

/***************** 상태 *****************/
bool wifiStarted = false;

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

/***************** 선언 *****************/
void setupLoRa();
void startWiFi();
void serviceWiFi();
void serviceMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishRSSI(long rssi1, long rssi2);
void softResetSequence();
void serviceLoRaRx();

/***************** WiFi 시작 *****************/
void startWiFi() {
  Serial.print("[WiFi] 연결 시도: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  wifiStarted = true;
  lastWiFiRetryTime = millis();
}

/***************** WiFi 서비스 (non-blocking) *****************/
void serviceWiFi() {
  wl_status_t st = WiFi.status();

  static wl_status_t prevStatus = WL_IDLE_STATUS;
  if (st != prevStatus) {
    prevStatus = st;
    if (st == WL_CONNECTED) {
      Serial.print("[WiFi] 연결됨, IP: ");
      Serial.println(WiFi.localIP());
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
  LoRa.enableCrc();
  Serial.println(" 성공!");
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

  if (t == TOPIC_LED_CMD) {
    if (msg == "PULSE" || msg == "LED_PULSE") {
      LoRa.beginPacket();
      LoRa.print("LED_PULSE");
      LoRa.endPacket();
      Serial.println("[LoRa] Node로 LED_PULSE 전송 완료");
    }
  }
  else if (t == TOPIC_RESET_CMD) {
    if (msg == "SOFT_RESET" || msg == "RESET_NODE") {
      softResetSequence();
    }
  }
}

/***************** Gateway + Node Soft Reset *****************/
void softResetSequence() {
  Serial.println("[RESET] Soft reset sequence 시작: Node -> Gateway");

  LoRa.beginPacket();
  LoRa.print("RESET_NODE");
  LoRa.endPacket();
  Serial.println("[RESET] Node로 RESET_NODE 전송 완료");

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_STATUS, "SOFT_RESET_NODE_SENT", true);
  }

  delay(2000);

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_STATUS, "SOFT_RESET_GATEWAY", true);
  }

  Serial.println("[RESET] Gateway 소프트 리셋");
  delay(200);
  ESP.restart();
}

/***************** MQTT 서비스 (non-blocking) *****************/
void serviceMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMQTTRetryTime >= MQTT_RETRY_MS) {
      lastMQTTRetryTime = now;

      Serial.print("[MQTT] 연결 시도...");
      if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println(" 성공");
        mqttClient.subscribe(TOPIC_LED_CMD);
        mqttClient.subscribe(TOPIC_RESET_CMD);
        mqttClient.publish(TOPIC_STATUS, "GATEWAY_ONLINE", true);
      } else {
        Serial.print(" 실패, rc=");
        Serial.println(mqttClient.state());
      }
    }
    return;
  }

  mqttClient.loop();
}

/***************** RSSI 발행 *****************/
void publishRSSI(long rssi1, long rssi2) {
  if (!mqttClient.connected()) return;

  char buf[16];

  snprintf(buf, sizeof(buf), "%ld", rssi1);
  mqttClient.publish(TOPIC_RSSI1, buf, true);

  snprintf(buf, sizeof(buf), "%ld", rssi2);
  mqttClient.publish(TOPIC_RSSI2, buf, true);
}

/***************** LoRa 수신 서비스 *****************/
void serviceLoRaRx() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String incoming = "";
  while (LoRa.available()) {
    incoming += (char)LoRa.read();
  }
  incoming.trim();

  long rssi1 = LoRa.packetRssi();
  long rssi2 = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

  int commaIndex = incoming.indexOf(',');
  if (commaIndex <= 0) {
    Serial.print("[LoRa] raw 수신: ");
    Serial.println(incoming);
    return;
  }

  String luxStr   = incoming.substring(0, commaIndex);
  String stateStr = incoming.substring(commaIndex + 1);
  luxStr.trim();
  stateStr.trim();

  Serial.print("[LoRa] 수신: ");
  Serial.print(incoming);
  Serial.print(" / RSSI1=");
  Serial.print(rssi1);
  Serial.print(" dBm");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(", RSSI2=");
    Serial.print(rssi2);
    Serial.print(" dBm");
  }
  Serial.println();

  if (mqttClient.connected()) {
    mqttClient.publish(TOPIC_LUX_VALUE, luxStr.c_str(), true);
    mqttClient.publish(TOPIC_LUX_STATE, stateStr.c_str(), true);
    publishRSSI(rssi1, rssi2);
    mqttClient.publish(TOPIC_STATUS, "DATA_OK", true);
  }
}

/***************** Setup *****************/
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Gateway] ESP32 + LoRa(SX1276) + MQTT(HiveMQ Cloud TLS)");
  Serial.println("[Mode] LoRa 중계는 WiFi/MQTT와 독립 동작");

  setupLoRa();

  espClient.setCACert(ca_cert);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  startWiFi();
}

/***************** Loop *****************/
void loop() {
  // 1. LoRa는 항상 최우선
  serviceLoRaRx();

  // 2. WiFi/MQTT는 별도 서비스
  serviceWiFi();
  serviceMQTT();
}