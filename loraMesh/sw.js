const CACHE_NAME = "lora-mesh-pwa-v1";
const ASSETS = [
  "/loraMesh/",
  "/loraMesh/loraMesh.html",
  "/loraMesh/manifest.json",
  "/loraMesh/loraMesh.png"
];

// 설치 시 캐시
self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS))
  );
  self.skipWaiting();
});

// 오래된 캐시 정리
self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.map((k) => (k !== CACHE_NAME ? caches.delete(k) : null)))
    )
  );
  self.clients.claim();
});

// 네트워크 우선, 실패 시 캐시
self.addEventListener("fetch", (event) => {
  event.respondWith(
    fetch(event.request).catch(() => caches.match(event.request))
  );
});
