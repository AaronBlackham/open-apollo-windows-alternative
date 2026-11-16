'use strict';
/*
 * [Out] Output and front-panel settings (driver/monitor_plan.h APOLLO_MP_* [Out]; host names in app/src/monitor_cmds.h):
 *   linepad1..4  x8p line pairs 1-2 .. 7-8 reference level: +4 dBu (0) or -10 dBV (1)
 *   monref       the monitor outputs' reference level, both units
 *   digmirror    x8p: the monitor mirrored to the S/PDIF output
 *   fcn          x8p function switch: none 0, mono 2, dim 3, talkback 4
 *   cliphold     front-panel meter clip hold: none, 1, 3, 5, 10 s, infinite (0-5)
 *   peakhold     front-panel meter peak hold: none, 1, 3, 5, 10 s (0-4)
 *   aux1mono, aux2mono   the aux bus summed to mono
 *   identify     the front panel's identify states (UA's engine runs 3, 1, 2, 0); a momentary blink, never saved
 * Nothing reads back: the values live with the unit's mix (cfg.mixes[...].out) and go out again once per connection
 * (app.js savedMixOut -> applyOut). Untouched = left as the unit has it (UA defaults: +4 dBu, mirror off, FCN none,
 * holds 3 s, aux stereo).
 * Loaded before app.js; uses its globals (el, sw, seg, setPressed, setHidden, put, isInt, mixOf, saveMix, hwSend,
 * mixWritable, noteUndo, ctl, capsOf, unitType, unitShort, isRefused, S, toast) only inside functions.
 */
const Outs = (() => {
  /* per model (driver/monitor_plan.h ApolloMonModelFor [Out]); the x8p is type 0x22, the Twin X 0x99 */
  const CAPS = {
    0x22: { pads: ['Line 1-2', 'Line 3-4', 'Line 5-6', 'Line 7-8'], monRef: true, digMirror: true, fcn: true, meters: true, aux: true },
    0x99: { pads: [], monRef: true, digMirror: false, fcn: false, meters: true, aux: true },
  };
  const NONE = { pads: [], monRef: false, digMirror: false, fcn: false, meters: false, aux: false };
  const REFS = ['+4 dBu', '-10 dBV'];
  const FCN = [[0, 'None'], [2, 'Mono'], [3, 'Dim'], [4, 'Talkback']];
  const CLIP = ['None', '1 s', '3 s', '5 s', '10 s', 'Infinite'];
  const PEAK = ['None', '1 s', '3 s', '5 s', '10 s'];
  const WRITE = /^(linepad[1-4]|monref|digmirror|fcn|cliphold|peakhold|aux[12]mono)$/;
  const capsFor = idx => CAPS[unitType(idx)] || NONE;

  /* ---------------------------------------------------------- the stored values */

  function clean(src) {
    const o = {};
    if (!src || typeof src !== 'object') return o;
    if (Array.isArray(src.pads)) o.pads = src.pads.slice(0, 4).map(v => (typeof v === 'boolean' ? v : undefined));
    for (const f of ['monRef', 'digMirror']) if (typeof src[f] === 'boolean') o[f] = src[f];
    if (FCN.some(([v]) => v === src.fcn)) o.fcn = src.fcn;
    if (isInt(src.clip, 0, 5)) o.clip = src.clip;
    if (isInt(src.peak, 0, 4)) o.peak = src.peak;
    if (Array.isArray(src.auxMono)) o.auxMono = src.auxMono.slice(0, 2).map(v => (typeof v === 'boolean' ? v : undefined));
    return o;
  }
  const outOf = idx => { const m = mixOf(idx); return m.out || (m.out = {}); };

  /* name -> [read, write] of the stored value (write(undefined) forgets it) */
  function field(idx, name) {
    const o = outOf(idx);
    const arr = (key, i) => [() => (o[key] || [])[i], v => { const a = o[key] || (o[key] = []); a[i] = v; }];
    let m = /^linepad([1-4])$/.exec(name);
    if (m) return arr('pads', m[1] - 1);
    m = /^aux([12])mono$/.exec(name);
    if (m) return arr('auxMono', m[1] - 1);
    const key = { monref: 'monRef', digmirror: 'digMirror', fcn: 'fcn', cliphold: 'clip', peakhold: 'peak' }[name];
    return [() => o[key], v => { if (v === undefined) delete o[key]; else o[key] = v; }];
  }
  const wire = v => (typeof v === 'boolean' ? (v ? 1 : 0) : v);

  /* what goes out once per connection (after the ALT outputs): only what was set here, only what the model has */
  function applyOut(idx, out) {
    const k = capsFor(idx), o = outOf(idx);
    k.pads.forEach((_, i) => { const v = (o.pads || [])[i]; if (v !== undefined) out.push([`linepad${i + 1}`, v ? 1 : 0]); });
    if (k.monRef && o.monRef !== undefined) out.push(['monref', o.monRef ? 1 : 0]);
    if (k.digMirror && o.digMirror !== undefined) out.push(['digmirror', o.digMirror ? 1 : 0]);
    if (k.fcn && o.fcn !== undefined) out.push(['fcn', o.fcn]);
    if (k.meters && o.clip !== undefined) out.push(['cliphold', o.clip]);
    if (k.meters && o.peak !== undefined) out.push(['peakhold', o.peak]);
    if (k.aux) [0, 1].forEach(a => { const v = (o.auxMono || [])[a]; if (v !== undefined) out.push([`aux${a + 1}mono`, v ? 1 : 0]); });
  }

  /* one setting from the card: kept, sent, the card redrawn */
  function set(name, value, idx = S.sel) {
    const [get, put] = field(idx, name), c = ctl(idx), key = `out:${name}`;
    if (get() === value) return;
    if (!mixWritable(idx, key)) return;
    noteUndo(c, key, get());
    put(value);
    saveMix(idx);
    hwSend(idx, 'monitor', name, wire(value));
    render();
  }

  /* a refused write (app.js restoreWrite): back to the value before the run of writes */
  function restore(idx, name, before) {
    const prev = before && typeof before === 'object' ? before.prev : undefined;
    field(idx, name)[1](prev);
    saveMix(idx);
    render();
  }

  const NAMES = { monref: 'monitor output level', digmirror: 'monitor to S/PDIF', fcn: 'function switch',
    cliphold: 'clip hold', peakhold: 'peak hold' };
  function name(key, idx) {
    const unit = idx ? `${unitShort(idx)} ` : '';
    let m = /^linepad([1-4])$/.exec(key);
    if (m) return `${unit}${(CAPS[0x22].pads[m[1] - 1])} output level`;
    m = /^aux([12])mono$/.exec(key);
    if (m) return `${unit}AUX ${m[1]} mono`;
    return `${unit}${NAMES[key] || key}`;
  }

  /* ---------------------------------------------------------- identify */

  let blinking = 0;
  /* The front panel flashes for about four seconds: UA's engine steps the states 3, 1, 2, 0; here 1 and 2 alternate. */
  function identify(idx = S.sel) {
    if (blinking || !mixWritable(idx, 'out:identify')) return;
    const steps = [1, 2, 1, 2, 1, 2, 1, 2, 0];
    blinking = 1;
    render();
    steps.forEach((v, i) => setTimeout(() => {
      hwSend(idx, 'monitor', 'identify', v);
      if (v === 0) { blinking = 0; render(); }
    }, i * 450));
  }

  /* ---------------------------------------------------------- the card (monitor column) */

  const ui = {};
  function card() {
    ui.pads = el('div', { class: 'out-pads' });
    ui.monRef = null;
    ui.mirror = sw('MON TO S/PDIF', { title: 'Mirror the monitor mix to the S/PDIF output', onClick: () => set('digmirror', !outOf(S.sel).digMirror) });
    ui.fcn = el('select', { class: 'select', id: 'outFcn', onchange: e => set('fcn', Number(e.target.value)) },
      FCN.map(([v, t]) => el('option', { value: String(v), text: t })));
    ui.fcnField = el('div', { class: 'field' }, el('label', { for: 'outFcn', text: 'Function switch' }), ui.fcn);
    ui.clip = el('select', { class: 'select', id: 'outClip', onchange: e => set('cliphold', Number(e.target.value)) },
      CLIP.map((t, i) => el('option', { value: String(i), text: t })));
    ui.peak = el('select', { class: 'select', id: 'outPeak', onchange: e => set('peakhold', Number(e.target.value)) },
      PEAK.map((t, i) => el('option', { value: String(i), text: t })));
    ui.meters = el('div', {},
      el('div', { class: 'field' }, el('label', { for: 'outClip', text: 'Meter clip hold' }), ui.clip),
      el('div', { class: 'field' }, el('label', { for: 'outPeak', text: 'Meter peak hold' }), ui.peak));
    ui.aux = [0, 1].map(a => sw(`AUX ${a + 1} MONO`, { title: `Sum AUX ${a + 1} to mono`, onClick: () => set(`aux${a + 1}mono`, !(outOf(S.sel).auxMono || [])[a]) }));
    ui.auxRow = el('div', { class: 'out-row' }, ...ui.aux);
    ui.ident = el('button', { class: 'btn btn-quiet', title: 'Flash this unit\'s front panel', onclick: () => identify() }, 'Identify');
    ui.card = el('div', { class: 'mon-card mon-out' },
      el('div', { class: 'sect-title', text: 'Outputs' }),
      ui.pads, ui.mirror, ui.fcnField, ui.meters, ui.auxRow, ui.ident,
      el('span', { class: 'hp-note', text: 'Kept here and sent again when the unit connects: the unit does not report them.' }));
    ui.built = -1;
    return ui.card;
  }

  /* the reference-level rows follow the focused model */
  function build(idx) {
    const k = capsFor(idx);
    const rows = [];
    ui.refs = [];
    const add = (label, name) => {
      const s = seg(REFS, i => set(name, i === 1), `${label} reference level`);
      ui.refs.push({ s, name });
      rows.push(el('div', { class: 'out-ref' }, el('span', { class: 'lbl', text: label }), s.node));
    };
    if (k.monRef) add('Monitor', 'monref');
    k.pads.forEach((p, i) => add(p, `linepad${i + 1}`));
    ui.pads.replaceChildren(...rows);
    ui.built = unitType(idx);
  }

  function render() {
    if (!ui.card) return;
    const idx = S.sel, k = capsFor(idx), o = outOf(idx);
    if (ui.built !== unitType(idx)) build(idx);
    setHidden(ui.card, !(k.monRef || k.pads.length || k.digMirror || k.fcn || k.meters || k.aux));
    for (const r of ui.refs) {
      const v = field(idx, r.name)[0]();
      r.s.set(v ? 1 : 0);                 /* untouched shows UA's default, +4 dBu */
      r.s.node.classList.toggle('refused', isRefused(`out:${r.name}`));
    }
    setHidden(ui.mirror, !k.digMirror);
    setPressed(ui.mirror, !!o.digMirror);
    ui.mirror.classList.toggle('refused', isRefused('out:digmirror'));
    setHidden(ui.fcnField, !k.fcn);
    ui.fcn.value = String(o.fcn ?? 0);
    setHidden(ui.meters, !k.meters);
    ui.clip.value = String(o.clip ?? 2);
    ui.peak.value = String(o.peak ?? 2);
    setHidden(ui.auxRow, !k.aux);
    ui.aux.forEach((b, a) => { setPressed(b, !!(o.auxMono || [])[a]); b.classList.toggle('refused', isRefused(`out:aux${a + 1}mono`)); });
    ui.ident.disabled = !!blinking;
    put(ui.ident, blinking ? 'Identifying…' : 'Identify');
  }

  return { clean, applyOut, set, restore, name, identify, card, render, isWrite: key => WRITE.test(key || ''), CAPS };
})();
