'use strict';
/*
 * [Ins] The aux buses (docs/apollo-control-units.md 1.9.6, 1.9.7): AUX 1 and AUX 2 per unit, stereo.
 *   per input  a send into each aux (auxsend:<a>:<bus>) and its pan (auxpan), the two AUX knobs on every strip;
 *   per aux    PRE / POST (auxpost:<a>:<bus> for every input), the return into the monitor mix (auxret:<a>, the aux
 *              strip's fader), MUTE (auxmute:<a>), its sends into the cues (auxcue:<a>:<c>, auxcuepan) and four insert
 *              slots (plugins.js, target aux).
 * Nothing reads back: the values live with the unit's mix (cfg.mixes[x8p | unit id].aux) and go out again once per
 * connection (app.js applySavedMix -> applyOut). Each aux's return and mute always go out then: until a return level
 * is sent the unit keeps its own default, which leaves the aux returns silent.
 * Loaded before app.js; uses its globals (mixOf, saveMix, hwSend, throttled, mixWritable, Knob, Fader, seg, mixSwitch,
 * capsOf, unitType, unitShort, mixBuses, isInt, SEND_OFF, SEND_MAX, ...) only inside functions.
 */
const Aux = (() => {
  const AUXES = 2;
  const W = 66;                                           /* an aux strip's width, for the segment's minimum */

  /* ---------------------------------------------------------- the stored values */

  const auxOf = (idx, a) => {
    const m = mixOf(idx);
    if (!Array.isArray(m.aux)) m.aux = [{}, {}];
    return m.aux[a] || (m.aux[a] = {});
  };
  const retOf = (idx, a) => auxOf(idx, a).ret ?? 0;
  const muteOf = (idx, a) => !!auxOf(idx, a).mute;
  const postOf = (idx, a) => !!auxOf(idx, a).post;
  const sendOf = (idx, a, bus) => { const s = (auxOf(idx, a).sends || {})[bus] || {}; return { level: s.level ?? SEND_OFF, pan: s.pan ?? 0 }; };
  const cueOf = (idx, a, c) => { const s = (auxOf(idx, a).cues || {})[c] || {}; return { level: s.level ?? SEND_OFF, pan: s.pan ?? 0 }; };
  const cueCount = idx => (capsOf(unitType(idx)).cues || []).length;

  /* a saved mix's aux part as loaded: only well-formed values survive (app.js cleanMix) */
  function clean(src) {
    const out = [{}, {}];
    (Array.isArray(src) ? src : []).slice(0, AUXES).forEach((x, a) => {
      if (!x || typeof x !== 'object') return;
      const o = out[a];
      if (isInt(x.ret, SEND_OFF, SEND_MAX)) o.ret = x.ret;
      if (typeof x.mute === 'boolean') o.mute = x.mute;
      if (typeof x.post === 'boolean') o.post = x.post;
      for (const [field, max] of [['sends', 31], ['cues', 3]]) {
        if (!x[field] || typeof x[field] !== 'object') continue;
        for (const [k, s] of Object.entries(x[field])) {
          const n = Number(k);
          if (!isInt(n, 0, max) || !s || typeof s !== 'object') continue;
          (o[field] || (o[field] = {}))[n] = { level: isInt(s.level, SEND_OFF, SEND_MAX) ? s.level : SEND_OFF, pan: isInt(s.pan, -100, 100) ? s.pan : 0 };
        }
      }
    });
    return out;
  }

  /* what goes out once per connection, after the monitor mix: mutes and PRE / POST, then pans, sends and the returns */
  function applyOut(idx, out) {
    const k = capsOf(unitType(idx));
    if (!k.preamps) return;
    const buses = mixBuses(k), cues = cueCount(idx);
    for (let a = 0; a < AUXES; a++) out.push([`auxmute:${a + 1}`, muteOf(idx, a) ? 1 : 0]);
    for (let a = 0; a < AUXES; a++) {
      if (auxOf(idx, a).post !== undefined) for (const b of buses) out.push([`auxpost:${a + 1}:${b}`, postOf(idx, a) ? 1 : 0]);
      for (const [bus, s] of Object.entries(auxOf(idx, a).sends || {})) {
        if (!buses.includes(Number(bus))) continue;
        if (s.pan) out.push([`auxpan:${a + 1}:${bus}`, s.pan]);
        out.push([`auxsend:${a + 1}:${bus}`, s.level]);
      }
      for (const [c, s] of Object.entries(auxOf(idx, a).cues || {})) {
        if (Number(c) >= cues) continue;
        if (s.pan) out.push([`auxcuepan:${a + 1}:${Number(c) + 1}`, s.pan]);
        out.push([`auxcue:${a + 1}:${Number(c) + 1}`, s.level]);
      }
    }
    for (let a = 0; a < AUXES; a++) out.push([`auxret:${a + 1}`, retOf(idx, a)]);
  }

  /* ---------------------------------------------------------- setting */

  function write(idx, name, value, level) {
    if (!mixWritable(idx, `aux:${name}`)) { render(idx); return false; }
    saveMix(idx);
    if (level) throttled(`aux:${idx}:${name}`, () => hwSend(idx, 'monitor', name, value()));
    else hwSend(idx, 'monitor', name, value());
    render(idx);
    return true;
  }

  function setSend(idx, a, bus, field, v) {
    const x = auxOf(idx, a), all = x.sends || (x.sends = {}), s = all[bus] || (all[bus] = { level: SEND_OFF, pan: 0 });
    if (s[field] === v) return;
    s[field] = v;
    write(idx, `${field === 'level' ? 'auxsend' : 'auxpan'}:${a + 1}:${bus}`, () => sendOf(idx, a, bus)[field], true);
  }

  function setCue(idx, a, c, field, v) {
    const x = auxOf(idx, a), all = x.cues || (x.cues = {}), s = all[c] || (all[c] = { level: SEND_OFF, pan: 0 });
    if (s[field] === v) return;
    s[field] = v;
    write(idx, `${field === 'level' ? 'auxcue' : 'auxcuepan'}:${a + 1}:${c + 1}`, () => cueOf(idx, a, c)[field], true);
  }

  function setRet(idx, a, v) {
    if (retOf(idx, a) === v && auxOf(idx, a).ret !== undefined) return;
    auxOf(idx, a).ret = v;
    write(idx, `auxret:${a + 1}`, () => retOf(idx, a), true);
  }

  function setMute(idx, a, on) {
    auxOf(idx, a).mute = !!on;
    write(idx, `auxmute:${a + 1}`, () => (muteOf(idx, a) ? 1 : 0), false);
  }

  /* the aux strip's PRE / POST: Console's per-send PostFaderOverride, sent for every input (and the talkback) */
  function setPost(idx, a, post) {
    auxOf(idx, a).post = !!post;
    if (!mixWritable(idx, `aux:auxpost:${a + 1}`)) { render(idx); return; }
    saveMix(idx);
    for (const b of mixBuses(capsOf(unitType(idx)))) hwSend(idx, 'monitor', `auxpost:${a + 1}:${b}`, post ? 1 : 0);
    render(idx);
  }

  /* ---------------------------------------------------------- the views */

  const views = new Map();                                 /* unit slot -> { sends: [...], strips: [...] } */
  const viewOf = idx => views.get(idx) || (views.set(idx, { sends: [], strips: [] }), views.get(idx));
  const sendKnob = (aria, label, onChange) => new Knob({
    size: 22, min: SEND_OFF, max: SEND_MAX, step: 1, wheelStep: 2, pageStep: 12, value: SEND_OFF, label,
    aria, toPos: sendToPos, fromPos: sendFromPos, format: v => (v <= SEND_OFF ? 'off' : (v / 2).toFixed(1)),
    editText: v => (v <= SEND_OFF ? 'off' : (v / 2).toFixed(1)), parse: parseSend, onChange,
  });
  const panKnob = (aria, onChange) => new Knob({
    size: 18, min: -100, max: 100, step: 1, wheelStep: 5, pageStep: 25, value: 0, origin: 0.5,
    aria, format: fmtPan, editText: fmtPan, parse: parsePan, onChange,
  });

  /* an input strip's AUX row: a send into AUX 1 and AUX 2, and their pans (the wide layouts show them) */
  function stripSends(idx, ch, who) {
    const row = { idx, ch, level: [], pan: [] };
    const cells = [];
    for (let a = 0; a < AUXES; a++) {
      const lv = sendKnob(`${who} send to AUX ${a + 1}`, `A${a + 1}`, v => setSend(idx, a, ch, 'level', v));
      const pn = panKnob(`${who} AUX ${a + 1} pan`, v => setSend(idx, a, ch, 'pan', v));
      row.level.push(lv);
      row.pan.push(pn);
      cells.push(el('div', { class: 'aux-send', dataset: { aux: String(a + 1) } }, lv.node, el('div', { class: 'aux-pan' }, pn.node)));
    }
    row.node = el('div', { class: 'aux-sends', role: 'group', 'aria-label': `${who} aux sends` }, cells);
    viewOf(idx).sends.push(row);
    return row.node;
  }

  /* a unit's Aux section: AUX 1 and AUX 2 strips (inserts, cue sends, PRE / POST, MUTE, the return fader) */
  function section(idx) {
    const short = unitShort(idx) || 'unit', v = viewOf(idx);
    v.strips = [];
    const nodes = [];
    for (let a = 0; a < AUXES; a++) {
      const name = `${idx ? `${short} ` : ''}AUX ${a + 1}`;
      const post = seg(['PRE', 'POST'], i => setPost(idx, a, i === 1), `${name} sends: pre or post fader`);
      const mute = mixSwitch('MUTE', `${name} return mute`, true, () => setMute(idx, a, !muteOf(idx, a)));
      const fader = new Fader({ aria: `${name} return level`, onChange: x => setRet(idx, a, x) });
      const cues = Array.from({ length: cueCount(idx) }, (_, c) => sendKnob(`${name} into CUE ${c + 1}`, `C${c + 1}`, x => setCue(idx, a, c, 'level', x)));
      const s = { a, post, mute, fader, cues };
      s.node = el('div', { class: 'aux-strip', role: 'group', 'aria-label': name, dataset: { aux: String(a), unit: String(idx) } },
        el('div', { class: 'strip-top' }, el('div', { class: 'strip-num num', text: `AUX ${a + 1}` }), el('span', { class: 'tag aux-tag', text: 'ST' })),
        el('div', { class: 'st-sec st-ins aux-ins' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'INS' }), Plug.slotNodesFor(idx, 100 + a)),
        el('div', { class: 'st-sec aux-cues' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'TO CUES' }),
          el('div', { class: 'aux-cue-grid' }, cues.map(k => k.node))),
        el('div', { class: 'st-sec aux-ctl' }, post.node, mute),
        el('div', { class: 'st-sec st-mix aux-ret' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'RETURN' }),
          el('div', { class: 'strip-meter' }, el('div', { class: 'fd-row' }, fader.node), el('div', { class: 'fd-foot' }, fader.readout))));
      v.strips.push(s);
      nodes.push(s.node);
    }
    render(idx);
    return el('div', { class: 'aux-sec', dataset: { unit: String(idx) } }, nodes);
  }

  function render(idx) {
    const v = views.get(idx);
    if (!v) return;
    for (const r of v.sends) {
      for (let a = 0; a < AUXES; a++) {
        const s = sendOf(idx, a, r.ch);
        r.level[a].set(s.level);
        r.pan[a].set(s.pan);
        r.level[a].node.classList.toggle('off', s.level <= SEND_OFF);
      }
    }
    for (const s of v.strips) {
      s.post.set(postOf(idx, s.a) ? 1 : 0);
      setChecked(s.mute, muteOf(idx, s.a));
      s.fader.set(retOf(idx, s.a));
      s.node.classList.toggle('silent', muteOf(idx, s.a));
      s.cues.forEach((k, c) => k.set(cueOf(idx, s.a, c).level));
    }
  }

  /* a segment rebuilt or dropped: its views go with it */
  function forget(idx) { views.delete(idx); }

  return { AUXES, W, clean, applyOut, stripSends, section, render, forget, setSend, setRet, setMute, setPost, setCue,
    sendOf, retOf, muteOf, postOf, cueOf };
})();
