// https://chatgpt.com/c/68d36481-0044-8323-b702-5463d8e8c391
// NR_SCD42_Node_LED_RouterBlink.ino
// - RHMesh Node (AVR + RFM95)
// - Sensor: SCD42 (SensirionI2cScd4x 원본 라이브러리)
// - Reply: "BAT=...,T=...|NA,RH=...|NA,CO2=...|NA,LED=..."
// - "SETL:1/0" → LED 제어 → "ACK=1/0" 응답
// - ★ 라우팅(중계) 프레임 수신 시에도 LED Blink (rf95.available())

#include <SPI.h>
#include <Wire.h>
#include <RH_RF95.h>
#include <RHMesh.h>
#include <SensirionI2cScd4x.h>

// ===== LoRa / Mesh =====
#define NODE_ADDRESS   5
#define GATEWAY_ADDR   1
static const float   RF_FREQ_MHZ   = 915.0;
static const uint8_t TX_POWER_DBM  = 13;    // 원 소스 값

// SX1276 (Arduino Nano 예시 핀)
static const uint8_t PIN_LORA_SS   = 10;
static const uint8_t PIN_LORA_RST  = 9;
static const uint8_t PIN_LORA_DIO0 = 2;

// LED
static const uint8_t PIN_LED_COMM  = 4;   // 통신 Blink
static const uint8_t PIN_LED_CTRL  = 5;   // 제어 LED

RH_RF95 rf95(PIN_LORA_SS, PIN_LORA_DIO0);
RHMesh  mesh(rf95, NODE_ADDRESS);

// 통신 LED
static inline void blinkComm(){ digitalWrite(PIN_LED_COMM, HIGH); delay(2); digitalWrite(PIN_LED_COMM, LOW); }
static inline void loraReset(){ pinMode(PIN_LORA_RST, OUTPUT); digitalWrite(PIN_LORA_RST, LOW); delay(10); digitalWrite(PIN_LORA_RST, HIGH); delay(10); }

// ===== 배터리 (분압 없음: AVcc 기준 + 밴드갭으로 Vcc 환산) =====
static const uint8_t ADC_SAMPLES = 10;
long readVcc_mV() {
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__) || defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega2560__)
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);  // 내부 1.1V 밴드갭
  delay(2);
  ADCSRA |= _BV(ADSC); while (bit_is_set(ADCSRA, ADSC));
  ADCSRA |= _BV(ADSC); while (bit_is_set(ADCSRA, ADSC));
  uint16_t adc = ADC ? ADC : 1;
  return 1125300L / adc;  // 1.1V * 1023 * 1000
#else
  return 5000;            // 다른 MCU는 환경에 맞게 수정
#endif
}
float readBat_V(){
  float vcc = readVcc_mV()/1000.0f; // Vcc (V)
  analogReference(DEFAULT);         // 참조 = AVcc
  analogRead(A0); delay(3);         // 첫 샘플 버림
  uint32_t acc=0; for(uint8_t i=0;i<ADC_SAMPLES;i++) acc += analogRead(A0);
  float avg = acc / (float)ADC_SAMPLES;
  return (avg * vcc) / 1023.0f;     // 핀 전압(V)
}

// ===== SCD42 =====
SensirionI2cScd4x scd4x;
bool hasSCD=false;

void scdInit(){
  Wire.begin();
  scd4x.begin(Wire, 0x62);                 // SCD42 I2C 주소
  scd4x.stopPeriodicMeasurement();         // 클린 스타트
  delay(2);
  hasSCD = (scd4x.startPeriodicMeasurement() == 0);
}

bool readSCD(float& tC, int& rhPct, uint16_t& co2ppm){
  if(!hasSCD) return false;
  uint16_t co2=0; float t=0, rh=0;
  if (scd4x.readMeasurement(co2, t, rh) != 0) return false;
  if (co2 == 0) return false;              // 초기/미준비 → 실패 처리
  tC = t; rhPct = (int)(rh + 0.5f); co2ppm = co2; return true;
}

void setup(){
  pinMode(PIN_LED_COMM, OUTPUT);
  pinMode(PIN_LED_CTRL, OUTPUT);
  digitalWrite(PIN_LED_COMM, LOW);
  digitalWrite(PIN_LED_CTRL, LOW);

  loraReset();
  SPI.begin();
  if(!mesh.init()){ for(;;){ digitalWrite(PIN_LED_COMM,!digitalRead(PIN_LED_COMM)); delay(120);} }
  rf95.setFrequency(RF_FREQ_MHZ);
  rf95.setTxPower(TX_POWER_DBM, false);   // ★ PA_BOOST
  mesh.setRetries(4);
  mesh.setTimeout(1800);

  scdInit();
}

void loop(){
  // ★ 라우팅/중계 트래픽 감지 시에도 통신 LED Blink (추가된 유일한 변경)
  if (rf95.available()) {
    blinkComm();
  }

  // 게이트웨이 명령 수신 처리 (원 소스 스타일)
  uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);
  uint8_t from = 0;

  if (mesh.recvfromAckTimeout(buf, &len, 120, &from)) {
    blinkComm();
    if (len >= RH_MESH_MAX_MESSAGE_LEN) len = RH_MESH_MAX_MESSAGE_LEN-1;
    buf[len]='\0';
    char* in=(char*)buf;

    if (strcmp(in, "GET") == 0) {
      float bat = readBat_V();
      float tC=0.0f; int rh=0; uint16_t co2=0; bool ok = readSCD(tC, rh, co2);
      int   led = digitalRead(PIN_LED_CTRL) ? 1 : 0;

      char bats[8], ts[8]; dtostrf(bat,0,1,bats); if(ok) dtostrf(tC,0,1,ts);
      char reply[84];
      if(ok){
        snprintf(reply,sizeof(reply),"BAT=%s,T=%s,RH=%d,CO2=%u,LED=%d",
                 bats, ts, rh, co2, led);
      }else{
        snprintf(reply,sizeof(reply),"BAT=%s,T=NA,RH=NA,CO2=NA,LED=%d",
                 bats, led);
      }
      delay(random(2,7)); // 충돌 완화
      mesh.sendtoWait((uint8_t*)reply, strlen(reply), from);
      blinkComm();
    }
    else if (strncmp(in, "SETL:", 5) == 0) {
      int want = atoi(in + 5);
      digitalWrite(PIN_LED_CTRL, want ? HIGH : LOW);
      delay(2);
      int fb = digitalRead(PIN_LED_CTRL) ? 1 : 0;

      char reply[12]; snprintf(reply,sizeof(reply),"ACK=%d",fb);
      delay(random(2,7));
      mesh.sendtoWait((uint8_t*)reply, strlen(reply), from);
      blinkComm();
    }
  }
}
