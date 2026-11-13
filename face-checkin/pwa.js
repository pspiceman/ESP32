/* Q24 입장 등록 – 앱 설치(홈 화면) 지원
 · sw.js 등록 → Chrome에서 "앱 설치"/"홈 화면에 추가" 시 아이콘이 있는 앱으로 설치
 · 설정 화면의 "앱으로 설치" 버튼(#pwaInstall)과 안내(#pwaMessage) 처리 */
(()=>{
 if('serviceWorker' in navigator&&window.isSecureContext){
  addEventListener('load',()=>navigator.serviceWorker.register('./sw.js',{scope:'./'}).catch(e=>console.warn('서비스 워커 등록 실패',e)));
 }
 let deferred=null;
 const $=id=>document.getElementById(id);
 const msg=t=>{const m=$('pwaMessage');if(m)m.textContent=t};
 const installed=()=>matchMedia('(display-mode: standalone)').matches||matchMedia('(display-mode: fullscreen)').matches||navigator.standalone;
 addEventListener('beforeinstallprompt',e=>{e.preventDefault();deferred=e;const b=$('pwaInstall');if(b){b.hidden=false;b.disabled=false}msg('이 기기에 앱으로 설치할 수 있습니다. 아래 버튼을 누르세요.')});
 addEventListener('appinstalled',()=>{deferred=null;const b=$('pwaInstall');if(b)b.hidden=true;msg('앱으로 설치되었습니다. 홈 화면의 아이콘으로 실행하세요.')});
 const init=()=>{
  const b=$('pwaInstall');if(!b)return;
  if(installed()){b.hidden=true;msg('설치된 앱으로 실행 중입니다.');return}
  msg('Chrome 메뉴 ⋮ → "앱 설치" 또는 "홈 화면에 추가"를 누르면 앱으로 설치됩니다.');
  b.onclick=async()=>{
   if(!deferred){msg('Chrome 메뉴 ⋮ → "앱 설치" 또는 "홈 화면에 추가"를 눌러 주세요.');return}
   deferred.prompt();const r=await deferred.userChoice.catch(()=>({outcome:'dismissed'}));deferred=null;b.hidden=true;
   msg(r.outcome==='accepted'?'설치 중입니다… 잠시 후 홈 화면에 아이콘이 생깁니다.':'설치를 취소했습니다. 다시 하려면 새로고침하세요.');
  };
 };
 if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',init);else init();
})();
