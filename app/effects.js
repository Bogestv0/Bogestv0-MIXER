/* Экран «Эффекты»: 16 кнопок со звуками, на какие выходы они играют, общая громкость, клавиши F1–F12 (пока окно в фокусе). */
(function () {
  const B = window.Bridge;
  const $ = (sel) => document.querySelector(sel);
  const rerender = () => { if (!gdrag && !$('#pad-modal') && window.__dc) window.__dc.setState({}); };

  /* ---------- клики ---------- */
  document.addEventListener('click', (ev) => {
    const out = ev.target.closest('[data-prout]');
    if (out) { const k = +out.dataset.prout; const m = B.state && B.state.pads ? B.state.pads.r : B.settings.padOut; B.setPadRoute(k, !((m >> k) & 1)); return; }
    if (ev.target.closest('[data-act="pstop"]')) { B.padStopAll(); return; }
    if (ev.target.closest('[data-act="padadd"]')) {
      const free = B.settings.pads.findIndex((p) => !p.file);
      if (free >= 0) openModal(free); else B.notice('Все 16 кнопок заняты. Удалите ненужный звук.');
      return;
    }
    const ed = ev.target.closest('[data-padedit]');
    if (ed) { openModal(+ed.dataset.padedit); return; }
    const pad = ev.target.closest('[data-pad]');
    if (pad) B.padPlay(+pad.dataset.pad);
  });
  document.addEventListener('keydown', (ev) => {
    const pad = ev.target.closest && ev.target.closest('[data-pad]');
    if (pad && (ev.key === 'Enter' || ev.key === ' ') && !ev.target.closest('[data-padedit]')) { ev.preventDefault(); B.padPlay(+pad.dataset.pad); }
  });

  /* ---------- громкость звуков ---------- */
  let gdrag = false;
  function gainTo(ev) {
    const el = $('[data-pgain]');
    if (!el) return;
    const r = el.getBoundingClientRect();
    const frac = Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width));
    let db = Math.round((frac * 60 - 48) * 2) / 2;
    if (Math.abs(db) <= 1) db = 0;
    paintGain(db);
    B.setPadGain(db);
  }
  function paintGain(db) {
    const pct = Math.max(0, Math.min(100, (db + 48) / 60 * 100));
    const f = $('[data-pgfill]'), c = $('[data-pgcap]'), t = $('[data-pgtxt]'), s = $('[data-pgain]');
    if (f) f.style.width = pct + '%';
    if (c) c.style.left = `calc(${pct}% - 11px)`;
    if (t) t.textContent = (db > 0 ? '+' : db < 0 ? '−' : '') + Math.abs(db).toFixed(1).replace('.0', '') + ' дБ';
    if (s) s.setAttribute('aria-valuenow', String(db));
  }
  document.addEventListener('pointerdown', (ev) => {
    const el = ev.target.closest('[data-pgain]');
    if (!el) return;
    gdrag = true;
    try { el.setPointerCapture(ev.pointerId); } catch (e) { /* не критично */ }
    gainTo(ev);
    ev.preventDefault();
  });
  document.addEventListener('pointermove', (ev) => { if (gdrag) gainTo(ev); });
  const gEnd = () => { if (!gdrag) return; gdrag = false; B.saveSettings(); rerender(); };
  document.addEventListener('pointerup', gEnd);
  document.addEventListener('pointercancel', gEnd);
  document.addEventListener('keydown', (ev) => {
    const el = ev.target.closest && ev.target.closest('[data-pgain]');
    if (!el || !B.state) return;
    const d = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[ev.key];
    if (!d) return;
    ev.preventDefault();
    const db = Math.max(-48, Math.min(12, B.settings.padGain + d * (ev.shiftKey ? 3 : 0.5)));
    paintGain(db); B.setPadGain(db); B.saveSettings();
  });

  /* ---------- клавиши ---------- */
  function keyName(ev) {
    if (/^F([1-9]|1[0-2])$/.test(ev.key)) return ev.key;
    if (/^Key[A-Z]$/.test(ev.code)) return ev.code.slice(3);
    if (/^Digit[0-9]$/.test(ev.code)) return ev.code.slice(5);
    return '';
  }
  document.addEventListener('keydown', (ev) => {
    if ($('#pad-modal') || ev.ctrlKey || ev.metaKey || ev.altKey || ev.repeat) return;
    const t = ev.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    const name = keyName(ev);
    if (!name) return;
    const i = B.settings.pads.findIndex((p) => p.file && p.key === name);
    if (i < 0) return;
    ev.preventDefault();
    B.padPlay(i);
  });

  /* ---------- окно добавления и изменения звука ---------- */
  function openModal(i) {
    const cur = B.settings.pads[i];
    const isNew = !cur.file;
    const draft = { name: cur.name, key: cur.key, file: null };
    const wrap = document.createElement('div');
    wrap.id = 'pad-modal';
    wrap.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:60';
    const box = document.createElement('div');
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', isNew ? 'Новый звук' : 'Изменить звук');
    box.style.cssText = "width:min(460px,92vw);display:flex;flex-direction:column;gap:14px;background:var(--surface);color:var(--fg);border-radius:20px;padding:22px;box-shadow:0 10px 40px rgba(0,0,0,.35);font:400 15px/1.4 'Onest',sans-serif";
    const btn = 'min-height:44px;padding:0 16px;border-radius:12px;font:500 14px \'Onest\',sans-serif;border:1px solid var(--line);background:var(--surface);color:var(--fg);cursor:pointer';
    const lab = 'font:600 12px \'Onest\',sans-serif;text-transform:uppercase;letter-spacing:.09em;color:var(--muted)';
    box.innerHTML = `
      <div style="font:600 18px/1.2 'Unbounded',sans-serif">${isNew ? 'Новый звук' : 'Изменить звук'}</div>
      <label style="display:flex;flex-direction:column;gap:6px"><span style="${lab}">Название</span><input id="pm-name" maxlength="40" style="min-height:44px;padding:0 12px;border-radius:12px;border:1px solid var(--line);background:var(--surface-2);color:var(--fg);font:500 15px 'Onest',sans-serif"></label>
      <div style="display:flex;flex-direction:column;gap:6px"><span style="${lab}">Файл</span><div style="display:flex;gap:10px;align-items:center"><button id="pm-pick" style="${btn}">${isNew ? 'Выбрать файл…' : 'Заменить файл…'}</button><span id="pm-file" style="font:500 13px 'JetBrains Mono',monospace;color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0"></span></div>
        <span style="font-size:12.5px;color:var(--muted)">WAV, MP3 или FLAC, до 20 секунд. Копия хранится в папке программы.</span><input id="pm-input" type="file" accept="audio/*,.wav,.mp3,.flac,.ogg" hidden></div>
      <div style="display:flex;flex-direction:column;gap:6px"><span style="${lab}">Клавиша</span><div style="display:flex;gap:10px;align-items:center"><button id="pm-key" style="${btn};min-width:160px"></button><button id="pm-keyclear" style="${btn}">Без клавиши</button></div></div>
      <div id="pm-err" role="alert" style="min-height:18px;font-size:13px;color:var(--bad)"></div>
      <div style="display:flex;gap:10px;justify-content:space-between"><div>${isNew ? '' : `<button id="pm-del" style="${btn};color:var(--bad);border-color:var(--bad)">Удалить</button>`}</div><div style="display:flex;gap:10px"><button id="pm-cancel" style="${btn}">Отмена</button><button id="pm-ok" style="${btn};background:var(--signal);color:var(--signal-ink);border-color:var(--signal)">Сохранить</button></div></div>`;
    wrap.append(box);
    document.body.append(wrap);
    const q = (id) => box.querySelector(id);
    q('#pm-name').value = draft.name;
    const paintKey = (listening) => { q('#pm-key').textContent = listening ? 'Нажмите клавишу…' : (draft.key || 'не задана'); };
    paintKey(false);
    q('#pm-file').textContent = isNew ? 'не выбран' : 'уже загружен';
    let listening = false;
    const err = (m) => { q('#pm-err').textContent = m || ''; };
    const close = () => { document.removeEventListener('keydown', onKey, true); wrap.remove(); rerender(); };
    function onKey(ev) {
      if (listening) {
        ev.preventDefault(); ev.stopPropagation();
        if (ev.key === 'Escape') { listening = false; paintKey(false); return; }
        const n = keyName(ev);
        if (!n) { err('Подойдут F1–F12, буквы и цифры.'); return; }
        const other = B.settings.pads.findIndex((p, j) => j !== i && p.file && p.key === n);
        if (other >= 0) { err('Клавиша ' + n + ' уже занята: «' + (B.settings.pads[other].name || 'Звук ' + (other + 1)) + '».'); return; }
        err(''); draft.key = n; listening = false; paintKey(false);
        return;
      }
      if (ev.key === 'Escape') close();
    }
    document.addEventListener('keydown', onKey, true);
    q('#pm-key').onclick = () => { listening = true; err(''); paintKey(true); };
    q('#pm-keyclear').onclick = () => { draft.key = ''; listening = false; paintKey(false); };
    q('#pm-pick').onclick = () => q('#pm-input').click();
    q('#pm-input').onchange = (e) => {
      const f = e.target.files[0];
      if (!f) return;
      if (f.size > 16 * 1024 * 1024) { err('Файл слишком большой. Нужен короткий звук (до 20 секунд).'); return; }
      draft.file = f; q('#pm-file').textContent = f.name; err('');
      if (!q('#pm-name').value.trim()) q('#pm-name').value = f.name.replace(/\.[^.]+$/, '').slice(0, 40);
    };
    q('#pm-cancel').onclick = close;
    wrap.addEventListener('pointerdown', (e) => { if (e.target === wrap) close(); });
    const del = q('#pm-del');
    if (del) del.onclick = () => { B.padDelete(i); close(); };
    q('#pm-ok').onclick = async () => {
      if (isNew && !draft.file) { err('Выберите файл со звуком.'); return; }
      const name = q('#pm-name').value.trim();
      q('#pm-ok').disabled = true;
      try {
        if (draft.file) await B.importSound(i, draft.file);
        B.padEdit(i, { name: name || (draft.file ? draft.file.name.replace(/\.[^.]+$/, '') : 'Звук ' + (i + 1)), key: draft.key });
        close();
      } catch (e) { err(String(e && e.message ? e.message : e)); q('#pm-ok').disabled = false; }
    };
    q('#pm-name').focus();
  }

  /* ---------- ход воспроизведения ---------- */
  let cache = [];
  function collect() { cache = Array.from(document.querySelectorAll('[data-pbar]')); }
  function paintProgress() {
    const M = B.meters;
    if (!M || !M.p) return;
    cache.forEach((el) => { const v = M.p[+el.dataset.pbar]; el.style.width = (v >= 0 ? Math.round(v * 100) : 0) + '%'; });
  }
  document.addEventListener('dc-rendered', collect);
  collect();

  let sig = '';
  B.on((type) => {
    if (type === 'meters') paintProgress();
    else if (type === 'state') {
      // Перерисовываем, только если изменилось то, что видно: что играет, длины, куда играют, громкость.
      const p = B.state && B.state.pads;
      const s = p ? JSON.stringify([p.g, p.r, p.l, p.p]) : '';
      if (s !== sig) { sig = s; rerender(); }
    } else if (type === 'conn' || type === 'ready' || type === 'pads') rerender();
  });
})();
