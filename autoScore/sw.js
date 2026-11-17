/* 큐24 자동득점 서비스워커
   - 앱 파일을 캐시해 두어 인터넷이 없어도 실행됩니다.
   - 온라인이면 항상 서버의 최신 3qAutoscore.html을 먼저 가져옵니다(업데이트 즉시 반영).
   - 3qAutoscore.html을 바꿔 올릴 때 아래 버전 숫자를 올리면 예전 캐시가 정리됩니다. */
const CACHE = 'q24-autoscore-v2';
const FILES = ['./3qAutoscore.html', './manifest.json', './icon-192.png', './icon-512.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return; // 외부 요청(ntfy 등)은 그대로 통과
  if (req.mode === 'navigate') {
    // 화면(HTML): 네트워크 우선 → 실패 시 캐시
    e.respondWith(
      fetch(req)
        .then(res => { const copy = res.clone(); caches.open(CACHE).then(c => c.put('./3qAutoscore.html', copy)); return res; })
        .catch(() => caches.match('./3qAutoscore.html'))
    );
    return;
  }
  // 아이콘·manifest 등: 캐시 우선
  e.respondWith(caches.match(req).then(hit => hit || fetch(req)));
});
