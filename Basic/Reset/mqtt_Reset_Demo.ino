#include <WiFi.h>
#include <WiFiClient.h>          // << 변경: Secure → 일반 WiFiClient
#include <PubSubClient.h>

// ===== WiFi 설정 =====
const char* ssid     = "Backhome";
const char* password = "1700note";

// ===== 데모용 MQTT Public Broker 설정 =====
const char* mqtt_server = "broker.hivemq.com";   // << 변경 1
const int   mqtt_port   = 1883;                  // << 변경 1
const char* mqtt_user   = "";                    // 데모 broker는 계정 없음
const char* mqtt_pass   = "";

// ===== MQTT 기본 객체 =====
WiFiClient espClient;        // << 변경 2: Secure → 일반 Client
PubSubClient client(espClient);

// ===== LED 설정 =====
const int LED_PIN = 8;       // GPIO 8
bool ledState = false;
unsigned long previousMillis = 0;
const unsigned long interval = 1000; // LED Blink 1초

// ===== IP 발행 주기 =====
unsigned long previousIPMillis = 0;
const unsigned long ipInterval = 10000; // 10초 간격

// ===== MQTT 콜백 =====
void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("================================");
  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);
  Serial.print("[MQTT] Message: ");
  Serial.println(msg);
  Serial.println("================================");

  // Reset 명령 수신
  if (String(topic) == "esp32/reset") {
    if (msg == "1") {
      Serial.println("[ESP32] Reset 명령 수신 → 재부팅...");
      client.publish("esp32/response", "ESP32 restarting...");
      delay(500);
      ESP.restart();
    }
  }
}

// ===== MQTT 재접속 =====
void reconnect() {
  while (!client.connected()) {
    Serial.print("[MQTT] 재접속 시도… ");
    if (client.connect("ESP32_Client")) {    // 데모 broker는 user/pass 필요 없음
      Serial.println("연결됨!");
      client.subscribe("esp32/reset");

      // 연결 직후 IP 발행
      String ipMsg = WiFi.localIP().toString();
      client.publish("esp32/ip", ipMsg.c_str());
      client.publish("esp32/response", "ESP32 connected");
    } else {
      Serial.print("실패, rc=");
      Serial.print(client.state());
      Serial.println(" 5초 후 재시도...");
      delay(5000);
    }
  }
}

// ===== WiFi 연결 =====
void connectWiFi() {
  Serial.print("WiFi 연결 중: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi 연결 완료!");
  Serial.print("IP 주소: ");
  Serial.println(WiFi.localIP());
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("========== ESP32-C3 MQTT Soft Reset + LED Blink + IP ==========");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Active LOW → LED OFF

  connectWiFi();

  // TLS 제거 → setInsecure() 필요 없음

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("[Setup] 완료, loop에서 MQTT 처리 및 LED Blink 시작");
}

// ===== Loop =====
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // ===== LED Blink =====
  if(currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LOW : HIGH); // Active LOW
  }

  // ===== IP 주기 발행 =====
  if(currentMillis - previousIPMillis >= ipInterval) {
    previousIPMillis = currentMillis;
    String ipMsg = WiFi.localIP().toString();
    client.publish("esp32/ip", ipMsg.c_str());
    Serial.println("[ESP32] IP 발행: " + ipMsg);
  }
}
