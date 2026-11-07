얼굴 체크인 — Android 설치용 / GitHub Pages

1. ZIP 압축을 풀고 Face_Checkin_Tablet.html, manifest.webmanifest, pwa.js, sw.js,
   icons 폴더를 같은 위치에 업로드합니다. ZIP 자체를 올리는 것이 아닙니다.
   기존 Face_Checkin_Tablet(2).html 대신 이 패키지의 Face_Checkin_Tablet.html로 접속합니다.
2. GitHub 저장소 Settings → Pages → Deploy from a branch에서
   업로드한 브랜치(예: main)와 /(root)를 지정하고 Save 합니다.
3. 게시 완료 후 https://pspiceman.github.io/ESP32/face-checkin/Face_Checkin_Tablet.html 주소를 Android Chrome으로 엽니다.
   github.com의 코드 보기나 raw 파일 주소가 아닌 Pages 주소를 사용합니다.
4. 화면의 '홈 화면에 설치' 버튼을 누릅니다.
   또는 Chrome ⋮ → 홈 화면에 추가 → 설치를 선택합니다.
   메뉴 문구는 Chrome 버전에 따라 다를 수 있습니다.
5. 홈 화면에 생성된 '얼굴 체크인' 아이콘으로 실행합니다.
   standalone 모드로 실행되어 브라우저 주소창이 보이지 않습니다.
   Android 상태 표시줄과 하단 시스템 탐색 영역은 남을 수 있습니다.
6. 카메라 권한을 허용하고 똑똑 버튼으로 얼굴 인식을 이용합니다.

주소창이 남으면
- 예전 일반 바로가기를 제거한 뒤 위 Pages 주소에서 앱으로 다시 설치합니다.
- Chrome 탭이나 카카오톡 내장 브라우저가 아니라 설치된 앱 아이콘으로 엽니다.
- 앱 범위 밖의 외부 사이트로 이동하면 브라우저 UI가 표시될 수 있습니다.
- manifest, pwa.js, sw.js와 icons 폴더가 모두 업로드되었는지 확인합니다.

운영 안내
- 기존 인식·등록·체크인 코드와 저장소 키는 유지했습니다.
- 얼굴 모델과 라이브러리는 기존처럼 외부 CDN에서 불러옵니다.
  앱 화면 캐시는 제공하지만, 얼굴 인식까지 완전 오프라인 동작을 보장하지 않습니다.
- 등록 데이터는 기기의 해당 브라우저 저장소에 있습니다.
  다른 기기·브라우저·도메인으로 자동 이전되지 않습니다.
- 소스 코드만 게시되며 기존 기기의 회원 데이터가 GitHub로 업로드되지는 않습니다.
- 수정 시 같은 경로에 파일을 덮어쓰고 앱을 완전히 닫았다 다시 여세요.
  서비스워커 캐시 구성이 바뀌면 sw.js의 v2를 v3 등으로 변경하세요.
- 실제 Android 기기에서 설치, 카메라 허용, 아이콘 실행을 최종 확인해 주세요.

참고: https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Manifest/Reference/display
