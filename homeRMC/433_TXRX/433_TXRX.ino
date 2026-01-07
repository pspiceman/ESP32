#include <RCSwitch.h>

RCSwitch mySwitch = RCSwitch();

// 저장된 신호 변수
unsigned long learnedValue = 0;
unsigned int learnedBitLength = 0;
unsigned int learnedProtocol = 0;
unsigned int learnedDelay = 0;

void setup() {
  Serial.begin(115200);

  // 수신기 설정 (WL101) - GPIO 13 (인터럽트 지원 핀)
  mySwitch.enableReceive(13); 

  // 송신기 설정 (WL102) - GPIO 12
  mySwitch.enableTransmit(12);

  Serial.println("--- 433MHz 학습 모드 활성화 ---");
  Serial.println("리모컨 버튼을 눌러주세요...");
}

void loop() {
  // 1. 신호 수신 및 학습
  if (mySwitch.available()) {
    learnedValue = mySwitch.getReceivedValue();
    
    if (learnedValue == 0) {
      Serial.println("Unknown encoding");
    } else {
      learnedBitLength = mySwitch.getReceivedBitlength();
      learnedProtocol = mySwitch.getReceivedProtocol();
      learnedDelay = mySwitch.getReceivedDelay();

      Serial.print("신호 학습 완료! [값: ");
      Serial.print(learnedValue);
      Serial.print(" / 비트: ");
      Serial.print(learnedBitLength);
      Serial.print(" / 프로토콜: ");
      Serial.print(learnedProtocol);
      Serial.println("]");
      Serial.println("시리얼 모니터에 's'를 입력하면 저장된 신호를 다시 송신합니다.");
    }
    mySwitch.resetAvailable();
  }

  // 2. 학습된 신호 재송신 (시리얼 입력 's' 수신 시)
  if (Serial.available() > 0) {
    char input = Serial.read();
    if (input == 's' && learnedValue != 0) {
      Serial.println("저장된 신호 송신 중...");
      
      mySwitch.setProtocol(learnedProtocol);
      mySwitch.setPulseLength(learnedDelay);
      mySwitch.send(learnedValue, learnedBitLength);
      
      Serial.println("송신 완료!");
    }
  }
}