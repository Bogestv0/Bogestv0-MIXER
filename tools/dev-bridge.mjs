// Тестовый мост для работы в обычном браузере (без Tauri): запускает движок, раздаёт app/ и связывает их.
// Запуск: node tools/dev-bridge.mjs [путь/к/engine] [порт]   затем открыть http://localhost:8766/main.html
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '../app');
const enginePath = process.argv[2] || 'engine';
const port = Number(process.argv[3] || 8766);
const settingsFile = path.join(import.meta.dirname, '.dev-settings.json');
const boot = Date.now();
const devDir = path.join(import.meta.dirname, '.dev-data');   // звуки и записи при проверке в браузере

const engine = spawn(enginePath, ['--serve'], { stdio: ['pipe', 'pipe', 'inherit'] });
const clients = new Set();
let buf = '';
engine.stdout.setEncoding('utf8');
engine.stdout.on('data', (d) => {
  buf += d;
  let n;
  while ((n = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, n).replace(/\r$/, '');
    buf = buf.slice(n + 1);
    for (const c of clients) c.write(`data: ${line}\n\n`);
  }
});
engine.on('exit', () => { for (const c of clients) c.write('data: {"t":"exit"}\n\n'); });

const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css', '.png': 'image/png', '.woff2': 'font/woff2' };
http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (url.pathname === '/info') { res.setHeader('content-type', 'application/json'); return res.end(JSON.stringify({ boot })); }
  if (url.pathname === '/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
    res.write('\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }
  if (url.pathname === '/send' && req.method === 'POST') {
    let b = '';
    req.on('data', (d) => (b += d));
    req.on('end', () => { engine.stdin.write(b.replace(/[\r\n]+/g, ' ') + '\n'); res.end('ok'); });
    return;
  }
  if (url.pathname === '/settings') {
    if (req.method === 'POST') {
      let b = '';
      req.on('data', (d) => (b += d));
      req.on('end', () => { fs.writeFileSync(settingsFile, b); res.end('ok'); });
      return;
    }
    return res.end(fs.existsSync(settingsFile) ? fs.readFileSync(settingsFile, 'utf8') : '');
  }
  if (url.pathname === '/dirs') {
    res.setHeader('content-type', 'application/json');
    return res.end(JSON.stringify({ rec: path.join(devDir, 'records'), sounds: path.join(devDir, 'sounds') }));
  }
  if (url.pathname === '/sound' && req.method === 'POST') {
    const chunks = [];
    req.on('data', (d) => chunks.push(d));
    req.on('end', () => {
      fs.mkdirSync(path.join(devDir, 'sounds'), { recursive: true });
      const name = (url.searchParams.get('name') || 'sound').replace(/[^\p{L}\p{N}._ -]/gu, '_');
      const file = path.join(devDir, 'sounds', `${Date.now()}-${name}`);
      fs.writeFileSync(file, Buffer.concat(chunks));
      res.end(file);
    });
    return;
  }
  if (url.pathname === '/reveal') { res.end('ok'); return; }
  const file = path.join(root, url.pathname === '/' ? 'main.html' : url.pathname);
  if (!file.startsWith(root) || !fs.existsSync(file)) { res.statusCode = 404; return res.end('not found'); }
  res.setHeader('content-type', types[path.extname(file)] || 'application/octet-stream');
  fs.createReadStream(file).pipe(res);
}).listen(port, () => console.log(`http://localhost:${port}/main.html`));
process.on('exit', () => engine.kill());
process.on('SIGINT', () => process.exit(0));
