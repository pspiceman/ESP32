/* 큐24 업주(관리자 웹) · 홈 화면 앱 설치용 · 항상 최신 화면을 받도록 캐시하지 않음 */
self.addEventListener('install',()=>self.skipWaiting());
self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));
self.addEventListener('fetch',()=>{});
