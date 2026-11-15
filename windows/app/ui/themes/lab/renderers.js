'use strict';
/* Lab: the analyzer (docs/THEMES.md 3). Encoders as numeric fields with a small arc indicator (click the number to type,
   arrows step, Page Up/Down ten steps, the wheel, a drag on the arc); precision sliders with a major / minor graticule and
   numeric entry (click the readout); square soft-keys with a status lamp; selectors as list boxes; bar-graph meters with
   peak and average markers; plug-ins as a sortable catalog table, the parameter view as a table. Instant: no motion. */
Skin.register('lab', {
  knob: {
    build(k, R) {
      const S = R.svg, g = S('svg', { class: 'sk-lab lb-arc', viewBox: '0 0 24 24', width: 18, height: 18, 'aria-hidden': 'true' });
      const val = S('path', { class: 'lb-arc-v' });
      g.append(S('path', { class: 'lb-arc-t', d: R.arc(12, 12, 9, -135, 135) }), val);
      if (k.o.label) k.node.append(R.deco('div', 'k-label', k.o.label));
      k.node.append(el('div', { class: 'lb-field sk-lab-knob' }, g, k.readout, R.deco('span', 'lb-spin')));
      const off1 = R.vertical(k, g, { travel: 160 }), off2 = R.field(k, k.readout);
      return {
        draw({ p, p0 }) {
          const a = -135 + p * 270, b = -135 + p0 * 270;
          val.setAttribute('d', Math.abs(p - p0) > 0.002 ? R.arc(12, 12, 9, Math.min(a, b), Math.max(a, b)) : '');
        },
        destroy() { off1(); off2(); },
      };
    },
  },
  fader: {
    build(f, R) {
      /* the graticule: major ticks labelled, minor every 2 dB above -20, every 10 dB below */
      const minor = [];
      for (let db = 12; db >= -20; db -= 2) minor.push(db);
      for (let db = -30; db >= -60; db -= 10) minor.push(db);
      const grat = el('div', { class: 'sk-deco lb-grat', 'aria-hidden': 'true' },
        minor.map(db => el('span', { class: [12, 6, 0, -6, -12, -20, -30, -40, -60].includes(db) ? 'maj' : 'min', style: `bottom:${(f.o.toPos(db * 2) * 100).toFixed(1)}%` })));
      f.cap = el('div', { class: 'fd-cap lb-ptr' });
      f.track = el('div', { class: 'fd-track lb-slide sk-lab' }, grat, f.cap);
      f.node.append(f.scaleNode(), f.track);
      const off = f.trackDrag(f.track);
      const type = () => f.edit();
      f.readout.addEventListener('click', type);
      return { draw({ p }) { f.cap.style.bottom = `${(p * 100).toFixed(2)}%`; }, destroy() { off(); f.readout.removeEventListener('click', type); } };
    },
  },
  meter: {
    build(m, R) {
      m.meter.classList.add('lb-graph');
      const rms = R.deco('div', 'lb-rms');
      m.bar.append(rms);
      return { paint(f, h, g, a) { rms.style.bottom = `${(a * 100).toFixed(2)}%`; rms.style.opacity = a > 0.004 ? '1' : '0'; } };
    },
  },
  button: { decorate(b, R) { b.prepend(R.deco('span', 'lb-led')); } },
  selector: { decorate(n) { n._skinSet = null; } },
  slot: { decorate() {} },
  browser: {
    layout: 'table',
    flat: true,
    columns: [['name', 'Name'], ['type', 'Type'], ['code', 'Code w'], ['lat', 'Lat'], ['par', 'Par'], ['lic', 'Lic']],
    item(node, it, ctx) {
      const c = (cls, t) => el('span', { class: `sk-deco lb-c lb-${cls}`, 'aria-hidden': 'true', text: t });
      node.append(c('type', ctx.group), c('code', String(ctx.need)), c('lat', String(it.latency || 0)), c('par', String(it.params ?? '')),
        c('lic', it.licensed === true ? 'yes' : it.licensed === false ? 'no' : '?'));
    },
  },
  params: { layout: 'table' },
  /* [Sess] the session list's layout (sessions.js): a sortable table with soft-keys and a status line */
  sessions: { layout: 'table' },
});
