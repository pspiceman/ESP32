'use strict';
// Only the static application shell is cached. Face records and external models are not cached here.
const PREFIX = 'face-checkin-shell-' + self.registration.scope + '-';
const CACHE = PREFIX + 'v2';
const SHELL = ['./Face_Checkin_Tablet.html', './pwa.js', './manifest.webmanifest', './icons/icon-192.png', './icons/icon-512.png'];
self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(SHELL)));
});
self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key.startsWith(PREFIX) && key !== CACHE).map(key => caches.delete(key)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  const allowed = SHELL.map(path => new URL(path, self.registration.scope).href);
  const home = new URL('./', self.registration.scope).href;
  if (!allowed.includes(url.href) && url.href !== home) return;
  event.respondWith((async () => {
    const cache = await caches.open(CACHE);
    try {
      const response = await fetch(event.request);
      if (response.ok) await cache.put(event.request, response.clone());
      return response;
    } catch {
      const cached = await cache.match(event.request);
      if (cached) return cached;
      if (event.request.mode === 'navigate') return (await cache.match('./Face_Checkin_Tablet.html')) || Response.error();
      return Response.error();
    }
  })());
});
