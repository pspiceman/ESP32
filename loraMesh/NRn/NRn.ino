#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#include <avr/interrupt.h>
#include <avr/sleep.h>

// =====================================================
// Address
// =====================================================
// ✅ Node: 2~49(권장)
// ✅ Router: 50~54
#define MY_ADDRESS   3      // <<< 여기만 바꿔서 노드/라우터 설정
#define GW_ADDRESS   1

static const bool IS_ROUTER = (MY_ADDRESS >= 50);

// =====================================================
// Router 응답 제한 (요구사항: 60초에 1회)
// =====================================================
static const uint32_t ROUTER_REPORT_INTERVAL_MS = 60000UL;
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
#define LORA_FREQ     922.0
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

      // Router: 60초에 1번만 응답
      if(IS_ROUTER){
        uint32_t now = millis();
        if(now - lastRouterReportMs >= ROUTER_REPORT_INTERVAL_MS){
          lastRouterReportMs = now;
          sendTelemetry(from);
        }
        blinkOnPoll();
        return true;
      }

      // Node: LED cmd 있으면 우선 적용 + 텔레메트리 2회
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
// 개선된 Node Scheduler 파라미터
// =====================================================
static uint16_t ticksToNextRx = 0;      // 125ms tick
static uint8_t  missStreak = 0;
static uint8_t  okStreak   = 0;

// “계속 엇갈려 놓치는” 경우를 깨기 위한 phase shift (다음 주기 tick 보정)
static int8_t phaseAdjustTicks = 0;    // -2..+2 정도로 사용

// RX window 동적 조절(전류 유지 목적: 잘 되면 줄이고, 안 되면 조금만 늘림)
static uint16_t rxWindowMs = 300;      // 기본(기존 350보다 낮춤으로 평균전류 절감)

// =====================================================
// RX window (Node 전용) - 성공 여부 반환
// =====================================================
static bool rxWindowOnce(uint16_t ms){
  wakeToRxStable();

  unsigned long t0 = millis();
  while(millis() - t0 < ms){
    if(processRxOnce()){
      // 수신 성공 시 빠르게 슬립 복귀
      loraSleep();
      return true;
    }
    delay(1);
  }

  loraSleep();
  return false;
}

// =====================================================
// 다음 RX 스케줄: "고정 주기 + 작은 지터" + phaseAdjustTicks 반영
// =====================================================
static void scheduleNextRx(){
  // 기본 1초(=8 ticks) 고정. (랜덤 7~9가 계속 위상 락을 만들 수 있어 고정이 유리)
  int16_t base = 8;

  // phase shift 반영(-2..+2)
  int16_t next = base + phaseAdjustTicks;
  if(next < 5) next = 5;     // 너무 짧아지지 않게 안전장치
  if(next > 12) next = 12;

  ticksToNextRx = (uint16_t)next;
  phaseAdjustTicks = 0;      // 1회 적용 후 리셋
}

// =====================================================
// missStreak 기반 "전류 거의 0 증가" 수신율 튜닝 로직
// - 더블 RX 대신: 다음 주기의 타이밍을 앞/뒤로 이동(phase shift)
// - 성공률 좋으면 RX window를 줄여 평균전류를 더 낮춤
// - 성공률 나쁘면 RX window를 조금만 늘림(2배 금지)
// =====================================================
static void updateTuningAfterRx(bool ok){
  if(ok){
    missStreak = 0;
    if(okStreak < 200) okStreak++;

    // 수신이 안정적이면 window를 조금 줄여 평균전류↓
    if(okStreak >= 6){
      if(rxWindowMs > 260) rxWindowMs -= 10;   // 300->290->... 최소 260ms
      okStreak = 6; // 더 내려가는 속도 제한
    }
  }else{
    okStreak = 0;
    if(missStreak < 250) missStreak++;

    // 수신이 안 좋으면 window를 조금만 확대(전류 증가 최소화)
    if(missStreak == 1){
      if(rxWindowMs < 330) rxWindowMs = 330;
    }else if(missStreak == 3){
      if(rxWindowMs < 380) rxWindowMs = 380;
    }else if(missStreak >= 6){
      if(rxWindowMs < 450) rxWindowMs = 450;  // 최악에도 2배(700ms)까지는 안 감
    }

    // 더블 RX 대신 phase shift로 다음번 “겹치는 타이밍”을 깨기
    // missStreak이 늘수록 보정 폭을 키워서 다양한 위상을 탐색
    // (tick=125ms 단위)
    if(missStreak == 1){
      phaseAdjustTicks = +1;           // +125ms 지연
    }else if(missStreak == 2){
      phaseAdjustTicks = -1;           // -125ms 당김
    }else if(missStreak == 3){
      phaseAdjustTicks = +2;           // +250ms 지연
    }else if(missStreak == 4){
      phaseAdjustTicks = -2;           // -250ms 당김
    }else{
      // 5회 이상: 작은 랜덤 탐색(±2tick)
      phaseAdjustTicks = (int8_t)random(-2, 3);
    }
  }
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
    rf95.setModeRx();
    lastRouterReportMs = millis();
  } else {
    loraSleep();
    scheduleNextRx();
  }
}

void loop(){

  // =====================================================
  // Router Mode (No Sleep)
  // =====================================================
  if(IS_ROUTER){
    processRxOnce();
    delay(2);
    return;
  }

  // =====================================================
  // Node Mode (Low Power)
  // =====================================================
  if(pitFlag){
    pitFlag = false;

    if(ticksToNextRx > 0) ticksToNextRx--;

    if(ticksToNextRx == 0){
      // 작은 지터(0~35ms)만 주고 바로 RX (전류 영향 매우 작음, 위상 겹침 해소)
      delay((uint8_t)random(0, 36));

      bool ok = rxWindowOnce(rxWindowMs);
      updateTuningAfterRx(ok);

      scheduleNextRx();
    }
  }

  loraSleep();
  mcuSleepStandby();
}
