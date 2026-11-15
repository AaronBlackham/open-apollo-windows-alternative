'use strict';
/* Day: the guide (docs/THEMES.md 3). Large round knobs with a thick value arc and a dot (gentle easing); pill sliders with
   a round thumb; switches as toggle pills; selectors as segmented pills; level bars with too quiet / good / too loud zones;
   plug-ins browsed by purpose ("Clearer vocals", "Control peaks", "Add space", ...), the slot a friendly "add" pill. */
const DAY_PURPOSES = [
  ['vocals', 'Clearer vocals', 'EQs and de-essers'],
  ['peaks', 'Control peaks', 'compressors and limiters'],
  ['space', 'Add space', 'reverbs and delays'],
  ['warmth', 'Warmth', 'preamps, channel strips and tape'],
  ['guitar', 'Guitar tone', 'amps and cabinets'],
  ['motion', 'Movement', 'chorus, flanger, phaser'],
  ['finish', 'Finish a mix', 'mastering'],
  ['other', 'Something else', 'special tools'],
];
Skin.register('day', {
  knob: {
    build(k, R) {
      const S = R.svg, sz = k.o.size;
      const g = S('svg', { class: 'sk-day sk-day-knob', width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true' });
      const val = S('path', { class: 'dy-val' });
      const dot = S('g', { class: 'dy-dotg' });
      dot.append(S('circle', { class: 'dy-dot', cx: 50, cy: 26, r: 4.5 }));
      g.append(S('path', { class: 'dy-track', d: R.arc(50, 50, 40, -135, 135) }), val, S('circle', { class: 'dy-face', cx: 50, cy: 50, r: 31 }), dot);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(g, k.readout);
      const off = R.vertical(k, g, { travel: 260, detents: k.o.origin ? [k.o.origin] : [] });
      return {
        draw({ p, p0 }) {
          const a = -135 + p * 270, b = -135 + p0 * 270;
          val.setAttribute('d', Math.abs(p - p0) > 0.002 ? R.arc(50, 50, 40, Math.min(a, b), Math.max(a, b)) : '');
          dot.style.transform = `rotate(${a.toFixed(1)}deg)`;
        },
        destroy: off,
      };
    },
  },
  fader: {
    build(f) {
      f.fill = el('div', { class: 'fd-fill dy-pfill' });
      f.cap = el('div', { class: 'fd-cap dy-thumb' });
      f.track = el('div', { class: 'fd-track dy-pill sk-day' }, f.fill, f.cap);
      f.node.append(f.scaleNode(), f.track);
      const off = f.trackDrag(f.track);
      return { draw({ p }) { const b = `${(p * 100).toFixed(2)}%`; f.cap.style.bottom = b; f.fill.style.height = b; }, destroy: off };
    },
  },
  meter: {
    build(m) {
      m.meter.classList.add('dy-zoned');
      const floor = cfg.meters.floorDb, z = db => (frac(db, floor) * 100).toFixed(1);
      m.bar.prepend(el('div', { class: 'sk-deco dy-zones', 'aria-hidden': 'true', style: `--q:${z(-40)}%; --g:${z(-6)}%` }));
      m.node.title = 'Too quiet below -40 dBFS, good up to -6, too loud above. Click to clear the peak hold.';
      return { paint() {} };
    },
  },
  button: { decorate(b, R) { b.prepend(el('span', { class: 'sk-deco dy-switch', 'aria-hidden': 'true' }, el('span', { class: 'dy-knob' }))); } },
  selector: { decorate(n) { n._skinSet = null; } },
  slot: { decorate() {} },
  browser: {
    layout: 'purpose',
    groups(items, ctx) {
      const of = it => {
        const g = ctx.categorize(it).id;
        if (g === 'eq' || (g === 'dyn' && /ess/i.test(it.name))) return 'vocals';
        return { dyn: 'peaks', master: 'finish', verb: 'space', delay: 'space', pre: 'warmth', tape: 'warmth', amp: 'guitar', mod: 'motion' }[g] || 'other';
      };
      return DAY_PURPOSES.map(([id, name, sub]) => ({ id, name, sub, items: items.filter(it => of(it) === id) })).filter(g => g.items.length);
    },
  },
  params: { layout: 'cards' },
  /* [Sess] the session list's layout (sessions.js): plain words, big targets */
  sessions: { layout: 'plain' },
});
