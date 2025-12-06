// 아두이노 나노 VCC 측정 + A0, A1 전압 표시

long readVcc() {
  // 1.1V 내부 기준전압을 이용한 VCC 계산
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(2);
  ADCSRA |= _BV(ADSC);
  while (bit_is_set(ADCSRA, ADSC));
  
  uint16_t result = ADC;
  long vcc = 1125300L / result; // 1.1*1023*1000
  return vcc; // mV 단위
}

void setup() {
  Serial.begin(9600);
}

void loop() {
  long vcc = readVcc(); // 현재 VCC (mV)

  // ADC 값 읽기
  int adcA0 = analogRead(A0);
  int adcA1 = analogRead(A1);

  // ADC → 전압 변환 (mV 단위)
  float voltageA0 = (adcA0 / 1023.0) * vcc;
  float voltageA1 = (adcA1 / 1023.0) * vcc;

  // 시리얼 출력
  Serial.print("VCC = ");
  Serial.print(vcc / 1000.0, 3);  // V 단위로 표시
  Serial.println(" V");

  Serial.print("A0 = ");
  Serial.print(voltageA0 / 1000.0, 3);
  Serial.println(" V");

  Serial.print("A1 = ");
  Serial.print(voltageA1 / 1000.0, 3);
  Serial.println(" V");

  Serial.println("----------------------");

  delay(1000);
}
