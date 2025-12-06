#include <Wire.h>
#include <SensirionI2cScd4x.h>

SensirionI2cScd4x scd42;

void setup() {
  Wire.begin();
  Serial.begin(115200);

  // SCD42 I2C 주소는 기본 0x62
  scd42.begin(Wire, 0x62);

  // 재시작/재업로드 시 안전하게 초기화
  scd42.stopPeriodicMeasurement();
  scd42.startPeriodicMeasurement();
}

void loop() {
  uint16_t co2_raw = 0;
  float temperature = NAN;
  float humidity = NAN;

  // 준비되지 않은 경우 co2_raw == 0으로 올 수 있음 (초기 약 ~5초)
  if (scd42.readMeasurement(co2_raw, temperature, humidity) == 0 && co2_raw != 0) {
    // ---- 최소 출력: 한 줄 ----
    Serial.print("T:");   Serial.print(temperature, 2);  Serial.print("C ");
    Serial.print("RH:");  Serial.print(humidity, 2);     Serial.print("% ");
    Serial.print("CO2:"); Serial.print(co2_raw);         Serial.print("ppm");
    Serial.println();
  }

  delay(2000);
}
