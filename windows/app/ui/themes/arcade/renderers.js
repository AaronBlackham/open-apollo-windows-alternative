'use strict';
/* Arcade: the cabinet (docs/THEMES.md 3, the one deliberately retro theme). Pixel-art knobs with an 8-direction pointer and
   stepped values (a click per step); chunky block fader caps on a stepped track; big buttons that press down 2 px; level-
   select cursors on selectors; health-bar meters and 7-segment readouts; plug-ins as a power-up inventory, a coin blip when
   one drops into a slot (visual only), a small shake on a clip (off with reduced motion). */
const ARC_PIX = (() => {
  /* a 16 x 16 pixel dial: ring and face as single paths */
  const px = (x, y) => `M${x} ${y}h1v1h-1z`;
  let ring = '', face = '';
  for (let y = 0; y < 16; y++) {
    for (let x = 0; x < 16; x++) {
      const d = Math.hypot(x + 0.5 - 8, y + 0.5 - 8);
      if (d < 7.4 && d >= 6) ring += px(x, y);
      else if (d < 6) face += px(x, y);
    }
  }
  return { ring, face, px };
})();
Skin.register('arcade', {
  knob: {
    build(k, R) {
      const S = R.svg, sz = k.o.size;
      const g = S('svg', { class: 'sk-arcade sk-arcade-knob', width: sz, height: sz, viewBox: '0 0 16 16', 'shape-rendering': 'crispEdges', 'aria-hidden': 'true' });
      const ptr = S('path', { class: 'ar-ptr' });
      g.append(S('path', { class: 'ar-ring', d: ARC_PIX.ring }), S('path', { class: 'ar-face', d: ARC_PIX.face }), ptr);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(g, k.readout);
      const off = R.stepped(k, g, { steps: 16, px: 8 });
      let last = -9;
      return {
        draw({ p }) {
          const dir = Math.round((-135 + p * 270) / 45);              /* eight directions */
          if (dir === last) return;
          last = dir;
          const a = dir * 45 * Math.PI / 180;
          let d = '';
          for (const r0 of [1.5, 3, 4.5]) d += ARC_PIX.px(Math.floor(8 + r0 * Math.sin(a) - 0.5 + 0.5), Math.floor(8 - r0 * Math.cos(a) - 0.5 + 0.5));
          ptr.setAttribute('d', d);
        },
        destroy: off,
      };
    },
  },
  fader: {
    build(f, R) {
      f.cap = el('div', { class: 'fd-cap ar-block' }, R.deco('span', 'ar-grip'));
      f.track = el('div', { class: 'fd-track ar-rail sk-arcade' }, f.cap);
      f.node.append(f.scaleNode(), f.track);
      const off = f.trackDrag(f.track, { steps: 36 });
      return { draw({ p }) { f.cap.style.bottom = `${(Math.round(p * 36) / 36 * 100).toFixed(2)}%`; }, destroy: off };
    },
  },
  meter: { build(m) { m.meter.classList.add('ar-health'); return { paint() {} }; } },
  button: { decorate() {} },
  selector: {
    decorate(n, R) {
      n._skinSet = null;
      if (n.querySelectorAll(':scope > button').length < 3) return;
      for (const b of n.querySelectorAll(':scope > button')) b.prepend(R.deco('span', 'ar-cursor'));
    },
  },
  slot: { decorate() {} },
  browser: { layout: 'tiles' },
  params: { layout: 'item' },
  on: {
    drop(node) {
      const row = node.closest('.ins-row') || node.parentElement;
      if (!row) return;
      const coin = el('span', { class: 'sk-deco ar-coin', 'aria-hidden': 'true' });
      row.append(coin);
      setTimeout(() => coin.remove(), 800);
    },
    clip(node, R) { const sg = node.closest('.useg'); if (sg) R.pulse(sg, 'ar-shake', 260); },
    refuse(node, R) { R.pulse(node, 'ar-buzz', 400); },
  },
  /* [Sess] the session list's layout (sessions.js): SAVE SLOTS */
  sessions: { layout: 'slots' },
});
