// Конвертер: ui/*.dc.html -> app/*.html (обычные страницы с dc.js).
// Запуск: node tools/port-screens.mjs
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(import.meta.dirname, '..');
const OUT = path.join(root, 'app');
const slug = (f) => f.replace(/\.dc\.html$/, '').toLowerCase() + '.html';

const jobs = [];
for (const f of fs.readdirSync(path.join(root, 'ui'))) if (f.endsWith('.dc.html')) jobs.push([path.join(root, 'ui', f), f]);
for (const f of fs.readdirSync(path.join(root, 'ui/sketch-original'))) if (f.endsWith('.dc.html')) jobs.push([path.join(root, 'ui/sketch-original', f), f]);

for (const [file, name] of jobs) {
  const src = fs.readFileSync(file, 'utf8');
  const title = (/<title>([^<]*)<\/title>/.exec(src) || [, 'Bogestv0 MIXER'])[1];
  const helmet = /<helmet>([\s\S]*?)<\/helmet>/.exec(src)[1];
  const css = (/<style>([\s\S]*?)<\/style>/.exec(helmet) || [, ''])[1];
  let body = src.slice(src.indexOf('</helmet>') + 9, src.lastIndexOf('</x-dc>'));
  const sm = /<script type="text\/x-dc"[^>]*>([\s\S]*?)<\/script>/.exec(src);
  const logic = sm ? sm[1] : 'class Component extends DCLogic { renderVals() { return {}; } }';

  body = body
    .replace(/\/_blob\/[0-9a-f]+/g, 'logo.png')
    .replace(/width:1440px;height:880px/, 'width:100vw;height:100vh')
    .replace(/href="([A-Za-z]+)\.dc\.html"/g, (_, n) => `href="${n.toLowerCase()}.html"`)
    .replace(/ hint-[a-z-]+="[^"]*"/g, '');
  const logic2 = logic.replace(/'([A-Za-z]+)\.dc\.html'/g, (_, n) => `'${n.toLowerCase()}.html'`);

  const html = `<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title}</title>
<link rel="stylesheet" href="fonts.css">
<style>
${css.trim()}
html,body{height:100%;overflow:hidden}
</style>
<script src="dc.js"></script>
</head>
<body>
<template id="tpl">
${body.trim()}
</template>
<div id="app"></div>
<script>
${logic2.trim()}
</script>
<script>DC.mount(Component);</script>
<script src="app.js"></script>
</body>
</html>
`;
  fs.writeFileSync(path.join(OUT, slug(name)), html);
  console.log('ok', slug(name));
}
