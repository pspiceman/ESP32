/* CUE24 Score Board · service worker
 * - 홈 화면 설치(PWA) 조건 충족 + 오프라인 실행
 * - 점수판 HTML은 네트워크 우선(수정본 즉시 반영), 실패 시 캐시
 * - 아이콘 등 정적 파일은 캐시 우선
 * 파일을 수정해 배포할 때는 VERSION 숫자를 올리면 이전 캐시가 정리됩니다.
 */
const VERSION = 'cue24-v1';
const CORE = [
  './cue24_scoreboard.html',
  './manifest.webmanifest',
  './icon-192.png',
  './icon-512.png',
  './icon-maskable-192.png',
  './icon-maskable-512.png',
  './apple-touch-icon.png',
  './favicon-32.png'
];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(VERSION).then(c => c.addAll(CORE)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== VERSION).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  const req = e.request;
  const url = new URL(req.url);
  // 같은 출처의 GET만 처리 (카메라 스트림, 호출 서버, AI 엔진 등은 건드리지 않음)
  if (req.method !== 'GET' || url.origin !== location.origin) return;
  if (/3Q_Score|cue24-bridge\.js/.test(url.pathname)) return;

  if (req.mode === 'navigate' || url.pathname.endsWith('.html')) {
    e.respondWith(
      fetch(req)
        .then(res => { const copy = res.clone(); caches.open(VERSION).then(c => c.put(req, copy)); return res; })
        .catch(() => caches.match(req, { ignoreSearch: true }).then(r => r || caches.match('./cue24_scoreboard.html')))
    );
    return;
  }
  e.respondWith(
    caches.match(req).then(r => r || fetch(req).then(res => {
      if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(req, copy)); }
      return res;
    }))
  );
});
