// Проверяет, что версия в теге совпадает с версией в файлах проекта.
import fs from 'node:fs';
import path from 'node:path';
const root = path.resolve(import.meta.dirname, '..');
const want = process.argv[2];
const read = (p) => fs.readFileSync(path.join(root, p), 'utf8');
const found = {
  'package.json': JSON.parse(read('package.json')).version,
  'src-tauri/tauri.conf.json': JSON.parse(read('src-tauri/tauri.conf.json')).version,
  'src-tauri/Cargo.toml': /^version\s*=\s*"([^"]+)"/m.exec(read('src-tauri/Cargo.toml'))?.[1]
};
let bad = false;
for (const [f, v] of Object.entries(found)) {
  const ok = v === want;
  console.log(`${ok ? 'ok ' : 'ОШИБКА'} ${f}: ${v}`);
  if (!ok) bad = true;
}
if (bad) { console.error(`Версия тега (${want}) не совпадает с файлами проекта.`); process.exit(1); }
