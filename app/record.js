/* Экран «Запись»: выбор шин, режим файлов, папка, старт и стоп, таймер, уровни и список записей. Саму запись делает движок. */
(function () {
  const B = window.Bridge;
  const $ = (sel) => document.querySelector(sel);
  const on = () => !!(B.state && B.state.rec && B.state.rec.on);
  const rerender = () => { if (!$('#rec-modal') && window.__dc) window.__dc.setState({}); };
  const lvl = (lin) => Math.max(0, Math.min(100, (20 * Math.log10(Math.max(lin, 1e-6)) + 60) / 60 * 100));
  let delArm = { name: '', t: 0 };

  document.addEventListener('click', (ev) => {
    const bus = ev.target.closest('[data-rbus]');
    if (bus) {
      if (on()) { B.notice('Во время записи менять выбор нельзя. Остановите запись.'); return; }
      const k = +bus.dataset.rbus;
      const cur = B.state && B.state.rec ? B.state.rec.mask : B.settings.rec.mask;
      if (B.state && B.state.rec) B.state.rec.mask = cur ^ (1 << k);
      B.recSet({ mask: cur ^ (1 << k) });
      return;
    }
    const mode = ev.target.closest('[data-rmode]');
    if (mode) {
      if (on()) { B.notice('Во время записи менять режим нельзя. Остановите запись.'); return; }
      B.recSet({ mode: mode.dataset.rmode });
      return;
    }
    if (ev.target.closest('[data-act="rdir"]')) { if (on()) B.notice('Папку можно менять, когда запись остановлена.'); else openDir(); return; }
    if (ev.target.closest('[data-act="rtoggle"]')) {
      if (!B.state) return;
      if (on()) B.recStop();
      else if (!(B.state.rec.mask & 31)) B.notice('Выберите хотя бы один выход для записи.');
      else B.recStart();
      return;
    }
    const rv = ev.target.closest('[data-rreveal]');
    if (rv) { const d = (B.recs && B.recs.dir) || B.settings.rec.dir; B.reveal(d + (d.includes('\\') ? '\\' : '/') + rv.dataset.rreveal); return; }
    const del = ev.target.closest('[data-rdel]');
    if (del) {
      // Удаление необратимо: первое нажатие просит подтвердить, второе в течение 4 секунд удаляет.
      const name = del.dataset.rdel, now = Date.now();
      if (delArm.name === name && now - delArm.t < 4000) { delArm = { name: '', t: 0 }; B.recDelete(name); }
      else { delArm = { name, t: now }; B.notice('Нажмите «Удалить» ещё раз, чтобы стереть «' + name + '» насовсем.'); }
    }
  });

  function openDir() {
    const wrap = document.createElement('div');
    wrap.id = 'rec-modal';
    wrap.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:60';
    const box = document.createElement('div');
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', 'Папка для записей');
    box.style.cssText = "width:min(520px,92vw);display:flex;flex-direction:column;gap:14px;background:var(--surface);color:var(--fg);border-radius:20px;padding:22px;box-shadow:0 10px 40px rgba(0,0,0,.35);font:400 15px/1.4 'Onest',sans-serif";
    const btn = "min-height:44px;padding:0 16px;border-radius:12px;font:500 14px 'Onest',sans-serif;border:1px solid var(--line);background:var(--surface);color:var(--fg);cursor:pointer";
    box.innerHTML = `<div style="font:600 18px/1.2 'Unbounded',sans-serif">Папка для записей</div>
      <label style="display:flex;flex-direction:column;gap:6px"><span style="font-size:13px;color:var(--muted)">Полный путь. Если папки нет, программа создаст её при первой записи.</span><input id="rm-dir" style="min-height:44px;padding:0 12px;border-radius:12px;border:1px solid var(--line);background:var(--surface-2);color:var(--fg);font:500 13px 'JetBrains Mono',monospace"></label>
      <div style="display:flex;gap:10px;justify-content:space-between"><button id="rm-def" style="${btn}">По умолчанию</button><div style="display:flex;gap:10px"><button id="rm-cancel" style="${btn}">Отмена</button><button id="rm-ok" style="${btn};background:var(--signal);color:var(--signal-ink);border-color:var(--signal)">Сохранить</button></div></div>`;
    wrap.append(box);
    document.body.append(wrap);
    const input = box.querySelector('#rm-dir');
    input.value = B.settings.rec.dir || '';
    const close = () => { document.removeEventListener('keydown', onKey, true); wrap.remove(); rerender(); };
    const onKey = (e) => { if (e.key === 'Escape') close(); if (e.key === 'Enter') box.querySelector('#rm-ok').click(); };
    document.addEventListener('keydown', onKey, true);
    box.querySelector('#rm-cancel').onclick = close;
    box.querySelector('#rm-def').onclick = () => { if (B.dirs) input.value = B.dirs.rec; };
    box.querySelector('#rm-ok').onclick = () => {
      const v = input.value.trim();
      if (v) { B.recSet({ dir: v }); B.recList(); }
      close();
    };
    wrap.addEventListener('pointerdown', (e) => { if (e.target === wrap) close(); });
    input.focus();
  }

  /* ---------- таймер и уровни ---------- */
  const clock = (sec) => {
    sec = Math.max(0, Math.floor(sec));
    return [Math.floor(sec / 3600), Math.floor(sec % 3600 / 60), sec % 60].map((x) => String(x).padStart(2, '0')).join(':');
  };
  let meters = [];
  function collect() { meters = Array.from(document.querySelectorAll('[data-rmeter]')); paintTimer(); }
  function paintTimer() {
    const t = $('[data-rtimer]');
    const M = B.meters;
    if (t) t.textContent = on() && M && M.r ? clock(M.r[1]) : '00:00:00';
  }
  function paintMeters() {
    const M = B.meters;
    if (!M || !M.b) return;
    meters.forEach((el) => {
      const b = M.b[+el.dataset.rmeter];
      if (b) el.style.width = lvl(Math.max(b[0], b[1])) + '%';
    });
    paintTimer();
    if (on() && M.r && M.r[2] > 0 && !B.dropWarned) { B.dropWarned = true; B.notice('Запись не успевает за диском: часть звука потеряна. Выберите папку на более быстром диске.'); }
  }
  document.addEventListener('dc-rendered', collect);
  collect();

  let sig = '';
  B.on((type) => {
    if (type === 'meters') paintMeters();
    else if (type === 'state') {
      const r = B.state && B.state.rec;
      const s = r ? JSON.stringify(r) : '';
      if (s !== sig) { sig = s; if (!r || !r.on) B.dropWarned = false; rerender(); }
    } else if (type === 'recs' || type === 'conn' || type === 'ready') rerender();
  });
  if (B.state) B.recList();
  B.on((t) => { if (t === 'ready') B.recList(); });
})();
