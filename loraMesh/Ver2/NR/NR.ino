#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

// =====================================================
// Address
// =====================================================
#define MY_ADDRESS   4     // Node:2~49 / Router:50~
#define GW_ADDRESS   1
const bool IS_ROUTER = (MY_ADDRESS >= 50);

// =====================================================
// LoRa Pins
// =====================================================
#define RFM95_CS    7
#define RFM95_RST   10
#define RFM95_DIO0  11

#define LED_COMM    12
#define LED_CTRL    13

#define LORA_FREQ     920.0
#define LORA_TX_POWER 13

// =====================================================
// Protocol
// =====================================================
enum : uint8_t {
  PKT_POLL    = 0x01,
  PKT_LED_SET = 0x02,
  PKT_TELEM   = 0x81,
  PKT_LED_ACK = 0x82,
  PKT_BEACON  = 0xB1
};

#pragma pack(push,1)
struct TelemetryPayload {
  uint16_t vbat_mV;
  int16_t  t;
  int16_t  h;
  int16_t  vib;
  uint8_t  led_state;
};
#pragma pack(pop)

RH_RF95 rf95(RFM95_CS, RFM95_DIO0);
RHMesh  manager(rf95, MY_ADDRESS);

// =====================================================
// ADC(VCC)
// =====================================================
static const uint16_t INTREF_mV = 1100;

static void adcInit(){
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc | VREF_AC0REFSEL_1V1_gc;
  VREF.CTRLB = VREF_ADC0REFEN_bm | VREF_AC0REFEN_bm;
  ADC0.CTRLC = ADC_PRESC_DIV64_gc | ADC_REFSEL_VDDREF_gc;
  ADC0.CTRLA = ADC_ENABLE_bm;
  delay(5);
}
static uint16_t readVCC_mV_once(){
  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;
  ADC0.COMMAND = ADC_STCONV_bm;
  while(!(ADC0.INTFLAGS & ADC_RESRDY_bm)){}
  uint16_t adc=ADC0.RES;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  if(adc==0) return 0;
  uint32_t vcc = (uint32_t)INTREF_mV * 1023UL / (uint32_t)adc;
  return (uint16_t)vcc;
}
static uint16_t readVCC_mV_avg(uint8_t n=6){
  uint32_t sum=0;
  for(uint8_t i=0;i<n;i++){ sum+=readVCC_mV_once(); delay(2); }
  return (uint16_t)(sum/n);
}

// =====================================================
// sensors dummy
// =====================================================
static void readSensors(int16_t &t, int16_t &h, int16_t &vib){
  t   = (int16_t)random(20, 41);
  h   = (int16_t)random(30, 91);
  vib = (int16_t)random(0, 101);
}

static void blinkComm(uint8_t n=1){
  for(uint8_t i=0;i<n;i++){
    digitalWrite(LED_COMM, HIGH); delay(25);
    digitalWrite(LED_COMM, LOW);  delay(25);
  }
}

static void loraReset(){
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(20);
  digitalWrite(RFM95_RST, HIGH); delay(50);
}

// =====================================================
// send telemetry
// =====================================================
static void sendTelemetry(){
  TelemetryPayload p;
  p.vbat_mV = readVCC_mV_avg(6);
  readSensors(p.t,p.h,p.vib);
  p.led_state = digitalRead(LED_CTRL) ? 1 : 0;

  uint8_t out[1+sizeof(TelemetryPayload)];
  out[0]=PKT_TELEM;
  memcpy(out+1,&p,sizeof(p));

  rf95.waitCAD();
  manager.sendtoWait(out,sizeof(out),GW_ADDRESS);
}

static void sendLedAck(uint8_t applied){
  uint8_t out[2]={PKT_LED_ACK, (uint8_t)(applied?1:0)};
  rf95.waitCAD();
  manager.sendtoWait(out,sizeof(out),GW_ADDRESS);
}

// =====================================================
// RTC PIT sleep (megaAVR)
// =====================================================
ISR(RTC_PIT_vect){
  RTC.PITINTFLAGS = RTC_PI_bm;
}

static void pitSleep(uint8_t pitPeriod){
  RTC.CLKSEL = RTC_CLKSEL_INT32K_gc;
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITCTRLA   = pitPeriod | RTC_PITEN_bm;

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sei();
  sleep_cpu();
  sleep_disable();

  RTC.PITCTRLA = 0;
  RTC.PITINTCTRL = 0;
}

// =====================================================
// RX service: handle BEACON/POLL/LED
// =====================================================
static bool rxService(uint32_t windowMs, bool stopOnBeacon, bool &gotBeacon){
  uint32_t t0=millis();
  gotBeacon=false;

  while(millis()-t0 < windowMs){
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
    uint8_t len=sizeof(buf);
    uint8_t from;

    if(manager.recvfromAck(buf,&len,&from)){
      blinkComm(1);
      if(len<1) continue;

      // BEACON (broadcast or GW)
      if(buf[0]==PKT_BEACON){
        gotBeacon=true;
        if(stopOnBeacon) return true; // beacon 잡으면 즉시 빠져나감
        continue;
      }

      // POLL → TELEM
      if(buf[0]==PKT_POLL && from==GW_ADDRESS){
        sendTelemetry();
        continue;
      }

      // LED_SET → apply + ACK
      if(buf[0]==PKT_LED_SET && from==GW_ADDRESS){
        if(len>=2){
          uint8_t target = buf[1]?1:0;
          digitalWrite(LED_CTRL, target?HIGH:LOW);
          sendLedAck(target);
        }
        continue;
      }
    }

    delay(2);
  }
  return false;
}

// =====================================================
// Node sync loop (low power)
// =====================================================

// ✅ BEACON 탐색 주기: 1초 sleep 후 120ms RX (반복)
// => 평균전류 낮으면서도 BEACON을 거의 놓치지 않음
static void waitBeaconLowPower(){
  while(true){
    // 1) 잠깐 sleep (1s)
    pitSleep(RTC_PERIOD_CYC1024_gc); // ~1s

    // 2) 짧게 깨어서 BEACON 탐색
    bool gotBeacon=false;
    rxService(120, true, gotBeacon);
    if(gotBeacon) return;
  }
}

void setup(){
  pinMode(LED_COMM, OUTPUT);
  pinMode(LED_CTRL, OUTPUT);
  digitalWrite(LED_COMM, LOW);
  digitalWrite(LED_CTRL, LOW);

  Serial.begin(115200);
  delay(100);

  adcInit();
  SPI.begin();
  loraReset();

  if(!manager.init()){
    Serial.println("RHMesh init FAIL");
    while(1){ blinkComm(2); delay(200); }
  }

  rf95.setFrequency(LORA_FREQ);
  rf95.setTxPower(LORA_TX_POWER,false);

  Serial.print("Start addr="); Serial.println(MY_ADDRESS);
  if(IS_ROUTER) Serial.println("MODE: ROUTER (No Sleep)");
  else          Serial.println("MODE: NODE (Beacon Sync + Low Power)");
}

void loop(){
  if(IS_ROUTER){
    // ✅ Router는 무조건 No Sleep
    bool dummy=false;
    rxService(200,false,dummy);
    return;
  }

  // ✅ Node: Low Power + Beacon Sync
  // 1) BEACON을 잡을 때까지 저전력 방식으로 탐색
  waitBeaconLowPower();

  // 2) BEACON을 잡으면 즉시 RX window 확장해서 poll/led 명령 처리
  //    (GW는 beacon 직후 slot poll을 시작하므로 이 안에서 POLL이 들어옴)
  bool dummy=false;
  rxService(600,false,dummy); // 600ms면 안정적 (200~600으로 조정 가능)

  // 3) 다시 beacon 탐색 상태로 돌아감(저전력)
}
