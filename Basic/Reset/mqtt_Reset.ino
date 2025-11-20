#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ===== WiFi 설정 =====
const char* ssid     = "Backhome";
const char* password = "1700note";

// ===== HiveMQ Cloud MQTT 설정 =====
const char* mqtt_server = "6c56aefe0ddb4d57977c735f5070abe8.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "mqtt_ESP32";
const char* mqtt_pass   = "gomqtt_ESP32";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ===== LED 설정 =====
const int LED_PIN = 8;   // GPIO 8
bool ledState = false;
unsigned long previousMillis = 0;
const unsigned long interval = 1000;

// ===== IP 발행 주기 =====
unsigned long previousIPMillis = 0;
const unsigned long ipInterval = 10000;

// ===== MQTT 콜백 =====
void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("=============================");
  Serial.print("[MQTT] Topic: ");
  Serial.println(topic);
  Serial.print("[MQTT] Message: ");
  Serial.println(msg);
  Serial.println("=============================");

  if (String(topic) == "esp32/reset") {
    if (msg == "1") {
      Serial.println("[ESP32] Reset 명령 → 재부팅");
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

    if (client.connect("ESP32_Client", mqtt_user, mqtt_pass)) {
      Serial.println("연결됨!");
      client.subscribe("esp32/reset");

      String ipMsg = WiFi.localIP().toString();
      client.publish("esp32/ip", ipMsg.c_str());
      client.publish("esp32/response", "ESP32 connected");
    } else {
      Serial.print("실패, rc=");
      Serial.print(client.state());
      Serial.println(" → 5초 후 재시도");
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
  delay(300);

  Serial.println("======= ESP32-C3 + HiveMQ Cloud =======");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Active LOW: OFF

  connectWiFi();

  // HiveMQ Cloud → TLS 필요 (인증서 검증 비활성화)
  espClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("[Setup 완료] MQTT + LED + Reset Ready");
}

// ===== Loop =====
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long currentMillis = millis();

  // --- LED Blink ---
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LOW : HIGH);
  }

  // --- IP 주기적 발행 ---
  if (currentMillis - previousIPMillis >= ipInterval) {
    previousIPMillis = currentMillis;

    String ipMsg = WiFi.localIP().toString();
    client.publish("esp32/ip", ipMsg.c_str());

    Serial.println("[ESP32] IP 발행: " + ipMsg);
  }
}
