const int rstPin = 3;
const int ledPin1 = 8;
const int ledPin2 = 27;

// 부저 주파수 (Hz)
#define BUZZER_PIN 1
#define BUZZ_FREQ 1000 // 1kHz

void setup() {
  Serial.begin(115200);  // 시리얼 통신 시작 (115200bps)
  pinMode(rstPin, INPUT);
  pinMode(ledPin1, OUTPUT);
  pinMode(ledPin2, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
}

void loop() {
  // LED ON
  digitalWrite(ledPin1, HIGH);
  digitalWrite(ledPin2, HIGH);
  Serial.println("LED 4: ON, LED 5: ON");

  // 1kHz 부저 ON (500ms)
  tone(BUZZER_PIN, BUZZ_FREQ);
  delay(500);

  // LED OFF
  digitalWrite(ledPin1, LOW);
  digitalWrite(ledPin2, LOW);
  Serial.println("LED 4: OFF, LED 5: OFF");

  // 부저 OFF (500ms)
  noTone(BUZZER_PIN);
  delay(500);
}
