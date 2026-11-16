'use strict';
/*
 * Plug-ins (Apollo Control): UAD plug-in inserts on a unit's inputs, hosted by apollo.sys on the unit's strip DSP
 * (docs/PLUGINS.md 5, message contract docs/apollo-control-units.md 1.5). All of it lives on the Control view:
 *
 *   - the browser docked beside the strips (#plb): every mapped plug-in grouped by type, search, a "fits the DSP"
 *     filter, counts per group, a small preview per plug-in and a detail card on hover or focus;
 *   - the insert slot(s) on each strip of every unit, between the preamp and the monitor mix (signal-flow order);
 *   - drag a plug-in onto a slot to load it, drag a loaded slot onto another to move it, drag it back onto the
 *     browser (or press the slot's x) to unload it; while dragging, every slot says whether it takes the plug-in
 *     and, if not, why (not licensed, no room on the DSP, needs a power cycle, not on this driver build, ...);
 *   - the same by keyboard: Enter on a plug-in picks it up and moves focus to the first slot that takes it,
 *     Enter on a slot puts it there, Esc cancels; on a loaded slot M picks it up to move, Delete removes, B bypasses;
 *   - the parameter drawer, opened in place under the strips for one slot: the plug-in's own value texts, knobs and
 *     stepped buttons with numeric entry, and per control whether the DSP read it back.
 *
 * Per unit (docs 1.5 [P7]): the page says it reads "unit" with "units":true on its plug-* commands; the host then sends
 * "plugins", "plugin-list" and "plugin-result" per slot. Each unit keeps its own state (hosting, DSP room, inserts,
 * model, inputs, pairs, supported) and its own plug-in list (its licence answer). A unit's slots take plug-ins once the
 * host sends a "plugins" for it with supported true; until then (or supported false: an apollo.sys that hosts on the
 * x8p only) they say "not supported on this driver build". Slots exist below "inputs"; a linked pair takes a stereo
 * insert when its first input is in "pairs". Away from 48 kHz the inserts wait ([Rate] plugins48k, insert code rate).
 * The browser lists the unit being placed on (an empty slot clicked first), else the focused unit when it hosts, else
 * the x8p; a drop is checked against the target unit's own list and room.
 *
 * Categories come from data, in this order: the host's "category" (UA's own primary category from the local UAD
 * install's manifest.json, read offline by plugin_engine.h), then its "tags" (the manifest's categories), then a
 * small name-rule table (NAME_RULES below) for a host without them. Previews: the host's "art" (a
 * https://uad-art.apollo/... URL WebView2 serves straight from the user's own UAD install, read-only, never
 * copied), else a drawn placeholder per category.
 *
 * [Ins] Chains (docs 1.9): a host that sends "slots" (4) composes up to four inserts per input (and per linked pair)
 * as one chain on the strip DSP, in slot order; each strip then shows four slot rows, the Unison slot above them, a
 * REC / MON toggle and the chain's state once; each unit's AUX 1 / AUX 2 strips (auxbus.js) carry four slots of their own
 * (target aux, taken only while the host says auxInserts). A drop or pick into a row is plug-insert with slot, a drag
 * between rows of one chain plug-move, remove / bypass / params name their slot. A host without "slots" (section 1.5)
 * gets one slot per strip and the old messages, unchanged. Targets: an input's number, or AUX_T + a for an aux.
 *
 * In demo mode this file simulates the host for the x8p with invented plug-ins (the demo's Twin X does not host,
 * as with today's driver). Loaded before app.js; it uses app.js globals (S, el, svg, Knob, seg, sw, toast, dialog,
 * host, chanCfg, ctl, unitType, unitShort, ...) only inside functions, after app.js has booted.
 */
const Plug = (() => {
  /* refusal codes the host sends (plugin_engine.h Blocked / Refusal) and their short labels */
  const CODES = {
    licence: 'Not licensed', room: 'No room', warm: 'Needs a power cycle', quarantine: 'DSP quarantined',
    modules: 'Modules missing', hosting: 'Hosting off', caller: 'Not the host program', strips: 'Strips not live',
    busy: 'Busy', stereo: 'No stereo', staged: 'Driver too old', maps: 'Map problem', readback: 'Not confirmed',
    engine: 'Parameter engine', driver: 'Refused', absent: 'Not connected', admin: 'Administrator step',
    rate: 'Waits for 48 kHz', unit: 'Not on this driver build',
    cycles: 'DSP load', layout: 'Layout', unison: 'No Unison map', aux: 'Aux inserts off',
    /* [Chainfix] a plug-in open-apollo could not qualify, a chain that stopped the DSP (saved bypassed), the chain guard */
    unqualified: 'Not qualified', stopped: 'Stopped the DSP', chain: 'One insert per input',
  };
  const STATE_TEXT = { running: 'Running', loading: 'Loading', bypassed: 'Bypassed', waiting: 'Waiting', failed: 'Not running', idle: 'Plain strip' };
  const MAX_SLOTS = 4;                  /* rows built per strip; a host without "slots" (1.5) shows the first only */
  const AUX_T = 100;                    /* the target number of AUX 1 (AUX 2: 101) next to the inputs' 0.. */
  const AUX_OFF = 'Aux inserts are off: they are experimental (an administrator sets Parameters\\AuxInserts = 1 to turn them on).';
  const UNISON_NONE = 'Unison: no plug-in has a Unison map yet (the mapper maps only the insert form of a plug-in).';
  const MIME = 'application/x-apollo-plugin';

  /* Display groups, in browser order. ua: UA's own category words (manifest primary_category, UA.xml titles,
     manifest categories), lower-case with "&" read as "and". */
  const GROUPS = [
    { id: 'eq', name: 'EQ', ua: ['equalizers', 'equalization', 'eq'] },
    { id: 'dyn', name: 'Dynamics', sub: 'compressors, limiters, de-essers', ua: ['compressors and limiters', 'dynamics'] },
    { id: 'pre', name: 'Preamp & channel strip', ua: ['channel strip and preamp', 'preamp and channel strip'] },
    { id: 'verb', name: 'Reverb', ua: ['reverb and room', 'reverb'] },
    { id: 'delay', name: 'Delay & echo', ua: ['delay'] },
    { id: 'mod', name: 'Modulation', ua: ['modulation'] },
    { id: 'amp', name: 'Guitar & bass amps', ua: ['guitar and bass'] },
    { id: 'tape', name: 'Tape & saturation', ua: ['tape and saturation', 'harmonic and tape'] },
    { id: 'master', name: 'Mastering', ua: ['mastering'] },
    { id: 'other', name: 'Special & other', ua: ['special processing', 'pitch correction', 'microphone'] },
  ];
  const GROUP = Object.fromEntries(GROUPS.map(g => [g.id, g]));
  const GROUP_SHORT = { eq: 'EQ', dyn: 'DYN', pre: 'PRE', verb: 'VERB', delay: 'DLY', mod: 'MOD', amp: 'AMP', tape: 'TAPE', master: 'MST', other: 'OTH' };
  /* Name rules, only for a plug-in the host sends no category for (an older host, the demo's unnamed ones).
     First match wins; documented in docs/apollo-control-units.md 1.5. */
  const NAME_RULES = [
    ['amp', /\b(amp|amplifier|marshall|fender|ampeg|engl|diezel|suhr|friedman|bogner|svt|b15|tweed|cabinet)\b/i],
    ['verb', /(reverb|plate|chamber|\broom\b|hall|480l|\b224\b|rmx|emt|bx 20|spring)/i],
    ['delay', /(delay|echo|dmx|\bep-34\b|galaxy|tapped)/i],
    ['mod', /(chorus|flang|phaser|tremolo|vibe|modulat|leslie|rotary|dimension|brigade)/i],
    ['eq', /(\beq\b|equali[sz]|pultec|\b55\d\b|\b560\b|massive passive|cambridge|bx_digital)/i],
    ['dyn', /(compress|limiter|1176|la-2|la-3|fairchild|distressor|dbx|bus comp|leveler|de-?esser|esser|dyna|maximi[sz]er|gate)/i],
    ['pre', /(preamp|channel strip|\bstrip\b|1073|1084|\bv76\b|\b610\b|avalon|vision)/i],
    ['tape', /(tape|\batr\b|studer|ampex|saturat|culture vulture|oxide|j37)/i],
    ['master', /(master|inflator)/i],
  ];

  const P = {
    sts: {},                        /* unit slot -> the host's last "plugins" for it */
    lists: {},                      /* unit slot -> the host's last "plugin-list" for it (its licence answer) */
    notes: [],                      /* refusals shown in the browser and on the slot: { unit, input, slot, op, code, text } */
    pending: new Map(),             /* `${unit}:${target}:${slot}:${index}` -> value sent, until the host answers */
    refusedParam: new Set(),        /* the same keys: a change the DSP did not confirm */
    search: '', fits: false, closed: new Set(),
    open: null,                     /* the drawer's insert: { unit, input (target), slot } */
    armed: null,                    /* keyboard placement: { kind: 'add', item } | { kind: 'move', unit, from (target), slot, name, label } */
    drag: null,                     /* the same, while a mouse drag runs */
    target: null,                   /* an empty slot the user started from: { idx, ch, k } (the next pick goes there) */
    moves: new Map(),               /* `${unit}:${target}:${slot}` -> { unit, input, slot, fromUnit, from, fromSlot, values, name } a move in flight */
    recmon: new Map(),              /* `${unit}:${input}` -> MON (true) as last sent, for an input with no chain to report it */
    sel: '',                        /* the browser's selected plug-in (name) */
    built: false, ui: {},
    demo: null,
  };

  /* ---------------------------------------------------------------- sending */

  function send(obj) {
    if (S.demo) { demoCmd(obj); return; }
    if (host) host.postMessage(JSON.stringify(obj));
  }

  /* hello from the host (not the demo): the strips' slots want the inserts of every unit that hosts */
  function hello() { send({ cmd: 'plug-state', units: true }); }

  const unitOf = m => (typeof m.unit === 'number' && m.unit > 0 ? m.unit | 0 : 0);

  /* ---------------------------------------------------------------- host messages */

  function onState(m) {
    const idx = unitOf(m);
    P.sts[idx] = m;
    for (const k of [...P.pending.keys()]) {
      const [u, input, slot] = k.split(':').map(Number);
      if (u !== idx) continue;
      const ins = slotIns(idx, input, slot);
      if (!ins || !ins.busy) P.pending.delete(k);
    }
    for (const c of chainsOf(idx)) if (c.aux === null || c.aux === undefined) P.recmon.set(`${idx}:${c.input}`, !!c.mon);
    runMoves();
    renderAll();
    if (S.view === 'settings') renderSettings();
  }

  function onList(m) {
    P.lists[unitOf(m)] = m;
    renderBrowser();
    renderSlots();
  }

  function onResult(m) {
    const idx = unitOf(m), input = m.aux === 0 || m.aux === 1 ? AUX_T + m.aux : m.input | 0;
    const slot = Number.isInteger(m.slot) ? m.slot : m.slot === 'unison' ? 'u' : 0;
    if (m.op === 'param') {
      for (const k of [...P.pending.keys()]) {
        if (!k.startsWith(`${idx}:${input}:${slot}:`)) continue;
        P.pending.delete(k);
        if (!m.ok) P.refusedParam.add(k); else P.refusedParam.delete(k);
      }
    }
    if (m.op === 'insert' && !m.ok) P.moves.delete(`${idx}:${input}:${slot}`);   /* a move's source stays */
    P.notes = P.notes.filter(n => !(n.unit === idx && n.input === input && (n.op === m.op || (m.ok && m.op === 'insert'))));
    if (!m.ok && m.code) {
      const what = { insert: 'not inserted', param: 'parameter not changed', bypass: 'not switched', retry: 'not restarted',
        hosting: 'hosting not changed', modules: 'modules not installed', remove: 'not removed', move: 'not moved', recmon: 'REC / MON not switched',
        unison: 'Unison not loaded', 'unison-remove': 'Unison not removed' }[m.op] || 'refused';
      const where = ['hosting', 'modules'].includes(m.op) ? 'Plug-in hosting' : `${inputName(idx, input)}${Number.isInteger(m.slot) && multi(idx) ? ` insert ${m.slot + 1}` : ''}`;
      P.notes.push({ unit: idx, input, slot, op: m.op, code: m.code, detail: m.detail || '', text: `${where}: ${what}. ${m.detail || CODES[m.code] || ''}`.trim() });
      setTimeout(() => Skin.on('refuse', slotNode(idx, input, slot === 'u' ? 'u' : slot)), 0);
      if (S.view !== 'control' && S.view !== 'settings') toast(`${where}: ${what}. ${m.detail || ''}`, 'warn', 7000);
    } else if (m.ok && (m.op === 'hosting' || m.op === 'modules') && m.detail) {
      toast(m.detail, 'info', 6000);
    }
    renderAll();
    if (S.view === 'settings') renderSettings();
  }

  /* ---------------------------------------------------------------- state helpers */

  const EMPTY = { present: false, hosting: {}, dsp: {}, strips: {}, maps: {}, inserts: [] };
  const st = idx => P.sts[idx] || EMPTY;
  /* the host reports that this unit can host plug-ins: a "plugins" for its slot, supported (absent on a host from before
     the per-unit work, which only ever sends the x8p's) */
  const hosts = idx => { const s = P.sts[idx]; return !!s && s.supported !== false && s.model !== null && !!(s.hosting || {}).driver; };
  /* the inputs that have a slot, and the first inputs of the pairs that take a stereo insert (the host's, else the model's) */
  const inputsOf = idx => { const s = P.sts[idx]; return s && Number.isInteger(s.inputs) && s.inputs > 0 ? s.inputs : capsOf(unitType(idx)).preamps; };
  const pairsOf = idx => { const s = P.sts[idx]; return s && Array.isArray(s.pairs) ? s.pairs : Array.from({ length: inputsOf(idx) >> 1 }, (_, i) => i * 2); };
  const inserts = idx => st(idx).inserts || [];
  const isAux = ch => ch >= AUX_T;
  const tgt = ch => (isAux(ch) ? { aux: ch - AUX_T } : { input: ch });
  /* [Ins] slot rows per strip: the host's "slots" (4), else one (a section 1.5 host), none on a unit that does not host */
  const multi = idx => (st(idx).slots | 0) > 1;
  const slotCount = idx => clamp(st(idx).slots | 0 || 1, 1, MAX_SLOTS);
  const auxInsertsOn = idx => !!st(idx).auxInserts;
  /* the unit's chains: the host's (1.9), else one per insert of a section 1.5 host */
  function chainsOf(idx) {
    const s = st(idx);
    if (Array.isArray(s.chains)) return s.chains;
    return inserts(idx).map(i => ({ input: i.input, aux: null, stereo: !!i.stereo, mon: false, state: i.state, code: i.code || '', why: i.why || '',
      busy: i.busy || '', latency: i.latency || 0, slots: [{ ...i, slot: 0 }], unison: null }));
  }
  const noAux = c => c.aux === null || c.aux === undefined;
  const chainFor = (idx, ch) => chainsOf(idx).find(c => (isAux(ch) ? c.aux === ch - AUX_T : noAux(c) && (c.input === ch || (c.stereo && c.input + 1 === ch)))) || null;
  /* one slot's insert, with its chain's target and layout: { slot, name, label, state, input (target), stereo, chain, params, ... } */
  function slotIns(idx, ch, k) {
    const c = chainFor(idx, ch);
    if (!c) return null;
    const s = k === 'u' ? c.unison : (c.slots || []).find(x => x.slot === k);
    if (!s) return null;
    return { ...s, slot: k, input: isAux(ch) ? ch : c.input, stereo: isAux(ch) || !!c.stereo, chain: c, state: s.state || c.state,
      busy: s.busy || '', code: s.code || c.code || '', why: s.why || c.why || '', params: s.params || [] };
  }
  const allSlots = idx => chainsOf(idx).flatMap(c => (c.slots || []).map(s => ({ ...s, chain: c })));
  /* the first insert of an input (a section 1.5 page's "insert"), for the meter bridge, the routing, typed commands */
  const insertOn = (idx, input) => { const c = chainFor(idx, input); return c && c.slots && c.slots.length ? slotIns(idx, input, c.slots[0].slot) : null; };
  const inputName = (idx, ch) => {
    if (isAux(ch)) return `${idx ? `${unitShort(idx)} ` : ''}AUX ${ch - AUX_T + 1}`;
    const n = chanCfg(idx, ch).name;
    const base = idx ? `${unitShort(idx)} input ${ch + 1}` : `Input ${ch + 1}`;
    return n ? `${base} (${n})` : base;
  };
  const slotNode = (idx, ch, k) => document.querySelector(k === 'u'
    ? `#inputs .ins-uni[data-unit="${idx}"][data-ch="${ch}"]` : `#inputs .ins-slot[data-unit="${idx}"][data-ch="${ch}"][data-n="${k}"]`);
  const pairLinked = (idx, ch) => {
    const k = capsOf(unitType(idx)), p = ch >> 1, c = S.ctl[idx];          /* never makes a unit's state (a unit that left) */
    return !!c && !!(c.links[p]) && !!((k.linkPairs >>> p) & 1) && (ch | 1) < k.preamps && pairsOf(idx).includes(ch & ~1);
  };
  const fmtWords = n => Number(n || 0).toLocaleString('en-US');
  const listOf = idx => P.lists[idx] || null;
  const itemsOf = idx => (listOf(idx) && listOf(idx).items) || [];
  const itemFor = (idx, name) => itemsOf(idx).find(i => i.name === name) || null;
  const hex4 = n => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
  const dspName = idx => `DSP ${(st(idx).dsp || {}).index ?? 1}`;
  const unitWord = idx => (idx ? `the ${unitShort(idx)}` : 'the x8p');

  /* The unit the browser lists: the one being placed on, else the focused one when it hosts, else the x8p. */
  function browserUnit() {
    if (P.target) return P.target.idx;
    if (S.sel && hosts(S.sel) && listOf(S.sel)) return S.sel;
    return 0;
  }

  /* what keeps any insert out of unit idx right now: { code, short, text } or null */
  function unitBlock(idx) {
    const s = st(idx), h = s.hosting || {}, short = unitShort(idx) || 'unit';
    const r = typeof rateFor === 'function' ? rateFor(idx) : null;
    if (r && r.plugins48k) {
      return { code: 'rate', short: '48 KHZ ONLY', text: `Plug-ins run at 48 kHz only: the ${short} runs at ${(r.rate / 1000).toFixed(1).replace(/\.0$/, '')} kHz. Its inserts wait and come back at 48 kHz.` };
    }
    if (S.demo && idx === 0) return null;
    if ((idx !== 0 && !hosts(idx)) || (P.sts[idx] && P.sts[idx].supported === false)) {
      return { code: 'unit', short: 'NOT ON THIS BUILD', text: `Plug-ins on the ${short} are not supported on this driver build${P.sts[idx] && P.sts[idx].supported === false ? ' (it hosts them on the x8p only)' : ''}.` };
    }
    const an = /^[aeiouxAEIOUX]/.test(short) ? 'an' : 'a';
    if (!s.present) return { code: 'absent', short: `${short.toUpperCase()} OFFLINE`, text: `The ${short} is not connected (or not in full mode): saved inserts come back when it is.` };
    if (!h.driver) return { code: 'driver', short: 'NO HOST', text: 'The installed apollo.sys has no plug-in hosting.' };
    if (!h.on) {
      /* the driver says why it holds plug-ins back (caps Why): a rate away from 48 kHz is not "hosting off" */
      const why = String(h.why || '').trim();
      if (/kHz/.test(why)) {
        const at = why.replace(/:.*$/, '').replace(/^the unit/, `the ${short}`);
        return { code: 'rate', short: '48 KHZ ONLY', text: `Plug-ins run at 48 kHz only: ${at}. Its inserts wait and come back at 48 kHz.` };
      }
      return { code: 'hosting', short: 'HOSTING OFF', text: 'Plug-in hosting is off. Turning it on needs an administrator once (Settings, Plug-in hosting).' };
    }
    if (!h.caller) return { code: 'caller', short: 'OTHER PROGRAM', text: 'The driver takes plug-in calls from another program. Turn hosting on again from this Apollo Control.' };
    if ((s.dsp || {}).quarantined) return { code: 'quarantine', short: 'QUARANTINED', text: `${dspName(idx)} is quarantined after an unproven plug-in step. Power-cycle the ${short}${idx ? '' : ' (it also drops the Twin X)'}.` };
    /* [Warm] the driver says whether the strip DSP is ours (a warm start that adopted the strips counts) and why not */
    const d = s.dsp || {};
    if (!d.owned || d.ownWhy || (typeof d.ownWhy !== 'string' && (s.strips || {}).warm)) {
      if (d.ownWhy && /checks its input strips/.test(d.ownWhy)) return { code: 'strips', short: 'STARTING', text: `${d.ownWhy[0].toUpperCase()}${d.ownWhy.slice(1)}.` };
      if (d.ownWhy) return { code: 'warm', short: 'POWER CYCLE', text: `${d.ownWhy[0].toUpperCase()}${d.ownWhy.slice(1)}.` };
      return { code: 'warm', short: 'POWER CYCLE', text: `Needs ${an} ${short} power cycle: after a warm start (driver update, PC restart with the ${short} on) no DSP is ours to place on. Saved inserts come back after it.` };
    }
    if (!(s.strips || {}).live) return { code: 'strips', short: 'STRIPS NOT LIVE', text: `The ${short}'s input strips are not live yet.` };
    return null;
  }

  /* code words a plug-in needs on unit idx's DSP now: 0 when the same plug-in already runs there (its code is shared) */
  function needs(idx, item) {
    return allSlots(idx).some(i => i.name === item.name && (i.state || i.chain.state) === 'running') ? 0 : (item.codeWords || 0);
  }
  const roomNow = idx => { const d = st(idx).dsp || {}; return d.largest ?? d.room ?? 0; };
  const fits = (idx, it) => roomNow(idx) >= needs(idx, it);

  /* ---------------------------------------------------------------- categories */

  const norm = s => String(s || '').toLowerCase().replace(/&/g, 'and').replace(/\s+/g, ' ').trim();
  const groupOfUa = word => { const w = norm(word); return (GROUPS.find(g => g.ua.includes(w)) || {}).id || ''; };

  /* The display group of a plug-in, and where it came from (shown in the detail card). */
  function categorize(it) {
    const tags = (Array.isArray(it.tags) ? it.tags : []).map(groupOfUa).filter(Boolean);
    const prim = norm(it.category);
    if (prim === 'delay and modulation') {
      const d = tags.includes('delay'), m = tags.includes('mod');
      if (m && !d) return { id: 'mod', src: 'ua' };
      if (d && !m) return { id: 'delay', src: 'ua' };
      return { id: /mod|chorus|flang|phas/i.test(it.name) ? 'mod' : 'delay', src: 'ua' };
    }
    const g = groupOfUa(it.category);
    /* UA files de-essers and the like under "Special Processing" with a Dynamics tag: the tag is more specific */
    if (g && g !== 'other') return { id: g, src: 'ua' };
    const specific = tags.find(t => t !== 'other');
    if (specific) return { id: specific, src: 'ua' };
    if (g) return { id: g, src: 'ua' };
    for (const [id, re] of NAME_RULES) if (re.test(it.label || it.name)) return { id, src: 'name' };
    return { id: 'other', src: 'none' };
  }

  /* ---------------------------------------------------------------- previews */

  /* A line glyph per group (24 x 12): the placeholder when the UAD install has no art for a plug-in. */
  const GLYPHS = {
    eq: 'M1 10 C6 10 8 2 12 2 C16 2 18 10 23 10',
    dyn: 'M1 11 L12 5 L23 3 M1 11 L23 1',
    pre: 'M8 1 L18 6 L8 11 Z M1 3.5 H8 M1 8.5 H8 M18 6 H23',
    verb: 'M2 11 V2 M5 11 V4 M8 11 V6 M11 11 V7.5 M14 11 V8.5 M17 11 V9.3 M20 11 V10',
    delay: 'M3 11 V2 M9 11 V4.5 M15 11 V7 M21 11 V9 M1 11 H23',
    mod: 'M1 6 C4 0 7 0 9 6 S14 12 17 6 S21 0 23 3',
    amp: 'M3 1 H21 V11 H3 Z M12 6 m-3.2 0 a3.2 3.2 0 1 0 6.4 0 a3.2 3.2 0 1 0 -6.4 0',
    tape: 'M7 6 m-4 0 a4 4 0 1 0 8 0 a4 4 0 1 0 -8 0 M17 6 m-4 0 a4 4 0 1 0 8 0 a4 4 0 1 0 -8 0 M3 11 H21',
    master: 'M1 3 H23 M1 9 L4 4 L7 3 L10 3 L13 3 L16 4 L19 8 L23 11',
    other: 'M3 3 h1 M8 3 h1 M13 3 h1 M18 3 h1 M3 9 h1 M8 9 h1 M13 9 h1 M18 9 h1',
  };
  function glyph(id) {
    const s = svg('svg', { class: 'pl-glyph', viewBox: '0 0 24 12', 'aria-hidden': 'true' });
    s.append(svg('path', { d: GLYPHS[id] || GLYPHS.other }));
    return s;
  }
  /* The plug-in's own art from the local UAD install (the host's art URL), or the group glyph. */
  function preview(it, cls) {
    const g = categorize(it).id;
    const box = el('span', { class: `pl-prev ${cls || ''}`, dataset: { group: g } });
    if (it.art) {
      const img = el('img', { src: it.art, alt: '', loading: 'lazy', draggable: 'false' });
      img.addEventListener('error', () => { box.dataset.art = 'none'; img.replaceWith(glyph(g)); }, { once: true });
      box.dataset.art = 'ua';
      box.append(img);
    } else {
      box.dataset.art = 'none';
      box.append(glyph(g));
    }
    return box;
  }

  /* ---------------------------------------------------------------- drop rules */

  /*
   * Whether slot k of target ch on unit idx takes p (add: a list item; move: an insert being moved), and the insert it
   * would be: { ok, code, short, why, input, stereo, there, within }. within: a move between slots of one chain (the
   * host's plug-move: onto an empty slot it moves, onto a used one the two swap). The reasons are what the slot shows
   * while a plug-in is placed. The target unit's own list decides the licence, its own DSP the room, its links stereo.
   */
  function dropCheck(p, idx, ch, k = 0) {
    const no = (code, short, why) => ({ ok: false, code, short, why });
    const b = unitBlock(idx);
    if (b) return no(b.code, b.short, b.text);
    const aux = isAux(ch);
    if (aux) {
      if (!auxInsertsOn(idx)) { const c = chainFor(idx, ch); return no('aux', 'AUX OFF', (c && c.code === 'aux' && c.why) || AUX_OFF); }
    } else if (ch >= inputsOf(idx)) return no('unit', 'NO SLOT', 'No plug-in slot on this input.');
    if (k >= slotCount(idx)) return no('unit', 'NO SLOT', 'This driver build runs one insert per input.');
    const name = p.kind === 'add' ? p.item.name : p.name, label = p.kind === 'add' ? p.item.label : p.label;
    const mine = itemFor(idx, name), cross = p.kind === 'add' || p.unit !== idx;
    const it = mine || (p.kind === 'add' ? p.item : { name, label, mono: true, stereo: true, codeWords: 0, licensed: true });
    if (cross) {
      if (!mine && listOf(idx) && listOf(idx).state === 'ok') return no('licence', 'NOT MAPPED', `${label} is not mapped for ${unitWord(idx)}.`);
      if (it.licensed === false) return no('licence', 'NOT LICENSED', `${label} is not licensed on ${unitWord(idx)}: new licences come through UA's own software.`);
      if (it.licensed === null || it.licensed === undefined) return no('licence', 'LICENCE ?', `No licence answer from ${unitWord(idx)} yet (hosting off, or no answer).`);
    }
    const first = aux ? ch : ch & ~1, linked = !aux && pairLinked(idx, ch), h = st(idx).hosting || {};
    const input = aux ? ch : linked ? first : ch, stereo = aux || linked;
    const src = !cross ? chainFor(idx, p.from) : null, dst = chainFor(idx, input);
    const within = !cross && !!src && src === dst;
    if (within && p.slot === k) return no('same', 'HERE NOW', 'It is in this slot now.');
    if (!cross && !within && !multi(idx)) {
      const s0 = insertOn(idx, p.from);
      if (p.from === input || (s0 && s0.stereo && (s0.input === first || s0.input + 1 === ch))) return no('same', 'HERE NOW', 'It is on this input already.');
    }
    if (!within) {
      if (aux && it.stereo === false) return no('stereo', 'MONO ONLY', `${label} has no stereo layout: an aux is stereo.`);
      if (stereo && !aux && it.stereo === false) return no('stereo', 'MONO ONLY', `Inputs ${first + 1}-${first + 2} are linked: ${label} has no stereo layout. Unlink the pair for a mono insert.`);
      if (stereo && !(h.stereo || (S.demo && idx === 0))) return no('stereo', 'NO STEREO', aux ? 'The installed apollo.sys starts plug-ins mono only: an aux needs stereo.' : 'Inputs are linked, and the installed apollo.sys starts plug-ins mono only.');
      if (!stereo && it.mono === false) return no('stereo', 'STEREO ONLY', `${label} runs stereo only: link inputs ${first + 1}-${first + 2} first.`);
    }
    const there = slotIns(idx, input, k) || (!multi(idx) && stereo && !aux ? insertOn(idx, input + 1) : null);
    if (there && (there.state === 'loading' || there.busy)) return no('busy', 'BUSY', `${there.label} is loading here.`);
    if (dst && dst.state === 'loading') return no('busy', 'BUSY', `${inputName(idx, input)} is being rebuilt${dst.busy ? `: ${dst.busy}` : ''}.`);
    if (cross) {
      const freed = there && there.name !== it.name && !allSlots(idx).some(i => !(i.chain === there.chain && i.slot === there.slot) && i.name === there.name) ? (there.codeWords || 0) : 0;
      const need = needs(idx, it);
      if (roomNow(idx) + freed < need) return no('room', 'NO ROOM', `No room on ${dspName(idx)}: ${label} needs ${fmtWords(need)} code words, ${fmtWords(roomNow(idx))} are free.`);
    }
    if (within) return { ok: true, code: '', short: there ? 'SWAP' : 'MOVE', why: there ? `Swaps with ${there.label}.` : '', input, stereo, there, within };
    return { ok: true, code: '', short: there ? 'REPLACE' : 'DROP', why: there ? `Replaces ${there.label}.` : '', input, stereo, there, within };
  }

  /* ---------------------------------------------------------------- placing: add, move, remove */

  const whereOf = (idx, r) => (isAux(r.input) ? inputName(idx, r.input) : r.stereo ? `${idx ? `${unitShort(idx)} inputs` : 'inputs'} ${r.input + 1}-${r.input + 2}` : inputName(idx, r.input));

  async function place(p, idx, ch, k = 0) {
    const r = dropCheck(p, idx, ch, k);
    cancel();
    if (!r.ok) { toast(r.why || 'That slot does not take it.', 'warn'); return false; }
    const label = p.kind === 'add' ? p.item.label : p.label, name = p.kind === 'add' ? p.item.name : p.name;
    const where = `${whereOf(idx, r)}${multi(idx) ? `, insert ${k + 1}` : ''}`;
    const slotField = multi(idx) ? { slot: k } : {};
    if (r.within) {
      send({ cmd: 'plug-move', unit: idx, ...tgt(r.input), from: p.slot, to: k });
      toast(r.there ? `${label} and ${r.there.label} swap places on ${whereOf(idx, r)}.` : `Moving ${label} to insert ${k + 1} of ${whereOf(idx, r)}.`);
      Skin.on('drop', slotNode(idx, ch, k));
      return true;
    }
    if (r.there) {
      const ok = await dialog({ title: `Replace ${r.there.label}?`, body: `${label} takes its place on ${where}. ${r.there.label}'s settings are not kept.`, confirm: 'Replace' });
      if (!ok) return false;
    }
    P.notes = P.notes.filter(n => !(n.unit === idx && n.input === r.input));
    if (p.kind === 'move') {
      const src = slotIns(p.unit, p.from, p.slot ?? 0);
      P.moves.set(`${idx}:${r.input}:${k}`, { unit: idx, input: r.input, slot: k, fromUnit: p.unit, from: p.from, fromSlot: p.slot ?? 0,
        values: src ? src.params.map(x => x.value) : [], name });
      toast(`Moving ${label} to ${where}: it starts there, takes its settings, then leaves ${inputName(p.unit, p.from)}.`);
    } else {
      toast(`Inserting ${label} on ${where}.`);
    }
    send({ cmd: 'plug-insert', unit: idx, ...tgt(r.input), ...slotField, name, stereo: !!r.stereo });
    Skin.on('drop', slotNode(idx, ch, k));
    return true;
  }

  /* A move whose new insert runs: its settings follow (one plug-param per parameter that differs), then the old goes. */
  function runMoves() {
    for (const [key, mv] of [...P.moves]) {
      const ins = slotIns(mv.unit, mv.input, mv.slot);
      if (!ins || ins.name !== mv.name || ins.state !== 'running') continue;
      P.moves.delete(key);
      const sl = multi(mv.unit) ? { slot: mv.slot } : {};
      ins.params.forEach((q, i) => {
        if (mv.values[i] !== undefined && mv.values[i] !== q.value) {
          P.pending.set(`${mv.unit}:${mv.input}:${mv.slot}:${i}`, mv.values[i]);
          send({ cmd: 'plug-param', unit: mv.unit, ...tgt(mv.input), ...sl, index: i, value: mv.values[i] });
        }
      });
      const src = slotIns(mv.fromUnit, mv.from, mv.fromSlot);
      if (src && !(mv.fromUnit === mv.unit && src.chain === ins.chain && src.slot === ins.slot)) {
        send({ cmd: 'plug-remove', unit: mv.fromUnit, ...tgt(src.input), ...(multi(mv.fromUnit) ? { slot: mv.fromSlot } : {}) });
      }
    }
  }

  async function remove(idx, ins) {
    const where = isAux(ins.input) ? inputName(idx, ins.input) : ins.stereo ? `${idx ? `${unitShort(idx)} inputs` : 'Inputs'} ${ins.input + 1}-${ins.input + 2}` : inputName(idx, ins.input);
    const rest = multi(idx) && (ins.chain.slots || []).length > 1;
    const ok = await dialog({ title: `Remove ${ins.label}?`, body: rest ? `${ins.label} leaves ${where}; the other inserts stay. Its settings are not kept.`
      : `${where} ${ins.stereo && !isAux(ins.input) ? 'go' : 'goes'} back to the plain strip. Its settings are not kept.`, confirm: 'Remove', kind: 'danger' });
    if (ok) send({ cmd: 'plug-remove', unit: idx, ...tgt(ins.input), ...(multi(idx) ? { slot: ins.slot } : {}) });
    return !!ok;
  }

  const bypass = (idx, ins) => send({ cmd: 'plug-bypass', unit: idx, ...tgt(ins.input), ...(multi(idx) ? { slot: ins.slot } : {}), on: !ins.bypass });

  /* ---------------------------------------------------------------- the Unison slot (docs 1.9.5) */

  /* why the Unison slot of input ch takes no pick now ('' when it does): no lease, a refusal, no Unison map */
  function unisonWhy(idx, ch) {
    const c = chainFor(idx, ch), note = P.notes.find(n => n.unit === idx && n.input === (pairLinked(idx, ch) ? ch & ~1 : ch) && n.code === 'unison');
    if (!st(idx).unison && !S.demo) return 'Unison: this apollo.sys has no Unison lease.';
    if (note) return `Unison: ${note.detail || 'no Unison map of this plug-in'}.`;
    if (c && c.code === 'unison' && c.why) return `Unison: ${c.why}`;
    if (!itemsOf(idx).some(i => i.unison === true)) return UNISON_NONE;
    return '';
  }

  function placeUnison(p, idx, ch) {
    cancel();
    const input = pairLinked(idx, ch) ? ch & ~1 : ch, why = unisonWhy(idx, ch);
    if (p.kind !== 'add') { toast('Only a new plug-in goes into the Unison slot.', 'warn'); return false; }
    if (why) { toast(why, 'warn'); return false; }
    if (p.item.unison !== true) { toast(`${p.item.label} has no Unison form.`, 'warn'); return false; }
    send({ cmd: 'plug-unison', unit: idx, input, name: p.item.name });
    toast(`Loading ${p.item.label} as the Unison preamp of ${inputName(idx, input)}.`);
    return true;
  }

  /* ---------------------------------------------------------------- REC / MON (docs 1.9.4) */

  /* REC: the record channel carries the inserts; MON: it records the dry input and the inserts are heard only */
  function recmonNode(idx, ch) {
    const sg = seg(['REC', 'MON'], i => {
      const input = pairLinked(idx, ch) ? ch & ~1 : ch, mon = i === 1;
      P.recmon.set(`${idx}:${input}`, mon);
      send({ cmd: 'plug-recmon', unit: idx, input, mon });
      renderSlots();
    }, `${inputName(idx, ch)}: inserts recorded (REC) or heard only (MON)`);
    sg.node.classList.add('ins-recmon');
    sg.node.dataset.unit = String(idx);
    sg.node.dataset.ch = String(ch);
    sg.node._set = sg.set;
    return sg.node;
  }

  /* ---------------------------------------------------------------- keyboard placement */

  function arm(p) {
    P.armed = p;
    P.target = null;
    hideCard();
    renderSlots();
    renderPlacing();
    /* focus the first empty slot that takes it (no accidental replace), else the first that does */
    const ok = slotNodes().filter(b => b.dataset.drop === 'ok');
    (ok.find(b => b.dataset.state === 'empty') || ok[0] || slotNodes()[0] || document.body).focus();
  }

  function cancel() {
    const was = P.armed || P.drag || P.target;
    P.armed = null;
    P.drag = null;
    P.target = null;
    document.body.classList.remove('pl-dragging');
    if (was) { renderSlots(); renderPlacing(); renderBrowser(); }
  }

  /* The status line over the strips while a plug-in is being placed (aria-live). */
  function renderPlacing() {
    const bar = document.getElementById('placeBar');
    if (!bar) return;
    const p = P.armed || P.drag;
    document.body.classList.toggle('pl-placing', !!p);
    if (!p && !P.target) { bar.hidden = true; bar.replaceChildren(); return; }
    const what = p ? (p.kind === 'add' ? p.item.label : p.label) : '';
    const text = p
      ? (p.kind === 'add' ? `PLACING ${what}: Enter on a lit slot loads it.` : `MOVING ${what}: Enter on a lit slot moves it, or drop it on the browser to unload.`)
      : `CHOOSE A PLUG-IN for ${inputName(P.target.idx, P.target.ch)}: Enter or double-click one in the browser.`;
    bar.hidden = false;
    bar.replaceChildren(el('span', { class: 'place-lamp', 'aria-hidden': 'true' }), el('span', { class: 'place-text', text }),
      el('span', { class: 'place-key num', text: 'ESC' }), el('button', { class: 'btn btn-quiet', onclick: cancel }, 'Cancel'));
  }

  /* ---------------------------------------------------------------- the strips' slots */

  const slotNodes = () => [...document.querySelectorAll('#inputs .ins-slot')];

  /*
   * The insert rows of one target (an input strip, or AUX_T + a for an aux): the Unison slot (inputs, shown while the
   * host composes chains), MAX_SLOTS slot rows in chain order (rows past the host's "slots" are left out), the chain's
   * state once, and a refused build's words (room, cycles, layout, unison) inline.
   */
  function slotNodesFor(idx, ch) {
    const aux = isAux(ch);
    const box = el('div', { class: `ins-slots${aux ? ' ins-aux' : ''}`, role: 'group', dataset: { unit: String(idx), ch: String(ch) },
      'aria-label': `${inputName(idx, ch).replace(/ \(.*\)$/, '')} inserts` });
    if (!aux) box.append(unisonRow(idx, ch));
    box.append(slotRow(idx, ch, 0));                       /* renderBox adds the rows the host's "slots" asks for */
    box.append(el('div', { class: 'ins-chain num', 'aria-live': 'polite', hidden: true }), el('div', { class: 'ins-refusal', role: 'status', hidden: true }));
    return box;
  }

  function slotRow(idx, ch, k) {
    const b = el('button', { class: 'ins-slot', type: 'button', dataset: { ch: String(ch), unit: String(idx), n: String(k) } },
      el('span', { class: 'ins-lamp', 'aria-hidden': 'true' }), el('span', { class: 'ins-name' }), el('span', { class: 'ins-why', 'aria-hidden': 'true' }));
    const x = el('button', { class: 'ins-x', type: 'button', tabindex: '-1', title: 'Remove (or drag the slot onto the browser)' }, '×');
    b.addEventListener('click', () => slotClick(idx, ch, k));
    b.addEventListener('keydown', e => slotKey(e, idx, ch, k));
    b.addEventListener('dragstart', e => slotDragStart(e, idx, ch, k));
    b.addEventListener('dragend', () => { if (P.drag) cancel(); });
    b.addEventListener('dragover', e => slotDragOver(e, idx, ch, k));
    b.addEventListener('dragleave', () => b.classList.remove('over'));
    b.addEventListener('drop', e => slotDrop(e, idx, ch, k));
    x.addEventListener('click', () => { const ins = slotIns(idx, ch, k); if (ins) remove(idx, ins); });
    Skin.decorate('slot', b);
    return el('div', { class: 'ins-row', dataset: { n: String(k) } }, b, x);
  }

  /* the Unison slot: above the inserts, drawn disabled with its reason while it takes no pick */
  function unisonRow(idx, ch) {
    const b = el('button', { class: 'ins-uni', type: 'button', dataset: { ch: String(ch), unit: String(idx), n: 'u' } },
      el('span', { class: 'ins-lamp', 'aria-hidden': 'true' }), el('span', { class: 'ins-name' }));
    const x = el('button', { class: 'ins-x', type: 'button', tabindex: '-1', title: 'Remove the Unison preamp' }, '×');
    b.addEventListener('click', () => {
      const p = P.armed, ins = slotIns(idx, ch, 'u');
      if (p) { placeUnison(p, idx, ch); return; }
      if (ins) { toast(`${ins.label}: ${ins.busy || STATE_TEXT[ins.state] || ins.state}. B bypasses it, Delete removes it.`); return; }
      toast(unisonWhy(idx, ch) || 'Pick a plug-in with a Unison form in the browser, then this slot.', 'info');
    });
    b.addEventListener('keydown', e => {
      const ins = slotIns(idx, ch, 'u'), k = e.key.toLowerCase();
      if (!ins || P.armed) return;
      if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); send({ cmd: 'plug-unison-remove', unit: idx, input: ins.input }); }
      else if (k === 'b' && !e.ctrlKey && !e.altKey) { e.preventDefault(); e.stopPropagation(); send({ cmd: 'plug-bypass', unit: idx, input: ins.input, slot: 'unison', on: !ins.bypass }); }
    });
    b.addEventListener('dragover', e => { if (P.drag && P.drag.kind === 'add' && !unisonWhy(idx, ch)) e.preventDefault(); });
    b.addEventListener('drop', e => { e.preventDefault(); if (P.drag) placeUnison(P.drag, idx, ch); });
    x.addEventListener('click', () => { const ins = slotIns(idx, ch, 'u'); if (ins) send({ cmd: 'plug-unison-remove', unit: idx, input: ins.input }); });
    return el('div', { class: 'ins-row ins-uni-row', dataset: { n: 'u' } }, b, x);
  }

  const isOpen = (idx, input, k = 0) => !!P.open && P.open.unit === idx && P.open.input === input && (P.open.slot ?? 0) === k;

  function slotClick(idx, ch, k) {
    const p = P.armed;
    if (p) { place(p, idx, ch, k); return; }
    const b = unitBlock(idx);
    if (b && b.code === 'unit') { toast(b.text, 'info'); return; }
    if (isAux(ch) && !auxInsertsOn(idx)) { const c = chainFor(idx, ch); toast((c && c.why) || AUX_OFF, 'info'); return; }
    const ins = slotIns(idx, ch, k);
    if (ins) { openDrawer(isOpen(idx, ins.input, k) ? null : { unit: idx, input: ins.input, slot: k }); return; }
    /* an empty slot: the next plug-in picked in the browser goes here (the browser lists this unit's) */
    P.target = { idx, ch, k };
    renderSlots();
    renderPlacing();
    renderBrowser();
    focusSearch();
  }

  function slotKey(e, idx, ch, k) {
    const ins = slotIns(idx, ch, k), key = e.key.toLowerCase();
    if (e.key === 'Escape') { if (P.armed || P.target) { cancel(); e.preventDefault(); } else if (P.open) { openDrawer(null); e.preventDefault(); } return; }
    if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && !e.ctrlKey && !e.altKey) {
      /* between the rows of one strip */
      const rows = [...(e.currentTarget.closest('.ins-slots') || document).querySelectorAll('.ins-row:not([hidden]) > button:first-child')];
      const i = rows.indexOf(e.currentTarget), j = i + (e.key === 'ArrowDown' ? 1 : -1);
      if (i >= 0 && rows[j]) { rows[j].focus(); e.preventDefault(); }
      return;
    }
    if (P.armed) return;                                      /* Enter / Space click: place */
    if (!ins) return;
    if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); remove(idx, ins); }
    else if (key === 'm' && !e.ctrlKey && !e.altKey) { e.preventDefault(); e.stopPropagation(); arm({ kind: 'move', unit: idx, from: ins.input, slot: k, name: ins.name, label: ins.label }); }
    else if (key === 'b' && !e.ctrlKey && !e.altKey) { e.preventDefault(); e.stopPropagation(); bypass(idx, ins); }
  }

  function slotDragStart(e, idx, ch, k) {
    const ins = slotIns(idx, ch, k);
    if (!ins || ins.state === 'loading') { e.preventDefault(); return; }
    P.drag = { kind: 'move', unit: idx, from: ins.input, slot: k, name: ins.name, label: ins.label };
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData(MIME, `move:${idx}:${ins.input}:${k}`);
    e.dataTransfer.setData('text/plain', ins.label);
    startDragUi();
  }

  function slotDragOver(e, idx, ch, k) {
    if (!P.drag) return;
    const r = dropCheck(P.drag, idx, ch, k);
    e.currentTarget.classList.toggle('over', r.ok);
    if (r.ok) { e.preventDefault(); e.dataTransfer.dropEffect = P.drag.kind === 'move' ? 'move' : 'copy'; }
  }

  function slotDrop(e, idx, ch, k) {
    e.preventDefault();
    e.currentTarget.classList.remove('over');
    const p = P.drag;
    if (p) place(p, idx, ch, k);
  }

  function startDragUi() {
    hideCard();
    document.body.classList.add('pl-dragging');
    /* defer: changing the DOM inside dragstart can cancel the drag */
    setTimeout(() => { if (P.drag) { renderSlots(); renderPlacing(); renderBrowserHint(); } }, 0);
  }

  const fmtLat = n => `${n | 0} smp`;
  /* the strip's REC / MON tooltip: what each does, the record delay, and whether the chain's latency is over budget */
  function recmonTitle(c, mon) {
    const base = mon ? 'MON: the input is recorded dry; its inserts are heard in the monitor mix only.' : 'REC: the inserts are recorded (the record channel carries them).';
    if (!c || c.state === 'idle') return `${base} No insert now: the switch is kept for the next chain.`;
    const delay = Number.isFinite(c.recordDelay) ? ` Record delay ${c.recordDelay} smp.` : '';
    return `${base}${delay} Insert latency ${fmtLat(c.latency)}${c.overloaded ? ': over the 200-sample budget, so the record lags.' : '.'}`;
  }

  /* One slot: the insert's name, its state light and, while a plug-in is placed, whether this slot takes it. */
  function renderSlots() {
    const p = P.armed || P.drag;
    for (const box of document.querySelectorAll('#inputs .ins-slots')) renderBox(box, p);
    for (const rm of document.querySelectorAll('#inputs .ins-recmon')) {
      const idx = Number(rm.dataset.unit), ch = Number(rm.dataset.ch), c = chainFor(idx, ch);
      const show = !!st(idx).recmon && !(unitBlock(idx) || {}).code;
      rm.hidden = !show && !(S.demo && idx === 0);
      const input = pairLinked(idx, ch) ? ch & ~1 : ch;
      const mon = c && noAux(c) ? !!c.mon : !!P.recmon.get(`${idx}:${input}`);
      if (rm._set) rm._set(mon ? 1 : 0);
      rm.title = recmonTitle(c, mon);
      rm.classList.toggle('over', !!(c && c.overloaded));
      rm.dataset.mode = mon ? 'mon' : 'rec';
    }
  }

  function renderBox(box, p) {
    const idx = Number(box.dataset.unit), ch = Number(box.dataset.ch), aux = isAux(ch);
    const blk = unitBlock(idx), na = (!!blk && blk.code === 'unit') || (!aux && ch >= inputsOf(idx));
    const off = aux && !na && !auxInsertsOn(idx);
    const auxHidden = aux && !multi(idx) && !(S.demo && idx === 0);    /* a 1.5 host: no aux slots at all */
    const rows = auxHidden ? 0 : na ? 1 : slotCount(idx), c = na ? null : chainFor(idx, ch);
    box.hidden = auxHidden;
    box.dataset.slots = String(rows);
    const have = [...box.querySelectorAll(':scope > .ins-row:not(.ins-uni-row)')];
    if (have.length !== rows) {
      for (const r of have.slice(rows)) r.remove();
      const before = box.querySelector(':scope > .ins-chain');
      for (let k = have.length; k < rows; k++) box.insertBefore(slotRow(idx, ch, k), before);
    }
    /* the narrow layouts show the used rows and the next empty one (every row while a plug-in is placed) */
    const used = c ? Math.max(-1, ...(c.slots || []).map(s => s.slot)) : -1;
    for (const row of box.querySelectorAll('.ins-row')) {
      const n = row.dataset.n;
      if (n === 'u') { renderUnison(row, idx, ch, na); continue; }
      const k = Number(n);
      row.hidden = k >= rows;
      row.classList.toggle('ins-extra', k > used + 1);
      if (row.hidden) continue;
      renderSlot(row, idx, ch, k, p, { na, off, blk, rows });
    }
    /* the chain's state once per strip (each slot shows its own) */
    const line = box.querySelector('.ins-chain'), ref = box.querySelector('.ins-refusal');
    const busy = c && (c.state === 'loading' || c.busy);
    const show = !!c && !na && multi(idx) && c.state && c.state !== 'idle' && (c.state !== 'running' || busy || c.overloaded);
    line.hidden = !show;
    if (show) {
      const t = `${(STATE_TEXT[c.state] || c.state).toUpperCase()}${c.busy ? `: ${c.busy}` : ''}${c.state === 'running' ? ` · LAT ${c.latency | 0}${c.overloaded ? ' OVER' : ''}` : ''}${c.load && c.load.after ? ` · LD ${(c.load.after / 100).toFixed(1)}%` : ''}`;
      if (line.textContent !== t) line.textContent = t;
      line.dataset.state = c.state;
      line.title = `${inputName(idx, c.input ?? ch)}: the chain ${STATE_TEXT[c.state] || c.state}${c.why ? `. ${c.why}` : ''}${c.load ? `. DSP 1 load ${((c.load.before || 0) / 100).toFixed(1)}% before, ${((c.load.after || 0) / 100).toFixed(1)}% after the last build` : ''}.`;
    }
    /* a refused build's words inline under the strip's inserts */
    const first = aux ? ch : pairLinked(idx, ch) ? ch & ~1 : ch;
    const note = na ? null : P.notes.find(n => n.unit === idx && n.input === first && ['room', 'cycles', 'layout', 'unison', 'aux'].includes(n.code));
    const why = note ? `${CODES[note.code] || 'Refused'}: ${note.detail || note.text}` : (c && c.state === 'failed' && ['room', 'cycles', 'layout'].includes(c.code) && c.why ? `${CODES[c.code]}: ${c.why}` : '');
    ref.hidden = !why;
    if (ref.textContent !== why) ref.textContent = why;
    ref.dataset.code = note ? note.code : c ? c.code || '' : '';
  }

  function renderSlot(row, idx, ch, k, p, o) {
    const b = row.querySelector('.ins-slot'), x = row.querySelector('.ins-x');
    const name = b.querySelector('.ins-name'), why = b.querySelector('.ins-why');
    const who = `${inputName(idx, ch).replace(/ \(.*\)$/, '')} insert${o.rows > 1 ? ` ${k + 1}` : ''}`;
    const ins = o.na ? null : slotIns(idx, ch, k);
    const note = !o.na ? P.notes.find(n => n.unit === idx && (n.input === (isAux(ch) ? ch : ch & ~1) || n.input === ch) && n.op !== 'param' && (n.slot === undefined || n.slot === k || n.slot === 0 && !multi(idx))) : null;
    let state = 'empty', text = o.rows > 1 ? `${k + 1} ·` : 'INSERT', label = `${who}: empty`;
    if (o.na) { state = 'na'; text = 'N/A'; label = `${who}: ${o.blk && o.blk.code === 'unit' ? 'plug-ins are not supported on this driver build' : 'no plug-in slot on this input'}`; }
    else if (o.off && !ins) { state = 'off'; text = k === 0 ? 'AUX INS OFF' : '-'; label = `${who}: aux inserts are off`; }
    else if (ins) {
      state = ins.state;
      text = ins.label + (ins.stereo && !isAux(ch) ? (ch === ins.input ? ' L' : ' R') : '');
      label = `${who}: ${ins.label}, ${ins.busy || STATE_TEXT[ins.state] || ins.state}`;
    }
    b.dataset.state = state;
    row.dataset.state = state;
    b.dataset.refused = note ? '1' : '0';
    b.draggable = !!ins && ins.state !== 'loading';
    b.classList.toggle('target', !!P.target && P.target.idx === idx && P.target.ch === ch && (P.target.k ?? 0) === k);
    b.classList.toggle('open', !!ins && isOpen(idx, ins.input, k));
    if (name.textContent !== text) name.textContent = text;
    const off = o.off ? ((chainFor(idx, ch) || {}).why || AUX_OFF) : '';
    b.title = o.na ? (o.blk && o.blk.code === 'unit' ? o.blk.text : 'No plug-in slot on this input.')
      : ins ? `${ins.label}: ${ins.busy || STATE_TEXT[ins.state] || ins.state}${ins.why && ins.state !== 'running' ? `. ${ins.why}` : ''}. Enter: parameters, M: move, B: bypass, Delete: remove.`
        : off || (note ? note.text : 'Drag a plug-in here, or click and pick one in the browser.');
    if (p) {
      const r = dropCheck(p, idx, ch, k);
      b.dataset.drop = r.ok ? 'ok' : 'no';
      why.textContent = r.short;
      b.setAttribute('aria-label', `${label}. ${r.ok ? `Takes ${p.kind === 'add' ? p.item.label : p.label}${r.there ? `, ${r.within ? 'swapping with' : 'replacing'} ${r.there.label}` : ''}` : `Does not take it: ${r.why}`}`);
      if (!r.ok) b.title = r.why;
    } else {
      delete b.dataset.drop;
      why.textContent = '';
      b.setAttribute('aria-label', off && !ins ? `${label}. ${off}` : label);
    }
    b.setAttribute('aria-disabled', o.off && !ins ? 'true' : 'false');
    x.hidden = !ins;
    x.setAttribute('aria-label', ins ? `Remove ${ins.label} from ${inputName(idx, ins.input)}` : 'Remove');
  }

  function renderUnison(row, idx, ch, na) {
    const b = row.querySelector('.ins-uni'), x = row.querySelector('.ins-x'), name = b.querySelector('.ins-name');
    row.hidden = na || !multi(idx) || (ch & 1 && pairLinked(idx, ch));
    if (row.hidden) return;
    const ins = slotIns(idx, ch, 'u'), why = ins ? '' : unisonWhy(idx, ch);
    const state = ins ? ins.state : why ? 'off' : 'empty';
    b.dataset.state = state;
    row.dataset.state = state;
    b.setAttribute('aria-disabled', why ? 'true' : 'false');
    const text = ins ? ins.label : 'UNISON';
    if (name.textContent !== text) name.textContent = text;
    const who = `${inputName(idx, ch).replace(/ \(.*\)$/, '')} Unison preamp`;
    b.title = ins ? `${ins.label} (Unison): ${ins.busy || STATE_TEXT[ins.state] || ins.state}. B: bypass, Delete: remove.` : why || 'Unison: pick a plug-in with a Unison form, then this slot.';
    b.setAttribute('aria-label', ins ? `${who}: ${ins.label}, ${STATE_TEXT[ins.state] || ins.state}` : `${who}: ${why ? `not available. ${why}` : 'empty'}`);
    if (P.armed || P.drag) b.dataset.drop = !why && (P.armed || P.drag).kind === 'add' && (P.armed || P.drag).item.unison === true ? 'ok' : 'no';
    else delete b.dataset.drop;
    x.hidden = !ins;
  }

  /* ---------------------------------------------------------------- DSP room (the browser head, every segment head) */

  const dspNodes = new Map();           /* node -> unit slot (or -1: the browser's, which follows browserUnit) */

  function dspNode(idx) {
    const n = el('div', { class: 'dsp-meter', dataset: { unit: String(idx) } });
    dspNodes.set(n, idx);
    renderDspNode(idx, n);
    return n;
  }

  /* The unit's strip DSP code pool (pool 0) as a bar: the strips and inserts | free, with the words; the data pools as
     thin bars when the host sends the ledger's pools; the load word raw. A unit that does not host: says so. */
  function renderDspNode(idx, n) {
    if (!n) return;
    const blk = unitBlock(idx);
    if (blk && blk.code === 'unit') {
      if (n.dataset.key !== 'na') {
        n.dataset.key = 'na';
        n.replaceChildren(el('span', { class: 'dsp-lbl', text: 'DSP' }), el('span', { class: 'dsp-na', text: 'plug-ins: not on this driver build' }));
      }
      return;
    }
    const s = st(idx), d = s.dsp || {}, total = d.total || 16384, have = !!P.sts[idx], room = d.room ?? 0;
    const pools = Array.isArray(d.pools) ? d.pools : [];
    const key = JSON.stringify([idx, total, room, d.estimate, d.load, d.index, pools, have]);
    if (n.dataset.key === key) return;
    n.dataset.key = key;
    const used = have ? total - room : 0;
    const pct = v => `${clamp(v / total * 100, 0, 100).toFixed(1)}%`;
    const bar = el('div', { class: 'dsp-bar', role: 'meter', 'aria-label': `${unitShort(idx) || 'x8p'} ${dspName(idx)} code pool in use`, 'aria-valuemin': '0',
      'aria-valuemax': String(total), 'aria-valuenow': String(used), title: `${dspName(idx)} code pool (pool 0): the strips' words and every insert's code` },
    el('span', { class: 'dsp-used', style: `width:${pct(used)}` }));
    bar.dataset.low = have && room < total * 0.1 ? '1' : '0';
    const kids = [el('span', { class: 'dsp-lbl', text: dspName(idx).replace(' ', '') }), bar,
      el('span', { class: 'dsp-txt num', text: have ? `${fmtWords(room)} w free${d.estimate ? '*' : ''}` : 'no data',
        title: d.estimate ? 'Estimated: 16,384 less the strips\' 994 until the first insert reads the ledger' : null })];
    if (pools.length > 1) {
      const thin = pools.slice(1).map((q, i) => el('span', { class: 'dsp-pool', title: `Data pool ${i + 1}: ${fmtWords(q.used)} of ${fmtWords(q.total)} words used, ${fmtWords(q.free)} free` },
        el('span', { class: 'dsp-pool-fill', style: `width:${q.total ? clamp(q.used / q.total * 100, 0, 100).toFixed(1) : 0}%` })));
      kids.push(el('span', { class: 'dsp-pools', 'aria-hidden': 'true' }, thin));
    }
    if (d.load) kids.push(el('span', { class: 'dsp-load num', title: `${dspName(idx)} load word (window +0x0C), raw`, text: `LD ${d.load}` }));
    n.replaceChildren(...kids);
  }

  function renderDsp() {
    for (const [n, idx] of [...dspNodes]) {
      if (idx >= 0 && !n.isConnected && !document.getElementById('inputs').contains(n)) { if (!n.closest('.useg')) dspNodes.delete(n); }
      renderDspNode(idx < 0 ? browserUnit() : idx, n);
    }
  }

  /* ---------------------------------------------------------------- the browser */

  function build() {
    const root = document.getElementById('plb');
    if (!root) return;
    const u = P.ui;
    u.root = root;
    u.toggle = el('button', { class: 'btn btn-quiet plb-toggle', 'aria-expanded': 'true', 'aria-controls': 'plbBody', title: 'Show or hide the plug-in browser' }, '‹');
    u.toggle.addEventListener('click', () => setCollapsed(!cfg.plb.closed));
    u.count = el('span', { class: 'plb-count num' });
    u.unit = el('span', { class: 'plb-unit num' });
    u.chip = el('span', { class: 'plb-chip', dataset: { kind: 'off' } });
    u.search = el('input', { class: 'text-in plb-search', type: 'search', placeholder: 'Search  (Ctrl+F)', 'aria-label': 'Search plug-ins', spellcheck: 'false' });
    u.search.addEventListener('input', () => { P.search = u.search.value; renderBrowser(); });
    u.search.addEventListener('keydown', e => {
      if (e.key === 'ArrowDown') { e.preventDefault(); const f = itemNodes()[0]; if (f) f.focus(); }
      else if (e.key === 'Escape') { if (P.target || P.armed) cancel(); else { u.search.value = ''; P.search = ''; renderBrowser(); } }
      e.stopPropagation();
    });
    u.fitsLab = el('span', { class: 'sw-lab', text: 'FITS DSP1' });
    u.fits = el('button', { class: 'sw plb-fits', 'aria-pressed': 'false', title: 'Only plug-ins that are licensed and fit the DSP\'s free room now' }, u.fitsLab);
    Skin.decorate('button', u.fits);
    u.fits.addEventListener('click', () => { P.fits = !P.fits; renderBrowser(); });
    u.room = el('div', { class: 'plb-room dsp-meter' });
    dspNodes.set(u.room, -1);
    u.notes = el('div', { class: 'plb-notes', 'aria-live': 'polite' });
    u.hint = el('div', { class: 'plb-hint' });
    u.list = el('div', { class: 'plb-list', role: 'list', 'aria-label': 'Plug-ins by type' });
    u.foot = el('div', { class: 'plb-foot fine' });
    u.drop = el('div', { class: 'plb-drop', 'aria-hidden': 'true' }, el('span', { text: 'DROP HERE TO UNLOAD' }));
    u.body = el('div', { class: 'plb-body', id: 'plbBody' },
      el('div', { class: 'plb-tools' }, u.search, u.fits), u.room, u.notes, u.hint, u.list, u.foot);
    root.replaceChildren(el('div', { class: 'plb-head' }, el('span', { class: 'plb-title', text: 'Plug-ins' }), u.unit, u.count, u.chip, u.toggle), u.body, u.drop);
    root.addEventListener('dragover', e => { if (P.drag && P.drag.kind === 'move') { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; root.classList.add('unload'); } });
    root.addEventListener('dragleave', e => { if (!root.contains(e.relatedTarget)) root.classList.remove('unload'); });
    root.addEventListener('drop', e => {
      root.classList.remove('unload');
      const p = P.drag;
      if (!p || p.kind !== 'move') return;
      e.preventDefault();
      cancel();
      const ins = slotIns(p.unit, p.from, p.slot ?? 0);
      if (ins) remove(p.unit, ins);
    });
    buildCard();
    P.built = true;
    setCollapsed(!!cfg.plb.closed, true);
  }

  function setCollapsed(closed, quiet) {
    cfg.plb.closed = closed;
    const v = document.getElementById('view-control');
    if (v) v.dataset.plb = closed ? 'closed' : 'open';
    P.ui.toggle.setAttribute('aria-expanded', closed ? 'false' : 'true');
    P.ui.toggle.textContent = closed ? '›' : '‹';
    P.ui.toggle.setAttribute('aria-label', closed ? 'Show the plug-in browser' : 'Hide the plug-in browser');
    P.ui.body.hidden = closed;
    if (!quiet) saveConfig();
  }

  function focusSearch() {
    if (!P.built) return;
    if (cfg.plb.closed) setCollapsed(false);
    P.ui.search.focus();
    P.ui.search.select();
  }

  const itemNodes = () => [...P.ui.list.querySelectorAll('.plb-item')];

  function renderBrowser() {
    if (!P.built) return;
    const u = P.ui, bu = browserUnit(), l = listOf(bu) || listOf(0), s = st(bu), h = s.hosting || {};
    /* chip: hosting of the unit listed */
    let chip = 'HOSTING OFF', kind = 'off';
    const blk = unitBlock(bu);
    if (S.demo && bu === 0) { chip = 'DEMO'; kind = 'on'; }
    else if (h.admin) { chip = 'ADMIN STEP'; kind = 'busy'; }
    else if (blk && blk.code === 'unit') chip = 'NOT ON BUILD';
    else if (!s.present) chip = 'OFFLINE';
    else if (!h.driver) chip = 'NO HOST';
    else if (h.on && h.caller) { chip = 'HOSTING ON'; kind = 'on'; }
    else if (h.on) { chip = 'OTHER PROGRAM'; kind = 'bad'; }
    u.chip.textContent = chip;
    u.chip.dataset.kind = kind;
    u.chip.title = h.admin || (blk ? blk.text : 'Plug-in hosting is on for this Apollo Control');
    const many = Object.keys(P.sts).filter(k => hosts(Number(k))).length > 1 || bu !== 0;
    u.unit.textContent = many ? `· ${(unitShort(bu) || 'x8p').toUpperCase()}` : '';
    u.unit.title = many ? `Licences, room and fit for the ${unitShort(bu)}` : '';
    setPressed(u.fits, P.fits);
    u.fitsLab.textContent = `FITS ${dspName(bu).replace(' ', '')}`;
    renderDspNode(bu, u.room);
    renderNotes();
    renderBrowserHint();
    /* the list, grouped; licences and fit are the listed unit's */
    const all = (l && l.items) || [], q = P.search.trim().toLowerCase();
    const licOf = it => (listOf(bu) ? (itemFor(bu, it.name) || {}).licensed : it.licensed);
    const shown = all.filter(it => (!q || `${it.label} ${it.name}`.toLowerCase().includes(q) || GROUP[categorize(it).id].name.toLowerCase().includes(q)) &&
      (!P.fits || (licOf(it) === true && fits(bu, it))));
    const lic = all.filter(it => licOf(it) === true).length;
    u.count.textContent = l && l.state === 'ok' ? `${lic}/${all.length}` : '';
    u.count.title = `${lic} of ${all.length} mapped plug-ins are licensed on ${unitWord(bu)}`;
    const br = Skin.r('browser') || {};
    u.root.dataset.layout = br.layout || 'list';
    const key = JSON.stringify([Skin.id(), bu, shown.map(i => [i.name, licOf(i), fits(bu, i), i.art]), [...P.closed], P.sel, roomNow(bu), P.fits, P.sort]);
    if (u.listKey !== key) {
      u.listKey = key;
      const focused = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.name : '';
      const ctx = { categorize, bu, licOf };
      let groups;
      if (br.flat) groups = [{ g: { id: 'all', name: 'All' }, list: sortFlat(shown, bu, licOf), flat: true }];
      else if (br.groups) groups = br.groups(shown, ctx).map(x => ({ g: x, list: x.items, all: x.items }));
      else groups = GROUPS.map(g => ({ g, list: shown.filter(it => categorize(it).id === g.id) })).filter(x => x.list.length);
      u.list.replaceChildren(...(br.flat ? [tableHead(br)] : []), ...groups.map(({ g, list, flat, all: ga }) => groupNode(g, list, ga || all, bu, licOf, flat)));
      if (!groups.length) u.list.append(el('p', { class: 'fine plb-empty', text: l && l.state === 'ok' ? 'No plug-in matches.' : '' }));
      if (focused) { const n = itemNodes().find(x => x.dataset.name === focused); if (n) { P.ui.quiet = true; n.focus(); P.ui.quiet = false; } }
      rovingFix();
    }
    let foot = '';
    if (!l) foot = blk && blk.code === 'unit' ? blk.text : 'Reading the plug-in maps...';
    else if (l.state === 'loading') foot = 'Reading the plug-in maps (the first time takes a while)...';
    else if (l.state === 'failed') foot = l.why || 'No plug-in maps on this PC.';
    else {
      foot = `${l.authorized} of ${l.mapped} mapped plug-ins are licensed on ${unitWord(bu)}.`;
      if (l.licence !== 'ok') foot = `${l.mapped} mapped plug-ins; ${unitWord(bu)}'s licence answer: ${l.licenceWhy || 'not read yet'}.`;
    }
    u.foot.textContent = foot;
  }

  /* the flat table of a pack with columns (Lab, Mono): sorted by the column the user picked */
  const SORTS = {
    name: it => it.label.toLowerCase(), type: it => GROUP[categorize(it).id].name, code: it => it.codeWords || 0,
    lat: it => it.latency || 0, par: it => it.params || 0,
  };
  function sortFlat(list, bu, licOf) {
    const s0 = P.sort || { key: 'name', dir: 1 }, f = s0.key === 'lic' ? (it => (licOf(it) === true ? 0 : licOf(it) === false ? 2 : 1)) : SORTS[s0.key] || SORTS.name;
    return [...list].sort((a, b) => { const x = f(a), y = f(b); return (x < y ? -1 : x > y ? 1 : 0) * s0.dir || a.label.localeCompare(b.label); });
  }
  function tableHead(br) {
    const s0 = P.sort || { key: 'name', dir: 1 };
    return el('div', { class: 'plb-thead', role: 'presentation' }, (br.columns || []).map(([k, t]) => {
      const b = el('button', { class: `plb-th lb-${k}`, 'aria-label': `Sort by ${t}`, 'aria-pressed': s0.key === k ? 'true' : 'false',
        onclick: () => { P.sort = { key: k, dir: s0.key === k ? -s0.dir : 1 }; renderBrowser(); } }, t + (s0.key === k ? (s0.dir > 0 ? ' ▲' : ' ▼') : ''));
      return b;
    }));
  }

  function groupNode(g, list, all, bu, licOf, flat) {
    const ordered0 = flat ? list : null;
    if (flat) {
      return el('div', { class: 'plb-group plb-flat', role: 'listitem', dataset: { group: 'all' } },
        el('div', { role: 'group', 'aria-label': `All plug-ins, ${list.length}` }, el('div', { class: 'plb-items', role: 'list' }, ordered0.map(it => itemNode(it, bu, licOf(it))))));
    }
    const closed = P.closed.has(g.id) && !P.search;
    const total = (all.some(it => categorize(it).id === g.id) ? all.filter(it => categorize(it).id === g.id) : all).length;
    const head = el('button', { class: 'plb-group-head', 'aria-expanded': closed ? 'false' : 'true', title: g.sub ? `${g.name}: ${g.sub}` : g.name },
      el('span', { class: 'plb-caret', 'aria-hidden': 'true', text: closed ? '▸' : '▾' }), el('span', { class: 'plb-group-name', text: g.name }),
      el('span', { class: 'plb-group-n num', text: list.length === total ? String(total) : `${list.length}/${total}` }));
    head.addEventListener('click', () => { if (P.closed.has(g.id)) P.closed.delete(g.id); else P.closed.add(g.id); renderBrowser(); });
    const ordered = [...list].sort((a, b) => (licOf(b) === true) - (licOf(a) === true) || a.label.localeCompare(b.label));
    const ul = el('div', { class: 'plb-items', role: 'list' }, closed ? [] : ordered.map(it => itemNode(it, bu, licOf(it))));
    return el('div', { class: 'plb-group', role: 'listitem', dataset: { group: g.id } },
      el('div', { role: 'group', 'aria-label': `${g.name}, ${list.length} plug-ins` }, head, ul));
  }

  function itemNode(it0, bu, lic) {
    const it = { ...it0, licensed: lic }, f = fits(bu, it), need = needs(bu, it);
    const tag = lic === false ? 'NO LIC' : lic !== true ? 'LIC ?' : !f ? 'NO ROOM' : '';
    const n = el('div', {
      class: 'plb-item', role: 'listitem', tabindex: '-1', draggable: lic === true ? 'true' : 'false',
      dataset: { name: it.name, lic: lic === true ? '1' : lic === false ? '0' : '?', fits: f ? '1' : '0' },
      'aria-label': `${it.label}, ${GROUP[categorize(it).id].name}, ${need ? `${fmtWords(need)} code words` : 'code shared'}${tag ? `, ${tag.toLowerCase()}` : ''}`,
      'aria-selected': P.sel === it.name ? 'true' : 'false',
    },
    preview(it, 'plb-art'),
    el('span', { class: 'plb-name', text: it.label }),
    el('span', { class: 'plb-words num', text: need ? `${(need / 1000).toFixed(1)}k` : '0' }),
    tag ? el('span', { class: 'plb-tag', text: tag }) : el('span', { class: 'plb-lamp', 'aria-hidden': 'true' }));
    const br = Skin.r('browser');
    if (br && br.item) br.item(n, it, { need, group: GROUP_SHORT[categorize(it).id] || '', categorize, unit: bu }, Skin.R);
    n.addEventListener('click', () => { P.sel = it.name; for (const x of itemNodes()) x.setAttribute('aria-selected', x === n ? 'true' : 'false'); n.focus(); showCard(it, n); });
    n.addEventListener('dblclick', () => pick(it));
    n.addEventListener('keydown', e => itemKey(e, it, n));
    n.addEventListener('focus', () => { rovingSet(n); if (!P.ui.quiet) showCard(it, n); });
    n.addEventListener('blur', () => hideCardSoon());
    n.addEventListener('mouseenter', () => { clearTimeout(P.ui.cardTimer); P.ui.cardTimer = setTimeout(() => showCard(it, n), 180); });
    n.addEventListener('mouseleave', () => { clearTimeout(P.ui.cardTimer); hideCardSoon(); });
    n.addEventListener('dragstart', e => {
      if (lic !== true) { e.preventDefault(); toast(`${it.label} is not licensed on ${unitWord(bu)}.`, 'warn'); return; }
      P.drag = { kind: 'add', item: it };
      e.dataTransfer.effectAllowed = 'copy';
      e.dataTransfer.setData(MIME, `add:${it.name}`);
      e.dataTransfer.setData('text/plain', it.label);
      startDragUi();
    });
    n.addEventListener('dragend', () => { if (P.drag) cancel(); });
    return n;
  }

  /* Enter, Space or a double-click: into the slot the user came from, else pick it up for the keyboard. */
  function pick(it) {
    if (P.target) { const t = P.target; place({ kind: 'add', item: it }, t.idx, t.ch, t.k ?? 0); return; }
    arm({ kind: 'add', item: it });
  }

  function itemKey(e, it, n) {
    const list = itemNodes(), i = list.indexOf(n);
    const go = j => { const t = list[clamp(j, 0, list.length - 1)]; if (t) t.focus(); };
    if (e.key === 'ArrowDown') { e.preventDefault(); go(i + 1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); if (i === 0) P.ui.search.focus(); else go(i - 1); }
    else if (e.key === 'Home') { e.preventDefault(); go(0); }
    else if (e.key === 'End') { e.preventDefault(); go(list.length - 1); }
    else if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); P.sel = it.name; pick(it); }
    else if (e.key === 'Escape') { if (P.target || P.armed) cancel(); hideCard(); }
    else return;
    e.stopPropagation();
  }

  /* one tab stop in the list: the selected or first item */
  function rovingSet(n) { for (const x of itemNodes()) x.tabIndex = x === n ? 0 : -1; }
  function rovingFix() {
    const list = itemNodes();
    if (!list.length || list.some(x => x.tabIndex === 0)) return;
    (list.find(x => x.dataset.name === P.sel) || list[0]).tabIndex = 0;
  }

  /* Notes: every hosting unit's block (and the x8p's always), then the refusals, each with Dismiss. */
  function renderNotes() {
    const lines = [];
    const units = [...new Set([0, ...Object.keys(P.sts).map(Number)])];
    for (const idx of units) {
      const b = unitBlock(idx);
      if (b && b.code !== 'unit') lines.push({ kind: 'info', text: idx ? `${unitShort(idx)}: ${b.text}` : b.text });
    }
    for (const n of P.notes) lines.push({ kind: 'bad', text: n.text, note: n });
    const key = JSON.stringify(lines.map(l => [l.kind, l.text]));
    if (P.ui.notesKey === key) return;
    P.ui.notesKey = key;
    P.ui.notes.replaceChildren(...lines.map(l => el('div', { class: 'ctl-note', dataset: { kind: l.kind } },
      el('span', { text: l.text }),
      l.note ? el('button', { class: 'btn btn-quiet ctl-dismiss', onclick: () => { P.notes = P.notes.filter(x => x !== l.note); renderAll(); } }, 'Dismiss') : null)));
    P.ui.notes.hidden = !lines.length;
  }

  function renderBrowserHint() {
    if (!P.built) return;
    const t = P.target, p = P.drag;
    const text = p && p.kind === 'move' ? `Drop ${p.label} here to unload it.` :
      t ? `Picking for ${inputName(t.idx, t.ch)}: Enter or double-click a plug-in.` : '';
    P.ui.hint.textContent = text;
    P.ui.hint.hidden = !text;
    P.ui.root.classList.toggle('picking', !!t);
  }

  /* ---------------------------------------------------------------- the detail card */

  function buildCard() {
    P.ui.card = el('div', { class: 'pl-card', id: 'plCard', role: 'tooltip', hidden: true });
    P.ui.card.addEventListener('mouseenter', () => clearTimeout(P.ui.hideTimer));
    P.ui.card.addEventListener('mouseleave', () => hideCardSoon());
    document.body.append(P.ui.card);
    /* a click anywhere but the browser or the card closes it */
    document.addEventListener('pointerdown', e => { if (!e.target.closest('#plb, #plCard')) hideCard(); }, true);
  }

  function hideCardSoon() { clearTimeout(P.ui.hideTimer); P.ui.hideTimer = setTimeout(hideCard, 160); }
  function hideCard() { clearTimeout(P.ui.hideTimer); clearTimeout(P.ui.cardTimer); if (P.ui.card) { P.ui.card.hidden = true; P.ui.cardFor = ''; } }

  function showCard(it, anchor) {
    if (P.drag) return;
    clearTimeout(P.ui.hideTimer);
    const bu = browserUnit(), c = P.ui.card, d = st(bu).dsp || {}, total = d.total || 16384, room = roomNow(bu), need = needs(bu, it);
    const cat = categorize(it), rate = (unitInfo(bu) || {}).sampleRate || 48000;
    const pools = Array.isArray(d.pools) ? d.pools : [];
    const where = unitWord(bu).replace(/^the /, '');
    const lic = it.licensed === true ? [`licensed on this ${where}`, 'ok'] : it.licensed === false ? [`not licensed on this ${where}`, 'bad'] : ['not known yet', 'warn'];
    const usedPct = clamp((total - room) / total * 100, 0, 100), needPct = clamp(need / total * 100, 0, 100 - usedPct);
    const pool = el('div', { class: 'card-pool', title: `${dspName(bu)} code pool: in use | this plug-in | free` },
      el('span', { class: 'cp-used', style: `width:${usedPct.toFixed(1)}%` }),
      el('span', { class: 'cp-need', dataset: { fits: room >= need ? '1' : '0' }, style: `width:${(room >= need ? needPct : clamp(100 - usedPct, 0, 100)).toFixed(1)}%` }));
    const row = (k, v, cls) => [el('dt', { text: k }), el('dd', { class: `num ${cls || ''}`, text: v })];
    const ua = [it.category, ...(it.tags || [])].filter(Boolean);
    c.replaceChildren(
      el('div', { class: 'card-art' }, preview(it, 'card-prev')),
      el('div', { class: 'card-name', text: it.label }),
      el('div', { class: 'card-cat' }, el('span', { text: GROUP[cat.id].name.toUpperCase() }),
        el('span', { class: 'card-src', text: cat.src === 'ua' ? `UA: ${[...new Set(ua)].join(' · ')}` : cat.src === 'name' ? 'by name rule' : 'no category data' })),
      pool,
      el('dl', { class: 'card-kv' },
        row('DSP code', need ? `${fmtWords(need)} w` : `0 w (shared: ${it.label} runs)`),
        row(`${dspName(bu)} room`, `${fmtWords(room)} of ${fmtWords(total)} w free → ${room >= need ? `${fmtWords(room - need)} after` : 'NO ROOM'}`, room >= need ? '' : 'bad'),
        it.dataWords ? row('DSP data', `${fmtWords(it.dataWords)} w${pools[1] ? ` (pool 1: ${fmtWords(pools[1].free)} free)` : ''}`) : null,
        row('Latency', `${it.latency || 0} smp · ${((it.latency || 0) / rate * 1000).toFixed(3)} ms @ ${(rate / 1000).toFixed(1).replace(/\.0$/, '')} kHz`),
        row('Parameters', String(it.params ?? '-')),
        row('I/O', [it.mono ? 'mono' : null, it.stereo ? 'stereo' : null].filter(Boolean).join(' · ') || '-'),
        row('Startup', (it.pages || 1) > 1 ? `${it.pages} pages, staged` : '1 page'),
        row('Licence', lic[0], lic[1]),
        it.renderId ? row('Render id', hex4(it.renderId)) : null));
    c.hidden = false;
    P.ui.cardFor = it.name;
    /* beside the browser, level with the item, inside the window */
    const a = anchor.getBoundingClientRect(), b = P.ui.root.getBoundingClientRect(), z = parseFloat(document.body.style.zoom || '1') || 1;
    const vw = window.innerWidth / z, vh = window.innerHeight / z;
    c.style.left = `${Math.min(b.right / z + 8, vw - c.offsetWidth - 8)}px`;
    c.style.top = `${clamp(a.top / z - 20, 8, vh - c.offsetHeight - 8)}px`;
  }

  /* ---------------------------------------------------------------- the parameter drawer */

  function openDrawer(o) {
    P.open = o;
    const d = document.getElementById('plDrawer');
    if (!d) return;
    if (!o) { d.hidden = true; d.replaceChildren(); P.ui.panelKey = ''; renderSlots(); return; }
    renderDrawer();
    renderSlots();
    if (!d.contains(document.activeElement)) d.focus();
  }

  function renderDrawer() {
    const d = document.getElementById('plDrawer');
    if (!d || !P.open) return;
    const ins = slotIns(P.open.unit, P.open.input, P.open.slot ?? 0);
    if (!ins) { openDrawer(null); return; }
    const key = `${P.open.unit}|${ins.input}|${ins.slot}|${ins.name}|${ins.stereo}|${ins.params.length}`;
    if (P.ui.panelKey !== key || !d.contains(P.ui.cells && P.ui.cells.head)) buildDrawer(d, P.open.unit, ins, key);
    updateDrawer(ins);
  }

  function buildDrawer(d, idx, ins, key) {
    const ch = ins.input, c = { ins, idx };
    P.ui.panelKey = key;
    P.ui.cells = c;
    c.bypass = sw('BYPASS', { title: 'Takes the plug-in off the DSP (the rest of the chain runs); its settings stay', onClick: () => bypass(idx, c.ins) });
    c.replace = el('button', { class: 'btn', title: 'Pick another plug-in in the browser for this slot', onclick: () => {
      P.target = { idx, ch: c.ins.input, k: c.ins.slot }; renderSlots(); renderPlacing(); renderBrowser(); focusSearch();
    } }, 'Replace');
    c.remove = el('button', { class: 'btn', onclick: () => remove(idx, c.ins) }, 'Remove');
    c.retry = el('button', { class: 'btn', onclick: () => send({ cmd: 'plug-retry', unit: idx, ...tgt(ch) }) }, 'Try again');
    c.close = el('button', { class: 'btn btn-quiet pl-dr-close', 'aria-label': 'Close the parameters', title: 'Close (Esc)', onclick: () => openDrawer(null) }, '×');
    c.facts = el('div', { class: 'pl-dr-facts num' });
    c.state = el('span', { class: 'pl-state' });
    c.lamp = el('span', { class: 'ins-lamp', 'aria-hidden': 'true' });
    c.where = el('span', { class: 'pl-dr-where num' });
    c.head = el('div', { class: 'pl-dr-head' },
      el('div', { class: 'pl-dr-id' }, c.lamp, c.where, el('h3', { class: 'pl-dr-title', text: ins.label }), c.state),
      el('div', { class: 'pl-actions' }, c.bypass, c.replace, c.remove, c.retry, c.close));
    c.why = el('div', { class: 'ctl-note' }, el('span'));
    c.fine = el('p', { class: 'fine pl-dr-fine' });
    c.params = ins.params.map((p, i) => paramCell(c, p, i));
    c.caret = el('span', { class: 'pl-dr-caret', 'aria-hidden': 'true' });
    d.replaceChildren(c.caret, c.head, c.facts, c.why,
      el('div', { class: 'pl-params', role: 'group', 'aria-label': `${ins.label} parameters` }, c.params.map(x => x.node)), c.fine);
    const pr = Skin.r('params') || {};
    d.dataset.layout = pr.layout || 'grid';
    const it = itemFor(idx, ins.name);
    if (it) c.head.prepend(preview(it, 'pl-dr-art'));
    d.setAttribute('aria-label', `${ins.label} on ${inputName(idx, ch)}${multi(idx) ? `, insert ${ins.slot + 1}` : ''}: parameters`);
    d.tabIndex = -1;
    d.hidden = false;
    d.onkeydown = e => {
      if (e.key !== 'Escape') return;
      openDrawer(null);
      const s = slotNode(idx, ch, ins.slot) || slotNodes().find(b => Number(b.dataset.ch) === ch && Number(b.dataset.unit) === idx);
      if (s) s.focus();
      e.stopPropagation();
    };
  }

  function updateDrawer(ins) {
    const c = P.ui.cells, ch = ins.input, idx = c.idx;
    c.ins = ins;
    const loading = ins.state === 'loading';
    setPressed(c.bypass, ins.bypass);
    c.bypass.disabled = loading;
    c.replace.disabled = loading;
    c.remove.disabled = loading;
    c.retry.hidden = !((ins.state === 'failed' || ins.state === 'waiting') && !ins.bypass);
    c.where.textContent = `${idx ? `${(unitShort(idx) || '').toUpperCase()} · ` : ''}${isAux(ch) ? `AUX ${ch - AUX_T + 1}` : `IN ${ins.stereo ? `${ch + 1}-${ch + 2}` : ch + 1}`} · INS ${ins.slot + 1} · ${dspName(idx).replace(' ', '')}`;
    c.lamp.dataset.state = ins.state;
    c.facts.textContent = [ins.latency ? `LAT ${ins.latency} smp` : 'LAT 0 smp', ins.codeWords ? `CODE ${fmtWords(ins.codeWords)} w` : null,
      `${ins.params.length} PARAM`, ins.pages > 1 ? `${ins.pages} PAGES STAGED` : '1 PAGE', ins.stereo ? 'STEREO' : 'MONO'].filter(Boolean).join('  ·  ');
    c.state.dataset.state = ins.state;
    c.state.textContent = ins.busy || STATE_TEXT[ins.state] || ins.state;
    c.why.hidden = !(ins.state !== 'running' && ins.why);
    c.why.dataset.kind = ['warm', 'absent', 'rate', 'unit'].includes(ins.code) ? 'info' : 'bad';
    c.why.firstChild.textContent = `${CODES[ins.code] || 'Not running'}: ${ins.why}`;
    c.fine.textContent = ins.state === 'running'
      ? 'Each change runs the plug-in\'s own code in the parameter engine and is read back from the DSP before it counts (RB). The first change after a start takes a few seconds.'
      : 'Changes here are kept and go to the DSP with the insert.';
    ins.params.forEach((p, i) => c.params[i] && c.params[i].update(p, loading));
    /* the caret points at the strip the insert is on */
    const strip = document.querySelector(isAux(ch) ? `#inputs .useg[data-unit="${idx}"] .aux-strip[data-aux="${ch - AUX_T}"]` : `#inputs .useg[data-unit="${idx}"] .strip[data-ch="${ch}"]`), d = document.getElementById('plDrawer');
    if (strip && d) {
      const z = parseFloat(document.body.style.zoom || '1') || 1, r = strip.getBoundingClientRect();
      const x = (r.left + r.width / 2 - d.getBoundingClientRect().left) / z;
      c.caret.style.left = `${clamp(x - 6, 6, d.clientWidth - 18)}px`;
    }
  }

  /* One parameter: a knob, or buttons / a list for a stepped one; the plug-in's own text, numeric entry (%), and
     whether the DSP read the last change back (RB OK), it is still on its way (SENT) or it was not confirmed. */
  function paramCell(c, p0, i) {
    const idx = c.idx, key = `${idx}:${c.ins.input}:${c.ins.slot}:${i}`, aria = `${c.ins.label} ${p0.name}`;
    const ref = { p: p0 };
    const pct = v => `${Math.round(v / Math.max(1, ref.p.max) * 100)}%`;
    const sendValue = v => {
      P.pending.set(key, v);
      P.refusedParam.delete(key);
      cell.update(ref.p, false);
      throttled(`plug:${key}`, () => send({ cmd: 'plug-param', unit: idx, ...tgt(c.ins.input), ...(multi(idx) ? { slot: c.ins.slot } : {}), index: i, value: v }), 150);
    };
    const text = el('div', { class: 'pl-param-text num' });
    const rb = el('span', { class: 'pl-rb num' });
    const raw = el('span', { class: 'pl-raw num', 'aria-hidden': 'true' });
    const range = el('span', { class: 'pl-range num', 'aria-hidden': 'true', text: p0.steps && p0.steps.length ? `${p0.steps.length} steps` : `0..${p0.max}` });
    let ctlNode, set, entry = null;
    if (p0.steps && p0.steps.length) {
      const n = p0.steps.length;
      const toValue = k => (ref.p.max === n - 1 ? k : Math.round(k / Math.max(1, n - 1) * ref.p.max));
      const toIndex = v => clamp(ref.p.max === n - 1 ? v : Math.round(v / Math.max(1, ref.p.max) * (n - 1)), 0, n - 1);
      if (n <= 4) {
        const sg = seg(p0.steps, k => { sg.set(k); sendValue(toValue(k)); }, aria);
        ctlNode = sg.node;
        set = (v, locked) => { sg.set(toIndex(v)); sg.node.querySelectorAll('button').forEach(b => { b.disabled = locked; }); };
      } else {
        ctlNode = el('select', { class: 'select', 'aria-label': aria, onchange: e => sendValue(toValue(Number(e.target.value))) },
          p0.steps.map((t, k) => el('option', { value: String(k), text: t })));
        set = (v, locked) => { ctlNode.value = String(toIndex(v)); ctlNode.disabled = locked; };
      }
    } else {
      const k = new Knob({
        size: 44, min: 0, max: p0.max, step: 1, wheelStep: Math.max(1, Math.round(p0.max / 100)), pageStep: Math.max(1, Math.round(p0.max / 10)),
        value: p0.value, aria,
        format: v => (v === ref.p.value && !P.pending.has(key) && ref.p.text ? ref.p.text : pct(v)),
        editText: v => String(Math.round(v / Math.max(1, ref.p.max) * 100)),
        parse: s => { const v = parseFloat(String(s).replace('%', '')); return Number.isFinite(v) ? Math.round(clamp(v, 0, 100) / 100 * ref.p.max) : null; },
        lockedText: 'Wait for the insert to load.',
        onChange: v => sendValue(v),
      });
      ctlNode = k.node;
      entry = el('input', { class: 'pl-entry num', type: 'text', inputmode: 'decimal', 'aria-label': `${aria} in percent`, title: 'Type a value in percent of the range, Enter to send' });
      entry.addEventListener('keydown', e => {
        e.stopPropagation();
        if (e.key !== 'Enter') return;
        const v = parseFloat(entry.value.replace('%', ''));
        if (!Number.isFinite(v)) { toast('Type a number from 0 to 100.', 'warn'); return; }
        const raw = Math.round(clamp(v, 0, 100) / 100 * ref.p.max);
        k.set(raw);
        sendValue(raw);
      });
      set = (v, locked) => {
        k.set(v); k.draw(); k.setLocked(locked); k.setUnconfirmed(P.pending.has(key)); entry.disabled = locked;
        if (document.activeElement !== entry) entry.value = String(Math.round(v / Math.max(1, ref.p.max) * 100));
      };
    }
    const node = el('div', { class: 'pl-param', dataset: { index: String(i) } },
      el('div', { class: 'pl-param-name', text: p0.name, title: p0.name }), ctlNode, text, raw, range,
      el('div', { class: 'pl-param-foot' }, entry ? el('label', { class: 'pl-entry-wrap' }, entry, el('span', { class: 'pl-pct', text: '%' })) : null, rb));
    const cell = {
      node,
      update(p, locked) {
        ref.p = p;
        const pend = P.pending.has(key), bad = P.refusedParam.has(key);
        set(pend ? P.pending.get(key) : p.value, locked);
        node.classList.toggle('unconfirmed', pend);
        node.dataset.rb = pend ? 'sent' : bad ? 'refused' : 'ok';
        rb.textContent = pend ? 'SENT' : bad ? 'NOT CONFIRMED' : 'RB OK';
        raw.textContent = String(pend ? P.pending.get(key) : p.value);
        rb.title = pend ? 'Sent: waiting for the DSP readback' : bad ? 'The DSP did not confirm the last change: the value shown is what it holds' :
          `Read back from the DSP: ${p.value} of ${p.max}`;
        /* a stepped control's buttons already say what the plug-in says, unless it holds another value */
        const n = p.steps ? p.steps.length : 0;
        const label = n ? p.steps[clamp(p.max === n - 1 ? p.value : Math.round(p.value / Math.max(1, p.max) * (n - 1)), 0, n - 1)] : null;
        text.textContent = pend ? '...' : (p.text && p.text !== label ? p.text : '');
      },
    };
    return cell;
  }

  /* ---------------------------------------------------------------- the Control view */

  function renderAll() {
    renderSlots();
    renderDsp();
    renderBrowser();
    renderDrawer();
    renderPlacing();
  }

  /* The Control view is shown: the host refreshes each second while it is (plug-watch). */
  function onShow() {
    if (!P.built) build();
    send({ cmd: 'plug-state', units: true });
    send({ cmd: 'plug-watch', units: true });
    if (!P.lists[0] || P.lists[0].state !== 'ok') send({ cmd: 'plug-list', units: true });
    renderAll();
  }

  function onHide() {
    cancel();
    hideCard();
    if (!S.demo) send({ cmd: 'plug-unwatch' });
  }

  /* Escape anywhere on the Control view: a placement first, then the drawer. */
  function escape() {
    if (P.armed || P.drag || P.target) { cancel(); return true; }
    if (P.open) { openDrawer(null); return true; }
    if (P.ui.card && !P.ui.card.hidden) { hideCard(); return true; }
    return false;
  }

  /* ---------------------------------------------------------------- Settings: plug-in hosting */

  function settingsPanel() {
    const s = st(0), h = s.hosting || {}, m = s.maps || {};
    const state = S.demo ? 'demo (nothing reaches the x8p)' : !s.present ? 'the x8p is not connected' : !h.driver ? 'not in the installed apollo.sys'
      : h.on && h.caller ? 'on, for this Apollo Control' : h.on ? 'on, for another program' : 'off';
    const others = Object.keys(P.sts).map(Number).filter(i => i > 0);
    const dir = el('input', { class: 'text-in', type: 'text', value: m.configured || '', placeholder: m.dir || 'apollo-win\\plugins',
      'aria-label': 'Plug-in maps folder', style: 'width:200px' });
    const busy = !!h.admin;
    return el('div', { class: 'panel', id: 'plSettings' },
      el('div', { class: 'sect-title', text: 'Plug-in hosting' }),
      el('span', { class: 'desc', text: 'UAD plug-ins run on a unit\'s strip DSP through apollo.sys, only those the unit itself licenses. ' +
        'Turning hosting on asks for an administrator once: it sets the driver\'s PluginHost value, names this program as the only one ' +
        'that may use the plug-in calls without administrator rights, and copies the plug-ins\' DSP modules (from your own UAD install) ' +
        'into the driver\'s administrator-only module store. Turning it off removes the inserts from the DSP and needs an administrator again.' }),
      setRow('Hosting', state, el('div', { class: 'pl-set-actions' },
        el('button', { class: 'btn btn-primary', disabled: busy || S.demo || (h.on && h.caller), onclick: () => send({ cmd: 'plug-hosting', on: true }) },
          h.on && !h.caller ? 'Turn on for this program' : 'Turn on'),
        el('button', { class: 'btn', disabled: busy || S.demo || !h.on, onclick: () => send({ cmd: 'plug-hosting', on: false }) }, 'Turn off'))),
      others.map(i => setRow(`${unitShort(i)} hosting`, hosts(i) ? 'this driver build hosts plug-ins on it' : 'not supported on this driver build', el('span'))),
      setRow('Module store', 'Copies the DSP modules of every mapped plug-in again (after mapping new ones).',
        el('button', { class: 'btn', disabled: busy || S.demo, onclick: () => send({ cmd: 'plug-modules' }) }, 'Install modules')),
      setRow('Plug-in maps folder', m.ok ? `In use: ${m.dir}` : (m.why || 'not found'), el('div', { class: 'pl-set-actions' }, dir,
        el('button', { class: 'btn', disabled: S.demo, onclick: () => send({ cmd: 'plug-dir', dir: dir.value.trim() }) }, 'Use'))),
      h.admin ? el('span', { class: 'desc', text: h.admin }) : null,
      P.notes.filter(n => ['hosting', 'modules'].includes(n.op)).map(n => el('div', { class: 'ctl-note', dataset: { kind: 'bad' } }, el('span', { text: n.text }))),
      el('span', { class: 'desc', text: 'The maps, modules, saved plug-in settings and plug-in previews come from your own UAD install. They stay on this PC.' }));
  }

  function renderSettings() {
    const old = document.getElementById('plSettings');
    if (old) old.replaceWith(settingsPanel());
  }

  /* ---------------------------------------------------------------- demo: invented plug-ins, nothing reaches hardware */

  const knobP = (name, value, unit, lo, hi) => ({ name, max: 32768, value, unit, lo, hi });
  const DEMO_PLUGINS = [
    { name: 'Demo Compressor', category: 'Compressors and Limiters', tags: ['Dynamics'], mono: true, stereo: true, latency: 0, codeWords: 2400, dataWords: 420, pages: 1,
      params: [knobP('Input', 16384, 'dB', -20, 20), { name: 'Ratio', max: 3, value: 0, steps: ['4:1', '8:1', '12:1', '20:1'] },
        knobP('Attack', 8000, 'ms', 0.02, 0.8), knobP('Release', 12000, 'ms', 50, 1100), { name: 'Power', max: 1, value: 1, steps: ['Off', 'On'] }] },
    { name: 'Demo EQ', category: 'Equalizers', tags: ['Equalization'], mono: true, stereo: true, latency: 0, codeWords: 3100, dataWords: 380, pages: 1,
      params: [knobP('Low', 16384, 'dB', -12, 12), knobP('Low Mid', 16384, 'dB', -12, 12), knobP('High Mid', 16384, 'dB', -12, 12), knobP('High', 16384, 'dB', -12, 12),
        { name: 'Shape', max: 6, value: 2, steps: ['3k', '4k', '5k', '8k', '10k', '12k', '16k'] }] },
    { name: 'Demo Plate Reverb', category: 'Reverb and Room', tags: ['Reverb & Room'], mono: true, stereo: true, latency: 64, codeWords: 14000, dataWords: 5200, pages: 12,
      params: [knobP('Decay', 9000, 's', 0.2, 8), knobP('Pre-delay', 4000, 'ms', 0, 250), knobP('Mix', 10000, '%', 0, 100)] },
    { name: 'Demo De-Esser', category: 'Special Processing', tags: ['Special Processing', 'Dynamics'], mono: true, stereo: false, latency: 0, codeWords: 2000, dataWords: 210, pages: 1,
      params: [knobP('Threshold', 0, 'dB', 0, -40), { name: 'Speed', max: 1, value: 1, steps: ['Slow', 'Fast'] }] },
    { name: 'Demo Tape Echo', category: 'Delay and Modulation', tags: ['Delay'], mono: true, stereo: true, latency: 0, codeWords: 5000, pages: 1, unlicensed: true, params: [] },
    { name: 'Demo Tube Preamp', category: 'Channel Strip and Preamp', tags: ['Preamp & Channel Strip'], mono: true, stereo: true, latency: 0, codeWords: 1800, dataWords: 160, pages: 1,
      params: [knobP('Gain', 20000, 'dB', 0, 60), knobP('Low', 16384, 'dB', -10, 10), knobP('High', 16384, 'dB', -10, 10), knobP('Output', 16384, 'dB', -20, 20)] },
    { name: 'Demo Chorus', category: 'Delay and Modulation', tags: ['Modulation'], mono: true, stereo: true, latency: 0, codeWords: 1500, pages: 1,
      params: [knobP('Rate', 6000, 'Hz', 0.1, 8), knobP('Depth', 14000, '%', 0, 100)] },
    { name: 'Demo Bass Amp', category: 'Guitar and Bass', tags: ['Guitar & Bass'], mono: true, stereo: false, latency: 0, codeWords: 4200, pages: 3,
      params: [knobP('Drive', 12000, '', 0, 10), knobP('Bass', 16384, '', 0, 10), knobP('Treble', 16384, '', 0, 10)] },
    { name: 'Demo Tape Machine', mono: true, stereo: true, latency: 0, codeWords: 3600, pages: 1,
      params: [knobP('Input', 16384, 'dB', -12, 12), { name: 'Speed', max: 1, value: 0, steps: ['15 ips', '30 ips'] }] },
    { name: 'Demo Bus Limiter', category: 'Mastering', tags: ['Mastering'], mono: true, stereo: true, latency: 32, codeWords: 2600, pages: 1,
      params: [knobP('Ceiling', 30000, 'dB', -12, 0), knobP('Release', 12000, 'ms', 1, 1000)] },
    { name: 'Demo Pitch Fix', category: 'Pitch Correction', tags: ['Special Processing'], mono: true, stereo: false, latency: 128, codeWords: 6000, pages: 1, unlicensed: true, params: [] },
  ];
  const demoText = (d, v) => {
    if (d.steps) return d.steps[clamp(v, 0, d.steps.length - 1)];
    const x = d.lo + (d.hi - d.lo) * (v / d.max);
    return `${Math.abs(x) < 10 ? x.toFixed(2) : x.toFixed(1)}${d.unit ? ` ${d.unit}` : ''}`;
  };

  function demoEnter() {
    P.demo = { chains: [], room: 16384 - 994, recmon: {}, load: 1989 };
    P.sts = {};
    P.lists = {};
    demoPublish();
    P.lists[0] = { type: 'plugin-list', unit: 0, state: 'ok', licence: 'ok', mapped: DEMO_PLUGINS.length, authorized: DEMO_PLUGINS.filter(d => !d.unlicensed).length,
      items: DEMO_PLUGINS.map((d, n) => ({ name: d.name, label: d.name, mono: d.mono, stereo: d.stereo, latency: d.latency, codeWords: d.codeWords,
        dataWords: d.dataWords || 0, renderId: 0x900 + n, pages: d.pages, params: d.params.length, licensed: !d.unlicensed,
        category: d.category || '', tags: d.tags || [], art: '' })) };
    renderAll();
  }

  function demoExit() {
    P.demo = null;
    P.sts = {};
    P.lists = {};
    P.moves.clear();
    P.recmon.clear();
    P.notes = [];
    openDrawer(null);
    renderAll();
  }

  /* [Ins] the demo answers as a 1.9 host: chains of up to four slots per input and per aux, REC / MON, the Unison slot
     (refused: no Unison map), aux inserts taken (as with Parameters\AuxInserts = 1) */
  const demoKey = o => (o.aux === 0 || o.aux === 1 ? `a${o.aux}` : `i${o.input | 0}`);
  function demoChain(o, make) {
    const D = P.demo, k = demoKey(o);
    let c = D.chains.find(x => x.key === k);
    if (!c && make) {
      c = { key: k, input: o.aux === 0 || o.aux === 1 ? 0 : o.input | 0, aux: o.aux === 0 || o.aux === 1 ? o.aux : null, stereo: o.aux === 0 || o.aux === 1 || !!o.stereo, slots: [] };
      D.chains.push(c);
    }
    return c || null;
  }
  const demoUsed = (skip) => {
    const seen = new Set();
    let n = 0;
    for (const c of P.demo.chains) for (const s of c.slots) if (s !== skip && !seen.has(s.name)) { seen.add(s.name); n += s.codeWords; }
    return n;
  };

  function demoPublish() {
    const D = P.demo;
    const room = D.room - demoUsed(null);
    const chains = D.chains.filter(c => c.slots.length).sort((a, b) => (a.aux ?? -1) - (b.aux ?? -1) || a.input - b.input).map(c => {
      const act = c.slots.filter(s => !s.bypass);
      const latency = act.reduce((n, s) => n + (s.latency || 0), 0), mon = c.aux === null && !!D.recmon[c.input];
      return { input: c.input, aux: c.aux, stereo: c.stereo, mon, state: act.length ? 'running' : 'idle', code: '', why: '', busy: '',
        latency, recordDelay: mon || c.aux !== null ? 200 : Math.max(0, 200 - latency), overloaded: latency > 200, running: act.length > 0,
        load: act.length ? { before: D.load, after: D.load + act.length * 640 } : null, unison: null, analog: false,
        slots: c.slots.slice().sort((a, b) => a.slot - b.slot).map(s => ({ slot: s.slot, name: s.name, label: s.label, bypass: s.bypass,
          state: s.bypass ? 'bypassed' : 'running', code: '', why: '', latency: s.latency, codeWords: s.codeWords, pages: s.pages,
          params: s.params.map(p => ({ name: p.name, max: p.max, value: p.value, text: demoText(p, p.value), steps: p.steps || null })) })) };
    });
    const firsts = chains.filter(c => c.aux === null && c.slots.some(s => !s.bypass)).map(c => {
      const s = c.slots.find(x => !x.bypass);
      return { ...s, input: c.input, stereo: c.stereo, busy: '' };
    });
    onState({ type: 'plugins', unit: 0, present: true, model: 'x8p', inputs: 8, pairs: [0, 2, 4, 6], hiz: [0, 1], supported: true,
      hosting: { driver: true, on: true, caller: true, stereo: true, staged: true, admin: '' },
      dsp: { index: 1, room, total: 16384, largest: room, estimate: false, quarantined: false, owned: true, admitted: true, load: D.load,
        pools: [{ total: 16384, used: 16384 - room, free: room, largest: room, scratch: 0 }, { total: 43008, used: 3100 + chains.length * 86, free: 43008 - 3100 - chains.length * 86, largest: 30000, scratch: 448 },
          { total: 8192, used: 512, free: 7680, largest: 7680, scratch: 192 }, { total: 16777216, used: 4096, free: 16773120, largest: 16773120, scratch: 0 }] },
      strips: { live: true, started: 255, warm: false }, maps: { ok: true, dir: 'demo', python: true, catalog: 'ready' },
      slots: 4, recmon: true, auxBuses: 2, unison: true, auxInserts: true, chains, inserts: firsts });
  }

  function demoCmd(o) {
    const D = P.demo;
    if (!D) return;
    const op = String(o.cmd || '').replace(/^plug-/, '');
    if (op === 'state' || op === 'watch' || op === 'unwatch' || op === 'list') return;
    const aux = o.aux === 0 || o.aux === 1 ? o.aux : null;
    const result = (ok, code = '', detail = '') => setTimeout(() => onResult({ type: 'plugin-result', unit: o.unit | 0, op, input: o.input | 0, aux,
      slot: o.slot === 'unison' ? 'unison' : Number.isInteger(o.slot) ? o.slot : null, ok, code, detail }), 0);
    if ((o.unit | 0) !== 0) { result(false, 'driver', 'not supported on this driver build (demo)'); return; }
    const slot = Number.isInteger(o.slot) ? clamp(o.slot, 0, 3) : 0;
    if (op === 'insert') {
      const d = DEMO_PLUGINS.find(x => x.name === o.name);
      if (!d) { result(false, 'maps', 'no map of it'); return; }
      if (d.unlicensed) { result(false, 'licence', 'not licensed on this x8p (demo)'); return; }
      /* a linked pair's chain is its first input's; an insert with the other layout takes the chain along */
      if (aux === null) {
        const other = D.chains.find(c => c.aux === null && c.input !== (o.input | 0) && (c.input === (o.input ^ 1)) && c.slots.length);
        if (other && o.stereo) D.chains = D.chains.filter(c => c !== other);
      }
      const c = demoChain(o, true), there = c.slots.find(s => s.slot === slot);
      c.stereo = aux !== null || !!o.stereo;
      if (d.codeWords > D.room - demoUsed(there) && !D.chains.some(x => x.slots.some(s => s !== there && s.name === d.name))) {
        result(false, 'room', `no room on DSP 1 for the chain: needs ${d.codeWords} words, ${D.room - demoUsed(there)} free (demo)`); return;
      }
      c.slots = c.slots.filter(s => s.slot !== slot);
      c.slots.push({ slot, bypass: false, name: d.name, label: d.name, latency: d.latency, codeWords: d.codeWords, pages: d.pages, params: d.params.map(p => ({ ...p })) });
      demoPublish();
      result(true);
      return;
    }
    if (op === 'recmon') { D.recmon[o.input | 0] = o.mon === true || o.mode === 'mon'; demoPublish(); result(true); return; }
    if (op === 'unison') { result(false, 'unison', `no Unison map of ${o.name}: the mapper maps only the insert form (demo)`); return; }
    if (op === 'unison-remove') { result(true); return; }
    const c = demoChain(o, false);
    if (!c) { result(false, 'absent', 'no chain on that input (demo)'); return; }
    if (op === 'move') {
      const a = c.slots.find(s => s.slot === o.from), b = c.slots.find(s => s.slot === o.to);
      if (a) a.slot = o.to;
      if (b) b.slot = o.from;
    } else if (op === 'remove') {
      c.slots = Number.isInteger(o.slot) ? c.slots.filter(s => s.slot !== o.slot) : [];
    } else if (op === 'bypass') {
      for (const s of c.slots) if (!Number.isInteger(o.slot) || s.slot === o.slot) s.bypass = !!o.on;
    } else if (op === 'param') {
      const s = c.slots.find(x => x.slot === slot), p = s && s.params[o.index | 0];
      if (p) p.value = clamp(o.value | 0, 0, p.max);
    }
    demoPublish();
    result(true);
  }

  /* screenshots and checks: a few filled chains, as a session might have them (the demo starts empty) */
  function demoFill() {
    if (!P.demo) return;
    const put = (o, name) => demoCmd({ cmd: 'plug-insert', unit: 0, name, ...o });
    put({ input: 0, slot: 0 }, 'Demo Tube Preamp');
    put({ input: 0, slot: 1 }, 'Demo Compressor');
    put({ input: 0, slot: 2 }, 'Demo EQ');
    demoCmd({ cmd: 'plug-bypass', unit: 0, input: 0, slot: 2, on: true });
    put({ input: 1, slot: 0 }, 'Demo De-Esser');
    put({ input: 1, slot: 1 }, 'Demo Bus Limiter');
    D_recmon(1, true);
    put({ aux: 0, slot: 0 }, 'Demo Chorus');
  }
  const D_recmon = (input, mon) => demoCmd({ cmd: 'plug-recmon', unit: 0, input, mon });

  /* [Sess] the demo's chains in the shape a session keeps (plugins.json version 2, as the host's plug-session-get), and
     back: a session's chains replace the demo x8p's (sessions.js; the host does the same in plugin_engine.h) */
  function demoSessionDoc(unitId) {
    const D = P.demo;
    if (!D) return null;
    const chains = D.chains.filter(c => c.slots.length || (c.aux === null && D.recmon[c.input])).map(c => ({
      unit: unitId, input: c.input, aux: c.aux, stereo: c.stereo, mon: c.aux === null && !!D.recmon[c.input],
      slots: c.slots.slice().sort((a, b) => a.slot - b.slot).map(s => ({ slot: s.slot, bypass: !!s.bypass, name: s.name, bundle: `demo/${s.name}`, bundleSha: '',
        values: s.params.map(p => p.value), texts: s.params.map(p => demoText(p, p.value)), diff: {} })),
      unison: null }));
    for (const [input, mon] of Object.entries(D.recmon)) {
      if (mon && !chains.some(c => c.aux === null && c.input === Number(input))) chains.push({ unit: unitId, input: Number(input), aux: null, stereo: false, mon: true, slots: [], unison: null });
    }
    return { version: 2, mapsDir: 'demo', chains };
  }
  function demoSessionSet(chains) {
    const D = P.demo;
    if (!D) return;
    D.recmon = {};
    D.chains = [];
    for (const c of Array.isArray(chains) ? chains : []) {
      const aux = c.aux === 0 || c.aux === 1 ? c.aux : null;
      if (aux === null && c.mon) D.recmon[c.input | 0] = true;
      const slots = (c.slots || []).map(s => {
        const d = DEMO_PLUGINS.find(x => x.name === s.name);
        if (!d || d.unlicensed || !Number.isInteger(s.slot)) return null;
        const v = Array.isArray(s.values) ? s.values : [];
        return { slot: clamp(s.slot, 0, 3), bypass: !!s.bypass, name: d.name, label: d.name, latency: d.latency, codeWords: d.codeWords, pages: d.pages,
          params: d.params.map((p, i) => ({ ...p, value: Number.isInteger(v[i]) ? clamp(v[i], 0, p.max) : p.value })) };
      }).filter(Boolean);
      if (slots.length) D.chains.push({ key: aux !== null ? `a${aux}` : `i${c.input | 0}`, input: aux !== null ? 0 : c.input | 0, aux, stereo: aux !== null || !!c.stereo, slots });
    }
    demoPublish();
  }

  /* [Themes] typed placement (cmd.js): the best match by name, then the same checks and dialogs as a drop; into the
     first empty slot (else the first) unless a slot is named */
  async function insertByName(q, idx, ch, k = null) {
    const t = String(q || '').trim().toLowerCase(), items = itemsOf(idx);
    if (!t) return { ok: false, why: 'Which plug-in? insert <name> on <input>' };
    if (!items.length) return { ok: false, why: `No plug-in list from ${unitWord(idx)} yet.` };
    const lab = i => (i.label || i.name).toLowerCase(), nm = i => i.name.toLowerCase();
    let hit = items.filter(i => lab(i) === t || nm(i) === t);
    if (!hit.length) hit = items.filter(i => lab(i).startsWith(t) || nm(i).startsWith(t));
    if (!hit.length) hit = items.filter(i => lab(i).includes(t) || nm(i).includes(t));
    if (!hit.length) return { ok: false, why: `No plug-in called "${q}" on ${unitWord(idx)}.` };
    hit.sort((a, b) => (b.licensed === true) - (a.licensed === true) || lab(a).length - lab(b).length);
    const input = !isAux(ch) && pairLinked(idx, ch) ? ch & ~1 : ch;
    const slot = k !== null ? k : [...Array(slotCount(idx)).keys()].find(n => !slotIns(idx, input, n)) ?? 0;
    const it = hit[0], r = dropCheck({ kind: 'add', item: it }, idx, ch, slot);
    if (!r.ok) return { ok: false, why: r.why };
    const done = await place({ kind: 'add', item: it }, idx, ch, slot);
    const also = hit.length > 1 ? ` (${hit.length - 1} more match: ${hit.slice(1, 4).map(i => i.label || i.name).join(', ')})` : '';
    return done ? { ok: true, why: `${it.label || it.name} on ${inputName(idx, r.input)}${multi(idx) ? `, insert ${slot + 1}` : ''}${also}.` } : { ok: false, why: 'Not inserted.' };
  }

  async function removeAt(idx, ch, k = null) {
    const ins = k !== null ? slotIns(idx, ch, k) : insertOn(idx, ch);
    if (!ins) return { ok: false, why: `No insert on ${inputName(idx, ch)}${k !== null ? ` slot ${k + 1}` : ''}.` };
    const done = await remove(idx, ins);
    return done ? { ok: true, why: `Removing ${ins.label}.` } : { ok: false, why: 'Kept.' };
  }

  /* REC / MON by command (cmd.js): the pair's first input on a linked pair */
  function setRecMon(idx, ch, mon) {
    const input = pairLinked(idx, ch) ? ch & ~1 : ch;
    P.recmon.set(`${idx}:${input}`, !!mon);
    send({ cmd: 'plug-recmon', unit: idx, input, mon: !!mon });
    renderSlots();
  }

  /* every insert of an input in chain order (the meter bridge's INS stage, the routing hand-off) */
  function insertLabel(idx, ch) {
    const c = chainFor(idx, ch);
    if (!c) return '';
    return (c.slots || []).slice().sort((a, b) => a.slot - b.slot).map(s => `${s.label}${s.bypass ? ' (byp)' : ''}`).join(' > ');
  }

  return {
    insertByName, removeAt, setRecMon, itemNames: idx => itemsOf(idx).map(i => i.label || i.name), insertLabel,
    onState, onList, onResult, onShow, onHide, escape, renderAll, renderSlots, renderDsp, slotNodesFor, recmonNode, dspNode, focusSearch, openDrawer,
    hello, settingsPanel, renderSettings, demoEnter, demoExit, demoFill, demoSessionDoc, demoSessionSet, categorize, GROUPS, NAME_RULES, AUX_T,
    chainFor, slotIns, slotCount, multi,
    get state() { return P; },
  };
})();
