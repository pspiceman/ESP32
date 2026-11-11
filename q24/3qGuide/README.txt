3Q Score – 안드로이드 탭 설치(PWA) 안내
========================================

[구성]
3qAutoscore.html      앱 본체 (manifest·아이콘·서비스워커 연결 코드 추가됨)
manifest.webmanifest  앱 이름/아이콘/전체화면 설정
sw.js                 오프라인 실행용 서비스워커
favicon.png, icons/   런처 아이콘 (일반 + 마스커블)

[중요] 홈 화면 "설치"와 카메라는 HTTPS(또는 localhost)에서만 동작합니다.
파일을 탭에 복사해 file:// 로 열면 설치 버튼이 나오지 않습니다.

[방법 1 – 무료 HTTPS 호스팅 (추천)]
1. 이 폴더의 파일 전체를 GitHub 저장소 q24/3qAutoscore/ 폴더에
   3qAutoscore.html 과 같은 위치로 업로드
2. 저장소 Settings → Pages 에서 main 브랜치로 GitHub Pages 켜기
3. 안드로이드 탭 Chrome에서 아래 주소 접속
   https://pspiceman.github.io/ESP32/q24/3qAutoscore/3qAutoscore.html
4. 주소창 오른쪽 "설치" 아이콘 또는 메뉴(⋮) → "앱 설치"
   (메뉴에 "홈 화면에 추가"만 보이면 그것을 눌러도 동일하게 설치됨)
5. 홈 화면/앱 서랍에 Q24 아이콘이 생기고, 주소창 없는 전체화면 앱으로 실행
6. 한 번 실행한 뒤에는 인터넷이 없어도 오프라인 실행 가능

[방법 2 – PC에서 로컬 테스트]
  PC에서:  python -m http.server 8000   (이 폴더 안에서 실행)
  탭을 USB 연결 → PC Chrome의 chrome://inspect → Port forwarding 8000 설정
  → 탭 Chrome에서 http://localhost:8000/3qAutoscore.html 접속 → 설치

[업데이트할 때]
3qAutoscore.html 교체 후 sw.js 맨 위 CACHE 값(예: 3qscore-v58.45)을 바꿔 다시 업로드.
앱을 한 번 껐다 켜면 새 버전이 적용됩니다.

[APK가 꼭 필요하면]
https 주소를 pwabuilder.com 에 넣으면 Play 스토어용/사이드로드용 APK(TWA)를
같은 아이콘으로 생성할 수 있습니다.
