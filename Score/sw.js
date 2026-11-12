/* CUE24 Score Board · service worker v3
 * - 홈 화면 설치(PWA) 조건 충족 + 오프라인 실행
 * - 설치 시 파일을 하나씩 캐시 → 파일 하나가 없어도 설치 실패하지 않음 (v1의 태블릿 문제 원인 중 하나)
 * - 점수판 HTML은 네트워크 우선(수정본 즉시 반영), 실패 시 캐시
 * - 아이콘 등 정적 파일은 캐시 우선
 * 파일을 수정해 배포할 때 VERSION을 올리면 이전 캐시가 정리됩니다.
 */
const VERSION = 'cue24-v3';
const APP = './cue24_scoreboard.html';
const CORE = [
  APP, './index.html', './manifest.webmanifest', './favicon.ico', './favicon-32.png', './apple-touch-icon.png',
  './icon-48.png', './icon-72.png', './icon-96.png', './icon-144.png', './icon-192.png', './icon-512.png',
  './icon-maskable-192.png', './icon-maskable-512.png'
];

self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(VERSION)
      .then(c => Promise.all(CORE.map(u => c.add(new Request(u, { cache: 'reload' })).catch(() => null))))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k.startsWith('cue24-') && k !== VERSION).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  // 같은 출처만 처리 (카메라 스트림, 호출 서버 등은 건드리지 않음) · AI 엔진/브릿지는 항상 네트워크
  if (url.origin !== location.origin) return;
  if (/3Q_Score|cue24-bridge\.js/.test(url.pathname)) return;

  if (req.mode === 'navigate' || url.pathname.endsWith('.html') || url.pathname.endsWith('/')) {
    e.respondWith(
      fetch(req)
        .then(res => {
          if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(req, copy)); }
          return res;
        })
        .catch(() => caches.match(req, { ignoreSearch: true }).then(r => r || caches.match(APP)))
    );
    return;
  }
  e.respondWith(
    caches.match(req, { ignoreSearch: true }).then(r => r || fetch(req).then(res => {
      if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(req, copy)); }
      return res;
    }))
  );
});
