'use strict';
/*
 * [Sess] Sessions (docs/apollo-control-units.md 1.11): UA Console's session save / recall for everything Apollo Control
 * controls, per unit; the last-state autosave; Start with Windows; the tray's monitor volume and recent sessions.
 *
 * A session file (%APPDATA%\Avium\ApolloControl\sessions\<name>.json, app/src/session_host.h writes and lists them)
 * holds per unit, by unit id: the preamps (gain, pad, mic / line, low cut, polarity, link, 48V, Hi-Z load), the monitor
 * section (level, mute, dim, mono, dim level, source, headphone level and sources, speakers), the monitor mix, the cues,
 * the aux buses and ALT outputs (the unit's cfg.mixes entry: what does not read back), the input names, the plug-in
 * chains (plugins.json version 2, from the host's engine: session-plugins) and the sample rate; and Apollo Control's
 * own view (theme, screen, focused unit). "format":"apollo-control-session", "version":1; a newer version is refused.
 *
 * Applying goes through the app's own send paths, per unit, in this order: the rate first (rate-set, then the final
 * rate-result is waited for), the preamps (links, then each input: setInput), the mix (the stored mix replaced and sent
 * the way a connect sends it, mutes first, with UA's defaults sent for what the session leaves untouched), the monitor
 * (setMon), and last the plug-in chains (plug-session-set: the engine replaces the unit's chains and builds them once
 * its strips are Live). 48V is never turned on silently: a load that would switch phantom on asks first and names the
 * inputs (always, whatever "Ask before turning on 48V" says). A unit that is not connected keeps its part
 * (cfg.session.pending, by unit id) and gets it, in the same order and with the same question, when it connects.
 *
 * The last state is what the app keeps across restarts and replugs: cfg.mixes (monitor mix, cues, aux, ALT, monitor
 * routing), the Hi-Z load and the engine's plugins.json. It is mirrored to sessions\last-state.json a few seconds after
 * a change (loadable as "Last state"). On launch and on each unit's (re)connect, once its strips are Live, app.js
 * syncMixApply re-sends what does not read back; the preamps, 48V, monitor level, mute, dim and mono read back and stay
 * as the unit reports them.
 *
 * Views (docs/THEMES.md 2, one model, many views): the pack's renderers.js names a layout, sessions: { layout }:
 * console (Studio: a console's session menu), hud (Cyberpunk), table (Lab), terminal (Mono, with `session ...` on its
 * command line and in Ctrl+K), plain (Day), slots (Arcade: SAVE SLOTS). The actions, keys (Ctrl+S saves, Ctrl+O opens
 * the list, Esc closes) and the words for state (CURRENT, EDITED, WAITING) are the same in every layout.
 * Loaded before app.js; it uses app.js globals only inside functions, after boot.
 */
const Sess = (() => {
  const FORMAT = 'apollo-control-session', VERSION = 1;
  const DEMO_KEY = 'apollo-sessions';
  const SS = {
    list: [], last: null, dir: '', listed: false,
    root: null, btn: null, open: false, sort: { key: 'saved', dir: -1 },
    busy: '', note: null,
    waits: [], known: {}, cur: new Set(),
    base: '', autoFp: '', autoAt: -1e9, applying: 0, liveSeen: false,
    auto: { known: false, on: false, current: false, command: '', error: 0, detail: '', asked: false },
    dlg: Promise.resolve(),
  };
  const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
  const bool = v => typeof v === 'boolean';
  const clone = x => JSON.parse(JSON.stringify(x));
  const viaHost = () => !!host && !S.demo;
  const scfg = () => cfg.session;

  /* ------------------------------------------------------------ talking to the host (or the demo's storage) */

  /* one request, its answer: the first host message test(m) takes, or null after ms */
  function ask(msg, test, ms = 6000) {
    return new Promise(res => {
      const w = { test, res, t: setTimeout(() => { SS.waits = SS.waits.filter(x => x !== w); res(null); }, ms) };
      SS.waits.push(w);
      send(JSON.stringify(msg));
    });
  }

  function demoStore() {
    try { const d = JSON.parse(localStorage.getItem(DEMO_KEY) || 'null'); if (d && typeof d === 'object' && d.files) return d; } catch (e) { /* none */ }
    return { files: {}, last: null };
  }
  function demoKeep(d) { try { localStorage.setItem(DEMO_KEY, JSON.stringify(d)); } catch (e) { /* not kept */ } }
  const fileOf = name => `${String(name).replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').trim().slice(0, 80) || 'session'}.json`;
  const entryOf = (file, s) => ({
    file, name: s.name || file.replace(/\.json$/i, ''), saved: s.saved || '', version: s.version || 0, bad: '',
    units: (s.units || []).map(u => ({ id: u.id, model: u.model, deviceType: u.deviceType, rate: u.rate || 0 })),
    inserts: (s.units || []).reduce((n, u) => n + ((u.plugins && u.plugins.chains) || []).reduce((m, c) => m + (c.slots || []).length, 0), 0),
  });
  function demoList() {
    const d = demoStore();
    return { type: 'sessions', dir: 'this browser (demo)', list: Object.entries(d.files).map(([f, s]) => entryOf(f, s)).sort((a, b) => (a.saved < b.saved ? 1 : -1)),
      last: d.last ? entryOf('last-state.json', d.last) : null };
  }

  const store = {
    async list() {
      const m = viaHost() ? await ask({ cmd: 'session-list' }, x => x.type === 'sessions') : demoList();
      if (m) onList(m);
      return m;
    },
    async save(name, s) {
      if (!viaHost()) {
        const d = demoStore();
        for (const [f, x] of Object.entries(d.files)) if (x.name.toLowerCase() === name.toLowerCase()) delete d.files[f];
        d.files[fileOf(name)] = s;
        demoKeep(d);
        onList(demoList());
        return { ok: true, file: fileOf(name) };
      }
      return ask({ cmd: 'session-save', name, session: s }, x => x.type === 'session-result' && x.op === 'save');
    },
    async load(it) {
      if (!viaHost()) {
        const d = demoStore(), s = it.last ? d.last : d.files[it.file] || Object.values(d.files).find(x => x.name === it.name);
        return s ? { type: 'session', op: it.last ? 'last' : 'load', name: s.name, session: s } : { type: 'session-result', ok: false, why: 'none' };
      }
      const op = it.last ? 'last' : 'load';
      return ask(it.last ? { cmd: 'session-load', last: true } : { cmd: 'session-load', name: it.name, file: it.file || '' },
        x => (x.type === 'session' && x.op === op) || (x.type === 'session-result' && x.op === op));
    },
    async del(it) {
      if (!viaHost()) {
        const d = demoStore();
        delete d.files[it.file];
        demoKeep(d);
        onList(demoList());
        return { ok: true };
      }
      return ask({ cmd: 'session-delete', name: it.name, file: it.file || '' }, x => x.type === 'session-result' && x.op === 'delete');
    },
    autosave(s) {
      if (viaHost()) { send(JSON.stringify({ cmd: 'session-autosave', session: s })); return; }
      const d = demoStore();
      d.last = s;
      demoKeep(d);
    },
    async exportOne(name, s, file) {
      if (!viaHost()) {
        const data = s || demoStore().files[file];
        if (!data) return { ok: false, why: 'the session is not there' };
        const a = el('a', { href: URL.createObjectURL(new Blob([`${JSON.stringify(data, null, 1)}\n`], { type: 'application/json' })), download: fileOf(name) });
        document.body.append(a);
        a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
        return { ok: true, file: fileOf(name) };
      }
      return ask(s ? { cmd: 'session-export', name, session: s } : { cmd: 'session-export', name, file }, x => x.type === 'session-result' && x.op === 'export', 600000);
    },
    async importOne() {
      if (!viaHost()) {
        const f = await new Promise(res => {
          const i = el('input', { type: 'file', accept: '.json,application/json', hidden: true, onchange: e => res(e.target.files && e.target.files[0]) });
          document.body.append(i);
          i.click();
          setTimeout(() => i.remove(), 600000);
        });
        if (!f) return null;
        try { return { type: 'session', op: 'import', file: f.name, session: JSON.parse(await f.text()) }; } catch (e) { return { type: 'session-result', op: 'import', ok: false, why: 'not a JSON file' }; }
      }
      return ask({ cmd: 'session-import' }, x => (x.type === 'session' || x.type === 'session-result') && x.op === 'import', 600000);
    },
  };

  /* everything the host says about sessions, autostart and plug-in chains */
  function onHost(m) {
    for (const w of SS.waits.slice()) {
      if (w.test(m)) { clearTimeout(w.t); SS.waits = SS.waits.filter(x => x !== w); w.res(m); break; }
    }
    if (m.type === 'sessions') onList(m);
    else if (m.type === 'session' && m.op === 'last') takeLast(m.session);
    else if (m.type === 'session-result' && m.op === 'autosave' && m.ok) { SS.last = { ...(SS.last || {}), saved: new Date().toISOString(), file: 'last-state.json' }; render(); }
    else if (m.type === 'autostart') onAutostart(m);
    else if (m.type === 'plugin-result' && m.op === 'session' && !m.ok) toast(`Plug-ins of the session were not put back: ${m.detail || m.code}.`, 'warn', 8000);
  }

  function onList(m) {
    SS.list = (Array.isArray(m.list) ? m.list : []).filter(x => x && typeof x.name === 'string');
    SS.last = m.last || null;
    SS.dir = String(m.dir || SS.dir);
    SS.listed = true;
    render();
  }

  /* the last state as the host read it at start: the parts of units not connected now, for the next save */
  function takeLast(s) {
    const c = clean(s);
    if (c.error) return;
    for (const p of c.units) if (p.id && !SS.known[p.id]) SS.known[p.id] = p;
  }

  /* the host's (or the demo's) plug-in chains, plugins.json version 2: { chains: [...] }, or null */
  async function pluginDoc() {
    if (S.demo) return Plug.demoSessionDoc ? Plug.demoSessionDoc(idOf(0)) : null;
    if (!host) return null;
    const m = await ask({ cmd: 'session-plugins' }, x => x.type === 'plugin-session', 2500);   /* the host asks its engine (plug-session-get) */
    return m && m.doc && Array.isArray(m.doc.chains) ? m.doc : null;
  }

  /* ------------------------------------------------------------ the model: capture */

  function idOf(idx) {
    const e = listedUnit(idx);
    if (e && e.id) return e.id;
    return idx === 0 ? 'x8p' : '';
  }

  /* one unit as a session holds it (doc: the engine's chains, null = unknown: a load then leaves the inserts alone) */
  function partOf(idx, doc) {
    const c = ctl(idx), k = capsOf(unitType(idx)), e = listedUnit(idx) || {}, ui = unitInfo(idx) || {}, r = rateFor(idx), id = idOf(idx);
    const mix = clone(mixOf(idx));
    delete mix.mon;
    return {
      id, model: unitModel(idx), deviceType: unitType(idx), serial: String(e.serial || ui.serial || ''),
      rate: (r && r.rate) || ui.sampleRate || null,
      inputs: c.inputs.slice(0, k.preamps).map(i => ({ gain: i.gain, line: !!i.line, pad: !!i.pad, p48: !!i.p48, lowcut: !!i.lowcut, phase: !!i.phase })),
      links: c.links.slice(0, Math.ceil(k.preamps / 2)).map((on, p) => ((k.linkPairs >>> p) & 1 ? !!on : null)),
      hizImp: hizLoadOf(idx),
      monitor: { level: c.mon.level, mute: !!c.mon.mute, dim: !!c.mon.dim, mono: !!c.mon.mono, dimLevel: c.mon.dimLevel, source: c.mon.source,
        hp1: k.hpLevel ? c.mon.hp1 : null, hp1src: c.mon.hp1src, hp2src: c.mon.hp2src, alt: c.mon.alt || 0 },
      mix,
      channels: Array.from({ length: k.preamps }, (_, ch) => { const n = chanCfg(idx, ch); return { name: n.name || '', color: n.color || '', hidden: !!n.hidden }; }),
      plugins: doc ? { chains: doc.chains.filter(x => x && x.unit === id) } : null,
    };
  }

  /* the units a session takes now: present, of a known model, connected (the demo's always) */
  const here = () => controlUnits().filter(u => S.demo || connected(u.index));

  /* a session of the state now. all: the last state (every unit known here, connected or not) */
  async function capture(name, all = false) {
    const doc = await pluginDoc();
    const units = [], ids = new Set();
    for (const u of here()) {
      const p = partOf(u.index, doc);
      units.push(p);
      ids.add(p.id);
      SS.known[p.id] = p;
    }
    /* a unit that is not connected keeps its part: the one waiting for it, else (the session's own units, or the
       last state's) what it held when it was last here */
    const pend = scfg().pending;
    for (const [id, p] of Object.entries(SS.known)) {
      if (ids.has(id)) continue;
      const part = pend[id] ? pend[id].part : (all || SS.cur.has(id)) ? p : null;
      if (part) { units.push({ ...part, absent: true }); ids.add(id); }
    }
    for (const [id, e] of Object.entries(pend)) if (!ids.has(id) && e.part) units.push({ ...e.part, absent: true });
    return {
      format: FORMAT, version: VERSION, name, saved: new Date().toISOString(), app: typeof appVersion === 'string' ? appVersion : '',
      view: { look: cfg.appearance.look, screen: VIEWS.includes(S.view) ? S.view : 'control', unit: cfg.controlUnit || '' },
      units,
    };
  }

  /* ------------------------------------------------------------ the model: a file as loaded */

  function cleanPart(p) {
    if (!p || typeof p !== 'object' || typeof p.id !== 'string' || !p.id) return null;
    const out = { id: p.id, model: String(p.model || ''), deviceType: Number.isInteger(p.deviceType) ? p.deviceType : 0, serial: String(p.serial || ''),
      rate: RATES.includes(p.rate) ? p.rate : null };
    if (Array.isArray(p.inputs)) {
      out.inputs = p.inputs.slice(0, 8).map(x => {
        if (!x || typeof x !== 'object') return {};
        const o = {};
        if (isInt(x.gain, 0, 99)) o.gain = clamp(x.gain, 10, 65);
        for (const f of ['line', 'pad', 'p48', 'lowcut', 'phase']) if (bool(x[f])) o[f] = x[f];
        return o;
      });
    }
    if (Array.isArray(p.links)) out.links = p.links.slice(0, 4).map(v => (bool(v) ? v : null));
    if (isInt(p.hizImp, -1, 7)) out.hizImp = p.hizImp;
    if (p.monitor && typeof p.monitor === 'object') {
      const m = p.monitor, o = {};
      if (isInt(m.level, 0, 192)) o.level = m.level;
      if (isInt(m.hp1, 0, 192)) o.hp1 = m.hp1;
      for (const f of ['mute', 'dim', 'mono']) if (bool(m[f])) o[f] = m[f];
      if (isInt(m.dimLevel, 1, 7)) o.dimLevel = m.dimLevel;
      if (isInt(m.source, 0, 4)) o.source = m.source;
      if (isInt(m.hp1src, 0, 1)) o.hp1src = m.hp1src;
      if (isInt(m.hp2src, 0, 1)) o.hp2src = m.hp2src;
      if (isInt(m.alt, 0, 3)) o.alt = m.alt;
      out.monitor = o;
    }
    if (p.mix && typeof p.mix === 'object') { out.mix = cleanMix(p.mix); delete out.mix.mon; }
    if (Array.isArray(p.channels)) {
      out.channels = p.channels.slice(0, 8).map(x => ({
        name: x && typeof x.name === 'string' ? x.name.slice(0, 24) : '', color: x && STRIP_COLORS[x.color] ? x.color : '', hidden: !!(x && x.hidden === true) }));
    }
    out.plugins = p.plugins && Array.isArray(p.plugins.chains) ? { chains: p.plugins.chains.filter(c => c && typeof c === 'object').slice(0, 64) } : null;
    return out;
  }

  /* a session object as read: { name, view, units } or { error } */
  function clean(s) {
    if (!s || typeof s !== 'object' || s.format !== FORMAT) return { error: 'it is not an Apollo Control session file' };
    if (!Number.isInteger(s.version) || s.version < 1) return { error: 'it has no session version' };
    if (s.version > VERSION) return { error: `it was saved by a newer Apollo Control (session version ${s.version}; this one reads version ${VERSION})` };
    const seen = new Set(), units = [];
    for (const p of (Array.isArray(s.units) ? s.units : []).map(cleanPart)) if (p && !seen.has(p.id)) { seen.add(p.id); units.push(p); }
    const v = s.view && typeof s.view === 'object' ? s.view : {};
    return { name: typeof s.name === 'string' && s.name.trim() ? s.name.trim().slice(0, 60) : 'Untitled', saved: String(s.saved || ''), units,
      view: { look: typeof v.look === 'string' ? v.look : '', screen: typeof v.screen === 'string' ? v.screen : '', unit: typeof v.unit === 'string' ? v.unit : '' } };
  }

  /* ------------------------------------------------------------ the model: apply */

  /* which present unit takes each part: the unit with its id, else one of the same model no other part names */
  function matchUnits(parts) {
    const list = controlUnits(), taken = new Set();
    const plan = parts.map(p => {
      const u = list.find(x => x.id === p.id || (!x.id && x.index === 0 && p.id === 'x8p'));
      if (u) taken.add(u.index);
      return { p, idx: u ? u.index : -1 };
    });
    for (const x of plan) {
      if (x.idx >= 0) continue;
      const u = list.find(y => !taken.has(y.index) && y.deviceType === x.p.deviceType && !parts.some(q => q.id === y.id));
      if (u) { taken.add(u.index); x.idx = u.index; x.other = true; }
    }
    return plan;
  }

  /* the linked pairs after the part, per model: a linked pair's second input takes the first's settings (as setInput) */
  function preampsAfter(idx, p) {
    const k = capsOf(unitType(idx)), c = ctl(idx);
    const linkAfter = pair => !!((k.linkPairs >>> pair) & 1) && (Array.isArray(p.links) && bool(p.links[pair]) ? p.links[pair] : c.links[pair]);
    const raw = (p.inputs || []).slice(0, k.preamps);
    return { linkAfter, ins: raw.map((x, i) => ((i & 1) && linkAfter(i >> 1) && raw[i - 1] ? raw[i - 1] : x)) };
  }

  /* the inputs this part would switch phantom power on for */
  function p48On(idx, p) {
    const c = ctl(idx);
    return preampsAfter(idx, p).ins.map((x, ch) => (x && x.p48 === true && !c.inputs[ch].p48 ? { idx, ch } : null)).filter(Boolean);
  }

  const waitFor = (test, ms) => new Promise(res => {
    const t0 = now();
    (function poll() { if (test()) res(true); else if (now() - t0 > ms) res(false); else setTimeout(poll, 100); })();
  });

  /* 1. the rate, waited for (a combined device's once for all its members) */
  async function applyRate(idx, want, o) {
    const key = rateKey(idx);
    if (o.rateKeys.has(key)) return;
    o.rateKeys.add(key);
    const agg = key === 'agg' ? rateAgg() : null, r = agg ? null : rateFor(idx);
    if (!(agg || r)) { if (want !== ((unitInfo(idx) || {}).sampleRate || want)) o.notes.push(`the ${unitShort(idx)}'s rate stays (this driver build does not change it)`); return; }
    const cur = agg ? agg.rate : r.rate;
    if (cur === want) return;
    const allowed = agg ? agg.allowed : (r.allowed || r.offered || []);
    if (allowed.length && !allowed.includes(want)) { o.notes.push(`${kHz(want)} is not allowed on the ${agg ? 'combined device' : unitShort(idx)} now`); return; }
    SS.busy = `Changing to ${kHz(want)}`;
    render();
    rateSet(key, idx, want);
    const done = await waitFor(() => !RATE.pending[key], 20000);
    if (!done) o.notes.push(`the change to ${kHz(want)} did not finish in 20 s`);
    else if (RATE.notes[key]) o.notes.push(RATE.notes[key]);
  }

  /* 2. the preamps: links first (as a preset recall), then each input; 48V on only when allowed */
  function applyPreamps(idx, p, o) {
    const k = capsOf(unitType(idx)), c = ctl(idx);
    if (!k.preamps || !Array.isArray(p.inputs)) return;
    (Array.isArray(p.links) ? p.links : []).forEach((on, pair) => {
      if (!bool(on) || !((k.linkPairs >>> pair) & 1) || c.links[pair] === on) return;
      markPending(c, `link:${pair}`, on, c.links[pair]);
      c.links[pair] = on;
      c.refused.delete(`link:${pair}`);
      hwSend(idx, 'input', pair * 2, 'link', on ? 1 : 0);
    });
    let lockedGain = false;
    preampsAfter(idx, p).ins.forEach((x, ch) => {
      if (!x) return;
      for (const key of ['line', 'pad', 'lowcut', 'phase', 'gain', '48v']) {
        const v = x[INPUT_FIELDS[key]];
        if (v === undefined) continue;
        if (key === '48v' && v && !c.inputs[ch].p48 && !o.allow48) continue;
        if (key === 'gain' && cfg.safety.lockGain) { if (v !== c.inputs[ch].gain) lockedGain = true; continue; }
        setInput(ch, key, v, { noLink: true, unit: idx });
      }
    });
    if (lockedGain) o.notes.push(`the ${unitShort(idx)}'s gains stay: gain knobs are locked (Settings, Safety)`);
    if (Number.isInteger(p.hizImp) && p.hizImp >= 0 && (k.hizLoad & 1)) setHizLoad(0, p.hizImp, idx);
  }

  function applyNames(idx, p) {
    if (!Array.isArray(p.channels)) return;
    p.channels.forEach((x, ch) => {
      const n = chanCfg(idx, ch);
      if (n === NO_NAME || !x) return;
      n.name = x.name;
      n.color = x.color;
      n.hidden = x.hidden;
    });
    saveConfig();
  }

  /* what the session leaves untouched but the unit was sent before: UA's default for it, so the unit matches the file */
  function withResets(old, nu) {
    const e = clone(nu), D = { level: 0, pan: 0, mute: false, solo: false };
    for (const [bus, s] of Object.entries(old.main || {})) {
      for (const f of Object.keys(D)) if (s[f] !== undefined && (e.main[bus] || {})[f] === undefined) (e.main[bus] || (e.main[bus] = {}))[f] = D[f];
    }
    (old.cues || []).forEach((q, c) => {
      const t = e.cues[c] || (e.cues[c] = {});
      for (const [f, d] of [['mix', 0], ['mono', false], ['mute', false]]) if (q[f] !== undefined && t[f] === undefined) t[f] = d;
      for (const bus of Object.keys(q.sends || {})) if (!(t.sends && t.sends[bus])) (t.sends || (t.sends = {}))[bus] = { level: SEND_OFF, pan: 0 };
    });
    (old.alt || []).forEach((a, n) => {
      const t = e.alt[n] || (e.alt[n] = {});
      if (a.on !== undefined && t.on === undefined) t.on = false;
      if (a.trim !== undefined && t.trim === undefined) t.trim = ALT_TRIM_UNITY;
    });
    (old.aux || []).forEach((a, n) => {
      const t = e.aux[n] || (e.aux[n] = {});
      if (a.post !== undefined && t.post === undefined) t.post = false;
      for (const f of ['sends', 'cues']) for (const kk of Object.keys(a[f] || {})) if (!(t[f] && t[f][kk])) (t[f] || (t[f] = {}))[kk] = { level: SEND_OFF, pan: 0 };
    });
    return e;
  }

  /* 3. the mix: the stored one replaced, then sent as a connect sends it (mutes and solos first) */
  function applyMix(idx, p) {
    if (!p.mix) return;
    const old = mixOf(idx), nu = cleanMix(p.mix);
    nu.mon = { ...(old.mon || {}) };
    setMixModel(idx, withResets(old, nu));
    const out = savedMixOut(idx);
    setMixModel(idx, nu);
    for (const [name, v] of out) hwSend(idx, 'monitor', name, v);
    saveMix(idx);
  }

  /* a monitor field with no readback (source, headphone sources, dim level): sent even when the app shows it already */
  function forceMon(idx, key, v) {
    const c = ctl(idx), field = MON_FIELDS[key], k = `mon:${field}`;
    if (!monAllowed(idx, key, v)) return;
    c.undo.set(k, c.mon[field]);
    c.mon[field] = v;
    c.refused.delete(k);
    storeRouting(idx, key, v);
    hwSend(idx, 'monitor', key, v);
  }

  /* 3b. the monitor section: switches, routing, speakers, then the level (never past the monitor limit) */
  function applyMonitor(idx, p) {
    const m = p.monitor || {};
    for (const key of ['mute', 'dim', 'mono']) if (bool(m[key])) setMon(key, m[key], idx);
    for (const [key, f] of [['dimlevel', 'dimLevel'], ['source', 'source'], ['hp1src', 'hp1src'], ['hp2src', 'hp2src']]) if (Number.isInteger(m[f])) forceMon(idx, key, m[f]);
    if (Number.isInteger(m.hp1)) setMon('hp1', m.hp1, idx);
    if (Number.isInteger(m.alt)) setMon('altsel', m.alt, idx);
    if (Number.isInteger(m.level)) setMon('level', Math.min(m.level, LEVEL_MAX + 2 * cfg.safety.monitorLimitDb), idx);
  }

  /* 4. the plug-in chains, through the engine (null in the file: the inserts are left as they are) */
  function applyPlugins(idx, p, o) {
    if (!p.plugins) return;
    if (S.demo) {
      if (idx === 0 && Plug.demoSessionSet) Plug.demoSessionSet(p.plugins.chains);
      else if (p.plugins.chains.length) o.notes.push(`the demo's ${unitShort(idx)} hosts no plug-ins`);
      return;
    }
    send(JSON.stringify({ cmd: 'plug-session-set', unit: idx, unitId: idOf(idx), chains: p.plugins.chains }));
  }

  async function applyUnit(idx, p, o) {
    if (p.rate) await applyRate(idx, p.rate, o);
    if (!S.demo && !(unitPresent(idx) && connected(idx))) { o.later.push(p); return; }   /* it left under the rate change */
    SS.busy = `Loading the ${unitShort(idx)}`;
    render();
    applyPreamps(idx, p, o);
    applyNames(idx, p);
    const c = ctl(idx), m = p.monitor || {};
    if (Number.isInteger(m.alt) && m.alt !== c.mon.alt && c.mon.alt) setMon('altsel', 0, idx);   /* the main speakers while ALTs change */
    applyMix(idx, p);
    applyMonitor(idx, p);
    applyPlugins(idx, p, o);
    renderUnitInputs(idx);
    renderMixFor(idx);
    renderMaster(idx);
    Aux.render(idx);
    if (idx === S.sel) { renderMonitor(); renderCues(); }
    buildBridge();
  }

  function applyView(v) {
    if (v.look && LOOKS.some(l => l.id === v.look) && v.look !== cfg.appearance.look) setLook(v.look);
    const u = v.unit && controlUnits().find(x => x.id === v.unit);
    if (u) selectUnit(u.index);
    if (v.screen && VIEWS.includes(v.screen) && v.screen !== S.view) showView(v.screen);
  }

  /* one question at a time: a dialog waits for any other to close */
  function whenFree() {
    const d = document.getElementById('dlg');
    return waitFor(() => !d.open, 3600000);
  }

  function choose(title, body, buttons, focus) {
    const run = async () => {
      await whenFree();
      const dlg = document.getElementById('dlg');
      return new Promise(res => {
        const done = v => { dlg.close(); res(v); };
        const bs = buttons.map(([v, label, cls]) => el('button', { class: cls, type: 'button', dataset: { choice: v }, onclick: () => done(v) }, label));
        dlg.replaceChildren(el('div', { class: 'dlg-body' }, el('h3', { text: title }), el('p', { text: body })),
          el('div', { class: 'dlg-foot' }, el('div'), el('div', { class: 'right' }, bs)));
        dlg.oncancel = e => { e.preventDefault(); done('cancel'); };
        dlg.showModal();
        (bs.find(b => b.dataset.choice === focus) || bs[0]).focus();
      });
    };
    const p = SS.dlg.then(run, run);
    SS.dlg = p.catch(() => {});
    return p;
  }

  const names = list => (list.length > 1 ? `${list.slice(0, -1).join(', ')} and ${list[list.length - 1]}` : list.join(''));

  /* 48V: 'on', 'off' (load the rest, phantom stays off) or 'cancel' */
  function ask48(list, sessName) {
    if (viaHost()) send('show');                  /* a tray load: the question needs the window */
    const who = names(list.map(x => inputLabel(x.ch, x.idx)));
    return choose(`Turn on 48V for ${who}?`,
      `Loading "${sessName}" switches phantom power on for ${list.length === 1 ? 'this input' : 'these inputs'}. Phantom power can damage ribbon mics and some older gear: check what is plugged in first.`,
      [['cancel', 'Cancel', 'btn'], ['off', 'Load with 48V off', 'btn'], ['on', 'Turn on 48V', 'btn btn-danger']], 'cancel');
  }

  /* a session object: applied to the units here, kept for the ones that are not (opts.last: the last state, not named) */
  async function apply(s0, opts = {}) {
    const s = clean(s0);
    if (s.error) { toast(`Not loaded: ${s.error}.`, 'bad', 8000); return false; }
    if (SS.applying) { toast('A session is being loaded already.', 'warn'); return false; }
    const label = opts.last ? 'the last state' : `"${s.name}"`;
    const plan = matchUnits(s.units), now1 = [], later = [], skipped = [];
    for (const x of plan) {
      if (x.idx < 0) later.push(x.p);
      else if (!S.demo && unitReadOnly(x.idx)) skipped.push(`the ${unitShort(x.idx)} is read-only (${unitReadOnly(x.idx)} mode)`);
      else if (!S.demo && !connected(x.idx)) later.push(x.p);
      else now1.push(x);
    }
    const on = now1.flatMap(x => p48On(x.idx, x.p));
    let allow48 = true;
    if (on.length) {
      const a = await ask48(on, opts.last ? 'the last state' : s.name);
      if (a === 'cancel') { toast(`${opts.last ? 'The last state' : `"${s.name}"`} was not loaded.`); return false; }
      allow48 = a === 'on';
      if (!allow48) skipped.push(`48V stays off on ${names(on.map(x => inputLabel(x.ch, x.idx)))}`);
    }
    SS.applying++;
    const o = { allow48, rateKeys: new Set(), notes: [], later: [] };
    try {
      SS.busy = `Loading ${label}`;
      render();
      for (const x of now1) await applyUnit(x.idx, x.p, o);
    } finally {
      SS.applying--;
      SS.busy = '';
    }
    const pend = scfg().pending;
    for (const id of Object.keys(pend)) if (!opts.last || s.units.some(p => p.id === id)) delete pend[id];
    for (const p of [...later, ...o.later]) { pend[p.id] = { name: opts.last ? 'Last state' : s.name, part: p, at: Date.now() }; SS.known[p.id] = p; }
    if (opts.view !== false) applyView(s.view);
    if (!opts.last) {
      scfg().name = s.name;
      remember(s.name);
      SS.cur = new Set(s.units.map(p => p.id));
    }
    saveConfig();
    const waiting = [...later, ...o.later].map(p => capsOf(p.deviceType).short || p.model || 'unit');
    const notes = [...skipped, ...o.notes];
    if (waiting.length) notes.push(`${names(waiting)} ${waiting.length === 1 ? 'is' : 'are'} not connected: ${waiting.length === 1 ? 'its part goes' : 'their parts go'} out on connect`);
    SS.note = { kind: notes.length ? 'warn' : 'ok', text: `Loaded ${label}${notes.length ? `: ${notes.join('; ')}` : ''}.` };
    toast(SS.note.text, notes.length ? 'warn' : 'info', notes.length ? 9000 : 4500);
    if (!opts.last) settle();
    render();
    return true;
  }

  /* "edited" compares the state now with the one right after a save or load (cfg.session.base, kept across restarts);
     after a load it is taken again once readback and the plug-in engine have answered */
  function settle() {
    const take = () => { if (!SS.applying) { scfg().base = fingerprint(false); saveConfig(); render(); } };
    take();
    setTimeout(take, 300);
    setTimeout(take, 2500);
  }

  /* a unit that connected (app.js syncMixApply, once its strips are Live): a session part waiting for it goes out
     instead of the last state. True when it took over. */
  function arrive(idx) {
    const id = idOf(idx), pend = scfg().pending, e = id && pend[id];
    if (!e || S.demo) return false;
    delete pend[id];
    saveConfig();
    (async () => {
      const p = cleanPart(e.part);
      if (!p || unitReadOnly(idx)) { applySavedMix(idx); return; }
      const on = p48On(idx, p);
      let allow48 = true;
      if (on.length) {
        const a = await ask48(on, e.name);
        if (a === 'cancel' || !(unitPresent(idx) && connected(idx))) { applySavedMix(idx); toast(`The ${unitShort(idx)}'s part of "${e.name}" was not loaded.`); render(); return; }
        allow48 = a === 'on';
      }
      SS.applying++;
      const o = { allow48, rateKeys: new Set(), notes: [], later: [] };
      if (!p.mix) applySavedMix(idx);             /* a part without a mix: the unit's last state for that */
      try { await applyUnit(idx, p, o); } finally { SS.applying--; SS.busy = ''; }
      for (const q of o.later) pend[q.id] = { name: e.name, part: q, at: Date.now() };
      saveConfig();
      SS.note = { kind: o.notes.length ? 'warn' : 'ok', text: `The ${unitShort(idx)} connected: its part of "${e.name}" is loaded${o.notes.length ? ` (${o.notes.join('; ')})` : ''}.` };
      toast(SS.note.text, o.notes.length ? 'warn' : 'info', 7000);
      if (scfg().name === e.name) settle();
      render();
    })();
    return true;
  }

  /* the last state goes out once the unit's strips are Live: live.strips.state 4 (or off / failed: nothing to wait
     for; or a driver without strips); a host that sends no "live" at all: at once. Idle, armed or building for 30 s
     after the connect: no longer waited for. */
  function stripsReady(idx) {
    if (S.demo || !SS.liveSeen) return true;
    const u = S.units[idx], lv = u && u.live;
    if (!lv) return false;
    const st = lv.strips;
    if (!st) return Number(lv.stripsErr) !== 21;        /* ERROR_NOT_READY: asked before the unit connected */
    if (st.state === 4 || st.state === 0 || st.state === 5) return true;
    return now() - ctl(idx).connectedAt > 30000;
  }
  function onLive(idx) {
    SS.liveSeen = true;
    if (S.ctl[idx] && !S.ctl[idx].mixApplied) syncMixApply(idx);
  }

  /* ------------------------------------------------------------ edited, and the autosave */

  /* the state a session holds, as text (transient: with the monitor level, mute and dim, for the autosave) */
  function fpParts(parts, transient) {
    return JSON.stringify(parts.map(p => {
      const m = p.monitor || {}, mix = { ...(p.mix || {}) };
      delete mix.mon;
      const chains = ((p.plugins && p.plugins.chains) || []).map(c => [c.aux ?? -1, c.input | 0, !!c.stereo, !!c.mon,
        (c.slots || []).map(x => [x.slot, x.name, !!x.bypass]).sort((a, b) => a[0] - b[0])]).filter(c => c[4].length || c[3]).sort();
      return [p.id, p.rate, p.inputs, p.links, p.hizImp, transient ? m : { ...m, level: 0, mute: false, dim: false }, mix, p.channels, chains];
    }).sort((a, b) => (a[0] < b[0] ? -1 : 1)));
  }
  /* the units here now, with their inserts as the Plug-ins view shows them */
  function fingerprint(transient) {
    return fpParts(here().map(u => {
      const p = partOf(u.index, null), ch = (Plug.state.sts[u.index] || {}).chains;
      p.plugins = { chains: Array.isArray(ch) ? ch : [] };
      return p;
    }), transient);
  }
  const edited = () => !!scfg().name && !!scfg().base && !SS.applying && fingerprint(false) !== scfg().base;

  /* the last state, a few seconds after a change (the host's file; the demo's in this browser, never the host's) */
  async function autosave() {
    if (S.capture || SS.applying || !here().length) return;
    const f = fingerprint(true);
    if (f === SS.autoFp || now() - SS.autoAt < 5000) return;
    SS.autoFp = f;
    SS.autoAt = now();
    const s = await capture('Last state', true);
    s.autosave = true;
    store.autosave(s);
    if (!viaHost()) { SS.last = entryOf('last-state.json', s); render(); }
  }

  /* ------------------------------------------------------------ actions */

  function remember(name) {
    const r = scfg().recent.filter(x => x.toLowerCase() !== name.toLowerCase());
    r.unshift(name);
    scfg().recent = r.slice(0, 8);
    sendRecent();
  }
  function sendRecent() { if (viaHost()) send(JSON.stringify({ cmd: 'session-recent', names: scfg().recent })); }
  const findItem = name => SS.list.find(x => x.name.toLowerCase() === String(name).toLowerCase()) || null;
  const reserved = name => /^last[\s-]?state$/i.test(name);

  async function saveAs(suggest) {
    const name0 = await dialog({ title: 'Save the session as', body: 'A session keeps every setting of every unit: preamps and 48V, monitor, mixes, cues, aux, plug-ins and the sample rate.',
      confirm: 'Save', input: suggest || scfg().name || `Session ${SS.list.length + 1}` });
    if (!name0) return false;
    return save(name0, { ask: true });
  }

  /* save under name (o.ask: a different session with that name is replaced only after asking) */
  async function save(name0, o = {}) {
    const name = String(name0 || '').trim().slice(0, 60);
    if (!name) return false;
    if (reserved(name)) { toast('"Last state" is the autosave\'s name: pick another.', 'warn'); return false; }
    const ex = findItem(name);
    if (ex && o.ask && ex.name.toLowerCase() !== (scfg().name || '').toLowerCase()) {
      const ok = await dialog({ title: `Replace "${ex.name}"?`, body: 'A session with this name exists. Saving replaces it.', confirm: 'Replace' });
      if (!ok) return false;
    }
    SS.busy = `Saving "${name}"`;
    render();
    const s = await capture(name);
    const r = await store.save(name, s);
    SS.busy = '';
    if (!r || !r.ok) {
      SS.note = { kind: 'bad', text: `"${name}" was not saved${r ? `: ${r.why}` : ': no answer from Apollo Control'}.` };
      toast(SS.note.text, 'bad', 8000);
      render();
      return false;
    }
    scfg().name = name;
    SS.cur = new Set(s.units.map(p => p.id));
    remember(name);
    scfg().base = fingerprint(false);
    saveConfig();
    SS.note ={ kind: 'ok', text: `Saved "${name}"${s.units.some(u => u.absent) ? ', with the parts of units not connected now' : ''}.` };
    toast(SS.note.text);
    if (viaHost()) await store.list();
    render();
    return true;
  }

  async function quickSave() { return scfg().name ? save(scfg().name) : saveAs(); }

  async function load(it, opts = {}) {
    if (!it) return false;
    if (SS.applying) { toast('A session is being loaded already.', 'warn'); return false; }
    SS.busy = `Opening ${it.last ? 'the last state' : `"${it.name}"`}`;
    render();
    const m = await store.load(it);
    SS.busy = '';
    if (!m || m.type !== 'session') {
      const why = !m ? 'no answer from Apollo Control' : m.why === 'none' ? 'the file is not there' : m.why;
      SS.note = { kind: 'bad', text: `${it.last ? 'The last state' : `"${it.name}"`} was not loaded: ${why}.` };
      toast(SS.note.text, 'bad', 8000);
      render();
      return false;
    }
    return apply(m.session, { ...opts, last: !!it.last });
  }

  async function loadByName(name, opts = {}) {
    if (!SS.listed || !findItem(name)) await store.list();
    const it = findItem(name);
    if (!it) { toast(`No session called "${name}".`, 'warn'); return false; }
    return load(it, opts);
  }

  async function remove(it) {
    const ok = await dialog({ title: `Delete "${it.name}"?`, body: viaHost() ? 'The file goes to the Recycle Bin.' : 'It is removed from this browser.', confirm: 'Delete', kind: 'danger' });
    if (!ok) return false;
    const r = await store.del(it);
    if (!r || !r.ok) { toast(`"${it.name}" was not deleted${r && r.why ? `: ${r.why}` : ''}.`, 'bad'); return false; }
    if ((scfg().name || '').toLowerCase() === it.name.toLowerCase()) { scfg().name = ''; scfg().base = ''; }
    scfg().recent = scfg().recent.filter(x => x.toLowerCase() !== it.name.toLowerCase());
    sendRecent();
    saveConfig();
    SS.note = { kind: 'ok', text: `Deleted "${it.name}".` };
    render();
    return true;
  }

  /* overwrite a listed session with the state now (Arcade's SAVE on a slot) */
  async function overwrite(it) {
    const ok = await dialog({ title: `Save over "${it.name}"?`, body: 'Its settings are replaced by the ones now.', confirm: 'Save over it' });
    return ok ? save(it.name) : false;
  }

  async function exportIt(it) {
    const name = it ? it.name : (scfg().name || 'Session');
    const s = it ? null : await capture(name);
    const r = await store.exportOne(name, s, it ? it.file : '');
    if (r && r.ok) { SS.note = { kind: 'ok', text: `Exported ${it ? `"${it.name}"` : 'the session'} to ${r.file}.` }; toast(SS.note.text); }
    else if (r && !r.cancelled) toast(`Not exported${r.why ? `: ${r.why}` : ''}.`, 'bad');
    render();
    return !!(r && r.ok);
  }

  async function importIt() {
    const m = await store.importOne();
    if (!m || m.cancelled) return false;
    if (m.type !== 'session') { toast(`Not imported: ${m.why || 'the file could not be read'}.`, 'bad', 8000); return false; }
    const s = clean(m.session);
    if (s.error) { toast(`Not imported: ${s.error}.`, 'bad', 8000); return false; }
    const name = reserved(s.name) ? `${s.name} (imported)` : s.name;
    if (!SS.listed) await store.list();
    const ex = findItem(name);
    if (ex && !(await dialog({ title: `Replace "${ex.name}"?`, body: 'A session with this name exists. Importing replaces it.', confirm: 'Replace' }))) return false;
    const r = await store.save(name, { ...m.session, name });          /* the file's own content, as it came */
    if (!r || !r.ok) { toast(`Not imported${r && r.why ? `: ${r.why}` : ''}.`, 'bad', 8000); return false; }
    if (viaHost()) await store.list();
    SS.note = { kind: 'ok', text: `Imported "${name}"${m.file ? ` from ${m.file}` : ''}.` };
    render();
    const go = await dialog({ title: `Imported "${name}"`, body: 'It is in the session list now. Load it?', confirm: 'Load it', cancel: 'Not now' });
    if (go) await loadByName(name);
    return true;
  }

  /* ------------------------------------------------------------ autostart, the tray */

  function onAutostart(m) {
    SS.auto = { known: true, on: !!m.on, current: !!m.current, command: String(m.command || ''), error: Number(m.error) || 0, detail: String(m.detail || ''), asked: false };
    if (SS.auto.error) toast(`Start with Windows was not changed: ${SS.auto.detail || `error ${SS.auto.error}`}.`, 'bad', 8000);
    sendPrefs();
    if (S.view === 'settings') renderSettings();
  }
  function setAutostart(on) {
    if (!host) return;
    SS.auto.asked = true;
    send(JSON.stringify({ cmd: 'autostart-set', on: !!on }));
  }
  /* the window's close button keeps it in the tray: the user's choice, else on while it starts with Windows */
  const closeToTray = () => (cfg.windowPrefs.closeToTraySet ? !!cfg.windowPrefs.closeToTray : SS.auto.on);

  function hello() {
    if (viaHost()) {
      send(JSON.stringify({ cmd: 'autostart-get' }));
      send(JSON.stringify({ cmd: 'session-load', last: true }));
      store.list();
      sendRecent();
    } else {
      store.list();
      const d = demoStore();
      if (d.last) takeLast(d.last);
    }
  }

  /* ------------------------------------------------------------ typed (Mono's command line, Ctrl+K) */

  const ok = text => ({ ok: true, text });
  const no = text => ({ ok: false, text });
  const SUBS = ['save', 'load', 'list', 'delete', 'export', 'import', 'last'];
  function pick(arg) {
    const t = arg.toLowerCase();
    return findItem(arg) || (SS.list.filter(x => x.name.toLowerCase().startsWith(t)).length === 1 ? SS.list.find(x => x.name.toLowerCase().startsWith(t)) : null);
  }
  async function exec(rest) {
    const sub = String(rest[0] || 'list').toLowerCase(), arg = rest.slice(1).join(' ').trim();
    if (!SS.listed) await store.list();
    switch (sub) {
      case 'save': {
        const n = arg || scfg().name;
        if (!n) return no('session save <name>');
        return (await save(n, { ask: true })) ? ok(`Saved "${n}".`) : no(`"${n}" was not saved.`);
      }
      case 'saveas': return (await saveAs(arg)) ? ok('Saved.') : no('Not saved.');
      case 'load': case 'open': case 'recall': {
        if (!arg) return no('session load <name>');
        const it = pick(arg);
        if (!it) return no(`No session called "${arg}". session list shows them.`);
        return (await load(it)) ? ok(`Loaded "${it.name}".`) : no(`"${it.name}" was not loaded.`);
      }
      case 'last': return (await load({ last: true, name: 'Last state' })) ? ok('Loaded the last state.') : no('The last state was not loaded.');
      case 'list': case 'ls': {
        if (!SS.list.length) return ok('No sessions yet. session save <name> saves one.');
        return ok(SS.list.map(x => `${x.name === scfg().name ? '*' : ' '} ${x.name}  ${when(x.saved)}  ${unitsText(x)}${x.bad ? `  (${x.bad})` : ''}`).join('\n'));
      }
      case 'delete': case 'rm': case 'del': {
        const it = arg && pick(arg);
        if (!it) return no('session delete <name>');
        return (await remove(it)) ? ok(`Deleted "${it.name}".`) : no('Kept.');
      }
      case 'export': {
        const it = arg ? pick(arg) : null;
        if (arg && !it) return no(`No session called "${arg}".`);
        return (await exportIt(it)) ? ok('Exported.') : no('Not exported.');
      }
      case 'import': return (await importIt()) ? ok('Imported.') : no('Not imported.');
      default: return no(`session ${SUBS.join('|')} [name]`);
    }
  }
  const subs = () => SUBS.slice();
  const listNames = () => SS.list.map(x => x.name);

  function actions() {
    const A = [
      { label: 'Save the session', run: () => quickSave(), key: 'Ctrl+S' },
      { label: 'Save the session as...', run: () => saveAs() },
      { label: 'Sessions: open the list', run: () => open(), key: 'Ctrl+O' },
      { label: 'Load the last state', cmd: 'session last' },
      { label: 'Export the session to a file', cmd: 'session export' },
      { label: 'Import a session file', cmd: 'session import' },
    ];
    for (const x of SS.list) A.push({ label: `Load session: ${x.name}`, cmd: `session load ${x.name}` });
    return A;
  }

  /* ------------------------------------------------------------ the views */

  function when(iso) {
    const d = new Date(iso);
    if (!iso || isNaN(d)) return '';
    const today = new Date(), t = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    if (d.toDateString() === today.toDateString()) return `today ${t}`;
    return `${d.toLocaleDateString([], { day: 'numeric', month: 'short', year: d.getFullYear() === today.getFullYear() ? undefined : 'numeric' })} ${t}`;
  }
  const shortOf = u => capsOf(u.deviceType).short || u.model || 'unit';
  const unitsText = it => (it.units || []).map(shortOf).join(' + ') || 'no units';
  const rateText = it => { const r = [...new Set((it.units || []).map(u => u.rate).filter(Boolean))]; return r.length ? r.map(kHz).join(' / ') : ''; };
  const insText = it => `${it.inserts | 0} insert${(it.inserts | 0) === 1 ? '' : 's'}`;

  function vm() {
    const cur = scfg().name || '';
    const items = SS.list.map(x => ({ ...x, current: !!cur && x.name.toLowerCase() === cur.toLowerCase() }));
    const k = SS.sort.key, d = SS.sort.dir;
    const val = x => (k === 'name' ? x.name.toLowerCase() : k === 'units' ? unitsText(x) : k === 'rate' ? rateText(x) : k === 'inserts' ? (x.inserts | 0) : x.saved);
    items.sort((a, b) => (val(a) < val(b) ? -d : val(a) > val(b) ? d : 0));
    const pend = Object.entries(scfg().pending).map(([id, e]) => ({ id, session: e.name, unit: e.part ? (capsOf(e.part.deviceType).short || e.part.model) : 'unit' }));
    return { cur, edited: edited(), items, last: SS.last, pend, busy: SS.busy, note: SS.note, demo: !viaHost(), dir: SS.dir, sort: SS.sort, listed: SS.listed };
  }

  const A = {
    save: () => quickSave(), saveAs: () => saveAs(), load: it => load(it), last: () => load({ last: true, name: 'Last state' }),
    del: it => remove(it), exp: it => exportIt(it), expCur: () => exportIt(null), imp: () => importIt(), over: it => overwrite(it),
    sort: key => { SS.sort = { key, dir: SS.sort.key === key ? -SS.sort.dir : key === 'saved' ? -1 : 1 }; render(); },
    close: () => close(),
    typed: line => Cmd.run(/^session\b/i.test(line) ? line : `session ${line}`),
  };

  /* shared pieces: every layout says the same things in its own idiom */
  const B = (text, onclick, o = {}) => el('button', { type: 'button', class: o.cls || 'btn', 'aria-label': o.aria || null, title: o.title || null,
    disabled: o.disabled || null, dataset: o.key ? { key: o.key } : null, onclick }, text);
  const stateWord = v => (v.busy ? v.busy : !v.cur ? 'not saved' : v.edited ? 'edited' : 'saved');
  const curText = v => v.cur || 'Untitled';
  function noteNode(v) {
    const out = [];
    if (v.busy) out.push(el('div', { class: 'ss-note', dataset: { kind: 'busy' }, role: 'status', text: `${v.busy}...` }));
    for (const p of v.pend) out.push(el('div', { class: 'ss-note', dataset: { kind: 'wait' }, text: `WAITING: the ${p.unit}'s part of "${p.session}" goes out when it connects.` }));
    if (v.note && !v.busy) out.push(el('div', { class: 'ss-note', dataset: { kind: v.note.kind }, role: 'status', text: v.note.text }));
    return el('div', { class: 'ss-notes', 'aria-live': 'polite' }, out);
  }
  const meta = it => [when(it.saved), unitsText(it), rateText(it), insText(it)].filter(Boolean).join(' · ');
  const rowAria = it => `${it.name}${it.current ? ' (current)' : ''}, saved ${when(it.saved)}, ${unitsText(it)}${it.bad ? `, ${it.bad}` : ''}`;
  const lastLine = v => (v.last ? `autosaved ${when(v.last.saved)}` : 'not written yet');
  const foot = text => el('div', { class: 'ss-foot', text });
  const KEYS = 'Ctrl+S saves · Ctrl+O opens this list · Esc closes';

  const L = {
    /* Studio: a console's session menu: the scribble strip with the session, backlit keys, a RECALL list */
    console(v) {
      return el('div', { class: 'ss ss-console' },
        el('div', { class: 'ss-head' }, el('span', { class: 'lbl ss-title', text: 'Session' }),
          el('div', { class: 'ss-scribble num' }, el('span', { class: 'ss-cur', text: curText(v) }), el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: stateWord(v).toUpperCase() }))),
        el('div', { class: 'ss-keys', role: 'group', 'aria-label': 'Session' },
          B('SAVE', A.save, { cls: 'ss-key', key: 'save', aria: 'Save the session', title: 'Ctrl+S' }), B('SAVE AS', A.saveAs, { cls: 'ss-key', aria: 'Save the session as' }),
          B('IMPORT', A.imp, { cls: 'ss-key', aria: 'Import a session file' }), B('EXPORT', A.expCur, { cls: 'ss-key', aria: 'Export the session' })),
        noteNode(v),
        el('div', { class: 'ss-sub lbl', text: 'Recall' }),
        el('ul', { class: 'ss-list', 'aria-label': 'Sessions' },
          el('li', { class: 'ss-row ss-last' }, B(el('span', { class: 'ss-name' }, 'LAST STATE'), A.last, { cls: 'ss-recall', aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last }),
            el('span', { class: 'ss-meta num', text: lastLine(v) })),
          v.items.map(it => el('li', { class: 'ss-row', dataset: { current: it.current ? '1' : '0' }, 'aria-current': it.current ? 'true' : null },
            B([el('span', { class: 'ss-name', text: it.name }), it.current ? el('span', { class: 'ss-tag', text: 'CURRENT' }) : null], () => A.load(it), { cls: 'ss-recall', aria: `Load ${rowAria(it)}`, disabled: !!it.bad }),
            el('span', { class: 'ss-meta num', text: it.bad || meta(it) }),
            B('EXP', () => A.exp(it), { cls: 'ss-small', aria: `Export ${it.name}` }), B('DEL', () => A.del(it), { cls: 'ss-small ss-del', aria: `Delete ${it.name}` })))),
        v.items.length ? null : el('div', { class: 'ss-empty', text: 'No sessions yet: SAVE AS stores this setup.' }),
        foot(KEYS));
    },

    /* Cyberpunk: save states as HUD cards with angled headers, stats in code */
    hud(v) {
      const card = (it, last) => el('li', { class: 'ss-card', dataset: { current: it && it.current ? '1' : '0', last: last ? '1' : '0' }, 'aria-current': it && it.current ? 'true' : null },
        el('div', { class: 'ss-card-head' }, el('span', { class: 'ss-name', text: last ? 'AUTOSAVE // LAST STATE' : it.name }),
          it && it.current ? el('span', { class: 'ss-tag', text: v.edited ? 'LOADED · EDITED' : 'LOADED' }) : null),
        el('div', { class: 'ss-stats num', text: last ? lastLine(v).toUpperCase() : (it.bad || [unitsText(it), rateText(it), insText(it), when(it.saved)].filter(Boolean).join(' // ').toUpperCase()) }),
        el('div', { class: 'ss-card-keys' },
          last ? B('LOAD', A.last, { cls: 'ss-pill', aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last })
            : [B('LOAD', () => A.load(it), { cls: 'ss-pill', aria: `Load ${rowAria(it)}`, disabled: !!it.bad }), B('EXP', () => A.exp(it), { cls: 'ss-pill', aria: `Export ${it.name}` }),
              B('DEL', () => A.del(it), { cls: 'ss-pill ss-del', aria: `Delete ${it.name}` })]));
      return el('div', { class: 'ss ss-hud' },
        el('div', { class: 'ss-head' }, el('span', { class: 'ss-title', text: '// SAVE STATES' }),
          el('span', { class: 'ss-cur num' }, `NODE STATE: ${curText(v).toUpperCase()} `, el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: `[${stateWord(v).toUpperCase()}]` }))),
        el('div', { class: 'ss-keys', role: 'group', 'aria-label': 'Session' },
          B('SAVE', A.save, { cls: 'ss-pill', key: 'save', aria: 'Save the session', title: 'Ctrl+S' }), B('SAVE AS', A.saveAs, { cls: 'ss-pill', aria: 'Save the session as' }),
          B('IMPORT', A.imp, { cls: 'ss-pill', aria: 'Import a session file' }), B('EXPORT', A.expCur, { cls: 'ss-pill', aria: 'Export the session' })),
        noteNode(v),
        el('ul', { class: 'ss-cards', 'aria-label': 'Sessions' }, card(null, true), v.items.map(it => card(it, false))),
        v.items.length ? null : el('div', { class: 'ss-empty num', text: 'NO SAVE STATES. SAVE AS WRITES ONE.' }),
        foot(KEYS));
    },

    /* Lab: a table, sortable, with soft-keys per row and a status line */
    table(v) {
      const th = (key, text) => el('th', { 'aria-sort': v.sort.key === key ? (v.sort.dir > 0 ? 'ascending' : 'descending') : 'none' },
        B([text, el('span', { class: 'ss-sort', 'aria-hidden': 'true', text: v.sort.key === key ? (v.sort.dir > 0 ? ' ^' : ' v') : '' })], () => A.sort(key), { cls: 'ss-th', aria: `Sort by ${text.toLowerCase()}` }));
      return el('div', { class: 'ss ss-table' },
        el('div', { class: 'ss-head' }, el('span', { class: 'lbl ss-title', text: 'Sessions' }),
          el('span', { class: 'ss-cur num' }, `CURRENT ${curText(v)} `, el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: `[${stateWord(v)}]` }))),
        el('div', { class: 'ss-keys', role: 'group', 'aria-label': 'Session' },
          B('SAVE', A.save, { cls: 'ss-soft', key: 'save', aria: 'Save the session', title: 'Ctrl+S' }), B('SAVE AS', A.saveAs, { cls: 'ss-soft', aria: 'Save the session as' }),
          B('IMPORT', A.imp, { cls: 'ss-soft', aria: 'Import a session file' }), B('EXPORT', A.expCur, { cls: 'ss-soft', aria: 'Export the session' })),
        noteNode(v),
        el('table', { class: 'ss-tbl num' },
          el('thead', {}, el('tr', {}, th('name', 'Name'), th('saved', 'Saved'), th('units', 'Units'), th('rate', 'Rate'), th('inserts', 'Ins'), el('th', { text: 'Keys' }))),
          el('tbody', {},
            el('tr', { class: 'ss-last' }, el('td', { text: 'last state' }), el('td', { text: v.last ? when(v.last.saved) : '-' }), el('td', { text: v.last ? unitsText(v.last) : '-' }),
              el('td', { text: v.last ? rateText(v.last) : '' }), el('td', { text: v.last ? String(v.last.inserts | 0) : '' }),
              el('td', {}, B('LOAD', A.last, { cls: 'ss-soft', aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last }))),
            v.items.map(it => el('tr', { dataset: { current: it.current ? '1' : '0' }, 'aria-current': it.current ? 'true' : null },
              el('td', { class: 'ss-name' }, it.current ? '> ' : '', it.name, it.current ? el('span', { class: 'ss-tag', text: ' CURRENT' }) : null),
              el('td', { text: when(it.saved) }), el('td', { text: it.bad || unitsText(it) }), el('td', { text: rateText(it) }), el('td', { text: String(it.inserts | 0) }),
              el('td', { class: 'ss-softs' }, B('LOAD', () => A.load(it), { cls: 'ss-soft', aria: `Load ${rowAria(it)}`, disabled: !!it.bad }),
                B('EXP', () => A.exp(it), { cls: 'ss-soft', aria: `Export ${it.name}` }), B('DEL', () => A.del(it), { cls: 'ss-soft ss-del', aria: `Delete ${it.name}` })))))),
        el('div', { class: 'ss-status num', role: 'status', text: `${v.items.length} session${v.items.length === 1 ? '' : 's'} · last state ${lastLine(v)} · ${v.dir || 'sessions folder'}` }),
        foot(KEYS));
    },

    /* Mono: the terminal: text rows, [load] [exp] [del], and a session prompt that takes typed commands */
    terminal(v) {
      const pad = (s, n) => (s.length > n ? `${s.slice(0, n - 1)}~` : s.padEnd(n));
      const input = el('input', { class: 'ss-prompt-in num', type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Session command (save <name>, load <name>, list, delete <name>, export, import, last)',
        placeholder: 'save <name> · load <name> · last · export · import' });
      input.addEventListener('keydown', async e => {
        e.stopPropagation();
        if (e.key === 'Enter' && input.value.trim()) { const t = input.value.trim(); input.value = ''; const r = await A.typed(t); if (r) { SS.note = { kind: r.ok ? 'ok' : 'bad', text: r.text.split('\n')[0] }; render(); } }
        else if (e.key === 'Escape') { if (input.value) input.value = ''; else A.close(); }
      });
      return el('div', { class: 'ss ss-term num' },
        el('div', { class: 'ss-line ss-dim', text: '$ session list' }),
        el('div', { class: 'ss-line' }, `session: ${curText(v)} `, el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: `(${stateWord(v)})` })),
        noteNode(v),
        el('div', { class: 'ss-line ss-dim', 'aria-hidden': 'true', text: `  ${pad('NAME', 22)} ${pad('SAVED', 14)} ${pad('UNITS', 14)} ${pad('RATE', 8)} INS` }),
        el('ul', { class: 'ss-rows', 'aria-label': 'Sessions' },
          el('li', { class: 'ss-line ss-last' }, el('span', { 'aria-hidden': 'true', text: `  ${pad('last-state', 22)} ${pad(v.last ? when(v.last.saved) : '-', 14)} ${pad(v.last ? unitsText(v.last) : '-', 14)} ${pad(v.last ? rateText(v.last) : '', 8)} ${v.last ? v.last.inserts | 0 : ''} ` }),
            B('[load]', A.last, { cls: 'ss-tx', aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last })),
          v.items.map(it => el('li', { class: 'ss-line', dataset: { current: it.current ? '1' : '0' }, 'aria-current': it.current ? 'true' : null },
            el('span', { 'aria-hidden': 'true', text: `${it.current ? '*' : ' '} ${pad(it.name, 22)} ${pad(when(it.saved), 14)} ${pad(it.bad ? 'unreadable' : unitsText(it), 14)} ${pad(rateText(it), 8)} ${String(it.inserts | 0).padEnd(3)} ` }),
            B('[load]', () => A.load(it), { cls: 'ss-tx', aria: `Load ${rowAria(it)}`, disabled: !!it.bad }), B('[exp]', () => A.exp(it), { cls: 'ss-tx', aria: `Export ${it.name}` }),
            B('[del]', () => A.del(it), { cls: 'ss-tx', aria: `Delete ${it.name}` })))),
        v.items.length ? null : el('div', { class: 'ss-line ss-dim', text: '  (no sessions: session save <name>)' }),
        el('div', { class: 'ss-prompt' }, el('span', { class: 'ss-ps', 'aria-hidden': 'true', text: 'session>' }), input,
          B('[save]', A.save, { cls: 'ss-tx', key: 'save', aria: 'Save the session', title: 'Ctrl+S' })),
        el('div', { class: 'ss-line ss-dim ss-wrap', text: 'also on the command line and in Ctrl+K: session save <name> · session load <name> · session list' }));
    },

    /* Day: plain words, big targets, one thing at a time */
    plain(v) {
      const card = it => el('li', { class: 'ss-card', dataset: { current: it.current ? '1' : '0' }, 'aria-current': it.current ? 'true' : null },
        el('div', { class: 'ss-card-text' }, el('div', { class: 'ss-name' }, it.name, it.current ? el('span', { class: 'ss-tag', text: v.edited ? 'Open now, changed since saving' : 'Open now' }) : null),
          el('div', { class: 'ss-meta', text: it.bad ? `Can't be opened: ${it.bad}` : `Saved ${when(it.saved)} · ${unitsText(it)}${rateText(it) ? ` · ${rateText(it)}` : ''} · ${it.inserts | 0} plug-in${(it.inserts | 0) === 1 ? '' : 's'}` })),
        el('div', { class: 'ss-card-keys' }, B('Open', () => A.load(it), { cls: 'btn btn-primary', aria: `Open ${rowAria(it)}`, disabled: !!it.bad }),
          B('Export', () => A.exp(it), { aria: `Export ${it.name}` }), B('Delete', () => A.del(it), { aria: `Delete ${it.name}` })));
      return el('div', { class: 'ss ss-plain' },
        el('h2', { class: 'ss-title', text: 'Sessions' }),
        el('p', { class: 'ss-intro', text: 'A session remembers every setting of your Apollos: preamps and 48V, the monitor, mixes, cues, plug-ins and the sample rate. Open one to put it all back.' }),
        el('div', { class: 'ss-now' }, el('span', { class: 'ss-now-l', text: 'Now: ' }), el('strong', { text: v.cur || 'not saved yet' }),
          v.cur ? el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: v.busy || (v.edited ? ' · changed since you saved it' : ' · saved') }) : null),
        el('div', { class: 'ss-keys', role: 'group', 'aria-label': 'Session' },
          B(v.cur ? `Save "${v.cur}"` : 'Save this setup', A.save, { cls: 'btn btn-primary', key: 'save', aria: 'Save the session', title: 'Ctrl+S' }),
          B('Save as new...', A.saveAs, { aria: 'Save the session as' }), B('Import a file...', A.imp, { aria: 'Import a session file' }), B('Export...', A.expCur, { aria: 'Export the session' })),
        noteNode(v),
        el('ul', { class: 'ss-cards', 'aria-label': 'Sessions' }, v.items.map(card),
          el('li', { class: 'ss-card ss-last' }, el('div', { class: 'ss-card-text' }, el('div', { class: 'ss-name', text: 'Last state' }),
            el('div', { class: 'ss-meta', text: v.last ? `Saved automatically ${when(v.last.saved)}: how everything was a moment ago.` : 'Saved automatically once a unit is connected.' })),
          el('div', { class: 'ss-card-keys' }, B('Open', A.last, { aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last })))),
        v.items.length ? null : el('p', { class: 'ss-empty', text: 'No sessions yet. "Save this setup" keeps everything as it is now.' }),
        foot(KEYS));
    },

    /* Arcade: SAVE SLOTS: numbered slots, LOAD / SAVE / ERASE, an empty slot for a new one, the AUTO slot */
    slots(v) {
      const n2 = i => String(i).padStart(2, '0');
      const slot = (it, i) => el('li', { class: 'ss-slot', dataset: { current: it.current ? '1' : '0' }, 'aria-current': it.current ? 'true' : null },
        el('div', { class: 'ss-slot-n', text: `SLOT ${n2(i + 1)}` }),
        el('div', { class: 'ss-name', text: it.name.toUpperCase() }),
        el('div', { class: 'ss-meta num', text: it.bad ? 'BAD DISK' : `${when(it.saved).toUpperCase()}  ${(it.units || []).map((u, j) => `P${j + 1}`).join('+') || '-'}` }),
        it.current ? el('div', { class: 'ss-tag', text: v.edited ? 'PLAYING *EDITED*' : 'PLAYING' }) : null,
        el('div', { class: 'ss-slot-keys' }, B('LOAD', () => A.load(it), { cls: 'ss-pix', aria: `Load ${rowAria(it)}`, disabled: !!it.bad }),
          B('SAVE', () => A.over(it), { cls: 'ss-pix', aria: `Save over ${it.name}` }), B('ERASE', () => A.del(it), { cls: 'ss-pix ss-del', aria: `Delete ${it.name}` })));
      return el('div', { class: 'ss ss-slots' },
        el('div', { class: 'ss-head' }, el('span', { class: 'ss-title', text: 'SAVE SLOTS' }),
          el('span', { class: 'ss-cur num' }, `NOW: ${curText(v).toUpperCase()} `, el('span', { class: 'ss-state', dataset: { state: stateWord(v) }, text: v.edited ? '*EDITED*' : v.cur ? 'SAVED' : 'NOT SAVED' }))),
        noteNode(v),
        el('ul', { class: 'ss-grid', 'aria-label': 'Save slots' },
          el('li', { class: 'ss-slot ss-auto' }, el('div', { class: 'ss-slot-n', text: 'AUTO' }), el('div', { class: 'ss-name', text: 'LAST STATE' }),
            el('div', { class: 'ss-meta num', text: v.last ? when(v.last.saved).toUpperCase() : 'EMPTY' }),
            el('div', { class: 'ss-slot-keys' }, B('LOAD', A.last, { cls: 'ss-pix', aria: `Load the last state, ${lastLine(v)}`, disabled: !v.last }))),
          v.items.map(slot),
          el('li', { class: 'ss-slot ss-new' }, el('div', { class: 'ss-slot-n', text: `SLOT ${n2(v.items.length + 1)}` }), el('div', { class: 'ss-name', text: 'EMPTY SLOT' }),
            el('div', { class: 'ss-slot-keys' }, B('SAVE', A.saveAs, { cls: 'ss-pix', key: 'save', aria: 'Save the session as', title: 'Ctrl+S saves the current one' })))),
        el('div', { class: 'ss-keys', role: 'group', 'aria-label': 'Session' },
          B(v.cur ? 'SAVE GAME' : 'SAVE AS', A.save, { cls: 'ss-pix', aria: 'Save the session', title: 'Ctrl+S' }), B('IMPORT', A.imp, { cls: 'ss-pix', aria: 'Import a session file' }),
          B('EXPORT', A.expCur, { cls: 'ss-pix', aria: 'Export the session' })),
        foot(KEYS.toUpperCase()));
    },
  };

  const layout = () => { const r = Skin.r('sessions'); return (r && L[r.layout]) ? r.layout : 'plain'; };

  function renderButton() {
    if (!SS.btn) return;
    const cur = scfg().name, st = SS.busy ? 'busy' : !cur ? 'none' : edited() ? 'edited' : 'saved';
    const key = `${cur}|${st}`;
    if (SS.btn.dataset.stamp === key) return;
    SS.btn.dataset.stamp = key;
    SS.btn.dataset.state = st;
    SS.btn.replaceChildren(...[el('span', { class: 'sb-l', text: 'Session' }), el('span', { class: 'sb-n', text: cur || 'untitled' }),
      st === 'edited' ? el('span', { class: 'sb-e', text: 'edited' }) : st === 'busy' ? el('span', { class: 'sb-e', text: 'loading' }) : null].filter(Boolean));
  }

  /* rebuilt only when what it shows changed (the 2 s tick asks often); focus and a half-typed prompt are kept */
  function render() {
    renderButton();
    if (!SS.root || !SS.open) return;
    const lay = layout(), v = vm(), stamp = JSON.stringify([lay, v]);
    if (SS.root.dataset.stamp === stamp) return;
    SS.root.dataset.stamp = stamp;
    const a = document.activeElement, keep = a && SS.root.contains(a) ? (a.getAttribute('aria-label') || a.textContent) : null;
    const typed = SS.root.querySelector('.ss-prompt-in');
    const text = typed ? typed.value : '';
    SS.root.dataset.layout = lay;
    SS.root.replaceChildren(L[lay](v));
    const p = SS.root.querySelector('.ss-prompt-in');
    if (p && text) p.value = text;
    if (keep) {
      const back = [...SS.root.querySelectorAll('button, input')].find(n => (n.getAttribute('aria-label') || n.textContent) === keep);
      if (back) back.focus();
    }
  }

  function open() {
    if (!SS.root) return;
    if (SS.open) { focusFirst(); return; }
    SS.open = true;
    SS.root.hidden = false;
    SS.root.dataset.stamp = '';
    SS.btn.setAttribute('aria-expanded', 'true');
    render();
    focusFirst();
    store.list();
  }
  function focusFirst() {
    const f = SS.root.querySelector('[data-key="save"]') || SS.root.querySelector('button:not([disabled]), input');
    if (f) f.focus();
  }
  function close(back = true) {
    if (!SS.open) return;
    SS.open = false;
    SS.root.hidden = true;
    SS.btn.setAttribute('aria-expanded', 'false');
    if (back) SS.btn.focus();
  }

  function build() {
    SS.btn = document.getElementById('sessBtn');
    SS.root = document.getElementById('sessPanel');
    if (!SS.btn || !SS.root) return;
    SS.btn.addEventListener('click', e => { e.stopPropagation(); if (SS.open) close(); else open(); });
    SS.root.addEventListener('keydown', e => { if (e.key === 'Escape' && !document.getElementById('dlg').open) { close(); e.preventDefault(); e.stopPropagation(); } });
    document.addEventListener('mousedown', e => {
      if (SS.open && !document.getElementById('dlg').open && !e.target.closest('#sessPanel, #sessBtn')) close(false);
    });
    renderButton();
    setInterval(() => { renderButton(); autosave(); render(); }, 2000);
  }

  function onTheme() { if (SS.root) SS.root.dataset.stamp = ''; render(); }

  return {
    build, hello, onHost, onTheme, open, close, render, stripsReady, onLive, arrive, capture, apply, clean, quickSave, saveAs, save, load, loadByName,
    exec, actions, subs, names: listNames, setAutostart, closeToTray, importIt, exportIt,
    autostart: () => ({ ...SS.auto }),
    get state() { return SS; },
  };
})();
