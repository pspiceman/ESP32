myHome v13 - 주식 시세 브라우저 직접조회 버전

핵심 변경:
- 주식 시세를 ESP32 -> MQTT 경유 방식에서 제거
- GitHub Pages의 myHome.html 브라우저가 네이버 금융에서 직접 조회
- ESP32의 HTTPS/HTML 파싱 실패와 무관하게 주식 시세 표시

조회 우선순위:
1) https://polling.finance.naver.com/api/realtime/domestic/stock/{code}
2) 브라우저 CORS가 막히면 네이버 PC 금융이 사용하는 JSONP 방식
   https://polling.finance.naver.com/api/realtime?query=SERVICE_ITEM:{code}
   _callback 파라미터 사용

종목:
- 삼성전자우 005935 / 10,610주
- SOL AI반도체TOP2플러스 0167A0 / 69주

표시:
- 시가
- 현재가 / 오늘 종가
- 전일 종가
- 고가 / 저가
- 장 상태
- 평가금액
- 총 평가금액
- 총액 - 650,000,000원

중요:
- v13에서는 주식 표시를 위해 ESP32 펌웨어를 다시 올릴 필요가 없습니다.
- GitHub Pages의 HTML/PWA 파일만 교체하면 됩니다.
- MQTT는 ESP32 상태/주변 Wi-Fi 등 기존 용도로 계속 사용합니다.
