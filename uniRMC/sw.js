/* Universal RMC PWA Service Worker */
const CACHE_NAME = 'universal-rmc-v1';
const APP_SHELL = [
  './',
  './uniRMC_pwa.html',
  './manifest.json',
  './sw.js',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/icon-192-maskable.png',
  './icons/icon-512-maskable.png'
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE_NAME);
    await cache.addAll(APP_SHELL);
    self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.map(k => (k !== CACHE_NAME) ? caches.delete(k) : Promise.resolve()));
    self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  const pathname = url.pathname;
  const isShell = APP_SHELL.some(p => pathname.endsWith(p.replace('./','')));

  if (isShell) {
    event.respondWith((async () => {
      const cache = await caches.open(CACHE_NAME);
      const cached = await cache.match(req, { ignoreSearch: true });
      if (cached) return cached;
      const res = await fetch(req);
      cache.put(req, res.clone());
      return res;
    })());
    return;
  }

  event.respondWith((async () => {
    const cache = await caches.open(CACHE_NAME);
    try{
      const res = await fetch(req);
      cache.put(req, res.clone());
      return res;
    }catch(e){
      const cached = await cache.match(req, { ignoreSearch: true });
      if (cached) return cached;
      return cache.match('./uniRMC_pwa.html');
    }
  })());
});
