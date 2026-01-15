첨부 html과 아이콘 이미지파일을 적용, 웹 설치(홈화면 추가..)시 아이콘(안드로이드)이 나오도록 필요 코드를 만들어 죠  
html파일  
manifest.json  
sw.js  
///////////////////////////////////////////////////////  
// ===== WiFi =====  
#define WIFI_SSID     "Backhome"  
#define WIFI_PASSWORD "1700note"  

// ===== MQTT =====  
const char*    MQTT_BROKER = "broker.hivemq.com";  
const uint16_t MQTT_PORT   = 1883;

