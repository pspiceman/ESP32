const int ledPin = 27;

void setup() {
  Serial.begin(115200);  // 시리얼 통신 시작 (115200bps)
  pinMode(ledPin, OUTPUT);
}

void loop() {
  // LED ON
  digitalWrite(ledPin, HIGH);
  Serial.println("LED ON");
  delay(500);
  // LED OFF
  digitalWrite(ledPin, LOW);
  Serial.println("LED OFF");
  delay(500);
}
