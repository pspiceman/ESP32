/*
  ESP32-S3 Home MQTT Dashboard Publisher
  --------------------------------------
  Wi-Fi:
    SSID : Backhome
    PW   : 1700note

  MQTT:
    Public broker (test/demo): broker.hivemq.com
    TCP port: 1883
    Topic base: tswell/home/esp32s3-demo-01

  Publishes:
    /status   : Wi-Fi/IP/RSSI/uptime
    /weather  : Yongin weather
    /stock    : Samsung Electronics Preferred 005935.KS
    /wifi     : nearby Wi-Fi scan summary

  External dashboard:
    Use the companion HTML file. It connects to the same broker
    through secure WebSocket and subscribes to the above topics.

  Required library:
    PubSubClient by Nick O'Leary

  Arduino-ESP32 Core:
    3.x recommended

  SECURITY NOTE:
    This example uses a PUBLIC MQTT broker for easy testing.
    Do not publish private information. For permanent use, move to
    a private authenticated MQTT broker.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------
static const char* WIFI_SSID = "Backhome";
static const char* WIFI_PASS = "1700note";

// ------------------------------------------------------------
// MQTT
// ------------------------------------------------------------
static const char* MQTT_HOST = "broker.hivemq.com";
static const uint16_t MQTT_PORT = 1883;

// Change this if more than one device is used.
// Keep the dashboard HTML value identical.
static const char* DEVICE_ID = "esp32s3-demo-01";

String TOPIC_STATUS;
String TOPIC_WEATHER;
String TOPIC_STOCK;
String TOPIC_WIFI;

WiFiClient netClient;
PubSubClient mqtt(netClient);

// ------------------------------------------------------------
// Yongin
// ------------------------------------------------------------
static const float YONGIN_LAT = 37.2411;
static const float YONGIN_LON = 127.1776;

// ------------------------------------------------------------
// Timing
// ------------------------------------------------------------
unsigned long lastStatusMs  = 0;
unsigned long lastWeatherMs = 0;
unsigned long lastStockMs   = 0;
unsigned long lastWifiMs    = 0;
unsigned long lastMqttTryMs = 0;

static const unsigned long STATUS_PERIOD  = 10UL * 1000UL;
static const unsigned long WEATHER_PERIOD = 10UL * 60UL * 1000UL;
static const unsigned long STOCK_PERIOD   = 60UL * 1000UL;
static const unsigned long WIFI_PERIOD    = 5UL * 60UL * 1000UL;
static const unsigned long MQTT_RETRY     = 5UL * 1000UL;

// ------------------------------------------------------------
// JSON helpers
// ------------------------------------------------------------
String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if ((uint8_t)c >= 0x20) {
      out += c;
    }
  }
  return out;
}

bool jsonGetNumber(const String& json, const String& key, float& value, int startAt = 0) {
  String token = "\"" + key + "\":";
  int p = json.indexOf(token, startAt);
  if (p < 0) return false;
  p += token.length();

  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;

  int e = p;
  while (e < (int)json.length()) {
    char c = json[e];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
        c == '.' || c == 'e' || c == 'E') {
      e++;
    } else {
      break;
    }
  }

  if (e <= p) return false;
  value = json.substring(p, e).toFloat();
  return true;
}

bool jsonGetInt(const String& json, const String& key, int& value, int startAt = 0) {
  float f;
  if (!jsonGetNumber(json, key, f, startAt)) return false;
  value = (int)f;
  return true;
}

bool jsonGetString(const String& json, const String& key, String& value, int startAt = 0) {
  String token = "\"" + key + "\":\"";
  int p = json.indexOf(token, startAt);
  if (p < 0) return false;
  p += token.length();

  int e = json.indexOf('"', p);
  if (e < 0) return false;

  value = json.substring(p, e);
  return true;
}

// ------------------------------------------------------------
// Weather
// ------------------------------------------------------------
String weatherText(int code) {
  if (code == 0) return "맑음";
  if (code == 1) return "대체로 맑음";
  if (code == 2) return "부분 흐림";
  if (code == 3) return "흐림";
  if (code == 45 || code == 48) return "안개";
  if (code >= 51 && code <= 57) return "이슬비";
  if (code >= 61 && code <= 67) return "비";
  if (code >= 71 && code <= 77) return "눈";
  if (code >= 80 && code <= 82) return "소나기";
  if (code == 85 || code == 86) return "눈 소나기";
  if (code >= 95) return "뇌우";
  return "정보 없음";
}

bool publishWeather() {
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) return false;

  String url =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=" + String(YONGIN_LAT, 4) +
    "&longitude=" + String(YONGIN_LON, 4) +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
    "&timezone=Asia%2FSeoul";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, url)) return false;

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[WEATHER] HTTP %d\n", httpCode);
    https.end();
    return false;
  }

  String body = https.getString();
  https.end();

  int p = body.indexOf("\"current\":");
  if (p < 0) return false;

  float temp = 0, feels = 0, humidity = 0, wind = 0;
  int wcode = -1;
  String updated = "-";

  bool ok =
    jsonGetNumber(body, "temperature_2m", temp, p) &&
    jsonGetNumber(body, "apparent_temperature", feels, p) &&
    jsonGetNumber(body, "relative_humidity_2m", humidity, p) &&
    jsonGetNumber(body, "wind_speed_10m", wind, p) &&
    jsonGetInt(body, "weather_code", wcode, p);

  jsonGetString(body, "time", updated, p);

  if (!ok) return false;

  String payload = "{";
  payload += "\"city\":\"Yongin\",";
  payload += "\"temp\":" + String(temp, 1) + ",";
  payload += "\"feels\":" + String(feels, 1) + ",";
  payload += "\"humidity\":" + String(humidity, 0) + ",";
  payload += "\"wind\":" + String(wind, 1) + ",";
  payload += "\"code\":" + String(wcode) + ",";
  payload += "\"text\":\"" + jsonEscape(weatherText(wcode)) + "\",";
  payload += "\"updated\":\"" + jsonEscape(updated) + "\"";
  payload += "}";

  bool result = mqtt.publish(TOPIC_WEATHER.c_str(), payload.c_str(), true);
  Serial.println(result ? "[MQTT] weather published" : "[MQTT] weather publish failed");
  return result;
}

// ------------------------------------------------------------
// Portfolio quotes
// - Samsung Electronics Preferred: 005935
// - SOL AI Semiconductor TOP2 Plus: 0167A0
// Source: Naver Finance domestic realtime quotes
// ------------------------------------------------------------
float parseNaverNumber(String s) {
  s.replace(",", "");
  s.replace("+", "");
  s.trim();
  return s.toFloat();
}

struct QuoteData {
  bool ok = false;
  String code;
  String name;
  float price = 0;
  float prevClose = 0;
  float change = 0;
  float changePct = 0;
  float open = 0;
  float high = 0;
  float low = 0;
  float overPrice = 0;
  String marketStatus;
  String overMarketStatus;
  String tradedAt;
};

bool fetchNaverQuote(const String& code, const String& name, QuoteData& q) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = "https://polling.finance.naver.com/api/realtime/domestic/stock/" + code;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);
  https.addHeader("User-Agent", "Mozilla/5.0 ESP32-S3-HomeDashboard");
  https.addHeader("Accept", "application/json");

  if (!https.begin(client, url)) return false;

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[QUOTE %s] HTTP %d\n", code.c_str(), httpCode);
    https.end();
    return false;
  }

  String body = https.getString();
  https.end();

  int dataPos = body.indexOf("\"datas\"");
  if (dataPos < 0) dataPos = 0;

  String priceStr, changeStr, ratioStr;
  String openStr, highStr, lowStr;
  String marketStatus, tradedAt;

  bool ok =
    jsonGetString(body, "closePrice", priceStr, dataPos) &&
    jsonGetString(body, "compareToPreviousClosePrice", changeStr, dataPos) &&
    jsonGetString(body, "fluctuationsRatio", ratioStr, dataPos);

  jsonGetString(body, "marketStatus", marketStatus, dataPos);
  jsonGetString(body, "localTradedAt", tradedAt, dataPos);
  jsonGetString(body, "openPrice", openStr, dataPos);
  jsonGetString(body, "highPrice", highStr, dataPos);
  jsonGetString(body, "lowPrice", lowStr, dataPos);

  if (!ok) {
    Serial.printf("[QUOTE %s] parse failed\n", code.c_str());
    return false;
  }

  q.ok = true;
  q.code = code;
  q.name = name;
  q.price = parseNaverNumber(priceStr);
  q.change = parseNaverNumber(changeStr);
  q.changePct = parseNaverNumber(ratioStr);
  q.prevClose = q.price - q.change;
  q.open = parseNaverNumber(openStr);
  q.high = parseNaverNumber(highStr);
  q.low = parseNaverNumber(lowStr);
  q.marketStatus = marketStatus;
  q.tradedAt = tradedAt;

  int overPos = body.indexOf("\"overMarketPriceInfo\"");
  if (overPos >= 0) {
    String overPriceStr;
    jsonGetString(body, "overPrice", overPriceStr, overPos);
    jsonGetString(body, "overMarketStatus", q.overMarketStatus, overPos);
    if (overPriceStr.length()) q.overPrice = parseNaverNumber(overPriceStr);
  }

  return true;
}

String quoteJson(const QuoteData& q) {
  String s = "{";
  s += "\"ok\":" + String(q.ok ? "true" : "false") + ",";
  s += "\"code\":\"" + jsonEscape(q.code) + "\",";
  s += "\"name\":\"" + jsonEscape(q.name) + "\",";
  s += "\"price\":" + String(q.price, 0) + ",";
  s += "\"prevClose\":" + String(q.prevClose, 0) + ",";
  s += "\"change\":" + String(q.change, 0) + ",";
  s += "\"changePct\":" + String(q.changePct, 2) + ",";
  s += "\"open\":" + String(q.open, 0) + ",";
  s += "\"high\":" + String(q.high, 0) + ",";
  s += "\"low\":" + String(q.low, 0) + ",";
  s += "\"marketStatus\":\"" + jsonEscape(q.marketStatus) + "\",";
  s += "\"tradedAt\":\"" + jsonEscape(q.tradedAt) + "\",";
  s += "\"overPrice\":" + String(q.overPrice, 0) + ",";
  s += "\"overMarketStatus\":\"" + jsonEscape(q.overMarketStatus) + "\"";
  s += "}";
  return s;
}

bool publishStock() {
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) return false;

  QuoteData samsung;
  QuoteData sol;

  bool ok1 = fetchNaverQuote("005935", "Samsung Electronics Preferred", samsung);
  bool ok2 = fetchNaverQuote("0167A0", "SOL AI Semiconductor TOP2 Plus", sol);

  if (!ok1 && !ok2) return false;

  const long SAMSUNG_QTY = 10610;
  const long SOL_QTY = 69;
  const double BASE_AMOUNT = 650000000.0;

  double samsungValue = samsung.price * (double)SAMSUNG_QTY;
  double solValue = sol.price * (double)SOL_QTY;
  double totalValue = samsungValue + solValue;
  double overBase = totalValue - BASE_AMOUNT;

  String payload = "{";
  payload += "\"samsung\":" + quoteJson(samsung) + ",";
  payload += "\"sol\":" + quoteJson(sol) + ",";
  payload += "\"samsungQty\":" + String(SAMSUNG_QTY) + ",";
  payload += "\"solQty\":" + String(SOL_QTY) + ",";
  payload += "\"samsungValue\":" + String(samsungValue, 0) + ",";
  payload += "\"solValue\":" + String(solValue, 0) + ",";
  payload += "\"totalValue\":" + String(totalValue, 0) + ",";
  payload += "\"baseAmount\":" + String(BASE_AMOUNT, 0) + ",";
  payload += "\"overBase\":" + String(overBase, 0);
  payload += "}";

  bool result = mqtt.publish(TOPIC_STOCK.c_str(), payload.c_str(), true);

  Serial.printf("[PORTFOLIO] Samsung %.0f x %ld, SOL %.0f x %ld, Total %.0f\n",
                samsung.price, SAMSUNG_QTY, sol.price, SOL_QTY, totalValue);

  return result;
}

// ------------------------------------------------------------
// Wi-Fi status
// ------------------------------------------------------------
void publishStatus() {
  if (!mqtt.connected()) return;

  String payload = "{";
  payload += "\"online\":true,";
  payload += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"gateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  payload += "\"channel\":" + String(WiFi.channel()) + ",";
  payload += "\"mac\":\"" + WiFi.macAddress() + "\",";
  payload += "\"uptime\":" + String(millis() / 1000UL);
  payload += "}";

  mqtt.publish(TOPIC_STATUS.c_str(), payload.c_str(), true);
}

// ------------------------------------------------------------
// Nearby Wi-Fi scan
// ------------------------------------------------------------
void publishWifiScan() {
  if (!mqtt.connected()) return;

  int n = WiFi.scanNetworks(false, true);
  if (n < 0) return;

  // Keep payload small for PubSubClient.
  // Publish top 12 by scan order. Dashboard sorts by RSSI.
  String payload = "{\"networks\":[";
  int limit = min(n, 12);

  for (int i = 0; i < limit; i++) {
    if (i) payload += ",";

    payload += "{";
    payload += "\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
    payload += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    payload += "\"channel\":" + String(WiFi.channel(i));
    payload += "}";
  }

  payload += "]}";

  mqtt.publish(TOPIC_WIFI.c_str(), payload.c_str(), true);
  WiFi.scanDelete();

  Serial.printf("[MQTT] WiFi scan published (%d networks)\n", limit);
}

// ------------------------------------------------------------
// MQTT
// ------------------------------------------------------------
String makeClientId() {
  uint64_t mac = ESP.getEfuseMac();
  char id[48];
  snprintf(id, sizeof(id),
           "ESP32S3-HOME-%04X%08X",
           (uint16_t)(mac >> 32),
           (uint32_t)mac);
  return String(id);
}

void connectMqtt() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (millis() - lastMqttTryMs < MQTT_RETRY) return;
  lastMqttTryMs = millis();

  String clientId = makeClientId();

  Serial.printf("[MQTT] connecting to %s:%u ...\n", MQTT_HOST, MQTT_PORT);

  // LWT: mark offline if connection disappears unexpectedly.
  String offline = "{\"online\":false}";

  bool ok = mqtt.connect(
    clientId.c_str(),
    TOPIC_STATUS.c_str(),
    0,
    true,
    offline.c_str()
  );

  if (!ok) {
    Serial.printf("[MQTT] failed, state=%d\n", mqtt.state());
    return;
  }

  Serial.println("[MQTT] connected");

  publishStatus();
  publishWeather();
  publishStock();
  publishWifiScan();

  lastStatusMs  = millis();
  lastWeatherMs = millis();
  lastStockMs   = millis();
  lastWifiMs    = millis();
}

// ------------------------------------------------------------
// Wi-Fi
// ------------------------------------------------------------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[WiFi] connecting to %s", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("[WiFi] connection failed");
  }
}

// ------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(800);

  String base = "tswell/home/" + String(DEVICE_ID);
  TOPIC_STATUS  = base + "/status";
  TOPIC_WEATHER = base + "/weather";
  TOPIC_STOCK   = base + "/stock";
  TOPIC_WIFI    = base + "/wifi";

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(4096);
  mqtt.setKeepAlive(30);

  Serial.println();
  Serial.println("ESP32-S3 MQTT Home Dashboard Publisher");
  Serial.println("--------------------------------------");
  Serial.printf("Device ID : %s\n", DEVICE_ID);
  Serial.printf("MQTT Host : %s:%u\n", MQTT_HOST, MQTT_PORT);
  Serial.printf("Base Topic: %s\n", base.c_str());

  connectWifi();
  connectMqtt();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiTry = 0;
    if (millis() - lastWifiTry > 10000UL) {
      lastWifiTry = millis();
      Serial.println("[WiFi] reconnect");
      WiFi.reconnect();
    }
    delay(5);
    return;
  }

  connectMqtt();

  if (mqtt.connected()) {
    mqtt.loop();

    unsigned long now = millis();

    if (now - lastStatusMs >= STATUS_PERIOD) {
      lastStatusMs = now;
      publishStatus();
    }

    if (now - lastWeatherMs >= WEATHER_PERIOD) {
      lastWeatherMs = now;
      publishWeather();
    }

    if (now - lastStockMs >= STOCK_PERIOD) {
      lastStockMs = now;
      publishStock();
    }

    if (now - lastWifiMs >= WIFI_PERIOD) {
      lastWifiMs = now;
      publishWifiScan();
    }
  }

  delay(2);
}
