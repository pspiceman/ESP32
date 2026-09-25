// LoRaMesh Gateway FINAL firmware (v2 baseline, final reviewed build).
// Final addition: Active-HIGH gateway activity LED pulse on LoRa E2E success / RESET TX success.
// Baseline: original GW5(9).ino LoRa/WiFi/MQTT protocol behavior.
// Added: ESP32/ESP32-C3 shared BLE-NUS CONFIG transport via BOOT key.
// Added: upstream network gate so LoRa POLL runs only with WiFi + MQTT online.
// FINAL: Normal boot never initializes BLE. BLE RUN saves settings and reboots cleanly.
// LoRa polling starts only when BOTH WiFi and MQTT are connected.
// Gateway=1 / Sensor=2~49 / Router=50~55 / MaxHop 3 / RouteTable 64.
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Preferences.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// RadioHead route-table capacity for large Node deployments.
// Must be defined BEFORE RHMesh/RHRouter headers are included.
#ifndef RH_ROUTING_TABLE_SIZE
#define RH_ROUTING_TABLE_SIZE 64
#endif
#include <string.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// ===== Board Select =====
#define BOARD_ESP32C3 0
#define BOARD_ESP32   1

#if (BOARD_ESP32C3 + BOARD_ESP32) != 1
#error "BOARD_ESP32C3 또는 BOARD_ESP32 중 하나만 1로 설정하세요."
#endif

#if BOARD_ESP32C3
const int LED_PIN    = 0;                      // combo = 0 
static const uint8_t PIN_LORA_SS   = 7;
static const uint8_t PIN_LORA_RST  = 2;
static const uint8_t PIN_LORA_DIO0 = 3;
static const int PIN_VSPI_SCK  = 4;
static const int PIN_VSPI_MISO = 5;
static const int PIN_VSPI_MOSI = 6;
static const uint8_t GW_CONFIG_BOOT_GPIO = 9;   // ESP32-C3 BOOT button, active LOW
#elif BOARD_ESP32
const int LED_PIN    = 15;
static const uint8_t PIN_LORA_SS   = 5;
static const uint8_t PIN_LORA_RST  = 17;
static const uint8_t PIN_LORA_DIO0 = 16;
static const int PIN_VSPI_SCK  = 18;
static const int PIN_VSPI_MISO = 19;
static const int PIN_VSPI_MOSI = 23;
static const uint8_t GW_CONFIG_BOOT_GPIO = 0;   // ESP32 BOOT button, active LOW
#endif

// ===== Gateway activity LED =====
// Active HIGH: ESP32-C3 GPIO10 / ESP32-WROOM GPIO15.
// Pulse only after a valid end-to-end LoRa reply or a successful RESET TX.
static const uint32_t GW_ACTIVITY_LED_PULSE_MS=100UL;
static bool gwActivityLedOn=false;
static uint32_t gwActivityLedOffAt=0;

// ===== WiFi =====
// Default values are used when NVS has no saved WiFi configuration.
static const char* DEFAULT_WIFI_SSID = "Backhome";
static const char* DEFAULT_WIFI_PASSWORD = "1700note";
static const size_t WIFI_SSID_MAX_LEN = 32;
static const size_t WIFI_PASSWORD_MAX_LEN = 63;
static char wifiSsid[WIFI_SSID_MAX_LEN + 1] = {0};
static char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1] = {0};
static bool wifiConfigLoadedFromNvs = false;
static bool wifiConfigDirty = false;

// ===== MQTT =====
const char*    MQTT_BROKER = "broker.hivemq.com";
const uint16_t MQTT_PORT   = 1883;

static const char* TOPIC_LED_SUB      = "tswell/lora/node/+/led";
static const char* TOPIC_TELEM_FMT    = "tswell/lora/node/%u/telemetry";
static const char* TOPIC_LEDSTATE_FMT = "tswell/lora/node/%u/led_state";
static const char* TOPIC_GW_CMD       = "tswell/lora/gateway/cmd";
static const char* TOPIC_WIFI_RSSI    = "tswell/lora/gateway/wifi_rssi";


// =====================================================
// USER CONFIG (NVS Preferences + Serial)
// =====================================================
// 1 Group fixed architecture:
//   Gateway=1 / Sensor Node=2~49(max 48) / Router=50~55(max 6)
//   Max Hop=3 / Routing Table=64
// Serial Monitor: 115200 baud. RESET 후 5초 안에 C 또는 c 입력 -> CONFIG MODE
//   SHOW
//   SET PROFILE 1       // Node 2
//   SET PROFILE 10      // Node 2~11
//   SET PROFILE 48      // Node 2~49
//   // PROFILE은 1~48 아무 숫자나 사용 가능
//   SET NODES 2,7,15    // manual Sensor list
//   SET ROUTERS 50,51,52
//   SET FREQ 922.0
//   SET POWER 13
//   SET ID Backhome
//   SET PW 1700note
//   SHOW WIFI
//   RUN                // Gateway 설정 변경값 저장 후 정상 Gateway 시작
//   // WiFi의 SET ID / SET PW / SET DEFAULT는 입력 즉시 NVS 저장
//   // SAVE는 하위 호환용 선택 명령(필수 아님)
#define GW_ADDR 1

static const uint8_t MAX_SENSOR_NODES=48;
static const uint8_t MAX_ROUTERS=6;
static const uint8_t MAX_DEVICES=MAX_SENSOR_NODES+MAX_ROUTERS;
static const uint32_t GW_CFG_MAGIC=0x47574346UL; // 'GWCF'
static const uint8_t GW_CFG_VERSION=1;
static const uint32_t CONFIG_WINDOW_MS=5000UL;

#pragma pack(push,1)
struct GatewayConfig {
  uint32_t magic;
  uint8_t version;
  uint8_t profile;       // 0=manual, 3,10,48=profile
  uint8_t sensorCount;
  uint8_t sensorNodes[MAX_SENSOR_NODES];
  uint8_t routerCount;
  uint8_t routerNodes[MAX_ROUTERS];
  uint32_t freq_khz;
  int8_t tx_power;
  uint16_t crc;
};
#pragma pack(pop)

// Explicit prototypes keep Arduino's .ino auto-prototype generator from
// placing GatewayConfig-dependent declarations before the struct definition.
static uint16_t calcGatewayConfigCrc(const GatewayConfig &c);
static bool validGatewayConfig(const GatewayConfig &c);

static GatewayConfig gwCfg;
static bool configLoadedFromNvs=false;
static bool configDirty=false; // SET 명령이 들어오면 true, RUN 시 1회 자동 저장

static void serviceGatewayActivityLed() {
  if(gwActivityLedOn && (int32_t)(millis()-gwActivityLedOffAt)>=0) {
    digitalWrite(LED_PIN,LOW);
    gwActivityLedOn=false;
  }
}
static void pulseGatewayActivityLed() {
  digitalWrite(LED_PIN,HIGH);
  gwActivityLedOn=true;
  gwActivityLedOffAt=millis()+GW_ACTIVITY_LED_PULSE_MS;
}

static void setDefaultWiFiConfig() {
  strlcpy(wifiSsid, DEFAULT_WIFI_SSID, sizeof(wifiSsid));
  strlcpy(wifiPassword, DEFAULT_WIFI_PASSWORD, sizeof(wifiPassword));
  wifiConfigLoadedFromNvs=false;
  wifiConfigDirty=false;
}
static void loadWiFiConfig() {
  setDefaultWiFiConfig();
  Preferences prefs;
  if(!prefs.begin("lora-gw", true)) return;
  String ssid=prefs.getString("wifi_ssid", "");
  String pw=prefs.getString("wifi_pw", "");
  prefs.end();
  if(ssid.length()>0 && ssid.length()<=WIFI_SSID_MAX_LEN && pw.length()<=WIFI_PASSWORD_MAX_LEN) {
    strlcpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid));
    strlcpy(wifiPassword, pw.c_str(), sizeof(wifiPassword));
    wifiConfigLoadedFromNvs=true;
  }
  wifiConfigDirty=false;
}
static bool saveWiFiConfig() {
  // WiFi SET changes are committed immediately.  After writing, read the
  // values back from NVS so BT/USB CONFIG never reports success for a value
  // that was not actually persisted.
  const size_t ssidLen=strlen(wifiSsid);
  const size_t pwLen=strlen(wifiPassword);
  if(ssidLen<1 || ssidLen>WIFI_SSID_MAX_LEN || pwLen>WIFI_PASSWORD_MAX_LEN) return false;

  Preferences prefs;
  if(!prefs.begin("lora-gw", false)) return false;
  const size_t n1=prefs.putString("wifi_ssid", wifiSsid);
  const size_t n2=prefs.putString("wifi_pw", wifiPassword);
  prefs.end();
  if(n1==0 || (pwLen>0 && n2==0)) return false;

  Preferences verify;
  if(!verify.begin("lora-gw", true)) return false;
  const String savedSsid=verify.getString("wifi_ssid", "");
  const String savedPw=verify.getString("wifi_pw", "");
  verify.end();

  const bool ok=(savedSsid==wifiSsid && savedPw==wifiPassword);
  if(ok) {
    wifiConfigLoadedFromNvs=true;
    wifiConfigDirty=false;
  }
  return ok;
}
// ===== Optional BLE CONFIG transport (used ONLY when BOOT is pressed) =====
// Normal Gateway boot never initializes BLE. BLE CONFIG uses Nordic UART Service
// so the same Web Bluetooth page can be used with ESP32 and ESP32-C3.
static const char* BLE_NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* BLE_NUS_RX_UUID      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // Browser/phone -> GW
static const char* BLE_NUS_TX_UUID      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // GW -> Browser/phone
static const uint32_t BLE_BOOT_DEBOUNCE_MS = 35UL;
static const size_t BLE_RX_RING_SIZE = 512;
static const size_t BLE_NOTIFY_CHUNK = 20; // safe with default ATT MTU
static const uint32_t BLE_NOTIFY_GAP_MS = 2UL;

enum ConfigTransport : uint8_t { CFG_TRANSPORT_USB=0, CFG_TRANSPORT_BLE=1 };
static ConfigTransport cfgTransport=CFG_TRANSPORT_USB;
static BLEServer* bleCfgServer=nullptr;
static BLECharacteristic* bleCfgTx=nullptr;
static volatile bool bleCfgConnected=false;
static uint8_t bleRxRing[BLE_RX_RING_SIZE];
static volatile size_t bleRxHead=0,bleRxTail=0;
static portMUX_TYPE bleRxMux=portMUX_INITIALIZER_UNLOCKED;

static void bleRxReset() {
  portENTER_CRITICAL(&bleRxMux);
  bleRxHead=bleRxTail=0;
  portEXIT_CRITICAL(&bleRxMux);
}
static void bleRxPush(uint8_t c) {
  portENTER_CRITICAL(&bleRxMux);
  const size_t next=(bleRxHead+1U)%BLE_RX_RING_SIZE;
  if(next!=bleRxTail) {
    bleRxRing[bleRxHead]=c;
    bleRxHead=next;
  }
  portEXIT_CRITICAL(&bleRxMux);
}
static int bleRxAvailable() {
  portENTER_CRITICAL(&bleRxMux);
  const size_t head=bleRxHead,tail=bleRxTail;
  portEXIT_CRITICAL(&bleRxMux);
  return (int)((head+BLE_RX_RING_SIZE-tail)%BLE_RX_RING_SIZE);
}
static int bleRxRead() {
  int v=-1;
  portENTER_CRITICAL(&bleRxMux);
  if(bleRxTail!=bleRxHead) {
    v=bleRxRing[bleRxTail];
    bleRxTail=(bleRxTail+1U)%BLE_RX_RING_SIZE;
  }
  portEXIT_CRITICAL(&bleRxMux);
  return v;
}
static void bleCfgSend(const uint8_t* data,size_t len) {
  if(cfgTransport!=CFG_TRANSPORT_BLE || !bleCfgConnected || !bleCfgTx || !data || !len) return;
  while(len) {
    const size_t n=(len>BLE_NOTIFY_CHUNK)?BLE_NOTIFY_CHUNK:len;
    bleCfgTx->setValue(data,n);
    bleCfgTx->notify();
    data+=n;
    len-=n;
    if(len) delay(BLE_NOTIFY_GAP_MS);
  }
}

// Config-only console. USB CONFIG behavior remains the same; in BLE CONFIG the
// same output is mirrored to BLE while still being visible on USB Serial.
class ConfigConsole : public Print {
public:
  size_t write(uint8_t c) override { return write(&c,1); }
  size_t write(const uint8_t* buffer,size_t size) override {
    if(!buffer || !size) return 0;
    Serial.write(buffer,size);
    bleCfgSend(buffer,size);
    return size;
  }
  int available() {
    return cfgTransport==CFG_TRANSPORT_BLE ? bleRxAvailable() : Serial.available();
  }
  int read() {
    return cfgTransport==CFG_TRANSPORT_BLE ? bleRxRead() : Serial.read();
  }
  size_t printf(const char* fmt,...) {
    char buf[320];
    va_list ap;
    va_start(ap,fmt);
    const int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if(n<=0) return 0;
    const size_t sendLen=(size_t)n<sizeof(buf)?(size_t)n:sizeof(buf)-1;
    return write((const uint8_t*)buf,sendLen);
  }
};
static ConfigConsole cfgIo;

class BleConfigServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    (void)pServer;
    bleCfgConnected=true;
  }
  void onDisconnect(BLEServer* pServer) override {
    bleCfgConnected=false;
    if(pServer) pServer->startAdvertising();
  }
};
class BleConfigRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    if(!pCharacteristic) return;
    String value=pCharacteristic->getValue();
    for(size_t i=0;i<value.length();i++) bleRxPush((uint8_t)value[i]);
  }
};
static BleConfigServerCallbacks bleCfgServerCallbacks;
static BleConfigRxCallbacks bleCfgRxCallbacks;

static String makeBleConfigDeviceName() {
  const uint16_t suffix=(uint16_t)(ESP.getEfuseMac() & 0xFFFFU);
  char name[32];
#if BOARD_ESP32C3
  snprintf(name,sizeof(name),"LoRaMesh-GW-C3-%04X",suffix);
#else
  snprintf(name,sizeof(name),"LoRaMesh-GW-ESP32-%04X",suffix);
#endif
  return String(name);
}
static bool startBleConfigTransport() {
  cfgTransport=CFG_TRANSPORT_BLE;
  bleRxReset();
  bleCfgConnected=false;

  const String devName=makeBleConfigDeviceName();
  if(!BLEDevice::init(devName)) {
    Serial.println("[BT] BLE init failed");
    cfgTransport=CFG_TRANSPORT_USB;
    return false;
  }

  bleCfgServer=BLEDevice::createServer();
  if(!bleCfgServer) {
    Serial.println("[BT] BLE server create failed");
    return false;
  }
  bleCfgServer->setCallbacks(&bleCfgServerCallbacks);

  BLEService* service=bleCfgServer->createService(BLE_NUS_SERVICE_UUID);
  if(!service) {
    Serial.println("[BT] BLE service create failed");
    return false;
  }

  bleCfgTx=service->createCharacteristic(BLE_NUS_TX_UUID,BLECharacteristic::PROPERTY_NOTIFY);
  BLECharacteristic* rx=service->createCharacteristic(
    BLE_NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  if(!bleCfgTx || !rx) {
    Serial.println("[BT] BLE characteristic create failed");
    return false;
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
  // Bluedroid requires an explicit CCCD for notifications.
  bleCfgTx->addDescriptor(new BLE2902());
#endif
  // NimBLE creates the 0x2902 CCCD automatically for NOTIFY characteristics.
  rx->setCallbacks(&bleCfgRxCallbacks);
  service->start();

  BLEAdvertising* advertising=bleCfgServer->getAdvertising();
  advertising->addServiceUUID(BLE_NUS_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.printf("[BT] BLE CONFIG advertising: %s\n",devName.c_str());
  Serial.println("[BT] Use LoRaMesh Gateway web page or Nordic-UART compatible terminal.");
  Serial.println("[BT] RUN saves GW changes, then reboots into a clean NORMAL MODE.");
  return true;
}

static void printWiFiConfig() {
  cfgIo.println("--- WIFI CONFIG ---");
  cfgIo.printf("SSID    : %s\n", wifiSsid);
  cfgIo.printf("PASSWORD: %s\n", wifiPassword); // field setup convenience; remove/mask if needed
  if(wifiConfigDirty) cfgIo.println("SOURCE  : RAM (modified; RUN auto-saves)");
  else cfgIo.printf("SOURCE  : %s\n", wifiConfigLoadedFromNvs?"NVS":"DEFAULT (not saved)");
}
static char* trimValue(char* s) {
  while(*s==' ' || *s=='\t') ++s;
  size_t n=strlen(s);
  while(n>0 && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r' || s[n-1]=='\n')) s[--n]=0;
  return s;
}

static uint16_t configCrc16(const uint8_t* data,size_t len) {
  uint16_t crc=0xFFFF;
  while(len--) {
    crc^=*data++;
    for(uint8_t i=0;i<8;i++) crc=(crc&1)?(uint16_t)((crc>>1)^0xA001):(uint16_t)(crc>>1);
  }
  return crc;
}
static uint16_t calcGatewayConfigCrc(const GatewayConfig &c) {
  return configCrc16((const uint8_t*)&c,sizeof(GatewayConfig)-sizeof(c.crc));
}
static bool listHasDuplicate(const uint8_t* list,uint8_t count) {
  for(uint8_t i=0;i<count;i++) for(uint8_t j=i+1;j<count;j++) if(list[i]==list[j]) return true;
  return false;
}
static bool validGatewayConfig(const GatewayConfig &c) {
  if(c.magic!=GW_CFG_MAGIC || c.version!=GW_CFG_VERSION) return false;
  if(c.profile>MAX_SENSOR_NODES) return false; // 0=manual, 1~48=sequential profile
  if(c.sensorCount<1 || c.sensorCount>MAX_SENSOR_NODES) return false;
  if(c.routerCount<1 || c.routerCount>MAX_ROUTERS) return false;
  for(uint8_t i=0;i<c.sensorCount;i++) if(c.sensorNodes[i]<2 || c.sensorNodes[i]>49) return false;
  for(uint8_t i=0;i<c.routerCount;i++) if(c.routerNodes[i]<50 || c.routerNodes[i]>55) return false;
  if(listHasDuplicate(c.sensorNodes,c.sensorCount) || listHasDuplicate(c.routerNodes,c.routerCount)) return false;
  // PROFILE 1~48은 Node 2부터 연속 주소를 자동 구성합니다.
  if(c.profile>0) {
    if(c.sensorCount!=c.profile) return false;
    for(uint8_t i=0;i<c.profile;i++) if(c.sensorNodes[i]!=(uint8_t)(2+i)) return false;
  }
  if(c.freq_khz<850000UL || c.freq_khz>950000UL) return false;
  if(c.tx_power<5 || c.tx_power>20) return false;
  return c.crc==calcGatewayConfigCrc(c);
}
static void applyProfile(uint8_t profile) {
  if(profile<1 || profile>MAX_SENSOR_NODES) return;
  gwCfg.profile=profile;
  gwCfg.sensorCount=profile;
  for(uint8_t i=0;i<profile;i++) gwCfg.sensorNodes[i]=(uint8_t)(2+i);
}
static void setDefaultGatewayConfig() {
  memset(&gwCfg,0,sizeof(gwCfg));
  gwCfg.magic=GW_CFG_MAGIC;
  gwCfg.version=GW_CFG_VERSION;
  applyProfile(3);
  gwCfg.routerCount=1;
  gwCfg.routerNodes[0]=50;
  gwCfg.freq_khz=922000UL;
  gwCfg.tx_power=13;
  gwCfg.crc=calcGatewayConfigCrc(gwCfg);
}
static void loadGatewayConfig() {
  Preferences prefs;
  bool opened=prefs.begin("lora-gw",true);
  if(opened) {
    const size_t len=prefs.getBytesLength("config");
    if(len==sizeof(gwCfg)) prefs.getBytes("config",&gwCfg,sizeof(gwCfg));
    prefs.end();
  }
  configLoadedFromNvs=opened && validGatewayConfig(gwCfg);
  if(!configLoadedFromNvs) setDefaultGatewayConfig();
  configDirty=false;
}
static bool saveGatewayConfig() {
  gwCfg.magic=GW_CFG_MAGIC;
  gwCfg.version=GW_CFG_VERSION;
  gwCfg.crc=calcGatewayConfigCrc(gwCfg);
  if(!validGatewayConfig(gwCfg)) return false;
  Preferences prefs;
  if(!prefs.begin("lora-gw",false)) return false;
  const size_t n=prefs.putBytes("config",&gwCfg,sizeof(gwCfg));
  prefs.end();
  configLoadedFromNvs=(n==sizeof(gwCfg));
  if(configLoadedFromNvs) configDirty=false;
  return configLoadedFromNvs;
}
static float gatewayFrequencyMHz() { return gwCfg.freq_khz/1000.0f; }
static int8_t gatewayTxPower() { return gwCfg.tx_power; }
static uint8_t deviceCount() { return (uint8_t)(gwCfg.sensorCount+gwCfg.routerCount); }

static void printNodeList(const uint8_t* list,uint8_t count) {
  for(uint8_t i=0;i<count;i++) { if(i) cfgIo.print(','); cfgIo.print(list[i]); }
  cfgIo.println();
}
static void printGatewayConfig() {
  cfgIo.println("--- GW CONFIG ---");
  cfgIo.printf("PROFILE : %u%s\n",gwCfg.profile,gwCfg.profile?"":" (MANUAL)");
  cfgIo.printf("SENSORS : %u -> ",gwCfg.sensorCount); printNodeList(gwCfg.sensorNodes,gwCfg.sensorCount);
  cfgIo.printf("ROUTERS : %u -> ",gwCfg.routerCount); printNodeList(gwCfg.routerNodes,gwCfg.routerCount);
  cfgIo.printf("FREQ    : %.3f MHz\n",gatewayFrequencyMHz());
  cfgIo.printf("POWER   : %d dBm\n",gatewayTxPower());
  cfgIo.printf("FIXED   : GW=1 / MaxHop=3 / RouteTable=64\n");
  if(configDirty) cfgIo.println("SOURCE  : RAM (modified; RUN auto-saves)");
  else cfgIo.printf("SOURCE  : %s\n",configLoadedFromNvs?"NVS":"DEFAULT (not saved)");
  printWiFiConfig();
}
static void printGatewayConfigHelp() {
  cfgIo.println("[CONFIG MODE]");
  cfgIo.println(" SHOW | SHOW WIFI");
  cfgIo.println(" SET PROFILE 1~48 | SET NODES 2,7,15 | SET ROUTERS 50,51");
  cfgIo.println(" SET FREQ 922.0 | SET POWER 13");
  cfgIo.println(" SET ID <ssid>");
  cfgIo.println(" SET PW <password>");
  cfgIo.println(" SET DEFAULT");
  cfgIo.println(" RUN");
  cfgIo.println("WiFi SET ID/PW/DEFAULT saves immediately to NVS.");
  cfgIo.println("Other GW SET changes are saved when RUN is entered.");
}
static void upperLine(char* s) { while(*s) { *s=(char)toupper((unsigned char)*s); ++s; } }
static bool configModeExitRequested=false;
static bool parseCsvList(const char* text,uint8_t minId,uint8_t maxId,uint8_t maxCount,uint8_t* out,uint8_t &outCount) {
  char tmp[192];
  if(strlen(text)>=sizeof(tmp)) return false;
  strcpy(tmp,text);
  uint8_t count=0;
  char* tok=strtok(tmp,",");
  while(tok) {
    while(*tok==' ' || *tok=='\t') ++tok;
    const int v=atoi(tok);
    if(v<minId || v>maxId || count>=maxCount) return false;
    for(uint8_t i=0;i<count;i++) if(out[i]==v) return false;
    out[count++]=(uint8_t)v;
    tok=strtok(NULL,",");
  }
  if(count==0) return false;
  outCount=count;
  return true;
}
static void handleGatewayConfigCommand(char* line) {
  char* raw=trimValue(line);
  if(!*raw) return;

  // Preserve original case for WiFi SSID/password. Use an uppercase copy only for command matching.
  char cmd[192];
  strlcpy(cmd, raw, sizeof(cmd));
  upperLine(cmd);

  if(strcmp(cmd,"SHOW")==0) { printGatewayConfig(); return; }
  if(strcmp(cmd,"SHOW WIFI")==0) { printWiFiConfig(); return; }
  if(strcmp(cmd,"VERSION")==0) {
    cfgIo.println("--- VERSION ---");
    cfgIo.println("FW : Ver1.0");
#if BOARD_ESP32C3
    cfgIo.println("BOARD : ESP32-C3");
#else
    cfgIo.println("BOARD : ESP32-WROOM");
#endif
    return;
  }
  if(strcmp(cmd,"HELP")==0 || strcmp(cmd,"?")==0) { printGatewayConfigHelp(); return; }
  if(strcmp(cmd,"RUN")==0 || strcmp(cmd,"EXIT")==0) {
    if(configDirty) {
      gwCfg.magic=GW_CFG_MAGIC; gwCfg.version=GW_CFG_VERSION; gwCfg.crc=calcGatewayConfigCrc(gwCfg);
      if(!saveGatewayConfig()) {
        cfgIo.println("ERR: GW NVS auto-save failed / config invalid; not starting");
        return;
      }
      cfgIo.println("OK: GW NVS auto-saved");
    } else {
      cfgIo.println("OK: no GW config change; NVS write skipped");
    }
    if(wifiConfigDirty) {
      if(!saveWiFiConfig()) {
        cfgIo.println("ERR: WiFi NVS auto-save failed; not starting");
        return;
      }
      cfgIo.println("OK: WiFi NVS auto-saved");
    } else {
      cfgIo.println("OK: no WiFi config change; NVS write skipped");
    }
    configModeExitRequested=true;
    if(cfgTransport==CFG_TRANSPORT_BLE) cfgIo.println("OK: CONFIG MODE exit -> reboot");
    else cfgIo.println("OK: CONFIG MODE exit -> Gateway start");
    return;
  }
  // Optional/backward-compatible command. Normal field procedure is SET -> RUN.
  if(strcmp(cmd,"SAVE")==0) {
    gwCfg.magic=GW_CFG_MAGIC; gwCfg.version=GW_CFG_VERSION; gwCfg.crc=calcGatewayConfigCrc(gwCfg);
    const bool gwOk=saveGatewayConfig();
    const bool wifiOk=saveWiFiConfig();
    cfgIo.println((gwOk && wifiOk)?"OK: GW + WiFi NVS saved (optional command)":"ERR: NVS save failed / config invalid");
    return;
  }

  // WiFi commands: preserve the user's original case and save immediately to NVS.
  // "SET ID " and "SET PW " are both 7 characters including the trailing space.
  if(strncmp(cmd,"SET ID ",7)==0) {
    char* value=trimValue(raw+7);
    const size_t n=strlen(value);
    if(n<1 || n>WIFI_SSID_MAX_LEN) {
      cfgIo.println("ERR: WIFI SSID length 1~32");
      return;
    }

    char oldSsid[sizeof(wifiSsid)];
    strlcpy(oldSsid,wifiSsid,sizeof(oldSsid));

    strlcpy(wifiSsid,value,sizeof(wifiSsid));
    wifiConfigDirty=true;

    if(saveWiFiConfig()) {
      cfgIo.printf("OK: WIFI SSID=%s (saved + NVS verified)\n",wifiSsid);
      printWiFiConfig();
    } else {
      strlcpy(wifiSsid,oldSsid,sizeof(wifiSsid));
      wifiConfigDirty=false;
      cfgIo.println("ERR: WIFI SSID NVS save failed");
    }
    return;
  }

  if(strncmp(cmd,"SET PW ",7)==0) {
    char* value=trimValue(raw+7);
    const size_t n=strlen(value);
    if(n>WIFI_PASSWORD_MAX_LEN) {
      cfgIo.println("ERR: WIFI PW length 0~63");
      return;
    }

    char oldPw[sizeof(wifiPassword)];
    strlcpy(oldPw,wifiPassword,sizeof(oldPw));

    strlcpy(wifiPassword,value,sizeof(wifiPassword));
    wifiConfigDirty=true;

    if(saveWiFiConfig()) {
      cfgIo.println("OK: WIFI PW saved + NVS verified");
      printWiFiConfig();
    } else {
      strlcpy(wifiPassword,oldPw,sizeof(wifiPassword));
      wifiConfigDirty=false;
      cfgIo.println("ERR: WIFI PW NVS save failed");
    }
    return;
  }

  if(strcmp(cmd,"SET DEFAULT")==0) {
    char oldSsid[sizeof(wifiSsid)];
    char oldPw[sizeof(wifiPassword)];
    strlcpy(oldSsid,wifiSsid,sizeof(oldSsid));
    strlcpy(oldPw,wifiPassword,sizeof(oldPw));

    strlcpy(wifiSsid,DEFAULT_WIFI_SSID,sizeof(wifiSsid));
    strlcpy(wifiPassword,DEFAULT_WIFI_PASSWORD,sizeof(wifiPassword));
    wifiConfigDirty=true;

    if(saveWiFiConfig()) {
      cfgIo.println("OK: WIFI defaults saved + NVS verified");
      printWiFiConfig();
    } else {
      strlcpy(wifiSsid,oldSsid,sizeof(wifiSsid));
      strlcpy(wifiPassword,oldPw,sizeof(wifiPassword));
      wifiConfigDirty=false;
      cfgIo.println("ERR: WIFI DEFAULT NVS save failed");
    }
    return;
  }

  if(strncmp(cmd,"SET PROFILE ",12)==0) {
    const int v=atoi(cmd+12);
    if(v<1 || v>MAX_SENSOR_NODES) { cfgIo.println("ERR: PROFILE 1~48"); return; }
    applyProfile((uint8_t)v); gwCfg.crc=calcGatewayConfigCrc(gwCfg); configDirty=true;
    if(v==1) cfgIo.println("OK: PROFILE=1, Sensor 2");
    else cfgIo.printf("OK: PROFILE=%d, Sensor 2~%d\n",v,v+1);
    return;
  }
  if(strncmp(cmd,"SET NODES ",10)==0) {
    uint8_t temp[MAX_SENSOR_NODES],count=0;
    if(!parseCsvList(cmd+10,2,49,MAX_SENSOR_NODES,temp,count)) { cfgIo.println("ERR: NODES must be unique 2~49"); return; }
    memcpy(gwCfg.sensorNodes,temp,count); gwCfg.sensorCount=count; gwCfg.profile=0; gwCfg.crc=calcGatewayConfigCrc(gwCfg); configDirty=true;
    cfgIo.printf("OK: MANUAL Sensors=%u\n",count); return;
  }
  if(strncmp(cmd,"SET ROUTERS ",12)==0) {
    uint8_t temp[MAX_ROUTERS],count=0;
    if(!parseCsvList(cmd+12,50,55,MAX_ROUTERS,temp,count)) { cfgIo.println("ERR: ROUTERS must be unique 50~55, max 6"); return; }
    memcpy(gwCfg.routerNodes,temp,count); gwCfg.routerCount=count; gwCfg.crc=calcGatewayConfigCrc(gwCfg); configDirty=true;
    cfgIo.printf("OK: Routers=%u\n",count); return;
  }
  if(strncmp(cmd,"SET FREQ ",9)==0) {
    const float mhz=(float)atof(cmd+9);
    const uint32_t khz=(uint32_t)(mhz*1000.0f+0.5f);
    if(khz<850000UL || khz>950000UL) { cfgIo.println("ERR: FREQ 850~950 MHz"); return; }
    gwCfg.freq_khz=khz; gwCfg.crc=calcGatewayConfigCrc(gwCfg); configDirty=true;
    cfgIo.printf("OK: FREQ=%.3f MHz\n",gatewayFrequencyMHz()); return;
  }
  if(strncmp(cmd,"SET POWER ",10)==0) {
    const int v=atoi(cmd+10);
    if(v<5 || v>20) { cfgIo.println("ERR: POWER 5~20 dBm"); return; }
    gwCfg.tx_power=(int8_t)v; gwCfg.crc=calcGatewayConfigCrc(gwCfg); configDirty=true;
    cfgIo.printf("OK: POWER=%d dBm\n",v); return;
  }
  cfgIo.println("ERR: unknown command. Type HELP");
}
static void runGatewayConfigWindow() {
  // Original USB CONFIG: C/c during the 5 second boot window.
  // Added BLE CONFIG: press the physical BOOT key during the same window.
  // IMPORTANT: BLE is NOT initialized at all on NORMAL MODE boots.
  Serial.println();
  Serial.printf("[BOOT] Within 5 sec: press C/c for USB CONFIG, or BOOT(GPIO %u) for BLE CONFIG.\n",
                GW_CONFIG_BOOT_GPIO);

  pinMode(GW_CONFIG_BOOT_GPIO,INPUT_PULLUP);
  const uint32_t entryStart=millis();
  uint32_t bootLowStart=0;
  bool enterUsbConfig=false;
  bool enterBleConfig=false;

  while((uint32_t)(millis()-entryStart)<CONFIG_WINDOW_MS) {
    while(Serial.available()) {
      const char c=(char)Serial.read();
      if(c=='C' || c=='c') { enterUsbConfig=true; break; }
    }
    if(enterUsbConfig) break;

    if(digitalRead(GW_CONFIG_BOOT_GPIO)==LOW) {
      if(bootLowStart==0) bootLowStart=millis();
      if((uint32_t)(millis()-bootLowStart)>=BLE_BOOT_DEBOUNCE_MS) {
        enterBleConfig=true;
        break;
      }
    } else {
      bootLowStart=0;
    }
    delay(1);
  }
  pinMode(GW_CONFIG_BOOT_GPIO,INPUT); // remove temporary pull-up after the entry window

  if(!enterUsbConfig && !enterBleConfig) {
    Serial.println("[BOOT] NORMAL MODE -> Gateway start");
    return;
  }

  if(enterBleConfig) {
    if(!startBleConfigTransport()) {
      // Never continue into WiFi after a partial BLE start failure.
      // A reboot restores the same clean startup boundary used after BLE RUN.
      Serial.println("[BT] BLE CONFIG start failed -> reboot");
      Serial.flush();
      delay(300);
      ESP.restart();
    }
  } else {
    cfgTransport=CFG_TRANSPORT_USB;
  }

  configModeExitRequested=false;
  cfgIo.println();
  cfgIo.println(enterBleConfig ? "=== BLE CONFIG MODE (NVS) ===" : "=== CONFIG MODE (NVS) ===");
  printGatewayConfig();
  printGatewayConfigHelp();
  cfgIo.println("WiFi SET ID/PW/DEFAULT saves immediately. Other SET changes save on RUN.");
  cfgIo.print("> ");

  char line[192]; size_t pos=0;
  while(!configModeExitRequested) {
    while(cfgIo.available()) {
      const int rv=cfgIo.read();
      if(rv<0) break;
      const char c=(char)rv;

      if(cfgTransport==CFG_TRANSPORT_USB) {
        // Preserve original USB Serial behavior exactly: CR ignored, LF executes.
        if(c=='\r') continue;
        if(c=='\n') {
          line[pos]=0;
          if(pos) handleGatewayConfigCommand(line);
          pos=0;
          if(!configModeExitRequested) cfgIo.print("> ");
        } else if(pos<sizeof(line)-1) {
          line[pos++]=c;
        }
      } else {
        // BLE terminals/web pages may use CR, LF, or CR+LF.
        if(c=='\r' || c=='\n') {
          line[pos]=0;
          if(pos) handleGatewayConfigCommand(line);
          pos=0;
          if(!configModeExitRequested) cfgIo.print("> ");
        } else if(pos<sizeof(line)-1) {
          line[pos++]=c;
        }
      }
    }
    delay(1);
  }

  printGatewayConfig();

  if(enterBleConfig) {
    // Do not transition BLE -> WiFi in the same boot session.
    // A full reboot guarantees the original Gateway starts from a clean radio state.
    cfgIo.println("[BT] CONFIG complete. Rebooting to NORMAL MODE...");
    delay(400);
    Serial.flush();
    ESP.restart();
  }
  // USB CONFIG intentionally preserves the original behavior: continue directly
  // into the original Gateway setup in the same boot session.
}
// Scheduling targets, NOT hard realtime deadlines: RadioHead discovery blocks.
static const uint32_t NODE_SLOT_MS=10000UL;
static const uint32_t ROUTER_SLOT_MS=8000UL;
static const uint32_t RESPONSE_WAIT_MS=3500UL;
static const uint32_t TX_START_RESERVE_MS=5000UL;
static const uint32_t ROUTER_POLL_INTERVAL_MS=60000UL;
static const uint8_t MAX_ATTEMPTS=2;

// Node는 다음 자기 Poll 전에 미리 깨어 있도록 약 14초 마진을 둡니다.
// Sleep 시간은 현재 설정된 Sensor Node 수에 따라 Gateway가 자동 계산합니다.
static const uint32_t SENSOR_WAKE_MARGIN_MS=14000UL;

// 참고: Web timeout은 대규모 구성(특히 48 Node)에서 별도 조정이 필요합니다.
static uint32_t nodeAliveMs() { return (uint32_t)gwCfg.sensorCount*NODE_SLOT_MS + 60000UL; }
static const uint32_t ROUTER_ALIVE_MS=180000UL;
enum : uint8_t { PKT_POLL=0x01, PKT_RESET=0x03, PKT_TELEM=0x81, PKT_SLEEP_GRANT=0x82 };
#pragma pack(push,1)
struct TelemetryPayload {uint16_t vbat_mV; int16_t t,h,vib; int16_t link_rssi; uint8_t led_state;};
#pragma pack(pop)
static_assert(sizeof(TelemetryPayload)==11,"Telemetry layout mismatch");
struct NodeState {
  TelemetryPayload p={};
  bool seen=false,dirty=false;
  uint32_t lastOkMs=0,rxCount=0,maxGapMs=0;
  uint16_t seq=0;
  int16_t rssi=-999;       // Node first-link RSSI for Web/MQTT
  int16_t gwLastRssi=-999; // Final RF hop RSSI seen at Gateway, serial debug only
  uint8_t hops=0,failCount=0;
};
struct LedPending {bool active=false; uint8_t target=0;};
NodeState state[MAX_DEVICES];
LedPending led[MAX_DEVICES];
RH_RF95 rf95(PIN_LORA_SS,PIN_LORA_DIO0);
RHMesh manager(rf95,GW_ADDR);
WiFiClient espClient;
PubSubClient mqtt(espClient);
static uint16_t nextSeq=0;
static int activeIdx=-1;
static uint16_t activeSeq=0;
static bool activeReceived=false;
static uint32_t lastRouterPollMs[MAX_ROUTERS]={};
static bool routerPolled[MAX_ROUTERS]={};
static uint32_t lastWifiAttemptMs=0,lastMqttAttemptMs=0,lastRssiMs=0;
static bool wifiAttempted=false,mqttAttempted=false;
// Web/MQTT reset request. "reset" resets Sensor Nodes -> Router -> GW.
// Kept as a queued action so a button press never reboots the GW in the middle
// of a LoRa transaction.
static bool resetAllRequested=false;
static bool resetGwOnlyRequested=false;

static int sensorIndexOf(uint8_t id) {
  for(uint8_t i=0;i<gwCfg.sensorCount;i++) if(gwCfg.sensorNodes[i]==id) return i;
  return -1;
}
static int routerIndexOf(uint8_t id) {
  for(uint8_t i=0;i<gwCfg.routerCount;i++) if(gwCfg.routerNodes[i]==id) return i;
  return -1;
}
static int idxOf(uint8_t id) {
  const int s=sensorIndexOf(id);
  if(s>=0) return s;
  const int r=routerIndexOf(id);
  if(r>=0) return gwCfg.sensorCount+r;
  return -1;
}
static uint8_t idAtIndex(uint8_t idx) {
  return (idx<gwCfg.sensorCount) ? gwCfg.sensorNodes[idx] : gwCfg.routerNodes[idx-gwCfg.sensorCount];
}
static bool isSensorId(uint8_t id) { return sensorIndexOf(id)>=0; }
static bool isRouterId(uint8_t id) { return routerIndexOf(id)>=0; }

static uint16_t sensorSleepGrantTicks() {
  if(gwCfg.sensorCount<3 || NODE_SLOT_MS<10000UL) return 0;
  const uint32_t sensorCycleMs=(uint32_t)gwCfg.sensorCount*NODE_SLOT_MS;
  if(sensorCycleMs<=SENSOR_WAKE_MARGIN_MS) return 0;
  uint32_t sleepMs=sensorCycleMs-SENSOR_WAKE_MARGIN_MS;
  // 3 Node 프로필은 기존 약 16초 Sleep 수준을 유지.
  if(sleepMs<16000UL) sleepMs=16000UL;
  uint32_t ticks=(sleepMs+124UL)/125UL;
  if(ticks>4096UL) ticks=4096UL;
  return (uint16_t)ticks;
}

static bool beforeDeadline(uint32_t now,uint32_t deadline) {
  return (int32_t)(deadline-now)>0;
}
static void mqttCb(char* topic,byte* payload,unsigned int length) {
  char msg[16];
  if(length>=sizeof(msg)) return;
  memcpy(msg,payload,length); msg[length]=0;
  if(strcmp(topic,TOPIC_GW_CMD)==0) {
    // Existing Web RESET button may keep publishing payload "reset".
    // It now means RESET ALL. "reset_all" is accepted as an alias.
    if(strcmp(msg,"reset")==0 || strcmp(msg,"reset_all")==0) {
      resetAllRequested=true;
      Serial.println("[RESET] ALL queued; execute at next cycle boundary");
    } else if(strcmp(msg,"reset_gw")==0) {
      resetGwOnlyRequested=true;
      Serial.println("[RESET] GW-only queued");
    }
    return;
  }
  if(strcmp(msg,"0")!=0 && strcmp(msg,"1")!=0) return;
  // LED 명령은 Sensor Node에만 적용. Router는 제외.
  for(uint8_t n=0;n<gwCfg.sensorCount;n++) {
    const uint8_t id=gwCfg.sensorNodes[n];
    char expected[64];
    snprintf(expected,sizeof(expected),"tswell/lora/node/%u/led",id);
    if(strcmp(topic,expected)!=0) continue;
    led[n].active=true; led[n].target=(msg[0]=='1');
    Serial.printf("[LED QUEUED] node=%u target=%u; next cycle priority\n",
                  id,led[n].target);
    break;
  }
}
static void receiveOnce() {
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN],len=sizeof(buf),from=0,hops=0;
  if(!manager.recvfromAck(buf,&len,&from,NULL,NULL,NULL,&hops)) return;
  if(len!=14 || buf[0]!=PKT_TELEM) return;
  const int i=idxOf(from);
  if(i<0) return;
  const uint16_t seq=(uint16_t)buf[12] | ((uint16_t)buf[13]<<8);
  // Only accept this target's outstanding transaction. Late old packets cannot
  // complete a different poll or refresh the web as if they were new samples.
  if(i!=activeIdx || seq!=activeSeq || activeReceived) {
    Serial.printf("[LATE/DUP] node=%u seq=%u active=%u\n",from,seq,activeSeq);
    return;
  }
  TelemetryPayload p;
  memcpy(&p,buf+1,sizeof(p));
  if(p.led_state>1) return;
  const uint32_t now=millis();
  NodeState &s=state[i];
  const uint32_t gap=s.seen ? (uint32_t)(now-s.lastOkMs) : 0;
  if(gap>s.maxGapMs) s.maxGapMs=gap;
  s.p=p; s.lastOkMs=now; s.seq=seq; s.seen=true; s.dirty=true;
  s.rxCount++; s.failCount=0;
  s.rssi=p.link_rssi;            // Web RSSI: Node<->GW direct, or Node<->nearest Router
  s.gwLastRssi=rf95.lastRssi();  // Debug only: final Router<->GW (or direct Node<->GW) hop
  s.hops=hops;
  activeReceived=true;
  pulseGatewayActivityLed();
  if(led[i].active && led[i].target==p.led_state) led[i].active=false;
  Serial.printf("[E2E OK] node=%u seq=%u hops=%u nodeLinkRssi=%d gwLastLinkRssi=%d gapMs=%lu maxGapMs=%lu\n",
    from,seq,hops,s.rssi,s.gwLastRssi,(unsigned long)gap,(unsigned long)s.maxGapMs);
}
static void listenFor(uint32_t ms,bool stopOnReply) {
  const uint32_t start=millis();
  while((uint32_t)(millis()-start)<ms) {
    serviceGatewayActivityLed();
    receiveOnce();
    if(stopOnReply && activeReceived) break;
    delay(1);
  }
  serviceGatewayActivityLed();
}
static void publishPending() {
  if(!mqtt.connected()) return;
  for(uint8_t i=0;i<deviceCount();i++) {
    NodeState &s=state[i];
    if(!s.dirty) continue;
    // Never replay an old queued sample as fresh after a network outage.
    if((uint32_t)(millis()-s.lastOkMs)>5000UL) {s.dirty=false; continue;}
    const uint8_t id=idAtIndex(i);
    char topic[64],payload[256],ledTopic[64],ledValue[2];
    snprintf(topic,sizeof(topic),TOPIC_TELEM_FMT,id);
    snprintf(payload,sizeof(payload),
      "{\"node\":%u,\"vbat\":%.3f,\"t\":%d,\"h\":%d,\"vib\":%d,\"led\":%u,\"rssi\":%d,\"seq\":%u,\"hops\":%u}",
      id,s.p.vbat_mV/1000.0f,s.p.t,s.p.h,s.p.vib,s.p.led_state,s.rssi,s.seq,s.hops);
    if(mqtt.publish(topic,payload,true)) {
      s.dirty=false;
      snprintf(ledTopic,sizeof(ledTopic),TOPIC_LEDSTATE_FMT,id);
      ledValue[0]=s.p.led_state?'1':'0'; ledValue[1]=0;
      mqtt.publish(ledTopic,ledValue,true);
    }
  }
}
// Called only outside a radio transaction: no MQTT/socket work between
// relayed POLL and the corresponding Telemetry.
static void serviceNetwork(bool allowConnect) {
  serviceGatewayActivityLed();
  uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED) {
    if(allowConnect && (!wifiAttempted || (uint32_t)(now-lastWifiAttemptMs)>=15000UL)) {
      wifiAttempted=true; lastWifiAttemptMs=now;
      WiFi.begin(wifiSsid,wifiPassword); // runtime config from NVS/Serial; no blocking wait loop
    }
    return;
  }
  if(!mqtt.connected()) {
    if(!allowConnect || (mqttAttempted && (uint32_t)(now-lastMqttAttemptMs)<15000UL)) return;
    mqttAttempted=true; lastMqttAttemptMs=now;
    String cid="tswell-gw-"+String((uint32_t)ESP.getEfuseMac(),HEX);
    if(mqtt.connect(cid.c_str())) {
      mqtt.subscribe(TOPIC_LED_SUB); mqtt.subscribe(TOPIC_GW_CMD);
      Serial.println("[MQTT] connected");
    }
  }
  if(!mqtt.connected()) return;
  mqtt.loop();
  publishPending();
  now=millis();
  if((uint32_t)(now-lastRssiMs)>=3000UL) {
    lastRssiMs=now;
    char value[16]; snprintf(value,sizeof(value),"%d",WiFi.RSSI());
    mqtt.publish(TOPIC_WIFI_RSSI,value,true);
  }
}

// LoRa polling is intentionally gated by the upstream path.  Sensor values are
// useful to the existing Web only when both WiFi and MQTT are online.  We never
// abort an in-progress radio transaction; the gate is checked between slots.
static bool networkReadyForPolling() {
  return WiFi.status()==WL_CONNECTED && mqtt.connected();
}
static bool pollGateWasReady=false;
static uint32_t lastPollGateLogMs=0;
static bool checkPollGate(bool logStatus=true) {
  serviceNetwork(true);
  const bool ready=networkReadyForPolling();
  const uint32_t now=millis();

  if(ready) {
    if(!pollGateWasReady) {
      Serial.printf("[NET READY] WiFi+MQTT online IP=%s -> LoRa polling enabled\n",
                    WiFi.localIP().toString().c_str());
    }
    pollGateWasReady=true;
    return true;
  }

  if(pollGateWasReady || (logStatus && (lastPollGateLogMs==0 || (uint32_t)(now-lastPollGateLogMs)>=5000UL))) {
    Serial.printf("[NET WAIT] WiFi=%s MQTT=%s -> LoRa polling paused\n",
                  WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE",
                  mqtt.connected()?"ONLINE":"OFFLINE");
    lastPollGateLogMs=now;
  }
  pollGateWasReady=false;
  return false;
}
// Send a reset packet in the device's NORMAL slot. Keeping the original slot
// timing is important because Sensor Nodes may still be sleeping from the last
// grant. Sensors are reset first; the Router is reset last so it can relay all
// Sensor reset packets.
static bool sendResetInSlot(uint8_t id,uint32_t slotMs) {
  const uint32_t start=millis(),deadline=start+slotMs;
  uint8_t cmd[2]={PKT_RESET,0xA5};
  bool ok=false;

  for(uint8_t attempt=0;attempt<MAX_ATTEMPTS;attempt++) {
    const uint32_t now=millis();
    if(!beforeDeadline(now,deadline) || (uint32_t)(deadline-now)<TX_START_RESERVE_MS) break;
    const uint8_t err=manager.sendtoWait(cmd,sizeof(cmd),id);
    Serial.printf("[RESET TX] node=%u try=%u nextHopErr=%u\n",id,attempt+1,err);
    if(err==RH_ROUTER_ERROR_NONE) { ok=true; break; }
    listenFor(200,false);
  }
  if(ok) pulseGatewayActivityLed();

  // Preserve the same slot cadence as normal polling so later sleeping Nodes
  // wake at the expected time.
  uint32_t lastService=millis();
  while(beforeDeadline(millis(),deadline)) {
    serviceGatewayActivityLed();
    receiveOnce();
    if((uint32_t)(millis()-lastService)>=250UL) {
      lastService=millis(); serviceNetwork(false);
    }
    delay(1);
  }
  return ok;
}

static void performResetAll() {
  resetAllRequested=false;
  Serial.println("[RESET ALL] Sensors -> Routers -> GW");

  // 1) Sensor Nodes first. Router가 살아 있어야 Reset 패킷 중계 가능.
  for(uint8_t n=0;n<gwCfg.sensorCount;n++) {
    sendResetInSlot(gwCfg.sensorNodes[n],NODE_SLOT_MS);
  }

  // 2) 사용 중인 Router를 마지막에 순차 Reset.
  for(uint8_t r=0;r<gwCfg.routerCount;r++) {
    sendResetInSlot(gwCfg.routerNodes[r],ROUTER_SLOT_MS);
  }

  // 3) Finally reboot the Gateway.
  Serial.println("[RESET ALL] reboot GW");
  Serial.flush();
  delay(300);
  ESP.restart();
}

static bool pollDevice(uint8_t id,uint32_t slotMs) {
  const int i=idxOf(id);
  if(i<0) return false;
  const uint32_t start=millis(),deadline=start+slotMs;
  activeIdx=i; activeSeq=++nextSeq; activeReceived=false;
  uint8_t req[4]={PKT_POLL,led[i].active?led[i].target:(uint8_t)0xFF,
                 (uint8_t)activeSeq,(uint8_t)(activeSeq>>8)};
  for(uint8_t attempt=0;attempt<MAX_ATTEMPTS;attempt++) {
    const uint32_t now=millis();
    if(!beforeDeadline(now,deadline) || (uint32_t)(deadline-now)<TX_START_RESERVE_MS) break;
    // Drain queued route failures/late telemetry before transmitting again.
    receiveOnce();
    if(activeReceived) break;
    const uint32_t txStart=millis();
    const uint8_t err=manager.sendtoWait(req,sizeof(req),id);
    Serial.printf("[POLL] node=%u seq=%u try=%u nextHopErr=%u txMs=%lu\n",
      id,activeSeq,attempt+1,err,(unsigned long)(millis()-txStart));
    // ALWAYS listen even when the first-hop ACK was lost. Do not divide the
    // end-to-end wait by a burst count. Do not issue concurrent transactions.
    listenFor(RESPONSE_WAIT_MS,true);
    if(activeReceived) break;
    listenFor(200,false);
  }
  if(!activeReceived) {
    // A response just after the retry loop still counts within the slot.
    while(beforeDeadline(millis(),deadline) && !activeReceived) {
      receiveOnce(); delay(1);
    }
  }
  const bool got=activeReceived;
  if(!got && state[i].failCount<255) state[i].failCount++;
  // After two unsuccessful slots, relearn this destination next round.
  // Do not repeatedly clear every route or reset the radio.
  if(!got && state[i].failCount>=2) manager.deleteRouteTo(id);
  // Sensor Node만 Sleep Grant 대상. Node 수가 늘면 다음 자기 Poll까지의
  // 대기시간도 늘어나므로 현재 Sensor Node 수 기준으로 Sleep 시간을 자동 계산.
  const uint16_t sleepTicks=sensorSleepGrantTicks();
  if(got && isSensorId(id) && sleepTicks>0 && manager.getRouteTo(id)) {
    listenFor(200,false); // Allow telemetry forwarding/ACK turnaround.
    uint8_t grant[5]={PKT_SLEEP_GRANT,(uint8_t)activeSeq,
                     (uint8_t)(activeSeq>>8),
                     (uint8_t)sleepTicks,(uint8_t)(sleepTicks>>8)};
    uint8_t err=manager.sendtoWait(grant,sizeof(grant),id);
    Serial.printf("[SLEEP GRANT] node=%u seq=%u sleep=%.1fs nextHopErr=%u\n",
                  id,activeSeq,sleepTicks*0.125f,err);
  }
  activeIdx=-1;
  serviceNetwork(false);
  // Preserve ~10-second normal slots on healthy links; no unbounded RESCUE.
  uint32_t lastService=millis();
  while(beforeDeadline(millis(),deadline)) {
    serviceGatewayActivityLed();
    receiveOnce();
    if((uint32_t)(millis()-lastService)>=250UL) {
      lastService=millis(); serviceNetwork(false);
    }
    delay(1);
  }
  Serial.printf("[SLOT] node=%u ok=%u elapsedMs=%lu fails=%u\n",
    id,got,(unsigned long)(millis()-start),state[i].failCount);
  return got;
}
void setup() {
  Serial.begin(115200);
  loadGatewayConfig();
  loadWiFiConfig();
  runGatewayConfigWindow();
  if(!validGatewayConfig(gwCfg)) {
    Serial.println("[FATAL] GW CONFIG invalid");
    for(;;) delay(1000);
  }
  Serial.printf("[CONFIG] profile=%u sensors=%u routers=%u maxHop=3 routeTable=64 sleepGrant=%.1fs\n",
                gwCfg.profile,gwCfg.sensorCount,gwCfg.routerCount,sensorSleepGrantTicks()*0.125f);
  pinMode(LED_PIN,OUTPUT);
  digitalWrite(LED_PIN,LOW);
  gwActivityLedOn=false;
  pinMode(PIN_LORA_RST,OUTPUT);
  digitalWrite(PIN_LORA_RST,LOW); delay(20);
  digitalWrite(PIN_LORA_RST,HIGH); delay(50);
  SPI.begin(PIN_VSPI_SCK,PIN_VSPI_MISO,PIN_VSPI_MOSI,PIN_LORA_SS);
  if(!manager.init()) {
    // Critical-only safeguard: never remain permanently halted on one bad
    // radio startup. Reboot and let the normal boot path try again.
    Serial.println("[FATAL] radio init failed -> reboot in 3 s");
    Serial.flush();
    delay(3000);
    ESP.restart();
  }
  rf95.setFrequency(gatewayFrequencyMHz());
  rf95.setTxPower(gatewayTxPower(),false);
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  manager.setTimeout(400); manager.setRetries(2);
  manager.setMaxHops(3); manager.setIsaRouter(true);
  nextSeq=(uint16_t)esp_random();
  WiFi.mode(WIFI_STA);
  Serial.printf("[WIFI] SSID=%s source=%s\n",wifiSsid,wifiConfigLoadedFromNvs?"NVS":"DEFAULT");
  mqtt.setServer(MQTT_BROKER,MQTT_PORT); mqtt.setCallback(mqttCb);
  mqtt.setSocketTimeout(1);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(384);
  serviceNetwork(true);
  Serial.println("[GW Group-A] Node 2~49 / Router 50~55 / MaxHop 3 / RouteTable 64");
  // A reset may occur while nodes still hold grants from the previous boot.
  const uint32_t bootGuard=millis();
  while((uint32_t)(millis()-bootGuard)<20000UL) {
    receiveOnce(); serviceNetwork(true); delay(10);
  }
}
void loop() {
  serviceGatewayActivityLed();
  // Do not start a LoRa polling cycle until the existing Web path is usable.
  // While offline we still service WiFi/MQTT and receive any stray LoRa frame,
  // but no POLL or SLEEP GRANT transaction is initiated.
  if(!checkPollGate(true)) {
    receiveOnce();
    delay(10);
    return;
  }

  const uint32_t cycleStart=millis();

  // Execute resets only between cycles, never in the middle of POLL/TELEMETRY.
  if(resetGwOnlyRequested) {
    resetGwOnlyRequested=false;
    Serial.println("[RESET] GW-only reboot");
    Serial.flush(); delay(200); ESP.restart();
  }
  if(resetAllRequested) performResetAll();

  // Sensor Nodes are polled sequentially.  If WiFi/MQTT drops between slots,
  // pause immediately before starting the next transaction.
  for(uint8_t n=0;n<gwCfg.sensorCount;n++) {
    if(!checkPollGate(true)) return;
    pollDevice(gwCfg.sensorNodes[n],NODE_SLOT_MS);
    if(!checkPollGate(true)) return;
  }

  // Routers remain RX/relay devices.  Their health Poll is also network-gated.
  for(uint8_t r=0;r<gwCfg.routerCount;r++) {
    if(!routerPolled[r] || (uint32_t)(millis()-lastRouterPollMs[r])>=ROUTER_POLL_INTERVAL_MS) {
      if(!checkPollGate(true)) return;
      pollDevice(gwCfg.routerNodes[r],ROUTER_SLOT_MS);
      lastRouterPollMs[r]=millis(); routerPolled[r]=true;
      if(!checkPollGate(true)) return;
    }
  }
  Serial.printf("[CYCLE] sensors=%u routers=%u elapsedMs=%lu\n",
                gwCfg.sensorCount,gwCfg.routerCount,(unsigned long)(millis()-cycleStart));
}
