/* Подключение наших «Out» к OBS без скриптов и сторонних программ.

   Зачем. «Звук рабочего стола» в OBS видит только устройства воспроизведения (наши In). Устройства записи (наши Out)
   OBS кладёт в «Микрофон/дополнительный звук» и в источник «Захват входного аудио». Устройство в Windows либо воспроизведения,
   либо записи, поэтому один и тот же Out не может быть в обоих списках. Вместо ручной настройки восьми устройств микшер
   сам создаёт в OBS источники «Захват входного аудио» по одному на дорожку (Games, Music, ... и шины B1, B2).

   Два способа:
   1. WebSocket (встроен в OBS с версии 28, ничего ставить не нужно): микшер подключается к запущенному OBS и добавляет
      источники в ВАШУ текущую коллекцию сцен. Порт и пароль берутся из настроек OBS (читает приложение, вводить не надо).
   2. Файл коллекции сцен: если OBS закрыт или WebSocket выключен, микшер кладёт в папку OBS готовую коллекцию
      «Bogestv0 MIXER», её выбирают в меню OBS «Коллекция сцен».

   Файл работает и в браузере (для проверки в Node: tools/test-obs.mjs). */
(function (root) {
  'use strict';

  /* ---------- какие устройства подключаем ---------- */
  // Порядок и имена те же, что в драйвере (driver/patch_sysvad.py, TRACKS) и на экране «Устройства».
  // strip: номер полосы в микшере (0, 1 = микрофоны, 2..7 = V1..V6): оттуда берётся имя источника в OBS.
  const TARGETS = [
    { tag: 'T1', dev: 'Games', strip: 2 },
    { tag: 'T2', dev: 'Music', strip: 3 },
    { tag: 'T3', dev: 'Discord', strip: 4 },
    { tag: 'T4', dev: 'Browser 1', strip: 5 },
    { tag: 'T5', dev: 'Donations', strip: 6 },
    { tag: 'T6', dev: 'Browser 2', strip: 7 },
    // Шины: общий микс после фейдеров. Старые имена драйвера («Bogestv0 Output 1 / 2») тоже находим.
    { tag: 'B1', dev: 'Mix B1', label: 'Mix B1', legacy: /^Bogestv0 Output 1(\s|$)/i },
    { tag: 'B2', dev: 'Mix B2', label: 'Mix B2', legacy: /^Bogestv0 Output 2(\s|$)/i }
  ];
  const KIND = 'wasapi_input_capture';
  const SCENE = 'Bogestv0 MIXER';
  const PREFIX = 'Bogestv0 ';

  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

  // Находит в списке устройств записи (то, что прислал движок: [{name, id}]) наши Out и готовит имена источников OBS.
  // Возвращает по строке на каждую дорожку: { tag, label, obsName, deviceName, deviceId, ok, why }.
  function plan(caps, stripNames) {
    caps = caps || [];
    stripNames = stripNames || [];
    const used = new Set();
    return TARGETS.map((t) => {
      const re = new RegExp('^Bogestv0 ' + esc(t.dev) + ' Out(\\s|$)', 'i');
      const hit = caps.find((d) => d && re.test(d.name || '')) || (t.legacy && caps.find((d) => d && t.legacy.test(d.name || '')));
      const label = t.label || stripNames[t.strip] || t.dev;
      let obsName = PREFIX + label;
      for (let n = 2; used.has(obsName); n++) obsName = PREFIX + label + ' ' + n;   // две дорожки с одинаковым именем
      used.add(obsName);
      const row = { tag: t.tag, label, obsName, deviceName: hit ? hit.name : '', deviceId: hit && hit.id ? hit.id : '', ok: false, why: '' };
      if (!hit) row.why = 'устройство «Bogestv0 ' + t.dev + ' Out» не найдено в Windows';
      else if (!hit.id) row.why = 'движок не передал идентификатор устройства (обновите приложение)';
      else row.ok = true;
      return row;
    });
  }

  /* ---------- SHA-256 и base64 (для пароля WebSocket; свой код, чтобы не зависеть от crypto.subtle) ---------- */
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]);
  function sha256(bytes) {
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const len = bytes.length;
    const total = (((len + 9 + 63) >> 6) << 6);
    const buf = new Uint8Array(total);
    buf.set(bytes);
    buf[len] = 0x80;
    const dv = new DataView(buf.buffer);
    dv.setUint32(total - 8, Math.floor((len * 8) / 4294967296));
    dv.setUint32(total - 4, (len * 8) >>> 0);
    const w = new Uint32Array(64);
    const rr = (x, n) => (x >>> n) | (x << (32 - n));
    for (let off = 0; off < total; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (let i = 0; i < 64; i++) {
        const S1 = rr(e, 6) ^ rr(e, 11) ^ rr(e, 25);
        const ch = (e & f) ^ (~e & g);
        const t1 = (hh + S1 + ch + K[i] + w[i]) >>> 0;
        const S0 = rr(a, 2) ^ rr(a, 13) ^ rr(a, 22);
        const mj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + mj) >>> 0;
        hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    const out = new Uint8Array(32);
    const odv = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) odv.setUint32(i * 4, h[i]);
    return out;
  }
  const utf8 = (s) => new TextEncoder().encode(s);
  const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return btoa(s); };
  const sha256b64 = (s) => b64(sha256(utf8(s)));
  // Ответ на вызов сервера по правилам obs-websocket 5: base64(sha256( base64(sha256(пароль + соль)) + вызов )).
  function authString(password, salt, challenge) {
    return sha256b64(sha256b64(password + salt) + challenge);
  }

  /* ---------- клиент obs-websocket 5 ---------- */
  // opts: { port, password, host, timeout, WebSocket }. Возвращает { request(тип, данные) → Promise<данные ответа>, close() }.
  function connect(opts) {
    opts = opts || {};
    const host = opts.host || '127.0.0.1';
    const port = opts.port || 4455;
    const timeout = opts.timeout || 6000;
    const WS = opts.WebSocket || root.WebSocket;
    return new Promise((resolve, reject) => {
      if (!WS) { reject(new Error('в этом окне нет WebSocket')); return; }
      let ws;
      try { ws = new WS('ws://' + host + ':' + port, 'obswebsocket.json'); } catch (e) { reject(new Error('не удалось открыть соединение: ' + e.message)); return; }
      const pending = new Map();
      let nextId = 1, identified = false, finished = false;
      const failAll = (err) => { pending.forEach((p) => { clearTimeout(p.timer); p.reject(err); }); pending.clear(); };
      const done = (err, client) => {
        if (finished) return;
        finished = true;
        clearTimeout(connTimer);
        if (err) { try { ws.close(); } catch (e) { /* уже закрыто */ } reject(err); } else resolve(client);
      };
      const connTimer = setTimeout(() => done(new Error('OBS не отвечает на порту ' + port)), timeout);
      const client = {
        request(type, data) {
          return new Promise((res, rej) => {
            const id = String(nextId++);
            const timer = setTimeout(() => { pending.delete(id); rej(new Error('OBS не ответил на запрос ' + type)); }, timeout);
            pending.set(id, { resolve: res, reject: rej, timer });
            try { ws.send(JSON.stringify({ op: 6, d: { requestType: type, requestId: id, requestData: data || {} } })); }
            catch (e) { clearTimeout(timer); pending.delete(id); rej(new Error('соединение с OBS закрыто')); }
          });
        },
        close() { try { ws.close(); } catch (e) { /* уже закрыто */ } }
      };
      ws.onmessage = (ev) => {
        let m;
        try { m = JSON.parse(typeof ev.data === 'string' ? ev.data : String(ev.data)); } catch (e) { return; }
        const d = m.d || {};
        if (m.op === 0) {          // Hello: отвечаем Identify (с паролем, если сервер его требует)
          const id = { rpcVersion: 1, eventSubscriptions: 0 };
          if (d.authentication) {
            if (!opts.password) { done(new Error('OBS просит пароль, а в его настройках пароля нет')); return; }
            id.authentication = authString(opts.password, d.authentication.salt, d.authentication.challenge);
          }
          ws.send(JSON.stringify({ op: 1, d: id }));
        } else if (m.op === 2) {   // Identified
          identified = true;
          done(null, client);
        } else if (m.op === 7) {   // ответ на запрос
          const p = pending.get(String(d.requestId));
          if (!p) return;
          pending.delete(String(d.requestId));
          clearTimeout(p.timer);
          const st = d.requestStatus || {};
          if (st.result) p.resolve(d.responseData || {});
          else { const e = new Error((st.comment || 'отказ OBS') + ' (' + d.requestType + ', код ' + st.code + ')'); e.code = st.code; p.reject(e); }
        }
      };
      ws.onerror = () => done(new Error('не удалось подключиться к OBS на порту ' + port + ' (OBS закрыт или WebSocket выключен)'));
      ws.onclose = (ev) => {
        const err = new Error(ev && ev.code === 4009 ? 'OBS отклонил пароль' : 'соединение с OBS закрыто');
        if (!identified) done(err);
        failAll(err);
      };
    });
  }

  /* ---------- добавление в запущенный OBS ---------- */
  // Создаёт в OBS сцену «Bogestv0 MIXER» с источниками «Захват входного аудио» (по строке плана) и вкладывает её в каждую сцену.
  // Повторный запуск безопасен: источник ищется по устройству, а не по имени, ничего не дублируется.
  // opts: { muteDesktop } – заглушить «Звук рабочего стола» (иначе звук In и Out задвоится).
  async function sync(client, rows, opts) {
    opts = opts || {};
    const todo = rows.filter((r) => r.ok);
    if (!todo.length) throw new Error('нет ни одного найденного устройства Out');
    const res = { created: [], reused: [], scenesAdded: [], muted: [] };
    const asArr = (x) => (Array.isArray(x) ? x : []);

    const scenes = asArr((await client.request('GetSceneList')).scenes).map((s) => s.sceneName);
    if (!scenes.includes(SCENE)) { await client.request('CreateScene', { sceneName: SCENE }); }

    // Все имена источников заняты одним пространством: проверяем и входы, и сцены.
    const inputs = asArr((await client.request('GetInputList')).inputs);
    const names = new Set(inputs.map((i) => i.inputName).concat(scenes, [SCENE]));
    const byDevice = new Map();   // идентификатор устройства -> имя уже существующего источника
    for (const inp of inputs) {
      if (inp.inputKind !== KIND) continue;
      try {
        const st = await client.request('GetInputSettings', { inputName: inp.inputName });
        const id = st.inputSettings && st.inputSettings.device_id;
        if (id && !byDevice.has(id)) byDevice.set(id, inp.inputName);
      } catch (e) { /* источник мог исчезнуть: пропускаем */ }
    }

    const inScene = new Set(asArr((await client.request('GetSceneItemList', { sceneName: SCENE })).sceneItems).map((i) => i.sourceName));
    for (const r of todo) {
      const have = byDevice.get(r.deviceId);
      if (have) {
        if (!inScene.has(have)) { await client.request('CreateSceneItem', { sceneName: SCENE, sourceName: have, sceneItemEnabled: true }); inScene.add(have); }
        res.reused.push(have);
        continue;
      }
      let name = r.obsName;
      for (let n = 2; names.has(name); n++) name = r.obsName + ' ' + n;
      names.add(name);
      await client.request('CreateInput', { sceneName: SCENE, inputName: name, inputKind: KIND, inputSettings: { device_id: r.deviceId, use_device_timing: false }, sceneItemEnabled: true });
      inScene.add(name);
      res.created.push(name);
    }

    // Вложенная сцена во всех сценах: микшер OBS показывает звук только у сцен в эфире, поэтому нужна каждая.
    for (const sc of scenes) {
      if (sc === SCENE) continue;
      const items = asArr((await client.request('GetSceneItemList', { sceneName: sc })).sceneItems);
      if (items.some((i) => i.sourceName === SCENE)) continue;
      await client.request('CreateSceneItem', { sceneName: sc, sourceName: SCENE, sceneItemEnabled: true });
      res.scenesAdded.push(sc);
    }

    if (opts.muteDesktop) {
      const sp = await client.request('GetSpecialInputs');
      for (const k of ['desktop1', 'desktop2']) {
        if (!sp[k]) continue;
        await client.request('SetInputMute', { inputName: sp[k], inputMuted: true });
        res.muted.push(sp[k]);
      }
    }
    return res;
  }

  /* ---------- коллекция сцен (файл) ---------- */
  function uuid() {
    if (root.crypto && root.crypto.randomUUID) return root.crypto.randomUUID();
    const h = '0123456789abcdef';
    let s = '';
    for (let i = 0; i < 36; i++) s += i === 8 || i === 13 || i === 18 || i === 23 ? '-' : i === 14 ? '4' : h[Math.floor(Math.random() * 16)];
    return s;
  }
  const PREV_VER = 536870912;   // 32.0.0: OBS не применяет к этим источникам старые миграции
  const hk = (names) => { const o = {}; names.forEach((n) => { o[n] = []; }); return o; };
  const baseSource = (id, name, settings, extra) => Object.assign({
    balance: 0.5, deinterlace_field_order: 0, deinterlace_mode: 0, enabled: true, flags: 0, hotkeys: {}, id, mixers: 255,
    monitoring_type: 0, muted: false, name, prev_ver: PREV_VER, private_settings: {}, 'push-to-mute': false, 'push-to-mute-delay': 0,
    'push-to-talk': false, 'push-to-talk-delay': 0, settings, sync: 0, uuid: uuid(), versioned_id: id, volume: 1.0
  }, extra || {});

  // Готовая коллекция «Bogestv0 MIXER»: одна сцена «Сцена» и в ней все найденные Out. Глобальных устройств
  // «Звук рабочего стола» и «Микрофон» в ней нет нарочно: звук идёт только из Out и не задваивается.
  function collection(rows, name) {
    const todo = rows.filter((r) => r.ok);
    if (!todo.length) throw new Error('нет ни одного найденного устройства Out');
    name = name || SCENE;
    const sceneName = 'Сцена';
    const srcs = todo.map((r) => baseSource(KIND, r.obsName, { device_id: r.deviceId, use_device_timing: false }, { hotkeys: hk(['libobs.mute', 'libobs.unmute', 'libobs.push-to-mute', 'libobs.push-to-talk']) }));
    const items = srcs.map((s, i) => ({
      align: 5, blend_method: 'default', blend_type: 'normal', bounds: { x: 0.0, y: 0.0 }, bounds_align: 0, bounds_crop: false, bounds_type: 0,
      crop_bottom: 0, crop_left: 0, crop_right: 0, crop_top: 0, group_item_backup: false, id: i + 1, locked: false, name: s.name,
      pos: { x: 0.0, y: 0.0 }, private_settings: {}, rot: 0.0, scale: { x: 1.0, y: 1.0 }, scale_filter: 'disable',
      show_transition: { duration: 0 }, hide_transition: { duration: 0 }, source_uuid: s.uuid, visible: true
    }));
    const scene = baseSource('scene', sceneName, { custom_size: false, id_counter: items.length, items }, { mixers: 0, hotkeys: hk(['OBSBasic.SelectScene']) });
    return {
      current_program_scene: sceneName,
      current_scene: sceneName,
      groups: [],
      modules: {},
      name,
      preview_locked: false,
      quick_transitions: [],
      saved_projectors: [],
      scaling_enabled: false,
      scene_order: [{ name: sceneName }],
      sources: srcs.concat([scene]),
      transition_duration: 300,
      transitions: []
    };
  }

  const Obs = { TARGETS, KIND, SCENE, plan, connect, sync, collection, authString, sha256, sha256b64 };
  root.ObsLink = Obs;
  if (typeof module !== 'undefined' && module.exports) module.exports = Obs;
})(typeof window !== 'undefined' ? window : globalThis);
