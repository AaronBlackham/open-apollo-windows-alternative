'use strict';
/*
 * Units & routing (Apollo Control): the x8p, the Twin X and later units as one combined device.
 * Design: docs/spec-twinx.md 6.3, 6.6-6.9. Message contract: docs/apollo-control-units.md.
 *
 * Per-unit state belongs to app.js: S.unitList (the host's "units", normalised) and S.units[slot]
 * (status info, readback words, live). app.js handles every host message, then calls in here:
 * onUnits(previous list) for the connect/disconnect notices and the combined device's draft,
 * onAgg(m) for the driver's configuration. This file owns the Units & routing view, the agg-get /
 * agg-set exchange and the demo's units (which it lists through app.js setUnitList). Loaded before
 * app.js; it uses app.js globals (S, el, put, setHidden, thead, clone, kHz, toast, dialog, host, ...)
 * only inside functions, which run after app.js has booted. app.js calls in at the hooks marked "units.js".
 */
const Units = (() => {
  /* ---------------------------------------------------------------- */
  /* Models                                                           */
  /* ---------------------------------------------------------------- */

  const X8P = 0x22, TWINX = 0x99;
  const SHORT = { [X8P]: 'x8p', [TWINX]: 'TwinX' };
  /* Latency figures from spec 6.3.5, frames at 48 kHz: ADC per model, capture guard, render lead,
     FRAME_CFG, resampler look-ahead (Default quality) and its jitter margin. */
  const ADC_FRAMES = { [X8P]: 10, [TWINX]: 5 };
  const GUARD = 16, LEAD = 144, FRAME_CFG = 8, ASRC_H = 16, ASRC_M = 16;
  const PRESETS = [['studio', 'Studio'], ['mirror', 'Mirror'], ['phones', 'Headphones split'], ['custom', 'Custom']];
  const SCENARIOS = [['x8p', 'x8p only'], ['both', 'x8p + Twin X'], ['unplugged', 'Twin X unplugged'], ['observe', 'Twin X observe']];
  const AGG_GET_WAIT_MS = 5000, AGG_SET_WAIT_MS = 60000;
  const EP_RUNNING = 0x10;

  const nums = n => Array.from({ length: n }, (_, i) => String(i + 1));
  const run = (prefix, type, labels) => labels.map((l, i) => ({ name: `${prefix} ${l}`, id: (type << 8) | (i + 1) }));

  /* x8p playback slots in descriptor order (the ASIO DLL's table, confirmed on hardware). */
  const X8P_OUT = [
    ...run('MON', 0x09, ['L', 'R']), ...run('LINE', 0x03, nums(8)), ...run('ADAT', 0x04, nums(8)),
    ...run('S/PDIF', 0x05, ['L', 'R']), ...run('VIRTUAL', 0x07, nums(8)),
    ...run('CUE', 0x0A, ['1 L', '1 R', '2 L', '2 R', '3 L', '3 R', '4 L', '4 R']),
  ];
  /* Twin X: the expected order of spec 3.2, until the unit reports its descriptor. */
  const TWINX_IN = ['MIC/LINE/HIZ 1', 'MIC/LINE 2', 'MON L', 'MON R', ...nums(4).map(n => `VIRTUAL ${n}`),
    ...nums(8).map(n => `ADAT ${n}`), 'S/PDIF L', 'S/PDIF R', 'AUX 1 L', 'AUX 1 R', 'AUX 2 L', 'AUX 2 R',
    'TALKBACK 1', 'TALKBACK 2'];
  const TWINX_OUT = [
    ...run('MON', 0x09, ['L', 'R']), { name: 'LINE 3', id: 0x0A03 }, { name: 'LINE 4', id: 0x0A04 },
    ...run('VIRTUAL', 0x07, nums(4)), { name: 'HP L', id: 0x0A01 }, { name: 'HP R', id: 0x0A02 },
  ];
  /* Routing destinations: a stereo port is two playback channel IDs (type << 8 | number). */
  const PORTS = {
    [X8P]: [
      { port: 'mon', label: 'MON', ids: [0x0901, 0x0902], title: 'Monitor outputs' },
      { port: 'line1-2', label: 'LINE 1-2', ids: [0x0301, 0x0302] },
      { port: 'line3-4', label: 'LINE 3-4', ids: [0x0303, 0x0304] },
      { port: 'line5-6', label: 'LINE 5-6', ids: [0x0305, 0x0306] },
      { port: 'line7-8', label: 'LINE 7-8', ids: [0x0307, 0x0308] },
      { port: 'hp1', label: 'HP 1', ids: [0x0A01, 0x0A02], title: 'CUE 1 L/R: headphone 1 while its source is CUE 1' },
      { port: 'hp2', label: 'HP 2', ids: [0x0A03, 0x0A04], title: 'CUE 2 L/R: headphone 2 while its source is CUE 2' },
    ],
    [TWINX]: [
      { port: 'mon', label: 'MON', ids: [0x0901, 0x0902], title: 'Monitor outputs' },
      { port: 'line3-4', label: 'LINE 3/4', ids: [0x0A03, 0x0A04], title: 'CUE 2 L/R: the LINE 3/4 outputs' },
      { port: 'hp', label: 'HP', ids: [0x0A01, 0x0A02], title: 'CUE 1 L/R: the headphone output' },
    ],
  };
  const REC_NAME = { 'Mic / Line': 'MIC/LINE', ADAT: 'ADAT', 'S/PDIF': 'S/PDIF', Virtual: 'VIRTUAL', Monitor: 'MON', Aux: 'AUX' };

  /* x8p record names from the app's own slot table (REC_GROUPS, app.js), then talkback. */
  function x8pInputs() {
    const names = [];
    for (const g of REC_GROUPS) g.labels.forEach((l, i) => { names[g.from + i] = `${REC_NAME[g.name] || g.name.toUpperCase()} ${l}`; });
    return names.concat(['TALKBACK 1', 'TALKBACK 2']);
  }

  /* ---------------------------------------------------------------- */
  /* State                                                            */
  /* ---------------------------------------------------------------- */

  /* needsAdmin: AGG_SET_CONFIG is a user-level IOCTL and the driver stores the configuration itself
     (spec 6.8), so Apply needs no elevation unless a host says otherwise. */
  const freshAgg = () => ({ asked: -Infinity, received: false, supported: true, noReply: false, needsAdmin: false, elevated: false, timer: 0 });
  const U = {
    controlUnit: 0,          /* the x8p's slot, whose status app.js keeps in S.host (not S.units[0].info) */
    applied: null,           /* the configuration the driver holds (null: none saved) */
    draft: null,             /* what this page edits */
    touched: false,          /* the user changed the draft */
    agg: freshAgg(),
    applying: null,          /* { sent, pending, detail, timer } while an agg-set is out */
    error: '',
    drift: new Map(),        /* unit id -> { m: master id, v: smoothed ppm } */
    shape: '',
    timer: 0,
    demo: { on: false, scenario: 'both', saved: null, t0: 0 },
  };
  const liveCells = new Map();

  /* ---------------------------------------------------------------- */
  /* Small helpers                                                    */
  /* ---------------------------------------------------------------- */

  const rkey = r => `${r.src}>${r.unit}:${r.port}`;
  const canon = d => (d ? JSON.stringify({ ...d, routes: d.routes.map(rkey).sort() }) : 'null');
  const shortOf = r => SHORT[r.deviceType] || String(r.model || 'Unit').replace(/^Apollo\s*/, '').replace(/\s+/g, '');
  const portsOf = r => PORTS[r.deviceType] || [];
  const ms = f => `${(f / 48).toFixed(2)} ms`;
  const rateText = i => (i && i.sampleRate ? kHz(i.sampleRate) : '-');
  const roOf = u => readOnlyMode(infoOf(u.index), u);       /* app.js: '' or 'observe' / 'bring-up' */
  const modeText = u => {
    const ro = roOf(u);
    return ro ? `${ro}, read-only` : typeof u.mode === 'number' ? MODE_NAMES[u.mode] || `mode ${u.mode}` : '-';
  };
  const joins = u => !roOf(u) && (u.mode === 2 || u.mode === null);   /* only full-mode units join the combined device */

  function post(obj) {
    if (S.demo) { demoHost(obj); return; }
    if (host) host.postMessage(JSON.stringify(obj));   /* a string: the host reads strings only */
  }

  /* ---------------------------------------------------------------- */
  /* Units: who is here                                               */
  /* ---------------------------------------------------------------- */

  /* An older host sends no "units": show the unit it does report, from its status. */
  function fallbackUnit(i) {
    return {
      id: i.serial ? `SN${i.serial}` : 'unit0', index: U.controlUnit, model: DEVICE_NAMES[i.deviceType] || 'Apollo',
      deviceType: i.deviceType, serial: i.serial || '', fw: fwText(i), mode: null, observe: false, present: true,
      active: (i.flags & FLAG.TRANSPORT) !== 0, fallback: true,
    };
  }

  /* Present units, from app.js's S.unitList (normalised there). */
  function listOf(list) {
    if (list) return list.filter(u => u.present);
    const i = info();
    return i ? [fallbackUnit(i)] : [];
  }
  const present = () => listOf(S.unitList);
  const presentById = id => present().find(u => u.id === id) || null;
  const infoOf = idx => (idx === U.controlUnit ? info() : (S.units[idx] || {}).info || null);

  /* app.js hook: S.unitList changed (prev: the list before, null if there was none). */
  function onUnits(prev) {
    const before = new Map(listOf(prev).map(u => [u.id, u]));
    const after = present();
    if (prev) {
      for (const u of after) if (!before.has(u.id)) toast(`${u.model} connected`, 'info', 3500);
      for (const [id, u] of before) if (!after.some(x => x.id === id)) toast(`${u.model} disconnected`, 'info', 3500);
    }
    for (const id of [...U.drift.keys()]) if (!after.some(u => u.id === id)) U.drift.delete(id);
    ensureTimer();
    if (!U.agg.received) askAgg();
    ensureDraft();
    render();
  }

  /* ---------------------------------------------------------------- */
  /* Clock and live figures                                           */
  /* ---------------------------------------------------------------- */

  function fps(idx) {
    const L = S.units[idx] && S.units[idx].live;
    const f = L && L.engine && L.engine.framesPerSec;
    return typeof f === 'number' && f > 0 ? f : null;
  }

  /* The unit whose clock the device follows now: the configured master, else the first running unit. */
  function runningMaster() {
    const d = U.applied || U.draft;
    if (!d) return null;
    const act = d.units.map(r => presentById(r.id)).filter(u => u && u.active);
    return act.find(u => u.id === d.master) || act[0] || null;
  }

  function lockOf(u) {
    if (!u.active) return ['-', '', 'Not running audio'];
    if (S.demo) return ['locked', 'ok', 'Demo'];
    const st = S.units[u.index];
    const w = st && st.rbValid ? st.words : null;
    if (!w) return ['-', '', 'No readback from the unit'];
    if (!bit(w[6], 30)) return ['-', '', 'The unit does not report clock lock (readback word 6 bit 30 clear)'];
    return bit(w[6], 31) ? ['locked', 'ok', 'Readback word 6 bit 31'] : ['NOT LOCKED', 'bad', 'Readback word 6 bit 31 clear: no clock lock'];
  }

  function sampleDrift() {
    const m = runningMaster();
    for (const u of present()) {
      const f = fps(u.index), fm = m && fps(m.index);
      if (!m || u.id === m.id || !f || !fm) { U.drift.delete(u.id); continue; }
      const ppm = (f / fm - 1) * 1e6;
      const p = U.drift.get(u.id);
      U.drift.set(u.id, { m: m.id, v: p && p.m === m.id ? p.v + 0.3 * (ppm - p.v) : ppm });
    }
  }

  function asioClient() {
    for (const u of present()) {
      const e = S.units[u.index] && S.units[u.index].live && S.units[u.index].live.engine;
      if (e && e.asioState) return e;
    }
    return null;
  }
  function runningStreams() {
    let n = 0;
    for (const u of present()) {
      const ep = S.units[u.index] && S.units[u.index].live && S.units[u.index].live.endpoints;
      if (ep && Array.isArray(ep.rows)) n += ep.rows.filter(r => r.flags & EP_RUNNING).length;
    }
    return n;
  }

  function ensureTimer() { if (!U.timer) U.timer = setInterval(tick, 1000); }
  function tick() {
    if (S.demo && U.demo.on && present().some(u => u.id === DEMO_TWX.id && u.active)) onLive(demoTwinLive());
    sampleDrift();
    renderLive();
  }

  /* ---------------------------------------------------------------- */
  /* Layout: the combined channel list                                */
  /* ---------------------------------------------------------------- */

  /* A unit's channels: reported by the host (descriptor), else the model's table. */
  function layoutOf(r) {
    const live = presentById(r.id);
    const src = live || r;
    if (Array.isArray(src.inputs) && Array.isArray(src.outputs)) {
      return {
        ins: src.inputs.map(c => (typeof c === 'string' ? c : String(c.name))),
        outs: src.outputs.map(c => (typeof c === 'string' ? { name: c, id: null } : { name: String(c.name), id: Number(c.id) })),
        expected: false,
      };
    }
    const i = live ? infoOf(live.index) : null;
    if (r.deviceType === X8P) {
      return { ins: x8pInputs().slice(0, (i && i.recChannels) || 32), outs: X8P_OUT.slice(0, (i && i.playChannels) || 34), expected: false };
    }
    if (r.deviceType === TWINX) return { ins: TWINX_IN, outs: TWINX_OUT, expected: true };
    return { ins: [], outs: [], expected: true };
  }

  function combined(d) {
    const L = new Map(d.units.map(r => [r.id, layoutOf(r)]));
    const make = dir => {
      const all = [];
      for (const r of d.units) (dir === 'in' ? L.get(r.id).ins : L.get(r.id).outs).forEach((_, slot) => all.push({ unit: r.id, slot }));
      let entries;
      if (d.channels && Array.isArray(d.channels[dir])) {
        const key = e => `${e.unit}/${e.slot}`;
        const exists = new Set(all.map(key));
        entries = d.channels[dir].filter(e => exists.has(key(e))).map(e => ({ unit: e.unit, slot: e.slot, hidden: !!e.hidden }));
        const have = new Set(entries.map(key));
        for (const e of all) {
          if (have.has(key(e))) continue;
          let at = -1;
          entries.forEach((x, i) => { if (x.unit === e.unit) at = i; });
          entries.splice(at < 0 ? entries.length : at + 1, 0, { ...e, hidden: false });
        }
      } else {
        entries = all.map(e => ({ ...e, hidden: false }));
      }
      let n = 0;
      return entries.map(e => {
        const r = d.units.find(u => u.id === e.unit);
        const lay = L.get(e.unit);
        const ch = dir === 'in' ? { name: lay.ins[e.slot], id: null } : lay.outs[e.slot];
        return {
          ...e, n: e.hidden ? 0 : ++n, name: ch.name, id: ch.id, full: `${shortOf(r)} ${ch.name}`,
          model: r.model, present: !!presentById(e.unit), expected: lay.expected,
        };
      });
    };
    return { ins: make('in'), outs: make('out') };
  }

  /* Routing sources: DAW mains (ASIO out 1-2), Windows audio, then every ASIO output pair that
     belongs to one unit port, keyed by that port so it survives reordering. */
  function sourceRows(d, C) {
    const vis = C.outs.filter(c => !c.hidden);
    const mains = vis.slice(0, 2);
    const portOfPair = (a, b) => {
      if (!a || !b || a.unit !== b.unit) return null;
      const r = d.units.find(u => u.id === a.unit);
      const p = portsOf(r).find(x => x.ids[0] === a.id && x.ids[1] === b.id);
      return p ? { unit: r.id, port: p.port, text: `${shortOf(r)} ${p.label}` } : null;
    };
    const numText = cs => (cs.some(c => !c || c.hidden) ? 'hidden' : cs[1].n === cs[0].n + 1 ? `${cs[0].n}-${cs[1].n}` : `${cs[0].n}, ${cs[1].n}`);
    const mh = portOfPair(mains[0], mains[1]);
    const rows = [
      {
        src: 'daw', label: 'DAW mains', home: mh,
        sub: mains.length === 2 ? `ASIO out ${numText(mains)} · ${mh ? mh.text : `${mains[0].full}, ${mains[1].full}`}` : 'ASIO out 1-2',
      },
      { src: 'windows', label: 'Windows audio', sub: 'Monitor endpoint', home: null },
    ];
    for (const r of d.units) {
      for (const p of portsOf(r)) {
        if (mh && mh.unit === r.id && mh.port === p.port) continue;
        const cs = p.ids.map(id => C.outs.find(c => c.unit === r.id && c.id === id));
        if (cs.some(c => !c)) continue;
        rows.push({
          src: `${r.id}:${p.port}`, label: `ASIO out ${numText(cs)}`, sub: `${shortOf(r)} ${p.label}`,
          home: { unit: r.id, port: p.port }, hidden: cs.some(c => c.hidden), present: !!presentById(r.id),
        });
      }
    }
    return rows;
  }
  const srcSet = d => new Set(sourceRows(d, combined(d)).map(r => r.src));

  /* ---------------------------------------------------------------- */
  /* Configuration                                                    */
  /* ---------------------------------------------------------------- */

  const unitRef = u => ({ id: u.id, model: u.model, deviceType: u.deviceType, serial: u.serial });

  function normConfig(c) {
    if (!c || typeof c !== 'object') return null;
    const units = Array.isArray(c.units) ? c.units.filter(u => u && u.id !== undefined).map(u => ({
      id: String(u.id), model: String(u.model || DEVICE_NAMES[u.deviceType] || 'Apollo'),
      deviceType: Number(u.deviceType) || 0, serial: String(u.serial || ''),
    })) : [];
    const chans = list => list.filter(e => e && e.unit !== undefined).map(e => ({ unit: String(e.unit), slot: e.slot | 0, hidden: !!e.hidden }));
    const channels = c.channels && Array.isArray(c.channels.in) && Array.isArray(c.channels.out)
      ? { in: chans(c.channels.in), out: chans(c.channels.out) } : null;
    const routes = Array.isArray(c.routes) ? c.routes
      .filter(r => r && typeof r.src === 'string' && r.unit !== undefined && typeof r.port === 'string')
      .map(r => ({ src: r.src, unit: String(r.unit), port: r.port, ids: Array.isArray(r.ids) ? r.ids.map(Number) : [] })) : [];
    return {
      ...c, version: 1, units, master: c.master === undefined || c.master === null ? null : String(c.master), channels,
      preset: PRESETS.some(p => p[0] === c.preset) ? c.preset : 'custom', routes,
    };
  }

  function route(d, src, unit, port) {
    const r = d.units.find(u => u.id === unit);
    const p = r && portsOf(r).find(x => x.port === port);
    return p ? { src, unit, port, ids: p.ids.slice() } : null;
  }
  const hasRoute = (d, src, unit, port) => d.routes.some(r => r.src === src && r.unit === unit && r.port === port);
  function addRoute(d, src, unit, port) {
    const r = route(d, src, unit, port);
    if (r && !hasRoute(d, src, unit, port)) d.routes.push(r);
  }

  /* The unit whose MON is the main listening position: the Twin X when configured. */
  function speakerUnit(d) {
    const tx = d.units.find(u => u.deviceType === TWINX);
    return (tx || d.units.find(u => u.id === d.master) || d.units[0] || {}).id;
  }
  function defaultMaster(d) {
    const here = d.units.filter(r => presentById(r.id));
    return ((here.find(r => r.deviceType === TWINX)) || here[0] || d.units[0] || {}).id || null;
  }

  /* Presets are routing matrices (spec 6.8): every other pair keeps its own output. */
  function presetRoutes(name, d) {
    const out = { units: d.units, routes: [] };
    for (const r of sourceRows(d, combined(d))) {
      if (r.src !== 'daw' && r.src !== 'windows' && r.home) addRoute(out, r.src, r.home.unit, r.home.port);
    }
    const sp = speakerUnit(d);
    const sr = d.units.find(u => u.id === sp);
    if (name === 'studio') { addRoute(out, 'daw', sp, 'mon'); addRoute(out, 'windows', sp, 'mon'); }
    if (name === 'mirror') for (const u of d.units) { addRoute(out, 'daw', u.id, 'mon'); addRoute(out, 'windows', u.id, 'mon'); }
    if (name === 'phones') {
      addRoute(out, 'daw', sp, 'mon');
      addRoute(out, 'windows', sp, sr && sr.deviceType === X8P ? 'hp1' : 'hp');
    }
    return out.routes;
  }

  function detectPreset(d) {
    const key = rs => rs.map(rkey).sort().join('|');
    const k = key(d.routes);
    const hit = PRESETS.find(([p]) => p !== 'custom' && key(presetRoutes(p, d)) === k);
    return hit ? hit[0] : 'custom';
  }

  /* After the units or the order change: presets are recomputed; custom routing keeps what still
     exists and gives a new pair its own output. */
  function reconcile(d, prev) {
    if (d.preset !== 'custom') { d.routes = presetRoutes(d.preset, d); return; }
    const rows = sourceRows(d, combined(d));
    const srcs = new Set(rows.map(r => r.src));
    const ids = new Set(d.units.map(u => u.id));
    d.routes = d.routes.filter(r => srcs.has(r.src) && ids.has(r.unit));
    for (const r of rows) if (r.home && r.src !== 'daw' && !prev.has(r.src)) addRoute(d, r.src, r.home.unit, r.home.port);
  }

  function defaultDraft() {
    const here = present().filter(joins);
    const tx = here.find(u => u.deviceType === TWINX);
    const order = tx ? [tx, ...here.filter(u => u !== tx)] : here;      /* master first (spec 6.6) */
    const d = { version: 1, units: order.map(unitRef), master: (tx || here[0] || {}).id || null, channels: null, preset: 'studio', routes: [] };
    d.routes = presetRoutes('studio', d);
    return d;
  }

  /* The draft follows the driver's configuration; a full-mode unit it does not know joins at the
     end (channel numbers of the others stay put), and a first Twin X becomes the master. */
  function ensureDraft() {
    if (!U.draft || (!U.applied && !U.touched)) U.draft = U.applied ? clone(U.applied) : defaultDraft();
    const d = U.draft;
    const prev = srcSet(d);
    let added = false;
    for (const u of present()) {
      if (d.units.some(r => r.id === u.id) || !joins(u)) continue;
      if (u.deviceType === TWINX && !d.units.some(r => r.deviceType === TWINX)) d.master = u.id;
      d.units.push(unitRef(u));
      added = true;
    }
    if (!d.master || !d.units.some(r => r.id === d.master)) d.master = defaultMaster(d);
    if (added) reconcile(d, prev);
  }

  function issues(d, C) {
    const out = [];
    const bad = t => out.push(['bad', t]), warn = t => out.push(['warn', t]);
    if (!d.units.length) { bad('No units to combine.'); return out; }
    if (!d.master || !d.units.some(u => u.id === d.master)) bad('Choose a clock master.');
    if (!C.ins.some(c => !c.hidden)) bad('Every input channel is hidden: ASIO would have no inputs.');
    if (!C.outs.some(c => !c.hidden)) bad('Every output channel is hidden: ASIO would have no outputs.');
    if (!d.routes.length) { bad('Nothing is routed to any output: the DAW and Windows would be silent.'); return out; }
    for (const [src, label, go, reach] of [['daw', 'DAW mains (ASIO out 1-2)', 'go', 'reach'], ['windows', 'Windows audio', 'goes', 'reaches']]) {
      const dest = [...new Set(d.routes.filter(r => r.src === src).map(r => r.unit))];
      if (!dest.length) { warn(`${label} ${go} nowhere.`); continue; }
      const gone = dest.filter(id => !presentById(id));
      if (gone.length === dest.length) {
        const names = gone.map(id => (d.units.find(u => u.id === id) || { model: id }).model).join(' and ');
        warn(`${label} only ${reach} ${names}, not connected: silent until it returns.`);
      }
    }
    for (const id of new Set(d.routes.map(r => r.unit))) {
      const u = presentById(id);
      const ro = u ? roOf(u) : '';
      if (ro) warn(`${u.model} is in ${ro} mode: it plays nothing until its Mode is 2 (full).`);
    }
    return out;
  }

  function changes() {
    const a = U.applied, d = U.draft;
    if (!d || !d.units.length) return [];
    if (!a) return [{ text: 'not saved in the driver yet', reset: true }];
    const out = [];
    const name = id => { const r = d.units.find(u => u.id === id) || a.units.find(u => u.id === id); return r ? r.model : 'none'; };
    for (const u of a.units) if (!d.units.some(x => x.id === u.id)) out.push({ text: `forget ${u.model}`, reset: true });
    for (const u of d.units) if (!a.units.some(x => x.id === u.id)) out.push({ text: `add ${u.model}`, reset: true });
    if (a.master !== d.master) out.push({ text: `clock master ${name(a.master)} → ${name(d.master)}`, reset: true, master: true });
    const both = x => x.units.map(u => u.id).filter(id => a.units.some(u => u.id === id) && d.units.some(u => u.id === id));
    if (both(a).join() !== both(d).join() || JSON.stringify(a.channels) !== JSON.stringify(d.channels)) {
      out.push({ text: 'channel order', reset: true });
    }
    if (canon({ routes: a.routes }) !== canon({ routes: d.routes }) || a.preset !== d.preset) {
      const pn = p => (PRESETS.find(x => x[0] === p) || ['', 'Custom'])[1];
      out.push({ text: a.preset !== d.preset ? `routing ${pn(a.preset)} → ${pn(d.preset)}` : 'routing', reset: false });
    }
    return out;
  }

  /* ---------------------------------------------------------------- */
  /* Host: agg-get / agg-set                                          */
  /* ---------------------------------------------------------------- */

  function askAgg() {
    if (S.demo) { demoHost({ cmd: 'agg-get' }); return; }
    if (!host || now() - U.agg.asked < 3000) return;
    U.agg.asked = now();
    post({ cmd: 'agg-get' });
    clearTimeout(U.agg.timer);
    U.agg.timer = setTimeout(() => { if (!U.agg.received) { U.agg.noReply = true; render(); } }, AGG_GET_WAIT_MS);
  }

  function errText(e) {
    if (!e) return '';
    if (typeof e === 'string') return e;
    const c = Number(e.code), detail = e.detail ? String(e.detail) : '';
    return c === 1223 ? 'the administrator prompt was cancelled'
      : c === 5 ? 'the driver denied access'
      : c === 87 ? `the driver rejected the configuration${detail ? `: ${detail}` : ''}`
      : c === 1 || c === 50 ? 'this driver build has no combined device yet (AGG_SET_CONFIG, spec 6.9)'
      : c === 170 ? 'the driver is busy, try again in a moment'
      : detail || `error ${c}`;
  }

  function onAgg(m) {
    U.agg.received = true;
    U.agg.noReply = false;
    clearTimeout(U.agg.timer);
    if (m.needsAdmin !== undefined) U.agg.needsAdmin = m.needsAdmin === true;     /* absent: unchanged */
    if (m.elevated !== undefined) U.agg.elevated = !!m.elevated;
    const incoming = m.config === undefined ? undefined : normConfig(m.config);
    const code = m.error && typeof m.error === 'object' ? Number(m.error.code) : 0;
    if (!incoming && (code === 1 || code === 50)) U.agg.supported = false;
    else if (incoming) U.agg.supported = true;

    if (U.applying) {
      if (m.pending) {
        U.applying.pending = true;
        U.applying.detail = typeof m.detail === 'string' ? m.detail : '';
        render();
        return;
      }
      const sent = U.applying.sent;
      clearTimeout(U.applying.timer);
      U.applying = null;
      if (m.error) {
        U.error = errText(m.error);
        if (incoming) U.applied = incoming;
      } else {
        U.error = '';
        U.applied = incoming || normConfig(JSON.parse(sent));
        if (JSON.stringify(U.draft) === sent) { U.draft = clone(U.applied); U.touched = false; }   /* no edits meanwhile */
        toast('Combined device updated.');
      }
      ensureDraft();
      render();
      return;
    }
    if (incoming !== undefined) {
      const clean = !U.touched || canon(U.draft) === canon(U.applied);
      U.applied = incoming;
      if (clean) { U.draft = null; U.touched = false; }
    }
    ensureDraft();
    render();
  }

  async function applyNow() {
    const d = U.draft;
    if (!d || U.applying) return;
    const ch = changes();
    if (!ch.length || issues(d, combined(d)).some(i => i[0] === 'bad')) return;
    const e = asioClient();
    if (ch.some(c => c.reset) && e) {
      const ok = await dialog({
        title: 'Apply and reset the ASIO client?',
        body: `An ASIO client is ${e.asioState === 2 ? 'streaming' : 'open'} (${e.asioFrames} frames). Applying resets it: the DAW stops audio and reloads the driver.` +
          (ch.some(c => c.master) ? ' Windows streams get one short gap.' : ''),
        confirm: 'Apply',
      });
      if (!ok) return;
    }
    const sent = JSON.stringify(U.draft);
    U.error = '';
    U.applying = { sent, pending: false, detail: '', timer: 0 };
    U.applying.timer = setTimeout(() => {
      if (!U.applying) return;
      U.applying = null;
      U.error = `no answer from the host after ${AGG_SET_WAIT_MS / 1000} s`;
      render();
    }, AGG_SET_WAIT_MS);
    post({ cmd: 'agg-set', config: JSON.parse(sent) });
    render();
  }

  function revert() {
    U.draft = U.applied ? clone(U.applied) : null;
    U.touched = false;
    U.error = '';
    ensureDraft();
    render();
  }

  /* ---------------------------------------------------------------- */
  /* Edits                                                            */
  /* ---------------------------------------------------------------- */

  function edit(fn) {
    const d = U.draft;
    const prev = srcSet(d);
    fn(d);
    reconcile(d, prev);
    U.touched = true;
    render();
  }

  const setMaster = id => edit(d => { d.master = id; });

  function moveUnit(i, dir) {
    edit(d => { const j = i + dir; if (j < 0 || j >= d.units.length) return; [d.units[i], d.units[j]] = [d.units[j], d.units[i]]; });
  }

  async function setAdvanced(on) {
    const d = U.draft;
    if (on === !!d.channels) return;
    if (on) {
      const C = combined(d);
      edit(x => { x.channels = { in: C.ins.map(c => ({ unit: c.unit, slot: c.slot, hidden: false })), out: C.outs.map(c => ({ unit: c.unit, slot: c.slot, hidden: false })) }; });
      return;
    }
    const plain = combined({ ...d, channels: null });
    const C = combined(d);
    const same = dir => JSON.stringify(C[dir].map(c => [c.unit, c.slot, c.hidden])) === JSON.stringify(plain[dir].map(c => [c.unit, c.slot, c.hidden]));
    if (!(same('ins') && same('outs'))) {
      const ok = await dialog({ title: 'Back to simple order?', body: 'Per-channel order and hidden channels go back to whole unit blocks, every channel shown.', confirm: 'Use simple order' });
      if (!ok) { render(); return; }
    }
    edit(x => { x.channels = null; });
  }

  /* Advanced edits act on the reconciled list, so channels a unit reported later are included. */
  function editChannels(dir, fn) {
    edit(d => {
      const C = combined(d);
      const key = dir === 'in' ? 'ins' : 'outs';
      d.channels = {
        in: C.ins.map(c => ({ unit: c.unit, slot: c.slot, hidden: c.hidden })),
        out: C.outs.map(c => ({ unit: c.unit, slot: c.slot, hidden: c.hidden })),
      };
      fn(d.channels[dir], C[key]);
    });
  }
  const moveChannel = (dir, i, step) => editChannels(dir, list => {
    const j = i + step;
    if (j >= 0 && j < list.length) [list[i], list[j]] = [list[j], list[i]];
  });
  const toggleHidden = (dir, i) => editChannels(dir, list => { list[i].hidden = !list[i].hidden; });

  function toggleRoute(src, unit, port) {
    const d = U.draft;
    if (hasRoute(d, src, unit, port)) d.routes = d.routes.filter(r => !(r.src === src && r.unit === unit && r.port === port));
    else addRoute(d, src, unit, port);
    d.preset = detectPreset(d);
    U.touched = true;
    render();
  }

  function setPreset(p) {
    const d = U.draft;
    if (p !== 'custom') d.routes = presetRoutes(p, d);
    d.preset = p;
    U.touched = true;
    render();
  }

  async function forget(id) {
    const d = U.draft;
    const r = d.units.find(u => u.id === id);
    if (!r) return;
    const ok = await dialog({
      title: `Forget ${r.model}?`,
      body: `Its place in the channel order and its routes are removed when you Apply${r.serial ? ` (serial ${r.serial})` : ''}. ` +
        'Nothing on the unit changes. Plugged in again, it joins as a new unit.',
      confirm: 'Forget', kind: 'danger',
    });
    if (!ok) return;
    edit(x => {
      x.units = x.units.filter(u => u.id !== id);
      if (x.channels) for (const dir of ['in', 'out']) x.channels[dir] = x.channels[dir].filter(e => e.unit !== id);
      x.routes = x.routes.filter(rt => rt.unit !== id && !rt.src.startsWith(`${id}:`));
      if (x.master === id) x.master = defaultMaster(x);
    });
  }

  /* ---------------------------------------------------------------- */
  /* View                                                             */
  /* ---------------------------------------------------------------- */

  function shapeKey() {
    return JSON.stringify(present().map(u => {
      const i = infoOf(u.index) || {};
      return [u.id, u.index, u.mode, u.active, i.recChannels, i.playChannels, i.sampleRate];
    }));
  }

  function render() {
    if (S.view !== 'units') return;
    const root = document.getElementById('ur');
    if (!root) return;
    ensureTimer();
    if (!U.agg.received) askAgg();
    ensureDraft();
    const view = document.getElementById('view-units');
    const keep = view.scrollTop;
    const a = document.activeElement;
    const fkey = a && a.dataset ? a.dataset.fkey : '';
    liveCells.clear();
    const d = U.draft, C = combined(d);
    const bar = applyBar(d, C);
    root.replaceChildren(unitsPanel(d), clockPanel(d), Clock.linkPanel(), orderPanel(d, C), routingPanel(d, C), bar);   /* [Clock] the monitor link (clockui.js) */
    document.body.style.setProperty('--ur-bar-h', `${bar.offsetHeight}px`);
    view.scrollTop = keep;
    if (fkey) { const f = root.querySelector(`[data-fkey="${CSS.escape(fkey)}"]`); if (f) f.focus(); }
    U.shape = shapeKey();
    renderLive();
  }

  /* Once a second: clock, frames/s and drift in place, so hover and focus hold. */
  function renderLive() {
    if (S.view !== 'units' || !document.getElementById('ur')) return;
    if (shapeKey() !== U.shape) { render(); return; }
    const m = runningMaster();
    for (const u of present()) {
      const c = liveCells.get(u.id);
      if (!c) continue;
      const [lt, ld, ltitle] = lockOf(u);
      setHidden(c.clockDot, !ld);
      if (ld) put(c.clockDot, '', `dot ${ld}`);
      put(c.clockText, lt, ld === 'bad' ? 'bad-text' : ld ? '' : 'faint', ltitle);
      const f = fps(u.index);
      put(c.fps, f ? f.toFixed(2) : '-', f ? 'n' : 'n faint');
      let dt = '-', dcls = 'n faint', dtitle = '';
      if (m && u.id === m.id && u.active) { dt = 'master'; dcls = 'n dim'; }
      else if (u.active && f && m && fps(m.index)) {
        const e = U.drift.get(u.id);
        const v = e && e.m === m.id ? e.v : (f / fps(m.index) - 1) * 1e6;
        dt = `${v >= 0 ? '+' : ''}${v.toFixed(1)} ppm`;
        dcls = 'n';
        dtitle = `Against the ${m.model}, smoothed over a few seconds of frames/s`;
      }
      put(c.drift, dt, dcls, dtitle);
    }
  }

  function unitsPanel(d) {
    const here = present();
    const inCfg = id => d.units.some(r => r.id === id);
    const ordered = [...d.units.map(r => presentById(r.id)).filter(Boolean), ...here.filter(u => !inCfg(u.id))];
    const absent = d.units.filter(r => !presentById(r.id));
    const multi = d.units.length > 1;
    const m = runningMaster();
    const cfgMaster = (U.applied || d).master;      /* this list shows what runs, not the draft */
    const tbody = el('tbody');
    for (const u of ordered) {
      const tags = [];
      if (multi && inCfg(u.id) && m && m.id === u.id) {
        tags.push(u.id === cfgMaster
          ? el('span', { class: 'tag on', text: 'MASTER', title: 'Its clock is the device clock' })
          : el('span', { class: 'tag on', text: 'ACTING MASTER', title: 'The configured master is not running audio: this unit clocks the device until it does' }));
      } else if (multi && inCfg(u.id) && u.active) {
        tags.push(el('span', { class: 'tag', text: 'RESAMPLED', title: 'Drift-corrected to the master clock in the driver' }));
      }
      if (!inCfg(u.id)) tags.push(el('span', { class: 'tag', text: 'NOT COMBINED', title: 'Joins the combined device once its Mode is 2 (full)' }));
      const c = { clockDot: el('span', { class: 'dot', hidden: true }), clockText: el('span'), fps: el('td', { class: 'n' }), drift: el('td', { class: 'n' }) };
      liveCells.set(u.id, c);
      const [dot, dotTitle] = u.active ? ['ok', 'Running audio'] : ['idle', 'Connected, not running audio'];
      tbody.append(el('tr', { dataset: { unit: u.id } },
        el('td', {}, el('span', { class: `dot ${dot}`, title: dotTitle }), el('span', { class: 'ur-name', text: u.model }), ...tags),
        el('td', { class: 'num dim', title: `Unit key Units\\${u.id}`, text: u.serial || u.id }),
        el('td', { class: 'num', text: u.fw || '-' }),
        el('td', { class: roOf(u) ? 'warn-text' : '', text: modeText(u),
          title: u.mode === null && !u.observe ? 'This host does not report the unit Mode' : 'Units\\<key>\\Mode: 0 observe (identify only), 1 bring-up (transport, no endpoints or strips), 2 full. Below 2 no setting is sent to the unit.' }),
        el('td', { class: 'n', text: rateText(infoOf(u.index)) }),
        el('td', {}, c.clockDot, c.clockText),
        c.fps, c.drift, el('td')));
    }
    for (const r of absent) {
      tbody.append(el('tr', { class: 'absent', dataset: { unit: r.id } },
        el('td', {}, el('span', { class: 'dot off', title: 'Not connected' }), el('span', { class: 'ur-name', text: r.model }),
          multi && cfgMaster === r.id ? el('span', { class: 'tag', text: 'MASTER', title: 'Configured master, not connected' }) : null),
        el('td', { class: 'num', title: `Unit key Units\\${r.id}`, text: r.serial || r.id }),
        el('td', { text: '-' }),
        el('td', { text: 'not connected', title: 'Its place in the channel order and its routes are kept until you forget it' }),
        el('td', { class: 'n', text: '-' }), el('td', { text: '-' }), el('td', { class: 'n', text: '-' }), el('td', { class: 'n', text: '-' }),
        el('td', { class: 'ur-act' }, el('button', {
          class: 'btn ur-small', 'aria-label': `Forget ${r.model}${r.serial ? ` ${r.serial}` : ''}`, dataset: { fkey: `forget:${r.id}` },
          onclick: () => forget(r.id),
        }, 'Forget'))));
    }
    if (!ordered.length && !absent.length) {
      tbody.append(el('tr', {}, el('td', { colspan: '9', class: 'faint', text: 'No Apollo connected. A unit appears here when it is powered on and plugged in.' })));
    }
    const count = `${here.length} connected${absent.length ? ` · ${absent.length} saved, not connected` : ''}`;
    return el('div', { class: 'panel ur-units-panel' },
      el('div', { class: 'panel-head' }, el('h2', { text: 'Units' }),
        el('div', { class: 'ur-tools' }, el('span', { class: 'muted num', text: count }), S.demo ? demoSwitch() : null)),
      !S.unitList && host && !S.demo ? el('p', { class: 'ur-note', text: 'This host build does not report units yet: the unit shown comes from its status.' }) : null,
      el('table', { class: 'st-table ur-units' },
        thead([['Unit'], ['Serial'], ['Firmware'], ['Mode'], ['Rate', 'n'], ['Clock', null, 'Clock lock from readback word 6'],
          ['Frames/s', 'n', 'Measured from the hardware position against the PC clock'],
          ['Drift', 'n', 'Against the clock master, parts per million'], ['']]),
        tbody));
  }

  function demoSwitch() {
    const s = seg(SCENARIOS.map(x => x[1]), i => demoScenario(SCENARIOS[i][0]), 'Demo units');
    s.set(SCENARIOS.findIndex(x => x[0] === U.demo.scenario));
    return el('div', { class: 'ur-auto ur-demo', title: 'Demo: simulate plugging and unplugging the Twin X' }, el('span', { class: 'muted', text: 'Demo' }), s.node);
  }

  function clockPanel(d) {
    const choices = d.units.filter(r => presentById(r.id) || r.id === d.master);
    const radios = choices.map(r => {
      const u = presentById(r.id);
      const note = !u ? 'not connected' : !u.active ? 'not running audio' : r.deviceType === TWINX ? 'default: its MON feeds the monitors' : '';
      return el('label', { class: `ur-radio${u ? '' : ' disabled'}` },
        el('input', {
          type: 'radio', name: 'urMaster', value: r.id, checked: d.master === r.id, disabled: !u,
          dataset: { fkey: `master:${r.id}` }, onchange: () => setMaster(r.id),
        }),
        el('span', { class: 'ur-name', text: r.model }), note ? el('span', { class: 'muted', text: note }) : null);
    });
    const waiting = present().filter(u => !d.units.some(r => r.id === u.id));
    for (const u of waiting) {
      radios.push(el('label', { class: 'ur-radio disabled' }, el('input', { type: 'radio', name: 'urMaster', disabled: true }),
        el('span', { class: 'ur-name', text: u.model }), el('span', { class: 'muted', text: `${modeText(u)}: not combined` })));
    }
    const master = d.units.find(r => r.id === d.master);
    const others = d.units.filter(r => r !== master);
    const parts = [];
    if (!master) parts.push(el('p', { class: 'ur-text', text: 'Choose the unit whose clock the DAW and Windows follow.' }));
    else if (!others.length) parts.push(el('p', { class: 'ur-text', text: `One unit: the ${master.model} clocks the device and nothing is resampled.` }));
    else {
      const names = others.map(r => r.model).join(' and ');
      parts.push(el('p', { class: 'ur-text', text:
        `The ${names} ${others.length === 1 ? 'runs' : 'run'} on ${others.length === 1 ? 'its' : 'their'} own clock and ${others.length === 1 ? 'is' : 'are'} resampled to the ${master.model} in the driver ` +
        '(software drift correction over Thunderbolt, no clock cable). That adds 16 to 32 frames (0.3 to 0.7 ms at 48 kHz) on ' +
        `${others.length === 1 ? 'its' : 'their'} inputs, by resampler quality; outputs keep the master's latency. Inputs are aligned, so ASIO reports one latency for every channel.` }));
    }
    const mu = master && presentById(master.id);
    const acting = runningMaster();
    if (master && others.length && (!mu || !mu.active) && acting && acting.id !== master.id) {
      parts.push(el('p', { class: 'ur-warn', text: `${master.model} is ${mu ? 'not running audio' : 'not connected'}: the ${acting.model} is acting master until it is. When it takes over, ASIO clients reset once.` }));
    }
    if (U.applied && U.applied.master && U.applied.master !== d.master) {
      parts.push(el('p', { class: 'ur-warn', text: 'Changing the clock master resets ASIO clients: the DAW stops and reloads the driver. Windows streams get one short gap.' }));
    }
    if (master) parts.push(latencyTable(d));
    return el('div', { class: 'panel' },
      el('div', { class: 'panel-head' }, el('h2', { text: 'Clock master' })),
      el('div', { class: 'ur-clock' }, el('div', { class: 'ur-radios', role: 'radiogroup', 'aria-label': 'Clock master' }, radios), el('div', {}, parts)));
  }

  function latencyTable(d) {
    const one = d.units.length === 1;
    const rows = d.units.map(r => {
      const lead = r.id === d.master || one;
      return { r, lead, inp: (ADC_FRAMES[r.deviceType] || 10) + GUARD + (lead ? 0 : ASRC_H + ASRC_M), out: LEAD + FRAME_CFG };
    });
    const ain = Math.max(...rows.map(x => x.inp)), aout = LEAD + FRAME_CFG;
    const tbody = el('tbody', {},
      rows.map(x => el('tr', {}, el('td', { text: `${x.r.model}${one ? '' : x.lead ? ' · master' : ' · resampled'}` }),
        el('td', { class: 'n', text: String(x.inp) }), el('td', { class: 'n', text: String(x.out) }))),
      el('tr', { class: 'ur-total' }, el('td', { text: 'ASIO reports' }),
        el('td', { class: 'n', text: `${ain} · ${ms(ain)}` }), el('td', { class: 'n', text: `${aout} · ${ms(aout)}` })));
    return el('div', { class: 'ur-lat' },
      el('table', { class: 'st-table' }, thead([['Latency, frames at 48 kHz'], ['In', 'n'], ['Out', 'n']]), tbody),
      el('p', { class: 'ur-note', text: 'Without the ASIO buffer. Derived from the design (spec 6.3.5), to be measured at gate G6.' }));
  }

  function orderPanel(d, C) {
    const adv = !!d.channels;
    const mode = seg(['Simple', 'Advanced'], i => setAdvanced(i === 1), 'Channel order mode');
    mode.set(adv ? 1 : 0);
    const nIn = C.ins.filter(c => !c.hidden).length, nOut = C.outs.filter(c => !c.hidden).length;
    const expected = d.units.some(r => layoutOf(r).expected);
    return el('div', { class: 'panel' },
      el('div', { class: 'panel-head' }, el('h2', { text: 'Channel order' }),
        el('div', { class: 'ur-tools' }, el('span', { class: 'muted num', text: `${nIn} in · ${nOut} out` }), el('div', { class: 'ur-auto' }, mode.node))),
      el('p', { class: 'ur-note', text: 'The ASIO and Windows channel list of the combined device, numbered as the DAW sees it. A unit that is not connected keeps its channels: they stay silent, so DAW routings survive.' }),
      adv ? advancedOrder(C) : simpleOrder(d, C),
      expected ? el('p', { class: 'ur-note ur-foot', text: 'Italic names are the Twin X order expected from spec 3.2, until the unit reports its descriptor.' }) : null);
  }

  function simpleOrder(d, C) {
    const range = cs => { const v = cs.filter(c => !c.hidden); return v.length ? `${v[0].n}-${v[v.length - 1].n}` : '-'; };
    const blocks = el('ol', { class: 'ur-blocks', 'aria-label': 'Unit blocks, first to last' });
    d.units.forEach((r, i) => {
      const here = !!presentById(r.id);
      blocks.append(el('li', { class: here ? '' : 'absent' },
        el('span', { class: 'ur-bn num', text: String(i + 1) }),
        el('span', { class: 'ur-name', text: r.model }),
        el('span', { class: 'muted num', text: `in ${range(C.ins.filter(c => c.unit === r.id))} · out ${range(C.outs.filter(c => c.unit === r.id))}` }),
        layoutOf(r).expected ? el('span', { class: 'tag', text: 'EXPECTED', title: 'Channel names from spec 3.2 until the unit reports its descriptor' }) : null,
        here ? null : el('span', { class: 'faint', text: 'not connected: silent, keeps its place' }),
        el('span', { class: 'ur-move' },
          i > 0 ? el('button', { class: 'btn ur-small', 'aria-label': `Move ${r.model} up`, dataset: { fkey: `up:${r.id}` }, onclick: () => moveUnit(i, -1) }, 'Move up') : null,
          i < d.units.length - 1 ? el('button', { class: 'btn ur-small', 'aria-label': `Move ${r.model} down`, dataset: { fkey: `down:${r.id}` }, onclick: () => moveUnit(i, 1) }, 'Move down') : null)));
    });
    const list = (title, cs) => el('div', { class: 'ur-chans' },
      el('div', { class: 'bgroup-name', text: title }),
      el('ol', { class: 'ur-list', 'aria-label': title }, cs.filter(c => !c.hidden).map(c => el('li', {
        class: `${c.present ? '' : 'absent'}${c.expected ? ' expected' : ''}`, title: `${c.model}, slot ${c.slot + 1}${c.present ? '' : ': not connected, silent'}`,
      }, el('span', { class: 'n', text: String(c.n) }), el('span', { text: c.full })))));
    return el('div', {}, blocks, list(`Inputs · ${C.ins.length}`, C.ins), list(`Outputs · ${C.outs.length}`, C.outs));
  }

  function advancedOrder(C) {
    const table = (dir, cs) => {
      const tbody = el('tbody');
      cs.forEach((c, i) => {
        const fk = `${dir}:${c.unit}:${c.slot}`;
        tbody.append(el('tr', { class: `${c.hidden ? 'ur-hidden' : ''}${c.present ? '' : ' absent'}` },
          el('td', { class: 'n', text: c.hidden ? '-' : String(c.n) }),
          el('td', { class: c.expected ? 'expected' : '', text: c.full }),
          el('td', { class: 'n dim', text: String(c.slot + 1), title: 'Slot in the unit\'s own frame' }),
          el('td', { class: 'ur-move' },
            el('button', { class: 'ur-mini', 'aria-label': `Move ${c.full} up`, disabled: i === 0, dataset: { fkey: `mu:${fk}` }, onclick: () => moveChannel(dir, i, -1) }, '↑'),
            el('button', { class: 'ur-mini', 'aria-label': `Move ${c.full} down`, disabled: i === cs.length - 1, dataset: { fkey: `md:${fk}` }, onclick: () => moveChannel(dir, i, 1) }, '↓')),
          el('td', {}, el('input', { type: 'checkbox', checked: !c.hidden, 'aria-label': `Show ${c.full}`, dataset: { fkey: `sh:${fk}` }, onchange: () => toggleHidden(dir, i) }))));
      });
      return el('div', { class: 'ur-adv-col' },
        el('div', { class: 'bgroup-name', text: `${dir === 'in' ? 'Inputs' : 'Outputs'} · ${cs.filter(c => !c.hidden).length} shown of ${cs.length}` }),
        el('div', { class: 'ur-scroll' }, el('table', { class: 'st-table ur-adv' }, thead([['#', 'n'], ['Channel'], ['Slot', 'n'], ['Order'], ['Show']]), tbody)));
    };
    return el('div', { class: 'ur-adv-grid' }, table('in', C.ins), table('out', C.outs));
  }

  function routingPanel(d, C) {
    const rows = sourceRows(d, C);
    const pre = seg(PRESETS.map(p => p[1]), i => setPreset(PRESETS[i][0]), 'Routing preset');
    pre.set(PRESETS.findIndex(p => p[0] === d.preset));
    const units = d.units.filter(r => portsOf(r).length);
    const off = r => (presentById(r.id) ? '' : ' off');
    const state = r => {
      const u = presentById(r.id);
      return !u ? 'not connected' : roOf(u) || (r.id === d.master && units.length > 1 ? 'master' : '');
    };
    const head = el('thead', {},
      el('tr', {}, el('th', { class: 'src' }), units.map(r => el('th', { class: `grp ur-unit${off(r)}`, colspan: String(portsOf(r).length) },
        el('span', { text: r.model }), state(r) ? el('span', { class: 'ur-state', text: state(r) }) : null))),
      el('tr', {}, el('th', { class: 'src', text: 'Source' }),
        units.flatMap(r => portsOf(r).map((p, j) => el('th', { class: `${j === 0 ? 'grp' : ''}${off(r)}`, title: p.title || null, text: p.label })))));
    const body = el('tbody', {}, rows.map(row => el('tr', { class: row.present === false || row.hidden ? 'off' : '' },
      el('th', { class: 'src', scope: 'row' }, el('span', { class: 'ur-src', text: row.label }), el('span', { class: 'ur-sub', text: row.sub })),
      units.flatMap(r => portsOf(r).map((p, j) => {
        const on = hasRoute(d, row.src, r.id, p.port);
        const home = !!row.home && row.home.unit === r.id && row.home.port === p.port;
        return el('td', { class: `${j === 0 ? 'grp' : ''}${off(r)}` }, el('button', {
          class: `rt${home ? ' home' : ''}`, 'aria-pressed': on ? 'true' : 'false',
          'aria-label': `${row.label} to ${shortOf(r)} ${p.label}`,
          title: `${row.label} (${row.sub}) to ${r.model} ${p.label}${home ? ': its own output' : ''}`,
          dataset: { fkey: `rt:${row.src}>${r.id}:${p.port}` }, onclick: () => toggleRoute(row.src, r.id, p.port),
        }));
      })))));
    const iss = issues(d, C);
    return el('div', { class: 'panel' },
      el('div', { class: 'panel-head' }, el('h2', { text: 'Output routing' }), el('div', { class: 'ur-auto' }, pre.node)),
      el('p', { class: 'ur-note', text: 'Where each source plays, mixed by the driver\'s engine across units. Sources on one output add together, so the DAW and Windows share the monitors. Level, mute and dim stay on each unit\'s monitor controls.' }),
      el('div', { class: 'ur-grid-wrap' }, el('table', { class: 'ur-grid' }, head, body)),
      el('p', { class: 'ur-note ur-legend', text: 'Filled: routed. Dashed outline: the pair\'s own output.' }),
      iss.length ? el('ul', { class: 'ur-issues', 'aria-label': 'Checks' }, iss.map(([k, t]) => el('li', { dataset: { kind: k } },
        el('span', { class: `dot ${k}` }), el('span', { class: k === 'bad' ? 'bad-text' : '', text: t })))) : null);
  }

  /* Spec 6.8: AGG_SET_CONFIG is a user-level IOCTL and the driver persists the configuration
     itself. The needsAdmin branch is only for a host that reports otherwise. */
  function adminText() {
    if (S.demo) return 'Demo: nothing is written. On the device, Apply needs no administrator rights: the driver keeps the configuration itself.';
    if (!U.agg.needsAdmin) return 'No administrator rights needed: the driver keeps the configuration itself.';
    if (U.agg.elevated) return 'Running as administrator: no prompt.';
    return 'This host build says Apply needs administrator rights: Windows asks for approval.';
  }

  function impactText(ch) {
    const parts = [];
    const e = asioClient(), n = runningStreams();
    if (ch.some(c => c.reset)) {
      parts.push(e ? `Interrupts: the open ASIO client (${e.asioState === 2 ? 'streaming' : 'open'}, ${e.asioFrames} frames) resets, so the DAW stops and reloads the driver.`
        : 'ASIO clients reset; none is open now.');
    } else {
      parts.push('Routing changes at the next 1 ms tick: nothing resets.');
    }
    if (ch.some(c => c.master)) parts.push(n ? `${n} running Windows stream${n === 1 ? ' gets' : 's get'} one short gap.` : 'Windows streams get one short gap; none is running now.');
    return parts.join(' ');
  }

  function applyBar(d, C) {
    const ch = changes();
    const iss = issues(d, C);
    const blocked = iss.find(i => i[0] === 'bad');
    const busy = !!U.applying;
    const supported = U.agg.supported && !U.agg.noReply;
    const prompt = U.agg.needsAdmin && !U.agg.elevated && !S.demo;
    let state = 'clean', line;
    const sub = [];
    if (!d.units.length) { state = 'off'; line = 'No Apollo connected: nothing to configure.'; }
    else if (busy) {
      state = 'busy';
      line = U.applying.pending ? (U.applying.detail || 'Applying: the driver takes the new layout at its next tick.')
        : prompt ? 'Waiting for administrator approval and the driver.' : 'Sent: waiting for the driver.';
    } else if (U.error) { state = 'error'; line = `Not applied: ${U.error}.`; }
    else if (!supported) {
      state = 'off';
      line = U.agg.noReply ? 'No answer to agg-get: this host build has no combined device yet. Preview only.'
        : 'This driver build has no combined device yet (AGG_SET_CONFIG, spec 6.9). Preview only.';
    } else if (ch.length) { state = 'dirty'; line = `${ch.length} change${ch.length === 1 ? '' : 's'}: ${ch.map(c => c.text).join(', ')}.`; }
    else line = 'Saved in the driver. Nothing to apply.';
    if (state === 'error' && ch.length) sub.push(`Still to apply: ${ch.map(c => c.text).join(', ')}.`);
    if (ch.length && !busy && d.units.length && supported) sub.push(impactText(ch));    /* preview: nothing to interrupt */
    if (blocked && ch.length && !busy) sub.push(`Cannot apply: ${blocked[1]}`);
    sub.push(adminText());
    const dotKind = { clean: 'ok', dirty: 'warn', busy: 'warn', error: 'bad', off: 'off' }[state];
    return el('div', { class: 'ur-bar', id: 'urBar', dataset: { state }, role: 'region', 'aria-label': 'Apply changes' },
      el('div', { class: 'ur-bar-text' },
        el('div', { class: 'ur-bar-line', role: 'status' }, el('span', { class: `dot ${dotKind}` }), el('span', { text: line })),
        sub.map(t => el('div', { class: 'ur-bar-sub', text: t }))),
      el('div', { class: 'ur-bar-btns' },
        el('button', { class: 'btn', disabled: !ch.length || busy, dataset: { fkey: 'revert' }, onclick: revert }, 'Revert'),
        el('button', { class: 'btn btn-primary', disabled: !ch.length || busy || !!blocked || !supported, dataset: { fkey: 'apply' }, onclick: applyNow }, 'Apply')));
  }

  /* ---------------------------------------------------------------- */
  /* Demo: a simulated Twin X beside app.js's simulated x8p           */
  /* ---------------------------------------------------------------- */

  const DEMO_X8P = { id: 'SNDEMO2017SIM00001', index: 0, model: 'Apollo x8p', deviceType: X8P, serial: 'DEMO2017SIM00001', fw: '11.4.0', mode: 2, present: true, active: true };
  const DEMO_TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X', deviceType: TWINX, serial: 'DEMO2025SIM00002', fw: '11.4.0', mode: 2, present: true, active: true };

  function demoSaved(withTwin) {
    const d = { version: 1, units: (withTwin ? [DEMO_TWX, DEMO_X8P] : [DEMO_X8P]).map(unitRef), channels: null, preset: 'studio', routes: [] };
    d.master = d.units[0].id;
    d.routes = presetRoutes('studio', d);
    return d;
  }

  function demoTwinInfo(mode) {
    const full = mode === 2;
    return {
      version: 6, stage: full ? STAGE.CONNECTED : STAGE.MAPPED, failedStage: 0, lastStatus: 0,
      flags: full ? FLAG.FW_V2 | FLAG.IRQ64 | FLAG.RINGS_LIVE | FLAG.DSPS | FLAG.ACEFACE | FLAG.CONNECTED | FLAG.MIXER_READY |
        FLAG.TRANSPORT | FLAG.SERVICE | FLAG.READBACK | FLAG.MSI : FLAG.FW_V2,
      fpgaRev: 0, extCaps: 0x81300410, fwRelease: 0x0B040000, subsystemId: 0x19, deviceType: TWINX, publicType: 0x19,
      numDsps: 4, playChannels: 10, recChannels: 24, ringFrames: full ? 8192 : 0, sampleRate: full ? 48000 : 0,
      servicePasses: 0, interruptCount: 0, notifStatusSeen: 0, numPreamps: 2, serial: DEMO_TWX.serial,
      mode, observe: mode === 0,
    };
  }

  /* Input 1 MIC at 30 dB, input 2 LINE at 10 dB, clock locked. */
  function demoTwinWords() {
    const w = new Array(40).fill(0);
    w[0] = 1 << 6;
    w[3] = 21 | (1 << 6);
    w[6] = (1 << 30) | (1 << 31);
    w[38] = 0x00010203;
    return w.map(x => x >>> 0);
  }

  function demoTwinLive() {
    const t = (now() - U.demo.t0) / 1000;
    const jitter = s => (Math.random() - 0.5) * s;
    return {
      type: 'live', unit: DEMO_TWX.index,
      engine: {
        version: 1, running: 1, lost: 0, starts: 1, ticks: Math.round(t * 1000), lateTicks: 0, busStalls: 0, deadReads: 0,
        maxIntervalUs: 1180, asioState: 0, asioFrames: 0, asioIn: 0, asioOut: 0, asioUnderruns: 0, asioOverruns: 0, asioSkipped: 0,
        ticksPerSec: 1000 + jitter(0.4), framesPerSec: 48000.62 + jitter(0.3),
      },
      strips: {
        version: 2, state: 4, flags: 0x79, enable: 1, mask: 0x03, stepsLimit: 9, stepsDone: 9, phantomState: 1, phantomMask: 0,
        why: 'stage 9 done on StripMask 0x03, DSP 1', perInput: true, taken: [2, 2, 0, 0, 0, 0, 0, 0], started: [1, 1, 0, 0, 0, 0, 0, 0],
      },
      /* Combined: the x8p hosts the Windows endpoints, the Twin X creates none (spec 6.7). */
      endpoints: { version: 1, count: 0, enabledMask: 0, prepared: 0, layoutOk: true, rows: [] },
    };
  }

  function demoScenario(name) {
    if (!U.demo.on) return;
    if (!SCENARIOS.some(s => s[0] === name)) name = 'both';
    U.demo.scenario = name;
    const hasTwin = c => !!c && c.units.some(u => u.id === DEMO_TWX.id);
    if (name === 'x8p' || name === 'observe') U.demo.saved = demoSaved(false);
    else if (!hasTwin(U.demo.saved)) U.demo.saved = demoSaved(true);
    const twin = name === 'both' ? { ...DEMO_TWX } : name === 'observe' ? { ...DEMO_TWX, mode: 0, observe: true, active: false } : null;
    if (twin) {
      /* Its slot in app.js's state, fresh for each scenario; setUnitList keeps a listed slot. */
      const words = twin.mode === 2 ? demoTwinWords() : null;
      S.units[twin.index] = { ...newUnit(), info: demoTwinInfo(twin.mode), words, rbValid: !!words };
    }
    setUnitList(twin ? [{ ...DEMO_X8P }, twin] : [{ ...DEMO_X8P }]);     /* app.js, as the host's "units" */
    if (twin && twin.active) onLive(demoTwinLive());
    onAgg({ type: 'agg', config: clone(U.demo.saved), pending: false, error: null, needsAdmin: false });
  }

  /* The demo's answers to agg-get and agg-set, as the host and driver would give them. */
  function demoHost(cmd) {
    if (!U.demo.on) return;
    if (cmd.cmd === 'agg-get') {
      setTimeout(() => { if (U.demo.on) onAgg({ type: 'agg', config: clone(U.demo.saved), pending: false, error: null, needsAdmin: false }); }, 50);
    } else if (cmd.cmd === 'agg-set') {
      setTimeout(() => { if (U.demo.on) onAgg({ type: 'agg', pending: true, detail: 'Applying: the driver takes the new layout at its next tick (demo).' }); }, 150);
      setTimeout(() => {
        if (!U.demo.on) return;
        U.demo.saved = normConfig(cmd.config);
        onAgg({ type: 'agg', config: clone(U.demo.saved), pending: false, error: null, needsAdmin: false });
      }, 1000);
    }
  }

  /* This page's own state. The units themselves (S.unitList, S.units) are app.js's to clear. */
  function reset() {
    clearTimeout(U.agg.timer);
    if (U.applying) clearTimeout(U.applying.timer);
    U.applied = null;
    U.draft = null;
    U.touched = false;
    U.applying = null;
    U.error = '';
    U.drift.clear();
    U.agg = freshAgg();
  }

  /* app.js hook: entering or leaving demo mode (after app.js cleared the unit list). Leaving
     waits for the host's own "units". */
  function demo(on) {
    reset();
    U.demo.on = on;
    if (on) {
      U.demo.t0 = now();
      ensureTimer();
      demoScenario(new URLSearchParams(location.search).get('units') || U.demo.scenario);
    } else {
      render();
    }
  }

  return {
    onUnits, onAgg, render, demo, demoScenario,
    get state() { return U; },
  };
})();
