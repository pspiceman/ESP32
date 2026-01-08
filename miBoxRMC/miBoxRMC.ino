#include <WiFi.h>
#include <PubSubClient.h>
#include <BleKeyboard.h>

// ===== WiFi =====
#define WIFI_SSID     "Backhome"
#define WIFI_PASSWORD "1700note"

// ===== MQTT =====
const char* MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

// ===== MQTT TOPIC =====
const char* TOPIC_CMD = "tswell/mibox3/cmd";

// ===== BLE =====
BleKeyboard bleKeyboard("ESP32_MiBox3_Remote", "TSWell", 100);

// ===== MQTT =====
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ===== 안정화용 =====
static uint32_t lastMqttRxMs = 0;
static String   lastMsg = "";
const uint16_t  MQTT_DEBOUNCE_MS = 120;

static uint32_t lastStatusMs = 0;
const uint32_t STATUS_INTERVAL_MS = 5000;


// ====== 키 탭 ======
static inline void tapKey(uint8_t key, uint16_t pressMs = 45) {
  bleKeyboard.press(key);
  delay(pressMs);
  bleKeyboard.release(key);
  delay(20);
}

// ====== Media RAW 한 번 전송 ======
static inline void tapMediaRaw(uint8_t b0, uint8_t b1, uint16_t pressMs = 80) {
  MediaKeyReport r = { b0, b1 };
  bleKeyboard.press(r);
  delay(pressMs);
  bleKeyboard.release(r);
  delay(40);
}

// ====== Media BOTH 전송 (v2 핵심) ======
// ✅ A방식(MSB,LSB) + Reverse(LSB,MSB) 둘 다 연속 전송
static inline void tapMediaBothU16(uint16_t v) {
  uint8_t msb = (v >> 8) & 0xFF;
  uint8_t lsb = (v) & 0xFF;

  // A방식
  tapMediaRaw(msb, lsb);
  delay(120);
  // Reverse 방식
  tapMediaRaw(lsb, msb);
}

// hex 문자열 -> uint16_t
uint16_t hexToU16(String h) {
  h.replace("0x", ""); h.replace("0X", "");
  h.trim();
  return (uint16_t) strtoul(h.c_str(), nullptr, 16);
}


// ====== WiFi 연결 ======
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WiFi] connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(350);
    Serial.print(".");
    if (millis() - t0 > 20000) {
      Serial.println("\n[WiFi] timeout -> retry");
      WiFi.disconnect(true);
      delay(200);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
  }
  Serial.println();
  Serial.print("[WiFi] IP=");
  Serial.println(WiFi.localIP());
}

// ====== MQTT 재연결 ======
void reconnectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("[MQTT] connecting... ");
    String cid = "ESP32_MIBOX3_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqtt.connect(cid.c_str())) {
      Serial.println("OK");
      mqtt.subscribe(TOPIC_CMD);
      Serial.print("[MQTT] subscribed: ");
      Serial.println(TOPIC_CMD);
    } else {
      Serial.print("FAIL rc=");
      Serial.print(mqtt.state());
      Serial.println(" retry 2s");
      delay(2000);
    }
  }
}

// ====== MQTT 콜백 ======
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  // ✅ 디바운스(중복 메시지 방지)
  uint32_t nowMs = millis();
  if (msg == lastMsg && (nowMs - lastMqttRxMs) < MQTT_DEBOUNCE_MS) {
    return;
  }
  lastMsg = msg;
  lastMqttRxMs = nowMs;

  Serial.printf("[MQTT] %s\n", msg.c_str());

  if (!bleKeyboard.isConnected()) {
    Serial.println("[BLE] NOT CONNECTED -> ignore");
    return;
  }

  // ===== 방향키 =====
  if      (msg == "up")    tapKey(KEY_UP_ARROW);
  else if (msg == "down")  tapKey(KEY_DOWN_ARROW);
  else if (msg == "left")  tapKey(KEY_LEFT_ARROW);
  else if (msg == "right") tapKey(KEY_RIGHT_ARROW);

  // ===== OK =====
  else if (msg == "ok1") {
    tapKey(KEY_RETURN);
  }

  // ===== VOL / MUTE =====
  else if (msg == "volup")   bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
  else if (msg == "voldown") bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
  else if (msg == "mute")    bleKeyboard.write(KEY_MEDIA_MUTE);

  // ===== RESET =====
  else if (msg == "reset") {
    Serial.println("[SYS] RESET");
    delay(100);
    ESP.restart();
  }

  // ===== v2 방식: MB:xxxx 처리 =====
  else if (msg.startsWith("MB:")) {
    uint16_t v = hexToU16(msg.substring(3));
    Serial.printf("[RAW] MB v=0x%04X (BOTH send)\n", v);
    tapMediaBothU16(v);
  }

  else {
    Serial.println("[WARN] Unknown cmd");
  }
}


void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println("\n=== MiBox3 BLE Remote (v2 Compatible + Stable) ===");
  Serial.println("[BLE] begin() -> pair on MiBox3");

  bleKeyboard.begin();

  connectWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(25);
  mqtt.setSocketTimeout(5);

  reconnectMQTT();
}

void loop() {
  // ✅ WiFi 끊기면 자동 재연결
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] disconnected -> reconnect");
    connectWiFi();
  }

  // ✅ MQTT 끊기면 자동 재연결
  if (!mqtt.connected()) {
    reconnectMQTT();
  }
  mqtt.loop();

  // ✅ 상태 출력
  uint32_t nowMs = millis();
  if (nowMs - lastStatusMs > STATUS_INTERVAL_MS) {
    lastStatusMs = nowMs;
    Serial.printf("[BLE] connected=%s | RSSI=%d dBm\n",
                  bleKeyboard.isConnected() ? "YES" : "NO",
                  WiFi.RSSI());
  }
}
