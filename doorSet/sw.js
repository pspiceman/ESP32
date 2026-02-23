const CACHE_NAME = "door-control-pwa-v2";
const ASSETS = [
  "./",
  "./doorSet_pwa.html",
  "./manifest.webmanifest",
  "./icon-192.png",
  "./icon-512.png",
  "./doorControl.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS)));
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(keys.map((k) => (k !== CACHE_NAME ? caches.delete(k) : null))))
  );
  self.clients.claim();
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  // App Shell for navigation requests
  if (req.mode === "navigate") {
    event.respondWith((async () => {
      try {
        const net = await fetch(req);
        const cache = await caches.open(CACHE_NAME);
        cache.put(req, net.clone());
        return net;
      } catch (e) {
        const cache = await caches.open(CACHE_NAME);
        return (await cache.match("./doorSet_pwa.html")) || (await cache.match("./"));
      }
    })());
    return;
  }
  // Cache-first for other requests
  event.respondWith((async () => {
    const cached = await caches.match(req);
    if (cached) return cached;
    const res = await fetch(req);
    const cache = await caches.open(CACHE_NAME);
    cache.put(req, res.clone());
    return res;
  })());
});
