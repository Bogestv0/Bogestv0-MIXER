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

  const B = (window.Bridge = {
    mode: 'none',          // tauri | dev | none
    connected: false,      // движок ответил
    state: null,           // последнее состояние движка
    devices: { cap: [], play: [] },
    meters: null,
    lastError: '',
    settings: null,
    SLOTS, STRIP_DEFAULTS,
    on(fn) { listeners.add(fn); return () => listeners.delete(fn); }
  });

  function emit(type) { listeners.forEach((f) => { try { f(type); } catch (e) { console.error(e); } }); }

  function defaults() {
    return {
      v: 1,
      strips: STRIP_DEFAULTS.map((d) => ({ ...d })),
      scenes: SCENES_DEFAULT.map((s) => ({ ...s, routes: s.routes.slice() })),
      active: 'play',
      ns: [2, 1],
      devices: null,
      mixer: null
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
      case 'exit': B.connected = false; B.state = null; B.lastError = 'Движок остановился'; emit('conn'); emit('err'); break;
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
      try { B.settings = Object.assign(defaults(), JSON.parse(text)); } catch (e) { B.settings = defaults(); }
    }
  }

  let saveTimer = null;
  function snapshotFromEngine() {
    const st = B.state;
    if (!st || !B.applied) return;
    B.settings.mixer = {
      strips: st.strips.map((s) => ({ g: s.g, m: s.m, r: s.r })),
      buses: st.buses.map((b) => ({ g: b.g, m: b.m }))
    };
    B.settings.devices = st.dev;
  }
  function scheduleSave() {
    if (!B.applied) return;
    clearTimeout(saveTimer);
    saveTimer = setTimeout(B.saveSettings, 600);
  }
  B.saveSettings = async function () {
    snapshotFromEngine();
    const json = JSON.stringify(B.settings, null, 2);
    try {
      if (B.mode === 'tauri') await T.core.invoke('settings_save', { json });
      else if (B.mode === 'dev') await fetch('/settings', { method: 'POST', body: json });
      else localStorage.setItem('bogestv0.settings', json);
    } catch (e) { B.lastError = 'Не удалось сохранить настройки'; emit('err'); }
  };

  /* ---------- действия, общие для экранов ---------- */
  B.setGain = (i, db) => B.send(`set strip ${i} gain ${db}`);
  B.setMute = (i, on) => B.send(`set strip ${i} mute ${on ? 1 : 0}`);
  B.setSolo = (i, on) => B.send(`set strip ${i} solo ${on ? 1 : 0}`);
  B.setSrc = (i, name) => B.send(`set strip ${i} src ${name}`);
  B.setDevice = (slot, name) => B.send(`set dev ${slot} ${name || '-'}`);
  // Ручное переключение маршрута: сцена становится «своими настройками».
  B.setRoute = function (i, k, on) {
    B.settings.active = null;
    return B.send(`set strip ${i} route ${k} ${on ? 1 : 0}`);
  };
  B.applyScene = function (id) {
    const sc = B.settings.scenes.find((s) => s.id === id);
    if (!sc) return;
    sc.routes.forEach((mask, i) => { for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(mask >> k) & 1}`); });
    B.settings.active = id;
    emit('state');
    scheduleSave();
  };

  // При каждом новом запуске движка возвращаем сохранённые устройства и настройки микшера.
  function applySaved() {
    const s = B.settings;
    if (s.devices) SLOTS.forEach((slot) => { if (s.devices[slot]) B.send(`set dev ${slot} ${s.devices[slot]}`); });
    const mx = s.mixer;
    if (mx) {
      mx.strips.forEach((st, i) => {
        B.send(`set strip ${i} gain ${st.g}`); B.send(`set strip ${i} mute ${st.m}`);
        for (let k = 0; k < 5; k++) B.send(`set strip ${i} route ${k} ${(st.r >> k) & 1}`);
      });
      mx.buses.forEach((b, k) => { B.send(`set bus ${k} gain ${b.g}`); B.send(`set bus ${k} mute ${b.m}`); });
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
      // Пока движок применяет сохранённое (открытие устройств занимает время), ничего не пишем в файл,
      // иначе промежуточное состояние затрёт сохранённые настройки.
      setTimeout(() => { B.applied = true; emit('ready'); }, 3000);
    } else {
      B.applied = true;
      emit('ready');
    }
  }
  B.ready = connect();
})();
