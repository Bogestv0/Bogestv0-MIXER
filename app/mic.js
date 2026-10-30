/* Экран «Микрофон»: режимы шумодава, ползунки, кнопки усиления и живые уровни фона.
   Состояние хранит движок; здесь только команды (см. bridge.js, команда «set mic»). */
(function () {
  const B = window.Bridge;
  const idx = () => micIndex();
  const find = (key) => SLIDERS.find((d) => d.key === key);

  let drag = null;       // перетаскиваемый ползунок
  let pending = {};      // команды, ждущие отправки (не чаще одного раза за кадр)
  let raf = 0;
  let lastDown = { key: '', t: 0 };

  function rerender() { if (!drag && window.__dc) window.__dc.setState({}); }

  /* ---------- кнопки ---------- */
  document.addEventListener('click', (ev) => {
    const mode = ev.target.closest('[data-mode]');
    if (mode) { B.setNs(idx(), +mode.dataset.mode); rerender(); return; }
    const q = ev.target.closest('[data-q]');
    if (q) { B.setMic(idx(), 'g', +q.dataset.q); rerender(); }
  });
  window.addEventListener('hashchange', rerender);

  /* ---------- ползунки ---------- */
  function flush() {
    raf = 0;
    const i = idx();
    Object.keys(pending).forEach((mk) => B.setMic(i, mk, pending[mk]));
    pending = {};
  }
  function showValue(d, v) {
    const root = document.querySelector(`[data-slider="${d.key}"]`);
    if (!root) return;
    const pct = (Math.max(d.lo, v) - d.lo) / (d.hi - d.lo) * 100;
    root.querySelector('[data-fill]').style.width = pct + '%';
    root.querySelector('[data-thumb]').style.left = `calc(${pct}% - 11px)`;
    const t = document.querySelector(`[data-val="${d.key}"]`);
    if (t) t.textContent = d.show(v);
  }
  function setValue(d, v) {
    v = Math.max(d.lo, Math.min(d.hi, Math.round(v / d.step) * d.step));
    showValue(d, v);
    pending[d.mk] = v;
    if (!raf) raf = requestAnimationFrame(flush);
  }
  function moveTo(ev) {
    const r = drag.el.getBoundingClientRect();
    const frac = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
    setValue(drag.d, drag.d.lo + frac * (drag.d.hi - drag.d.lo));
  }
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-slider]');
    if (!el) return;
    const d = find(el.dataset.slider);
    const now = Date.now();
    const twice = lastDown.key === d.key && now - lastDown.t < 400;   // двойное нажатие = значение по умолчанию
    lastDown = { key: d.key, t: now };
    ev.preventDefault();
    if (twice) {
      lastDown = { key: '', t: 0 };
      const def = B.MIC_DEFAULTS[idx()][d.mk];
      setValue(d, def); flush();
      return;
    }
    drag = { el, d };
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    moveTo(ev);
  });
  document.addEventListener('pointermove', (ev) => { if (drag) moveTo(ev); });
  const end = () => { if (drag) { drag = null; flush(); rerender(); } };
  document.addEventListener('pointerup', end);
  document.addEventListener('pointercancel', end);

  /* ---------- живые уровни фона (пять раз в секунду) ---------- */
  let last = 0;
  function paintMeters() {
    const now = Date.now();
    if (now - last < 200 || !B.meters || !B.meters.n) return;
    last = now;
    const N = B.meters.n[idx()];
    ['raw', 'proc'].forEach((key, k) => {
      const t = document.querySelector(`[data-ndb="${key}"]`), bar = document.querySelector(`[data-nbar="${key}"]`);
      if (t) t.textContent = dbText(N[k]);
      if (bar) bar.style.width = dbPct(N[k]) + '%';
    });
    const txt = document.querySelector('[data-ntext]');
    if (txt) txt.textContent = checkText(B.micOf(idx()), N);
  }

  B.on((type) => {
    if (type === 'meters') paintMeters();
    else if (type === 'state' || type === 'conn' || type === 'ready') rerender();
  });
})();
