// FINAL Group-A Node/Router firmware + multi-device UID/LED production SETUP mode (V6).
// Based on NR5(7).ino.
//
// Normal role/range:
//   Sensor Node = address 2~49
//   Router      = address 50~55
//
// Production/AS setup:
//   - Every boot opens a short LoRa setup window on SETUP_FREQ_MHZ.
//   - If EEPROM is blank/invalid, the board remains in SETUP mode until configured.
//   - Unconfigured board: LED_COMM slow blink continuously.
//   - Already configured board: LED_COMM stays OFF during the boot setup window unless IDENTIFY is requested.
//   - V6 first boot invalidates the old NR config once using a firmware reset marker.
//   - A newly built/uploaded firmware image automatically invalidates the saved NR config once.
//   - Ordinary RESET/power cycles do NOT erase EEPROM; saved ADDR/FREQ/POWER remain valid.
//   - Multiple blank boards may be powered at the same time; each reports a silicon-derived UID.
//   - IDENTIFY makes only the selected board blink LED_COMM VERY FAST for visual confirmation.
//   - Targeted WRITE includes UID, so only the selected board stores ADDR/FREQ/POWER.
//   - WRITE success lights LED_COMM solid for 2.5 s, then normal LoRa starts.
//   - Existing USB Serial CONFIG (C/c at boot) is retained as a service fallback.
//
// IMPORTANT: SETUP_FREQ_MHZ and protocol constants remain compatible with ESP32_LoRa_NR_SetupBox_UID_LED_V4.ino.

#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifndef RH_ROUTING_TABLE_SIZE
#define RH_ROUTING_TABLE_SIZE 64
#endif
#include <RH_RF95.h>
#include <RHMesh.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/io.h>
#include <avr/wdt.h>

// =====================================================
// USER CONFIG (EEPROM + Serial)
// =====================================================
// Same image is used for both Sensor Node and Router.
// Serial fallback: 115200 baud. RESET then C/c within 5 s.
//   SHOW
//   SET ADDR 2        // Sensor Node: 2~49
//   SET ADDR 50       // Router: 50~55
//   SET FREQ 922.0
//   SET POWER 13
//   RUN
// Role is derived from address: 2~49=Sensor, 50~55=Router.
#define GW_ADDRESS 1

static const uint16_t CFG_MAGIC=0x4E52; // 'NR'
static const uint8_t  CFG_VERSION=2; // Wireless config protocol/config layout unchanged
static const uint32_t DEFAULT_FREQ_KHZ=922000UL;
static const int8_t   DEFAULT_TX_POWER=13;
static const uint8_t  DEFAULT_ADDRESS=2;
static const uint32_t CONFIG_WINDOW_MS=5000UL;

// Automatic firmware-upload reset marker.
// The current firmware build has a signature made from __DATE__ + __TIME__.
// When a different firmware build is flashed, its signature differs, so the previous
// ADDR/FREQ/POWER block is invalidated exactly once. Normal RESET/power cycles keep it.
// This avoids a manually maintained reset token while preserving EEPROM after setup.
static const int EEPROM_CONFIG_ADDR=0;
static const int EEPROM_FW_BUILD_MARKER_ADDR=32;

#pragma pack(push,1)
struct DeviceConfig {
  uint16_t magic;
  uint8_t version;
  uint8_t address;
  uint32_t freq_khz;
  int8_t tx_power;
  uint16_t crc;
};
#pragma pack(pop)

static DeviceConfig deviceCfg;
static bool configLoadedFromEeprom=false;
static bool configDirty=false;

static uint16_t configCrc16(const uint8_t* data,size_t len) {
  uint16_t crc=0xFFFF;
  while(len--) {
    crc^=*data++;
    for(uint8_t i=0;i<8;i++) crc=(crc&1)?(uint16_t)((crc>>1)^0xA001):(uint16_t)(crc>>1);
  }
  return crc;
}
static uint16_t calcConfigCrc(const DeviceConfig &c) {
  return configCrc16((const uint8_t*)&c,sizeof(DeviceConfig)-sizeof(c.crc));
}
static bool validDeviceConfig(const DeviceConfig &c) {
  if(c.magic!=CFG_MAGIC || c.version!=CFG_VERSION) return false;
  if(c.address<2 || c.address>55) return false;
  if(c.freq_khz<850000UL || c.freq_khz>950000UL) return false;
  if(c.tx_power<5 || c.tx_power>20) return false;
  return c.crc==calcConfigCrc(c);
}
static void setDefaultDeviceConfig() {
  memset(&deviceCfg,0,sizeof(deviceCfg));
  deviceCfg.magic=CFG_MAGIC;
  deviceCfg.version=CFG_VERSION;
  deviceCfg.address=DEFAULT_ADDRESS;
  deviceCfg.freq_khz=DEFAULT_FREQ_KHZ;
  deviceCfg.tx_power=DEFAULT_TX_POWER;
  deviceCfg.crc=calcConfigCrc(deviceCfg);
}
static void loadDeviceConfig() {
  EEPROM.get(EEPROM_CONFIG_ADDR,deviceCfg);
  configLoadedFromEeprom=validDeviceConfig(deviceCfg);
  if(!configLoadedFromEeprom) setDefaultDeviceConfig();
  configDirty=false;
}
static void saveDeviceConfig() {
  deviceCfg.magic=CFG_MAGIC;
  deviceCfg.version=CFG_VERSION;
  deviceCfg.crc=calcConfigCrc(deviceCfg);
  EEPROM.put(EEPROM_CONFIG_ADDR,deviceCfg);
  configLoadedFromEeprom=true;
  configDirty=false;
}

static uint32_t currentFirmwareBuildSignature() {
  const char buildId[] = __DATE__ " " __TIME__;
  uint32_t h=2166136261UL; // FNV-1a 32-bit
  for(size_t i=0;i<sizeof(buildId)-1;i++) {
    h^=(uint8_t)buildId[i];
    h*=16777619UL;
  }
  // Avoid an all-0/all-1 style marker just to keep diagnostics obvious.
  if(h==0UL || h==0xFFFFFFFFUL) h^=0x5A39C7E1UL;
  return h;
}

static bool resetConfigForNewFirmwareBuild() {
  const uint32_t currentSig=currentFirmwareBuildSignature();
  uint32_t storedSig=0;
  EEPROM.get(EEPROM_FW_BUILD_MARKER_ADDR,storedSig);
  if(storedSig==currentSig) return false;

  // Clear only the NR config block. The hardware UID is silicon-derived and is not stored here.
  DeviceConfig blank{};
  EEPROM.put(EEPROM_CONFIG_ADDR,blank);
  EEPROM.put(EEPROM_FW_BUILD_MARKER_ADDR,currentSig);
  return true;
}
static uint8_t deviceAddress() { return deviceCfg.address; }
static bool isRouter() { return deviceCfg.address>=50 && deviceCfg.address<=55; }
static float deviceFrequencyMHz() { return deviceCfg.freq_khz/1000.0f; }
static int8_t deviceTxPower() { return deviceCfg.tx_power; }

static void printDeviceConfig() {
  Serial.println(F("--- NR CONFIG ---"));
  Serial.print(F("ADDR   : ")); Serial.println(deviceCfg.address);
  Serial.print(F("ROLE   : ")); Serial.println(isRouter()?F("ROUTER"):F("SENSOR NODE"));
  Serial.print(F("FREQ   : ")); Serial.print(deviceFrequencyMHz(),3); Serial.println(F(" MHz"));
  Serial.print(F("POWER  : ")); Serial.print(deviceCfg.tx_power); Serial.println(F(" dBm"));
  Serial.print(F("SOURCE : "));
  if(configDirty) Serial.println(F("RAM (modified; RUN auto-saves)"));
  else Serial.println(configLoadedFromEeprom?F("EEPROM"):F("DEFAULT (not saved)"));
}
static void printDeviceConfigHelp() {
  Serial.println(F("[CONFIG MODE] SHOW | SET ADDR 2~55 | SET FREQ 922.0 | SET POWER 13 | RUN"));
  Serial.println(F("RUN auto-saves SET changes. SAVE is optional/backward-compatible."));
}
static void upperLine(char* s) {
  while(*s) { *s=(char)toupper((unsigned char)*s); ++s; }
}
static bool configModeExitRequested=false;
static void handleDeviceConfigCommand(char* line) {
  while(*line==' ' || *line=='\t') ++line;
  upperLine(line);
  if(strcmp(line,"SHOW")==0) { printDeviceConfig(); return; }
  if(strcmp(line,"HELP")==0 || strcmp(line,"?")==0) { printDeviceConfigHelp(); return; }
  if(strcmp(line,"RUN")==0 || strcmp(line,"EXIT")==0) {
    if(configDirty) {
      if(!validDeviceConfig(deviceCfg)) { Serial.println(F("ERR: config invalid; not starting")); return; }
      saveDeviceConfig();
      Serial.println(F("OK: EEPROM auto-saved"));
    } else {
      Serial.println(F("OK: no config change; EEPROM write skipped"));
    }
    configModeExitRequested=true;
    Serial.println(F("OK: CONFIG MODE exit -> LoRa start"));
    return;
  }
  if(strcmp(line,"SAVE")==0) {
    if(!validDeviceConfig(deviceCfg)) { Serial.println(F("ERR: config invalid")); return; }
    saveDeviceConfig(); Serial.println(F("OK: EEPROM saved (optional command)")); return;
  }
  if(strncmp(line,"SET ADDR ",9)==0) {
    const int v=atoi(line+9);
    if(v<2 || v>55) { Serial.println(F("ERR: ADDR 2~55")); return; }
    deviceCfg.address=(uint8_t)v; deviceCfg.crc=calcConfigCrc(deviceCfg); configDirty=true;
    Serial.print(F("OK: ADDR=")); Serial.print(v);
    Serial.println(v>=50?F(" ROUTER"):F(" SENSOR")); return;
  }
  if(strncmp(line,"SET FREQ ",9)==0) {
    const float mhz=(float)atof(line+9);
    const uint32_t khz=(uint32_t)(mhz*1000.0f+0.5f);
    if(khz<850000UL || khz>950000UL) { Serial.println(F("ERR: FREQ 850~950 MHz")); return; }
    deviceCfg.freq_khz=khz; deviceCfg.crc=calcConfigCrc(deviceCfg); configDirty=true;
    Serial.print(F("OK: FREQ=")); Serial.print(deviceFrequencyMHz(),3); Serial.println(F(" MHz")); return;
  }
  if(strncmp(line,"SET POWER ",10)==0) {
    const int v=atoi(line+10);
    if(v<5 || v>20) { Serial.println(F("ERR: POWER 5~20 dBm")); return; }
    deviceCfg.tx_power=(int8_t)v; deviceCfg.crc=calcConfigCrc(deviceCfg); configDirty=true;
    Serial.print(F("OK: POWER=")); Serial.println(v); return;
  }
  Serial.println(F("ERR: unknown command. Type HELP"));
}

// =====================================================
// Production/AS wireless SETUP protocol
// =====================================================
// Dedicated setup channel. Change this value in BOTH firmware files if needed.
static const float SETUP_FREQ_MHZ = 923.000f;
static const int8_t SETUP_TX_POWER_DBM = 10;
static const uint32_t WIRELESS_SETUP_SESSION_MS = 120000UL;
static const uint32_t SETUP_MAGIC = 0x54535731UL; // "TSW1"
static const uint8_t SETUP_PROTOCOL_VERSION = 2;
static const uint16_t IDENTIFY_LED_MS = 10000U;
static const uint16_t SETUP_SUCCESS_LED_MS = 2500U;
static const uint16_t SETUP_SLOW_BLINK_MS = 700U;
static const uint16_t IDENTIFY_FAST_BLINK_MS = 60U; // toggle period: very fast, visually distinct

// LED_COMM is GPIO 12 on the current Node/Router hardware.
// Keep this constant in sync with LED_COMM below.
static const uint8_t SETUP_STATUS_LED_PIN = 12;
static uint32_t setupUid = 0;
static uint32_t identifyUntilMs = 0;
static bool setupLedState = false;
static uint32_t setupLedLastToggleMs = 0;

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
  uint8_t role;       // 0=Sensor, 1=Router
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

static bool initWirelessSetupRadio();
static bool wirelessSetupPollOnce(bool &sessionActivated,bool &configApplied);


static uint32_t readSetupUid() {
#if defined(__AVR_ATmega3208__) || defined(__AVR_ATmega3209__) || defined(__AVR_ATmega4808__) || defined(__AVR_ATmega4809__) || defined(ARDUINO_ARCH_MEGAAVR)
  const volatile uint8_t* serialBytes = &SIGROW.SERNUM0;
  uint32_t h = 2166136261UL; // FNV-1a 32-bit over factory 10-byte serial number
  for(uint8_t i=0; i<10; i++) {
    h ^= serialBytes[i];
    h *= 16777619UL;
  }
  if(h==0UL || h==0xFFFFFFFFUL) h ^= 0xA5C35A7DUL;
  return h;
#else
  // Fallback for an unexpected AVR target. Current production target is megaAVR 0-series.
  uint32_t h = 0x4E520000UL;
  h ^= ((uint32_t)DEFAULT_ADDRESS << 8);
  h ^= (uint32_t)DEFAULT_FREQ_KHZ;
  return h;
#endif
}

static void setupLedOff() {
  setupLedState=false;
  digitalWrite(SETUP_STATUS_LED_PIN,LOW);
}

static void updateSetupStatusLed() {
  const uint32_t now=millis();
  const bool identifying=((int32_t)(identifyUntilMs-now)>0);

  // Production LED rule:
  //   - blank/invalid EEPROM : slow blink continuously (waiting for setup)
  //   - configured EEPROM    : LED stays OFF
  //   - IDENTIFY selected    : selected board only blinks fast
  if(!identifying && configLoadedFromEeprom) {
    if(setupLedState) setupLedOff();
    return;
  }

  const uint16_t period=identifying?IDENTIFY_FAST_BLINK_MS:SETUP_SLOW_BLINK_MS;
  if((uint32_t)(now-setupLedLastToggleMs)>=period) {
    setupLedLastToggleMs=now;
    setupLedState=!setupLedState;
    digitalWrite(SETUP_STATUS_LED_PIN,setupLedState?HIGH:LOW);
  }
}

static void setupLedSuccess() {
  digitalWrite(SETUP_STATUS_LED_PIN,HIGH);
  delay(SETUP_SUCCESS_LED_MS);
  digitalWrite(SETUP_STATUS_LED_PIN,LOW);
  setupLedState=false;
}

static void runSerialConfigConsole() {
  configModeExitRequested=false;
  Serial.println();
  Serial.println(F("=== CONFIG MODE (EEPROM) ==="));
  printDeviceConfig();
  printDeviceConfigHelp();
  Serial.println(F("SET changes are saved automatically when RUN is entered."));
  Serial.print(F("> "));

  char line[64]; uint8_t pos=0;
  while(!configModeExitRequested) {
    while(Serial.available()) {
      const char c=(char)Serial.read();
      if(c=='\r') continue;
      if(c=='\n') {
        line[pos]=0;
        if(pos) handleDeviceConfigCommand(line);
        pos=0;
        if(!configModeExitRequested) Serial.print(F("> "));
      } else if(pos<sizeof(line)-1) {
        line[pos++]=c;
      }
    }
    delay(1);
  }
  printDeviceConfig();
}

static void runDeviceConfigWindow() {
  Serial.println();
  Serial.print(F("[BOOT] 5 sec SETUP window on "));
  Serial.print(SETUP_FREQ_MHZ,3);
  Serial.println(F(" MHz. C/c = USB CONFIG."));
  Serial.print(F("[SETUP] UID=")); Serial.println(setupUid,HEX);

  const bool wirelessReady=initWirelessSetupRadio();
  if(!wirelessReady) Serial.println(F("[SETUP] WARN: wireless setup radio init failed"));
  if(!configLoadedFromEeprom) {
    Serial.println(F("[SETUP] EEPROM not configured -> slow LED blink until targeted WRITE"));
  } else {
    Serial.println(F("[SETUP] EEPROM configured -> status LED stays OFF (IDENTIFY only)"));
  }

  const uint32_t entryStart=millis();
  bool enterSerial=false;
  bool wirelessSession=!configLoadedFromEeprom;
  uint32_t wirelessDeadline=0;
  setupLedLastToggleMs=millis();
  setupLedState=false;
  identifyUntilMs=0;
  digitalWrite(SETUP_STATUS_LED_PIN,LOW);

  for(;;) {
    updateSetupStatusLed();

    while(Serial.available()) {
      const char c=(char)Serial.read();
      if(c=='C' || c=='c') { enterSerial=true; break; }
    }
    if(enterSerial) break;

    if(wirelessReady) {
      bool sessionActivated=false;
      bool configApplied=false;
      if(wirelessSetupPollOnce(sessionActivated,configApplied)) {
        if(sessionActivated) {
          wirelessSession=true;
          wirelessDeadline=millis()+WIRELESS_SETUP_SESSION_MS;
        }
        if(configApplied) {
          Serial.println(F("[SETUP] Targeted wireless config saved -> normal LoRa start"));
          setupLedSuccess();
          return;
        }
      }
    }

    // Blank EEPROM: stay in production setup forever until this UID is configured.
    if(!configLoadedFromEeprom) { delay(1); continue; }

    if(wirelessSession) {
      if((int32_t)(millis()-wirelessDeadline)>=0) {
        setupLedOff();
        Serial.println(F("[SETUP] Wireless session timeout -> normal LoRa start"));
        return;
      }
    } else if((uint32_t)(millis()-entryStart)>=CONFIG_WINDOW_MS) {
      setupLedOff();
      Serial.println(F("[BOOT] NORMAL MODE -> LoRa start"));
      return;
    }
    delay(1);
  }

  setupLedOff();
  runSerialConfigConsole();
}

// =====================================================
// LoRa Pins
// =====================================================
#define RFM95_CS    7
#define RFM95_RST   10
#define RFM95_DIO0  11

// =====================================================
// Pins
// =====================================================
#define LED_CTRL    13
#define LED_COMM    12

// =====================================================
// VBAT measurement (A3)
// =====================================================
static const uint16_t INTREF_mV = 1100;
const uint8_t VBAT_PIN = A3;
const float  VBAT_DIV_RATIO = 1.0f;

// =====================================================
// Normal LoRa protocol
// =====================================================
enum : uint8_t { PKT_POLL=0x01, PKT_RESET=0x03, PKT_TELEM=0x81, PKT_SLEEP_GRANT=0x82 };
#pragma pack(push,1)
struct TelemetryPayload {
  uint16_t vbat_mV;
  int16_t t,h,vib;
  int16_t link_rssi;
  uint8_t led_state;
};
#pragma pack(pop)
static_assert(sizeof(TelemetryPayload)==11, "Telemetry layout mismatch");

RH_RF95 rf95(RFM95_CS, RFM95_DIO0);
RHMesh manager(rf95, 2);

static uint16_t setupPacketCrc(const void* p,size_t lenWithoutCrc) {
  return configCrc16((const uint8_t*)p,lenWithoutCrc);
}

// Keep CRC helpers non-template and independent of user-defined packet types.
// This avoids Arduino .ino auto-prototype errors on AVR cores.
static uint16_t setupPacketCalcCrc(const void* p, size_t totalLen) {
  return setupPacketCrc(p, totalLen - sizeof(uint16_t));
}

static bool initWirelessSetupRadio() {
  // GPIO/SPI are initialized before this function is called from setup().
  if(!rf95.init()) return false;
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  if(!rf95.setFrequency(SETUP_FREQ_MHZ)) return false;
  rf95.setTxPower(SETUP_TX_POWER_DBM,false);
  rf95.setModeRx();
  return true;
}

static uint16_t discoveryReplyDelayMs(uint16_t nonce) {
  uint32_t x=setupUid ^ ((uint32_t)nonce*2654435761UL);
  x ^= x >> 16;
  x *= 2246822519UL;
  x ^= x >> 13;
  return (uint16_t)(35U + (x % 640U));
}

static void sendSetupInfo(uint16_t nonce) {
  SetupInfoPacket p{};
  p.magic=SETUP_MAGIC;
  p.version=SETUP_PROTOCOL_VERSION;
  p.type=SETUP_PKT_INFO;
  p.nonce=nonce;
  p.uid=setupUid;
  p.configured=configLoadedFromEeprom?1:0;
  p.address=deviceCfg.address;
  p.role=(deviceCfg.address>=50)?1:0;
  p.freq_khz=deviceCfg.freq_khz;
  p.tx_power=deviceCfg.tx_power;
  p.crc=setupPacketCalcCrc(&p,sizeof(p));
  delay(discoveryReplyDelayMs(nonce));
  rf95.send((uint8_t*)&p,sizeof(p));
  rf95.waitPacketSent();
  rf95.setModeRx();
}

static void sendIdentifyAck() {
  SetupIdentifyAckPacket p{};
  p.magic=SETUP_MAGIC;
  p.version=SETUP_PROTOCOL_VERSION;
  p.type=SETUP_PKT_IDENTIFY_ACK;
  p.uid=setupUid;
  p.crc=setupPacketCalcCrc(&p,sizeof(p));
  rf95.send((uint8_t*)&p,sizeof(p));
  rf95.waitPacketSent();
  rf95.setModeRx();
}

static void sendSetupResult(uint8_t status) {
  SetupResultPacket p{};
  p.magic=SETUP_MAGIC;
  p.version=SETUP_PROTOCOL_VERSION;
  p.type=SETUP_PKT_RESULT;
  p.uid=setupUid;
  p.status=status;
  p.address=deviceCfg.address;
  p.freq_khz=deviceCfg.freq_khz;
  p.tx_power=deviceCfg.tx_power;
  p.crc=setupPacketCalcCrc(&p,sizeof(p));
  rf95.send((uint8_t*)&p,sizeof(p));
  rf95.waitPacketSent();
  rf95.setModeRx();
}

static bool wirelessSetupPollOnce(bool &sessionActivated,bool &configApplied) {
  sessionActivated=false;
  configApplied=false;
  if(!rf95.available()) return false;

  uint8_t buf[64];
  uint8_t len=sizeof(buf);
  if(!rf95.recv(buf,&len)) return false;
  if(len<6) return false;

  uint32_t magic=0;
  memcpy(&magic,buf,sizeof(magic));
  if(magic!=SETUP_MAGIC) return false;
  if(buf[4]!=SETUP_PROTOCOL_VERSION) return false;
  const uint8_t type=buf[5];

  if(type==SETUP_PKT_DISCOVER && len==sizeof(SetupDiscoverPacket)) {
    SetupDiscoverPacket p{};
    memcpy(&p,buf,sizeof(p));
    if(p.crc!=setupPacketCalcCrc(&p,sizeof(p))) return false;
    sessionActivated=true;
    sendSetupInfo(p.nonce);
    return true;
  }

  if(type==SETUP_PKT_IDENTIFY && len==sizeof(SetupIdentifyPacket)) {
    SetupIdentifyPacket p{};
    memcpy(&p,buf,sizeof(p));
    if(p.crc!=setupPacketCalcCrc(&p,sizeof(p))) return false;
    if(p.uid!=setupUid) return false;
    sessionActivated=true;
    const uint16_t d=(p.duration_ms<1500U)?1500U:((p.duration_ms>15000U)?15000U:p.duration_ms);
    identifyUntilMs=millis()+d;
    setupLedState=false;
    digitalWrite(SETUP_STATUS_LED_PIN,LOW);
    setupLedLastToggleMs=millis()-IDENTIFY_FAST_BLINK_MS;
    sendIdentifyAck();
    return true;
  }

  if(type==SETUP_PKT_WRITE && len==sizeof(SetupWritePacket)) {
    SetupWritePacket p{};
    memcpy(&p,buf,sizeof(p));
    if(p.crc!=setupPacketCalcCrc(&p,sizeof(p))) return false;
    if(p.uid!=setupUid) return false; // Critical: only the selected physical board may change.
    sessionActivated=true;
    if(p.address<2 || p.address>55) { sendSetupResult(SETUP_STATUS_BAD_ADDR); return true; }
    if(p.freq_khz<850000UL || p.freq_khz>950000UL) { sendSetupResult(SETUP_STATUS_BAD_FREQ); return true; }
    if(p.tx_power<5 || p.tx_power>20) { sendSetupResult(SETUP_STATUS_BAD_PWR); return true; }

    deviceCfg.address=p.address;
    deviceCfg.freq_khz=p.freq_khz;
    deviceCfg.tx_power=p.tx_power;
    deviceCfg.crc=calcConfigCrc(deviceCfg);
    saveDeviceConfig();
    sendSetupResult(SETUP_STATUS_OK);
    delay(120); // allow SETUP BOX to receive RESULT on 923 MHz before retune
    configApplied=true;
    return true;
  }

  return false;
}

static const uint32_t ROUTER_REPORT_INTERVAL_MS=60000UL;
static uint32_t lastRouterReportMs=0;
static bool routerReported=false;
static uint32_t lastCacheMs=0;
static bool cacheValid=false;
static bool cachedReportDelivered=false;
static uint16_t cachedSeq=0;
static uint8_t cachedLed=0xFF;
static uint8_t cachedReply[14];
static uint32_t lastStatsMs=0;
static uint32_t pollCount=0, txOkCount=0, txFailCount=0;

// Independent wall clock: millis() may stop in MCU standby.
static volatile uint32_t rtcTicks=0;
static uint32_t cachedPollTick=0, sleepDeadline=0;
static bool sleepPending=false, grantConsumed=false;
static bool sleepClockOk=false;
static const uint16_t MAX_SLEEP_OFFSET_TICKS=4096;
ISR(RTC_PIT_vect) { RTC.PITINTFLAGS=RTC_PI_bm; ++rtcTicks; }
static uint32_t ticksNow() {
  if(!sleepClockOk) return millis()/125UL;
  uint8_t saved=SREG; cli(); uint32_t t=rtcTicks; SREG=saved; return t;
}
static bool waitRtcClear(volatile uint8_t* reg,uint16_t timeoutMs=100) {
  const uint32_t start=millis();
  while(*reg) {
    if((uint32_t)(millis()-start)>=timeoutMs) return false;
  }
  return true;
}
static bool initSleepClock() {
  if(!waitRtcClear(&RTC.STATUS)) return false;
  RTC.CLKSEL=RTC_CLKSEL_INT32K_gc;
  if(!waitRtcClear(&RTC.PITSTATUS)) return false;
  RTC.PITINTFLAGS=RTC_PI_bm;
  RTC.PITINTCTRL=RTC_PI_bm;
  RTC.PITCTRLA=RTC_PERIOD_CYC4096_gc | RTC_PITEN_bm;
  return true;
}
static void sleepIfGranted() {
  if(isRouter() || !sleepClockOk || !sleepPending) return;
  sleepPending=false;
  if((int32_t)(sleepDeadline-ticksNow())<=0) return;
  Serial.println(F("[SLEEP] GW confirmed; wake before next slot"));
  Serial.flush();
  rf95.sleep();
  digitalWrite(RFM95_CS,HIGH);
  const uint8_t savedAdc=ADC0.CTRLA;
  ADC0.CTRLA &= ~ADC_ENABLE_bm;
  set_sleep_mode(SLEEP_MODE_STANDBY);
  for(;;) {
    cli();
    if((int32_t)(sleepDeadline-rtcTicks)<=0) { sei(); break; }
    sleep_enable();
    sei();
    sleep_cpu();
    sleep_disable();
  }
  ADC0.CTRLA=savedAdc;
  rf95.setModeRx();
  Serial.println(F("[WAKE] RX ready"));
}

static void ADC0_init_for_VCC_measure() {
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc | VREF_AC0REFSEL_1V1_gc;
  VREF.CTRLB = VREF_ADC0REFEN_bm | VREF_AC0REFEN_bm;
  ADC0.CTRLC = ADC_PRESC_DIV64_gc | ADC_REFSEL_VDDREF_gc;
  ADC0.CTRLA = ADC_ENABLE_bm;
  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;
  delay(5);
}
static bool waitAdcReady(uint16_t timeoutMs=20) {
  const uint32_t start=millis();
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {
    if((uint32_t)(millis()-start)>=timeoutMs) return false;
  }
  return true;
}
static uint16_t readVCC_mV_once() {
  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;
  ADC0.COMMAND = ADC_STCONV_bm;
  if(!waitAdcReady()) return 0;
  uint16_t adc = ADC0.RES;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  if(adc == 0) return 0;
  uint32_t vcc = (uint32_t)INTREF_mV * 1023UL / (uint32_t)adc;
  return (uint16_t)vcc;
}
static uint16_t readVCC_mV_avg(uint8_t n = 16) {
  uint32_t sum = 0;
  for(uint8_t i=0;i<n;i++){ sum += readVCC_mV_once(); delay(2); }
  return (uint16_t)(sum / n);
}
static uint16_t readVBAT_mV_once(uint16_t vcc_mV) {
  ADC0.MUXPOS = ADC_MUXPOS_AIN3_gc;
  ADC0.COMMAND = ADC_STCONV_bm;
  if(!waitAdcReady()) return 0;
  uint16_t adc = ADC0.RES;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  uint32_t vbat_mv = (uint32_t)adc * (uint32_t)vcc_mV / 1023UL;
  vbat_mv = (uint32_t)((float)vbat_mv * VBAT_DIV_RATIO);
  return (uint16_t)vbat_mv;
}
static uint16_t readVBAT_mV_avg(uint16_t vcc_mV, uint8_t n=16){
  uint32_t sum=0;
  for(uint8_t i=0;i<n;i++){ sum += readVBAT_mV_once(vcc_mV); delay(2); }
  return (uint16_t)(sum/n);
}

static void buildReply(uint16_t seq, int16_t linkRssi) {
  const uint16_t vcc=readVCC_mV_avg(12);
  TelemetryPayload p;
  p.vbat_mV=readVBAT_mV_avg(vcc,12);
  p.t=(int16_t)random(20,41);
  p.h=(int16_t)random(30,91);
  p.vib=(int16_t)random(0,101);
  p.link_rssi=linkRssi;
  p.led_state=digitalRead(LED_CTRL)?1:0;
  cachedReply[0]=PKT_TELEM;
  memcpy(cachedReply+1,&p,sizeof(p));
  cachedReply[12]=(uint8_t)seq;
  cachedReply[13]=(uint8_t)(seq>>8);
}

static void softwareResetNow() {
  Serial.flush();
  delay(20);
#if defined(RSTCTRL_SWRE_bm)
  _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRE_bm);
#elif defined(RSTCTRL_SWRST_bm)
  _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRST_bm);
#else
  wdt_enable(WDTO_15MS);
  for(;;) {}
#endif
  for(;;) {}
}

static void processRxOnce() {
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN], len=sizeof(buf), from=0;
  if(!manager.recvfromAck(buf,&len,&from)) return;

  if(from==GW_ADDRESS && len==2 && buf[0]==PKT_RESET && buf[1]==0xA5) {
    sleepPending=false;
    Serial.print(F("[RESET RX] address=")); Serial.println(deviceAddress());
    Serial.flush();
    delay(100);
    softwareResetNow();
  }

  if(from==GW_ADDRESS && len==5 && buf[0]==PKT_SLEEP_GRANT) {
    const uint16_t seq=(uint16_t)buf[1] | ((uint16_t)buf[2]<<8);
    const uint16_t offset=(uint16_t)buf[3] | ((uint16_t)buf[4]<<8);
    const uint32_t age=(uint32_t)(ticksNow()-cachedPollTick);
    if(!isRouter() && sleepClockOk && cacheValid && !grantConsumed && seq==cachedSeq &&
       offset>0 && offset<=MAX_SLEEP_OFFSET_TICKS && age<offset) {
      sleepDeadline=cachedPollTick+offset;
      grantConsumed=true;
      sleepPending=true;
    }
    return;
  }
  if(from!=GW_ADDRESS || len!=4 || buf[0]!=PKT_POLL) return;
  if(buf[1]!=0xFF && buf[1]!=0 && buf[1]!=1) return;

  const int16_t pollLinkRssi=rf95.lastRssi();
  const uint16_t seq=(uint16_t)buf[2] | ((uint16_t)buf[3]<<8);
  const uint32_t now=millis();
  const bool duplicate=cacheValid && seq==cachedSeq && buf[1]==cachedLed
                       && (isRouter() ? (uint32_t)(now-lastCacheMs)<20000UL
                           : (uint32_t)(ticksNow()-cachedPollTick)<160UL);
  if(isRouter() && !duplicate && routerReported &&
     (uint32_t)(now-lastRouterReportMs)<ROUTER_REPORT_INTERVAL_MS) return;
  pollCount++;
  if(!duplicate) {
    cachedPollTick=ticksNow();
    grantConsumed=false;
    sleepPending=false;
    if(!isRouter() && buf[1]!=0xFF) digitalWrite(LED_CTRL,buf[1]?HIGH:LOW);
    buildReply(seq,pollLinkRssi);
    cachedSeq=seq;
    cachedLed=buf[1];
    cacheValid=true;
    cachedReportDelivered=false;
    lastCacheMs=now;
  }
  delay((uint16_t)random(140,221));
  digitalWrite(LED_COMM,HIGH);
  const uint32_t sentAt=millis();
  const uint8_t err=manager.sendtoWait(cachedReply,sizeof(cachedReply),GW_ADDRESS);
  digitalWrite(LED_COMM,LOW);
  if(err==RH_ROUTER_ERROR_NONE) {
    txOkCount++;
    if(isRouter() && !cachedReportDelivered) {
      routerReported=true;
      lastRouterReportMs=millis();
      cachedReportDelivered=true;
    }
  } else {
    txFailCount++;
  }
  Serial.print(F("[REPLY] seq=")); Serial.print(seq);
  Serial.print(F(" duplicate=")); Serial.print(duplicate);
  Serial.print(F(" nextHopErr=")); Serial.print(err);
  Serial.print(F(" txMs=")); Serial.println((uint32_t)(millis()-sentAt));
}

void setup() {
#if !defined(RSTCTRL_SWRE_bm) && !defined(RSTCTRL_SWRST_bm)
  wdt_disable();
#endif
  Serial.begin(115200);

  const bool firmwareResetApplied=resetConfigForNewFirmwareBuild();
  loadDeviceConfig();
  if(firmwareResetApplied) {
    Serial.println(F("[EEPROM] New firmware build detected: previous NR config invalidated once."));
    Serial.println(F("[EEPROM] Board is UNCONFIGURED and will slow-blink until setup."));
  }
  Serial.print(F("[FW BUILD SIG] 0x")); Serial.println(currentFirmwareBuildSignature(),HEX);

  setupUid=readSetupUid();
  Serial.print(F("[HW UID] ")); Serial.println(setupUid,HEX);

  pinMode(LED_CTRL,OUTPUT); digitalWrite(LED_CTRL,LOW);
  pinMode(LED_COMM,OUTPUT); digitalWrite(LED_COMM,LOW);
  pinMode(RFM95_CS,OUTPUT); digitalWrite(RFM95_CS,HIGH);
  pinMode(RFM95_DIO0,INPUT);
  pinMode(RFM95_RST,OUTPUT);
  digitalWrite(RFM95_RST,LOW); delay(20);
  digitalWrite(RFM95_RST,HIGH); delay(50);
  SPI.begin();
  randomSeed(micros());

  // Wireless production/AS setup and legacy USB CONFIG share the same boot stage.
  runDeviceConfigWindow();

  manager.setThisAddress(deviceAddress());
  ADC0_init_for_VCC_measure();
  if(!isRouter()) {
    sleepClockOk=initSleepClock();
    if(!sleepClockOk) Serial.println(F("[WARN] RTC sleep clock failed; stay awake"));
  }

  if(!manager.init()) {
    Serial.println(F("[FATAL] radio init failed -> reboot"));
    Serial.flush();
    delay(3000);
    softwareResetNow();
  }
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  rf95.setFrequency(deviceFrequencyMHz());
  rf95.setTxPower(deviceTxPower(),false);
  manager.setTimeout(400);
  manager.setRetries(2);
  manager.setMaxHops(3);
  manager.setIsaRouter(isRouter());
  rf95.setModeRx();

  Serial.print(F("[FINAL EEPROM] address=")); Serial.print(deviceAddress());
  Serial.print(F(" router=")); Serial.print(isRouter());
  Serial.print(F(" freq=")); Serial.print(deviceFrequencyMHz(),3);
  Serial.print(F(" power=")); Serial.print(deviceTxPower());
  Serial.println(F(" sleep=GW-grant-only; routers always RX; sensor data=DUMMY"));
}

void loop() {
  processRxOnce();
  sleepIfGranted();
  if((uint32_t)(millis()-lastStatsMs)>=60000UL) {
    lastStatsMs=millis();
    Serial.print(F("[STATS] polls=")); Serial.print(pollCount);
    Serial.print(F(" nextHopOK=")); Serial.print(txOkCount);
    Serial.print(F(" nextHopFAIL=")); Serial.println(txFailCount);
  }
  delay(1);
}
