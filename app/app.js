/* Общая логика окна: ссылки меню и обновления через GitHub Releases. */
(function () {
  // Поменяйте OWNER на имя вашего аккаунта GitHub (то же самое в src-tauri/tauri.conf.json).
  const REPO = 'Bogestv0/Bogestv0-MIXER';
  const CHECK_EVERY_MS = 4 * 60 * 60 * 1000;
  const T = window.__TAURI__;
  const store = {
    get(k, d) { try { const v = localStorage.getItem('bogestv0.' + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem('bogestv0.' + k, JSON.stringify(v)); } catch (e) { /* без хранилища работаем с умолчаниями */ } }
  };

  /* ---------- меню: ссылки на экраны, которых нет в макете ---------- */
  const NAV_FIX = { 'Выходы': 'outputs.html', 'Устройства': 'devices.html' };
  function fixLinks() {
    document.querySelectorAll('a[href="#"]').forEach((a) => {
      const key = a.textContent.trim();
      if (NAV_FIX[key]) a.setAttribute('href', NAV_FIX[key]);
    });
  }

  /* ---------- обновления ---------- */
  let pending = null;           // найденное обновление
  let state = 'idle';           // idle | checking | none | available | installing | error
  let message = 'Ещё не проверялось.';
  let version = '0.0.1';

  const $ = (id) => document.getElementById(id);

  function paintSettings() {
    const status = $('upd-status');
    if (!status) return;
    $('upd-version').textContent = version;
    status.textContent = message;
    $('upd-check').disabled = state === 'checking' || state === 'installing';
    $('upd-install').hidden = state !== 'available';
    const on = store.get('autoUpdate', true);
    const sw = $('upd-auto');
    sw.setAttribute('aria-checked', String(on));
    sw.style.background = on ? 'var(--signal)' : 'var(--surface-2)';
    sw.style.border = '1px solid ' + (on ? 'var(--signal)' : 'var(--muted)');
    const knob = sw.firstElementChild;
    knob.style.left = on ? '22px' : '2px';
    knob.style.background = on ? 'var(--signal-ink)' : 'var(--muted)';
  }

  function bindSettings() {
    if (!$('upd-check')) return;
    $('upd-check').onclick = () => checkForUpdates(true);
    $('upd-install').onclick = () => installUpdate();
    $('upd-notes').onclick = () => openUrl('https://github.com/' + REPO + '/releases');
    $('upd-auto').onclick = () => { store.set('autoUpdate', !store.get('autoUpdate', true)); paintSettings(); };
    paintSettings();
  }

  function openUrl(url) {
    if (T && T.opener) T.opener.openUrl(url); else window.open(url, '_blank');
  }

  async function checkForUpdates(manual) {
    if (!T || !T.updater) {
      message = 'Проверка работает только в установленном приложении.';
      state = 'idle'; paintSettings(); return;
    }
    if (state === 'checking' || state === 'installing') return;
    state = 'checking'; message = 'Проверяю…'; paintSettings();
    try {
      store.set('lastCheck', Date.now());
      const upd = await T.updater.check();
      if (upd) {
        pending = upd; state = 'available';
        message = 'Доступна версия ' + upd.version + '.';
        paintSettings();
        showModal(upd);
      } else {
        pending = null; state = 'none';
        message = 'Установлена последняя версия.';
        paintSettings();
      }
    } catch (e) {
      state = 'error';
      message = manual ? 'Не удалось проверить: ' + (e && e.message ? e.message : e) : 'Не удалось проверить обновления.';
      paintSettings();
    }
  }

  async function installUpdate() {
    if (!pending) return;
    closeModal();
    state = 'installing'; message = 'Скачиваю обновление…'; paintSettings();
    let total = 0, got = 0;
    try {
      await pending.downloadAndInstall((ev) => {
        if (ev.event === 'Started') total = ev.data.contentLength || 0;
        if (ev.event === 'Progress') { got += ev.data.chunkLength; if (total) { message = 'Скачиваю… ' + Math.round(got / total * 100) + '%'; paintSettings(); } }
        if (ev.event === 'Finished') { message = 'Устанавливаю…'; paintSettings(); }
      });
      if (T.process) await T.process.relaunch();
    } catch (e) {
      state = 'error';
      message = 'Не получилось обновиться, осталась текущая версия. ' + (e && e.message ? e.message : e);
      paintSettings();
    }
  }

  /* ---------- окно «Что нового» ---------- */
  function closeModal() { const m = $('upd-modal'); if (m) m.remove(); }
  function showModal(upd) {
    closeModal();
    const wrap = document.createElement('div');
    wrap.id = 'upd-modal';
    wrap.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,.45);display:flex;align-items:center;justify-content:center;z-index:50';
    const box = document.createElement('div');
    box.setAttribute('role', 'dialog');
    box.setAttribute('aria-label', 'Доступно обновление');
    box.style.cssText = 'width:min(520px,90vw);max-height:80vh;display:flex;flex-direction:column;gap:12px;background:var(--surface);color:var(--fg);border-radius:20px;padding:22px;box-shadow:0 10px 40px rgba(0,0,0,.35);font:400 15px/1.5 var(--f-body, Onest, sans-serif)';
    const h = document.createElement('div');
    h.style.cssText = "font:600 18px/1.2 'Unbounded',sans-serif";
    h.textContent = 'Доступно ' + upd.version;
    const sub = document.createElement('div');
    sub.style.cssText = 'font-size:13px;color:var(--muted)';
    sub.textContent = 'У вас ' + version + '. Что нового:';
    const notes = document.createElement('div');
    notes.style.cssText = 'overflow:auto;white-space:pre-wrap;font-size:14px;padding:12px;border-radius:12px;background:var(--surface-2)';
    notes.textContent = (upd.body || 'Описание не указано.').trim();
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;gap:8px;justify-content:flex-end';
    const later = document.createElement('button');
    later.textContent = 'Позже';
    later.style.cssText = 'min-height:44px;padding:0 18px;border-radius:12px;background:none;color:var(--fg);border:1px solid var(--line);font:500 14px Onest,sans-serif';
    later.onclick = closeModal;
    const now = document.createElement('button');
    now.textContent = 'Обновить сейчас';
    now.style.cssText = 'min-height:44px;padding:0 18px;border-radius:12px;background:var(--signal);color:var(--signal-ink);border:1px solid var(--signal);font:500 14px Onest,sans-serif';
    now.onclick = installUpdate;
    row.append(later, now);
    box.append(h, sub, notes, row);
    wrap.append(box);
    wrap.addEventListener('click', (e) => { if (e.target === wrap) closeModal(); });
    document.body.append(wrap);
  }

  /* ---------- запуск ---------- */
  async function init() {
    fixLinks(); bindSettings();
    if (T && T.app) { try { version = await T.app.getVersion(); } catch (e) { /* оставляем 0.0.1 */ } }
    paintSettings();
    const due = Date.now() - store.get('lastCheck', 0) > CHECK_EVERY_MS;
    if (T && store.get('autoUpdate', true) && due) checkForUpdates(false);
  }
  document.addEventListener('dc-rendered', () => { fixLinks(); bindSettings(); });
  init();
  setInterval(() => { if (T && store.get('autoUpdate', true) && state !== 'installing') checkForUpdates(false); }, CHECK_EVERY_MS);
})();
