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

///////////////////////////////////////////////////////  
// POWER1 그룹 (NEC)  
{"POWER1", NEC, 0x20DF10EF, 32}  
{"VOL-1", NEC, 0x20DFC03F, 32}  
{"VOL+1", NEC, 0x20DF40BF, 32}  

// POWER2 그룹 (NEC_LIKE)  
{"POWER2", NEC_LIKE, 0x55CCA2FF, 32}  
{"VOL-2", NEC_LIKE, 0x55CCA8FF, 32}  
{"VOL+2", NEC_LIKE, 0x55CC90FF, 32}  


