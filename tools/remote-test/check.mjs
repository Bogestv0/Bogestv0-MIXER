// Проверка протокола пульта против tools/remote-test (cargo run -- 8099 /tmp/remote-test.log 4817).
// Запуск: node check.mjs [порт] [журнал]
import fs from 'node:fs';
const port = process.argv[2] || 8099, log = process.argv[3] || '/tmp/remote-test.log';
let bad = 0;
const ok = (c, what) => { console.log((c ? 'ok     ' : 'ОШИБКА ') + what); if (!c) bad++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const lines = () => (fs.existsSync(log) ? fs.readFileSync(log, 'utf8').split('\n').filter(Boolean) : []);

function open(headers) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`, headers ? { headers } : undefined);
    ws.binaryType = 'arraybuffer';
    ws.msgs = []; ws.bins = [];
    ws.onmessage = (e) => (typeof e.data === 'string' ? ws.msgs.push(JSON.parse(e.data)) : ws.bins.push(new Uint8Array(e.data)));
    ws.onopen = () => res(ws);
    ws.onerror = () => rej(new Error('ws error'));
  });
}
const has = (ws, t, f) => ws.msgs.find((m) => m.t === t && (!f || f(m)));

// 1. Страница и файлы
for (const [p, ct] of [['/', 'text/html'], ['/remote.js', 'javascript'], ['/fonts.css', 'text/css'], ['/logo.png', 'image/png'], ['/fonts/onest-latin-400-normal.woff2', 'font/woff2']]) {
  const r = await fetch(`http://127.0.0.1:${port}${p}`);
  ok(r.status === 200 && (r.headers.get('content-type') || '').includes(ct), `GET ${p}: 200, ${ct}`);
}
const root = await fetch(`http://127.0.0.1:${port}/`);
ok((root.headers.get('content-security-policy') || '').includes("default-src 'none'"), 'у страницы строгая политика безопасности (CSP)');
ok((await fetch(`http://127.0.0.1:${port}/fonts/../secret`)).status === 404, 'чужие файлы по пути не отдаются');
ok((await fetch(`http://127.0.0.1:${port}/fonts/nope.woff2`)).status === 404, 'нет такого шрифта: 404');

// 2. Защита: чужой сайт (Origin) не подключится
let blocked = false;
try { await open({ Origin: 'http://evil.example' }); } catch (e) { blocked = true; }
ok(blocked, 'WebSocket с чужого сайта (другой Origin) отклонён');
const own = await open({ Origin: `http://127.0.0.1:${port}` });
ok(!!own, 'WebSocket со страницы пульта (тот же Origin) принят');
own.close();

// 3. PIN
const a = await open();
await sleep(150);
ok(has(a, 'auth', (m) => m.ok === false && m.need === true), 'сервер просит PIN');
a.send(JSON.stringify({ t: 'gain', i: 2, v: -1 }));
await sleep(150);
ok(!lines().some((l) => l.startsWith('set strip 2 gain')), 'без PIN команды не выполняются');
a.send(JSON.stringify({ t: 'auth', pin: '0000' }));
await sleep(1200);
ok(has(a, 'auth', (m) => m.ok === false && m.msg === 'Неверный PIN'), 'неверный PIN отклонён');
a.send(JSON.stringify({ t: 'auth', pin: '4817' }));
await sleep(300);
ok(has(a, 'auth', (m) => m.ok === true), 'верный PIN принят');
ok(has(a, 'meta') && has(a, 'state'), 'после входа пришли имена дорожек и состояние');
const meta = has(a, 'meta');
ok(meta.strips[7].name === '<b>Браузер 2</b>' && meta.strips[1].hidden === true && meta.scenes.length === 2, 'meta: имена как есть (экранирует страница), скрытая дорожка помечена, сцены есть');

// 4. Команды
const send = async (o) => { a.send(JSON.stringify(o)); await sleep(120); };
await send({ t: 'gain', i: 3, v: -6.26 });
await send({ t: 'gain', i: 3, v: 99 });
await send({ t: 'gain', i: 3, v: 'abc' });
await send({ t: 'gain', i: 9, v: 0 });
await send({ t: 'mute', i: 4, v: true });
await send({ t: 'solo', i: 2, v: 1 });
await send({ t: 'route', i: 5, k: 3, v: 1 });
await send({ t: 'route', i: 5, k: 7, v: 1 });
await send({ t: 'rm -rf', i: 1 });
const L = lines();
ok(L.includes('set strip 3 gain -6.3'), 'громкость: «set strip 3 gain -6.3»');
ok(L.includes('set strip 3 gain 12.0'), 'громкость ограничена сверху +12 дБ');
ok(!L.some((l) => l.includes('abc') || l.includes('strip 9')), 'мусорное число и неверная дорожка отброшены');
ok(L.includes('set strip 4 mute 1') && L.includes('set strip 2 solo 1') && L.includes('set strip 5 route 3 1'), 'mute, solo и маршрут дошли до движка');
ok(!L.some((l) => l.includes('route 7') || l.includes('rm')), 'неверный маршрут и неизвестная команда отброшены');
const stNow = a.msgs.filter((m) => m.t === 'state').pop();
ok(stNow.strips[3].g === 12 && stNow.strips[4].m === 1 && stNow.strips[2].s === 1, 'состояние движка вернулось к телефону');
ok(a.msgs.some((m) => m.t === 'm'), 'уровни приходят');

// 5. Сцена
await send({ t: 'scene', id: 'call' });
ok(lines().includes('SCENE call') && lines().includes('set strip 2 route 1 1') && lines().includes('set strip 2 route 0 0'), 'сцена «Звонок»: маршруты разосланы, окно уведомлено');
await send({ t: 'scene', id: 'нет' });
ok(has(a, 'err', (m) => /не найдена/.test(m.msg)), 'неизвестная сцена даёт ошибку');

// 6. Прослушивание
await send({ t: 'listen', i: 3 });
await sleep(400);
ok(lines().includes('set listen 3'), 'движку отправлено «set listen 3»');
ok(a.bins.length >= 10, `пришёл звук (${a.bins.length} кадров за 0,5 с)`);
const f = a.bins[a.bins.length - 1];
ok(f[0] === 1 && f[1] === 3 && f.length === 2 + 960 * 4, 'кадр: тип 1, дорожка 3, 20 мс стерео 16 бит');
const dv = new DataView(f.buffer, f.byteOffset);
let peak = 0; for (let k = 0; k < 960; k++) peak = Math.max(peak, Math.abs(dv.getInt16(2 + k * 4, true)));
ok(peak > 8000 && peak < 12000, 'в кадре синус нужной громкости');
const b = await open();   // второй телефон, тоже вошёл
b.send(JSON.stringify({ t: 'auth', pin: '4817' })); await sleep(300);
const before = b.bins.length; await sleep(300);
ok(b.bins.length === before, 'второй телефон не получает звук, пока сам не нажал «Слушать»');
ok(stNow && a.msgs.filter((m) => m.t === 'state').pop().listen === 3, 'состояние сообщает, какая дорожка слушается');
await send({ t: 'listen', i: null });
await sleep(200);
ok(lines().includes('set listen off'), 'стоп: движку отправлено «set listen off»');
const n1 = a.bins.length; await sleep(300);
ok(a.bins.length === n1, 'после стопа звук не идёт');

// 7. Обрыв связи останавливает прослушивание
await send({ t: 'listen', i: 4 }); await sleep(200);
const c0 = lines().filter((l) => l === 'set listen off').length;
a.close(); await sleep(400);
ok(lines().filter((l) => l === 'set listen off').length === c0 + 1, 'телефон пропал: прослушивание выключено само');

// 8. Перебор PIN
const c = await open(); await sleep(100);
for (let k = 0; k < 5; k++) { c.send(JSON.stringify({ t: 'auth', pin: '1111' })); await sleep(900); }
c.send(JSON.stringify({ t: 'auth', pin: '4817' })); await sleep(300);
ok(has(c, 'auth', (m) => m.ok === false && /Слишком много/.test(m.msg || '')), 'после 5 ошибок вход закрыт, даже с верным PIN');
b.close(); c.close();

console.log(bad ? `\nОШИБОК: ${bad}` : '\nВсе проверки пройдены.');
process.exit(bad ? 1 : 0);
