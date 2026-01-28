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
#define MY_ADDRESS   2      // <<< 여기만 바꿔서 노드/라우터 설정
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
  rf95.waitCAD(); // (충돌 완화) 송신 전 채널 유휴 대기
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

// RX window 정책(요구사항 + 안정화):
// - 기본: 300ms
// - 성공: 250ms (단, "복귀 직후 1회"는 300ms 유지해서 재발 방지)
// - 실패: 450ms
static uint16_t rxWindowMs = 300;
static const uint16_t RX_WIN_BASE_MS = 300;
static const uint16_t RX_WIN_OK_MS   = 250;
static const uint16_t RX_WIN_FAIL_MS = 450;

// 복귀 직후 1회는 300ms로 유지(연속 RESCUE FAIL 같은 덩어리 미스 재발 방지)
static bool holdBaseOnceAfterRecover = false;

// 연속 미스 시 라디오 소프트리셋(모드 꼬임/슬립 타이밍 이슈 대응)
static const uint8_t SOFTRESET_MISS_THRESHOLD = 5;

// =====================================================
// LoRa 소프트 리셋 (Node 전용)
// =====================================================
static void loraSoftReset(){
  // SPI/핀은 이미 유지된다고 가정
  rf95.sleep();
  delay(5);

  // ✅ 하드 리셋 펄스(모듈 상태 꼬임 복구에 효과적)
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, LOW);
  delay(15);
  digitalWrite(RFM95_RST, HIGH);
  delay(20);

  // 재초기화
  if(manager.init()){
    rf95.setFrequency(LORA_FREQ);
    rf95.setTxPower(LORA_TX_POWER,false);
  }

  // 안전하게 슬립 복귀
  loraSleep();
}

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
  // 기본 1초(=8 ticks) 고정.
  int16_t base = 8;

  // phase shift 반영(-2..+2)
  int16_t next = base + phaseAdjustTicks;
  if(next < 5) next = 5;
  if(next > 12) next = 12;

  ticksToNextRx = (uint16_t)next;
  phaseAdjustTicks = 0;
}

// =====================================================
// 수신 후 튜닝 로직 (요구사항 반영 + 복귀 안정화 + 소프트리셋)
// =====================================================
static void updateTuningAfterRx(bool ok){
  if(ok){
    // 성공이면 missStreak 리셋
    bool wasMissing = (missStreak >= 1);
    missStreak = 0;

    if(okStreak < 200) okStreak++;

    // 복귀 직후 1회는 300ms 유지(재발 방지), 그 다음부터 250ms
    if(wasMissing){
      rxWindowMs = RX_WIN_BASE_MS;           // ✅ 복귀 직후 1회는 300ms
      holdBaseOnceAfterRecover = true;
    }else{
      if(holdBaseOnceAfterRecover){
        // 직전 성공에서 300을 1회 썼다면, 이번부터 250으로 내림
        rxWindowMs = RX_WIN_OK_MS;           // ✅ 정상 안정 상태: 250ms
        holdBaseOnceAfterRecover = false;
      }else{
        rxWindowMs = RX_WIN_OK_MS;           // ✅ 계속 성공: 250ms
      }
    }

    // 성공했으면 위상보정은 리셋
    phaseAdjustTicks = 0;
  }else{
    // 실패면 450ms로 스냅
    rxWindowMs = RX_WIN_FAIL_MS;

    okStreak = 0;
    if(missStreak < 250) missStreak++;

    // 실패 연속 시 위상 탐색
    if(missStreak == 1){
      phaseAdjustTicks = +1;
    }else if(missStreak == 2){
      phaseAdjustTicks = -1;
    }else if(missStreak == 3){
      phaseAdjustTicks = +2;
    }else if(missStreak == 4){
      phaseAdjustTicks = -2;
    }else{
      phaseAdjustTicks = (int8_t)random(-2, 3);
    }

    // 연속 미스가 너무 길면(RESCUE도 실패하는 타입) 라디오 소프트리셋
    if(missStreak >= SOFTRESET_MISS_THRESHOLD){
      loraSoftReset();
      // 리셋 직후에는 복귀 안정화를 위해 한 번 300ms 유지하도록 설정
      holdBaseOnceAfterRecover = true;
      rxWindowMs = RX_WIN_BASE_MS;
      missStreak = 0;
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
    rxWindowMs = RX_WIN_BASE_MS;
    holdBaseOnceAfterRecover = false;
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
      // 지터: 평소엔 작게, 연속 실패 시 크게 (안정성↑)
      if(missStreak >= 2) delay((uint8_t)random(0, 36));
      else                delay((uint8_t)random(0, 6));

      bool ok = rxWindowOnce(rxWindowMs);
      updateTuningAfterRx(ok);

      scheduleNextRx();
    }
  }

  loraSleep();
  mcuSleepStandby();
}
