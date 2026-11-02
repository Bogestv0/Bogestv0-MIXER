/* Экран «Микшер»: управление мышью и живые метры. Состояние хранит движок, здесь только команды. */
(function () {
  const B = window.Bridge;
  const $ = (sel) => document.querySelector(sel);
  const st = () => B.state;
  const fmt = (db) => (db > 0 ? '+' : '') + Number(db).toFixed(1) + ' дБ';
  const pos = (db) => Math.max(0, Math.min(100, (db + 48) / 60 * 100));
  const lvl = (lin) => Math.max(0, Math.min(100, (20 * Math.log10(Math.max(lin, 1e-6)) + 60) / 60 * 100));

  let drag = null;       // перетаскиваемый фейдер
  let pending = {};      // команды усиления, ждущие отправки
  let raf = 0;

  function rerender() { if (!drag && !knob && window.__dc) window.__dc.setState({}); }

  /* ---------- клики ---------- */
  document.addEventListener('click', (ev) => {
    const el = ev.target.closest('[data-act]');
    if (!el) return;
    const act = el.dataset.act, i = +el.dataset.i;
    const e = st() && st().strips[i];
    switch (act) {
      case 'mute': if (e) B.setMute(i, !e.m); break;
      case 'solo': if (e) B.setSolo(i, !e.s); break;
      case 'route': if (e) B.setRoute(i, +el.dataset.k, !((e.r >> +el.dataset.k) & 1)); break;
      case 'scene': B.applyScene(el.dataset.id); break;
      case 'src': openSourceModal(i); break;
      case 'ns': B.setNs(i, +el.dataset.v); break;   // режим шумодава: звук меняется сразу
    }
  });

  /* ---------- окно «Источник»: название вручную или из списка запущенных программ ---------- */
  function openSourceModal(i) {
    const strip = B.settings.strips[i];
    if (!strip) return;
    const e = st() && st().strips[i];
    const wrap = document.createElement('div');
    wrap.id = 'src-modal';
    wrap.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:60';
    const box = document.createElement('div');
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', 'Источник дорожки «' + strip.name + '»');
    box.style.cssText = "width:min(420px,92vw);max-height:80vh;display:flex;flex-direction:column;gap:12px;background:var(--surface);color:var(--fg);border-radius:20px;padding:22px;box-shadow:0 10px 40px rgba(0,0,0,.35);font:400 15px/1.4 'Onest',sans-serif";
    const btn = "min-height:40px;padding:0 14px;border-radius:10px;font:500 13.5px 'Onest',sans-serif;border:1px solid var(--line);background:var(--surface);color:var(--fg);cursor:pointer;text-align:left";
    const lab = "font:600 12px 'Onest',sans-serif;text-transform:uppercase;letter-spacing:.09em;color:var(--muted)";
    box.innerHTML = `
      <div style="font:600 18px/1.2 'Unbounded',sans-serif">Источник «${strip.name}»</div>
      <label style="display:flex;flex-direction:column;gap:6px"><span style="${lab}">Название</span><input id="sm-text" maxlength="40" style="min-height:44px;padding:0 12px;border-radius:12px;border:1px solid var(--line);background:var(--surface-2);color:var(--fg);font:500 15px 'Onest',sans-serif"></label>
      <div style="display:flex;flex-direction:column;gap:6px;min-height:0">
        <span style="${lab}">Из запущенных программ</span>
        <div id="sm-list" style="display:flex;flex-direction:column;gap:5px;overflow:auto;max-height:220px">Ищу программы…</div>
      </div>
      <div style="display:flex;flex-direction:column;gap:6px">
        <span style="${lab}">Тестовый сигнал (вместо программы)</span>
        <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:5px">
          <button id="sm-t0" style="min-height:36px;border-radius:9px;font:500 13px 'Onest',sans-serif">Выкл</button>
          <button id="sm-t1" style="min-height:36px;border-radius:9px;font:500 13px 'Onest',sans-serif">Синус</button>
          <button id="sm-t2" style="min-height:36px;border-radius:9px;font:500 13px 'Onest',sans-serif">Шум</button>
        </div>
        <span style="font-size:12px;color:var(--muted)">Пока сигнал включён, на дорожке вместо названия видно «Тест: …». Выключите его, чтобы снова увидеть название.</span>
      </div>
      <div style="display:flex;gap:10px;justify-content:flex-end"><button id="sm-cancel" style="${btn};text-align:center">Отмена</button><button id="sm-ok" style="${btn};text-align:center;background:var(--signal);color:var(--signal-ink);border-color:var(--signal)">Сохранить</button></div>`;
    wrap.append(box);
    document.body.append(wrap);
    const q = (id) => box.querySelector(id);
    q('#sm-text').value = strip.source || '';
    const on = 'background:var(--signal);color:var(--signal-ink);border:1px solid var(--signal)';
    const off = 'background:var(--surface);color:var(--fg);border:1px solid var(--line)';
    const paintTest = () => {
      const v = (e && e.src) || 0;
      ['t0', 't1', 't2'].forEach((id, k) => { q('#sm-' + id).style.cssText += ';' + (v === k ? on : off); });
    };
    paintTest();
    [['t0', 'off'], ['t1', 'sine'], ['t2', 'noise']].forEach(([id, cmd]) => {
      q('#sm-' + id).onclick = () => { B.setSrc(i, cmd); if (st() && st().strips[i]) st().strips[i].src = { off: 2, sine: 0, noise: 1 }[cmd]; paintTest(); };
    });
    const close = () => { document.removeEventListener('keydown', onKey, true); wrap.remove(); rerender(); };
    const onKey = (ev) => { if (ev.key === 'Escape') close(); };
    document.addEventListener('keydown', onKey, true);
    q('#sm-cancel').onclick = close;
    wrap.addEventListener('pointerdown', (ev) => { if (ev.target === wrap) close(); });
    q('#sm-ok').onclick = () => {
      const v = q('#sm-text').value.trim().slice(0, 40);
      strip.source = v || B.STRIP_DEFAULTS[i].source;
      B.saveSettings();
      close();
    };
    B.listProcesses().then((names) => {
      const list = q('#sm-list');
      if (!wrap.isConnected) return;
      if (!names.length) { list.textContent = 'Список недоступен. Впишите название вручную.'; return; }
      list.innerHTML = '';
      names.forEach((name) => {
        const b = document.createElement('button');
        b.textContent = name;
        b.style.cssText = btn;
        b.onclick = () => { q('#sm-text').value = name; };
        list.append(b);
      });
    }).catch(() => { if (wrap.isConnected) q('#sm-list').textContent = 'Список недоступен. Впишите название вручную.'; });
    q('#sm-text').focus();
  }

  /* ---------- фейдеры ---------- */
  function flush() {
    raf = 0;
    Object.keys(pending).forEach((i) => B.setGain(i, pending[i]));
    pending = {};
  }
  function setGainLocal(i, db) {
    if (st()) st().strips[i].g = db;
    const cap = $(`[data-cap="${i}"]`);
    if (cap) cap.style.bottom = `calc(${pos(db)}% - 17px)`;
    const t = $(`[data-db="${i}"]`);
    if (t) t.textContent = `${B.settings.strips[i].tag} · ${fmt(db)}`;
    pending[i] = db;
    if (!raf) raf = requestAnimationFrame(flush);
  }
  function moveTo(ev) {
    const r = drag.el.getBoundingClientRect();
    const frac = Math.max(0, Math.min(1, 1 - (ev.clientY - r.top) / r.height));
    let db = Math.round((frac * 60 - 48) * 2) / 2;      // шаг 0,5 дБ
    if (Math.abs(db) <= 1) db = 0;                       // «прилипание» к 0 дБ
    setGainLocal(drag.i, db);
  }
  let lastDown = { i: -1, t: 0 };
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-fader]');
    if (!el || !st()) return;
    const i = +el.dataset.fader;
    const now = Date.now();
    const twice = lastDown.i === i && now - lastDown.t < 400;   // двойное нажатие = сброс на 0 дБ
    lastDown = { i, t: now };
    if (twice) { lastDown = { i: -1, t: 0 }; setGainLocal(i, 0); flush(); ev.preventDefault(); return; }
    drag = { el, i };
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    moveTo(ev);
    ev.preventDefault();
  });
  document.addEventListener('pointermove', (ev) => { if (drag) moveTo(ev); });
  // Перерисует ответ движка (состояние придёт сразу после команды), поэтому здесь DOM не трогаем.
  const end = () => { if (drag) { drag = null; flush(); } };
  document.addEventListener('pointerup', end);
  document.addEventListener('pointercancel', end);


  /* ---------- крутилки «Гейт» и «Компр.» на микрофонах ---------- */
  // Тянуть вверх: значение растёт. 150 пикселей = весь диапазон. Двойной щелчок = значение по умолчанию.
  // Бас и ВЧ на виртуальных дорожках: полки ±12 дБ (движок: engine/eq.h). Двойной щелчок = 0 дБ.
  const eqShow = (v) => (v === 0 ? '0 дБ' : (v > 0 ? '+' : '−') + Math.abs(v));
  const KNOBS = {
    gate: { key: 'gate', lo: -80, hi: -20, step: 1, label: 'Гейт', def: () => B.MIC_DEFAULTS[0].gate, show: (v) => (v <= -79 ? 'выкл' : '−' + Math.abs(v)), get: (i) => B.micOf(i).gate },
    comp: { key: 'comp', lo: 1, hi: 10, step: 0.5, label: 'Компр.', def: () => B.MIC_DEFAULTS[0].comp, show: (v) => (v <= 1.04 ? 'выкл' : v + ':1'), get: (i) => B.micOf(i).comp },
    bass: { key: 'bass', lo: -12, hi: 12, step: 0.5, label: 'Бас', def: () => 0, show: eqShow, eq: true, get: (i) => B.eqOf('strip', i).bass },
    treble: { key: 'treble', lo: -12, hi: 12, step: 0.5, label: 'ВЧ', def: () => 0, show: eqShow, eq: true, get: (i) => B.eqOf('strip', i).treble }
  };
  let knob = null, knobLast = { id: '', t: 0 };
  function paintKnob(el, frac) {
    const deg = Math.round(frac * 270);
    el.style.background = `conic-gradient(from -135deg,var(--signal) 0deg,var(--signal) ${deg}deg,var(--line) ${deg}deg,var(--line) 270deg,transparent 270deg)`;
    if (el.firstElementChild) el.firstElementChild.style.transform = `rotate(${deg - 135}deg)`;
  }
  function knobSet(k, i, v, el) {
    const d = KNOBS[k];
    v = Math.max(d.lo, Math.min(d.hi, Math.round(v / d.step) * d.step));
    if (d.eq) B.setEq('strip', i, d.key, v); else B.setMic(i, d.key, v);
    paintKnob(el, (v - d.lo) / (d.hi - d.lo));
    const lab = document.querySelector(`[data-klabel="${k}${i}"]`);
    if (lab) lab.textContent = d.show(v);
    return v;
  }
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-knob]');
    if (!el || !KNOBS[el.dataset.knob] || !st()) return;
    const k = el.dataset.knob, i = +el.dataset.i, d = KNOBS[k];
    const id = k + i, now = Date.now();
    if (knobLast.id === id && now - knobLast.t < 400) { knobLast = { id: '', t: 0 }; knobSet(k, i, d.def(), el); ev.preventDefault(); return; }
    knobLast = { id, t: now };
    const cur = d.get(i);
    knob = { el, k, i, y: ev.clientY, frac: (cur - d.lo) / (d.hi - d.lo) };
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    ev.preventDefault();
  });
  document.addEventListener('pointermove', (ev) => {
    if (!knob) return;
    const d = KNOBS[knob.k];
    const frac = Math.max(0, Math.min(1, knob.frac + (knob.y - ev.clientY) / 150));
    knobSet(knob.k, knob.i, d.lo + frac * (d.hi - d.lo), knob.el);
  });
  const knobEnd = () => {
    if (!knob) return;
    const lab = document.querySelector(`[data-klabel="${knob.k}${knob.i}"]`);
    if (lab) lab.textContent = KNOBS[knob.k].label;
    knob = null;
    rerender();
  };
  document.addEventListener('pointerup', knobEnd);
  document.addEventListener('pointercancel', knobEnd);

  /* ---------- живые метры ---------- */
  let cache = [];
  function collect() { cache = Array.from(document.querySelectorAll('[data-meter]')); }
  function paintMeters() {
    const M = B.meters;
    if (!M) return;
    cache.forEach((el) => {
      const key = el.dataset.meter, i = parseInt(key, 10), ch = key.endsWith('l') ? 0 : 1;
      if (M.s[i]) el.style.height = (100 - lvl(M.s[i][ch])) + '%';
    });
  }
  document.addEventListener('dc-rendered', collect);
  collect();

  B.on((type) => {
    if (type === 'meters') paintMeters();
    else if (type === 'state' || type === 'conn' || type === 'ready') rerender();
  });
})();
