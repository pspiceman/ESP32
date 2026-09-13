// MINIMAL-STABLE variant based on the original NR5(1).ino.
// Communication timing/retry/sleep policy is intentionally left unchanged.
// Only critical hang guards + remote RESET ALL support were added.
#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/io.h>
#include <avr/wdt.h>



// =====================================================
// Address
// =====================================================
// ✅ Node: 2~49(권장)
// ✅ Router: 50~54
#ifndef MY_ADDRESS
#define MY_ADDRESS   50      // Node 2/3/4, Router 50
#endif
#define GW_ADDRESS   1

static const bool IS_ROUTER = (MY_ADDRESS >= 50 && MY_ADDRESS <= 54);

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
#define LED_COMM    12   // POLL 수신 시 blink

// =====================================================
// RF Settings
// =====================================================
#define LORA_FREQ     922.0
#define LORA_TX_POWER 13

// =====================================================
// VBAT 측정 (A3)
// =====================================================
static const uint16_t INTREF_mV = 1100;
const uint8_t VBAT_PIN = A3;
const float  VBAT_DIV_RATIO = 1.0f;


// LOW POWER V3: sleep only after a matching end-to-end GW grant.
// Sensor Nodes are endpoints, NOT transit routers. Flash all devices together.
// Wire format v2: POLL {type, LED, seqLo, seqHi};
// TELEMETRY {type, original 9-byte payload, seqLo, seqHi}.
enum : uint8_t { PKT_POLL=0x01, PKT_RESET=0x03, PKT_TELEM=0x81, PKT_SLEEP_GRANT=0x82 };
#pragma pack(push,1)
struct TelemetryPayload {
  uint16_t vbat_mV;
  int16_t t,h,vib;
  uint8_t led_state;
};
#pragma pack(pop)
static_assert(sizeof(TelemetryPayload)==9, "Telemetry layout mismatch");
RH_RF95 rf95(RFM95_CS, RFM95_DIO0);
RHMesh manager(rf95, MY_ADDRESS);
static const uint32_t ROUTER_REPORT_INTERVAL_MS=60000UL;
static uint32_t lastRouterReportMs=0;
static bool routerReported=false;
static uint32_t lastCacheMs=0;
static bool cacheValid=false;
static bool cachedReportDelivered=false;
static uint16_t cachedSeq=0;
static uint8_t cachedLed=0xFF;
static uint8_t cachedReply[12];
static uint32_t lastStatsMs=0;
static uint32_t pollCount=0, txOkCount=0, txFailCount=0;

// Independent wall clock: millis() may stop in MCU standby.
static volatile uint32_t rtcTicks=0;
static uint32_t cachedPollTick=0, sleepDeadline=0;
static bool sleepPending=false, grantConsumed=false;
static bool sleepClockOk=false;
static const uint16_t MAX_SLEEP_OFFSET_TICKS=128; // keep original 16 s sleep
ISR(RTC_PIT_vect) { RTC.PITINTFLAGS=RTC_PI_bm; ++rtcTicks; }
static uint32_t ticksNow() {
  // If RTC synchronization failed, stay awake and use millis only for cache age.
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
  if(IS_ROUTER || !sleepClockOk || !sleepPending) return;
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
  rf95.setModeRx(); // Keep routes; no reset or manager.init().
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
  for(uint8_t i=0;i<n;i++){
    sum += readVCC_mV_once();
    delay(2);
  }
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
  for(uint8_t i=0;i<n;i++){
    sum += readVBAT_mV_once(vcc_mV);
    delay(2);
  }
  return (uint16_t)(sum/n);
}


static void buildReply(uint16_t seq) {
  const uint16_t vcc=readVCC_mV_avg(12);
  TelemetryPayload p;
  p.vbat_mV=readVBAT_mV_avg(vcc,12);
  // Existing communication-test values, not physical soil/vibration readings.
  p.t=(int16_t)random(20,41);
  p.h=(int16_t)random(30,91);
  p.vib=(int16_t)random(0,101);
  p.led_state=digitalRead(LED_CTRL)?1:0;
  cachedReply[0]=PKT_TELEM;
  memcpy(cachedReply+1,&p,sizeof(p));
  cachedReply[10]=(uint8_t)seq;
  cachedReply[11]=(uint8_t)(seq>>8);
}

static void softwareResetNow() {
  // Explicit reset only. On the modern AVR used here, use the Reset
  // Controller instead of intentionally tripping the watchdog. This avoids
  // a possible watchdog-reset loop after RESET ALL.
  Serial.flush();
  delay(20);

#if defined(RSTCTRL_SWRE_bm)
  _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRE_bm);
#elif defined(RSTCTRL_SWRST_bm)
  _PROTECTED_WRITE(RSTCTRL.SWRR, RSTCTRL_SWRST_bm);
#else
  // Fallback for classic AVR targets only.
  wdt_enable(WDTO_15MS);
  for(;;) {}
#endif

  // Software reset should never return.
  for(;;) {}
}

static void processRxOnce() {
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN], len=sizeof(buf), from=0;
  // Must run continuously: this also handles discovery and transit traffic.
  if(!manager.recvfromAck(buf,&len,&from)) return;

  // RESET is acknowledged by recvfromAck() before this handler runs. Delay a
  // moment so the ACK can leave the radio, then reboot this Node/Router.
  if(from==GW_ADDRESS && len==2 && buf[0]==PKT_RESET && buf[1]==0xA5) {
    sleepPending=false;
    Serial.print(F("[RESET RX] address=")); Serial.println(MY_ADDRESS);
    Serial.flush();
    delay(100);
    softwareResetNow();
  }

  if(from==GW_ADDRESS && len==5 && buf[0]==PKT_SLEEP_GRANT) {
    const uint16_t seq=(uint16_t)buf[1] | ((uint16_t)buf[2]<<8);
    const uint16_t offset=(uint16_t)buf[3] | ((uint16_t)buf[4]<<8);
    const uint32_t age=(uint32_t)(ticksNow()-cachedPollTick);
    if(!IS_ROUTER && sleepClockOk && cacheValid && !grantConsumed && seq==cachedSeq &&
       offset>0 && offset<=MAX_SLEEP_OFFSET_TICKS && age<offset) {
      sleepDeadline=cachedPollTick+offset;
      grantConsumed=true;
      sleepPending=true;
    }
    return;
  }
  if(from!=GW_ADDRESS || len!=4 || buf[0]!=PKT_POLL) return;
  if(buf[1]!=0xFF && buf[1]!=0 && buf[1]!=1) return;
  const uint16_t seq=(uint16_t)buf[2] | ((uint16_t)buf[3]<<8);
  const uint32_t now=millis();
  const bool duplicate=cacheValid && seq==cachedSeq && buf[1]==cachedLed
                       && (IS_ROUTER ? (uint32_t)(now-lastCacheMs)<20000UL
                           : (uint32_t)(ticksNow()-cachedPollTick)<160UL);
  // Retransmissions of the SAME transaction may resend the cached report.
  // New Router transactions retain the 60 s reporting interval.
  if(IS_ROUTER && !duplicate && routerReported &&
     (uint32_t)(now-lastRouterReportMs)<ROUTER_REPORT_INTERVAL_MS) return;
  pollCount++;
  if(!duplicate) {
    cachedPollTick=ticksNow();
    grantConsumed=false;
    sleepPending=false;
    if(!IS_ROUTER && buf[1]!=0xFF) digitalWrite(LED_CTRL,buf[1]?HIGH:LOW);
    buildReply(seq);
    cachedSeq=seq;
    cachedLed=buf[1];
    cacheValid=true;
    cachedReportDelivered=false;
    lastCacheMs=now;
  }
  // Allow the last router to finish forwarding/ACK turnaround before replying.
  // Retries remain serialized by the GW's end-to-end response window.
  delay((uint16_t)random(140,221));
  digitalWrite(LED_COMM,HIGH);
  const uint32_t sentAt=millis();
  const uint8_t err=manager.sendtoWait(cachedReply,sizeof(cachedReply),GW_ADDRESS);
  digitalWrite(LED_COMM,LOW);
  if(err==RH_ROUTER_ERROR_NONE) {
    txOkCount++;
    if(IS_ROUTER && !cachedReportDelivered) {
      routerReported=true;
      lastRouterReportMs=millis();
      cachedReportDelivered=true;
    }
  } else {
    txFailCount++;
  }
  // err==0 confirms the next hop only. GW checks the echoed sequence end-to-end.
  Serial.print(F("[REPLY] seq=")); Serial.print(seq);
  Serial.print(F(" duplicate=")); Serial.print(duplicate);
  Serial.print(F(" nextHopErr=")); Serial.print(err);
  Serial.print(F(" txMs=")); Serial.println((uint32_t)(millis()-sentAt));
}

void setup() {
  // If this image is ever built for a classic AVR and the watchdog fallback
  // was used, make sure a watchdog reset cannot trap the MCU in a reboot loop.
#if !defined(RSTCTRL_SWRE_bm) && !defined(RSTCTRL_SWRST_bm)
  wdt_disable();
#endif
  Serial.begin(115200);
  pinMode(LED_CTRL,OUTPUT); digitalWrite(LED_CTRL,LOW);
  pinMode(LED_COMM,OUTPUT); digitalWrite(LED_COMM,LOW);
  pinMode(RFM95_CS,OUTPUT); digitalWrite(RFM95_CS,HIGH);
  pinMode(RFM95_DIO0,INPUT);
  pinMode(RFM95_RST,OUTPUT);
  digitalWrite(RFM95_RST,LOW); delay(20);
  digitalWrite(RFM95_RST,HIGH); delay(50);
  SPI.begin();
  ADC0_init_for_VCC_measure();
  if(!IS_ROUTER) {
    sleepClockOk=initSleepClock();
    if(!sleepClockOk) Serial.println(F("[WARN] RTC sleep clock failed; stay awake"));
  }
  randomSeed(micros());
  if(!manager.init()) {
    // Critical-only safeguard: do not remain permanently halted.
    Serial.println(F("[FATAL] radio init failed -> reboot"));
    Serial.flush();
    delay(3000);
    softwareResetNow();
  }
  // All radios use the same explicit modem configuration.
  rf95.setModemConfig(RH_RF95::Bw125Cr45Sf128);
  rf95.setFrequency(LORA_FREQ);
  rf95.setTxPower(LORA_TX_POWER,false);
  manager.setTimeout(400);
  manager.setRetries(2);
  manager.setMaxHops(5);
  manager.setIsaRouter(IS_ROUTER);
  rf95.setModeRx();
  Serial.print(F("[LOWPOWER V3] address=")); Serial.print(MY_ADDRESS);
  Serial.print(F(" router=")); Serial.print(IS_ROUTER);
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
