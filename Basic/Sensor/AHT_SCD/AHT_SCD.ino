#include <Wire.h>
#include <SensirionI2cScd4x.h>   // SCD42 원본(경량) 라이브러리
#include <AHTxx.h>               // AHT25 원본 라이브러리 (AHT10/20/21/25 공용)

SensirionI2cScd4x scd42;
AHTxx aht25(AHTXX_ADDRESS_X38, AHT2x_SENSOR);  // AHT25: I2C 0x38, 2x 계열

bool have_scd = false;
bool have_aht = false;
bool next_is_scd = true;   // 두 센서 모두 있을 때 번갈아 출력 (SCD42부터 시작)

void setup() {
  Wire.begin();
  Serial.begin(115200);

  // --- SCD42 초기화 ---
  scd42.begin(Wire, 0x62);           // SCD42 기본 주소 0x62
  scd42.stopPeriodicMeasurement();   // 재업로드 대비 안전 초기화
  if (scd42.startPeriodicMeasurement() == 0) {
    have_scd = true;
  }

  // --- AHT25 초기화 ---
  have_aht = aht25.begin();

  // 센서 부재는 한 번만 알림
  if (!have_scd) Serial.println("[SCD42] 센서 없음");
  if (!have_aht) Serial.println("[AHT25] 센서 없음");
}

void loop() {
  bool printed = false;

  if (have_scd && have_aht) {
    // --- 두 센서 모두 있을 때: 번갈아 출력 ---
    if (next_is_scd) {
      uint16_t co2_raw = 0;
      float t = NAN, h = NAN;
      if (scd42.readMeasurement(co2_raw, t, h) == 0 && co2_raw != 0) {
        Serial.print("[SCD42] ");
        Serial.print("T:");   Serial.print(t, 2);  Serial.print("C ");
        Serial.print("RH:");  Serial.print(h, 2);  Serial.print("% ");
        Serial.print("CO2:"); Serial.print(co2_raw); Serial.print("ppm");
        Serial.println();
        printed = true;
        next_is_scd = false; // 다음 턴엔 AHT25
      } else {
        // SCD42가 아직 유효 데이터 없으면 이번 턴엔 AHT25로 대체 출력
        float at = aht25.readTemperature();
        float ah = aht25.readHumidity();
        if (!isnan(at) && !isnan(ah)) {
          Serial.print("[AHT25] ");
          Serial.print("T:");  Serial.print(at, 2);  Serial.print("C ");
          Serial.print("RH:"); Serial.print(ah, 2);  Serial.print("%");
          Serial.println();
          printed = true;
          // next_is_scd는 그대로 true로 유지 → 다음 턴에 SCD42 재시도
        }
      }
    } else { // AHT 차례
      float at = aht25.readTemperature();
      float ah = aht25.readHumidity();
      if (!isnan(at) && !isnan(ah)) {
        Serial.print("[AHT25] ");
        Serial.print("T:");  Serial.print(at, 2);  Serial.print("C ");
        Serial.print("RH:"); Serial.print(ah, 2);  Serial.print("%");
        Serial.println();
        printed = true;
        next_is_scd = true; // 다음 턴엔 SCD42
      }
    }
  } else if (have_scd) {
    // --- SCD42만 있을 때 ---
    uint16_t co2_raw = 0;
    float t = NAN, h = NAN;
    if (scd42.readMeasurement(co2_raw, t, h) == 0 && co2_raw != 0) {
      Serial.print("[SCD42] ");
      Serial.print("T:");   Serial.print(t, 2);  Serial.print("C ");
      Serial.print("RH:");  Serial.print(h, 2);  Serial.print("% ");
      Serial.print("CO2:"); Serial.print(co2_raw); Serial.print("ppm");
      Serial.println();
      printed = true;
    }
  } else if (have_aht) {
    // --- AHT25만 있을 때 ---
    float at = aht25.readTemperature();
    float ah = aht25.readHumidity();
    if (!isnan(at) && !isnan(ah)) {
      Serial.print("[AHT25] ");
      Serial.print("T:");  Serial.print(at, 2);  Serial.print("C ");
      Serial.print("RH:"); Serial.print(ah, 2);  Serial.print("%");
      Serial.println();
      printed = true;
    }
  }

  // 유효 값이 있었을 때만 줄바꿈이 이미 들어가므로 별도 처리 불필요
  // (필요시 printed 플래그로 추가 제어 가능)

  delay(2000); // 2초 주기
}
