/* Связь окна со звуковым движком и файл настроек.
   Внутри приложения: Tauri (invoke / события). В браузере для разработки: tools/dev-bridge.mjs. */
(function () {
  const T = window.__TAURI__;
  const listeners = new Set();
  const SLOTS = ['hw1', 'hw2', 'a1', 'a2', 'a3', 'b1', 'b2'];

  const STRIP_DEFAULTS = [
    { name: 'Микрофон 1', tag: 'HW1', source: '' },
    { name: 'Микрофон 2', tag: 'HW2', source: '' },
    { name: 'Игры', tag: 'V1', source: 'Все игры' },
    { name: 'Музыка', tag: 'V2', source: 'Spotify' },
    { name: 'Discord', tag: 'V3', source: 'Discord' },
    { name: 'Браузер', tag: 'V4', source: 'Chrome, YouTube' },
    { name: 'Донаты', tag: 'V5', source: 'Оповещения о донатах' },
    { name: 'Браузер 2', tag: 'V6', source: 'Firefox' }
  ];
  // Маски маршрутов: A1=1, A2=2, A3=4, B1=8, B2=16.
  const SCENES_DEFAULT = [
    { id: 'play', name: 'Играю и стримлю', desc: 'Игры и музыка в наушники и на стрим; Discord только в наушники.', routes: [24, 24, 10, 10, 2, 2, 10, 2] },
    { id: 'call', name: 'Звонок', desc: 'Микрофоны на звонок, всё остальное только в наушники.', routes: [24, 24, 2, 2, 2, 2, 2, 2] },
    { id: 'spk', name: 'Всё в колонки', desc: 'Все программы в колонки, микрофоны на стрим и звонки.', routes: [24, 24, 1, 1, 1, 1, 1, 1] }
  ];
  const GAIN_DEFAULTS = [-3, -5, -6, -14, -4, -9, -8, -12];
  // Микрофоны (этап 4). g: усиление 0..30 дБ; gate: порог гейта −80..−20 (−80 = выкл); hpf: фильтр низких 20..300 Гц (20 = выкл);
  // comp: степень компрессора 1..10 (1 = выкл); ns: шумодав 0 выкл / 1 мягкое / 2 сильное / 3 максимум; amt: сила шумодава, %.
  const NS_AMT = [0, 50, 70, 100];     // сила по умолчанию для каждого режима
  const MIC_DEFAULTS = [
    { g: 10, gate: -45, hpf: 80, comp: 3, ns: 2, amt: 70 },
    { g: 10, gate: -45, hpf: 80, comp: 3, ns: 1, amt: 50 }
  ];
  // Аудио-параметры движка (этап 5): lim — лимитер выходов; dly — задержка A1..A3, мс; buf — блок ведущего устройства, кадры; rc — переподключение.
  const AUDIO_DEFAULTS = { lim: 1, dly: [0, 0, 0], buf: 480, rc: 2 };
  // Настройки программы (экран «Настройки»). Часть из них применяет окно, часть оболочка Tauri (app_prefs в main.rs).
  const APP_DEFAULTS = {
    reconnect: true, scope: 'all', loadOnStart: true, autosave: true,
    tray: true, hideOnClose: true, showOnStart: true, onTop: false, lockGui: false, autostart: false
  };
  const MIC_CMD = { g: 'gain', gate: 'gate', hpf: 'hpf', comp: 'comp', ns: 'ns', amt: 'nsamt' };

  const B = (window.Bridge = {
    mode: 'none',          // tauri | dev | none
    connected: false,      // движок ответил
    state: null,           // последнее состояние движка
    devices: { cap: [], play: [] },
    meters: null,
    lastError: '',
    settings: null,
    SLOTS, STRIP_DEFAULTS, MIC_DEFAULTS, NS_AMT, AUDIO_DEFAULTS, APP_DEFAULTS,
    on(fn) { listeners.add(fn); return () => listeners.delete(fn); }
  });

  function emit(type) { listeners.forEach((f) => { try { f(type); } catch (e) { console.error(e); } }); }

  function defaults() {
    return {
      v: 1,
      strips: STRIP_DEFAULTS.map((d) => ({ ...d })),
      scenes: SCENES_DEFAULT.map((s) => ({ ...s, routes: s.routes.slice() })),
      active: 'play',
      mics: MIC_DEFAULTS.map((m) => ({ ...m })),
      devices: null,
      mixer: null,
      audio: { ...AUDIO_DEFAULTS, dly: AUDIO_DEFAULTS.dly.slice() },
      app: { ...APP_DEFAULTS }
    };
  }
  B.defaults = defaults;
  B.settings = defaults();

  /* ---------- сообщения движка ---------- */
  function handle(line) {
    let m;
    try { m = JSON.parse(line); } catch (e) { return; }
    switch (m.t) {
      case 'hello': B.connected = true; B.version = m.v; emit('conn'); break;
      case 'state': B.connected = true; B.state = m; emit('state'); scheduleSave(); break;
      case 'devices': B.devices = m; emit('devices'); break;
      case 'm': B.meters = m; emit('meters'); break;
      case 'err': B.lastError = m.msg; emit('err'); break;
      case 'exit': B.connected = false; B.state = null; B.lastError = 'Движок остановился'; emit('conn'); emit('err'); autoRestart(); break;
    }
  }

  B.send = function (line) {
    if (B.mode === 'tauri') return T.core.invoke('engine_send', { line }).catch((e) => { B.lastError = String(e); emit('err'); });
    if (B.mode === 'dev') return fetch('/send', { method: 'POST', body: line }).catch(() => {});
    return Promise.resolve();
  };

  /* ---------- настройки ---------- */
  async function loadSettings() {
    let text = '';
    try {
      if (B.mode === 'tauri') text = await T.core.invoke('settings_load');
      else if (B.mode === 'dev') text = await (await fetch('/settings')).text();
      else text = localStorage.getItem('bogestv0.settings') || '';
    } catch (e) { /* без файла работаем с настройками по умолчанию */ }
    if (text) {
      try {
        const saved = JSON.parse(text);
        B.settings = Object.assign(defaults(), saved);
        if (!Array.isArray(saved.mics)) {
          // Версия 0.1.0 хранила только режим шумодава (ns): переносим его в новые настройки микрофонов.
          B.settings.mics = MIC_DEFAULTS.map((m, i) => {
            const ns = saved.ns && saved.ns[i] != null ? saved.ns[i] : m.ns;
            return { ...m, ns, amt: NS_AMT[ns] || m.amt };
          });
        } else {
          B.settings.mics = MIC_DEFAULTS.map((m, i) => ({ ...m, ...(saved.mics[i] || {}) }));
        }
        delete B.settings.ns;
        const d = defaults();
        B.settings.audio = { ...d.audio, ...(saved.audio || {}) };
        if (!Array.isArray(B.settings.audio.dly) || B.settings.audio.dly.length !== 3) B.settings.audio.dly = [0, 0, 0];
        B.settings.app = { ...d.app, ...(saved.app || {}) };
        B.settings.strips = d.strips.map((x, i) => ({ ...x, ...((saved.strips || [])[i] || {}) }));
      } catch (e) { B.settings = defaults(); }
    }
  }

  let saveTimer = null;
  function snapshotFromEngine() {
    const st = B.state;
    if (!st || !B.applied) return;
    B.settings.mixer = {
      strips: st.strips.map((s, i) => ({ g: s.g, m: s.m, r: s.r, bs: st.eq ? st.eq.s[i][0] : 0, tr: st.eq ? st.eq.s[i][1] : 0 })),
      buses: st.buses.map((b, k) => ({ g: b.g, m: b.m, bs: st.eq ? st.eq.b[k][0] : 0, tr: st.eq ? st.eq.b[k][1] : 0 }))
    };
    if (st.aud) B.settings.audio = { lim: st.aud.lim, dly: st.aud.dly.slice(), buf: st.aud.buf, rc: st.aud.rc };
    B.settings.devices = st.dev;
    if (st.mic) B.settings.mics = st.mic.map((m) => ({ g: m.g, gate: m.gate, hpf: m.hpf, comp: m.comp, ns: m.ns, amt: m.amt }));
  }
  // «Сохранять изменения сразу» выключено: пишем в файл только по кнопке «Сохранить настройки».
  function scheduleSave() {
    if (!B.applied) return;
    if (B.settings.app && B.settings.app.autosave === false) { B.unsaved = true; emit('unsaved'); return; }
    clearTimeout(saveTimer);
    saveTimer = setTimeout(B.saveSettings, 600);
  }
  B.scheduleSave = scheduleSave;
  B.saveSettings = async function () {
    snapshotFromEngine();
    B.unsaved = false;
    emit('unsaved');
    const json = JSON.stringify(B.settings, null, 2);
    try {
      if (B.mode === 'tauri') await T.core.invoke('settings_save', { json });
      else if (B.mode === 'dev') await fetch('/settings', { method: 'POST', body: json });
      else localStorage.setItem('bogestv0.settings', json);
    } catch (e) { B.lastError = 'Не удалось сохранить настройки'; emit('err'); }
  };

  /* ---------- перезапуск движка (этап 5) ---------- */
  // Сообщение, которое окно покажет внизу справа (не ошибка).
  B.notice = function (msg) { B.lastNotice = msg; emit('notice'); };

  let restarting = false;
  // Останавливает движок, запускает заново и возвращает ему сохранённые устройства и настройки.
  B.restartEngine = async function (auto) {
    if (B.mode !== 'tauri') { B.notice('Перезапуск движка работает в установленном приложении.'); return false; }
    if (restarting) return false;
    restarting = true;
    try {
      if (!auto) await B.saveSettings();   // последние движения фейдеров должны успеть записаться
      B.applied = false; B.connected = false; B.state = null; B.meters = null;
      emit('conn');
      const info = JSON.parse(await T.core.invoke('engine_restart'));
      try { localStorage.setItem('bogestv0.boot', String(info.boot)); } catch (e) { /* ok */ }
      B.send('devices');
      B.send('get');
      applySaved();
      // Пока движок применяет сохранённое, ничего не пишем в файл (как при первом запуске).
      setTimeout(() => { B.applied = true; emit('ready'); }, 3000);
      B.notice(auto ? 'Движок остановился и был перезапущен.' : 'Движок перезапущен.');
      return true;
    } catch (e) {
      B.lastError = 'Не удалось перезапустить движок: ' + e;
      emit('err');
      return false;
    } finally { restarting = false; }
  };

  // Движок упал сам: перезапускаем. Если падает снова и снова (больше 3 раз за минуту), останавливаемся и просим перезапустить программу.
  let crashes = [];
  function autoRestart() {
    if (B.mode !== 'tauri') return;
    const now = Date.now();
    crashes = crashes.filter((t) => now - t < 60000);
    crashes.push(now);
    if (crashes.length > 3) { B.lastError = 'Движок останавливается снова и снова. Закройте программу совсем (трей → Выход) и откройте заново.'; emit('err'); return; }
    setTimeout(() => B.restartEngine(true), 500);
  }

  // Ctrl+R (и Cmd+R) перезапускает движок, а не страницу.
  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && !e.shiftKey && !e.altKey && (e.code === 'KeyR' || e.key === 'r' || e.key === 'R')) {
      e.preventDefault();
      B.restartEngine(false);
    }
  });

  /* ---------- действия, общие для экранов ---------- */
  B.setGain = (i, db) => B.send(`set strip ${i} gain ${db}`);
  B.setMute = (i, on) => B.send(`set strip ${i} mute ${on ? 1 : 0}`);
  B.setSolo = (i, on) => B.send(`set strip ${i} solo ${on ? 1 : 0}`);
  B.setSrc = (i, name) => B.send(`set strip ${i} src ${name}`);
  // Шины: 0..2 = A1..A3, 3..4 = B1, B2. Фейдер −48…+12 дБ.
  B.setBusGain = (k, db) => B.send(`set bus ${k} gain ${db}`);
  B.setBusMute = (k, on) => B.send(`set bus ${k} mute ${on ? 1 : 0}`);
  B.setDevice = (slot, name) => B.send(`set dev ${slot} ${name || '-'}`);
  // Бас и ВЧ, −12…+12 дБ. kind: 'strip' (дорожка 0..7) или 'bus' (шина 0..4); key: 'bass' | 'treble'. Окно сразу показывает новое значение.
  B.setEq = function (kind, i, key, v) {
    v = Math.max(-12, Math.min(12, Math.round(v * 2) / 2));
    const st = B.state;
    if (st && st.eq) st.eq[kind === 'bus' ? 'b' : 's'][i][key === 'bass' ? 0 : 1] = v;
    return B.send(`set ${kind} ${i} ${key} ${v}`);
  };
  B.eqOf = function (kind, i) {
    const st = B.state;
    const a = st && st.eq && st.eq[kind === 'bus' ? 'b' : 's'][i];
    if (a) return { bass: a[0], treble: a[1] };
    const m = B.settings.mixer && B.settings.mixer[kind === 'bus' ? 'buses' : 'strips'][i];
    return { bass: (m && m.bs) || 0, treble: (m && m.tr) || 0 };
  };
  // Аудио-параметры движка: limiter 0/1, delay K мс, buffer кадров, reconnect 0/1/2.
  B.setAudio = function (key, a, b) {
    const st = B.state, au = B.settings.audio;
    if (key === 'limiter') { au.lim = a ? 1 : 0; if (st && st.aud) st.aud.lim = au.lim; return B.send(`set audio limiter ${au.lim}`); }
    if (key === 'delay') { au.dly[a] = b; if (st && st.aud) st.aud.dly[a] = b; return B.send(`set audio delay ${a} ${b}`); }
    if (key === 'buffer') { au.buf = a; if (st && st.aud) st.aud.buf = a; return B.send(`set audio buffer ${a}`); }
    if (key === 'reconnect') { au.rc = a; if (st && st.aud) st.aud.rc = a; return B.send(`set audio reconnect ${a}`); }
  };
  B.audioOf = () => (B.state && B.state.aud) || B.settings.audio;
  // Настройки программы: пишем в файл настроек и сообщаем оболочке Tauri (трей, закрытие окна, поверх окон).
  B.setApp = function (patch) {
    Object.assign(B.settings.app, patch);
    if ('reconnect' in patch || 'scope' in patch) {
      const a = B.settings.app;
      B.setAudio('reconnect', a.reconnect ? (a.scope === 'a1' ? 1 : 2) : 0);
    }
    pushAppPrefs();
    emit('app');
    B.saveSettings();   // это не поток движка, а выбор человека: пишем сразу, даже если автосохранение выключено
  };
  function pushAppPrefs() {
    if (B.mode !== 'tauri') return;
    const a = B.settings.app;
    T.core.invoke('app_prefs', { tray: !!a.tray, hideOnClose: !!a.hideOnClose, onTop: !!a.onTop }).catch(() => {});
  }
  B.pushAppPrefs = pushAppPrefs;
  // Микрофон: key = g | gate | hpf | comp | ns | amt. Окно сразу показывает новое значение, не дожидаясь ответа движка.
  B.setMic = function (i, key, v) {
    if (B.state && B.state.mic && B.state.mic[i]) B.state.mic[i][key] = v;
    if (B.settings.mics[i]) B.settings.mics[i][key] = v;
    return B.send(`set mic ${i} ${MIC_CMD[key]} ${v}`);
  };
  // Режим шумодава: вместе с ним ставится сила по умолчанию (её можно потом подвинуть на экране «Микрофон»).
  B.setNs = function (i, mode) {
    B.setMic(i, 'ns', mode);
    if (mode > 0) B.setMic(i, 'amt', NS_AMT[mode]);
  };
  B.micOf = (i) => (B.state && B.state.mic && B.state.mic[i]) || B.settings.mics[i] || MIC_DEFAULTS[i];
  B.sendMic = function (i, m) { Object.keys(MIC_CMD).forEach((k) => { if (m[k] != null) B.send(`set mic ${i} ${MIC_CMD[k]} ${m[k]}`); }); };
  // Ручное переключение маршрута: сцена становится «своими настройками».
  B.setRoute = function (i, k, on) {
    B.settings.active = null;
    return B.send(`set strip ${i} route ${k} ${on ? 1 : 0}`);
  };
  B.applyScene = function (id) {
    const sc = B.settings.scenes.find((s) => s.id === id);
    if (!sc) return;
    sc.routes.forEach((mask, i) => { for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(mask >> k) & 1}`); });
    // Сцена может хранить и настройки микрофонов: mics: [ {ns, amt, gate, ...}, {...} ]. Поля, которых нет, не меняются.
    if (Array.isArray(sc.mics)) sc.mics.forEach((m, i) => { if (m) B.sendMic(i, m); });
    B.settings.active = id;
    emit('state');
    scheduleSave();
  };

  // Аудио-параметры и режим переподключения.
  function sendAudio() {
    const au = B.settings.audio, a = B.settings.app;
    B.send(`set audio limiter ${au.lim ? 1 : 0}`);
    au.dly.forEach((ms, k) => B.send(`set audio delay ${k} ${ms}`));
    B.send(`set audio buffer ${au.buf}`);
    B.send(`set audio reconnect ${a.reconnect === false ? 0 : (a.scope === 'a1' ? 1 : 2)}`);
  }
  // Заводские значения микшера (устройства не трогаем).
  function defaultsToEngine() {
    GAIN_DEFAULTS.forEach((g, i) => {
      B.send(`set strip ${i} gain ${g}`); B.send(`set strip ${i} mute 0`); B.send(`set strip ${i} solo 0`);
      B.send(`set strip ${i} bass 0`); B.send(`set strip ${i} treble 0`);
    });
    for (let k = 0; k < 5; k++) { B.send(`set bus ${k} gain 0`); B.send(`set bus ${k} mute 0`); B.send(`set bus ${k} bass 0`); B.send(`set bus ${k} treble 0`); }
    const sc = SCENES_DEFAULT[0];
    sc.routes.forEach((mask, i) => { for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(mask >> k) & 1}`); });
    MIC_DEFAULTS.forEach((m, i) => B.sendMic(i, m));
  }
  // «Сбросить все настройки»: заводское состояние. Устройства остаются назначенными, иначе пропадёт звук.
  B.resetAll = async function () {
    const keepDevices = B.settings.devices;
    B.settings = defaults();
    B.settings.devices = keepDevices;
    defaultsToEngine();
    B.settings.audio = { ...AUDIO_DEFAULTS, dly: AUDIO_DEFAULTS.dly.slice() };
    sendAudio();
    B.settings.mixer = null;
    pushAppPrefs();
    B.send('get');
    emit('state'); emit('app');
    await B.saveSettings();
  };
  // «Файл настроек»: выгрузка и загрузка JSON.
  B.exportSettings = function () {
    snapshotFromEngine();
    const blob = new Blob([JSON.stringify(B.settings, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'bogestv0-settings.json';
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  };
  B.importSettings = async function (text) {
    let saved;
    try { saved = JSON.parse(text); } catch (e) { throw new Error('Это не файл настроек: не читается как JSON.'); }
    if (!saved || typeof saved !== 'object' || !Array.isArray(saved.strips) || !saved.mixer) throw new Error('В файле нет настроек микшера.');
    const keepDevices = B.settings.devices;
    B.settings = Object.assign(defaults(), saved);
    B.settings.mics = MIC_DEFAULTS.map((m, i) => ({ ...m, ...((saved.mics || [])[i] || {}) }));
    B.settings.audio = { ...AUDIO_DEFAULTS, ...(saved.audio || {}) };
    B.settings.app = { ...APP_DEFAULTS, ...(saved.app || {}) };
    B.settings.strips = defaults().strips.map((x, i) => ({ ...x, ...((saved.strips || [])[i] || {}) }));
    B.settings.devices = saved.devices || keepDevices;
    applySaved();
    pushAppPrefs();
    B.send('get');
    emit('state'); emit('app');
    await B.saveSettings();
  };

  // «Заблокировать интерфейс»: фейдеры, кнопки и крутилки не реагируют, пока блокировка включена. Перехват в фазе погружения, раньше экранов.
  const LOCK_SEL = '[data-act],[data-fader],[data-knob],[data-bfader],[data-bmute]';
  function lockGuard(ev) {
    if (!B.settings.app || !B.settings.app.lockGui) return;
    const t = ev.target && ev.target.closest && ev.target.closest(LOCK_SEL);
    if (!t || t.dataset.act === 'restart') return;
    ev.preventDefault(); ev.stopPropagation(); ev.stopImmediatePropagation();
    if (ev.type === 'pointerdown') B.notice('Интерфейс заблокирован. Снять блокировку: Настройки → «Заблокировать интерфейс».');
  }
  ['pointerdown', 'click', 'keydown'].forEach((t) => document.addEventListener(t, lockGuard, true));

  // При каждом новом запуске движка возвращаем сохранённые устройства и настройки микшера.
  function applySaved() {
    const s = B.settings;
    if (s.devices) SLOTS.forEach((slot) => { if (s.devices[slot]) B.send(`set dev ${slot} ${s.devices[slot]}`); });
    s.mics.forEach((m, i) => B.sendMic(i, m));
    const mx = s.mixer;
    sendAudio();
    if (mx && s.app.loadOnStart === false) {
      // «Загружать настройки при запуске» выключено: устройства вернули, остальное по умолчанию.
      defaultsToEngine();
    } else if (mx) {
      mx.strips.forEach((st, i) => {
        B.send(`set strip ${i} gain ${st.g}`); B.send(`set strip ${i} mute ${st.m}`);
        for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(st.r >> k) & 1}`);
        B.send(`set strip ${i} bass ${st.bs || 0}`); B.send(`set strip ${i} treble ${st.tr || 0}`);
      });
      mx.buses.forEach((b, k) => {
        B.send(`set bus ${k} gain ${b.g}`); B.send(`set bus ${k} mute ${b.m}`);
        B.send(`set bus ${k} bass ${b.bs || 0}`); B.send(`set bus ${k} treble ${b.tr || 0}`);
      });
    } else {
      GAIN_DEFAULTS.forEach((g, i) => B.send(`set strip ${i} gain ${g}`));
      const sc = s.scenes.find((x) => x.id === s.active);
      if (sc) sc.routes.forEach((mask, i) => { for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(mask >> k) & 1}`); });
    }
  }

  /* ---------- подключение ---------- */
  async function connect() {
    let boot = null;
    if (T && T.core && T.event) {
      B.mode = 'tauri';
      await T.event.listen('engine-line', (e) => handle(e.payload));
      try { boot = JSON.parse(await T.core.invoke('engine_info')).boot; } catch (e) { /* ниже */ }
    } else {
      try {
        const r = await fetch('/info');
        if (r.ok) {
          boot = (await r.json()).boot;
          B.mode = 'dev';
          const es = new EventSource('/events');
          es.onmessage = (e) => handle(e.data);
        }
      } catch (e) { /* открыто просто как файл: движка нет */ }
    }
    await loadSettings();
    emit('conn');
    if (B.mode === 'none') return;
    B.send('devices');
    B.send('get');
    let applied = null;
    try { applied = localStorage.getItem('bogestv0.boot'); } catch (e) { /* ok */ }
    if (String(boot) !== applied) {
      try { localStorage.setItem('bogestv0.boot', String(boot)); } catch (e) { /* ok */ }
      applySaved();
      pushAppPrefs();
      // Пока движок применяет сохранённое (открытие устройств занимает время), ничего не пишем в файл,
      // иначе промежуточное состояние затрёт сохранённые настройки.
      setTimeout(() => { B.applied = true; emit('ready'); }, 3000);
    } else {
      B.applied = true;
      pushAppPrefs();
      emit('ready');
    }
  }
  B.ready = connect();
})();
