/* CUE24 ⇄ 3Q Score bridge v1.0
 * 3Q Score 엔진 HTML의 </body> 바로 앞에 한 줄 추가:
 *   <script src="cue24-bridge.js"></script>
 * 엔진 판정 로직은 수정하지 않고, 완료된 샷 판정만 점수판(부모 창)으로 보냅니다.
 *   엔진 → 점수판 : {source:'3QScore', type:'ready'|'hb'|'shotStart'|'shot'|'shotUpdate', ...}
 *   점수판 → 엔진 : {target:'3QScore', cmd:'kiosk'|'full'|'start'|'stop'|'hold'|'newgame'}
 * 엔진 단독 실행(점수판 없이)일 때는 아무 동작도 하지 않습니다.
 */
(function () {
  'use strict';
  if (window.parent === window) return;
  var $ = function (id) { return document.getElementById(id); };
  var post = function (m) { m.source = '3QScore'; m.v = 1; try { window.parent.postMessage(m, '*'); } catch (e) {} };
  var g = function (name) { try { return (0, eval)(name); } catch (e) { return undefined; } };
  var sent = {}, lastStart = null, lastStatus = '';

  /* ---- 키오스크 표시: 영상(#stage)만 화면 가득 ---- */
  var kioskCss = document.createElement('style');
  kioskCss.textContent =
    'html,body{margin:0!important;background:#000!important;overflow:hidden!important}' +
    'body *{visibility:hidden!important}' +
    '#stage,#stage *{visibility:visible!important}' +
    '#stage{position:fixed!important;inset:0!important;width:100vw!important;height:100vh!important;max-width:none!important;margin:0!important;border-radius:0!important;z-index:2147483000!important;background:#000!important}' +
    '#stage video,#stage canvas{width:100%!important;height:100%!important;object-fit:contain!important}';
  function kiosk(on) {
    if (on && !kioskCss.parentNode) document.head.appendChild(kioskCss);
    if (!on && kioskCss.parentNode) kioskCss.parentNode.removeChild(kioskCss);
  }
  if (/[?&]cue24=1/.test(location.search)) kiosk(true);

  /* ---- 명령 ---- */
  function click(id) { var b = $(id); if (b && !b.disabled) { b.click(); return true; } return false; }
  function start() {
    if (g('running')) return;
    var t0 = Date.now(), lastCam = 0, lastRun = 0;
    (function wait() {
      if (g('running')) { post({ type: 'status', text: 'AI 분석 중' }); return; }
      var t = Date.now();
      if (!g('cameraActive')) { if (t - lastCam > 6000 && click('cameraBtn')) lastCam = t; }
      else if (t - lastRun > 4000 && click('runBtn')) lastRun = t;
      if (t - t0 < 45000) setTimeout(wait, 700);
      else post({ type: 'status', text: '분석 시작 실패 · 관리자 > 영상 전체 화면에서 보정' });
    })();
  }
  function markAll() {
    var shots = g('shots');
    if (Array.isArray(shots)) shots.forEach(function (s) { if (s && s.id != null) sent[s.id] = key(s); });
  }
  window.addEventListener('message', function (e) {
    var d = e.data; if (!d || d.target !== '3QScore') return;
    switch (d.cmd) {
      case 'kiosk': kiosk(true); break;
      case 'full': kiosk(false); break;
      case 'start': start(); break;
      case 'stop': click('stopBtn'); break;
      case 'newgame': markAll(); break;
      case 'hold': break;
    }
  });

  /* ---- 판정 전달 ---- */
  function info(s) {
    var result = s.result, pts = 0, cue = s.cue;
    try { result = g('effectiveResult')(s); } catch (e) {}
    try { pts = g('v39ScorePoints')(s); } catch (e) { pts = result === 'score' ? 1 : 0; }
    try { cue = g('observedCue')(s); } catch (e) {}
    return { result: result, points: pts, cue: cue };
  }
  function key(s) { var i = info(s); return i.result + '|' + i.points; }
  function poll() {
    try {
      var cs = g('currentShot');
      if (cs && cs.id !== lastStart) { lastStart = cs.id; post({ type: 'shotStart', id: cs.id }); }
      var shots = g('shots');
      if (Array.isArray(shots)) {
        if (shots.length === 0 && Object.keys(sent).length) sent = {};      // 엔진 리셋
        for (var i = 0; i < shots.length; i++) {
          var s = shots[i]; if (!s || s.id == null) continue;
          var inf = info(s), k = inf.result + '|' + inf.points;
          if (!(s.id in sent)) { sent[s.id] = k; post({ type: 'shot', id: s.id, result: inf.result, points: inf.points, cue: inf.cue, start: s.start, end: s.end }); }
          else if (sent[s.id] !== k) { sent[s.id] = k; post({ type: 'shotUpdate', id: s.id, result: inf.result, points: inf.points, cue: inf.cue }); }
        }
      }
    } catch (e) {}
  }
  setInterval(poll, 200);
  setInterval(function () {
    var v = $('verdict'), st = (g('running') ? 'AI 분석 중' : 'AI 대기') + (v && v.textContent ? ' · ' + v.textContent.trim().slice(0, 24) : '');
    post({ type: 'hb', status: st });
    if (st !== lastStatus) { lastStatus = st; }
  }, 2000);
  post({ type: 'ready' });
})();
