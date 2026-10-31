// Проверка app/obs.js без настоящего OBS: имитация сервера (tools/mock-obs.mjs) и проверка файла коллекции.
// Запуск: node tools/test-obs.mjs   (нужен Node 22 или новее: встроенный WebSocket). Зависимостей нет.
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { startMock } from './mock-obs.mjs';
const require = createRequire(import.meta.url);
const Obs = require('../app/obs.js');

let bad = 0;
const ok = (c, msg) => { console.log((c ? 'ok     ' : 'ОШИБКА ') + msg); if (!c) bad++; };

/* ---------- проверки ---------- */
// 1. SHA-256 против встроенного в Node
{
  let same = true;
  for (const n of [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 1000]) {
    const b = crypto.randomBytes(n);
    if (Buffer.compare(Buffer.from(Obs.sha256(b)), crypto.createHash('sha256').update(b).digest()) !== 0) { same = false; console.log('  длина', n); }
  }
  ok(same, 'sha256 совпадает со встроенным на длинах 0…1000 (границы блока 55/56/64)');
  const t = 'пароль' + 'соль';
  ok(Obs.sha256b64(t) === crypto.createHash('sha256').update(t, 'utf8').digest('base64'), 'sha256 и base64 для строки с кириллицей');
}

// 2. подбор устройств
const caps = [
  { name: 'Микрофон (Realtek)', id: '{0.0.1.00000000}.{aaa}' },
  { name: 'Bogestv0 Games Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{g}' },
  { name: 'Bogestv0 Music Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{m}' },
  { name: 'Bogestv0 Discord Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{d}' },
  { name: 'Bogestv0 Browser 1 Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{b1}' },
  { name: 'Bogestv0 Browser 2 Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{b2}' },
  { name: 'Bogestv0 Mix B1 Out (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{x1}' },
  { name: 'Bogestv0 Output 2 (Bogestv0 Virtual Audio)', id: '{0.0.1.00000000}.{x2}' }   // старое имя шины B2
];
const names = ['Микрофон 1', 'Микрофон 2', 'Игры', 'Музыка', 'Discord', 'Браузер', 'Донаты', 'Браузер 2'];
const rows = Obs.plan(caps, names);
ok(rows.length === 8, 'в плане 8 строк (6 дорожек и 2 шины)');
ok(rows.filter((r) => r.ok).length === 7, 'найдено 7 из 8: нет только Donations Out');
ok(rows[4].tag === 'T5' && !rows[4].ok && /Donations/.test(rows[4].why), 'для ненайденного устройства есть понятная причина');
ok(rows[0].obsName === 'Bogestv0 Игры' && rows[6].obsName === 'Bogestv0 Mix B1', 'имена источников: «Bogestv0 Игры», «Bogestv0 Mix B1»');
ok(rows[7].ok && rows[7].deviceId.endsWith('{x2}'), 'старое имя «Bogestv0 Output 2» находится как шина B2');
ok(Obs.plan(caps.map((c) => ({ name: c.name })), names).every((r) => !r.ok && /идентификатор/.test(r.why) || !r.deviceName), 'без ID от движка источник не создаётся, причина понятна');
{
  const dup = Obs.plan(caps, ['', '', 'Игры', 'Игры', 'Discord', 'Браузер', 'Донаты', 'Браузер 2']);
  ok(dup[0].obsName !== dup[1].obsName, 'одинаковые имена дорожек получают разные имена в OBS');
  ok(!Obs.plan(caps, names).some((r, i, a) => a.findIndex((x) => x.obsName === r.obsName) !== i), 'имена источников не повторяются');
}
ok(Obs.plan(caps, names).filter((r) => r.ok).every((r) => caps.some((c) => c.id === r.deviceId && c.name === r.deviceName)), 'у каждой строки свой идентификатор устройства');

// 3. WebSocket с паролем, первая настройка
{
  const { srv, port, state } = await startMock({ password: 's3кр.ет', scenes: ['Сцена игры', 'Сцена 2'], inputs: [{ inputName: 'Звук рабочего стола', inputKind: 'wasapi_output_capture', settings: { device_id: 'default' } }] });
  const c = await Obs.connect({ port, password: 's3кр.ет' });
  ok(state.protocol === 'obswebsocket.json', 'запрошен подпротокол obswebsocket.json');
  const r = await Obs.sync(c, rows, { muteDesktop: true });
  ok(r.created.length === 7 && r.reused.length === 0, 'создано 7 источников «Захват входного аудио»');
  ok(state.scenes['Bogestv0 MIXER'].length === 7, 'все 7 лежат в сцене «Bogestv0 MIXER»');
  ok(state.scenes['Сцена игры'].some((i) => i.sourceName === 'Bogestv0 MIXER') && state.scenes['Сцена 2'].some((i) => i.sourceName === 'Bogestv0 MIXER'), 'эта сцена вложена в обе сцены пользователя');
  ok(!state.scenes['Bogestv0 MIXER'].some((i) => i.sourceName === 'Bogestv0 MIXER'), 'сцена не вложена сама в себя');
  const made = state.inputs.filter((i) => i.inputKind === 'wasapi_input_capture');
  ok(made.length === 7 && made.every((i) => /^\{0\.0\.1\.00000000\}/.test(i.settings.device_id) && i.settings.use_device_timing === false), 'у источников верный device_id');
  ok(r.muted.length === 1 && state.muted['Звук рабочего стола'] === true, '«Звук рабочего стола» заглушён по просьбе');
  // повторный запуск
  const n1 = state.calls.filter((x) => x[0] === 'CreateInput' || x[0] === 'CreateSceneItem').length;
  const r2 = await Obs.sync(c, rows, {});
  const n2 = state.calls.filter((x) => x[0] === 'CreateInput' || x[0] === 'CreateSceneItem').length;
  ok(r2.created.length === 0 && r2.reused.length === 7 && n1 === n2, 'повторный запуск ничего не создаёт и не дублирует');
  // пользователь переименовал источник и добавил новую сцену
  state.inputs.find((i) => i.inputName === 'Bogestv0 Музыка').inputName = 'Моя музыка';
  state.scenes['Моя музыка'] = undefined; delete state.scenes['Моя музыка'];
  state.scenes['Сцена 3'] = [];
  state.scenes['Bogestv0 MIXER'] = state.scenes['Bogestv0 MIXER'].map((i) => (i.sourceName === 'Bogestv0 Музыка' ? { ...i, sourceName: 'Моя музыка' } : i));
  const r3 = await Obs.sync(c, rows, {});
  ok(r3.created.length === 0 && r3.reused.includes('Моя музыка') && r3.scenesAdded.join() === 'Сцена 3', 'переименованный источник находится по устройству; новая сцена получает вложение');
  c.close(); srv.close();
}

// 4. неверный пароль, пароль не нужен, пароль нужен но не задан, нет сервера
{
  const a = await startMock({ password: 'верный', scenes: ['С'], inputs: [] });
  let msg = '';
  try { await Obs.connect({ port: a.port, password: 'неверный' }); } catch (e) { msg = e.message; }
  ok(/пароль/.test(msg), 'неверный пароль: «' + msg + '»');
  msg = '';
  try { await Obs.connect({ port: a.port, password: '' }); } catch (e) { msg = e.message; }
  ok(/пароль/.test(msg), 'пароль нужен, но не задан: «' + msg + '»');
  a.srv.close();
  const b = await startMock({ password: '', scenes: ['С'], inputs: [] });
  const c = await Obs.connect({ port: b.port, password: '' });
  const r = await Obs.sync(c, rows, {});
  ok(r.created.length === 7 && b.state.scenes['С'].length === 1, 'без пароля тоже работает; одна сцена получила вложение');
  c.close(); b.srv.close();
  msg = '';
  try { await Obs.connect({ port: 1, password: '', timeout: 2000 }); } catch (e) { msg = e.message; }
  ok(/не удалось подключиться|не отвечает/.test(msg), 'OBS не запущен: «' + msg + '»');
}

// 5. файл коллекции
{
  const col = Obs.collection(rows, 'Bogestv0 MIXER');
  const txt = JSON.stringify(col);
  const back = JSON.parse(txt);
  const srcs = back.sources;
  const scene = srcs.find((s) => s.id === 'scene');
  const audio = srcs.filter((s) => s.id === 'wasapi_input_capture');
  ok(back.name === 'Bogestv0 MIXER' && back.current_scene === scene.name && back.scene_order[0].name === scene.name, 'коллекция: имя, текущая сцена, порядок сцен');
  ok(audio.length === 7 && scene.settings.items.length === 7, 'коллекция: 7 источников и 7 элементов сцены');
  ok(scene.settings.items.every((it) => audio.some((a) => a.uuid === it.source_uuid && a.name === it.name)), 'каждый элемент сцены указывает на свой источник (по uuid и имени)');
  ok(new Set(srcs.map((s) => s.uuid)).size === srcs.length && new Set(srcs.map((s) => s.name)).size === srcs.length, 'uuid и имена источников уникальны');
  ok(audio.every((a) => a.settings.device_id && a.mixers === 255 && a.monitoring_type === 0 && !a.muted), 'источники: device_id, все дорожки, без мониторинга, не заглушены');
  ok(scene.settings.id_counter === 7 && new Set(scene.settings.items.map((i) => i.id)).size === 7, 'id_counter и номера элементов согласованы');
  ok(!('DesktopAudioDevice1' in back) && !('AuxAudioDevice1' in back), 'в коллекции нет глобального «Звука рабочего стола» (звук не задвоится)');
  let msg = '';
  try { Obs.collection(Obs.plan([], names), 'x'); } catch (e) { msg = e.message; }
  ok(/нет ни одного/.test(msg), 'пустой список устройств не даёт пустую коллекцию');
}

console.log(bad ? '\nЕсть ошибки: ' + bad : '\nВсе проверки пройдены.');
process.exit(bad ? 1 : 0);
