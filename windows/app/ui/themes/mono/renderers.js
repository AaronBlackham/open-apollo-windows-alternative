'use strict';
/* Mono: the terminal (docs/THEMES.md 3). Every rotary control is a horizontal value bar with its number (press anywhere on
   the bar: the value is where the pointer is); faders are bars too; toggles are [x] / [ ] text; selectors bracket the
   choice; meters are thin lines with numbers; plug-ins are a text list with columns. No motion, nothing decorative. */
Skin.register('mono', {
  knob: {
    build(k, R) {
      const fill = el('span', { class: 'mn-fill' }), bar = el('span', { class: 'mn-bar sk-mono sk-mono-knob' }, fill);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(bar, k.readout);
      const off = R.horizontal(k, bar);
      return {
        draw({ p, p0 }) {
          const a = Math.min(p, p0), b = Math.max(p, p0);
          fill.style.left = `${(a * 100).toFixed(2)}%`;
          fill.style.width = `${((b - a) * 100).toFixed(2)}%`;
        },
        destroy: off,
      };
    },
  },
  fader: {
    /* a horizontal bar (rows): press anywhere, the value is where the pointer is */
    build(f, R) {
      f.fill = el('div', { class: 'fd-fill mn-hfill' });
      f.track = el('div', { class: 'fd-track mn-hbar sk-mono' }, f.fill);
      f.node.append(f.track);
      f.node.setAttribute('aria-orientation', 'horizontal');
      const off = R.horizontal(f, f.track);
      return { draw({ p }) { f.fill.style.width = `${(p * 100).toFixed(2)}%`; }, destroy: off };
    },
  },
  meter: { build(m) { m.meter.classList.add('mn-line'); m.horiz = m.group !== 'channels'; return { paint() {} }; } },
  button: { decorate(b, R) { b.prepend(R.deco('span', 'mn-chk')); } },
  selector: {
    decorate(n, R) {
      n._skinSet = null;
      for (const b of n.querySelectorAll(':scope > button')) { b.prepend(R.deco('span', 'mn-l')); b.append(R.deco('span', 'mn-r')); }
    },
  },
  slot: { decorate() {} },
  browser: {
    layout: 'text',
    flat: true,
    columns: [['name', 'NAME'], ['type', 'TYPE'], ['code', 'CODE'], ['lat', 'LAT'], ['lic', 'LIC']],
    item(node, it, ctx) {
      const c = (cls, t) => el('span', { class: `sk-deco lb-c lb-${cls}`, 'aria-hidden': 'true', text: t });
      node.append(c('type', ctx.group.toLowerCase()), c('code', String(ctx.need)), c('lat', String(it.latency || 0)),
        c('lic', it.licensed === true ? 'y' : it.licensed === false ? 'n' : '?'));
    },
  },
  params: { layout: 'rows' },
  /* [Sess] the session list's layout (sessions.js): text rows and a session> prompt (session save / load on the command line) */
  sessions: { layout: 'terminal' },
});
