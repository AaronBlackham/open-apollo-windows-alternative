'use strict';
/* Cyberpunk: the HUD (docs/THEMES.md 3). Segmented arc gauges with a numeric core and tick marks (circular drag around
   the dial, Shift or the wheel fine); thin blade sliders with a value tag riding the blade; cut-corner pills that scan on;
   hex-cell selectors; neon segmented meters with ghost peaks; plug-ins as mod cards with stat bars, sockets that light up
   under a targeting reticle while dragging, a glitch flash on a refusal. */
Skin.register('cyber', {
  knob: {
    build(k, R) {
      const S = R.svg, sz = k.o.size, n = 27, small = sz < 40;
      const g = S('svg', { class: 'sk-cyber sk-cyber-knob', width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true' });
      const segs = [];
      for (let i = 0; i < n; i++) {
        const a0 = -135 + i * 10 + 1.4, seg = S('path', { class: 'cy-seg', d: R.arc(50, 50, 42, a0, a0 + 7.2) });
        segs.push(seg);
        g.append(seg);
      }
      for (const t of [0, 0.25, 0.5, 0.75, 1]) {
        const a = -135 + t * 270, [x0, y0] = R.polar(50, 50, 29, a), [x1, y1] = R.polar(50, 50, 34, a);
        g.append(S('line', { class: 'cy-tick', x1: x0, y1: y0, x2: x1, y2: y1 }));
      }
      g.append(S('circle', { class: 'cy-core', cx: 50, cy: 50, r: small ? 18 : 25 }));
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      if (small) k.node.append(g, k.readout);
      else k.node.append(el('div', { class: 'cy-dial' }, g, k.readout));
      const off = R.circular(k, g);
      let last = '';
      return {
        draw({ p, p0 }) {
          const on = Math.round(p * n), o0 = Math.round(p0 * n), key = `${on}|${o0}`;
          if (key === last) return;
          last = key;
          segs.forEach((x, i) => x.classList.toggle('on', p0 > 0 ? i >= Math.min(on, o0) && i < Math.max(on, o0) : i < on));
        },
        destroy: off,
      };
    },
  },
  fader: {
    build(f, R) {
      f.cap = el('div', { class: 'fd-cap cy-blade' });
      const tag = R.deco('span', 'cy-tag');
      f.track = el('div', { class: 'fd-track cy-rail sk-cyber' }, f.cap, tag);
      f.node.append(f.scaleNode(), f.track);
      const off = f.trackDrag(f.track);
      return {
        draw({ p, text }) {
          const b = `${(p * 100).toFixed(2)}%`;
          f.cap.style.bottom = b;
          tag.style.bottom = b;
          const t = text.replace(' dB', '');
          if (tag.textContent !== t) tag.textContent = t;
        },
        destroy: off,
      };
    },
  },
  meter: {
    build(m, R) {
      m.meter.classList.add('cy-neon');
      const gh = R.deco('div', 'cy-ghost');
      m.bar.append(gh);
      return { paint(f, h, g) { gh.style.bottom = `${(g * 100).toFixed(2)}%`; gh.style.opacity = g > 0.004 ? '1' : '0'; } };
    },
  },
  button: { decorate(b, R) { b.append(R.deco('span', 'cy-scan')); } },
  selector: { decorate(n) { n._skinSet = null; } },
  slot: { decorate(b, R) { b.prepend(R.deco('span', 'cy-chip')); } },
  browser: {
    layout: 'cards',
    item(node, it, ctx, R) {
      const bar = (lbl, v, title) => el('span', { class: 'sk-deco cy-stat', 'aria-hidden': 'true', title },
        el('span', { class: 'cy-stat-l', text: lbl }), el('span', { class: 'cy-stat-b' }, el('span', { style: `width:${Math.round(clamp(v, 0, 1) * 100)}%` })));
      node.append(el('span', { class: 'sk-deco cy-stats', 'aria-hidden': 'true' },
        bar('DSP', (it.codeWords || 0) / 16384, `${it.codeWords || 0} code words`),
        bar('LAT', Math.min(1, (it.latency || 0) / 256), `${it.latency || 0} samples`),
        bar('PRM', Math.min(1, (it.params || 0) / 40), `${it.params || 0} parameters`)));
    },
  },
  params: { layout: 'module' },
  on: {
    refuse(node, R) { R.pulse(node, 'cy-glitch', 360); },
    drop(node, R) { R.pulse(node, 'cy-lock', 700); },
  },
  /* [Sess] the session list's layout (sessions.js): save states as HUD cards */
  sessions: { layout: 'hud' },
});
