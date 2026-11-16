'use strict';
/* Classic: a standard digital mixer app (docs/THEMES.md 2). No metaphor on purpose: it is laid out and behaves the way
   console software usually does, so someone who has used a mixer app before finds everything where they expect it.
   Full-height channel strips (preamp, inserts, sends, pan, a long fader with its dB scale beside the meter, mute / solo,
   the channel name at the foot), each unit's master section on its right.
   Controls: flat rotary knobs with a value arc and a pointer (vertical drag, Shift fine, double-click resets, wheel);
   faders with a flat cap on a thin slot (drag the cap, press the slot or the scale to jump, double-click for 0 dB);
   rectangular buttons that light when on; solid green / yellow / red meters. Plug-ins in a plain list by type,
   parameters in a plain grid, sessions in plain words. Only short state transitions, nothing else moves. */
Skin.register('classic', {
  knob: {
    build(k, R) {
      /* never under 24 px: the pan and send knobs (22 px elsewhere) are drawn at 24 so they stay a full target */
      const S = R.svg, sz = Math.max(24, k.o.size), small = sz <= 30;
      /* small knobs (pan, sends) get thicker strokes so the arc and the pointer stay readable at 18-22 px */
      const sw = small ? 11 : 8, rArc = small ? 43 : 44, rBody = small ? 27 : 32;
      const g = S('svg', { class: 'sk-classic cl-knob', width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true' });
      const track = S('path', { class: 'cl-k-track', 'stroke-width': sw, d: R.arc(50, 50, rArc, -135, 135) });
      const val = S('path', { class: 'cl-k-val', 'stroke-width': sw });
      const body = S('circle', { class: 'cl-k-body', cx: 50, cy: 50, r: rBody });
      const ptr = S('line', { class: 'cl-k-ptr', 'stroke-width': small ? 10 : 7 });
      g.append(track, val, body, ptr);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(g, k.readout);
      const off = R.vertical(k, g);
      return {
        draw({ p, p0 }) {
          const a = -135 + p * 270, b = -135 + p0 * 270;
          val.setAttribute('d', Math.abs(p - p0) > 0.002 ? R.arc(50, 50, rArc, Math.min(a, b), Math.max(a, b)) : '');
          const [x0, y0] = R.polar(50, 50, small ? 4 : 10, a), [x1, y1] = R.polar(50, 50, rBody - 2, a);
          ptr.setAttribute('x1', x0.toFixed(2)); ptr.setAttribute('y1', y0.toFixed(2));
          ptr.setAttribute('x2', x1.toFixed(2)); ptr.setAttribute('y2', y1.toFixed(2));
        },
        destroy: off,
      };
    },
  },
  fader: {
    build(f) {
      f.cap = el('div', { class: 'fd-cap cl-cap' });
      f.track = el('div', { class: 'fd-track cl-track sk-classic' }, f.cap);
      const scale = f.scaleNode();
      scale.classList.add('cl-scale');
      /* a press on the scale moves the fader there (the slot itself jumps too: Fader.trackDrag) */
      const jump = e => {
        if (e.button !== 0) return;
        const r0 = f.track.getBoundingClientRect();
        f.node.focus();
        f.commitPos(clamp((r0.bottom - e.clientY) / Math.max(1, r0.height), 0, 1));
      };
      scale.addEventListener('pointerdown', jump);
      f.node.append(scale, f.track);
      const off = f.trackDrag(f.track);
      return {
        draw({ p }) { f.cap.style.bottom = `${(p * 100).toFixed(2)}%`; },
        destroy() { off(); scale.removeEventListener('pointerdown', jump); },
      };
    },
  },
  /* solid bars on the shared fill (green to -18, yellow to -6, red above), a clip box on top, a hold line */
  meter: { build(m) { m.meter.classList.add('cl-meter'); return { paint() {} }; } },
  /* buttons, selectors and slots: the shared markup, styled in theme.css; nothing added */
  button: { decorate() {} },
  selector: { decorate(n) { n._skinSet = null; } },
  slot: { decorate() {} },
  browser: { layout: 'list' },
  params: { layout: 'grid' },
  on: {
    drop(node, R) { R.pulse(node, 'cl-dropped', 450); },
    refuse(node, R) { R.pulse(node, 'cl-refuse', 450); },
  },
  /* [Sess] the session list in plain words (sessions.js layout "plain"), at desktop density in theme.css */
  sessions: { layout: 'plain' },
});
