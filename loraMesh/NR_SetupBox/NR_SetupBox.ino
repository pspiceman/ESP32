// ESP32 + LoRa Node/Router Production/AS SETUP BOX - UID + LED V4
// Integrated loraMesh monitor/setup HTML connects to this board via Web Bluetooth.
// This board then configures Node/Router over the dedicated LoRa SETUP channel.
//
// Hardware pin map intentionally matches GW6_Ver1_0(1).ino.
// Default board: ESP32-C3.
// Target NR firmware: NR5_NodeRouter_ProductionSetup_UID_LED_V4.ino.

#include <SPI.h>
#include <RH_RF95.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <stdlib.h>

// ===== Board Select: same mapping as current Gateway firmware =====
#define BOARD_ESP32C3 1
#define BOARD_ESP32   0

#if (BOARD_ESP32C3 + BOARD_ESP32) != 1
#error "BOARD_ESP32C3 or BOARD_ESP32: select exactly one"
#endif

#if BOARD_ESP32C3
static const int LED_PIN = 0;
static const uint8_t PIN_LORA_SS   = 7;
static const uint8_t PIN_LORA_RST  = 2;
static const uint8_t PIN_LORA_DIO0 = 3;
static const int PIN_VSPI_SCK  = 4;
static const int PIN_VSPI_MISO = 5;
static const int PIN_VSPI_MOSI = 6;
#elif BOARD_ESP32
static const int LED_PIN = 15;
static const uint8_t PIN_LORA_SS   = 5;
static const uint8_t PIN_LORA_RST  = 17;
static const uint8_t PIN_LORA_DIO0 = 16;
static const int PIN_VSPI_SCK  = 18;
static const int PIN_VSPI_MISO = 19;
static const int PIN_VSPI_MOSI = 23;
#endif

// ===== Dedicated LoRa SETUP channel =====
// MUST match Node/Router firmware.
static const float SETUP_FREQ_MHZ = 923.000f;
static const int8_t SETUP_TX_POWER_DBM = 10;
static const uint32_t SETUP_MAGIC = 0x54535731UL; // "TSW1"
static const uint8_t SETUP_PROTOCOL_VERSION = 2;
static const char* NR_PROTOCOL_LABEL = "NR-UID-LED-V4";

static const uint32_t SCAN_TOTAL_MS = 8000UL;
static const uint32_t DISCOVER_LISTEN_MS = 760UL;
static const uint32_t WRITE_REPLY_TIMEOUT_MS = 1500UL;
static const uint32_t IDENTIFY_REPLY_TIMEOUT_MS = 1500UL;
static const uint16_t IDENTIFY_LED_MS = 10000U;
static const uint8_t MAX_SCAN_DEVICES = 32;

// ===== BLE Nordic UART Service =====
// Same service UUID family as the existing Gateway BLE setup page.
static const char* BLE_NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_NUS_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // Browser -> SETUP BOX
static const char* BLE_NUS_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // SETUP BOX -> Browser
static const size_t BLE_RX_RING_SIZE = 512;
static const size_t BLE_NOTIFY_CHUNK = 20;
static const uint32_t BLE_NOTIFY_GAP_MS = 2UL;

enum : uint8_t {
  SETUP_PKT_DISCOVER     = 0xD1,
  SETUP_PKT_INFO         = 0xD2,
  SETUP_PKT_WRITE        = 0xD3,
  SETUP_PKT_RESULT       = 0xD4,
  SETUP_PKT_IDENTIFY     = 0xD5,
  SETUP_PKT_IDENTIFY_ACK = 0xD6
};

enum : uint8_t {
  SETUP_STATUS_OK       = 0,
  SETUP_STATUS_BAD_CRC  = 1,
  SETUP_STATUS_BAD_ADDR = 2,
  SETUP_STATUS_BAD_FREQ = 3,
  SETUP_STATUS_BAD_PWR  = 4,
  SETUP_STATUS_BAD_PKT  = 5
};

#pragma pack(push,1)
struct SetupDiscoverPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t nonce;
  uint16_t crc;
};
struct SetupInfoPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t nonce;
  uint32_t uid;
  uint8_t configured;
  uint8_t address;
  uint8_t role;
  uint32_t freq_khz;
  int8_t tx_power;
  uint16_t crc;
};
struct SetupIdentifyPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t uid;
  uint16_t duration_ms;
  uint16_t crc;
};
struct SetupIdentifyAckPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t uid;
  uint16_t crc;
};
struct SetupWritePacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t uid;
  uint8_t address;
  uint32_t freq_khz;
  int8_t tx_power;
  uint16_t crc;
};
struct SetupResultPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t uid;
  uint8_t status;
  uint8_t address;
  uint32_t freq_khz;
  int8_t tx_power;
  uint16_t crc;
};
#pragma pack(pop)

struct ScanDevice {
  SetupInfoPacket info;
  int16_t rssi;
};

RH_RF95 rf95(PIN_LORA_SS, PIN_LORA_DIO0);
static bool radioReady = false;

static BLEServer* bleServer = nullptr;
static BLECharacteristic* bleTx = nullptr;
static volatile bool bleConnected = false;
static uint8_t bleRxRing[BLE_RX_RING_SIZE];
static volatile size_t bleRxHead = 0;
static volatile size_t bleRxTail = 0;
static portMUX_TYPE bleRxMux = portMUX_INITIALIZER_UNLOCKED;

static char cmdLine[128];
static size_t cmdPos = 0;

static uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i=0; i<8; i++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// Keep CRC helper non-template and independent of packet struct types.
// This is safe with Arduino's .ino auto-prototype generation.
static uint16_t packetCalcCrc(const void* p, size_t totalLen) {
  return crc16((const uint8_t*)p, totalLen - sizeof(uint16_t));
}

static void ledPulse(uint16_t ms=70) {
  digitalWrite(LED_PIN, HIGH);
  delay(ms);
  digitalWrite(LED_PIN, LOW);
}

static void radioReset() {
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_RST, LOW); delay(20);
  digitalWrite(PIN_LORA_RST, HIGH); delay(50);
}

static bool initRadio() {
  pinMode(PIN_LORA_SS, OUTPUT);
  digitalWrite(PIN_LORA_SS, HIGH);
  pinMode(PIN_LORA_DIO0, INPUT);
  radioReset();
  SPI.begin(PIN_VSPI_SCK, PIN_VSPI_MISO, PIN_VSPI_MOSI, PIN_LORA_SS);
  if (!rf95.init()) return false;
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  if (!rf95.setFrequency(SETUP_FREQ_MHZ)) return false;
  rf95.setTxPower(SETUP_TX_POWER_DBM, false);
  rf95.setModeRx();
  return true;
}

static void sendDiscover(uint16_t nonce) {
  SetupDiscoverPacket p{};
  p.magic = SETUP_MAGIC;
  p.version = SETUP_PROTOCOL_VERSION;
  p.type = SETUP_PKT_DISCOVER;
  p.nonce = nonce;
  p.crc = packetCalcCrc(&p, sizeof(p));
  rf95.send((uint8_t*)&p, sizeof(p));
  rf95.waitPacketSent();
  rf95.setModeRx();
}

static int findScanUid(const ScanDevice* list, uint8_t count, uint32_t uid) {
  for(uint8_t i=0; i<count; i++) if(list[i].info.uid==uid) return (int)i;
  return -1;
}

static void collectInfoWindow(uint16_t nonce, uint32_t timeoutMs, ScanDevice* list, uint8_t& count) {
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    if (rf95.available()) {
      uint8_t buf[64];
      uint8_t len = sizeof(buf);
      if (rf95.recv(buf, &len) && len == sizeof(SetupInfoPacket)) {
        SetupInfoPacket p{};
        memcpy(&p, buf, sizeof(p));
        if (p.magic == SETUP_MAGIC && p.version == SETUP_PROTOCOL_VERSION &&
            p.type == SETUP_PKT_INFO && p.nonce == nonce &&
            p.crc == packetCalcCrc(&p, sizeof(p))) {
          const int idx=findScanUid(list,count,p.uid);
          if(idx>=0) {
            list[idx].info=p;
            list[idx].rssi=rf95.lastRssi();
          } else if(count<MAX_SCAN_DEVICES) {
            list[count].info=p;
            list[count].rssi=rf95.lastRssi();
            count++;
          }
        }
      }
    }
    delay(1);
  }
}

static uint8_t scanTargets(ScanDevice* list) {
  if (!radioReady) return 0;
  uint8_t count=0;
  uint16_t nonce=(uint16_t)(micros() & 0xFFFFU);
  const uint32_t start=millis();
  while((uint32_t)(millis()-start)<SCAN_TOTAL_MS && count<MAX_SCAN_DEVICES) {
    nonce=(uint16_t)(nonce+0x31U);
    sendDiscover(nonce);
    collectInfoWindow(nonce,DISCOVER_LISTEN_MS,list,count);
  }
  if(count) ledPulse(100);
  return count;
}

static bool receiveIdentifyAck(uint32_t uid, uint32_t timeoutMs) {
  const uint32_t start=millis();
  while((uint32_t)(millis()-start)<timeoutMs) {
    if(rf95.available()) {
      uint8_t buf[64];
      uint8_t len=sizeof(buf);
      if(rf95.recv(buf,&len) && len==sizeof(SetupIdentifyAckPacket)) {
        SetupIdentifyAckPacket p{};
        memcpy(&p,buf,sizeof(p));
        if(p.magic==SETUP_MAGIC && p.version==SETUP_PROTOCOL_VERSION &&
           p.type==SETUP_PKT_IDENTIFY_ACK && p.uid==uid &&
           p.crc==packetCalcCrc(&p,sizeof(p))) return true;
      }
    }
    delay(1);
  }
  return false;
}

static bool identifyTarget(uint32_t uid) {
  SetupIdentifyPacket p{};
  p.magic=SETUP_MAGIC;
  p.version=SETUP_PROTOCOL_VERSION;
  p.type=SETUP_PKT_IDENTIFY;
  p.uid=uid;
  p.duration_ms=IDENTIFY_LED_MS;
  p.crc=packetCalcCrc(&p,sizeof(p));
  for(uint8_t attempt=0; attempt<3; attempt++) {
    rf95.send((uint8_t*)&p,sizeof(p));
    rf95.waitPacketSent();
    rf95.setModeRx();
    if(receiveIdentifyAck(uid,IDENTIFY_REPLY_TIMEOUT_MS)) {
      ledPulse(80);
      return true;
    }
    delay(100);
  }
  return false;
}

static bool receiveResult(uint32_t uid, uint32_t timeoutMs, SetupResultPacket& out) {
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < timeoutMs) {
    if (rf95.available()) {
      uint8_t buf[64];
      uint8_t len = sizeof(buf);
      if (rf95.recv(buf, &len) && len == sizeof(SetupResultPacket)) {
        SetupResultPacket p{};
        memcpy(&p, buf, sizeof(p));
        if (p.magic == SETUP_MAGIC && p.version == SETUP_PROTOCOL_VERSION &&
            p.type == SETUP_PKT_RESULT && p.uid==uid &&
            p.crc == packetCalcCrc(&p, sizeof(p))) {
          out = p;
          return true;
        }
      }
    }
    delay(1);
  }
  return false;
}

static bool writeTarget(uint32_t uid, uint8_t addr, uint32_t freqKhz, int8_t power, SetupResultPacket& result) {
  if (!radioReady) return false;
  SetupWritePacket p{};
  p.magic = SETUP_MAGIC;
  p.version = SETUP_PROTOCOL_VERSION;
  p.type = SETUP_PKT_WRITE;
  p.uid = uid;
  p.address = addr;
  p.freq_khz = freqKhz;
  p.tx_power = power;
  p.crc = packetCalcCrc(&p, sizeof(p));

  for (uint8_t attempt=0; attempt<3; attempt++) {
    rf95.send((uint8_t*)&p, sizeof(p));
    rf95.waitPacketSent();
    rf95.setModeRx();
    if (receiveResult(uid, WRITE_REPLY_TIMEOUT_MS, result)) {
      if (result.status == SETUP_STATUS_OK) ledPulse(120);
      return true;
    }
    delay(120);
  }
  return false;
}

static const char* statusToken(uint8_t s) {
  switch (s) {
    case SETUP_STATUS_OK:       return "OK";
    case SETUP_STATUS_BAD_CRC:  return "BAD_CRC";
    case SETUP_STATUS_BAD_ADDR: return "BAD_ADDR";
    case SETUP_STATUS_BAD_FREQ: return "BAD_FREQ";
    case SETUP_STATUS_BAD_PWR:  return "BAD_POWER";
    default:                    return "BAD_PACKET";
  }
}

static void bleRxReset() {
  portENTER_CRITICAL(&bleRxMux);
  bleRxHead = bleRxTail = 0;
  portEXIT_CRITICAL(&bleRxMux);
}

static void bleRxPush(uint8_t c) {
  portENTER_CRITICAL(&bleRxMux);
  const size_t next = (bleRxHead + 1U) % BLE_RX_RING_SIZE;
  if (next != bleRxTail) {
    bleRxRing[bleRxHead] = c;
    bleRxHead = next;
  }
  portEXIT_CRITICAL(&bleRxMux);
}

static bool bleRxPop(uint8_t& c) {
  bool ok = false;
  portENTER_CRITICAL(&bleRxMux);
  if (bleRxTail != bleRxHead) {
    c = bleRxRing[bleRxTail];
    bleRxTail = (bleRxTail + 1U) % BLE_RX_RING_SIZE;
    ok = true;
  }
  portEXIT_CRITICAL(&bleRxMux);
  return ok;
}

static void bleSendText(const String& text) {
  if (!bleConnected || !bleTx) return;
  const uint8_t* data = (const uint8_t*)text.c_str();
  size_t len = text.length();
  for (size_t off=0; off<len; off+=BLE_NOTIFY_CHUNK) {
    const size_t n = (len - off > BLE_NOTIFY_CHUNK) ? BLE_NOTIFY_CHUNK : (len - off);
    bleTx->setValue((uint8_t*)(data + off), n);
    bleTx->notify();
    delay(BLE_NOTIFY_GAP_MS);
  }
}

static void bleSendLine(const String& line) {
  bleSendText(line + "\n");
  Serial.println("[BLE TX] " + line);
}

class SetupBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    (void)pServer;
    bleConnected = true;
    Serial.println("[BLE] connected");
  }
  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
    Serial.println("[BLE] disconnected");
    if (pServer) pServer->startAdvertising();
  }
};

class SetupBleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    if (!pCharacteristic) return;
    String value = pCharacteristic->getValue();
    for (size_t i=0; i<value.length(); i++) bleRxPush((uint8_t)value[i]);
  }
};

static SetupBleServerCallbacks bleServerCallbacks;
static SetupBleRxCallbacks bleRxCallbacks;

static String makeDeviceName() {
  const uint16_t suffix = (uint16_t)(ESP.getEfuseMac() & 0xFFFFU);
  char name[32];
  snprintf(name, sizeof(name), "LoRa-NR-SETUP-%04X", suffix);
  return String(name);
}

static bool initBle() {
  bleRxReset();
  bleConnected = false;
  const String devName = makeDeviceName();

  BLEDevice::init(devName);
  bleServer = BLEDevice::createServer();
  if (!bleServer) return false;
  bleServer->setCallbacks(&bleServerCallbacks);

  BLEService* service = bleServer->createService(BLE_NUS_SERVICE_UUID);
  if (!service) return false;

  bleTx = service->createCharacteristic(BLE_NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* rx = service->createCharacteristic(
    BLE_NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  if (!bleTx || !rx) return false;

#if defined(CONFIG_BLUEDROID_ENABLED)
  bleTx->addDescriptor(new BLE2902());
#endif
  rx->setCallbacks(&bleRxCallbacks);
  service->start();

  BLEAdvertising* advertising = bleServer->getAdvertising();
  advertising->addServiceUUID(BLE_NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.print("[BLE] advertising: ");
  Serial.println(devName);
  return true;
}

static void handleHello() {
  char buf[110];
  snprintf(buf, sizeof(buf), "READY,LoRa-NR-SETUP,UID-LED-V2,%.3f", SETUP_FREQ_MHZ);
  bleSendLine(String(buf));
}

static void uidHex(uint32_t uid,char* out,size_t outLen) {
  snprintf(out,outLen,"%08lX",(unsigned long)uid);
}

static bool parseUidHex(const char* s,uint32_t& uid) {
  if(!s || !*s) return false;
  char* end=nullptr;
  const unsigned long v=strtoul(s,&end,16);
  if(!end || *end!='\0') return false;
  uid=(uint32_t)v;
  return uid!=0UL;
}

static void handleScan() {
  if (!radioReady) {
    bleSendLine("ERR,RADIO_INIT_FAILED");
    return;
  }

  bleSendLine("SCANBEGIN");
  ScanDevice devices[MAX_SCAN_DEVICES]{};
  const uint8_t count=scanTargets(devices);
  for(uint8_t i=0; i<count; i++) {
    const SetupInfoPacket& info=devices[i].info;
    char uid[12]; uidHex(info.uid,uid,sizeof(uid));
    char buf[190];
    snprintf(
      buf, sizeof(buf),
      "DEVICE,%s,%s,%u,%u,%.3f,%d,%d,%s",
      uid,
      info.role ? "router" : "sensor",
      (unsigned)info.configured,
      (unsigned)info.address,
      info.freq_khz / 1000.0f,
      (int)info.tx_power,
      (int)devices[i].rssi,
      NR_PROTOCOL_LABEL
    );
    bleSendLine(String(buf));
  }
  char endBuf[32];
  snprintf(endBuf,sizeof(endBuf),"SCANEND,%u",(unsigned)count);
  bleSendLine(String(endBuf));
}

static void handleIdentifyCommand(const char* line) {
  // Expected: IDENTIFY,<UIDHEX>
  const char* comma=strchr(line,',');
  if(!comma || !*(comma+1)) { bleSendLine("ERR,BAD_FORMAT"); return; }
  uint32_t uid=0;
  if(!parseUidHex(comma+1,uid)) { bleSendLine("ERR,BAD_UID"); return; }
  if(!identifyTarget(uid)) { bleSendLine("ERR,IDENTIFY_TIMEOUT"); return; }
  char uidText[12]; uidHex(uid,uidText,sizeof(uidText));
  bleSendLine(String("IDENTIFY,OK,")+uidText);
}

static void handleWriteCommand(const char* line) {
  // Expected: WRITE,<UIDHEX>,<addr>,<freqMHz>,<power>
  char work[144];
  strlcpy(work, line, sizeof(work));

  char* save = nullptr;
  char* tok = strtok_r(work, ",", &save); // WRITE
  if (!tok || strcmp(tok, "WRITE") != 0) {
    bleSendLine("ERR,BAD_COMMAND");
    return;
  }

  char* u = strtok_r(nullptr, ",", &save);
  char* a = strtok_r(nullptr, ",", &save);
  char* f = strtok_r(nullptr, ",", &save);
  char* p = strtok_r(nullptr, ",", &save);
  char* extra = strtok_r(nullptr, ",", &save);
  if (!u || !a || !f || !p || extra) {
    bleSendLine("ERR,BAD_FORMAT");
    return;
  }

  uint32_t uid=0;
  if(!parseUidHex(u,uid)) { bleSendLine("ERR,BAD_UID"); return; }
  const int addr = atoi(a);
  const float mhz = atof(f);
  const int power = atoi(p);
  const uint32_t khz = (uint32_t)(mhz * 1000.0f + 0.5f);

  if (addr < 2 || addr > 55) { bleSendLine("ERR,BAD_ADDR"); return; }
  if (khz < 850000UL || khz > 950000UL) { bleSendLine("ERR,BAD_FREQ"); return; }
  if (power < 5 || power > 20) { bleSendLine("ERR,BAD_POWER"); return; }

  SetupResultPacket result{};
  if (!writeTarget(uid,(uint8_t)addr,khz,(int8_t)power,result)) {
    bleSendLine("ERR,TARGET_TIMEOUT");
    return;
  }
  if (result.status != SETUP_STATUS_OK) {
    bleSendLine(String("ERR,") + statusToken(result.status));
    return;
  }

  char uidText[12]; uidHex(result.uid,uidText,sizeof(uidText));
  char buf[150];
  snprintf(
    buf, sizeof(buf),
    "RESULT,OK,%s,%u,%.3f,%d",
    uidText,
    (unsigned)result.address,
    result.freq_khz / 1000.0f,
    (int)result.tx_power
  );
  bleSendLine(String(buf));
}

static void processCommand(const char* raw) {
  String s(raw);
  s.trim();
  if (!s.length()) return;
  Serial.println("[BLE RX] " + s);

  if (s == "HELLO" || s == "PING") {
    handleHello();
  } else if (s == "SCAN") {
    handleScan();
  } else if (s.startsWith("IDENTIFY,")) {
    handleIdentifyCommand(s.c_str());
  } else if (s.startsWith("WRITE,")) {
    handleWriteCommand(s.c_str());
  } else {
    bleSendLine("ERR,UNKNOWN_COMMAND");
  }
}

static void serviceBleRx() {
  uint8_t c;
  while (bleRxPop(c)) {
    if (c == '\r') continue;
    if (c == '\n') {
      cmdLine[cmdPos] = 0;
      if (cmdPos) processCommand(cmdLine);
      cmdPos = 0;
      continue;
    }
    if (cmdPos < sizeof(cmdLine) - 1) {
      cmdLine[cmdPos++] = (char)c;
    } else {
      cmdPos = 0;
      bleSendLine("ERR,LINE_TOO_LONG");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  radioReady = initRadio();
  Serial.print("[RADIO] setup freq=");
  Serial.print(SETUP_FREQ_MHZ, 3);
  Serial.print(" MHz ready=");
  Serial.println(radioReady ? "YES" : "NO");

  const bool bleReady = initBle();
  Serial.print("[BLE] ready=");
  Serial.println(bleReady ? "YES" : "NO");
  Serial.println("[READY] Open the integrated loraMesh HTML and connect from Node/Router Production Setup.");
}

void loop() {
  serviceBleRx();
  delay(2);
}
