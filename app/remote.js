/* Страница удалённого пульта (открывается на телефоне). Говорит с программой по WebSocket (см. src-tauri/src/remote.rs).
   Тот же дизайн, что в окне микшера. Все подписи вставляются как текст (textContent), поэтому имена дорожек не могут внедрить разметку. */
(function () {
  'use strict';
  const app = document.getElementById('app');
  const ROUTES = ['A1', 'A2', 'A3', 'B1', 'B2'];
  const BUFFER_SEC = { fast: 0.06, balance: 0.12, safe: 0.25 };
  const DB_MIN = -48, DB_MAX = 12;

  let ws = null, retry = 0, pingTimer = null;
  let meta = null;            // имена дорожек, сцены, настройки прослушивания
  let st = null;              // последнее состояние движка
  let connected = false;
  let needPin = false, pinMsg = '', pin = '';
  try { pin = localStorage.getItem('bogestv0.remote.pin') || ''; } catch (e) { /* без хранилища */ }
  const dragging = new Set(); // дорожки, которые сейчас двигает палец: их положение не перезаписываем состоянием
  const ui = { strips: [], pills: null, listenCard: null, scenes: [] };

  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  const fmtDb = (db) => (db > 0 ? '+' : '') + Number(db).toFixed(1) + ' дБ';
  const pos = (db) => Math.max(0, Math.min(1, (db - DB_MIN) / (DB_MAX - DB_MIN)));
  const lvl = (lin) => Math.max(0, Math.min(100, (20 * Math.log10(Math.max(lin, 1e-6)) + 60) / 60 * 100));

  /* ---------- связь ---------- */
  function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    ws = new WebSocket(proto + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { retry = 0; send({ t: 'auth', pin }); clearInterval(pingTimer); pingTimer = setInterval(() => send({ t: 'ping' }), 15000); };
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') { playFrame(e.data); return; }
      let m; try { m = JSON.parse(e.data); } catch (x) { return; }
      handle(m);
    };
    ws.onclose = () => {
      connected = false; clearInterval(pingTimer);
      stopAudio();
      renderStatus();
      retry = Math.min(retry + 1, 6);
      setTimeout(connect, 600 * retry);
    };
    ws.onerror = () => { try { ws.close(); } catch (x) { /* уже закрыт */ } };
  }

  function handle(m) {
    switch (m.t) {
      case 'auth':
        if (m.ok) {
          connected = true; needPin = false; pinMsg = '';
          try { if (pin) localStorage.setItem('bogestv0.remote.pin', pin); } catch (x) { /* ок */ }
        } else {
          connected = false; needPin = true; pinMsg = m.msg || '';
          if (pinMsg) { pin = ''; try { localStorage.removeItem('bogestv0.remote.pin'); } catch (x) { /* ок */ } }
          renderLogin();
        }
        break;
      case 'meta': meta = m; buildMain(); break;
      case 'state': st = m; if (meta) paintState(); break;
      case 'm': if (meta) paintMeters(m); break;
      case 'err': toast(m.msg); break;
    }
  }

  let toastTimer = null;
  function toast(text) {
    let t = document.getElementById('toast');
    if (!t) { t = el('div', 'pill warn'); t.id = 'toast'; t.style.cssText = 'position:fixed;left:16px;right:16px;bottom:16px;justify-content:center;padding:10px 14px;z-index:9;white-space:normal;text-align:center'; document.body.appendChild(t); }
    t.textContent = text; t.hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
  }

  /* ---------- вход по PIN ---------- */
  function renderLogin() {
    app.replaceChildren();
    const box = el('form', 'login');
    box.appendChild(headerRow(false));
    box.appendChild(el('div', null, 'Введите PIN, который показан в настройках микшера на компьютере.'));
    const inp = el('input'); inp.type = 'password'; inp.inputMode = 'numeric'; inp.autocomplete = 'off'; inp.maxLength = 8; inp.setAttribute('aria-label', 'PIN');
    const msg = el('div', 'msg', pinMsg); msg.setAttribute('role', 'alert');
    const go = el('button', 'btn on', 'Войти'); go.type = 'submit';
    box.append(inp, msg, go);
    box.onsubmit = (e) => { e.preventDefault(); pin = inp.value.replace(/\D/g, ''); send({ t: 'auth', pin }); inp.value = ''; };
    app.appendChild(box);
    inp.focus();
  }

  /* ---------- экран ---------- */
  function headerRow(withStatus) {
    const row = el('div', 'row between');
    const left = el('div', 'row');
    const logo = el('div'); logo.style.cssText = 'width:34px;height:34px;border-radius:10px;background:#eef0f5;border:1px solid var(--line);display:flex;align-items:center;justify-content:center;flex:none';
    const img = el('img'); img.src = '/logo.png'; img.alt = 'Логотип Bogestv0'; img.style.cssText = 'width:26px;height:26px;object-fit:contain';
    logo.appendChild(img);
    const t = el('div'); const a = el('div', null, 'Bogestv0'); a.style.cssText = 'font:600 15px/1.2 var(--f-display)';
    const b = el('div', 'mono', 'MIXER · ПУЛЬТ'); b.style.cssText = 'font-size:11.5px;color:var(--muted);margin-top:2px';
    t.append(a, b); left.append(logo, t);
    row.appendChild(left);
    if (withStatus) { ui.pills = el('span', 'pill ok'); row.appendChild(ui.pills); }
    return row;
  }

  function renderStatus() {
    if (!ui.pills) return;
    ui.pills.className = 'pill ' + (connected ? 'ok' : 'warn');
    ui.pills.replaceChildren(el('i'), document.createTextNode(connected ? 'Подключено' : 'Нет связи'));
  }

  function buildMain() {
    app.replaceChildren();
    ui.strips = []; ui.scenes = [];
    app.appendChild(headerRow(true));
    renderStatus();

    // Сцены
    if (meta.scenes && meta.scenes.length) {
      const hero = el('div', 'hero');
      const top = el('div');
      top.appendChild(el('h2', null, 'Сцены'));
      const q = el('div', null, 'Куда идёт звук?'); q.style.cssText = 'font:600 18px/1.2 var(--f-display);margin:2px 0 3px';
      top.appendChild(q);
      hero.appendChild(top);
      const list = el('div', 'scenes');
      meta.scenes.forEach((sc) => {
        const b = el('button', 'btn', sc.name); b.type = 'button';
        b.onclick = () => { meta.active = sc.id; send({ t: 'scene', id: sc.id }); paintScenes(); };
        ui.scenes.push({ id: sc.id, b });
        list.appendChild(b);
      });
      hero.appendChild(list);
      app.appendChild(hero);
      paintScenes();
    }

    // Звук на телефоне
    if (meta.listen) {
      const card = el('div', 'card'); card.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:12px';
      const left = el('div'); left.style.minWidth = '0';
      left.appendChild(el('h2', null, 'Звук на телефоне'));
      const what = el('div'); what.style.cssText = 'font:600 16px/1.2 var(--f-display);margin:3px 0';
      const sub = el('div', 'mono'); sub.style.cssText = 'font-size:12px;color:var(--muted)';
      left.append(what, sub);
      const stop = el('button', 'btn bad', 'Остановить'); stop.type = 'button'; stop.style.flex = 'none';
      stop.onclick = () => stopListening();
      card.append(left, stop);
      ui.listenCard = { card, what, sub, stop };
      app.appendChild(card);
    }

    // Дорожки
    app.appendChild(el('h2', null, 'Микрофоны и программы'));
    meta.strips.forEach((s, i) => { if (!s.hidden) app.appendChild(buildStrip(s, i)); });
    paintState();
  }

  function paintScenes() {
    ui.scenes.forEach((s) => { s.b.className = 'btn' + (meta.active === s.id ? ' on' : ''); s.b.setAttribute('aria-pressed', String(meta.active === s.id)); });
  }

  function buildStrip(s, i) {
    const card = el('div', 'card'); card.style.cssText = 'display:flex;flex-direction:column;gap:10px';
    const head = el('div', 'row between'); head.style.alignItems = 'baseline';
    const name = el('span', null, s.name); name.style.cssText = 'font:600 15px var(--f-body)';
    const info = el('span', 'mono'); info.style.cssText = 'font-size:13px;color:var(--signal);font-weight:500';
    head.append(name, info);

    const body = el('div'); body.style.padding = '0 22px';
    const mL = el('div', 'meter'), mR = el('div', 'meter');
    const mw = el('div'); mw.style.cssText = 'display:flex;flex-direction:column;gap:3px;margin-bottom:6px';
    const sL = el('span'), sR = el('span'); mL.appendChild(sL); mR.appendChild(sR); mw.append(mL, mR);
    const fader = el('div', 'fader'); fader.setAttribute('role', 'slider');
    fader.setAttribute('aria-label', 'Громкость: ' + s.name); fader.setAttribute('aria-valuemin', String(DB_MIN)); fader.setAttribute('aria-valuemax', String(DB_MAX)); fader.tabIndex = 0;
    const cap = el('div', 'cap'); cap.appendChild(el('b'));
    fader.append(el('div', 'rail'), cap);
    body.append(mw, fader);

    const rbox = el('div'); rbox.appendChild(el('h2', null, 'Отправить на')); rbox.lastChild.style.marginBottom = '6px';
    const routes = el('div', 'routes');
    const rbtn = ROUTES.map((r, k) => { const b = el('button', 'btn', r); b.type = 'button'; b.onclick = () => { const cur = ((st && st.strips[i].r) >> k) & 1; send({ t: 'route', i, k, v: cur ? 0 : 1 }); if (st) { st.strips[i].r ^= (1 << k); meta.active = null; paintScenes(); paintState(); } }; routes.appendChild(b); return b; });
    rbox.appendChild(routes);

    const act = el('div', 'act');
    const mute = el('button', 'btn mute', 'Mute'), solo = el('button', 'btn solo', 'Solo'), listen = el('button', 'btn listen');
    mute.type = solo.type = listen.type = 'button';
    mute.onclick = () => { const v = st && st.strips[i].m ? 0 : 1; send({ t: 'mute', i, v }); if (st) { st.strips[i].m = v; paintState(); } };
    solo.onclick = () => { const v = st && st.strips[i].s ? 0 : 1; send({ t: 'solo', i, v }); };
    if (meta.listen) {
      const ic = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      ic.setAttribute('width', '18'); ic.setAttribute('height', '18'); ic.setAttribute('viewBox', '0 0 24 24'); ic.setAttribute('fill', 'none'); ic.setAttribute('stroke', 'currentColor'); ic.setAttribute('stroke-width', '1.9'); ic.setAttribute('stroke-linecap', 'round'); ic.setAttribute('stroke-linejoin', 'round'); ic.setAttribute('aria-hidden', 'true');
      ic.innerHTML = '<path d="M4 14v-2a8 8 0 0 1 16 0v2"/><rect x="3" y="14" width="5" height="7" rx="2"/><rect x="16" y="14" width="5" height="7" rx="2"/>';
      const lab = el('span');
      listen.append(ic, lab);
      listen.onclick = () => toggleListen(i);
      act.append(mute, solo, listen);
      ui.strips[i] = { info, cap, fader, sL, sR, rbtn, mute, solo, listen, lab, name: s.name };
    } else {
      act.append(mute, solo);
      ui.strips[i] = { info, cap, fader, sL, sR, rbtn, mute, solo, name: s.name };
    }
    card.append(head, body, rbox, act);
    bindFader(fader, i);
    return card;
  }

  /* ---------- ползунок ---------- */
  function bindFader(f, i) {
    let last = 0, pending = null, timer = null, lastTap = 0;
    const calc = (ev) => {
      const r = f.getBoundingClientRect();
      const frac = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
      return Math.round((DB_MIN + frac * (DB_MAX - DB_MIN)) * 2) / 2;
    };
    const push = (db) => {
      if (st) st.strips[i].g = db;
      paintStrip(i);
      const now = performance.now();
      if (now - last >= 33) { last = now; send({ t: 'gain', i, v: db }); pending = null; }
      else { pending = db; if (!timer) timer = setTimeout(() => { timer = null; if (pending != null) { last = performance.now(); send({ t: 'gain', i, v: pending }); pending = null; } }, 40); }
    };
    f.addEventListener('pointerdown', (ev) => {
      const now = performance.now();
      if (now - lastTap < 350) { lastTap = 0; push(0); return; }   // двойное касание: 0 дБ
      lastTap = now;
      dragging.add(i); try { f.setPointerCapture(ev.pointerId); } catch (x) { /* ок */ }
      push(calc(ev));
    });
    f.addEventListener('pointermove', (ev) => { if (dragging.has(i)) push(calc(ev)); });
    const end = () => { if (dragging.has(i)) { dragging.delete(i); if (pending != null) { send({ t: 'gain', i, v: pending }); pending = null; } } };
    f.addEventListener('pointerup', end); f.addEventListener('pointercancel', end);
    f.addEventListener('keydown', (ev) => {
      const g = st ? st.strips[i].g : 0;
      if (ev.key === 'ArrowRight' || ev.key === 'ArrowUp') { push(Math.min(DB_MAX, g + 0.5)); ev.preventDefault(); }
      else if (ev.key === 'ArrowLeft' || ev.key === 'ArrowDown') { push(Math.max(DB_MIN, g - 0.5)); ev.preventDefault(); }
    });
  }

  /* ---------- отрисовка состояния ---------- */
  function paintStrip(i) {
    const u = ui.strips[i];
    if (!u || !st || !st.strips[i]) return;
    const e = st.strips[i];
    const tag = meta.strips[i].tag;
    u.info.textContent = (tag ? tag + ' · ' : '') + fmtDb(e.g);
    u.cap.style.left = 'calc(' + (pos(e.g) * 100).toFixed(2) + '% - 22px)';
    u.cap.style.opacity = e.m ? '.5' : '1';
    u.fader.setAttribute('aria-valuenow', String(e.g));
    u.fader.setAttribute('aria-valuetext', fmtDb(e.g));
    u.rbtn.forEach((b, k) => { const on = ((e.r >> k) & 1) === 1; b.classList.toggle('on', on); b.setAttribute('aria-pressed', String(on)); });
    u.mute.classList.toggle('on', !!e.m); u.mute.setAttribute('aria-pressed', String(!!e.m));
    u.solo.classList.toggle('on', !!e.s); u.solo.setAttribute('aria-pressed', String(!!e.s));
    if (u.listen) {
      const on = st.listen === i;
      u.listen.classList.toggle('on', on); u.listen.setAttribute('aria-pressed', String(on));
      u.lab.textContent = on ? 'Слушаю' : 'Слушать';
    }
  }

  function paintState() {
    if (!st || !meta) return;
    meta.strips.forEach((s, i) => { if (!dragging.has(i)) paintStrip(i); });
    if (ui.listenCard) {
      const on = st.listen != null && st.listen >= 0;
      ui.listenCard.what.textContent = on ? 'Слушаю: ' + meta.strips[st.listen].name : 'Звук выключен';
      ui.listenCard.sub.textContent = on ? 'задержка ~' + Math.round((BUFFER_SEC[meta.buffer] || 0.12) * 1000 + 20) + ' мс' : 'нажмите «Слушать» на нужной дорожке';
      ui.listenCard.stop.hidden = !on;
      if (!on) stopAudio();
    }
  }

  function paintMeters(m) {
    for (const i in ui.strips) {
      const u = ui.strips[i];
      const v = m.s && m.s[i];
      if (!u || !v) continue;
      u.sL.style.width = (100 - lvl(v[0])).toFixed(0) + '%';
      u.sR.style.width = (100 - lvl(v[1])).toFixed(0) + '%';
    }
  }

  /* ---------- прослушивание ---------- */
  let ctx = null, nextTime = 0, playing = -1;

  function ensureAudio() {
    if (!ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) { toast('Этот браузер не умеет воспроизводить звук с пульта'); return false; }
      try { ctx = new AC({ sampleRate: 48000, latencyHint: 'interactive' }); } catch (e) { ctx = new AC(); }
    }
    if (ctx.state === 'suspended') ctx.resume();
    return true;
  }

  function toggleListen(i) {
    if (st && st.listen === i) { stopListening(); return; }
    // Контекст создаём прямо в обработчике касания: без касания браузер не разрешает звук.
    if (!ensureAudio()) return;
    nextTime = 0; playing = i;
    send({ t: 'listen', i });
  }

  function stopListening() { send({ t: 'listen', i: null }); stopAudio(); if (st) { st.listen = -1; paintState(); for (const i in ui.strips) paintStrip(Number(i)); } }
  function stopAudio() { playing = -1; nextTime = 0; }

  function playFrame(buf) {
    if (!ctx || playing < 0 || buf.byteLength < 6) return;
    const dv = new DataView(buf);
    if (dv.getUint8(0) !== 1) return;
    const frames = (buf.byteLength - 2) >> 2;
    if (!frames) return;
    const ab = ctx.createBuffer(2, frames, 48000);
    const L = ab.getChannelData(0), R = ab.getChannelData(1);
    for (let k = 0; k < frames; k++) {
      L[k] = dv.getInt16(2 + k * 4, true) / 32768;
      R[k] = dv.getInt16(4 + k * 4, true) / 32768;
    }
    const target = BUFFER_SEC[meta && meta.buffer] || 0.12;
    const now = ctx.currentTime;
    if (nextTime < now + 0.01) nextTime = now + target;              // запас кончился: накапливаем заново (будет короткая пауза)
    else if (nextTime - now > target * 3) return;                    // накопилось лишнее (сеть «выдохнула»): пропускаем, чтобы не копить задержку
    const src = ctx.createBufferSource();
    src.buffer = ab; src.connect(ctx.destination); src.start(nextTime);
    nextTime += frames / 48000;
  }

  document.addEventListener('visibilitychange', () => { if (!document.hidden && ctx && ctx.state === 'suspended') ctx.resume(); });

  renderLogin();
  app.replaceChildren(headerRow(true));
  renderStatus();
  connect();
})();
