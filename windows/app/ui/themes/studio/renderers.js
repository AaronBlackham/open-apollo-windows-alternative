'use strict';
/* Studio: the console (docs/THEMES.md 3). Machined knurled knobs with a pointer line and a printed scale (vertical drag,
   Shift fine, soft detents at unity and centre, double-click resets); long-throw faders with a ribbed cap in a groove and a
   printed dB legend (the cap glides; a press on the legend jumps there); backlit latching switches; rotary source
   selectors; LED-ladder meters; the plug-in slot is a patch point, the browser an outboard rack of faceplates. */
Skin.register('studio', {
  knob: {
    build(k, R) {
      const S = R.svg, sz = k.o.size;
      const g = S('svg', { class: 'sk-studio sk-studio-knob', width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true' });
      for (let i = 0; i <= 10; i++) {
        const a = -135 + i * 27, big = i % 5 === 0;
        const [x0, y0] = R.polar(50, 50, big ? 39 : 42, a), [x1, y1] = R.polar(50, 50, 48, a);
        g.append(S('line', { class: `st-tick${big ? ' big' : ''}`, x1: x0, y1: y0, x2: x1, y2: y1 }));
      }
      const rot = S('g', {});
      rot.append(S('line', { class: 'st-ptr', x1: 50, y1: 44, x2: 50, y2: 23 }));
      g.append(S('circle', { class: 'st-knurl', cx: 50, cy: 50, r: 34 }), S('circle', { class: 'st-cap', cx: 50, cy: 50, r: 27 }), rot);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(g, k.readout);
      const det = [];
      if (k.o.origin) det.push(k.o.origin);
      if (k.o.reset !== undefined) det.push(clamp(k.o.toPos(k.o.reset), 0, 1));
      const off = R.vertical(k, g, { detents: det });
      return { draw({ p }) { rot.setAttribute('transform', `rotate(${(-135 + p * 270).toFixed(1)} 50 50)`); }, destroy: off };
    },
  },
  fader: {
    build(f, R) {
      f.cap = el('div', { class: 'fd-cap st-fcap' }, R.deco('span', 'st-rib'));
      f.track = el('div', { class: 'fd-track st-groove sk-studio' }, f.cap);
      const scale = f.scaleNode();
      scale.classList.add('st-legend');
      const jump = e => {
        if (e.button !== 0) return;
        const r0 = f.track.getBoundingClientRect();
        f.node.focus();
        f.commitPos(clamp((r0.bottom - e.clientY) / Math.max(1, r0.height), 0, 1));
      };
      scale.addEventListener('pointerdown', jump);
      f.node.append(scale, f.track);
      const off = f.trackDrag(f.track);
      return { draw({ p }) { f.cap.style.bottom = `${(p * 100).toFixed(2)}%`; }, destroy() { off(); scale.removeEventListener('pointerdown', jump); } };
    },
  },
  meter: { build(m) { m.meter.classList.add('st-ladder'); return { paint() {} }; } },
  button: { decorate(b, R) { b.prepend(R.deco('span', 'st-lamp')); } },
  selector: {
    decorate(n, R) {
      n._skinSet = null;
      if (n.querySelectorAll(':scope > button').length < 3) return;
      const S = R.svg, g = S('svg', { class: 'sk-deco st-rot', viewBox: '0 0 20 20', 'aria-hidden': 'true' });
      const ptr = S('line', { x1: 10, y1: 10, x2: 10, y2: 3 });
      g.append(S('circle', { cx: 10, cy: 10, r: 8 }), ptr);
      n.prepend(g);
      n._skinSet = () => {
        const bs = [...n.querySelectorAll(':scope > button')], i = Math.max(0, bs.findIndex(b => b.getAttribute('aria-pressed') === 'true'));
        ptr.setAttribute('transform', `rotate(${bs.length > 1 ? -60 + i / (bs.length - 1) * 120 : 0} 10 10)`);
      };
      n._skinSet();
    },
  },
  slot: { decorate(b, R) { b.prepend(R.deco('span', 'st-jack')); } },
  browser: { layout: 'rack' },
  params: { layout: 'faceplate' },
  on: {
    drop(node, R) { R.pulse(node, 'st-patched', 700); },
    refuse(node, R) { R.pulse(node, 'st-fault', 900); },
  },
  /* [Sess] the session list's layout (sessions.js): a console's session menu: the scribble strip, backlit keys, RECALL */
  sessions: { layout: 'console' },
});
