/* 3쿠션 영상 코칭 — service worker
   Scope is limited to 3qCoaching.html so it never takes over other pages in the same folder
   (e.g. 3qGuide / 3qAutoscore and their own sw.js). Videos are opened as local files (blob:)
   and are never cached here. */
const CACHE='3qcoaching-v2';
const SHELL=['./3qCoaching.html','./3qCoaching.webmanifest','./coach-192.png','./coach-512.png','./coach-maskable-512.png'];
self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(SHELL)).then(()=>self.skipWaiting()));});
self.addEventListener('activate',e=>{e.waitUntil(caches.keys().then(ks=>Promise.all(ks.filter(k=>k.startsWith('3qcoaching-')&&k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim()));});
self.addEventListener('fetch',e=>{
  const req=e.request;if(req.method!=='GET')return;
  const url=new URL(req.url);if(url.origin!==location.origin)return;
  // page: network first (picks up a new upload), cache as offline fallback
  if(req.mode==='navigate'){e.respondWith(fetch(req).then(r=>{const c=r.clone();caches.open(CACHE).then(x=>x.put('./3qCoaching.html',c));return r;}).catch(()=>caches.match('./3qCoaching.html')));return;}
  e.respondWith(caches.match(req).then(hit=>hit||fetch(req)));
});
