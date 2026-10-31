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
      case 'src': if (e) B.setSrc(i, ['sine', 'noise', 'off'][e.src === 0 ? 0 : e.src === 1 ? 1 : 2]); break;
      case 'ns': B.setNs(i, +el.dataset.v); break;   // режим шумодава: звук меняется сразу
    }
  });

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
