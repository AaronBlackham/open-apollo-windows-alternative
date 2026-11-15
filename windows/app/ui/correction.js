'use strict';
/*
 * Speaker correction (Apollo Control): an Avium correction profile (Avium Measure, Correction mode,
 * Export) on one unit's speaker outputs. Load the file, see its filters and target, choose the
 * outputs, switch it on or off, A/B it. The driver runs it on the playback ring after every host
 * source is summed (include/apollo_corr.h); the host checks the file with its port of Avium
 * Measure's validate() and compiles it for the unit's rate (app/src/corr_profile.h, corr_host.h).
 *
 * Messages (JSON strings, like units.js):
 *   to host    {cmd:'corr-get'|'corr-check'|'corr-load'|'corr-set', unit:<unit id>, ...}
 *   from host  {type:'corr'} a unit's state, {type:'corr-check'} a file's verdict,
 *              {type:'corr-result'} the outcome of a load or a switch
 * app.js routes those types here (onHost) and calls show() when the view opens ([Corr] hooks).
 * Loaded before app.js; app.js globals (S, el, svg, sw, toast, dialog, host, setPressed) are used
 * only inside functions, which run after app.js booted. With no host (a plain browser) the page
 * runs on a demo unit state.
 */
const Correction = (() => {
  const X8P = 0x22, TWINX = 0x99;
  /* Playback slots a speaker pair is likely on, by model (the driver's slot order). */
  const OUTS = {
    [X8P]: ['MON L', 'MON R', 'LINE 1', 'LINE 2', 'LINE 3', 'LINE 4', 'LINE 5', 'LINE 6', 'LINE 7', 'LINE 8'],
    [TWINX]: ['MON L', 'MON R', 'LINE 3', 'LINE 4'],
  };
  const SHORT = { [X8P]: 'x8p', [TWINX]: 'Twin X' };
  const POLL_MS = 1000;
  const TYPE_LABEL = {
    peaking: 'Peak', low_shelf: 'Low shelf', high_shelf: 'High shelf', lowpass: 'Low-pass', highpass: 'High-pass',
    lowpass_1: 'Low-pass 6 dB', highpass_1: 'High-pass 6 dB', low_shelf_1: 'Low shelf 6 dB', high_shelf_1: 'High shelf 6 dB',
    notch: 'Notch', allpass: 'All-pass', bandpass: 'Band-pass',
  };
  const GAIN_TYPES = new Set(['peaking', 'low_shelf', 'high_shelf', 'low_shelf_1', 'high_shelf_1']);
  const Q_TYPES = new Set(['peaking', 'low_shelf', 'high_shelf', 'lowpass', 'highpass', 'notch', 'allpass', 'bandpass']);
  const CH_COLORS = ['var(--accent)', '#5a9be6', '#4fb86a', '#9b7be0'];
  const HW_NOTE = 'The Apollo\'s own input monitoring (mics and instruments in the unit\'s Console mix) never passes ' +
    'through the computer, so it is not corrected. Correction applies to computer audio on these outputs: Windows ' +
    '(the Monitor endpoint) and the DAW (Apollo (Avium) ASIO) both get it. It is applied before the unit\'s monitor mix, ' +
    'so ALT speakers and headphones that follow the monitor mix hear it too.';

  const P = {
    unit: null,             /* selected unit id */
    picked: false,          /* the user chose it (else it follows the default) */
    state: {},              /* unit id -> last {type:'corr'} message */
    preview: null,          /* a file being looked at: {unit, file, text, data, seq, verdict, slots, enable, error} */
    busy: '',               /* an op waiting for its corr-result */
    seq: 0,
    timer: 0,
    root: null,
    profileKey: '',
    demo: {},               /* unit id -> demo state (no host) */
  };
  let input = null;         /* the hidden file picker */

  /* ---------------------------------------------------------------- */
  /* Units                                                            */
  /* ---------------------------------------------------------------- */

  function units() {
    return (S.unitList || []).filter(u => u.present && !u.observe);
  }
  function unitById(id) { return units().find(u => u.id === id) || null; }
  function shortOf(u) { return u ? (SHORT[u.deviceType] || u.model) : 'unit'; }
  function outName(u, slot) {
    const names = u ? OUTS[u.deviceType] : null;
    return names && slot >= 0 && slot < names.length ? names[slot] : `slot ${slot + 1}`;
  }

  /* The unit with a profile, else the Twin X (the monitor unit), else the first. */
  function defaultUnit() {
    const list = units();
    const withProfile = list.find(u => { const s = P.state[u.id]; return s && s.state && s.state.loaded; });
    return (withProfile || list.find(u => u.deviceType === TWINX) || list[0] || { id: null }).id;
  }

  function syncUnit() {
    const list = units();
    if (!P.unit || !list.some(u => u.id === P.unit) || !P.picked) {
      const want = defaultUnit();
      if (want !== P.unit) { P.unit = want; return true; }
    }
    return false;
  }

  /* ---------------------------------------------------------------- */
  /* Host                                                             */
  /* ---------------------------------------------------------------- */

  function post(obj) {
    if (S.demo || !host) { demoHost(obj); return; }
    host.postMessage(JSON.stringify(obj));   /* a string: the host reads strings */
  }

  function poll() {
    if (P.unit) post({ cmd: 'corr-get', unit: P.unit });
  }

  function startPolling() {
    clearInterval(P.timer);
    P.timer = setInterval(() => {
      if (S.view !== 'correction') { clearInterval(P.timer); P.timer = 0; return; }
      const key = units().map(u => u.id).join(',');
      if (syncUnit() || key !== P.unitsKey) render();
      poll();
    }, POLL_MS);
  }

  function onHost(m) {
    if (m.type === 'corr') {
      P.state[m.unit] = m;
      if (syncUnit() || m.unit === P.unit) refresh();
    } else if (m.type === 'corr-check') {
      const p = P.preview;
      if (!p || m.unit !== p.unit || m.seq !== p.seq) return;    /* an answer about an older file or unit */
      p.verdict = m;
      p.error = '';
      render();
    } else if (m.type === 'corr-result') {
      const was = P.busy;
      P.busy = '';
      if (m.ok) {
        if (m.op === 'load') {
          toast(`Profile loaded on the ${shortOf(unitById(m.unit))}.` + (m.detail ? ` ${m.detail}.` : ''), 'info');
          P.preview = null;
          if (input) input.value = '';
        }
        if (m.op === 'clear') toast('Profile removed. The outputs play uncorrected.', 'info');
      } else if (m.op === 'load' && P.preview) {
        P.preview.error = m.detail || 'The driver refused the profile.';
      } else {
        toast(`${was === 'clear' ? 'Remove' : 'Speaker correction'}: ${m.detail || 'refused'}`, 'warn', 7000);
      }
      render();
    }
  }

  /* ---------------------------------------------------------------- */
  /* Actions                                                          */
  /* ---------------------------------------------------------------- */

  function cur() { const s = P.state[P.unit]; return s && s.state ? s.state : null; }

  function setEnabled(on) {
    if (!P.unit) return;
    P.busy = 'enable';
    post({ cmd: 'corr-set', unit: P.unit, op: 'enable', value: on });
  }
  function setBypass(on) {
    if (!P.unit) return;
    P.busy = 'bypass';
    post({ cmd: 'corr-set', unit: P.unit, op: 'bypass', value: on });
  }
  async function remove() {
    const u = unitById(P.unit), st = cur();
    if (!u || !st || !st.loaded) return;
    const ok = await dialog({ title: 'Remove the profile?',
      body: `The ${shortOf(u)} plays uncorrected after a 20 ms fade, and the profile is no longer loaded at start.`,
      confirm: 'Remove', kind: 'danger' });
    if (!ok) return;
    P.busy = 'clear';
    post({ cmd: 'corr-set', unit: P.unit, op: 'clear', value: 1 });
  }

  function pickFile() {
    if (!input) {
      input = el('input', { type: 'file', accept: '.json,application/json', hidden: true, 'aria-label': 'Profile file' });
      input.addEventListener('change', () => {
        const f = input.files && input.files[0];
        if (f) readFile(f);
      });
      document.body.append(input);
    }
    input.value = '';
    input.click();
  }

  function readFile(f) {
    const r = new FileReader();
    r.onload = () => openText(String(r.result || ''), f.name);
    r.onerror = () => toast(`Could not read ${f.name}.`, 'warn');
    r.readAsText(f);
  }

  /* A file's text: show what it says, ask the host for the verdict on this unit. */
  function openText(text, name) {
    let data = null;
    try { data = JSON.parse(text); } catch (e) { data = null; }
    const p = { unit: P.unit, file: name, text, data, seq: ++P.seq, verdict: null, slots: {}, enable: true, error: '' };
    const st = cur(), loaded = st && st.profile ? st.profile.channels : [];
    for (const c of channelsOf(data)) {
      const was = loaded.find(x => x.id === c.id);
      p.slots[c.id] = was ? was.slot : (c.id === 'L' ? 0 : c.id === 'R' ? 1 : -1);
    }
    P.preview = p;
    post({ cmd: 'corr-check', unit: p.unit, text, seq: p.seq });
    render();
  }

  function apply() {
    const p = P.preview;
    if (!p || !p.verdict || !p.verdict.ok) return;
    const used = Object.values(p.slots).filter(s => s >= 0);
    if (!used.length) { p.error = 'Choose an output for at least one channel.'; render(); return; }
    if (new Set(used).size !== used.length) { p.error = 'Two channels are on the same output.'; render(); return; }
    p.error = '';
    P.busy = 'load';
    post({ cmd: 'corr-load', unit: p.unit, text: p.text, slots: p.slots, enable: p.enable });
    render();
  }

  /* ---------------------------------------------------------------- */
  /* Profiles as the page shows them                                  */
  /* ---------------------------------------------------------------- */

  function channelsOf(d) {
    return d && Array.isArray(d.channels) ? d.channels.filter(c => c && typeof c.id === 'string') : [];
  }

  /* The file's JSON or the driver's copy -> {name, created, summary, preampDb, rate, channels: [...]} */
  function fromFile(d, summary) {
    return {
      name: String(d.name || ''), created: String(d.created || ''), summary: summary || '',
      preampDb: Number(d.preamp_db) || 0, rate: 48000,
      channels: channelsOf(d).map(c => ({
        id: c.id, name: String(c.name || ''), output: Number(c.output) || 0, slot: null,
        gainDb: Number(c.gain_db) || 0, delayMs: Number(c.delay_ms) || 0,
        filters: (Array.isArray(c.filters) ? c.filters : []).map(b => ({
          type: b.type, enabled: b.enabled !== false, freq: Number(b.freq_hz), gain: Number(b.gain_db) || 0,
          q: Number(b.q) || 0.7071, coef: b.coefficients && Array.isArray(b.coefficients['48000']) ? b.coefficients['48000'] : null,
        })),
      })),
    };
  }

  /* |H| in dB of a section [b0 b1 b2 a1 a2] at f (display only) */
  function sectionDb(c, f, fs) {
    const w = 2 * Math.PI * f / fs, cw = Math.cos(w), sw1 = Math.sin(w), c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
    const br = c[0] + c[1] * cw + c[2] * c2, bi = -(c[1] * sw1 + c[2] * s2);
    const ar = 1 + c[3] * cw + c[4] * c2, ai = -(c[3] * sw1 + c[4] * s2);
    return 10 * Math.log10(Math.max((br * br + bi * bi) / Math.max(ar * ar + ai * ai, 1e-300), 1e-30));
  }

  function curve(prof, ch, freqs) {
    return freqs.map(f => ch.filters.reduce((db, b) => db + (b.enabled && b.coef ? sectionDb(b.coef, f, prof.rate) : 0),
      prof.preampDb + ch.gainDb));
  }

  const fmtHz = f => !isFinite(f) ? '-' : f >= 1000 ? `${+(f / 1000).toFixed(f >= 10000 ? 1 : 2)} kHz` : `${+f.toFixed(f < 100 ? 1 : 0)} Hz`;
  const fmtDb = (v, d = 1) => `${v > 0 ? '+' : ''}${(Number(v) || 0).toFixed(d)} dB`;

  function chart(prof) {
    const W = 640, H = 210, L = 38, R = 16, T = 8, B = 22;
    const freqs = [];
    for (let i = 0; i <= 240; i++) freqs.push(20 * Math.pow(1000, i / 240));
    const curves = prof.channels.map(ch => curve(prof, ch, freqs));
    let lo = -12, hi = 6;
    for (const c of curves) for (const v of c) { if (v < lo) lo = v; if (v > hi) hi = v; }
    lo = Math.max(-36, Math.floor(lo / 6) * 6);
    hi = Math.min(18, Math.ceil(hi / 6) * 6);
    const x = f => L + (W - L - R) * Math.log10(f / 20) / 3;
    const y = v => T + (H - T - B) * (hi - Math.min(hi, Math.max(lo, v))) / (hi - lo);
    const root = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'cr-chart', role: 'img',
      'aria-label': `Correction curves, ${prof.channels.map(c => c.id).join(' and ')}` });
    for (const f of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
      root.append(svg('line', { x1: x(f), x2: x(f), y1: T, y2: H - B, class: 'cr-grid' }));
      const t = svg('text', { x: x(f), y: H - 6, class: 'cr-axis', 'text-anchor': 'middle' });
      t.textContent = f >= 1000 ? `${f / 1000}k` : String(f);
      root.append(t);
    }
    for (let v = lo; v <= hi; v += 6) {
      root.append(svg('line', { x1: L, x2: W - R, y1: y(v), y2: y(v), class: v === 0 ? 'cr-grid cr-zero' : 'cr-grid' }));
      const t = svg('text', { x: L - 6, y: y(v) + 3, class: 'cr-axis', 'text-anchor': 'end' });
      t.textContent = v > 0 ? `+${v}` : String(v);
      root.append(t);
    }
    curves.forEach((c, k) => {
      const d = c.map((v, i) => `${i ? 'L' : 'M'}${x(freqs[i]).toFixed(1)},${y(v).toFixed(1)}`).join('');
      root.append(svg('path', { d, class: 'cr-curve', style: `stroke:${CH_COLORS[k % CH_COLORS.length]}` }));
    });
    return root;
  }

  function filterTable(prof, ch, k, u) {
    const rows = ch.filters.map((b, i) => el('tr', { class: b.enabled ? null : 'cr-off' },
      el('td', { class: 'n', text: String(i + 1) }),
      el('td', { text: TYPE_LABEL[b.type] || String(b.type) }),
      el('td', { class: 'n', text: fmtHz(b.freq) }),
      el('td', { class: 'n', text: GAIN_TYPES.has(b.type) ? fmtDb(b.gain) : '' }),
      el('td', { class: 'n', text: Q_TYPES.has(b.type) ? (Number(b.q) || 0).toFixed(2) : '' }),
      el('td', {}, b.enabled ? null : el('span', { class: 'tag', text: 'OFF' }))));
    const where = ch.slot !== null && ch.slot !== undefined ? ` on ${outName(u, ch.slot)}` : '';
    return el('div', { class: 'cr-ch' },
      el('div', { class: 'cr-ch-head' },
        el('span', { class: 'cr-swatch', style: `background:${CH_COLORS[k % CH_COLORS.length]}` }),
        el('span', { class: 'cr-ch-name', text: `${ch.id}${ch.name && ch.name !== ch.id ? ` (${ch.name})` : ''}${where}` }),
        el('span', { class: 'muted', text: `trim ${fmtDb(ch.gainDb, 2)}, delay ${(+ch.delayMs).toFixed(2)} ms, ` +
          `${ch.filters.filter(b => b.enabled).length} of ${ch.filters.length} filters on` })),
      ch.filters.length ? el('table', { class: 'st-table cr-filters' },
        el('thead', {}, el('tr', {}, el('th', { class: 'n', text: '#' }), el('th', { text: 'Type' }), el('th', { class: 'n', text: 'Frequency' }),
          el('th', { class: 'n', text: 'Gain' }), el('th', { class: 'n', text: 'Q' }), el('th', { text: '' }))),
        el('tbody', {}, rows)) : el('p', { class: 'cr-none', text: 'No filters: trim and delay only.' }));
  }

  function profileDetails(prof, u) {
    return el('div', { class: 'cr-prof' },
      el('div', { class: 'cr-meta' },
        el('div', {}, el('span', { class: 'cr-k', text: 'Preamp' }), el('span', { class: 'num', text: fmtDb(prof.preampDb) })),
        prof.created ? el('div', {}, el('span', { class: 'cr-k', text: 'Created' }), el('span', { text: prof.created.replace('T', ' ').replace('Z', ' UTC') })) : null,
        prof.summary ? el('div', { class: 'cr-sum' }, el('span', { class: 'cr-k', text: 'Measured' }), el('span', { text: prof.summary })) : null),
      chart(prof),
      el('div', { class: 'cr-chs' }, prof.channels.map((ch, k) => filterTable(prof, ch, k, u))));
  }

  /* ---------------------------------------------------------------- */
  /* Rendering                                                        */
  /* ---------------------------------------------------------------- */

  function stateLine(m, st, u) {
    if (!m) return { text: 'Reading the driver...', kind: '' };
    if (!m.present || !st) {
      return { text: m.detail || 'The driver did not answer.', kind: 'bad' };
    }
    if (!st.loaded) return { text: 'No profile on this unit: the outputs play uncorrected.', kind: '' };
    if (st.rateMismatch) {
      return { text: `Not running: the profile is for ${st.profile ? st.profile.rate : '?'} Hz and the unit runs ${st.unitRate} Hz. Load it again.`, kind: 'bad' };
    }
    if (st.unitIdle && st.enabled) return { text: 'On, waiting for the unit\'s audio engine to run.', kind: 'warn' };
    if (!st.enabled) return { text: st.wet > 0 ? 'Turning off...' : 'Off: the outputs play uncorrected.', kind: '' };
    if (st.bypass) return { text: st.wet > 0 ? 'A/B: fading to uncorrected...' : 'A/B: you hear the uncorrected signal.', kind: 'warn' };
    return { text: st.wet < 1000 ? 'Fading in...' : `On: ${shortOf(u)} speakers corrected.`, kind: 'ok' };
  }

  function outputsText(st, u) {
    if (!st || !st.profile) return '';
    return st.profile.channels.map(c => `${c.id} on ${outName(u, c.slot)}`).join(', ');
  }

  function refresh() {
    const st = cur(), key = P.unit + '|' + JSON.stringify(st && st.profile);
    if (key !== P.profileKey) { render(); return; }
    update();
  }

  /* Only what changes every poll: state text, switches, driver figures. */
  function update() {
    if (!P.root || !P.els) return;
    const m = P.state[P.unit], st = cur(), u = unitById(P.unit), E = P.els;
    const line = stateLine(m, st, u);
    E.state.textContent = line.text;
    E.state.dataset.kind = line.kind;
    const loaded = !!(st && st.loaded);
    setPressed(E.enable, loaded && st.enabled);
    E.enable.disabled = !loaded || P.busy === 'enable';
    setPressed(E.ab, loaded && st.enabled && st.bypass);
    E.ab.disabled = !loaded || !st.enabled || P.busy === 'bypass';
    E.abText.textContent = loaded && st.enabled ? (st.bypass ? 'Hearing: uncorrected' : 'Hearing: corrected') : 'A/B needs correction on';
    E.remove.disabled = !loaded || P.busy === 'clear';
    E.outputs.textContent = loaded ? outputsText(st, u) : 'None';
    E.driver.textContent = st && st.loaded ?
      `${st.running ? 'running' : 'stopped'}, ${(st.lastNs / 1000).toFixed(1)} µs per 1 ms tick (peak ${(st.maxNs / 1000).toFixed(1)} µs)` +
      (st.late ? `, ${st.late} frames late` : '') + (st.stored ? ', kept for the next start' : ', not stored') : '-';
  }

  function render() {
    if (!P.root) P.root = document.getElementById('corr');
    if (!P.root) return;
    syncUnit();
    const list = units(), u = unitById(P.unit), st = cur();
    P.profileKey = P.unit + '|' + JSON.stringify(st && st.profile);
    P.unitsKey = list.map(x => x.id).join(',');

    /* unit selector (two or more units) */
    const bar = el('div', { class: 'ctl-row cr-bar' }, el('span', { class: 'lbl', text: 'Unit' }),
      el('div', { class: 'seg ctl-units', role: 'group', 'aria-label': 'Unit for speaker correction' },
        list.map(x => {
          const b = el('button', { 'aria-pressed': x.id === P.unit ? 'true' : 'false', onclick: () => {
            P.unit = x.id;
            P.picked = true;
            if (P.preview) { P.preview.unit = x.id; P.preview.verdict = null; P.preview.seq = ++P.seq;
              post({ cmd: 'corr-check', unit: x.id, text: P.preview.text, seq: P.preview.seq }); }
            render();
            poll();
          } }, shortOf(x));
          return b;
        })));
    bar.hidden = list.length < 2;

    if (!u) {
      P.els = null;
      P.root.replaceChildren(bar, el('div', { class: 'panel cr-panel' },
        el('div', { class: 'panel-head' }, el('h2', { text: 'Speaker correction' })),
        el('p', { class: 'cr-empty', text: 'No Apollo runs audio on the open driver right now. Speaker correction works on a unit in full mode.' }),
        el('p', { class: 'fine', text: HW_NOTE })));
      return;
    }

    const E = P.els = {};
    E.state = el('div', { class: 'cr-state', role: 'status', 'aria-live': 'polite' });
    E.enable = sw('CORRECTION', { big: true, aria: 'Speaker correction on', onClick: () => { const s = cur(); if (s && s.loaded) setEnabled(!s.enabled); } });
    E.ab = el('button', { class: 'sw sw-big cr-ab', 'aria-pressed': 'false', 'aria-label': 'A/B: listen to the uncorrected signal',
      title: 'Switch between corrected and uncorrected (20 ms crossfade, filters keep running)',
      onclick: () => { const s = cur(); if (s && s.loaded && s.enabled) setBypass(!s.bypass); } }, 'A/B');
    E.abText = el('span', { class: 'muted cr-ab-text' });
    E.remove = el('button', { class: 'btn', onclick: remove }, 'Remove');
    E.outputs = el('span', {});
    E.driver = el('span', { class: 'muted' });

    const status = el('div', { class: 'panel cr-panel' },
      el('div', { class: 'panel-head' }, el('h2', { text: `Speaker correction: ${u.model}` }),
        el('div', { class: 'panel-tools' }, el('button', { class: 'btn btn-primary', onclick: pickFile }, 'Load profile...'), E.remove)),
      el('div', { class: 'cr-controls' }, E.enable, E.ab, E.abText),
      E.state,
      el('table', { class: 'st-table st-kv cr-kv' }, el('tbody', {},
        el('tr', {}, el('td', { text: 'Profile' }), el('td', { text: st && st.profile ? st.profile.name || '(no name)' : 'None' })),
        el('tr', {}, el('td', { text: 'Applies to' }), el('td', {}, E.outputs)),
        el('tr', {}, el('td', { text: 'Driver' }), el('td', {}, E.driver)))),
      el('p', { class: 'fine cr-hw', text: HW_NOTE }));

    const parts = [bar, status];
    if (P.preview) parts.push(previewPanel(P.preview, u));
    else if (st && st.profile) {
      parts.push(el('div', { class: 'panel cr-panel' },
        el('div', { class: 'panel-head' }, el('h2', { text: st.profile.name || 'Profile' }),
          el('span', { class: 'muted', text: `coefficients at ${st.profile.rate / 1000} kHz` })),
        profileDetails({ ...st.profile, channels: st.profile.channels }, u)));
    }
    P.root.replaceChildren(...parts);
    update();
  }

  function previewPanel(p, u) {
    const head = el('div', { class: 'panel-head' }, el('h2', { text: `Profile file: ${p.file}` }),
      el('button', { class: 'btn btn-quiet', onclick: () => { P.preview = null; if (input) input.value = ''; render(); } }, 'Close'));
    const body = [];
    const v = p.verdict;
    if (!v) {
      body.push(el('p', { class: 'cr-checking', text: 'Checking the file...' }));
    } else if (!v.ok) {
      body.push(el('div', { class: 'cr-problems', role: 'alert' },
        el('p', { text: 'This profile cannot be used. Avium Measure\'s checks found:' }),
        el('ul', {}, v.problems.map(t => el('li', { text: t })))));
    }
    if (p.data && v && v.ok) {
      const prof = fromFile(p.data, v.summary);
      const pickers = channelsOf(p.data).map(c => {
        const sel = el('select', { class: 'select', 'aria-label': `Output for ${c.id}`, onchange: e => { p.slots[c.id] = Number(e.target.value); } },
          el('option', { value: '-1', text: 'Not corrected' }),
          (OUTS[u.deviceType] || []).map((n, i) => el('option', { value: String(i), text: n })));
        sel.value = String(p.slots[c.id] !== undefined ? p.slots[c.id] : -1);
        return el('label', { class: 'cr-pick' }, el('span', { class: 'cr-k', text: c.id }), sel);
      });
      const en = el('input', { type: 'checkbox', onchange: e => { p.enable = e.target.checked; } });
      en.checked = p.enable;
      body.push(el('div', { class: 'cr-apply' },
        el('div', { class: 'cr-picks' }, el('span', { class: 'lbl', text: `Outputs on the ${shortOf(u)}` }), pickers),
        el('label', { class: 'check' }, en, 'Turn correction on'),
        el('button', { class: 'btn btn-primary', disabled: P.busy === 'load' ? true : null, onclick: apply },
          P.busy === 'load' ? 'Loading...' : `Apply to the ${shortOf(u)}`)));
      if (p.error) body.push(el('p', { class: 'cr-error', role: 'alert', text: p.error }));
      if (v.designed) {
        body.push(el('p', { class: 'fine', text: `The file has no coefficients for ${v.unitRate} Hz: Apollo Control designs them from the filter settings, as the format says.` }));
      }
      body.push(profileDetails(prof, u));
    } else if (!p.data && v) {
      body.push(el('p', { class: 'fine', text: 'The file is not readable JSON.' }));
    }
    return el('div', { class: 'panel cr-panel cr-preview' }, head, ...body);
  }

  /* ---------------------------------------------------------------- */
  /* Demo (no host): one unit with a profile, switches act locally    */
  /* ---------------------------------------------------------------- */

  function rbjPeak(f0, g, q, fs) {
    const w = 2 * Math.PI * f0 / fs, A = Math.pow(10, g / 40), al = Math.sin(w) / (2 * q), c = Math.cos(w), a0 = 1 + al / A;
    return [(1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, -2 * c / a0, (1 - al / A) / a0];
  }
  function demoProfile() {
    const f = (type, freq, gain, q) => ({ type, enabled: true, freq, gain, q, coef: rbjPeak(freq, gain, q, 48000) });
    return { name: 'Studio A, listening position (demo)', id: 'demo', created: '2026-11-15T02:10:00Z', rate: 48000, preampDb: -1.5,
      summary: 'Standard, 9 spots, loopback timing. Target Studio tilt, -0.8 dB/oct around 1 kHz. Boosts up to 6 dB.',
      channels: [
        { id: 'L', slot: 0, output: 0, gainDb: 0, delayMs: 0, filters: [f('peaking', 45.3, -8.2, 5.1), f('peaking', 87.5, -5.4, 6.8), f('peaking', 2400, 1.5, 1.6)] },
        { id: 'R', slot: 1, output: 1, gainDb: -0.36, delayMs: 0.42, filters: [f('peaking', 38.6, -9.5, 8.2), f('peaking', 71, -6, 4)] },
      ] };
  }
  function demoHost(c) {
    const id = c.unit;
    if (!id) return;
    const d = P.demo[id] || (P.demo[id] = { loaded: false, enabled: false, bypass: false, profile: null });
    if (c.cmd === 'corr-check') {
      let ok = false, problems = ['Not JSON'];
      try { const j = JSON.parse(c.text); ok = j && j.format === 'avium-correction-profile'; problems = ok ? [] : ['format is not avium-correction-profile']; } catch (e) { /* not JSON */ }
      setTimeout(() => onHost({ type: 'corr-check', unit: id, seq: c.seq, ok, problems, unitRate: 48000, designed: false,
        summary: 'Demo: Apollo Control checks every rule of the format when it runs with the driver.' }), 0);
      return;
    }
    if (c.cmd === 'corr-load') {
      const prof = fromFile(JSON.parse(c.text), '');
      prof.channels.forEach(ch => { ch.slot = c.slots[ch.id] !== undefined ? c.slots[ch.id] : -1; });
      prof.channels = prof.channels.filter(ch => ch.slot >= 0);
      Object.assign(d, { loaded: true, profile: prof, enabled: d.enabled || c.enable, bypass: false });
      setTimeout(() => onHost({ type: 'corr-result', unit: id, op: 'load', ok: true, error: 0, detail: '' }), 0);
    } else if (c.cmd === 'corr-set') {
      if (c.op === 'enable') d.enabled = !!c.value && d.loaded;
      if (c.op === 'bypass') d.bypass = !!c.value;
      if (c.op === 'clear') Object.assign(d, { loaded: false, enabled: false, bypass: false, profile: null });
      setTimeout(() => onHost({ type: 'corr-result', unit: id, op: c.op, ok: true, error: 0, detail: '' }), 0);
    }
    const state = { loaded: d.loaded, enabled: d.enabled, bypass: d.bypass, running: d.enabled, rateMismatch: false, stored: d.loaded,
      slotRange: false, unitIdle: false, unitRate: 48000, wet: d.enabled && !d.bypass ? 1000 : 0, swap: 1000, runs: 0, frames: 0,
      late: 0, lastNs: d.enabled ? 5200 : 0, maxNs: d.enabled ? 8100 : 0, profile: d.profile };
    setTimeout(() => onHost({ type: 'corr', unit: id, present: true, state, error: 0, detail: '' }), 0);
  }
  function demoSeed() {
    const tw = units().find(u => u.deviceType === TWINX);
    if (tw && !P.demo[tw.id]) P.demo[tw.id] = { loaded: true, enabled: true, bypass: false, profile: demoProfile() };
  }

  /* ---------------------------------------------------------------- */

  function show() {
    if (S.demo || !host) demoSeed();
    syncUnit();
    render();
    poll();
    startPolling();
  }

  return { show, onHost, render, openText };
})();
