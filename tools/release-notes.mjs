// Печатает раздел CHANGELOG.md для версии: "## [0.0.1] ..." до следующего "## ".
import fs from 'node:fs';
import path from 'node:path';
const ver = process.argv[2];
const text = fs.readFileSync(path.resolve(import.meta.dirname, '../CHANGELOG.md'), 'utf8');
const lines = text.split('\n');
const start = lines.findIndex((l) => l.startsWith('## [' + ver + ']'));
if (start < 0) { console.error('В CHANGELOG.md нет раздела для версии ' + ver); process.exit(1); }
let end = lines.findIndex((l, i) => i > start && l.startsWith('## '));
if (end < 0) end = lines.length;
console.log(lines.slice(start + 1, end).join('\n').trim());
