(() => {
  'use strict';
  const panel = document.getElementById('pwaInstallPanel');
  const button = document.getElementById('pwaInstall');
  const message = document.getElementById('pwaMessage');
  const standalone = window.matchMedia('(display-mode: standalone)');
  let installPrompt = null;
  const refresh = () => { panel.hidden = standalone.matches || navigator.standalone === true; };
  refresh();
  standalone.addEventListener('change', refresh);
  window.addEventListener('beforeinstallprompt', event => {
    event.preventDefault();
    installPrompt = event;
    message.textContent = '설치할 준비가 되었습니다. 홈 화면에 설치 버튼을 눌러 주세요.';
  });
  window.addEventListener('appinstalled', () => {
    installPrompt = null;
    panel.hidden = true;
  });
  button.addEventListener('click', async () => {
    if (!installPrompt) {
      message.textContent = location.protocol === 'file:'
        ? 'GitHub Pages의 HTTPS 주소를 Android Chrome에서 열어 주세요. 파일을 직접 열면 설치할 수 없습니다.'
        : 'Android Chrome 오른쪽 위 ⋮ → 홈 화면에 추가 → 설치를 선택하세요. 설치 항목이 없으면 잠시 후 새로고침하세요. 이미 설치했다면 홈 화면의 앱 아이콘으로 실행하세요.';
      return;
    }
    const prompt = installPrompt;
    installPrompt = null;
    button.disabled = true;
    try {
      await prompt.prompt();
      const choice = await prompt.userChoice;
      message.textContent = choice.outcome === 'accepted'
        ? '설치가 끝나면 홈 화면의 얼굴 체크인 아이콘으로 실행해 주세요.'
        : '설치를 취소했습니다. Chrome 메뉴에서도 설치할 수 있습니다.';
    } catch {
      message.textContent = 'Chrome 메뉴의 홈 화면에 추가 → 설치를 이용해 주세요.';
    } finally { button.disabled = false; }
  });
  if ('serviceWorker' in navigator && window.isSecureContext && /^https?:$/.test(location.protocol)) {
    navigator.serviceWorker.register('./sw.js', {scope: './', updateViaCache: 'none'})
      .catch(() => { message.textContent = '앱 준비에 실패했습니다. 인터넷 연결과 sw.js 업로드 상태를 확인한 뒤 새로고침해 주세요.'; });
  }
})();
