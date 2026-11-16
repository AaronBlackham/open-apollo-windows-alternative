'use strict';
/*
 * [Clock] The clock source, the digital I/O formats, the front panel's monitor link and the talkback note
 * (docs/apollo-control-units.md 1.10; host app/src/clock_host.h; driver/clock.c, driver/panel.c, driver/clock_plan.h).
 *
 * The host's "clock" (per unit, by id: what it runs on and was asked for, lock, the jacks, what the unit offers, what a
 * clock-set may ask for now and what is proven, the digital formats and their channel counts, the raw registers),
 * "clock-result" (pending, then ok / code / why), "panel" (the front panel and the driver-wide monitor link) and
 * "panel-link" come here from app.js onHostMessage. This file draws:
 *   - the Device view's "Clock and digital I/O" panel for the focused unit (#devClock; app.js renderDevice calls in),
 *   - a clock badge in each segment's head and the talkback strip's no-signal note (app.js renderSegHead calls in),
 *   - the Units & routing view's "Monitor link" panel (units.js render takes linkPanel()).
 * Proven on these units: Internal only. Every other source and every format write is Experimental: one confirmation,
 * then try:true. The host runs one change at a time, so changes queue here and go out one by one, each after the last
 * one's final result; the controls wait meanwhile (and while the unit reports "changing").
 * Saved only after an ok result: cfg.unitPrefs[id].clock = {source, hold}, .digital = {opticalIn, opticalOut, src}, for
 * every unit by its id (the x8p's too; unitPrefs(idx) still answers null for slot 0, whose names stay in cfg.channels).
 * The driver forgets them at every connect on purpose: after a unit arrives running in a "units" list, its next "clock"
 * is compared with them and what differs goes out again (try:true: the user confirmed once), formats first.
 * cfg.panelLink = {monitor, level, mute, dim, mono, talkDim} (monitor '' = off, the default) goes out once each time both
 * units are running (the link is off at every driver load) and whenever it is changed here.
 * The demo answers like a host (simulated, nothing saved); the monitor link is not in the demo.
 * Loaded before app.js; uses its globals (S, cfg, host, el, seg, sw, put, dialog, toast, toastOnce, now, kHz, hex,
 * setPressed, listedUnit, unitShort, unitIdOf, slotOfId, controlUnits, unitButtons, readOnlyMode, unitReadOnly, writable,
 * newUnitPrefs, saveConfig, rateFor, renderSegHeads, capsOf, MODEL_CAPS, RATES) only inside functions.
 */
const Clock = (() => {
  const SOURCES = ['internal', 'spdif', 'adat', 'wordclock'];
  const SRC = { internal: 'Internal', spdif: 'S/PDIF', adat: 'ADAT', wordclock: 'Word clock', peer: 'Thunderbolt peer' };
  const TAG = { internal: 'INT', spdif: 'S/PDIF', adat: 'ADAT', wordclock: 'WCLK', peer: 'PEER' };
  const DIG = {
    'optical-in': { key: 'opticalIn', label: 'Optical in', long: 'optical input' },
    'optical-out': { key: 'opticalOut', label: 'Optical out', long: 'optical output' },
    src: { key: 'src', label: 'SR Convert', long: 'SR Convert' },
  };
  const FMT = { adat: 'ADAT', spdif: 'S/PDIF' };
  const LINK = [
    ['level', 'Level', 'The knob unit\'s monitor knob moves the speakers\' level, in 0.5 dB steps'],
    ['mute', 'Mute', 'MUTE on the knob unit (or a press of its knob) mutes the speakers'],
    ['dim', 'Dim', 'DIM on the knob unit dims the speakers'],
    ['mono', 'Mono', 'MONO on the knob unit sums the speakers to mono'],
    ['talkDim', 'Dim on talkback', 'Talkback on either unit dims the speakers, and undims only what it dimmed'],
  ];
  const LINK_DEFAULT = { monitor: '', level: true, mute: true, dim: true, mono: false, talkDim: true };
  const WAIT_MS = 15000, ASK_MS = 2500;
  const TB_NOTE = 'talkback mic: no signal yet (its input strip is not built in this driver)';
  const isUid = id => /^[0-9A-F]{16}$/i.test(String(id || ''));
  const srcName = s => SRC[s] || String(s || '?');

  /* clock, panel: the last messages by unit id; present: id -> 'here' | 'live' (running, Mode 2) from the units list;
     asked: when clock-get went out; reapply: compare the saved prefs with the next clock; fell: the source a fall-back
     was told for; hold: the hold checkbox per unit (the next change); notes: the last final result's words per unit and
     area; pending: the change the host runs now; queue: the changes after it; linkKey: the running pair the link went
     out for; listT: when the last units list came (the head badge takes the newer of it and the clock message). */
  const fresh = mode => ({ mode, clock: {}, panel: {}, link: null, linkResult: null, present: {}, asked: {}, reapply: {}, fell: {},
    hold: {}, notes: {}, pending: null, queue: [], timer: 0, linkKey: '', listT: 0 });
  let P = fresh('');
  const D = { stamp: '', units: null, details: null };      /* the Device panel */
  const L = { node: null, stamp: '' };                      /* the monitor link panel */

  function reset(mode) {
    clearTimeout(P.timer);
    P = fresh(mode);
    D.stamp = '';
    L.stamp = '';
  }

  function post(o) {
    if (S.demo) { demoHost(o); return; }
    if (host) host.postMessage(JSON.stringify(o));      /* a string: the host reads strings only */
  }

  /* ---------------------------------------------------------------- units, prefs */

  const slotOf = id => slotOfId(id);
  const nameOf = id => { const i = slotOf(id); return i >= 0 ? unitShort(i) : 'unit'; };
  const running = () => (S.unitList || []).filter(u => u.present && u.active && !readOnlyMode(null, u));

  /* A unit's saved prefs by id (cfg.unitPrefs, the same entry the other units' names live in); made on the first save. */
  function prefsOf(id, make) {
    if (!id) return null;
    const p = cfg.unitPrefs[id];
    if (p || !make) return p || null;
    return (cfg.unitPrefs[id] = newUnitPrefs());
  }
  const hasPrefs = id => { const p = prefsOf(id, false); return !!(p && (p.clock || p.digital)); };
  function holdOf(id) {
    if (typeof P.hold[id] === 'boolean') return P.hold[id];
    const p = prefsOf(id, false);
    if (p && p.clock) return !!p.clock.hold;
    return !!(P.clock[id] && P.clock[id].hold);
  }

  /* app.js merge: a loaded unitPrefs entry keeps its well-formed clock and digital prefs. */
  function keepPrefs(out, p) {
    if (!out || !p || typeof p !== 'object') return out;
    const c = p.clock;
    if (c && typeof c === 'object' && SOURCES.includes(c.source)) out.clock = { source: c.source, hold: c.hold === true };
    const d = p.digital;
    if (d && typeof d === 'object' && !Array.isArray(d)) {
      const o = {};
      for (const k of ['opticalIn', 'opticalOut']) if (d[k] === 'adat' || d[k] === 'spdif') o[k] = d[k];
      if (typeof d.src === 'boolean') o.src = d.src;
      if (Object.keys(o).length) out.digital = o;
    }
    return out;
  }

  function linkCfg() {
    let l = cfg.panelLink;
    if (!l || typeof l !== 'object') l = cfg.panelLink = { ...LINK_DEFAULT };
    const out = { monitor: typeof l.monitor === 'string' ? l.monitor : '' };
    for (const [f] of LINK) out[f] = typeof l[f] === 'boolean' ? l[f] : LINK_DEFAULT[f];
    return out;
  }

  /* ---------------------------------------------------------------- changes: one at a time */

  const area = what => (what === 'source' ? 'source' : 'digital');
  const wireValue = j => (j.what === 'src' ? (j.value ? 'on' : 'off') : j.value);
  function label(what, value) {
    if (what === 'source') return `clock (${srcName(value)})`;
    if (what === 'src') return `SR Convert (${value === true || value === 'on' ? 'on' : 'off'})`;
    return `${(DIG[what] || { long: what }).long} format (${FMT[value] || value})`;
  }

  function request(id, what, value, o = {}) {
    if (!id) return;
    P.queue.push({ id, what, value, try: !!o.try, hold: !!o.hold, auto: !!o.auto, retried: !!o.retried });
    pump();
    renderAll();
  }

  function pump() {
    if (P.pending || !P.queue.length) return;
    const j = P.queue.shift();
    P.pending = { ...j, t: now(), why: '' };
    if (P.notes[j.id]) delete P.notes[j.id][area(j.what)];
    const msg = j.what === 'source' ? { cmd: 'clock-set', unit: j.id, source: j.value }
      : { cmd: 'digital-set', unit: j.id, what: j.what, value: j.value };
    if (j.try) msg.try = true;
    if (j.what === 'source' && j.value !== 'internal') msg.hold = j.hold;
    arm(P.pending);
    post(msg);
  }

  /* No final answer in time (a host without clock control, or one that went away): the controls come back. */
  function arm(p) {
    clearTimeout(P.timer);
    P.timer = setTimeout(() => {
      if (P.pending !== p) return;
      finish({ unit: p.id, what: p.what, value: wireValue(p), pending: false, ok: false, code: 'timeout', why: `No answer from the host in ${WAIT_MS / 1000} s.` });
    }, WAIT_MS);
  }

  function onResult(m) {
    const id = String(m.unit || ''), what = String(m.what || 'source');
    if (m.pending) {
      if (P.pending && P.pending.id === id && P.pending.what === what) P.pending.why = String(m.why || '');
      else if (!P.pending && id) { P.pending = { id, what, value: m.value, t: now(), why: String(m.why || ''), foreign: true }; arm(P.pending); }
      renderAll();
      return;
    }
    finish(m);
  }

  function finish(m) {
    const id = String(m.unit || ''), what = String(m.what || 'source'), why = String(m.why || '');
    const j = P.pending && P.pending.id === id && P.pending.what === what ? P.pending : null;
    if (j) { clearTimeout(P.timer); P.pending = null; }
    const notes = id ? (P.notes[id] || (P.notes[id] = {})) : {};
    if (m.ok) {
      notes[area(what)] = { kind: 'ok', text: why || (m.ms ? `Done (${m.ms} ms).` : 'Done.') };
      if (!S.demo && id) store(id, what, m.value, j);
    } else if (m.code === 'busy') {
      toastOnce(why || 'Another clock change is under way.', 'warn');
      if (j && j.auto && !j.retried) setTimeout(() => { if (P.clock[id]) request(id, j.what, j.value, { ...j, retried: true }); }, 2000);
    } else {
      notes[area(what)] = { kind: 'bad', text: why || `Not changed (${m.code || 'error'}).` };
      if (j && j.auto) toast(`${nameOf(id)}: the saved ${label(what, j.value)} was not set again. ${why}`, 'warn', 9000);
    }
    pump();
    renderAll();
  }

  /* An ok result: kept for the unit's next connect. */
  function store(id, what, value, j) {
    const p = prefsOf(id, true);
    if (what === 'source') {
      if (!SOURCES.includes(value)) return;
      p.clock = { source: value, hold: j ? !!j.hold : !!(P.clock[id] && P.clock[id].hold) };
    } else if (DIG[what]) {
      const v = what === 'src' ? value === 'on' || value === true : value;
      if (what !== 'src' && !FMT[v]) return;
      p.digital = { ...(p.digital || {}), [DIG[what].key]: v };
    } else return;
    saveConfig();
  }

  /* After the unit arrived: what the saved prefs say and its clock does not, formats first (an ADAT clock needs the port). */
  function reapply(id, c) {
    const p = prefsOf(id, false);
    if (!p) return;
    const d = c.digital || {}, has = d.has || [], dg = p.digital || {}, jobs = [];
    for (const what of ['optical-in', 'optical-out']) {
      const k = DIG[what].key;
      if (dg[k] && has.includes(what) && d[k] !== dg[k]) jobs.push({ what, value: dg[k] });
    }
    if (typeof dg.src === 'boolean' && has.includes('src') && typeof d.src === 'boolean' && d.src !== dg.src) jobs.push({ what: 'src', value: dg.src });
    const ck = p.clock;
    if (ck && (c.sources || []).includes(ck.source) && (ck.source !== c.wanted || (ck.source !== 'internal' && !!ck.hold !== !!c.hold))) {
      jobs.push({ what: 'source', value: ck.source, hold: !!ck.hold });
    }
    for (const j of jobs) {
      const proven = j.what === 'source' ? c.proven || [] : d.proven || [];
      P.queue.push({ id, ...j, try: !proven.includes(j.what === 'source' ? j.value : j.what), hold: !!j.hold, auto: true, retried: false });
    }
    if (jobs.length) pump();
  }

  /* ---------------------------------------------------------------- host messages */

  function onHost(m) {
    if (P.mode !== 'host') reset('host');
    if (m.type === 'clock') onClock(m);
    else if (m.type === 'clock-result') onResult(m);
    else if (m.type === 'panel') onPanel(m);
    else if (m.type === 'panel-link') { P.linkResult = m; if (!m.ok) toastOnce(`Monitor link: ${m.why || m.code || 'not changed'}`, 'warn'); renderAll(); }
  }

  function onClock(m) {
    const id = String(m.unit || '');
    if (!id) return;
    if (m.present === false) { delete P.clock[id]; delete P.fell[id]; renderAll(); return; }
    P.clock[id] = { ...m, t: now() };
    if (m.error) delete P.reapply[id];
    else {
      if (m.fellBack && P.fell[id] !== m.wanted) {
        P.fell[id] = m.wanted;
        toast(`${nameOf(id)}: ${srcName(m.wanted)} lost lock, back on Internal. The Device view has Try again.`, 'warn', 9000);
      } else if (!m.fellBack) delete P.fell[id];
      if (P.reapply[id]) { delete P.reapply[id]; reapply(id, m); }
    }
    renderAll();
  }

  function onPanel(m) {
    const id = String(m.unit || '');
    if (!id) return;
    if (m.present === false || m.error) delete P.panel[id];
    else P.panel[id] = m;
    if (m.link && typeof m.link === 'object') P.link = m.link;
    renderAll();
  }

  /* app.js setUnitList: a unit that appears asks for its clock and panel; one that starts running (Mode 2, active) gets
     its saved prefs compared with its next clock; the monitor link goes out when both units run. */
  function onUnits() {
    const mode = S.demo ? 'demo' : 'host';
    if (P.mode !== mode) reset(mode);
    P.listT = now();
    const here = (S.unitList || []).filter(u => u.present && u.id);
    const ids = new Set(here.map(u => u.id));
    for (const id of Object.keys(P.present)) if (!ids.has(id)) forget(id);
    for (const u of here) {
      const live = !!u.active && !readOnlyMode(null, u), was = P.present[u.id];
      P.present[u.id] = live ? 'live' : 'here';
      if (!was || (live && was !== 'live')) arrive(u, live);
    }
    sendLink(false);
    renderAll();
  }

  function arrive(u, live) {
    if (S.demo) { if (!P.clock[u.id]) demoState(u); return; }
    if (live && hasPrefs(u.id)) P.reapply[u.id] = true;
    ask(u.id);
  }

  function ask(id) {
    if (S.demo || !host || !id) return;
    P.asked[id] = now();
    post({ cmd: 'clock-get', unit: id });
    post({ cmd: 'panel-get', unit: id });
    setTimeout(() => { if (S.view === 'device') renderDev(); }, ASK_MS + 50);
  }

  function forget(id) {
    for (const k of ['present', 'clock', 'panel', 'reapply', 'fell', 'asked']) delete P[k][id];
    P.queue = P.queue.filter(j => j.id !== id);
  }

  /* panel-link: once per running pair (or when changed here: force). The monitor unit must be running and have a HW_UID. */
  function sendLink(force) {
    if (S.demo || !host) return;
    const l = linkCfg(), live = running();
    if (live.length < 2) {
      P.linkKey = '';
      if (force && !l.monitor && (S.unitList || []).some(u => u.present)) post({ cmd: 'panel-link', monitor: null });
      return;
    }
    const key = live.map(u => u.id).sort().join(',');
    if (!l.monitor) {
      if (force) post({ cmd: 'panel-link', monitor: null });
      P.linkKey = key;
      return;
    }
    if (!isUid(l.monitor) || !live.some(u => u.id === l.monitor)) { P.linkKey = ''; return; }
    if (!force && key === P.linkKey) return;
    P.linkKey = key;
    post({ cmd: 'panel-link', monitor: l.monitor, level: l.level, mute: l.mute, dim: l.dim, mono: l.mono, talkDim: l.talkDim });
  }

  function renderAll() {
    renderSegHeads();
    if (S.view === 'device') renderDev();
    renderLink();
  }

  /* ---------------------------------------------------------------- the segment head and the talkback strip */

  function head(idx, sg) {
    if (!sg || !sg.clock) return;
    if (!sg.clkBadge) {
      sg.clkBadge = el('span', { class: 'clk-badge num', hidden: true });
      sg.clock.after(sg.clkBadge);
    }
    const id = unitIdOf(idx), e = listedUnit(idx), c = id ? P.clock[id] : null;
    const sum = e && e.clock && typeof e.clock === 'object' ? e.clock : null;
    const full = c && !c.error ? c : null;
    const st = full && (!sum || (full.t || 0) >= P.listT) ? full : sum || full;
    const show = !!st && !sg.gone && !!st.source;
    sg.clkBadge.hidden = !show;
    if (show) {
      const fell = !!st.fellBack, locked = !!st.locked;
      put(sg.clkBadge, fell ? `${TAG[st.wanted] || st.wanted} LOST · INT` : `${TAG[st.source] || st.source} · ${locked ? 'LOCKED' : 'NO LOCK'}`, undefined,
        fell ? `Clock: ${srcName(st.wanted)} lost lock, back on Internal (Device view: Try again)` : `Clock source: ${srcName(st.source)}, ${locked ? 'locked' : 'not locked'}`);
      sg.clkBadge.dataset.state = fell ? 'warn' : locked ? 'ok' : 'bad';
    }
    if (sg.tb && sg.tb.node) {
      const p = id ? P.panel[id] : null, off = !sg.gone && !!(p && p.talkbackChannel && p.talkbackChannel.strip === false);
      let n = sg.tb.node.querySelector('.tb-nosig');
      if (off && !n) {
        n = el('div', { class: 'tb-nosig', title: `The ${TB_NOTE}` },
          el('span', { 'aria-hidden': 'true', text: 'NO SIGNAL' }), el('span', { class: 'clk-sr', text: `${unitShort(idx)} ${TB_NOTE}` }));
        const name = sg.tb.node.querySelector('.tb-name');
        if (name) name.after(n); else sg.tb.node.append(n);
      }
      if (n) n.hidden = !off;
    }
  }

  /* ---------------------------------------------------------------- the Device view's panel */

  /* A source a clock-set cannot ask for now, and why (the clock message says only that). */
  function whyNot(c, s) {
    const d = c.digital || {};
    if (!(c.offered || []).includes(s)) return 'the unit does not offer it';
    if ((s === 'spdif' || s === 'adat') && c.rate > 96000) return `carries 44.1-96 kHz, the unit runs at ${kHz(c.rate)}`;
    if (s === 'adat' && d.opticalIn !== 'adat') return 'set the optical input to ADAT first';
    if (s === 'spdif' && !d.coax && d.opticalIn !== 'spdif') return 'set the optical input to S/PDIF first';
    if (s === 'spdif' && d.src === true) return 'turn SR Convert off first';
    return 'not available now';
  }
  function digWhyNot(c, what) {
    const d = c.digital || {};
    if (what === 'optical-in' && (c.source === 'adat' || (c.source === 'spdif' && !d.coax))) return 'the clock runs from this input: put it on Internal first';
    if (what === 'src' && c.source === 'spdif') return 'the clock runs from S/PDIF: put it on Internal first';
    return '';
  }
  function chanText(c) {
    if (c.rate >= 176400) return `Digital I/O off at this rate (${kHz(c.rate)}).`;
    const ch = (c.digital && c.digital.channels) || {}, parts = [];
    if (ch.adatIn) parts.push(`ADAT in ${ch.adatIn} ch`);
    if (ch.spdifIn) parts.push(`S/PDIF in ${ch.spdifIn} ch`);
    if (ch.adatOut) parts.push(`ADAT out ${ch.adatOut} ch`);
    if (ch.spdifOut) parts.push(`S/PDIF out ${ch.spdifOut} ch`);
    return parts.length ? parts.join(', ') : 'No digital channels carry audio now.';
  }

  /* Why the controls wait, or '' (the host runs one change at a time, for every unit). */
  function busyText(id, c) {
    const p = P.pending;
    if (p) {
      if (p.id !== id) return `Waiting for the ${nameOf(p.id)}'s change to finish.`;
      return p.why || (p.what === 'source' ? `Switching to ${srcName(p.value)}...` : 'Writing the format...');
    }
    if (P.queue.some(j => j.id === id)) return 'Waiting for the change before it.';
    if (P.queue.length) return 'Waiting for another change to finish.';
    if (c && c.changing) return 'A clock change is under way.';
    return '';
  }

  const fine = text => el('p', { class: 'clk-fine', text });
  const noteBox = lines => el('div', { class: 'clk-notes', role: 'status' }, lines.map(([kind, text]) => el('div', { class: 'clk-note', dataset: { kind }, text })));
  const row = (lbl, ctl, why) => el('div', { class: 'clk-row' }, el('span', { class: 'lbl', text: lbl }), ctl, why ? el('span', { class: 'clk-why', text: why }) : null);

  /* Rebuild a box, keeping keyboard focus on the same control (data-fkey) or in the box. */
  function swap(box, kids) {
    const a = document.activeElement, fk = a && box.contains(a) && a.dataset ? a.dataset.fkey : '';
    const had = !!a && box.contains(a);
    box.replaceChildren(...kids);
    if (!had) return;
    const f = fk ? box.querySelector(`[data-fkey="${CSS.escape(fk)}"]`) : null;
    if (f && !f.disabled) f.focus();
    else if (box.tabIndex >= -1 && box.hasAttribute('tabindex')) box.focus();
  }

  function renderDev(force) {
    const box = document.getElementById('devClock');
    if (!box) return;
    if (force) D.stamp = '';
    const idx = S.sel, id = unitIdOf(idx);
    if (S.demo && id && P.clock[id] && P.clock[id].demo) demoRate(idx, P.clock[id]);
    if (!S.demo && host && id && P.asked[id] === undefined) ask(id);
    const c = id ? P.clock[id] : null, p = id ? P.panel[id] : null, list = controlUnits();
    const waited = !!id && P.asked[id] !== undefined && now() - P.asked[id] > ASK_MS;
    const stamp = JSON.stringify([idx, id, S.demo, !!host, list.map(u => u.index), unitReadOnly(idx), c && { ...c, measuredHz: 0, t: 0, registers: 0 },
      p && [p.knob, p.preamp, p.role, p.valid, p.talkbackChannel], P.pending, P.queue.length, P.notes[id], holdOf(id), waited]);
    if (stamp !== D.stamp) {
      D.stamp = stamp;
      build(box, idx, id, c, p, waited, list);
    }
    live(c);
  }

  function build(box, idx, id, c, p, waited, list) {
    const name = unitShort(idx), kids = [];
    if (list.length > 1) {
      if (!D.units) D.units = { seg: el('div', { class: 'seg clk-units', role: 'group', 'aria-label': 'Unit shown' }), segKey: '', buttons: [] };
      unitButtons(D.units, list);
      for (const b of D.units.buttons) b.dataset.fkey = `clk-unit:${b.dataset.unit}`;
      kids.push(el('div', { class: 'clk-row' }, el('span', { class: 'lbl', text: 'Unit' }), D.units.seg));
    }
    D.details = null;
    if (!id) kids.push(fine(S.unitList ? `The ${name} is not connected.` : 'Waiting for the driver to name the unit.'));
    else if (!c) {
      kids.push(fine(S.demo ? 'Not in the demo.' : !host ? 'Not in this preview.'
        : waited ? 'No answer from the host: this Apollo Control or apollo.sys has no clock control.' : 'Asking the driver...'));
    } else if (c.error) kids.push(fine(`Not available: ${c.detail || `error ${c.error}`}.`));
    else kids.push(...clockPart(idx, id, c, name), ...digitalPart(idx, id, c, name), ...panelPart(p));
    swap(box, kids);
  }

  function clockPart(idx, id, c, name) {
    const ro = unitReadOnly(idx), busy = busyText(id, c), dis = !!busy || !!ro;
    const pend = P.pending && P.pending.id === id && P.pending.what === 'source' ? P.pending.value : '';
    const proven = c.proven || [], usable = c.usable || [], sources = (c.sources || []).filter(s => s !== 'peer');
    const sel = el('select', { class: 'select clk-sel', id: 'clkSource', 'aria-label': `${name} clock source`, dataset: { fkey: 'clk-source' },
      onchange: e => pickSource(id, e.target.value) });
    for (const s of sources) {
      const ok = usable.includes(s) || s === c.source, why = ok ? '' : whyNot(c, s);
      sel.append(el('option', { value: s, disabled: !ok, text: `${srcName(s)}${proven.includes(s) ? '' : ' (Experimental)'}${why ? `: ${why}` : ''}` }));
    }
    if (c.source && !sources.includes(c.source)) sel.prepend(el('option', { value: c.source, disabled: true, text: `${srcName(c.source)} (now)` }));
    sel.value = pend || c.source;
    sel.disabled = dis;
    sel.classList.toggle('unconfirmed', !!pend || !!c.changing);
    sel.title = ro ? `The ${name} is in ${ro} mode: read-only.` : busy || 'Where the unit takes its clock from. Internal is proven on these units; the others are experimental.';
    const st = pend || c.changing ? 'busy' : c.locked ? 'ok' : 'bad';
    const lock = el('span', { class: 'clk-lock', dataset: { state: st }, title: 'The unit\'s lock to its clock source' },
      el('span', { class: 'lamp', 'aria-hidden': 'true' }), el('span', { text: st === 'busy' ? 'CHANGING' : c.locked ? 'LOCKED' : 'NOT LOCKED' }));
    const cur = pend || c.source;
    const out = [el('div', { class: 'clk-row' }, el('label', { class: 'lbl', for: 'clkSource', text: 'Clock' }), sel, lock,
      proven.includes(cur) ? null : el('span', { class: 'tag clk-exp', text: 'EXPERIMENTAL', title: 'Never run on these units' }))];
    if (c.fellBack) {
      out.push(el('div', { class: 'clk-banner', role: 'status' },
        el('span', { class: 'clk-banner-text', text: `${srcName(c.wanted)} lost lock: back on Internal` }),
        el('span', { class: 'clk-banner-btns' },
          el('button', { class: 'btn btn-primary', type: 'button', disabled: dis, dataset: { fkey: 'clk-again' }, onclick: () => retry(id) }, 'Try again'),
          el('button', { class: 'btn', type: 'button', disabled: dis, dataset: { fkey: 'clk-stay' },
            onclick: () => { if (!busyText(id, P.clock[id])) request(id, 'source', 'internal', { hold: holdOf(id) }); } }, 'Stay on Internal'))));
    }
    const notes = [];
    if (ro) notes.push(['info', `The ${name} is in ${ro} mode: read-only, nothing is sent to it.`]);
    if (busy && !(P.pending && P.pending.id === id && P.pending.what !== 'source')) notes.push(['info', busy]);
    if (c.reapply && !c.fellBack) notes.push(['info', `Goes back on ${srcName(c.wanted)} once the transport runs again.`]);
    const n = (P.notes[id] || {}).source;
    if (n) notes.push([n.kind, n.text]);
    if (notes.length) out.push(noteBox(notes));
    const hold = el('input', { type: 'checkbox', id: 'clkHold', checked: holdOf(id), disabled: dis, dataset: { fkey: 'clk-hold' },
      onchange: e => { P.hold[id] = e.target.checked; renderDev(); } });
    out.push(el('div', { class: 'clk-adv' },
      el('label', { class: 'check', title: 'Advanced. Without it the unit goes back to Internal after 1 s without lock.' }, hold, 'Stay on the source when its signal drops'),
      fine(`Advanced, sent with the next clock change. Now: ${c.hold ? 'it stays on the source' : 'back on Internal after 1 s without lock'}.`)));
    D.details = el('div', { class: 'clk-details num' });
    out.push(D.details);
    return out;
  }

  function digitalPart(idx, id, c, name) {
    const d = c.digital || {}, has = d.has || [], rows = ['optical-in', 'optical-out', 'src'].filter(w => has.includes(w));
    if (!rows.length) return [];
    const ro = unitReadOnly(idx), busy = busyText(id, c), dis = !!busy || !!ro;
    const pend = P.pending && P.pending.id === id && P.pending.what !== 'source' ? P.pending : null;
    const out = [el('div', { class: 'clk-sub' }, el('span', { class: 'sect-title', text: 'Digital I/O' }),
      el('span', { class: 'tag clk-exp', text: 'EXPERIMENTAL', title: 'No format write has run on these units yet' }))];
    for (const what of rows) {
      const why = digWhyNot(c, what), mine = pend && pend.what === what;
      if (what === 'src') {
        const b = sw('SR CONVERT', { aria: `${name} SR Convert`, title: 'The sample-rate converter on the S/PDIF input', onClick: () => pickDigital(id, 'src', d.src !== true) });
        b.dataset.fkey = 'clk-src';
        const on = mine ? !!pend.value : d.src === true;
        setPressed(b, on);
        b.disabled = dis || !!why;
        b.classList.toggle('unconfirmed', !!mine);
        const r = row(DIG.src.label, b, why);
        b.after(el('span', { class: 'clk-state', 'aria-hidden': 'true', text: on ? 'ON' : 'OFF' }));     /* the state in words too */
        out.push(r);
      } else {
        const v = mine ? pend.value : d[DIG[what].key];
        const s = seg(['ADAT', 'S/PDIF'], i => pickDigital(id, what, i ? 'spdif' : 'adat'), `${name} ${DIG[what].long} format`);
        s.set(v === 'adat' ? 0 : v === 'spdif' ? 1 : -1);
        s.node.querySelectorAll(':scope > button').forEach((b, i) => { b.disabled = dis || !!why; b.dataset.fkey = `clk-${what}:${i}`; });
        s.node.classList.toggle('unconfirmed', !!mine);
        out.push(row(DIG[what].label, s.node, why));
      }
    }
    out.push(el('div', { class: 'clk-chans num', role: 'status', text: chanText(c) }));
    if (!d.known) out.push(fine('Not set from here since the unit connected: these are the formats it connects with (S/PDIF).'));
    const notes = [];
    if (pend && busy) notes.push(['info', busy]);
    const n = (P.notes[id] || {}).digital;
    if (n) notes.push([n.kind, n.text]);
    if (notes.length) out.push(noteBox(notes));
    return out;
  }

  function panelPart(p) {
    if (!p) return [];
    const parts = [], out = [];
    if (p.valid && p.knob) parts.push(`Knob: ${p.knob === 'preamp' ? `PREAMP ${p.preamp}` : String(p.knob).toUpperCase()}`);
    if (p.role === 'monitor') parts.push('monitor link: the speakers');
    else if (p.role === 'knob') parts.push('monitor link: a knob unit, it drives the speakers');
    if (parts.length) out.push(fine(parts.join(' · ')));
    if (p.talkbackChannel && p.talkbackChannel.strip === false) out.push(fine(`Talkback mic: no signal yet (its input strip is not built in this driver).`));
    return out.length ? [el('div', { class: 'clk-sub' }, el('span', { class: 'sect-title', text: 'Front panel' })), ...out] : [];
  }

  /* The measured rate and the raw registers, in place (they change every few seconds). */
  function live(c) {
    if (!D.details || !c) return;
    const r = c.registers || {}, rb = c.readbackLock;
    const parts = [c.measuredHz ? `Measured ${Number(c.measuredHz).toFixed(3)} Hz` : 'Measured: not yet',
      `clockCfg ${hex(r.clockCfg || 0, 1)}`, `rateInfo ${hex(r.rateInfo || 0, 1)}`, `clockInfo ${r.clockInfo ?? '-'}`,
      `readback lock ${rb === null || rb === undefined ? 'not reported' : rb ? 'yes' : 'no'}`];
    if (c.combined && c.combined.member) parts.push(c.combined.timeline ? 'the combined device\'s timeline' : 'in the combined device');
    put(D.details, parts.join(' · '));
  }

  /* ---------------------------------------------------------------- picks (one confirmation for the experimental) */

  async function pickSource(id, s) {
    const c = P.clock[id], idx = slotOf(id);
    if (!c || busyText(id, c) || (s === c.source && s === c.wanted) || !SOURCES.includes(s)) { renderDev(true); return; }
    if (idx >= 0 && !writable(idx)) { renderDev(true); return; }
    const exp = !(c.proven || []).includes(s);
    if (exp && !(await confirmSource(id, c, s))) { renderDev(true); return; }
    const c2 = P.clock[id];
    if (!c2 || busyText(id, c2)) { renderDev(true); return; }
    request(id, 'source', s, { try: exp, hold: holdOf(id) });
  }

  function confirmSource(id, c, s) {
    const name = nameOf(id);
    const body = el('div', {},
      el('p', { text: `${srcName(s)} is experimental, never run on these units: the unit may go silent for up to 2 s and comes back on Internal if the source does not lock.` }),
      c.combined && c.combined.timeline && running().length > 1
        ? el('p', { text: `The ${name}'s clock is the combined device's timeline: the other unit resyncs, and a failed change mutes it for about 10 s. Prefer a moment with no recording, or make the other unit the clock master first (Units & routing).` })
        : null);
    return dialog({ title: `Clock the ${name} from ${srcName(s)}?`, body, confirm: `Use ${srcName(s)}` });
  }

  function retry(id) {
    const c = P.clock[id];
    if (!c || !c.wanted || busyText(id, c)) return;
    request(id, 'source', c.wanted, { try: !(c.proven || []).includes(c.wanted), hold: holdOf(id) });
  }

  async function pickDigital(id, what, value) {
    const c = P.clock[id], idx = slotOf(id);
    if (!c || busyText(id, c) || !DIG[what]) return;
    const d = c.digital || {}, cur = what === 'src' ? d.src === true : d[DIG[what].key];
    if (cur === value) return;
    if (idx >= 0 && !writable(idx)) return;
    const exp = !(d.proven || []).includes(what);
    if (exp && !(await confirmDigital(id, what, value))) { renderDev(true); return; }
    const c2 = P.clock[id];
    if (!c2 || busyText(id, c2)) { renderDev(true); return; }
    request(id, what, value, { try: exp });
  }

  function confirmDigital(id, what, value) {
    const name = nameOf(id);
    if (what === 'src') {
      return dialog({ title: `Turn SR Convert ${value ? 'on' : 'off'} on the ${name}?`, confirm: `Turn it ${value ? 'on' : 'off'}`,
        body: `SR Convert on the S/PDIF input is experimental, never run on these units. While it is on, S/PDIF cannot be the clock source.` });
    }
    return dialog({ title: `Set the ${name} ${DIG[what].long} to ${FMT[value]}?`, confirm: `Set to ${FMT[value]}`,
      body: `Writing the ${DIG[what].long} format is experimental, never run on these units. The channel layout may change (ADAT channels appear or go) and a DAW using the driver gets a reset request.` });
  }

  /* ---------------------------------------------------------------- the monitor link (Units & routing) */

  function linkPanel() {
    if (!L.node) L.node = el('div', { class: 'panel clk-link' });
    renderLink();
    return L.node;
  }

  function linkState() {
    const k = P.link;
    if (!k) return null;
    if (!k.on) return ['info', 'The driver reports the link off.'];
    const what = LINK.filter(([f]) => k[f]).map(([, t]) => t.toLowerCase()).join(', ') || 'nothing';
    return ['ok', `On: speakers on the ${k.monitor ? nameOf(k.monitor) : 'unit'}, the knob unit drives ${what}. Passed on ${k.applied || 0}, busy ${k.busy || 0}, dropped ${k.dropped || 0}.`];
  }

  function renderLink() {
    const n = L.node;
    if (!n) return;
    const l = linkCfg(), units = (S.unitList || []).filter(u => u.present && MODEL_CAPS[u.deviceType]), live = running();
    const stamp = JSON.stringify([S.demo, !!host, l, units.map(u => [u.id, u.deviceType, u.active, u.mode, u.observe]), live.map(u => u.id),
      P.link, P.linkResult, Object.values(P.panel).map(p => [p.unit, p.role])]);
    if (stamp === L.stamp && n.firstChild) return;
    L.stamp = stamp;
    const on = !!(P.link && P.link.on);
    const kids = [el('div', { class: 'panel-head' }, el('h2', { text: 'Monitor link' }),
      el('span', { class: `tag${on ? ' on' : ''}`, text: on ? 'LINK ON' : 'LINK OFF' })),
    el('p', { class: 'ur-text', text: 'One unit has the speakers; the other unit\'s monitor knob and its MUTE, DIM and MONO buttons drive them too. ' +
      'The driver drops the link whenever it restarts, so Apollo Control sends it again each time both units are running.' })];
    if (S.demo || !host) {
      kids.push(el('p', { class: 'ur-note clk-na', text: 'Not in the demo: the monitor link runs in apollo.sys, with both units connected.' }));
      swap(n, kids);
      return;
    }
    const choices = [{ id: '', label: 'Off' }, ...units.map(u => ({ id: u.id, label: capsOf(u.deviceType).short || u.model, ok: isUid(u.id) }))];
    if (l.monitor && !units.some(u => u.id === l.monitor)) choices.push({ id: l.monitor, label: 'Saved unit (not connected)', ok: true });
    const s = seg(choices.map(x => x.label), i => setMonitor(choices[i].id), 'Speakers on');
    s.set(Math.max(0, choices.findIndex(x => x.id === l.monitor)));
    s.node.querySelectorAll(':scope > button').forEach((b, i) => {
      const x = choices[i];
      b.dataset.fkey = `clk-link-mon:${x.id || 'off'}`;
      if (x.id && x.ok === false) { b.disabled = true; b.title = 'Named by its serial (no HW_UID from this driver): it cannot be the speaker unit'; }
      else if (x.id) b.title = `The speakers are on the ${x.label}; the other unit's knob and buttons drive them`;
    });
    kids.push(el('div', { class: 'clk-row' }, el('span', { class: 'lbl', text: 'Speakers on' }), s.node));
    kids.push(el('div', { class: 'clk-link-flags', role: 'group', 'aria-label': 'What the knob unit drives' }, LINK.map(([f, t, title]) =>
      el('label', { class: 'check', title }, el('input', { type: 'checkbox', checked: l[f], dataset: { fkey: `clk-link:${f}` },
        onchange: e => setFlag(f, e.target.checked) }), t))));
    const lines = [];
    if (live.length < 2) lines.push(['info', l.monitor ? 'Needs both units running: it goes out when they are.' : 'Needs both units running.']);
    else if (l.monitor && !live.some(u => u.id === l.monitor)) lines.push(['info', 'The speaker unit is not running: the link goes out when it is.']);
    const st = linkState(), r = P.linkResult;
    if (st) lines.push(st);
    if (r && (!r.ok || !st)) lines.push([r.ok ? 'ok' : 'bad', r.why || (r.ok ? 'Done.' : `Not changed (${r.code || 'error'}).`)]);    /* the host's words */
    for (const p of Object.values(P.panel)) if (p.role === 'knob') lines.push(['info', `The ${nameOf(p.unit)} is a knob unit: it drives the monitor.`]);
    if (lines.length) kids.push(noteBox(lines));
    swap(n, kids);
  }

  function setMonitor(id) {
    cfg.panelLink = { ...linkCfg(), monitor: id };
    saveConfig();
    sendLink(true);
    renderLink();
  }

  function setFlag(f, v) {
    cfg.panelLink = { ...linkCfg(), [f]: !!v };
    saveConfig();
    if (cfg.panelLink.monitor) sendLink(true);
    renderLink();
  }

  /* ---------------------------------------------------------------- the demo: a host that always locks */

  function demoState(u) {
    const x8p = u.deviceType === 0x22;
    const c = {
      type: 'clock', unit: u.id, present: true, demo: true, source: 'internal', wanted: 'internal', locked: true, changing: false,
      fellBack: false, hold: false, reapply: false,
      sources: x8p ? ['internal', 'spdif', 'adat', 'wordclock'] : ['internal', 'spdif', 'adat'],
      offered: x8p ? ['internal', 'spdif', 'adat', 'wordclock', 'peer'] : ['internal', 'spdif', 'adat', 'peer'],
      usable: [], proven: ['internal'], rate: 48000, ratesOnSource: [],
      registers: x8p ? { clockCfg: 0x20C, rateInfo: 0x201, clockInfo: 48000 } : { clockCfg: 0x20C, rateInfo: 0x201, clockInfo: 48000 },
      readbackLock: true, combined: { member: false, timeline: false },
      digital: { has: x8p ? ['optical-in', 'optical-out', 'coax', 'src'] : ['optical-in', 'src'], proven: [], known: false, opticalIn: 'spdif',
        opticalOut: x8p ? 'spdif' : null, coax: x8p, src: false, channels: {} },
      last: { code: '', why: '', ms: 0, changes: 0, fallbacks: 0, lockLosses: 0 }, measuredHz: 0, t: now(),
    };
    demoDerive(c);
    P.clock[u.id] = c;
    P.panel[u.id] = { type: 'panel', unit: u.id, present: true, valid: true, knob: 'monitor', preamp: 1, role: null, demo: true,
      talkbackChannel: { recChannels: x8p ? 32 : 34, slots: x8p ? null : [32, 33], strip: false } };
  }

  function demoDerive(c) {
    const d = c.digital, x8p = c.sources.includes('wordclock'), low = c.rate <= 96000, smux = c.rate > 48000;
    c.usable = c.sources.filter(s => s === 'internal' || s === 'wordclock' || (low && s === 'adat' && d.opticalIn === 'adat') ||
      (low && s === 'spdif' && (d.coax || d.opticalIn === 'spdif') && !d.src));
    d.channels = {
      adatIn: low && (!d.known || d.opticalIn === 'adat') ? (smux && !x8p ? 4 : 8) : 0,
      adatOut: low && x8p && (!d.known || d.opticalOut === 'adat') ? 8 : 0,
      spdifIn: low && (d.coax || !d.known || d.opticalIn === 'spdif') ? 2 : 0,
      spdifOut: low && x8p ? 2 : 0,
    };
    c.ratesOnSource = c.source === 'spdif' || c.source === 'adat' ? RATES.filter(r => r <= 96000) : RATES.slice();
    c.measuredHz = c.rate - 0.278;
  }

  /* the demo's rate (app.js RATE) and whether it runs combined */
  function demoRate(idx, c) {
    const r = rateFor(idx), rate = (r && r.rate) || c.rate, both = (S.unitList || []).filter(u => u.present).length > 1;
    if (rate === c.rate && c.combined.member === both) return;
    c.rate = rate;
    c.combined = { member: both, timeline: both && idx === 0 };
    demoDerive(c);
  }

  function demoHost(o) {
    if (o.cmd !== 'clock-set' && o.cmd !== 'digital-set') return;
    const id = o.unit;
    if (!P.clock[id]) return;
    const what = o.cmd === 'clock-set' ? 'source' : o.what;
    const value = what === 'source' ? o.source : what === 'src' ? (o.value ? 'on' : 'off') : o.value;
    setTimeout(() => onResult({ type: 'clock-result', unit: id, what, value, pending: true, code: '',
      why: what === 'source' ? 'Switching the clock (up to 4 s; it goes back to Internal if the source does not lock)' : 'Writing the format', ms: 0 }), 0);
    setTimeout(() => {
      const c = P.clock[id];
      if (!S.demo || !c) return;
      if (what === 'source') Object.assign(c, { source: o.source, wanted: o.source, locked: true, hold: !!o.hold, fellBack: false });
      else { c.digital[DIG[what].key] = what === 'src' ? !!o.value : o.value; c.digital.known = true; }
      demoDerive(c);
      finish({ type: 'clock-result', unit: id, what, value, pending: false, ok: true, code: '', ms: 900,
        why: what === 'source' ? `Demo: on ${srcName(o.source)}, locked (simulated).` : `Demo: ${label(what, value)} written (simulated).` });
    }, 900);
  }

  return {
    onHost, onUnits, head, keepPrefs, linkPanel,
    renderDevice: renderDev,
    get state() { return P; },
  };
})();
