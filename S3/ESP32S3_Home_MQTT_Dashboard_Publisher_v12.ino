/*
  ESP32-S3 Home MQTT Dashboard Publisher v12
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
// Portfolio quotes - v12
// VERIFIED SOURCE: Maeil Business Newspaper (MK) market pages
//
// Samsung Electronics Preferred 005935
//   https://stock.mk.co.kr/price/home/KR7005931001
//
// SOL AI Semiconductor TOP2 Plus 0167A0
//   https://stock.mk.co.kr/price/home/KR70167A0001
//
// The MK page exposes current/final price, previous close, open, high, low
// directly in the server-rendered HTML. This version downloads that HTML,
// converts it to plain text, then parses the values around Korean labels.
//
// During market hours: "price" = current price
// After regular market close: "price" = today's final close
// ------------------------------------------------------------
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

String cleanNumberToken(String s) {
  s.replace(",", "");
  s.replace("+", "");
  s.replace("%", "");
  s.trim();
  return s;
}

float tokenToFloat(String s) {
  return cleanNumberToken(s).toFloat();
}

String htmlToPlainText(const String& html) {
  String out;
  out.reserve(html.length() / 3);

  bool inTag = false;
  bool lastSpace = false;

  for (size_t i = 0; i < html.length(); i++) {
    char c = html[i];

    if (c == '<') {
      inTag = true;
      if (!lastSpace) {
        out += ' ';
        lastSpace = true;
      }
      continue;
    }

    if (c == '>') {
      inTag = false;
      continue;
    }

    if (inTag) continue;

    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      if (!lastSpace) {
        out += ' ';
        lastSpace = true;
      }
    } else {
      out += c;
      lastSpace = false;
    }
  }

  // Common HTML entities used by Korean finance pages.
  out.replace("&nbsp;", " ");
  out.replace("&#160;", " ");
  out.replace("&amp;", "&");
  out.replace("&lt;", "<");
  out.replace("&gt;", ">");
  out.replace("&quot;", "\"");

  // Collapse spaces one more time after entity replacement.
  String compact;
  compact.reserve(out.length());
  lastSpace = false;

  for (size_t i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c == ' ') {
      if (!lastSpace) compact += c;
      lastSpace = true;
    } else {
      compact += c;
      lastSpace = false;
    }
  }

  compact.trim();
  return compact;
}

bool isNumChar(char c) {
  return (c >= '0' && c <= '9') || c == ',' || c == '.' || c == '-' || c == '+';
}

bool numberBeforeLabel(const String& text, const String& label, int from, float& value) {
  int p = text.indexOf(label, from);
  if (p < 0) return false;

  int i = p - 1;
  while (i >= from && (text[i] == ' ' || text[i] == '\t')) i--;
  if (i < from) return false;

  int end = i + 1;
  while (i >= from && isNumChar(text[i])) i--;
  int start = i + 1;

  if (end <= start) return false;

  String tok = text.substring(start, end);
  value = tokenToFloat(tok);
  return value > 0;
}

bool numberAfterLabel(const String& text, const String& label, int from, float& value) {
  int p = text.indexOf(label, from);
  if (p < 0) return false;

  int i = p + label.length();
  int maxPos = min((int)text.length(), i + 120);

  while (i < maxPos && !isNumChar(text[i])) i++;
  if (i >= maxPos) return false;

  int start = i;
  while (i < maxPos && isNumChar(text[i])) i++;

  if (i <= start) return false;

  String tok = text.substring(start, i);
  value = tokenToFloat(tok);
  return value > 0;
}

String extractQuoteTime(const String& text, int codePos, int prevLabelPos) {
  if (codePos < 0 || prevLabelPos < 0 || prevLabelPos <= codePos) return "";

  String head = text.substring(codePos, prevLabelPos);

  // Look for HH:MM close to the quote header.
  for (int i = 0; i + 4 < (int)head.length(); i++) {
    if (isDigit(head[i]) && isDigit(head[i+1]) &&
        head[i+2] == ':' &&
        isDigit(head[i+3]) && isDigit(head[i+4])) {
      int hh = (head[i]-'0')*10 + (head[i+1]-'0');
      int mm = (head[i+3]-'0')*10 + (head[i+4]-'0');
      if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
        return head.substring(i, i+5);
      }
    }
  }
  return "";
}

bool fetchMkQuote(const String& code,
                  const String& name,
                  const String& url,
                  QuoteData& q) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(12000);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.addHeader("User-Agent",
                  "Mozilla/5.0 (Linux; ESP32-S3) AppleWebKit/537.36 Chrome/120 Safari/537.36");
  https.addHeader("Accept",
                  "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  https.addHeader("Accept-Encoding", "identity");
  https.addHeader("Accept-Language", "ko-KR,ko;q=0.9");

  Serial.printf("[MK %s] GET %s\n", code.c_str(), url.c_str());

  if (!https.begin(client, url)) {
    Serial.printf("[MK %s] https.begin failed\n", code.c_str());
    return false;
  }

  int httpCode = https.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[MK %s] HTTP %d\n", code.c_str(), httpCode);
    https.end();
    return false;
  }

  String body = https.getString();
  https.end();

  Serial.printf("[MK %s] HTML bytes=%u\n", code.c_str(), (unsigned)body.length());

  if (body.length() < 1000) {
    Serial.printf("[MK %s] body too small\n", code.c_str());
    return false;
  }

  String text = htmlToPlainText(body);
  body = String(); // release large HTML buffer as early as possible

  // Search near the actual quote header.
  int codePos = text.indexOf("(" + code + ")");
  if (codePos < 0) codePos = text.indexOf(code);

  if (codePos < 0) {
    Serial.printf("[MK %s] code not found in page text\n", code.c_str());
    Serial.println(text.substring(0, min(800, (int)text.length())));
    return false;
  }

  int prevLabelPos = text.indexOf("전일", codePos);
  if (prevLabelPos < 0) {
    Serial.printf("[MK %s] '전일' label not found\n", code.c_str());
    Serial.println(text.substring(codePos, min(codePos + 1200, (int)text.length())));
    return false;
  }

  float price = 0, prev = 0, openP = 0, highP = 0, lowP = 0;

  bool okPrice = numberBeforeLabel(text, "전일", codePos, price);
  bool okPrev  = numberAfterLabel(text, "전일", codePos, prev);
  bool okHigh  = numberAfterLabel(text, "고가", codePos, highP);
  bool okOpen  = numberAfterLabel(text, "시가", codePos, openP);
  bool okLow   = numberAfterLabel(text, "저가", codePos, lowP);

  if (!(okPrice && okPrev && okOpen && okHigh && okLow)) {
    Serial.printf(
      "[MK %s] parse FAIL price=%d prev=%d open=%d high=%d low=%d\n",
      code.c_str(), okPrice, okPrev, okOpen, okHigh, okLow
    );
    Serial.println(text.substring(codePos, min(codePos + 1400, (int)text.length())));
    return false;
  }

  q.ok = true;
  q.code = code;
  q.name = name;
  q.price = price;
  q.prevClose = prev;
  q.open = openP;
  q.high = highP;
  q.low = lowP;
  q.change = q.price - q.prevClose;
  q.changePct = (q.prevClose > 0) ? (q.change / q.prevClose * 100.0f) : 0;
  q.marketStatus = "OK";
  q.tradedAt = extractQuoteTime(text, codePos, prevLabelPos);

  Serial.printf(
    "[MK %s] OK price=%.0f prev=%.0f open=%.0f high=%.0f low=%.0f time=%s\n",
    code.c_str(),
    q.price, q.prevClose, q.open, q.high, q.low,
    q.tradedAt.c_str()
  );

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
  s += "\"overPrice\":0,";
  s += "\"overMarketStatus\":\"\"";
  s += "}";
  return s;
}

bool publishStock() {
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) return false;

  QuoteData samsung;
  QuoteData sol;

  bool ok1 = fetchMkQuote(
    "005935",
    "Samsung Electronics Preferred",
    "https://stock.mk.co.kr/price/home/KR7005931001",
    samsung
  );

  delay(500);

  bool ok2 = fetchMkQuote(
    "0167A0",
    "SOL AI Semiconductor TOP2 Plus",
    "https://stock.mk.co.kr/price/home/KR70167A0001",
    sol
  );

  if (!ok1 && !ok2) {
    Serial.println("[PORTFOLIO] both MK quote requests failed");
    return false;
  }

  const long SAMSUNG_QTY = 10610;
  const long SOL_QTY = 69;
  const double BASE_AMOUNT = 650000000.0;

  double samsungValue = samsung.ok ? samsung.price * (double)SAMSUNG_QTY : 0;
  double solValue = sol.ok ? sol.price * (double)SOL_QTY : 0;
  double totalValue = samsungValue + solValue;
  double overBase = totalValue - BASE_AMOUNT;

  String payload = "{";
  payload += "\"source\":\"MK\",";
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

  Serial.printf(
    "[PORTFOLIO] MQTT=%s SamsungOK=%d SOL_OK=%d total=%.0f delta=%.0f\n",
    result ? "OK" : "FAIL",
    ok1, ok2, totalValue, overBase
  );

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
