/* Экран «Сцены»: список, переименование, дублирование, удаление, правка маршрутов и запоминание микрофонов. */
(function () {
  const B = window.Bridge;
  const rerender = () => { if (!document.getElementById('sc-modal') && window.__dc) window.__dc.setState({}); };
  const cur = () => B.settings.scenes.find((x) => x.id === window.__sceneSel) || B.settings.scenes[0];
  let delArm = 0;
  const save = () => { B.saveSettings(); rerender(); };
  const newId = () => 's' + Date.now().toString(36);
  const currentRoutes = () => (B.state ? B.state.strips.map((s) => s.r) : B.settings.strips.map(() => 0));

  document.addEventListener('click', (ev) => {
    const pick = ev.target.closest('[data-sc]');
    if (pick) { window.__sceneSel = pick.dataset.sc; rerender(); return; }
    const cell = ev.target.closest('[data-cell]');
    if (cell) {
      const [i, k] = cell.dataset.cell.split(':').map(Number), sc = cur();
      sc.routes[i] = (sc.routes[i] || 0) ^ (1 << k);
      // Если сцена сейчас включена, изменение слышно сразу (сцена остаётся активной).
      if (sc.id === B.settings.active) B.send(`set strip ${i} route ${k} ${(sc.routes[i] >> k) & 1}`);
      save();
      return;
    }
    const a = ev.target.closest('[data-sact]');
    if (!a) return;
    const sc = cur();
    switch (a.dataset.sact) {
      case 'apply': if (sc.id !== B.settings.active) { B.applyScene(sc.id); rerender(); } break;
      case 'new': {
        const n = { id: newId(), name: 'Новая сцена', desc: 'Начата с текущих маршрутов.', routes: currentRoutes() };
        B.settings.scenes.push(n); window.__sceneSel = n.id; save(); openEdit(n);
        break;
      }
      case 'dup': {
        const n = JSON.parse(JSON.stringify(sc));
        n.id = newId(); n.name = sc.name + ' (копия)';
        B.settings.scenes.push(n); window.__sceneSel = n.id; save();
        break;
      }
      case 'del':
        if (B.settings.scenes.length <= 1) { B.notice('Нужна хотя бы одна сцена.'); break; }
        if (Date.now() - delArm > 4000) { delArm = Date.now(); B.notice('Нажмите «Удалить» ещё раз, чтобы удалить сцену «' + sc.name + '».'); break; }
        delArm = 0;
        B.settings.scenes = B.settings.scenes.filter((x) => x.id !== sc.id);
        if (B.settings.active === sc.id) B.settings.active = null;
        window.__sceneSel = B.settings.scenes[0].id; save();
        break;
      case 'edit': openEdit(sc); break;
      case 'mics':
        if (Array.isArray(sc.mics)) delete sc.mics;
        else sc.mics = [0, 1].map((i) => { const m = B.micOf(i); return { g: m.g, gate: m.gate, hpf: m.hpf, comp: m.comp, ns: m.ns, amt: m.amt }; });
        save();
        break;
    }
  });

  function openEdit(sc) {
    const wrap = document.createElement('div');
    wrap.id = 'sc-modal';
    wrap.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:60';
    const box = document.createElement('div');
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', 'Название сцены');
    box.style.cssText = "width:min(480px,92vw);display:flex;flex-direction:column;gap:14px;background:var(--surface);color:var(--fg);border-radius:20px;padding:22px;box-shadow:0 10px 40px rgba(0,0,0,.35);font:400 15px/1.4 'Onest',sans-serif";
    const btn = "min-height:44px;padding:0 16px;border-radius:12px;font:500 14px 'Onest',sans-serif;border:1px solid var(--line);background:var(--surface);color:var(--fg);cursor:pointer";
    const inp = "min-height:44px;padding:0 12px;border-radius:12px;border:1px solid var(--line);background:var(--surface-2);color:var(--fg);font:500 15px 'Onest',sans-serif";
    box.innerHTML = `<div style="font:600 18px/1.2 'Unbounded',sans-serif">Сцена</div>
      <label style="display:flex;flex-direction:column;gap:6px"><span style="font-size:13px;color:var(--muted)">Название</span><input id="sm-name" maxlength="30" style="${inp}"></label>
      <label style="display:flex;flex-direction:column;gap:6px"><span style="font-size:13px;color:var(--muted)">Описание</span><input id="sm-desc" maxlength="90" style="${inp}"></label>
      <div style="display:flex;gap:10px;justify-content:flex-end"><button id="sm-cancel" style="${btn}">Отмена</button><button id="sm-ok" style="${btn};background:var(--signal);color:var(--signal-ink);border-color:var(--signal)">Сохранить</button></div>`;
    wrap.append(box); document.body.append(wrap);
    const q = (id) => box.querySelector(id);
    q('#sm-name').value = sc.name; q('#sm-desc').value = sc.desc || '';
    const close = () => { document.removeEventListener('keydown', onKey, true); wrap.remove(); rerender(); };
    const onKey = (e) => { if (e.key === 'Escape') close(); if (e.key === 'Enter') q('#sm-ok').click(); };
    document.addEventListener('keydown', onKey, true);
    q('#sm-cancel').onclick = close;
    q('#sm-ok').onclick = () => { sc.name = q('#sm-name').value.trim() || sc.name; sc.desc = q('#sm-desc').value.trim(); B.saveSettings(); close(); };
    wrap.addEventListener('pointerdown', (e) => { if (e.target === wrap) close(); });
    q('#sm-name').focus(); q('#sm-name').select();
  }

  B.on((t) => { if (t === 'state' || t === 'conn' || t === 'ready') rerender(); });
})();
