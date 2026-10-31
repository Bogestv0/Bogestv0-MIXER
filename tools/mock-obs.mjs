// Имитация сервера obs-websocket 5 (с паролем) для проверок без настоящего OBS. Без зависимостей.
// Повторяет протокол по документации obs-websocket 5.x; поведение настоящего OBS она не заменяет.
import http from 'node:http';
import crypto from 'node:crypto';

function frame(text) {
  const p = Buffer.from(text);
  const h = p.length < 126 ? Buffer.from([0x81, p.length]) : p.length < 65536 ? Buffer.from([0x81, 126, p.length >> 8, p.length & 255]) : null;
  return Buffer.concat([h, p]);
}
function* parse(buf) {   // кадры клиента (с маской): только текст и закрытие
  let o = 0;
  while (o + 2 <= buf.length) {
    const op = buf[o] & 15;
    let len = buf[o + 1] & 127, p = o + 2;
    if (len === 126) { len = buf.readUInt16BE(p); p += 2; } else if (len === 127) { len = Number(buf.readBigUInt64BE(p)); p += 8; }
    const mask = buf.subarray(p, p + 4); p += 4;
    const data = Buffer.from(buf.subarray(p, p + len));
    for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
    o = p + len;
    yield { op, text: data.toString('utf8') };
  }
}

export function startMock({ password, scenes, inputs }) {
  const state = {
    scenes: Object.fromEntries(scenes.map((s) => [s, []])),        // сцена -> [{id, sourceName}]
    inputs: inputs.map((i) => ({ ...i })),                          // {inputName, inputKind, settings}
    muted: {}, calls: [], protocol: ''
  };
  const srv = http.createServer();
  srv.on('upgrade', (req, sock) => {
    state.protocol = req.headers['sec-websocket-protocol'] || '';
    const key = req.headers['sec-websocket-key'];
    const acc = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + acc + '\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n\r\n');
    const send = (o) => sock.write(frame(JSON.stringify(o)));
    const salt = crypto.randomBytes(16).toString('base64'), challenge = crypto.randomBytes(16).toString('base64');
    send({ op: 0, d: { obsWebSocketVersion: '5.5.4', rpcVersion: 1, ...(password ? { authentication: { challenge, salt } } : {}) } });
    sock.on('data', (buf) => {
      for (const f of parse(buf)) {
        if (f.op === 8) { sock.end(); return; }
        const m = JSON.parse(f.text);
        if (m.op === 1) {
          if (password) {
            const secret = crypto.createHash('sha256').update(password + salt).digest('base64');
            const want = crypto.createHash('sha256').update(secret + challenge).digest('base64');
            if (m.d.authentication !== want) { sock.write(Buffer.from([0x88, 2, 4009 >> 8, 4009 & 255])); sock.end(); return; }   // закрытие с кодом 4009 (AuthenticationFailed)
          }
          send({ op: 2, d: { negotiatedRpcVersion: 1 } });
        } else if (m.op === 6) {
          const { requestType: t, requestId: id, requestData: d = {} } = m.d;
          state.calls.push([t, d]);
          const reply = (result, code, comment, data) => send({ op: 7, d: { requestType: t, requestId: id, requestStatus: { result, code, ...(comment ? { comment } : {}) }, ...(data ? { responseData: data } : {}) } });
          const nameTaken = (n) => state.inputs.some((i) => i.inputName === n) || n in state.scenes;
          if (t === 'GetSceneList') reply(true, 100, '', { currentProgramSceneName: Object.keys(state.scenes)[0], scenes: Object.keys(state.scenes).map((s, i) => ({ sceneName: s, sceneIndex: i })) });
          else if (t === 'CreateScene') { if (nameTaken(d.sceneName)) reply(false, 601, 'Already exists'); else { state.scenes[d.sceneName] = []; reply(true, 100, '', {}); } }
          else if (t === 'GetInputList') reply(true, 100, '', { inputs: state.inputs.filter((i) => !d.inputKind || i.inputKind === d.inputKind).map((i) => ({ inputName: i.inputName, inputKind: i.inputKind })) });
          else if (t === 'GetInputSettings') { const i = state.inputs.find((x) => x.inputName === d.inputName); i ? reply(true, 100, '', { inputSettings: i.settings, inputKind: i.inputKind }) : reply(false, 600, 'No input'); }
          else if (t === 'GetSceneItemList') { const s = state.scenes[d.sceneName]; s ? reply(true, 100, '', { sceneItems: s.map((x) => ({ sceneItemId: x.id, sourceName: x.sourceName })) }) : reply(false, 600, 'No scene'); }
          else if (t === 'CreateInput') {
            if (nameTaken(d.inputName)) return reply(false, 601, 'Already exists');
            if (!state.scenes[d.sceneName]) return reply(false, 600, 'No scene');
            state.inputs.push({ inputName: d.inputName, inputKind: d.inputKind, settings: d.inputSettings || {} });
            const list = state.scenes[d.sceneName];
            list.push({ id: list.length + 1, sourceName: d.inputName });
            reply(true, 100, '', { sceneItemId: list.length });
          } else if (t === 'CreateSceneItem') {
            const list = state.scenes[d.sceneName];
            const known = state.inputs.some((i) => i.inputName === d.sourceName) || d.sourceName in state.scenes;
            if (!list || !known) return reply(false, 600, 'Not found');
            if (d.sourceName === d.sceneName) return reply(false, 506, 'Scene would contain itself');
            list.push({ id: list.length + 1, sourceName: d.sourceName });
            reply(true, 100, '', { sceneItemId: list.length });
          } else if (t === 'GetSpecialInputs') reply(true, 100, '', { desktop1: 'Звук рабочего стола', desktop2: null, mic1: 'Микрофон', mic2: null, mic3: null, mic4: null });
          else if (t === 'SetInputMute') { state.muted[d.inputName] = d.inputMuted; reply(true, 100, '', {}); }
          else reply(false, 204, 'Unknown request type');
        }
      }
    });
  });
  return new Promise((res) => srv.listen(0, '127.0.0.1', () => res({ srv, port: srv.address().port, state })));
}

