#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// =========================
// WiFi 설정
// =========================
const char* ssid     = "Backhome";
const char* password = "1700note";

// =========================
// HiveMQ Cloud MQTT 설정
// =========================
const char* mqtt_server = "6c56aefe0ddb4d57977c735f5070abe8.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;

// HiveMQ Cloud 계정
const char* mqtt_user   = "mqtt_ESP32";
const char* mqtt_pass   = "gomqtt_ESP32";

WiFiClientSecure espClient;
PubSubClient client(espClient);

// =========================
// MQTT 메시지 수신 콜백
// =========================
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

// =========================
// MQTT 재접속
// =========================
void reconnect() {
  while (!client.connected()) {
    Serial.print("[MQTT] 재접속 시도… ");
    if (client.connect("ESP32_Client", mqtt_user, mqtt_pass)) {
      Serial.println("연결됨!");
      client.subscribe("esp32/reset");
      client.publish("esp32/response", "ESP32 connected");
    } else {
      Serial.print("실패, rc=");
      Serial.print(client.state());
      Serial.println(" 5초 후 재시도...");
      delay(5000);
    }
  }
}

// =========================
// WiFi 연결
// =========================
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

// =========================
// Setup
// =========================
void setup() {
  Serial.begin(115200); // 시리얼 모니터 설정
  delay(500);

  Serial.println("========== ESP32 MQTT Soft Reset ==========");
  connectWiFi();

  // HiveMQ Cloud TLS 인증서검증 비활성화
  espClient.setInsecure();   // ⚠️ 실제 배포 시 인증서 설정 권장

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  Serial.println("[Setup] 완료, loop에서 MQTT 처리 시작");
}

// =========================
// Loop
// =========================
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}
