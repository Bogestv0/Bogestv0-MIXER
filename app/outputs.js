/* Экран «Выходы»: фейдеры и Mute шин A1–A3, B1–B2 и живые метры. Состояние хранит движок, здесь только команды. */
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
  let focusK = null;     // у какого фейдера фокус с клавиатуры (после перерисовки возвращаем)

  function rerender() { if (!drag && !knob && window.__dc) window.__dc.setState({}); }

  /* ---------- клики: Mute и перезапуск ---------- */
  document.addEventListener('click', (ev) => {
    const mute = ev.target.closest('[data-bmute]');
    if (mute) {
      const k = +mute.dataset.bmute, b = st() && st().buses[k];
      if (b) B.setBusMute(k, !b.m);
      return;
    }
    if (ev.target.closest('[data-act="restart"]')) B.restartEngine(false);
  });

  /* ---------- фейдеры ---------- */
  function flush() {
    raf = 0;
    Object.keys(pending).forEach((k) => B.setBusGain(k, pending[k]));
    pending = {};
  }
  function setGainLocal(k, db) {
    db = Math.max(-48, Math.min(12, db));
    if (st()) st().buses[k].g = db;
    const cap = $(`[data-bcap="${k}"]`);
    if (cap) cap.style.bottom = `calc(${pos(db)}% - 17px)`;
    const t = $(`[data-bdb="${k}"]`);
    if (t) t.textContent = fmt(db);
    const f = $(`[data-bfader="${k}"]`);
    if (f) { f.setAttribute('aria-valuenow', String(db)); f.setAttribute('aria-valuetext', fmt(db)); }
    pending[k] = db;
    if (!raf) raf = requestAnimationFrame(flush);
  }
  function moveTo(ev) {
    const r = drag.el.getBoundingClientRect();
    const frac = Math.max(0, Math.min(1, 1 - (ev.clientY - r.top) / r.height));
    let db = Math.round((frac * 60 - 48) * 2) / 2;      // шаг 0,5 дБ
    if (Math.abs(db) <= 1) db = 0;                       // «прилипание» к 0 дБ
    setGainLocal(drag.k, db);
  }
  let lastDown = { k: -1, t: 0 };
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-bfader]');
    if (!el || !st()) return;
    const k = +el.dataset.bfader;
    const now = Date.now();
    const twice = lastDown.k === k && now - lastDown.t < 400;   // двойное нажатие = сброс на 0 дБ
    lastDown = { k, t: now };
    if (twice) { lastDown = { k: -1, t: 0 }; setGainLocal(k, 0); flush(); ev.preventDefault(); return; }
    drag = { el, k };
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    moveTo(ev);
    ev.preventDefault();
  });
  document.addEventListener('pointermove', (ev) => { if (drag) moveTo(ev); });
  const end = () => { if (drag) { drag = null; flush(); } };
  document.addEventListener('pointerup', end);
  document.addEventListener('pointercancel', end);

  // С клавиатуры: стрелки ±1 дБ (Shift ±5), Home = 0 дБ.
  document.addEventListener('keydown', (ev) => {
    const el = ev.target.closest && ev.target.closest('[data-bfader]');
    if (!el || !st()) return;
    const k = +el.dataset.bfader, cur = st().buses[k].g, step = ev.shiftKey ? 5 : 1;
    let db = null;
    if (ev.key === 'ArrowUp' || ev.key === 'ArrowRight') db = cur + step;
    else if (ev.key === 'ArrowDown' || ev.key === 'ArrowLeft') db = cur - step;
    else if (ev.key === 'Home') db = 0;
    if (db === null) return;
    ev.preventDefault();
    focusK = k;
    setGainLocal(k, db);
  });
  /* ---------- крутилки «Бас» и «ВЧ» шины ---------- */
  // Тянуть вверх: значение растёт. 150 пикселей = весь диапазон −12…+12 дБ. Двойной щелчок = 0 дБ.
  let knob = null, knobLast = { id: '', t: 0 };
  const eqShow = (v) => (v === 0 ? '0 дБ' : (v > 0 ? '+' : '−') + Math.abs(v));
  function paintKnob(el, v) {
    const deg = Math.round((v + 12) / 24 * 270);
    el.style.background = `conic-gradient(from -135deg,var(--signal) 0deg,var(--signal) ${deg}deg,var(--line) ${deg}deg,var(--line) 270deg,transparent 270deg)`;
    if (el.firstElementChild) el.firstElementChild.style.transform = `rotate(${deg - 135}deg)`;
  }
  function knobSet(el, k, key, v) {
    v = Math.max(-12, Math.min(12, Math.round(v * 2) / 2));
    B.setEq('bus', k, key, v);
    paintKnob(el, v);
    const lab = document.querySelector(`[data-beqlabel="${k}:${key}"]`);
    if (lab) lab.textContent = eqShow(v);
    return v;
  }
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-beq]');
    if (!el || !st()) return;
    const [ks, key] = el.dataset.beq.split(':'), k = +ks, now = Date.now(), id = el.dataset.beq;
    if (knobLast.id === id && now - knobLast.t < 400) { knobLast = { id: '', t: 0 }; knobSet(el, k, key, 0); ev.preventDefault(); return; }
    knobLast = { id, t: now };
    knob = { el, k, key, y: ev.clientY, v: B.eqOf('bus', k)[key] };
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    ev.preventDefault();
  });
  document.addEventListener('pointermove', (ev) => {
    if (knob) knobSet(knob.el, knob.k, knob.key, knob.v + (knob.y - ev.clientY) / 150 * 24);
  });
  const knobEnd = () => {
    if (!knob) return;
    const lab = document.querySelector(`[data-beqlabel="${knob.k}:${knob.key}"]`);
    if (lab) lab.textContent = knob.key === 'bass' ? 'Бас' : 'ВЧ';
    knob = null;
    rerender();
  };
  document.addEventListener('pointerup', knobEnd);
  document.addEventListener('pointercancel', knobEnd);

  /* ---------- живые метры ---------- */
  let cache = [];
  function collect() {
    cache = Array.from(document.querySelectorAll('[data-bmeter]'));
    if (focusK !== null) { const f = $(`[data-bfader="${focusK}"]`); if (f) f.focus(); }
  }
  function paintMeters() {
    const M = B.meters;
    if (!M || !M.b) return;
    cache.forEach((el) => {
      const key = el.dataset.bmeter, k = parseInt(key, 10), ch = key.endsWith('l') ? 0 : 1;
      if (M.b[k]) el.style.clipPath = `inset(${100 - lvl(M.b[k][ch])}% 0 0 0)`;
    });
  }
  document.addEventListener('dc-rendered', collect);
  collect();
  // Фокус с клавиатуры помним, пока пользователь не ушёл с фейдера мышью или Tab.
  document.addEventListener('pointerdown', () => { focusK = null; });

  B.on((type) => {
    if (type === 'meters') paintMeters();
    else if (type === 'state' || type === 'conn' || type === 'ready' || type === 'devices') rerender();
  });
})();
