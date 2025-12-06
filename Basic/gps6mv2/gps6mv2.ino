#include <SoftwareSerial.h>
#include <TinyGPS++.h>

// GPS 모듈 연결 핀 (TX->D7, RX->D6)
static const int RXPin = 7, TXPin = 6;
static const uint32_t GPSBaud = 9600;

SoftwareSerial gpsSerial(RXPin, TXPin);
TinyGPSPlus gps;

unsigned long lastCheck = 0;

void setup() {
  Serial.begin(9600);
  gpsSerial.begin(GPSBaud);

  Serial.println("=== GPS 모듈 초기화 ===");
  Serial.println("1) 모듈 LED가 점등/점멸하는지 확인하세요.");
  Serial.println("2) 반드시 야외에서 3~5분 대기 필요.");
  Serial.println("3) 원시 NMEA 데이터와 좌표를 동시에 출력합니다.");
  Serial.println("==========================================");
}

void loop() {
  // GPS 모듈에서 데이터 읽기
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    Serial.write(c); // NMEA 원시 데이터 출력

    // TinyGPS++ 파싱
    if (gps.encode(c)) {
      if (millis() - lastCheck > 2000) { // 2초마다 정보 출력
        lastCheck = millis();

        Serial.println("\n--- GPS 상태 ---");

        if (gps.location.isValid()) {
          Serial.print("위도: ");
          Serial.println(gps.location.lat(), 6);
          Serial.print("경도: ");
          Serial.println(gps.location.lng(), 6);
        } else {
          Serial.println("위치 정보 없음...");
        }

        if (gps.satellites.isValid()) {
          Serial.print("위성 수: ");
          Serial.println(gps.satellites.value());
        } else {
          Serial.println("위성 수 정보 없음...");
        }

        if (gps.date.isValid() && gps.time.isValid()) {
          Serial.print("날짜: ");
          Serial.print(gps.date.year());
          Serial.print("-");
          Serial.print(gps.date.month());
          Serial.print("-");
          Serial.println(gps.date.day());

          Serial.print("시간(UTC): ");
          Serial.print(gps.time.hour());
          Serial.print(":");
          Serial.print(gps.time.minute());
          Serial.print(":");
          Serial.println(gps.time.second());
        }
      }
    }
  }
}
