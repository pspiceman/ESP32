#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#include <avr/interrupt.h>
#include <avr/sleep.h>

// =====================================================
// Address
// =====================================================
// ✅ Node: 2~49(권장) 또는 2~51
// ✅ Router: 50~54
#define MY_ADDRESS   4      // <<< 여기만 바꿔서 노드/라우터 설정
#define GW_ADDRESS   1

static const bool IS_ROUTER = (MY_ADDRESS >= 50);

// =====================================================
// Router 응답 제한 (요구사항: 60초에 1회)
// =====================================================
static const uint32_t ROUTER_REPORT_INTERVAL_MS = 60000UL; // ✅ 60초(1분)
static uint32_t lastRouterReportMs = 0;

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
#define LORA_FREQ     920.0
#define LORA_TX_POWER 13

// =====================================================
// VBAT 측정 (A3)
// =====================================================
static const uint16_t INTREF_mV = 1100;
const uint8_t VBAT_PIN = A3;
const float  VBAT_DIV_RATIO = 1.0f;

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
// ADC0 Init for VCC + VBAT
// =====================================================
static void ADC0_init_for_VCC_measure() {
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc | VREF_AC0REFSEL_1V1_gc;
  VREF.CTRLB = VREF_ADC0REFEN_bm | VREF_AC0REFEN_bm;

  ADC0.CTRLC = ADC_PRESC_DIV64_gc | ADC_REFSEL_VDDREF_gc;
  ADC0.CTRLA = ADC_ENABLE_bm;

  ADC0.MUXPOS = ADC_MUXPOS_DACREF_gc;
  delay(5);
}

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

static uint16_t readVBAT_mV_once(uint16_t vcc_mV) {
  ADC0.MUXPOS = ADC_MUXPOS_AIN3_gc;

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
// POLL 수신 표시
// =====================================================
static inline void blinkOnPoll(){
  for(uint8_t i=0;i<2;i++){
    digitalWrite(LED_COMM, HIGH); delay(18);
    digitalWrite(LED_COMM, LOW);  delay(22);
  }
}

// =====================================================
// Random sensor
// =====================================================
static inline void makeRandomSensors(int16_t &t, int16_t &h, int16_t &vib){
  t   = (int16_t)random(20, 41);
  h   = (int16_t)random(30, 91);
  vib = (int16_t)random(0, 101);
}

// =====================================================
// Telemetry
// =====================================================
static void sendTelemetry(uint8_t dest){
  uint16_t vcc_mV  = readVCC_mV_avg(12);
  uint16_t vbat_mV = readVBAT_mV_avg(vcc_mV, 12);

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

  // ✅ Node는 TX 후 Sleep 복귀 (Router는 RX 계속)
  if(!IS_ROUTER) loraSleep();
}

static void sendTelemetryConfirm(uint8_t dest, uint8_t repeat=2){
  for(uint8_t i=0;i<repeat;i++){
    sendTelemetry(dest);
    if(i+1 < repeat) delay(35);
  }
}

// =====================================================
// RX 처리 (Node/Router 공용)
// =====================================================
static bool processRxOnce(){
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);
  uint8_t from = 0;

  if(manager.recvfromAck(buf, &len, &from)){
    if(len >= 1 && buf[0] == PKT_POLL){

      // =================================================
      // ✅ Router: 60초에 1번만 텔레메트리 응답
      // =================================================
      if(IS_ROUTER){
        uint32_t now = millis();

        // 60초 경과 시에만 1회 응답
        if(now - lastRouterReportMs >= ROUTER_REPORT_INTERVAL_MS){
          lastRouterReportMs = now;
          sendTelemetry(from);      // ✅ 1분에 1번만 응답
        }

        blinkOnPoll();              // (선택) 수신 표시
        return true;                // ✅ 나머지 처리는 하지 않음
      }

      // =================================================
      // ✅ Node: 기존 방식으로 항상 응답
      // =================================================
      bool hasLedCmd = (len >= 2 && buf[1] != 0xFF);

      if(hasLedCmd){
        // LED 명령 최우선 적용
        digitalWrite(LED_CTRL, buf[1] ? HIGH : LOW);
        delay(3);

        // 확정 Telemetry 2회 전송(수신율↑)
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

  ADC0_init_for_VCC_measure();

  if(!IS_ROUTER){
    setupPIT_125ms();
  }

  pinMode(RFM95_CS, OUTPUT);
  digitalWrite(RFM95_CS, HIGH);
  pinMode(RFM95_DIO0, INPUT);

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

  if(IS_ROUTER){
    rf95.setModeRx();     // ✅ Router는 상시 RX(중계)
    lastRouterReportMs = millis(); // 시작 후 60초 뒤 첫 응답
  } else {
    loraSleep();          // ✅ Node는 sleep 시작
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
  // ✅ Node Mode (Low Power)
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
