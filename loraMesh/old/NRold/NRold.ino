#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#include <avr/interrupt.h>
#include <avr/sleep.h>

// =====================================================
// Address
// =====================================================
#define MY_ADDRESS   4      // ✅ 2~49 = Node, 50 이상 = Router
#define GW_ADDRESS   1

static const bool IS_ROUTER = (MY_ADDRESS >= 50);

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
#define LED_COMM    12   // ✅ POLL 수신 시에만 blink

// =====================================================
// RF Settings
// =====================================================
#define LORA_FREQ     920.0
#define LORA_TX_POWER 13

// =====================================================
// ✅ VBAT 측정 설정 (A3 = ADC3)
// =====================================================
static const uint16_t INTREF_mV = 1100;   // 내부 기준전압(이론값)
const uint8_t VBAT_PIN = A3;              // ADC3 = A3
const float  VBAT_DIV_RATIO = 1.0f;       // 분압 없으면 1.0, 1/2 분압이면 2.0

// =====================================================
// Protocol
// =====================================================
enum : uint8_t { PKT_POLL = 0x01, PKT_TELEM = 0x81 };

#pragma pack(push,1)
struct TelemetryPayload {
  uint16_t vbat_mV;
  int16_t  t;
  int16_t  h;
  int16_t  vib;
  uint8_t  led_state;
};
#pragma pack(pop)

// =====================================================
// RadioHead
// =====================================================
RH_RF95 rf95(RFM95_CS, RFM95_DIO0);
RHMesh  manager(rf95, MY_ADDRESS);

// =====================================================
// RTC PIT 125ms (Node 전용)
// =====================================================
volatile bool pitFlag=false;

ISR(RTC_PIT_vect){
  RTC.PITINTFLAGS = RTC_PI_bm;
  pitFlag = true;
}

static void setupPIT_125ms(){
  while (RTC.STATUS > 0) {}
  RTC.CLKSEL = RTC_CLKSEL_INT32K_gc;
  RTC.PITCTRLA = RTC_PERIOD_CYC4096_gc | RTC_PITEN_bm; // 125ms
  RTC.PITINTCTRL = RTC_PI_bm;
}

// =====================================================
// ✅ ADC0 Init for VCC + VBAT (발췌 적용)
// =====================================================
static void ADC0_init_for_VCC_measure() {
  // VREF 설정: ADC0 1.1V enable
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc | VREF_AC0REFSEL_1V1_gc;
  VREF.CTRLB = VREF_ADC0REFEN_bm | VREF_AC0REFEN_bm;

  // ADC0: 기준전압 = VDD(VCC), 분주 = DIV64
  ADC0.CTRLC = ADC_PRESC_DIV64_gc | ADC_REFSEL_VDDREF_gc;

  // ADC Enable
  ADC0.CTRLA = ADC_ENABLE_bm;

  // 기본은 VCC 측정 채널(DACREF)
  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;

  delay(5);
}

// 내부 기준전압(DACREF)을 읽어 VCC(mV) 역산
static uint16_t readVCC_mV_once() {
  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;

  ADC0.COMMAND = ADC_STCONV_bm;
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {}
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

// A3(ADC3) 읽어서 mV로 환산 (기준전압=VCC)
static uint16_t readVBAT_mV_once(uint16_t vcc_mV) {
  ADC0.MUXPOS = ADC_MUXPOS_AIN3_gc; // A3 = ADC3

  ADC0.COMMAND = ADC_STCONV_bm;
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {}
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

// =====================================================
// Sleep helpers (Node 전용)
// =====================================================
static inline void loraSleep(){
  rf95.sleep();
  digitalWrite(RFM95_CS, HIGH);
  digitalWrite(RFM95_RST, HIGH);
}

static inline void mcuSleepStandby(){
  SLPCTRL.CTRLA = SLPCTRL_SMODE_STDBY_gc | SLPCTRL_SEN_bm;
  sei();
  __asm__ __volatile__("sleep");
  SLPCTRL.CTRLA &= ~SLPCTRL_SEN_bm;
}

static inline void wakeToRxStable(){
  rf95.setModeIdle();
  delay(5);
  rf95.setModeRx();
  delay(8);
}

// =====================================================
// ✅ POLL 수신 표시: 2번 빠르게 (TX 후 실행)
// =====================================================
static inline void blinkOnPoll(){
  for(uint8_t i=0;i<2;i++){
    digitalWrite(LED_COMM, HIGH); delay(18);
    digitalWrite(LED_COMM, LOW);  delay(22);
  }
}

// =====================================================
// ✅ 랜덤 센서값 생성 (정수)
// =====================================================
static inline void makeRandomSensors(int16_t &t, int16_t &h, int16_t &vib){
  t   = (int16_t)random(20, 41);  // 20~40
  h   = (int16_t)random(30, 91);  // 30~90
  vib = (int16_t)random(0, 101);  // 0~100
}

// =====================================================
// Telemetry (VBAT 전송)
// =====================================================
static void sendTelemetry(uint8_t dest){

  // ✅ 1) VCC 측정
  uint16_t vcc_mV = readVCC_mV_avg(12);

  // ✅ 2) VBAT(A3) 측정
  uint16_t vbat_mV = readVBAT_mV_avg(vcc_mV, 12);

  // ✅ 3) 랜덤 센서값
  int16_t t,h,vib;
  makeRandomSensors(t,h,vib);

  TelemetryPayload p;
  p.vbat_mV = vbat_mV;
  p.t = t;
  p.h = h;
  p.vib = vib;
  p.led_state = digitalRead(LED_CTRL) ? 1 : 0;

  uint8_t out[1 + sizeof(TelemetryPayload)];
  out[0] = PKT_TELEM;
  memcpy(out+1, &p, sizeof(p));

  rf95.setModeIdle();
  manager.sendtoWait(out, sizeof(out), dest);

  // ✅ Node는 TX 후 Sleep 복귀
  if(!IS_ROUTER) loraSleep();
}

// LED 명령 확정용 Telemetry 2회
static void sendTelemetryConfirm(uint8_t dest, uint8_t repeat=2){
  for(uint8_t i=0;i<repeat;i++){
    sendTelemetry(dest);
    if(i+1 < repeat) delay(35);
  }
}

// =====================================================
// 공용 RX 처리 (Node/Router 동일)
// =====================================================
static bool processRxOnce(){
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);
  uint8_t from = 0;

  if(manager.recvfromAck(buf, &len, &from)){
    if(len >= 1 && buf[0] == PKT_POLL){

      bool hasLedCmd = (len >= 2 && buf[1] != 0xFF);

      if(hasLedCmd){
        digitalWrite(LED_CTRL, buf[1] ? HIGH : LOW);
        delay(3);
        sendTelemetryConfirm(from, 2);
      }else{
        sendTelemetry(from);
      }

      blinkOnPoll();
      return true;
    }
  }
  return false;
}

// =====================================================
// RX window (Node 전용)
// =====================================================
static void rxWindow(uint16_t ms){
  wakeToRxStable();

  unsigned long t0 = millis();
  while(millis() - t0 < ms){
    if(processRxOnce()) return;
    delay(1);
  }

  loraSleep();
}

// =====================================================
// Scheduler (Node 전용)
// =====================================================
static uint16_t ticksToNextRx = 0;

static void scheduleNextRx(){
  ticksToNextRx = (uint8_t)random(7, 10); // 0.875~1.125s
}

// =====================================================
// Setup / Loop
// =====================================================
void setup(){
  pinMode(LED_CTRL, OUTPUT);
  digitalWrite(LED_CTRL, LOW);

  pinMode(LED_COMM, OUTPUT);
  digitalWrite(LED_COMM, LOW);

  randomSeed(micros());

  // ✅ ADC 준비 (Node/Router 공용)
  ADC0_init_for_VCC_measure();

  // ✅ Node만 PIT 사용
  if(!IS_ROUTER){
    setupPIT_125ms();
  }

  // LoRa pins
  pinMode(RFM95_CS, OUTPUT);
  digitalWrite(RFM95_CS, HIGH);
  pinMode(RFM95_DIO0, INPUT);

  // Reset
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  digitalWrite(RFM95_RST, LOW); delay(20);
  digitalWrite(RFM95_RST, HIGH); delay(50);

  SPI.begin();

  if(!manager.init()){
    while(1){
      digitalWrite(LED_COMM, !digitalRead(LED_COMM));
      delay(200);
    }
  }

  rf95.setFrequency(LORA_FREQ);
  rf95.setTxPower(LORA_TX_POWER,false);

  // ✅ 시작 모드
  if(IS_ROUTER){
    rf95.setModeRx();     // Router는 상시 RX
  } else {
    loraSleep();          // Node는 sleep 시작
    scheduleNextRx();
  }
}

void loop(){

  // =====================================================
  // ✅ Router Mode (No Sleep)
  // =====================================================
  if(IS_ROUTER){
    processRxOnce();  // 상시 처리 + Mesh 중계
    delay(2);
    return;
  }

  // =====================================================
  // ✅ Node Mode (Low Power Sleep)
  // =====================================================
  if(pitFlag){
    pitFlag = false;

    if(ticksToNextRx > 0) ticksToNextRx--;

    if(ticksToNextRx == 0){
      rxWindow(350);
      scheduleNextRx();
    }
  }

  loraSleep();
  mcuSleepStandby();
}
