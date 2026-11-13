/* Q24 입장 등록 서비스 워커
 · 화면(HTML)은 항상 인터넷에서 최신 파일을 먼저 받음(네트워크 우선) → 새로고침하면 바로 새 버전
 · 인터넷이 끊겼을 때만 마지막으로 받은 화면을 보여 줌
 · 얼굴 모델·MQTT 등 다른 주소의 파일은 건드리지 않음 */
const PAGE_CACHE='q24-pages-v1';
self.addEventListener('install',()=>self.skipWaiting());
self.addEventListener('activate',e=>e.waitUntil((async()=>{
 for(const k of await caches.keys())if(k.startsWith('q24-pages')&&k!==PAGE_CACHE)await caches.delete(k);
 await self.clients.claim();
})()));
const OFFLINE='<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Q24 입장 등록</title>'+
 '<body style="margin:0;height:100vh;display:grid;place-items:center;background:#0b161b;color:#fff;font:20px system-ui,sans-serif;text-align:center">'+
 '<div><div style="font-size:64px">📶</div><h1 style="font-size:28px">인터넷에 연결되어 있지 않습니다</h1><p style="color:#c0d4d6">와이파이를 확인한 뒤 다시 시도해 주세요.</p>'+
 '<button onclick="location.reload()" style="font:inherit;padding:14px 28px;border-radius:30px;border:0;background:#e8402f;color:#fff">다시 시도</button></div>'+
 '<script>addEventListener("online",()=>location.reload())<\/script></body>';
self.addEventListener('fetch',e=>{
 const r=e.request;if(r.method!=='GET')return;
 const u=new URL(r.url);if(u.origin!==self.location.origin)return;
 const isPage=r.mode==='navigate'||u.pathname.endsWith('.html')||u.pathname.endsWith('/');
 if(!isPage)return;   // 아이콘·manifest 등은 브라우저 기본 처리
 e.respondWith((async()=>{
  const key=u.origin+u.pathname;
  try{
   const res=await fetch(r,{cache:'no-store'});
   if(res.ok&&!u.searchParams.has('chk')){try{(await caches.open(PAGE_CACHE)).put(key,res.clone())}catch{}}
   return res;
  }catch(err){
   const hit=await (await caches.open(PAGE_CACHE)).match(key);
   if(hit)return hit;
   if(r.mode==='navigate')return new Response(OFFLINE,{headers:{'content-type':'text/html; charset=utf-8'}});
   throw err;
  }
 })());
});
