#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[Wi-Fi 스캔 후 선택 연결 예제]");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(1000);

  // 1️⃣ Wi-Fi 스캔
  Serial.println("주변 Wi-Fi 검색 중...");
  int n = WiFi.scanNetworks();

  if (n <= 0) {
    Serial.println("⚠️ 네트워크를 찾지 못했습니다.");
    return;
  }

  Serial.printf("🔍 %d개의 네트워크 발견!\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("%2d: %s (%d dBm)%s\n",
                  i + 1,
                  WiFi.SSID(i).c_str(),
                  WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " [OPEN]" : "");
    delay(10);
  }

  Serial.println("\n연결할 네트워크 번호를 입력하세요 (예: 1):");
}

void loop() {
  static bool waitingInput = true;
  static int chosenIndex = -1;
  static String ssid = "";
  static String password = "";

  // 2️⃣ 시리얼 입력 대기 (번호 선택)
  if (waitingInput && Serial.available()) {
    chosenIndex = Serial.parseInt(); // 사용자 입력 읽기 (예: 1)
    if (chosenIndex > 0) {
      Serial.printf("\n선택된 네트워크 번호: %d\n", chosenIndex);
      ssid = WiFi.SSID(chosenIndex - 1);
      waitingInput = false;

      Serial.printf("선택된 SSID: %s\n", ssid.c_str());
      if (WiFi.encryptionType(chosenIndex - 1) == WIFI_AUTH_OPEN) {
        Serial.println("이 네트워크는 암호가 없습니다. 바로 연결합니다.");
        password = "";
      } else {
        Serial.println("비밀번호를 입력하세요:");
      }
    }
  }

  // 3️⃣ 암호 입력 받기
  if (!waitingInput && password == "" && Serial.available() && WiFi.encryptionType(chosenIndex - 1) != WIFI_AUTH_OPEN) {
    password = Serial.readStringUntil('\n');
    password.trim(); // 개행문자 제거
    Serial.printf("\n입력된 비밀번호: %s\n", password.c_str());
  }

  // 4️⃣ 연결 시도
  if (!waitingInput && ssid != "" && (WiFi.encryptionType(chosenIndex - 1) == WIFI_AUTH_OPEN || password != "")) {
    Serial.printf("'%s' 네트워크에 연결 중...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
      delay(500);
      Serial.print(".");
      retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ Wi-Fi 연결 성공!");
      Serial.print("IP 주소: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n❌ 연결 실패. 비밀번호가 틀렸거나 신호가 약합니다.");
    }

    // 다시 입력을 받지 않도록 루프 멈춤
    while (true) {
      delay(1000);
    }
  }
}
