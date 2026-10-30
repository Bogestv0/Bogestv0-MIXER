/* Мини-рантайм для экранов макета: шаблон {{путь}}, sc-for, sc-if, DCLogic.
   Нужен только на этапе 1. На этапе 3 экраны получают состояние от движка. */
(function () {
  function get(scope, path) {
    path = path.trim();
    if (path === 'true') return true;
    if (path === 'false') return false;
    return path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), scope);
  }
  const RE = /\{\{([^}]*)\}\}/g;
  const interp = (s, scope) => s.replace(RE, (_, p) => { const v = get(scope, p); return v == null ? '' : String(v); });
  const whole = (s) => { const m = /^\s*\{\{([^}]*)\}\}\s*$/.exec(s); return m ? m[1] : null; };

  function build(node, scope, out) {
    if (node.nodeType === 3) { out.appendChild(document.createTextNode(interp(node.nodeValue, scope))); return; }
    if (node.nodeType !== 1) return;
    const tag = node.tagName.toLowerCase();
    if (tag === 'sc-for') {
      const list = get(scope, whole(node.getAttribute('list')) || '') || [];
      const as = node.getAttribute('as') || 'item';
      list.forEach((item) => {
        const sub = Object.assign(Object.create(scope), { [as]: item });
        node.childNodes.forEach((c) => build(c, sub, out));
      });
      return;
    }
    if (tag === 'sc-if') {
      if (get(scope, whole(node.getAttribute('value')) || '')) node.childNodes.forEach((c) => build(c, scope, out));
      return;
    }
    const el = node.cloneNode(false);
    for (const a of Array.from(node.attributes)) {
      if (a.name.startsWith('hint-')) { el.removeAttribute(a.name); continue; }
      if (a.name.startsWith('on')) {
        el.removeAttribute(a.name);
        const p = whole(a.value);
        const fn = p && get(scope, p);
        if (typeof fn === 'function') el.addEventListener(a.name.slice(2).toLowerCase(), fn);
        continue;
      }
      if (a.value.includes('{{')) el.setAttribute(a.name, interp(a.value, scope));
    }
    node.childNodes.forEach((c) => build(c, scope, el));
    out.appendChild(el);
  }

  class DCLogic {
    constructor(props) { this.props = props || {}; this.state = {}; this._render = null; }
    setState(patch) { this.state = Object.assign({}, this.state, patch); if (this._render) this._render(); }
  }

  function mount(Component) {
    const root = document.getElementById('app');
    const tpl = document.getElementById('tpl').content;
    const inst = new Component({});
    inst._render = () => {
      const vals = inst.renderVals();
      const frag = document.createDocumentFragment();
      tpl.childNodes.forEach((c) => build(c, vals, frag));
      root.replaceChildren(frag);
      document.dispatchEvent(new CustomEvent('dc-rendered'));
    };
    inst._render();
    window.__dc = inst;
    return inst;
  }

  window.DCLogic = DCLogic;
  window.DC = { mount };
})();
