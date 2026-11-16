'use strict';
/*
 * Apollo Control UI. Talks to apollo-control.exe through WebView2 messages:
 *   to host:   tab-separated strings ("input\t2\tgain\t42"); JSON strings for Units & routing
 *   from host: objects with a "type" (hello, status, readback, levels, live, units, agg, log, error, tray)
 * Opened in a plain browser (no host), it runs in demo mode.
 * [Plug-ins] plugins.js owns the plug-in browser on the Control view, the insert slots on the x8p's
 * strips and the parameter drawer; the host's "plugins", "plugin-list" and "plugin-result" go there.
 * [Rate] the sample-rate selectors (each segment's head, the Device view): "rate", "rate-result".
 *
 * Per-unit state lives here: S.unitList (the host's "units") and S.units[slot] (status info,
 * readback words, live). units.js (Units & routing) reads it and keeps only its own page state.
 * The Control view shows every present unit side by side, one segment each, and every control
 * sends to its own unit; S.sel is the focused unit (the monitor column, top bar, presets, tray,
 * M / D / O). Each unit keeps its own control state, pending writes, refusals and meters in
 * S.ctl[slot]. Looks (themes.css): cfg.appearance.look, Settings > Appearance, Ctrl+Shift+T.
 */

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

const PREAMPS = 8;
const LEVEL_MAX = 192;                 // raw monitor level for 0 dB; raw = 192 + 2 * dB
const FULL_SCALE = 8388607;            // 24-bit peak from GET_LEVELS
const GRACE_MS = 1500;                 // how long a sent value waits for readback to agree

const STAGE = { NONE: 0, MAPPED: 1, RINGS: 2, RING_CONNECTED: 3, ACEFACE: 4, CONNECTED: 5, FAILED: 0x100, GONE: 0x200 };
const STAGE_NAMES = {
  0: 'not started', 1: 'identified', 2: 'DSP rings ready', 3: 'DSP rings connected',
  4: 'audio handshake done', 5: 'connected', 256: 'failed', 512: 'removed',
};
const FLAG = {
  FW_V2: 0x1, IRQ64: 0x2, WARM: 0x4, RINGS_LIVE: 0x8, DSPS: 0x10, ACEFACE: 0x20, CONNECTED: 0x40,
  MIXER_READY: 0x80, TRANSPORT: 0x100, SERVICE: 0x200, READBACK: 0x400, MSI: 0x800,
  FW_UNTESTED: 0x1000, ENGINE_LOST: 0x2000,
};
const TESTED_FW = '11.15.54803';
const FLAG_CHIPS = [
  [FLAG.CONNECTED, 'Connected'], [FLAG.MIXER_READY, 'Mixer ready'], [FLAG.SERVICE, 'Service loop'],
  [FLAG.READBACK, 'Readback'], [FLAG.TRANSPORT, 'Clock running'], [FLAG.DSPS, 'DSP rings'],
  [FLAG.ACEFACE, 'Audio handshake'], [FLAG.WARM, 'Warm start'], [FLAG.MSI, 'MSI'], [FLAG.IRQ64, '64-bit vectors'],
  [FLAG.FW_UNTESTED, 'Untested firmware'], [FLAG.ENGINE_LOST, 'Engine lost'],
];

/* Tab order. Ctrl+1.. follows it. 'units' is Units & routing (units.js): the x8p and the Twin X as
   one device, channel order, clock master, output routing. [Plug-ins] no tab of their own any more:
   the browser is docked on the Control view (an old 'plugins' view opens Control). */
const VIEWS = ['control', 'status', 'cues', 'units', 'correction', 'channels', 'device', 'settings'];   /* [Corr] correction.js */
const VIEW_NAMES = 'Control, Status, Cues, Units & routing, Speakers, Channels, Device, Settings';
/* Looks (themes.css): every screen restyles from one set of tokens. mode: what the window chrome follows. */
/* the theme packs (themes/index.js, docs/THEMES.md): id, name, mode, desc from each pack's manifest */
const LOOKS = ((window.ApolloThemes && window.ApolloThemes.manifests) || [{ id: 'studio', name: 'Studio', mode: 'dark', desc: '' }])
  .map(m => ({ id: m.id, name: m.name, mode: m.mode || 'dark', desc: m.desc || '', metaphor: m.metaphor || '' }));
const DEVICE_NAMES = { 0x22: 'Apollo x8p', 0x99: 'Apollo Twin X' };
const MAX_UNITS = 4;                   /* unit slots 0..3; the host keeps slot 0 for the x8p */
const MODE_NAMES = ['observe', 'bring-up', 'full'];     /* Units\<key>\Mode 0..2 */

/*
 * What each model can do from software. The x8p's HP 1 and HP 2 levels are analog knobs on the
 * front panel (no software level), and the headphone source fields the driver can write (mixer
 * setting 12, bits 26:24 and 29:27, from open-apollo) have no readback and were never verified on
 * the x8p. The Twin X's main knob drives its headphones digitally (setting 2 bits 15:8), so its HP
 * level path stays here behind hpLevel. preamps / hiz: the Control view's strips and the Status
 * view's input rows (Hi-Z jacks on the first hiz inputs; Status uses the driver's count when it
 * reports one). The rest follows the driver's plans (driver/preamp_plan.h, bringup_plan.h): link
 * from the first input of the pairs in linkPairs (bit p = inputs 2p+1 and 2p+2), Hi-Z impedance
 * (APOLLO_PARAM_HIZ_IMP) on the inputs in hizLoad; the Twin X's monitor takes level, HP 1 level,
 * mute, mono, dim and talkback, not the dim level. Unknown model: nothing.
 * [Monitor] (driver/monitor_plan.h): cues = the cue buses by name (the Twin X names its two after
 * their outputs), alts = the line pair each ALT speaker set uses, hps = headphones with a source
 * (CUE 1 / CUE 2 only: the other sources have no known encoding). The monitor source offers MIX and
 * every cue; the x8p's CUE 3 / CUE 4 there follow the kext's encoder but were never captured.
 * Both units' HP sources are on: the same kext field (S12 [26:24] / [29:27]) on every model, clear of
 * the x8p's preamp 5-8 bits.
 */
const MODEL_CAPS = {
  0x22: { short: 'x8p', hpLevel: false, hpSource: true, hps: 2, preamps: 8, hiz: 2, linkPairs: 0xF, hizLoad: 0,
    monSource: true, dimLevel: true, talkback: true,
    cues: ['CUE 1', 'CUE 2', 'CUE 3', 'CUE 4'], alts: ['Line 1-2', 'Line 3-4'] },
  0x99: { short: 'Twin X', hpLevel: true, hpSource: true, hps: 1, preamps: 2, hiz: 1, linkPairs: 0x1, hizLoad: 0x1,
    monSource: true, dimLevel: false, talkback: true,
    cues: ['HP', 'LINE 3/4'], alts: ['Line 3/4'] },
};
const NO_CAPS = { short: '', hpLevel: false, hpSource: false, hps: 0, preamps: 0, hiz: 0, linkPairs: 0, hizLoad: 0,
  monSource: false, dimLevel: false, talkback: false, cues: [], alts: [] };
/* Hi-Z input impedance codes 0-7 (APOLLO_PARAM_HIZ_IMP, preamp_plan.h ApolloPreampHizName). No readback
   field: the app keeps the last value it set, per unit. */
const HIZ_LOADS = ['2.2M', '1M', '453k', '232k', '100k', '70k', '47k', '21k'];
/* Host error codes that mean "refused": shown inline in the Control view, not as a passing toast.
   1 the driver does not know it, 50 not supported (e.g. the Twin X preamps on a driver before G5),
   87 rejected value, 4317 the host refused (a read-only unit, the x8p's headphone knobs). */
const REFUSED = new Set([1, 50, 87, 4317]);

const DIM_LEVELS = [-9, -17, -26, -34, -43, -51, -60];      // hardware values 1..7

const INPUT_FIELDS = { gain: 'gain', line: 'line', pad: 'pad', '48v': 'p48', lowcut: 'lowcut', phase: 'phase' };
const INPUT_NAMES = { gain: 'gain', line: 'Mic/Line', pad: 'PAD', '48v': '48V', lowcut: 'Low cut', phase: 'Polarity', hizimp: 'Hi-Z impedance' };
const MON_FIELDS = {
  level: 'level', hp1: 'hp1', mute: 'mute', mono: 'mono', dim: 'dim', dimlevel: 'dimLevel',
  source: 'source', hp1src: 'hp1src', hp2src: 'hp2src', talkback: 'talkback', altsel: 'alt',
};
/* have readback; [Monitor] the speaker set too (word 6 [29:28]) */
const MON_CONFIRMED = new Set(['level', 'hp1', 'mute', 'mono', 'dim', 'talkback', 'altsel']);

/* [Monitor] cue sends (driver/monitor_plan.h): half-dB steps, -121 = off; buses ANALOG n = n-1, talkback 10 */
const SEND_OFF = -121, SEND_MAX = 24;
const TALKBACK_BUS = 10;
const ALT_TRIM_UNITY = 60;             /* raw ALT trim = 60 + 2 * dB, -30..+30 dB */
/* [Mix] the monitor mix (driver/monitor_plan.h APOLLO_MON_MIX_*): per bus a level (half-dB as a send, 0 = 0 dB, UA's default
   while untouched), pan, mute, solo. The record slots of the monitor-mix loopback: what the monitor outputs play. */
const MIX_NAME = { level: 'mix', pan: 'mixpan', mute: 'mixmute', solo: 'mixsolo' };
const MIX_OUT_SLOTS = [26, 27];

const ACCENTS = {
  amber: ['#e2a840', '#1b1405'], blue: ['#5a9be6', '#06111f'], green: ['#4fb86a', '#05160a'],
  red: ['#e0625e', '#1f0605'], gray: ['#b8c0c8', '#111417'],
};
const STRIP_COLORS = {
  red: '#d9534f', orange: '#e08a3c', yellow: '#d8b62a', green: '#4fb86a',
  teal: '#3fa7a0', blue: '#5a9be6', purple: '#9b7be0', gray: '#8a939c',
};
/* Record slots in the firmware descriptor's order, confirmed on Aaron's x8p Nov 14 2026: a tone
   played to MON L/R came back on slots 27-28 (1-based). Talkback (descriptor slots 33-34) is past
   the 32 slots the driver runs today. */
const REC_GROUPS = [
  { name: 'Mic / Line', from: 0, labels: ['1', '2', '3', '4', '5', '6', '7', '8'], inputs: true },
  { name: 'ADAT', from: 8, labels: ['1', '2', '3', '4', '5', '6', '7', '8'] },
  { name: 'S/PDIF', from: 16, labels: ['L', 'R'] },
  { name: 'Virtual', from: 18, labels: ['1', '2', '3', '4', '5', '6', '7', '8'] },
  { name: 'Monitor', from: 26, labels: ['L', 'R'] },
  { name: 'Aux', from: 28, labels: ['1 L', '1 R', '2 L', '2 R'] },
];
const METER_TICKS = { '-48': [0, -6, -12, -18, -24, -36], '-60': [0, -6, -12, -18, -24, -36], '-90': [0, -6, -12, -24, -36, -60] };

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

function defaultConfig() {
  return {
    version: 1,
    view: 'control',
    channels: Array.from({ length: PREAMPS }, () => ({ name: '', color: '', hidden: false })),
    safety: { confirm48v: true, monitorLimitDb: 0, lockGain: false },
    meters: { holdMs: 2000, floorDb: -60, releaseDbPerSec: 20 },
    /* look: LOOKS id (a new config starts on Classic, the standard mixer layout; a saved look is kept); followSystem: Day
       while Windows is light; accent: 'look' (the look's own) or an ACCENTS key; scanlines: Arcade's overlay. theme: kept
       for configs from before the looks (migrateLooks). */
    appearance: { theme: 'dark', look: 'classic', followSystem: false, accent: 'look', scale: 100, scanlines: false },
    plb: { closed: false },
    bridge: { std: 'vu' },
    auxStrips: { open: false },        /* [Ins] the console layouts show the AUX 1 / AUX 2 strips (the master's AUX button) */             /* [Themes] Studio's meter bridge: vu, ppm or k14 */            /* [Plug-ins] the Control view's plug-in browser collapsed */
    /* closeToTray counts once the user set it (closeToTraySet); until then the window closes to the tray while Apollo Control
       starts with Windows (sessions.js closeToTray). startWithWindows: unused, the HKCU Run value is the truth ([Sess]). */
    windowPrefs: { closeToTray: true, closeToTraySet: false, startWithWindows: false },
    /* [Sess] the session on screen (name; base: its state right after the save or load, for "edited"), the recent list
       (the tray menu) and the parts waiting for a unit that was not connected when they were loaded (by unit id) */
    session: { name: '', base: '', recent: [], pending: {} },
    presets: [],
    controlUnit: '',                   /* the unit id last picked in the Control view ('' = the x8p) */
    unitPrefs: {},                     /* unit id -> newUnitPrefs(): another unit's names and Hi-Z load */
    talkbackDim: true,                 /* [Monitor] dim the monitor while talkback is on */
    mixes: {},                         /* [Monitor] mixKey -> newMix(): what the units cannot report (ALT, cues) */
  };
}

/*
 * [Monitor] A unit's settings with no readback, as the user set them, re-sent when it connects (a DSP that
 * restarted holds no cue sends): alt[n] = { on, trim }, cues[c] = { mix, mono, mute, sends: { bus: { level, pan } } },
 * [Mix] main[bus] = { level, pan, mute, solo }: the monitor mix.
 * Only what was touched is kept (undefined = left as the unit has it).
 */
function newMix() { return { alt: [{}, {}], cues: [{}, {}, {}, {}], main: {}, aux: [{}, {}], mon: {}, out: {} }; }

/* A saved mix as loaded: only well-formed values survive. */
function cleanMix(m) {
  const out = newMix();
  const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
  (Array.isArray(m.alt) ? m.alt : []).slice(0, 2).forEach((a, n) => {
    if (!a || typeof a !== 'object') return;
    if (typeof a.on === 'boolean') out.alt[n].on = a.on;
    if (isInt(a.trim, 0, 120)) out.alt[n].trim = a.trim;
  });
  (Array.isArray(m.cues) ? m.cues : []).slice(0, 4).forEach((q, c) => {
    if (!q || typeof q !== 'object') return;
    const o = out.cues[c];
    if (q.mix === 0 || q.mix === 1) o.mix = q.mix;
    if (typeof q.mono === 'boolean') o.mono = q.mono;
    if (typeof q.mute === 'boolean') o.mute = q.mute;
    if (q.sends && typeof q.sends === 'object') {
      for (const [bus, s] of Object.entries(q.sends)) {
        const b = Number(bus);
        if (!isInt(b, 0, 31) || !s || typeof s !== 'object') continue;
        (o.sends || (o.sends = {}))[b] = {
          level: isInt(s.level, SEND_OFF, SEND_MAX) ? s.level : SEND_OFF, pan: isInt(s.pan, -100, 100) ? s.pan : 0,
        };
      }
    }
  });
  if (m.main && typeof m.main === 'object' && !Array.isArray(m.main)) {
    for (const [bus, s] of Object.entries(m.main)) {
      const b = Number(bus), o = {};
      if (!isInt(b, 0, 31) || !s || typeof s !== 'object') continue;
      if (isInt(s.level, SEND_OFF, SEND_MAX)) o.level = s.level;
      if (isInt(s.pan, -100, 100)) o.pan = s.pan;
      if (typeof s.mute === 'boolean') o.mute = s.mute;
      if (typeof s.solo === 'boolean') o.solo = s.solo;
      if (Object.keys(o).length) out.main[b] = o;
    }
  }
  out.aux = Aux.clean(m.aux);                  /* [Ins] the aux buses (auxbus.js) */
  out.out = Outs.clean(m.out);                 /* [Out] output and front-panel settings (outputs.js) */
  /* [Sess] the monitor routing (no readback): source, headphone sources, dim level as set here */
  if (m.mon && typeof m.mon === 'object') {
    if (isInt(m.mon.source, 0, 4)) out.mon.source = m.mon.source;
    if (isInt(m.mon.hp1src, 0, 1)) out.mon.hp1src = m.mon.hp1src;
    if (isInt(m.mon.hp2src, 0, 1)) out.mon.hp2src = m.mon.hp2src;
    if (isInt(m.mon.dimLevel, 1, 7)) out.mon.dimLevel = m.mon.dimLevel;
  }
  return out;
}

/* [Sess] A session's own config block as loaded: names and parts as stored, nothing else. */
function cleanSessionCfg(l) {
  const out = { name: '', base: '', recent: [], pending: {} };
  if (!l || typeof l !== 'object') return out;
  if (typeof l.name === 'string') out.name = l.name.slice(0, 60);
  if (typeof l.base === 'string') out.base = l.base;
  if (Array.isArray(l.recent)) out.recent = l.recent.filter(x => typeof x === 'string' && x).slice(0, 8);
  if (l.pending && typeof l.pending === 'object' && !Array.isArray(l.pending)) {
    for (const [id, e] of Object.entries(l.pending)) if (e && typeof e === 'object' && typeof e.name === 'string' && e.part && typeof e.part === 'object') out.pending[id] = e;
  }
  return out;
}

/* The x8p's input names stay in cfg.channels; other units keep theirs here, by unit id. */
function newUnitPrefs() {
  return { channels: Array.from({ length: PREAMPS }, () => ({ name: '', color: '', hidden: false })), hizImp: -1 };
}

function merge(base, loaded) {
  if (loaded === null || typeof loaded !== 'object') return base;
  for (const key of Object.keys(base)) {
    const b = base[key], l = loaded[key];
    if (l === undefined) continue;
    if (key === 'presets') {
      if (Array.isArray(l)) base.presets = l.filter(p => p && typeof p.name === 'string' && p.state && Array.isArray(p.state.inputs));
    } else if (key === 'unitPrefs') {
      if (l && typeof l === 'object' && !Array.isArray(l)) {
        for (const [id, p] of Object.entries(l)) base.unitPrefs[id] = merge(newUnitPrefs(), p);
      }
    } else if (key === 'mixes') {
      if (l && typeof l === 'object' && !Array.isArray(l)) {
        for (const [id, m] of Object.entries(l)) if (m && typeof m === 'object') base.mixes[id] = cleanMix(m);
      }
    } else if (key === 'session') {
      base.session = cleanSessionCfg(l);       /* [Sess] */
    } else if (Array.isArray(b)) {
      if (Array.isArray(l)) b.forEach((item, i) => { if (i < l.length) b[i] = merge(item, l[i]); });
    } else if (b !== null && typeof b === 'object') {
      base[key] = merge(b, l);
    } else if (typeof b === typeof l) {
      base[key] = l;
    }
  }
  return base;
}

/* A config from before the looks (Nov 15 2026): its theme picks the look, and the old default accent (amber) gives
   way to the look's own. A look or accent id this build does not know falls back. */
function migrateLooks(loaded) {
  const a = loaded && loaded.appearance, ap = cfg.appearance;
  if (a && typeof a === 'object' && a.look === undefined) {
    ap.look = a.theme === 'light' ? 'day' : 'studio';
    ap.followSystem = a.theme === 'system';
    if (a.accent === undefined || a.accent === 'amber') ap.accent = 'look';
  }
  if (!LOOKS.some(l => l.id === ap.look)) ap.look = 'studio';
  if (ap.accent !== 'look' && !ACCENTS[ap.accent]) ap.accent = 'look';
}

let cfg = defaultConfig();
let saveTimer = 0;

function saveConfig() {
  if (S.capture) return;                 /* --capture renders a screenshot: leave the user's settings alone */
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    const json = JSON.stringify(cfg);
    if (host) send('saveConfig', json);
    else try { localStorage.setItem('apollo-control', json); } catch (e) { /* preview without storage */ }
  }, 400);
}

/* ------------------------------------------------------------------ */
/* Host bridge                                                         */
/* ------------------------------------------------------------------ */

const host = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
let appVersion = 'preview';

function send(...parts) {
  if (host) host.postMessage(parts.map(String).join('\t'));
}

/* Hardware commands for unit slot idx go to the demo model or the host. The host takes the slot
   as a last field ("input\t<ch>\t<param>\t<value>\t<unit>"); slot 0 goes untagged, as before. */
function hwSend(idx, ...parts) {
  if (S.demo) { demoCommand(idx, parts); Undo.touch(idx); return; }
  if (!unitPresent(idx)) { toastOnce(`Not connected: the change was not sent to the ${unitShort(idx)}.`, 'warn'); return; }
  send(...parts, ...(idx ? [idx] : []));
  Undo.touch(idx);                     /* [Undo] a change this app made: undo.js keeps the step */
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

const S = {
  demo: false,
  capture: false,
  host: { present: false, info: null, error: 0, detail: '' },
  units: [newUnit()],                  /* per unit slot (gaps are undefined): 0 = the x8p the controls
                                          drive, others (a Twin X) from their own unit-tagged messages */
  unitList: null,                      /* the host's last "units" list, normalised; null = none yet */
  agg: null,                           /* the host's last "agg" reply (Units & routing) */
  sel: 0,                              /* the unit slot the Control view drives (the unit selector) */
  pin: '',                             /* a unit asked for by --view control:<twinx|x8p|slot>, until a pick */
  ctl: [newCtl()],                     /* per unit slot: the Control view's state of that unit (newCtl) */
  view: 'control',
  /* The selected unit's control state: what the Control view, presets and the tray show. */
  get inputs() { return cur().inputs; },
  get links() { return cur().links; },
  get mon() { return cur().mon; },
  get pending() { return cur().pending; },
  get levelsValid() { return cur().levelsValid; },
};

/* live: the host's last "live" message; counts and rose: when a fault counter last went up.
   info: a unit's own status info (units other than 0; unit 0 uses S.host).
   words, rbValid: its last readback (the Status view's preamps, the Units view's clock). */
function newUnit() { return { live: null, counts: null, rose: {}, info: null, words: null, rbValid: false }; }

/*
 * One unit's Control view state. inputs, links, mon: what its controls show (readback wins once it
 * agrees); pending: writes waiting for its readback; undo: the value before a write the readback
 * cannot confirm (dim level, sources, Hi-Z load), for a refusal; refused: writes the driver or the
 * host refused, shown inline until dismissed or retried; words, rbValid: its last readback;
 * hostReadback: the host's last one (kept while the demo runs); levelsValid, peaks: its meters.
 * Slot 0 (the x8p) keeps its state while absent, as before; another slot's goes when it leaves.
 */
function newCtl() {
  return {
    inputs: Array.from({ length: PREAMPS }, () => ({ gain: 10, line: false, pad: false, p48: false, lowcut: false, phase: false, hiz: false })),
    links: [false, false, false, false],
    mon: { level: 152, hp1: 152, mute: false, mono: false, dim: false, dimLevel: 1, source: 0, hp1src: 0, hp2src: 1, talkback: false,
      alt: 0 },
    pending: new Map(), undo: new Map(), refused: new Map(),
    words: null, rbValid: false, hostReadback: null, connectedAt: 0,
    levelsValid: false, peaks: new Array(64).fill(-Infinity),
    tbDimmed: false,                   /* [Monitor] this app dimmed the monitor for talkback */
    mixTmp: null,                      /* [Monitor] the mix of a unit with no key (no id yet, or the demo) */
  };
}
function ctl(idx) { return S.ctl[idx] || (S.ctl[idx] = newCtl()); }
function cur() { return ctl(S.sel); }

function info() { return S.demo ? DEMO_INFO : S.host.info; }
const capsOf = type => MODEL_CAPS[type] || NO_CAPS;
/* The unit slot a host message is about: 0 when it carries none (older hosts, unit 0). */
const msgUnit = m => (typeof m.unit === 'number' && m.unit > 0 ? m.unit | 0 : 0);
/* The host's list entry for a slot, while that unit is present. */
const listedUnit = idx => (S.unitList || []).find(u => u.index === idx && u.present) || null;
/* A slot's status info: slot 0's from S.host (or the demo's x8p), the others' from their own status. */
function unitInfo(idx) { return idx === 0 ? info() : (S.units[idx] && S.units[idx].info) || null; }
/* A slot's model: its status, else its list entry; slot 0 is the x8p's until something says otherwise. */
function unitType(idx) {
  const i = unitInfo(idx), e = listedUnit(idx);
  return (i && i.deviceType) || (e && e.deviceType) || (idx === 0 ? 0x22 : 0);
}
function unitShort(idx) { return capsOf(unitType(idx)).short || 'unit'; }
function unitPresent(idx) { return idx === 0 ? S.host.present : !!S.units[idx]; }
/* The selected unit's capabilities: which strips, links and monitor controls it has. */
function caps() { return capsOf(unitType(S.sel)); }
/* A slot's own names and Hi-Z load (cfg.unitPrefs), by its unit id; null without one (slot 0 uses cfg.channels). */
function unitPrefs(idx) {
  const e = listedUnit(idx);
  if (idx === 0 || !e || !e.id) return null;
  return cfg.unitPrefs[e.id] || (cfg.unitPrefs[e.id] = newUnitPrefs());
}
const NO_NAME = { name: '', color: '', hidden: false };
function chanCfg(idx, ch) {
  if (idx === 0) return cfg.channels[ch];
  const p = unitPrefs(idx);
  return (p && p.channels[ch]) || NO_NAME;
}
/* The Hi-Z load the app last set on a slot's input 1 (0-7), or -1. */
function hizLoadOf(idx) {
  const p = unitPrefs(idx), v = p ? p.hizImp : -1;
  return Number.isInteger(v) && v >= 0 && v < HIZ_LOADS.length ? v : -1;
}

/* '' when the driver runs the unit in full (or nothing says otherwise); else its Mode name
   ('observe', 'bring-up'): the host sends no setting to it. The status info is fresher; the list
   entry covers a unit whose info carries no Mode. */
function readOnlyMode(i, entry) {
  const src = i && (typeof i.mode === 'number' || typeof i.observe === 'boolean') ? i : entry;
  if (!src) return '';
  if (src.observe === true || src.mode === 0) return 'observe';
  return typeof src.mode === 'number' && src.mode < 2 ? (MODE_NAMES[src.mode] || `mode ${src.mode}`) : '';
}
function connected(idx = S.sel) {
  const i = unitInfo(idx);
  return !!i && i.stage === STAGE.CONNECTED && (i.flags & FLAG.MIXER_READY) !== 0;
}
/* '' when a slot takes settings, else its read-only Mode name (the host sends it nothing). */
const unitReadOnly = idx => readOnlyMode(unitInfo(idx), listedUnit(idx));

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

function el(tag, props = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k === 'dataset') Object.assign(n.dataset, v);
    else if (k === 'style') n.style.cssText = v;
    else if (k.startsWith('on') && typeof v === 'function') n.addEventListener(k.slice(2), v);
    else if (v === true) n.setAttribute(k, '');
    else n.setAttribute(k, v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) n.append(c);
  return n;
}

function svg(tag, attrs = {}) {
  const n = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  return n;
}

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const bit = (x, n) => ((x >>> n) & 1) === 1;
const rawToDb = raw => (raw - LEVEL_MAX) / 2;
const fmtLevel = raw => `${rawToDb(raw).toFixed(1)} dB`;
const hex = (v, w = 8) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(w, '0');
const now = () => performance.now();

function setPressed(node, on) { node.setAttribute('aria-pressed', on ? 'true' : 'false'); }

/* A latching switch (aria-pressed). Its look is the theme's (skin.js button.decorate adds aria-hidden parts only). */
function sw(label, opts = {}) {
  const b = el('button', {
    class: 'sw' + (opts.red ? ' sw-red' : '') + (opts.big ? ' sw-big' : '') + (opts.cls ? ' ' + opts.cls : ''),
    'aria-pressed': 'false', title: opts.title || null, 'aria-label': opts.aria || null,
    onclick: opts.onClick || null,
  }, el('span', { class: 'sw-lab', text: label }));
  Skin.decorate('button', b);
  return b;
}

/* A selector (one of a few). Its look is the theme's (skin.js selector.decorate; node._skinSet follows the choice). */
function seg(options, onPick, aria) {
  const buttons = options.map((label, i) => el('button', { 'aria-pressed': 'false', onclick: () => onPick(i) }, label));
  const node = el('div', { class: 'seg', role: 'group', 'aria-label': aria }, buttons);
  Skin.decorate('selector', node);
  return { node, set(i) { buttons.forEach((b, j) => setPressed(b, j === i)); if (node._skinSet) node._skinSet(); } };
}

/* ---------- toasts ---------- */

const recentToasts = new Map();
function toast(text, kind = 'info', ms = 4500) {
  const t = el('div', { class: 'toast', dataset: { kind } }, text);
  document.getElementById('toasts').append(t);
  setTimeout(() => t.remove(), ms);
  if (typeof Cmd !== 'undefined') Cmd.note(text, kind);      /* [Themes] Mono's log tail */
}
function toastOnce(text, kind = 'info', windowMs = 8000) {
  if (recentToasts.has(text) && now() - recentToasts.get(text) < windowMs) return;
  recentToasts.set(text, now());
  toast(text, kind);
}

/* ---------- dialog ---------- */

function dialog({ title, body, confirm = 'OK', kind = 'primary', cancel = 'Cancel', input = null, extra = null }) {
  const dlg = document.getElementById('dlg');
  return new Promise(resolve => {
    let field = null;
    if (input !== null) {
      field = el('input', { class: 'text-in', type: 'text', maxlength: '40', value: input });
    }
    const ok = el('button', { class: `btn btn-${kind}`, onclick: () => done(field ? field.value.trim() : true) }, confirm);
    const no = el('button', { class: 'btn', onclick: () => done(null) }, cancel);
    dlg.replaceChildren(
      el('div', { class: 'dlg-body' }, el('h3', { text: title }),
        typeof body === 'string' ? el('p', { text: body }) : body, field),
      el('div', { class: 'dlg-foot' }, el('div', {}, extra), el('div', { class: 'right' }, no, ok)));
    function done(v) { dlg.close(); resolve(v); }
    dlg.oncancel = e => { e.preventDefault(); done(null); };
    if (field) field.addEventListener('keydown', e => { if (e.key === 'Enter') done(field.value.trim()); });
    dlg.showModal();
    (field || no).focus();
    if (field) field.select();
  });
}

async function confirm48v(chs, idx = S.sel) {
  const names = chs.map(ch => inputLabel(ch, idx)).join(', ');
  const box = el('input', { type: 'checkbox', id: 'dontAsk48' });
  const extra = el('label', { class: 'check' }, box, "Don't ask again");
  const ok = await dialog({
    title: `Turn on 48V for ${names}?`,
    body: 'Phantom power can damage ribbon mics and some older gear. Check what is plugged in first.',
    confirm: 'Turn on 48V', kind: 'danger', extra,
  });
  if (ok && box.checked) { cfg.safety.confirm48v = false; saveConfig(); renderSettings(); }
  return !!ok;
}

/* "Input 3 (Vocal)" on the x8p; another unit's inputs carry its model ("Twin X input 1"). */
function inputLabel(ch, idx = S.sel) {
  const n = chanCfg(idx, ch).name;
  const base = idx === 0 ? `Input ${ch + 1}` : `${unitShort(idx)} input ${ch + 1}`;
  return n ? `${base} (${n})` : base;
}
const monLabel = idx => (idx === 0 ? 'Monitor' : `${unitShort(idx)} monitor`);

/* ------------------------------------------------------------------ */
/* Sending and confirming changes                                      */
/* ------------------------------------------------------------------ */

const throttles = new Map();
function throttled(key, fn, ms = 40) {
  const t = throttles.get(key);
  if (t) { t.next = fn; return; }
  fn();
  const entry = { next: null };
  throttles.set(key, entry);
  setTimeout(function tick() {
    if (entry.next) { const f = entry.next; entry.next = null; f(); setTimeout(tick, ms); }
    else throttles.delete(key);
  }, ms);
}

/* A write waiting for unit c's readback. prev: the value before the first write of a run (a knob
   drag sends many), what a refusal restores when no readback says better. */
function markPending(c, key, value, prev) {
  const p = c.pending.get(key);
  c.pending.set(key, { value, t: now(), prev: p ? p.prev : prev });
}
function isPending(key) { return cur().pending.has(key); }
function isRefused(key) { return cur().refused.has(key); }

/* Readback says unit c's hardware holds hwValue. Returns the value to show. */
function reconcile(c, key, hwValue, current, describe) {
  const p = c.pending.get(key);
  if (p) {
    if (p.value === hwValue) { c.pending.delete(key); return hwValue; }
    if (now() - p.t < GRACE_MS) return current;
    c.pending.delete(key);
    toastOnce(describe(p.value, hwValue), 'warn', 15000);
  }
  return hwValue;
}

/* Settings go only to a unit the driver runs in full (Mode 2); the host refuses the rest anyway. */
function writable(idx = S.sel) {
  const ro = unitReadOnly(idx);
  if (!ro) return true;
  toastOnce(`The ${unitShort(idx)} is in ${ro} mode: it is read-only, nothing was sent.`, 'warn');
  return false;
}

/*
 * The linked pair input ch belongs to, its first input first ([ch & ~1, ch | 1]), or null when that pair
 * is not linked or model k does not link it (MODEL_CAPS linkPairs). While a pair is linked the unit runs
 * both inputs from the first input's settings and ignores writes to the second (Twin X, Nov 15: a low cut
 * on input 2 was accepted and did nothing, one on input 1 changed both; the x8p's pairs use the same link
 * bits and presumably behave alike). UA's host does what setInput below does: the first input's write,
 * repeated on the second (oa:mixer-engine/hardware.py _gang, from CMxInput::UpdateHardwareState).
 * driver/preamp_plan.h says why the driver leaves this to the host.
 */
function linkedPair(c, ch, k = caps()) {
  const p = ch >> 1, a = ch & ~1;
  return c.links[p] && ((k.linkPairs >>> p) & 1) && a + 1 < k.preamps ? [a, a + 1] : null;
}

/*
 * One preamp setting. On a linked pair (unless opts.noLink) a change made on either input goes to the
 * pair's first input, the one the unit applies to both, and then the same value to the second input, so
 * it still holds it when the pair is unlinked (48V included); both inputs show it and wait for readback.
 */
function setInput(ch, key, value, opts = {}) {
  const idx = Number.isInteger(opts.unit) ? opts.unit : S.sel, c = ctl(idx), k = capsOf(unitType(idx)), field = INPUT_FIELDS[key];
  if (ch >= k.preamps) return;
  const chs = (!opts.noLink && linkedPair(c, ch, k)) || [ch];
  if (chs.every(n => c.inputs[n][field] === value) && !opts.force) return;
  if (!writable(idx)) { chs.forEach(n => renderInput(n, idx)); return; }
  const wire = typeof value === 'boolean' ? (value ? 1 : 0) : value;
  for (const n of chs) {
    const k = `in:${n}:${field}`;
    markPending(c, k, value, c.inputs[n][field]);
    c.inputs[n][field] = value;
    c.refused.delete(k);
    if (key === 'gain') throttled(`in:${idx}:${n}:gain`, () => hwSend(idx, 'input', n, 'gain', c.inputs[n].gain));
    else hwSend(idx, 'input', n, key, wire);
  }
  chs.forEach(n => renderInput(n, idx));
}

/* A unit's segment still shows it (it did not leave while a dialog was open). */
const segLive = idx => { const sg = segs.get(idx); return !!sg && !sg.gone; };

async function request48v(ch, on, idx = S.sel) {
  const c = ctl(idx), k = capsOf(unitType(idx));
  if (on && !writable(idx)) { renderInput(ch, idx); return; }
  const chs = linkedPair(c, ch, k) || [ch];
  if (on && cfg.safety.confirm48v && !(await confirm48v(chs, idx))) { renderInput(ch, idx); return; }
  if (!segLive(idx) && segs.size) return;      /* the unit left under the dialog: nothing sent */
  for (const n of chs) setInput(n, '48v', on, { noLink: true, unit: idx });
}

async function setLink(pair, on, idx = S.sel) {
  const c = ctl(idx), k = capsOf(unitType(idx)), a = pair * 2, b = a + 1;
  if (!((k.linkPairs >>> pair) & 1) || !writable(idx)) return;
  if (on && c.inputs[a].p48 && !c.inputs[b].p48 && cfg.safety.confirm48v && !(await confirm48v([b], idx))) return;
  if (!segLive(idx) && segs.size) return;
  markPending(c, `link:${pair}`, on, c.links[pair]);
  c.links[pair] = on;
  c.refused.delete(`link:${pair}`);
  hwSend(idx, 'input', a, 'link', on ? 1 : 0);
  if (on) {
    /* the second input takes the first one's settings (UA re-pushes them too: hardware.py _link_hardware) */
    for (const key of ['line', 'pad', 'lowcut', 'phase', 'gain', '48v']) {
      setInput(b, key, c.inputs[a][INPUT_FIELDS[key]], { noLink: true, unit: idx });
    }
  }
  linkMix(idx, a, b, on);
  renderPair(pair, idx);
  renderMixFor(idx);
  Plug.renderSlots();                  /* [Plug-ins] a linked pair takes stereo inserts */
}

/* Hi-Z input impedance (Twin X input 1): no readback, so the value shown is the last one set here. */
function setHizLoad(ch, code, idx = S.sel) {
  const c = ctl(idx), p = unitPrefs(idx), k = `hiz:${ch}`;
  if (!((capsOf(unitType(idx)).hizLoad >>> ch) & 1) || !p || !(code >= 0 && code < HIZ_LOADS.length) || !writable(idx)) { renderInput(ch, idx); return; }
  if (hizLoadOf(idx) === code && !c.refused.has(k)) return;
  c.undo.set(k, hizLoadOf(idx));
  c.refused.delete(k);
  p.hizImp = code;
  saveConfig();
  hwSend(idx, 'input', ch, 'hizimp', code);
  renderInput(ch, idx);
  renderNotes();
}

/* Per model (MODEL_CAPS): no HP level write to an x8p (analog knobs), and on the Twin X no dim level
   (its driver plan refuses it). [Monitor] The source takes MIX or one of the model's cues, HP 2 only
   where there is one, the speaker set MAIN or an ALT that is on. */
const MON_CAP = { hp1: 'hpLevel', hp1src: 'hpSource', hp2src: 'hpSource', source: 'monSource', dimlevel: 'dimLevel', talkback: 'talkback' };

function monAllowed(idx, key, value) {
  const k = capsOf(unitType(idx));
  if (MON_CAP[key] && !k[MON_CAP[key]]) return false;
  if (key === 'hp2src' && k.hps < 2) return false;
  if (key === 'source') return value >= 0 && value <= k.cues.length;
  if (key === 'altsel') return value === 0 || (value >= 1 && value <= k.alts.length && altOn(idx, value - 1));
  return true;
}

/* idx: the unit (the selected one unless talkback's dim acts on another); opts.auto: talkback's own dim. */
function setMon(key, value, idx = S.sel, opts = {}) {
  const c = ctl(idx), field = MON_FIELDS[key];
  if (!monAllowed(idx, key, value)) return;
  if (c.mon[field] === value) return;
  if (!writable(idx)) { if (idx === S.sel) renderMonitor(); else renderMaster(idx); return; }
  const k = `mon:${field}`;
  if (MON_CONFIRMED.has(key)) markPending(c, k, value, c.mon[field]);
  else c.undo.set(k, c.mon[field]);
  c.mon[field] = value;
  c.refused.delete(k);
  storeRouting(idx, key, value);                            /* [Sess] no readback: kept for the next connect */
  if (key === 'dim' && !opts.auto) c.tbDimmed = false;      /* the user took DIM over from talkback */
  const wire = typeof value === 'boolean' ? (value ? 1 : 0) : value;
  if (key === 'level' || key === 'hp1') throttled(`mon:${idx}:${key}`, () => hwSend(idx, 'monitor', key, c.mon[field]));
  else hwSend(idx, 'monitor', key, wire);
  if (key === 'talkback') followTalkback(idx);
  if (idx === S.sel) {
    renderMonitor();
    renderCues();
    sendTrayState();
  } else {
    renderMaster(idx);                 /* another unit's master section */
  }
}

/* [Sess] The monitor routing has no readback (source, headphone sources, dim level): what was set here is kept with the
   unit's mix (cfg.mixes[...].mon) and sent again when it connects, like the cue mixes. */
const ROUTING = { source: 'source', hp1src: 'hp1src', hp2src: 'hp2src', dimlevel: 'dimLevel' };
function storeRouting(idx, key, value) {
  if (!ROUTING[key]) return;
  const m = mixOf(idx);
  (m.mon || (m.mon = {}))[ROUTING[key]] = value;
  saveMix(idx);
}

/*
 * [Monitor] Talkback dims the monitor (cfg.talkbackDim, the talkback card's checkbox): when talkback goes on,
 * from here or the unit's own button (readback), DIM goes on if it was off; when talkback ends, the dim this
 * app set goes off again, unless DIM was changed meanwhile. The amount is the unit's dim level.
 */
function followTalkback(idx) {
  const c = ctl(idx);
  if (c.mon.talkback) {
    if (cfg.talkbackDim && !c.mon.dim && !c.tbDimmed) {
      c.tbDimmed = true;
      setMon('dim', true, idx, { auto: true });
    }
  } else if (c.tbDimmed) {
    c.tbDimmed = false;
    if (c.mon.dim) setMon('dim', false, idx, { auto: true });
  }
}

/* ---------- [Monitor] ALT outputs and cue mixes: settings the unit does not report ---------- */

/* Saved per unit: the x8p's slot as 'x8p' (as cfg.channels is its own), another unit by its id; the
   demo's and a unit's without an id yet stay in memory. */
function mixKey(idx) {
  if (idx === 0) return 'x8p';
  const e = listedUnit(idx);
  return e && e.id ? e.id : '';
}
function mixOf(idx) {
  const k = S.demo ? '' : mixKey(idx);
  if (!k) { const c = ctl(idx); return c.mixTmp || (c.mixTmp = newMix()); }
  return cfg.mixes[k] || (cfg.mixes[k] = newMix());
}
function saveMix(idx) { if (!S.demo && mixKey(idx)) saveConfig(); }

const sendOf = (idx, cue, bus) => {
  const s = (mixOf(idx).cues[cue].sends || {})[bus];
  return s ? { level: s.level, pan: s.pan } : { level: SEND_OFF, pan: 0 };
};
/* An ALT output is on: set so here, or the unit is using it right now (readback). */
const altOn = (idx, n) => mixOf(idx).alt[n].on === true || ctl(idx).mon.alt === n + 1;
/* The mixer buses of a model's sources: its inputs (ANALOG n = n-1) and the talkback mic. */
const cueBuses = k => [...Array.from({ length: k.preamps }, (_, i) => i), TALKBACK_BUS];

/* A write with no readback: the value before the first write of a run is what a refusal restores. */
function noteUndo(c, key, prev) {
  const u = c.undo.get(key);
  if (!u || typeof u !== 'object' || now() - u.t > 3000) c.undo.set(key, { prev, t: now() });
  else u.t = now();
}

function mixWritable(idx, key) {
  if (!writable(idx)) { renderCues(); renderMonitor(); return false; }
  ctl(idx).refused.delete(key);
  return true;
}

/* A cue send's level (half-dB, SEND_OFF = off) or pan (-100..100), monitor param send:<cue>:<bus> / pan:... */
function setSend(cue, bus, field, value, idx = S.sel) {
  const k = capsOf(unitType(idx)), c = ctl(idx);
  if (cue >= k.cues.length || !cueBuses(k).includes(bus)) return;
  const before = sendOf(idx, cue, bus)[field];
  if (before === value) return;
  const name = `${field === 'level' ? 'send' : 'pan'}:${cue + 1}:${bus}`, key = `cue:${name}`;
  if (!mixWritable(idx, key)) return;
  noteUndo(c, key, before);
  storeSend(idx, cue, bus, field, value);
  throttled(`cue:${idx}:${name}`, () => hwSend(idx, 'monitor', name, sendOf(idx, cue, bus)[field]));
  renderCues();
}
function storeSend(idx, cue, bus, field, value) {
  const q = mixOf(idx).cues[cue], all = q.sends || (q.sends = {});
  const s = all[bus] || { level: SEND_OFF, pan: 0 };
  s[field] = value;
  if (s.level === SEND_OFF && s.pan === 0) delete all[bus];
  else all[bus] = s;
  saveMix(idx);
}

/* A cue output's source (mix: 0 its own mix, 1 the monitor mix), mono or mute: cue<n>mix / mono / mute. */
function setCueFlag(cue, flag, value, idx = S.sel) {
  const k = capsOf(unitType(idx)), c = ctl(idx), q = mixOf(idx).cues[cue];
  if (cue >= k.cues.length) return;
  const before = q[flag] === undefined ? (flag === 'mix' ? 0 : false) : q[flag];
  if (before === value && q[flag] !== undefined) return;
  const name = `cue${cue + 1}${flag}`, key = `cue:${name}`;
  if (!mixWritable(idx, key)) return;
  noteUndo(c, key, q[flag]);
  q[flag] = value;
  saveMix(idx);
  hwSend(idx, 'monitor', name, typeof value === 'boolean' ? (value ? 1 : 0) : value);
  renderCues();
}

/* An ALT output (on) and its trim (raw 60 + 2 * dB): alt<n> / alt<n>trim. Turning the ALT in use off
   goes back to the main speakers first. */
function setAlt(n, field, value, idx = S.sel) {
  const k = capsOf(unitType(idx)), c = ctl(idx), a = mixOf(idx).alt[n];
  if (n >= k.alts.length) return;
  const before = a[field] === undefined ? (field === 'on' ? altOn(idx, n) : ALT_TRIM_UNITY) : a[field];
  if (before === value && a[field] !== undefined) return;
  const name = field === 'on' ? `alt${n + 1}` : `alt${n + 1}trim`, key = `alt:${name}`;
  if (!mixWritable(idx, key)) return;
  if (field === 'on' && !value && c.mon.alt === n + 1) setMon('altsel', 0, idx);
  noteUndo(c, key, a[field]);
  a[field] = value;
  saveMix(idx);
  if (field === 'trim') throttled(`alt:${idx}:${n}`, () => hwSend(idx, 'monitor', name, mixOf(idx).alt[n].trim));
  else hwSend(idx, 'monitor', name, value ? 1 : 0);
  if (idx === S.sel) renderMonitor();
}

/* ---------- [Mix] the monitor mix: what the monitor outputs play ---------- */

/* A bus's monitor-mix field as set here (undefined = never touched: the unit keeps its own, UA's 0 dB centre). */
const mixStored = (idx, bus, field) => ((mixOf(idx).main || {})[bus] || {})[field];
/* What a bus's row shows: what was set, else UA's default (on at 0 dB, centre). */
function mixOfBus(idx, bus) {
  const s = (mixOf(idx).main || {})[bus] || {};
  return { level: s.level ?? 0, pan: s.pan ?? 0, mute: !!s.mute, solo: !!s.solo };
}
/* The buses a model mixes: its inputs (ANALOG n = n-1) and the talkback mic, as its cue sources. */
const mixBuses = cueBuses;

function storeMix(idx, bus, field, value) {
  const m = mixOf(idx), main = m.main || (m.main = {});
  const s = main[bus] || (main[bus] = {});
  if (value === undefined) delete s[field];
  else s[field] = value;
  if (!Object.keys(s).length) delete main[bus];
  saveMix(idx);
}

/* The linked pair (first input first) a monitor-mix row belongs to, or null (linkedPair; never the talkback). */
function mixPair(idx, bus) {
  const k = capsOf(unitType(idx));
  return bus === TALKBACK_BUS || bus >= k.preamps ? null : linkedPair(ctl(idx), bus, k);
}

/*
 * One monitor-mix setting of a bus: level (half-dB, SEND_OFF = off), pan (-100..100, not the talkback), mute or solo;
 * monitor params mix:<bus>, mixpan:<bus>, mixmute:<bus>, mixsolo:<bus>. On a linked pair the first input's level, mute
 * and solo drive both inputs' buses and each input keeps its own pan, as UA's engine writes a stereo strip (hardware.py
 * _pair_follower, _write_partner_bus: Pan places the first bus, Pan2 the second). No readback: kept per unit (cfg.mixes)
 * and sent again when the unit connects; the driver also keeps and replays what it was sent.
 */
function setMix(bus, field, value, idx = S.sel) {
  const k = capsOf(unitType(idx)), c = ctl(idx);
  if (!mixBuses(k).includes(bus) || (field === 'pan' && bus === TALKBACK_BUS) || !MIX_NAME[field]) return;
  const pair = field === 'pan' ? null : mixPair(idx, bus);
  const targets = pair || [bus];
  if (targets.every(b => mixStored(idx, b, field) === value)) return;
  for (const b of targets) {
    const name = `${MIX_NAME[field]}:${b}`, key = `mix:${name}`;
    if (!mixWritable(idx, key)) return;
    noteUndo(c, key, mixStored(idx, b, field));
    storeMix(idx, b, field, value);
    if (field === 'level' || field === 'pan') {
      throttled(`mix:${idx}:${name}`, () => hwSend(idx, 'monitor', name, mixOfBus(idx, b)[field]));
    } else {
      hwSend(idx, 'monitor', name, value ? 1 : 0);
    }
  }
  renderMixFor(idx);
}

/* The safety button: every input and the talkback muted (or unmuted) in the monitor mix at once, monitor param
   mixmuteall (one driver call). Each row's own mute follows; a refusal puts every row back. */
function setMixMuteAll(on, idx = S.sel) {
  const k = capsOf(unitType(idx)), c = ctl(idx), key = 'mix:mixmuteall', buses = mixBuses(k);
  if (!k.preamps || !mixWritable(idx, key)) return;
  c.undo.set(key, { prev: buses.map(b => [b, mixStored(idx, b, 'mute')]), t: now() });
  for (const b of buses) {
    c.refused.delete(`mix:mixmute:${b}`);
    storeMix(idx, b, 'mute', on);
  }
  hwSend(idx, 'monitor', 'mixmuteall', on ? 1 : 0);
  renderMixFor(idx);
}

/*
 * A pair linked or unlinked here (setLink): the monitor mix follows UA (hardware.py link_tree, _link_hardware). Linked,
 * the first input's pan goes hard left and the second's hard right, and the second input takes the first one's level,
 * mute and solo where either was set; unlinked, both pans go back to the centre and each input keeps what it has.
 */
function linkMix(idx, a, b, on) {
  const k = capsOf(unitType(idx));
  if (b >= k.preamps) return;
  setMix(a, 'pan', on ? -100 : 0, idx);
  setMix(b, 'pan', on ? 100 : 0, idx);
  if (!on) return;
  for (const f of ['mute', 'solo', 'level']) {
    const v = mixStored(idx, a, f), w = mixStored(idx, b, f);
    if (v !== undefined || w !== undefined) setMix(a, f, v !== undefined ? v : mixOfBus(idx, a)[f], idx);
  }
}

/*
 * When a unit comes up (connected, mixer ready, Mode 2): what it cannot report goes out again, as the
 * Mixer Engine re-applies its saved state once per connection (a DSP that restarted holds no sends).
 * Mutes go first, so a muted cue's sends never sound; [Mix] the monitor mix's mutes and solos go before
 * anything else, so a muted input is silent before any level reaches it. Nothing saved, nothing sent.
 */
function applySavedMix(idx) {
  for (const [key, v] of savedMixOut(idx)) hwSend(idx, 'monitor', key, v);
  /* [Sess] the rest of the last state that does not read back: the monitor routing (after the ALTs) and the Hi-Z load */
  const c = ctl(idx), r = mixOf(idx).mon || {};
  for (const [key, field] of Object.entries(ROUTING)) {
    const v = r[field];
    if (v === undefined || !monAllowed(idx, key, v)) continue;
    c.mon[field] = v;
    hwSend(idx, 'monitor', key, v);
  }
  const hz = hizLoadOf(idx);
  if (hz >= 0 && (capsOf(unitType(idx)).hizLoad & 1)) hwSend(idx, 'input', 0, 'hizimp', hz);
  if (idx === S.sel) { renderMonitor(); renderCues(); } else renderMaster(idx);
}

/* [Sess] What a connect sends of a unit's stored mix, in order: the monitor mix's mutes and solos, its pans and levels, the
   aux buses, the ALT outputs, the cues. [[monitor param, value], ...] (sessions.js sends the same for a loaded mix). */
function setMixModel(idx, m) {
  const k = S.demo ? '' : mixKey(idx);
  if (k) cfg.mixes[k] = m;
  else ctl(idx).mixTmp = m;
}
function savedMixOut(idx) {
  const k = capsOf(unitType(idx)), m = mixOf(idx), out = [];
  const main = m.main || {}, set = mixBuses(k).filter(b => main[b]);
  for (const f of ['mute', 'solo']) {
    for (const b of set) if (main[b][f] !== undefined) out.push([`${MIX_NAME[f]}:${b}`, main[b][f] ? 1 : 0]);
  }
  for (const f of ['pan', 'level']) {
    for (const b of set) {
      if (main[b][f] !== undefined && !(f === 'pan' && b === TALKBACK_BUS)) out.push([`${MIX_NAME[f]}:${b}`, main[b][f]]);
    }
  }
  Aux.applyOut(idx, out);              /* [Ins] the aux buses: mutes, PRE / POST, sends, returns (auxbus.js) */
  m.alt.slice(0, k.alts.length).forEach((a, n) => {
    if (a.on !== undefined) out.push([`alt${n + 1}`, a.on ? 1 : 0]);
    if (a.trim !== undefined) out.push([`alt${n + 1}trim`, a.trim]);
  });
  Outs.applyOut(idx, out);             /* [Out] reference levels, S/PDIF mirror, FCN, meter holds, aux mono (outputs.js) */
  m.cues.slice(0, k.cues.length).forEach((q, c) => {
    if (q.mute !== undefined) out.push([`cue${c + 1}mute`, q.mute ? 1 : 0]);
    if (q.mix !== undefined) out.push([`cue${c + 1}mix`, q.mix]);
    if (q.mono !== undefined) out.push([`cue${c + 1}mono`, q.mono ? 1 : 0]);
    for (const [bus, s] of Object.entries(q.sends || {})) {
      if (!cueBuses(k).includes(Number(bus))) continue;
      if (s.pan) out.push([`pan:${c + 1}:${bus}`, s.pan]);
      out.push([`send:${c + 1}:${bus}`, s.level]);
    }
  });
  return out;
}

/* Called on every status, units and live change: re-sends a unit's last state once per connection, [Sess] once its
   strips are Live; a loaded session's part that waited for this unit goes out instead (sessions.js arrive). */
function syncMixApply(idx) {
  const c = S.ctl[idx];
  if (!c) return;
  if (S.demo || !unitPresent(idx) || !connected(idx)) { c.mixApplied = false; return; }
  if (c.mixApplied || unitReadOnly(idx) || !mixKey(idx)) return;
  if (!Sess.stripsReady(idx)) return;
  c.mixApplied = true;
  if (Sess.arrive(idx)) return;
  applySavedMix(idx);
}

let trayTimer = 0;
function sendTrayState() {
  clearTimeout(trayTimer);
  trayTimer = setTimeout(() => {
    const m = S.mon;
    const which = controlUnits().length > 1 ? `${unitShort(S.sel)} monitor` : 'Monitor';
    const sess = cfg.session && cfg.session.name ? `${cfg.session.name.slice(0, 40)} · ` : '';      /* [Sess] */
    const tip = `Apollo Control · ${sess}${which} ${m.mute ? 'muted' : rawToDb(m.level).toFixed(1) + ' dB'}${m.dim ? ', dimmed' : ''}`;
    send('trayState', m.mute ? 1 : 0, m.dim ? 1 : 0, tip);
  }, 200);
}

/* ------------------------------------------------------------------ */
/* Messages from the host                                              */
/* ------------------------------------------------------------------ */

function onHostMessage(m) {
  if (!m || typeof m !== 'object') return;
  switch (m.type) {
    case 'hello': onHello(m); break;
    case 'status':
      if (msgUnit(m) === 0) onStatus(m);
      else if (!S.demo) onUnitStatus(m);       /* another unit: its own slot, never S.host */
      break;
    case 'readback': {
      const idx = msgUnit(m);
      if (S.demo) { if (idx === 0) ctl(0).hostReadback = m; break; }   /* for leaving the demo */
      if (idx > 0 && !S.units[idx]) break;     /* late, from a unit that left */
      storeReadback(m);
      ctl(idx).hostReadback = m;
      onReadback(idx, m);                      /* every unit's controls; the view shows S.sel's */
      break;
    }
    case 'levels': {
      const idx = msgUnit(m);
      if (!S.demo && (idx === 0 || S.units[idx])) onLevels(idx, m.valid, m.peaks);
      break;
    }
    case 'live': if (!S.demo) onLive(m); break;
    case 'units': if (!S.demo) setUnitList(m.units); break;   /* the demo lists its own (units.js) */
    case 'agg':                                /* the combined device: Units & routing */
      S.agg = m;
      if (!S.demo) Units.onAgg(m);
      break;
    case 'log': if (!S.demo) showLog(m.text, m.total); break;
    case 'corr': case 'corr-check': case 'corr-result': Correction.onHost(m); break;   /* [Corr] correction.js */
    case 'error': onHostError(m); break;
    case 'plugins': if (!S.demo) Plug.onState(m); break;          /* [Plug-ins] plugins.js */
    case 'plugin-list': if (!S.demo) Plug.onList(m); break;
    case 'plugin-result':
      if (m.op === 'session') Sess.onHost(m);                  /* [Sess] a session's chains handed to the engine */
      else if (!S.demo) Plug.onResult(m);
      break;
    case 'plugin-session': case 'sessions': case 'session': case 'session-result': case 'autostart':
      Sess.onHost(m);                                          /* [Sess] sessions.js */
      break;
    case 'rate': if (!S.demo) onRate(m); break;                   /* [Rate] */
    case 'params': if (!S.demo) onParams(m); break;               /* [Params] */
    case 'event': if (!S.demo) onDriverEvent(m); break;           /* [Events] */
    case 'diag-result':                                            /* [Diag] */
      if (m.ok) toast(`Diagnostics saved (${m.files} files): ${m.dir}`, 'info', 8000);
      else toast(`Diagnostics not saved: ${m.why || 'error'}`, 'bad');
      break;
    case 'rate-result': if (!S.demo) onRateResult(m); break;
    case 'measure-result': Sig.onMeasure(m); break;            /* [Themes] Lab's Avium Measure hand-off */
    case 'endpoints': case 'endpoints-result': if (!S.demo) Endpoints.onHost(m); break;   /* [Endpoints] endpoints.js */
    case 'tray':                               /* the tray menu acts on the selected unit, like M and D */
      if (m.action === 'mute') setMon('mute', !S.mon.mute);
      if (m.action === 'dim') setMon('dim', !S.mon.dim);
      /* [Sess] monitor volume up / down: 2 dB a step, never past the monitor limit; a recent session loads */
      if (m.action === 'volup' || m.action === 'voldown') {
        setMon('level', clamp(S.mon.level + (m.action === 'volup' ? 4 : -4), 0, LEVEL_MAX + 2 * cfg.safety.monitorLimitDb));
      }
      if (m.action === 'session' && typeof m.name === 'string') Sess.loadByName(m.name, { from: 'tray' });
      break;
  }
}

function onHello(m) {
  appVersion = m.app || appVersion;
  S.capture = !!m.capture;
  let loaded = null;
  if (m.config) {
    try { loaded = JSON.parse(m.config); } catch (e) { toast('Settings file was unreadable; using defaults.', 'warn'); }
  }
  cfg = merge(defaultConfig(), loaded);
  migrateLooks(loaded);
  applyConfig();
  sendPrefs();
  if (m.demo) enterDemo();
  else {
    Plug.hello();                      /* [Plug-ins] the inserts, for the strips' slots */
    rateAsk();                         /* [Rate] */
  }
  Sess.hello();                        /* [Sess] the session list, the last state, Start with Windows, the tray's recent list */
  showView(m.view || cfg.view);
}

function onStatus(m) {
  const c = ctl(0);
  const was = S.host.present && S.host.info && S.host.info.stage === STAGE.CONNECTED;
  S.host.present = !!m.present;
  S.host.info = m.present ? m.info : null;
  S.host.error = m.error || 0;
  S.host.detail = m.detail || '';
  const is = S.host.present && S.host.info && S.host.info.stage === STAGE.CONNECTED;
  if (is && !was) { c.connectedAt = now(); rateAsk(); }
  rateSeen(0, S.host.info);           /* [Rate] another rate in a status: ask again */
  if (!S.host.present && !S.demo) { c.rbValid = false; c.levelsValid = false; clearLive(0); }
  syncUnit();                          /* the segments: the model decides the strips */
  renderStatus();
  renderMonitor();                     /* ... and which headphone controls exist */
  syncMixApply(0);                     /* [Monitor] its saved ALT and cue settings, once per connection */
}

/*
 * Units other than 0. A "status" with present:true is the host's word that the slot is in use, so
 * it opens the slot even before the next "units" names it; present:false or a "units" list without
 * the slot closes it. readback and live only update an open slot: late ones from a unit that left
 * are dropped, so no stale section comes back.
 */
function onUnitStatus(m) {
  const idx = msgUnit(m);
  if (idx >= MAX_UNITS) return;
  if (!m.present) {
    S.units[idx] = undefined;                         /* it left: its section and controls go */
    S.ctl[idx] = undefined;
  } else {
    const u = S.units[idx] || (S.units[idx] = newUnit());
    const was = !!u.info && u.info.stage === STAGE.CONNECTED;
    u.info = m.info || null;
    if (u.info && u.info.stage === STAGE.CONNECTED && !was) { ctl(idx).connectedAt = now(); rateAsk(); }
    rateSeen(idx, u.info);
  }
  syncUnit();
  if (idx === S.sel) {
    renderStatus();
    renderMonitor();
    renderNotes();
  } else {
    renderSegHead(idx);
    renderUnits();
  }
  syncMixApply(idx);
}

const rbWordsOk = m => !!m.valid && Array.isArray(m.words) && m.words.length >= 40 &&
  m.words[38] !== 0xFFFFFFFF && m.words[39] !== 0xFFFFFFFF;

/* Every unit's readback words: the Status view's preamps and the Units view's clock lock. */
function storeReadback(m) {
  const idx = msgUnit(m);
  const u = S.units[idx];
  if (!u) return;
  u.words = Array.isArray(m.words) ? m.words : null;
  u.rbValid = rbWordsOk(m);
  if (idx > 0) renderUnits();
}

const normUnitEntry = u => {
  const type = Number(u.deviceType) || 0;
  const mode = typeof u.mode === 'number' ? u.mode : null;
  return {
    ...u, id: String(u.id), index: Number.isInteger(u.index) ? u.index : 0, deviceType: type,
    model: String(u.model || DEVICE_NAMES[type] || 'Apollo'), serial: String(u.serial || ''), fw: String(u.fw || ''),
    mode, observe: u.observe === true || mode === 0, present: u.present !== false, active: !!u.active,
  };
};

/* The host's list of every unit on apollo.sys (or the demo's): slots it does not list as present
   close, listed ones open. units.js then gives its notices and updates the combined device. */
function setUnitList(list) {
  const prev = S.unitList;
  S.unitList = (Array.isArray(list) ? list : []).filter(u => u && u.id !== undefined && u.id !== null).map(normUnitEntry);
  const here = new Set(S.unitList.filter(u => u.present).map(u => u.index));
  for (let idx = 1; idx < MAX_UNITS; idx++) {
    if (!here.has(idx)) { S.units[idx] = undefined; S.ctl[idx] = undefined; }
    else if (!S.units[idx]) S.units[idx] = newUnit();
  }
  if (S.demo) demoUnits();
  Units.onUnits(prev);
  syncUnit();
  renderUnits();
  for (let idx = 1; idx < MAX_UNITS; idx++) syncMixApply(idx);    /* [Monitor] a unit whose id just arrived */
  const was = (prev || []).filter(u => u.present).map(u => u.index).join(), is = S.unitList.filter(u => u.present).map(u => u.index).join();
  if (was !== is) rateAsk();           /* [Rate] who runs together changed */
}

/* Entering or leaving the demo: no list, unit 0 only, until the demo or the host sends one. */
function clearUnitList() {
  S.unitList = null;
  for (let idx = 1; idx < S.units.length; idx++) S.units[idx] = undefined;
  for (let idx = 1; idx < S.ctl.length; idx++) S.ctl[idx] = undefined;
  for (const sg of segs.values()) sg.seen = false;     /* the demo's units (or the host's) leave no greyed segment behind */
  syncUnit();
  renderUnits();
}

/* ---------- writes the host could not make ---------- */

const MON_NAMES = {
  level: 'level', hp1: 'headphone level', mute: 'mute', mono: 'mono', dim: 'dim', dimlevel: 'dim level',
  source: 'source', hp1src: 'HP 1 source', hp2src: 'HP 2 source', talkback: 'talkback', altsel: 'speakers',
};

/* [Monitor] The monitor params of the cue mixes and ALT outputs (monitor_cmds.h names). */
const CUE_WRITE = /^(?:(send|pan):([1-4]):(\d+)|cue([1-4])(mix|mono|mute))$/;
const ALT_WRITE = /^alt([12])(trim)?$/;
/* [Mix] the monitor mix's params (monitor_cmds.h names) */
const MIX_WRITE = /^(?:(mix|mixpan|mixmute|mixsolo):(\d+)|(mixmuteall))$/;
const MIX_FIELD = { mix: 'level', mixpan: 'pan', mixmute: 'mute', mixsolo: 'solo' };

/* The control state key of a failed write (as markPending names it), or ''. */
function writeKey(m) {
  const ch = Number(m.index);
  if (m.op === 'input') {
    if (m.key === 'link') return `link:${ch >> 1}`;
    if (m.key === 'hizimp') return `hiz:${ch}`;
    return INPUT_FIELDS[m.key] ? `in:${ch}:${INPUT_FIELDS[m.key]}` : '';
  }
  if (m.op !== 'monitor') return '';
  if (MON_FIELDS[m.key]) return `mon:${MON_FIELDS[m.key]}`;
  if (CUE_WRITE.test(m.key)) return `cue:${m.key}`;
  if (MIX_WRITE.test(m.key)) return `mix:${m.key}`;
  if (Outs.isWrite(m.key)) return `out:${m.key}`;     /* [Out] */
  return ALT_WRITE.test(m.key) ? `alt:${m.key}` : '';
}

/* "Input 2" / "Talkback" on a unit, for a cue send. */
function cueSourceName(bus, idx) {
  return Number(bus) === TALKBACK_BUS ? (idx ? `${unitShort(idx)} talkback` : 'Talkback') : inputLabel(Number(bus), idx);
}
const cueName = (idx, c) => capsOf(unitType(idx)).cues[c] || `CUE ${c + 1}`;

function writeName(m, idx) {
  const ch = Number(m.index);
  if (m.op === 'input') {
    if (m.key === 'link') return `${idx ? `${unitShort(idx)} link` : 'Link'} ${(ch & ~1) + 1}-${(ch & ~1) + 2}`;
    return `${inputLabel(ch, idx)} ${INPUT_NAMES[m.key] || m.key}`;
  }
  if (m.op !== 'monitor') return m.op;
  const q = CUE_WRITE.exec(m.key || ''), a = ALT_WRITE.exec(m.key || ''), x = MIX_WRITE.exec(m.key || '');
  const unit = idx ? `${unitShort(idx)} ` : '';
  if (x && x[3]) return idx ? `${unitShort(idx)} monitor mix mute-all` : 'Monitor mix mute-all';
  if (x) return `${cueSourceName(x[2], idx)} monitor mix ${x[1] === 'mix' ? 'level' : MIX_FIELD[x[1]]}`;
  if (q && q[1]) return `${cueSourceName(q[3], idx)} ${q[1] === 'send' ? 'send' : 'pan'} to ${cueName(idx, q[2] - 1)}`;
  if (q) return `${unit}${cueName(idx, q[4] - 1)} ${{ mix: 'source', mono: 'mono', mute: 'mute' }[q[5]]}`;
  if (a) return `${unit}ALT ${a[1]}${a[2] ? ' trim' : ''}`;
  if (Outs.isWrite(m.key)) return Outs.name(m.key, idx);         /* [Out] */
  return `${monLabel(idx)} ${MON_NAMES[m.key] || m.key}`;
}

function unitModel(idx) {
  const e = listedUnit(idx), i = unitInfo(idx);
  return (i && i.modelName) || (e && e.model) || DEVICE_NAMES[unitType(idx)] || 'unit';
}

/* Worded to follow "X was not changed:" and "X and Y were not changed:" alike. */
function errorReason(m, idx) {
  const code = Number(m.code);
  if (code === 21) return 'the mixer is not ready yet';
  if (code === 170) return 'the driver is busy starting the unit';
  if (code === 87) return 'the driver rejected the value';
  if (code === 50) return `not supported by the installed driver on the ${unitModel(idx)}`;
  if (code === 1) return 'not known to the installed driver (an older build)';
  if (code === 5) return 'access denied';
  const d = String(m.detail || 'error').replace(/\.$/, '');
  return d.charAt(0).toLowerCase() + d.slice(1);
}

/*
 * A write that did not happen: show what the unit holds again. Its readback says it where it can;
 * else the value before the write (the pending entry's, or undo for fields without readback).
 */
function restoreWrite(idx, c, key) {
  const p = c.pending.get(key);
  const before = p ? p.prev : c.undo.get(key);
  c.pending.delete(key);
  c.undo.delete(key);
  const d = c.rbValid && c.words ? decodeControls(c.words, capsOf(unitType(idx))) : null;
  const pick = hw => (hw !== null && hw !== undefined ? hw : before);
  const [kind, a, field] = key.split(':');
  if (kind === 'hiz') {
    const prefs = unitPrefs(idx);
    if (prefs && before !== undefined) { prefs.hizImp = before; saveConfig(); }
  } else if (kind === 'in') {
    const ch = Number(a), v = pick(d && d.inputs[ch] ? d.inputs[ch][field] : null);
    if (v !== undefined && c.inputs[ch]) c.inputs[ch][field] = v;
  } else if (kind === 'link') {
    const v = pick(d ? d.links[Number(a)] : null);
    if (v !== undefined) c.links[Number(a)] = v;
  } else if (kind === 'mon') {
    const v = pick(d && a in d.mon ? d.mon[a] : null);
    if (v !== undefined) c.mon[a] = v;
    const rk = Object.keys(ROUTING).find(x => ROUTING[x] === a);     /* [Sess] the kept routing follows */
    if (rk && v !== undefined) storeRouting(idx, rk, v);
  } else if (kind === 'mix') {
    /* [Mix] no readback: the value before the run of writes (undefined = never set here: back to the unit's own) */
    const prev = before && typeof before === 'object' ? before.prev : undefined, x = MIX_WRITE.exec(key.slice(4));
    if (x && x[3]) {
      for (const [b, v] of Array.isArray(prev) ? prev : []) storeMix(idx, b, 'mute', v);
    } else if (x) {
      storeMix(idx, Number(x[2]), MIX_FIELD[x[1]], prev);
    }
  } else if (kind === 'out') {
    Outs.restore(idx, key.slice(4), before);       /* [Out] no readback: the value before the run of writes */
  } else if (kind === 'cue' || kind === 'alt') {
    /* [Monitor] no readback: the value before the run of writes (undefined = never set here) */
    const prev = before && typeof before === 'object' ? before.prev : undefined, name = key.slice(4);
    const q = CUE_WRITE.exec(name), al = ALT_WRITE.exec(name);
    if (q && q[1]) {
      const field = q[1] === 'send' ? 'level' : 'pan';
      storeSend(idx, q[2] - 1, Number(q[3]), field, prev !== undefined ? prev : (field === 'level' ? SEND_OFF : 0));
    } else if (q) {
      const cue = mixOf(idx).cues[q[4] - 1];
      if (prev === undefined) delete cue[q[5]]; else cue[q[5]] = prev;
      saveMix(idx);
    } else if (al) {
      const alt = mixOf(idx).alt[al[1] - 1], f = al[2] ? 'trim' : 'on';
      if (prev === undefined) delete alt[f]; else alt[f] = prev;
      saveMix(idx);
    }
  }
}

/*
 * The host's "error" for a write. A refusal (REFUSED: not supported, rejected, read-only) goes into
 * that unit's refused list, which the Control view shows inline under the unit selector until it is
 * dismissed or the setting is tried again; a passing problem (busy, not ready) stays a toast. A
 * refusal for a unit not on screen also toasts, since its note is not visible.
 */
function onHostError(m) {
  if (m.op === 'config') { toast(m.detail, 'bad', 8000); return; }
  const idx = msgUnit(m);
  const c = idx === 0 || S.units[idx] ? ctl(idx) : null;
  const what = writeName(m, idx), reason = errorReason(m, idx), key = writeKey(m);
  const refused = REFUSED.has(Number(m.code)) && !!c && !!key;
  if (c && key) {
    /* [Monitor] a cue, ALT or [Mix] monitor-mix setting that only failed for now (busy, not ready) stays as
       set: the app sends its saved mix again when the unit next connects */
    if (refused || !/^(cue|alt|mix):/.test(key)) restoreWrite(idx, c, key);
    if (refused) c.refused.set(key, { what, reason });
    if (key === 'mon:talkback') followTalkback(idx);      /* [Monitor] talkback did not go on: undo its dim */
  }
  /* on the Control view every present unit's segment is on screen (its note in ctlBar); elsewhere the selected one's */
  const shown = S.view === 'control' ? segLive(idx) || (idx === S.sel && !segs.size) : idx === S.sel;
  if (!refused || !shown) toastOnce(`${what} was not changed: ${reason}.`, 'bad');
  renderAllInputs();
  renderMonitor();
  renderCues();
  renderNotes();
}

/* ---------- readback ---------- */

/*
 * The control state readback words describe (spec 4.1, the same bits on the x8p and the Twin X):
 * preamp flags in word 0 (inputs 1-4) / 5 (5-8) at 6*(ch&3), GainC in word 3 / 7 (dB = GainC + 9;
 * 0 = not reported, gain null), links in the flag words, Hi-Z jacks word 0 bit 24+, monitor word 2,
 * talkback word 6 bit 8. Only the inputs, pairs (others null) and Hi-Z jacks model k has.
 */
function decodeControls(w, k) {
  const inputs = [];
  for (let ch = 0; ch < k.preamps; ch++) {
    const fw = ch < 4 ? w[0] : w[5], b = 6 * (ch & 3);
    const gc = ((ch < 4 ? w[3] : w[7]) >>> ((ch & 1) * 6 + (ch & 2) * 8)) & 0x3F;
    inputs.push({
      line: bit(fw, b), pad: bit(fw, b + 1), p48: bit(fw, b + 3), lowcut: bit(fw, b + 4), phase: bit(fw, b + 5),
      gain: gc === 0 ? null : gc >= 0x39 ? 65 : gc + 9, hiz: ch < k.hiz ? bit(w[0], 24 + ch) : false,
    });
  }
  const links = [0, 1, 2, 3].map(p => ((k.linkPairs >>> p) & 1 ? bit(p < 2 ? w[0] : w[5], 6 * ((p * 2) & 3) + 2) : null));
  const w2 = w[2] >>> 0;
  const mon = { level: w2 & 0xFF, hp1: (w2 >>> 8) & 0xFF, mute: bit(w2, 16), mono: bit(w2, 17), dim: bit(w2, 31), talkback: bit(w[6], 8),
    alt: (w[6] >>> 28) & 3 };                  /* [Monitor] the speaker set, oa:driver/ua_apollo.h UA_RB_ALTMON_SEL */
  return { inputs, links, mon };
}
const SPEAKERS = ['MAIN', 'ALT 1', 'ALT 2', 'ALT 3'];

/* Unit idx's readback: its control state follows (a pending write waits for it to agree). */
function onReadback(idx, m) {
  const c = ctl(idx);
  const ok = rbWordsOk(m);
  c.rbValid = ok;
  if (!ok) {
    if (idx === S.sel) renderStatus(); else if (idx === 0) renderUnits();
    return;
  }
  c.words = m.words;
  const d = decodeControls(m.words, capsOf(unitType(idx)));
  d.inputs.forEach((hw, ch) => {
    const inp = c.inputs[ch];
    for (const field of ['line', 'pad', 'p48', 'lowcut', 'phase']) {
      inp[field] = reconcile(c, `in:${ch}:${field}`, hw[field], inp[field],
        (want, got) => `${inputLabel(ch, idx)} ${flagName(field)}: sent ${onOff(want)}, the unit reports ${onOff(got)}.`);
    }
    if (hw.gain !== null) {
      inp.gain = reconcile(c, `in:${ch}:gain`, hw.gain, inp.gain,
        (want, got) => `${inputLabel(ch, idx)} gain: sent ${want} dB, the unit reports ${got} dB.`);
    }
    inp.hiz = hw.hiz;
  });
  d.links.forEach((v, p) => {
    if (v === null) return;
    c.links[p] = reconcile(c, `link:${p}`, v, c.links[p],
      (want, got) => `${idx ? `${unitShort(idx)} link` : 'Link'} ${p * 2 + 1}-${p * 2 + 2}: sent ${onOff(want)}, the unit reports ${onOff(got)}.`);
  });
  const tbBefore = c.mon.talkback;
  for (const [field, v] of Object.entries(d.mon)) {
    const say = x => (typeof x === 'boolean' ? onOff(x) : field === 'alt' ? SPEAKERS[x] : fmtLevel(x));
    c.mon[field] = reconcile(c, `mon:${field}`, v, c.mon[field],
      (want, got) => `${monLabel(idx)} ${field === 'alt' ? 'speakers' : field}: sent ${say(want)}, the unit reports ${say(got)}.`);
  }
  if (c.mon.talkback !== tbBefore) followTalkback(idx);  /* [Monitor] the unit's own TALK button too */
  renderUnitInputs(idx);               /* every unit's segment shows its own controls */
  Plug.renderSlots();                  /* [Plug-ins] links decide stereo inserts */
  if (idx === S.sel) {
    renderMonitor();
    renderCues();
    renderStatus();
    sendTrayState();
  } else {
    renderMaster(idx);
    renderMixFor(idx);
    if (idx === 0) renderUnits();      /* the Status view's x8p section reads ctl(0) */
  }
}

const flagName = f => ({ line: 'Mic/Line', pad: 'PAD', p48: '48V', lowcut: 'Low cut', phase: 'Polarity' }[f] || f);
const onOff = v => (v ? 'on' : 'off');

/* Unit idx's meters (peak per record slot). The Control view shows S.sel's, Channels the x8p's. */
function onLevels(idx, valid, peaks) {
  const c = ctl(idx);
  const changed = c.levelsValid !== !!valid;
  c.levelsValid = !!valid;
  if (changed) renderClockState();
  if (!valid) { c.peaks.fill(-Infinity); return; }
  for (let i = 0; i < c.peaks.length; i++) {
    const p = (peaks && peaks[i]) || 0;
    c.peaks[i] = p > 0 ? 20 * Math.log10(p / FULL_SCALE) : -Infinity;
  }
}

/* ------------------------------------------------------------------ */
/* Demo: a simulated x8p that answers through the same readback path   */
/* ------------------------------------------------------------------ */

const DEMO_INFO = {
  version: 2, stage: STAGE.CONNECTED, failedStage: 0, lastStatus: 0,
  flags: FLAG.FW_V2 | FLAG.IRQ64 | FLAG.WARM | FLAG.RINGS_LIVE | FLAG.DSPS | FLAG.ACEFACE | FLAG.CONNECTED |
    FLAG.MIXER_READY | FLAG.TRANSPORT | FLAG.SERVICE | FLAG.READBACK | FLAG.MSI,
  fpgaRev: 0xA2160A11, extCaps: 0x81600611, fwRelease: 0x0B040000, subsystemId: 0x14, deviceType: 0x22,
  publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000,
  servicePasses: 0, interruptCount: 0, notifStatusSeen: 0x00200000, numPreamps: 8, serial: 'DEMO2017SIM00001',
};
/* hw[slot]: the simulated unit in that slot (the x8p in 0, units.js's Twin X in 1); seen[slot]: the
   S.units entry it was built for (units.js gives a fresh one per scenario). */
const demo = { timer: 0, hw: [], seen: [], seq: 0, t0: 0, last: 0, lastLive: 0 };

/* Any unit's control state is reset when the demo starts or ends: refusals and pending writes
   belong to the host's units, not the simulated ones. */
function resetCtlWrites() {
  for (const c of S.ctl) if (c) { c.pending.clear(); c.undo.clear(); c.refused.clear(); c.mixTmp = null; c.tbDimmed = false; }
}

function enterDemo() {
  if (S.demo) return;
  S.demo = true;
  resetCtlWrites();
  const inputs = Array.from({ length: PREAMPS }, (_, i) => ({
    gain: [38, 32, 24, 24, 10, 10, 10, 10][i], line: i >= 2 && i < 4, pad: false,
    p48: i === 1, lowcut: i === 0, phase: false,
  }));
  const x8p = {
    inputs, links: [false, true, false, false], hiz: [false, false], w6: 0,
    mon: { level: 156, hp1: 160, mute: false, mono: false, dim: false, talkback: false, alt: 0 },
  };
  x8p.inputs[3] = { ...x8p.inputs[2] };
  demo.hw = [x8p];
  demo.seen = [];
  demo.t0 = now();
  demo.last = demo.t0;
  demo.lastLive = demo.t0;
  demo.timer = setInterval(demoTick, 33);
  demoPublish(0);
  clearLive();
  clearUnitList();                     /* the host's units wait outside the demo */
  onLive(demoLive());
  Units.demo(true);                    /* units.js: the demo's units (x8p + Twin X by default) */
  Plug.demoEnter();                    /* [Plug-ins] invented plug-ins, nothing reaches hardware */
  rateReset();
  demoRates();                         /* [Rate] the demo's units answer like a host with rate control */
  renderStatus();
}

function exitDemo() {
  if (!S.demo) return;
  S.demo = false;
  clearInterval(demo.timer);
  demo.hw = [];
  demo.seen = [];
  resetCtlWrites();
  const c0 = ctl(0);
  c0.levelsValid = false;
  c0.peaks.fill(-Infinity);
  clearLive();                         /* the host resends its own on 'resend' */
  clearUnitList();                     /* ... and its "units" */
  Units.demo(false);                   /* units.js: drop the demo's page state */
  Plug.demoExit();                     /* [Plug-ins] */
  Endpoints.demoExit();                /* [Endpoints] */
  if (host) Plug.hello();
  if (c0.hostReadback) onReadback(0, c0.hostReadback);
  else c0.rbValid = false;
  send('resend');
  rateReset();
  rateAsk();                           /* [Rate] the host's own answer */
  renderStatus();
}

/*
 * The demo's other units, as units.js lists them: a simulated model for each one that runs,
 * built from the readback words units.js gave its slot (so the pages agree), with an instrument
 * in the Twin X's Hi-Z jack and the monitor at a listening level. None for an observe unit.
 */
function demoUnits() {
  for (let idx = 1; idx < MAX_UNITS; idx++) {
    const u = S.units[idx];
    if (demo.seen[idx] === u) continue;
    demo.seen[idx] = u;
    demo.hw[idx] = undefined;
    S.ctl[idx] = undefined;
    if (!u || !u.rbValid || !Array.isArray(u.words)) continue;
    const k = capsOf((u.info && u.info.deviceType) || 0);
    const d = decodeControls(u.words, k);
    demo.hw[idx] = {
      inputs: d.inputs.map(x => ({ ...x, gain: x.gain || 10 })),
      links: d.links.map(v => !!v),
      hiz: d.inputs.map((x, ch) => ch === 0 && k.hiz > 0),
      w6: (u.words[6] & 0xC0000000) >>> 0,                /* its clock lock, as units.js reports it */
      mon: { ...d.mon, level: d.mon.level || 150, hp1: d.mon.hp1 || 148 },
    };
    demoPublish(idx);
  }
}

function demoCommand(idx, parts) {
  const [op, a, b, c] = parts;
  setTimeout(() => {
    const h = demo.hw[idx];
    if (!h) return;
    if (op === 'input') {
      const ch = Number(a), v = Number(c);
      if (!h.inputs[ch]) return;
      if (b === 'link') h.links[ch >> 1] = v === 1;
      else if (b === 'gain') h.inputs[ch].gain = v;
      else if (b === 'hizimp') h.hizImp = v;          /* no readback field, like the hardware */
      else h.inputs[ch][INPUT_FIELDS[b]] = v === 1;
    } else if (op === 'monitor') {
      const field = MON_FIELDS[a], v = Number(b);
      if (field in h.mon) h.mon[field] = (field === 'level' || field === 'hp1' || field === 'alt') ? v : v === 1;
    }
    demoPublish(idx);
  }, 40 + Math.random() * 80);
}

/* Like the hardware: a linked pair runs, and reads back, both inputs from the first input's settings; the
   second input keeps what was written to it (ignored while linked) for when the pair is unlinked. */
const demoEffective = (h, ch) => ((ch & 1) && h.links[ch >> 1] && h.inputs[ch - 1] ? h.inputs[ch - 1] : h.inputs[ch]);

function demoReadbackWords(h) {
  const w = new Array(40).fill(0);
  h.inputs.forEach((_, ch) => {
    const inp = demoEffective(h, ch), b = 6 * (ch & 3);
    const f = (inp.line ? 1 : 0) | (inp.pad ? 2 : 0) | (inp.p48 ? 8 : 0) | (inp.lowcut ? 16 : 0) | (inp.phase ? 32 : 0);
    w[ch < 4 ? 0 : 5] |= f << b;
    w[ch < 4 ? 3 : 7] |= ((inp.gain - 9) & 0x3F) << ((ch & 1) * 6 + (ch & 2) * 8);
  });
  h.links.forEach((on, p) => { if (on) w[p < 2 ? 0 : 5] |= 0x104 << (12 * (((p * 2) & 3) >> 1)); });
  h.hiz.forEach((on, ch) => { if (on && ch < 2) w[0] |= 1 << (24 + ch); });
  const m = h.mon;
  w[2] = m.level | (m.hp1 << 8) | (m.mute ? 1 << 16 : 0) | (m.mono ? 1 << 17 : 0) | (m.dim ? 1 << 31 : 0);
  w[6] = (m.talkback ? 1 << 8 : 0) | ((m.alt || 0) << 28) | h.w6;
  w[38] = 0x00010203;
  return w.map(x => x >>> 0);
}

/* The simulated unit answers through the host's path: its readback, for the controls and Status. */
function demoPublish(idx) {
  demo.seq++;
  const m = { type: 'readback', unit: idx, valid: true, seq: demo.seq, words: demoReadbackWords(demo.hw[idx]) };
  storeReadback(m);
  onReadback(idx, m);
}

function demoTick() {
  const t = (now() - demo.t0) / 1000;
  const rnd = () => Math.random();
  const lvl = (h, db, ch) => {
    const inp = demoEffective(h, ch);
    const d = db + (inp.gain - 38) - (inp.pad ? 20 : 0);
    return d > -100 ? Math.round(FULL_SCALE * Math.pow(10, Math.min(0, d) / 20)) : 0;
  };
  demo.hw.forEach((h, idx) => {
    if (!h) return;
    const peaks = new Array(32).fill(0);
    if (idx === 0) {
      // Input 1: speech, phrases of a few seconds with pauses.
      const phrase = Math.sin(t * 0.9) * Math.sin(t * 0.23 + 1);
      peaks[0] = phrase > 0.15 ? lvl(h, -22 + 9 * rnd() * Math.abs(Math.sin(t * 9)), 0) : lvl(h, -62 + 4 * rnd(), 0);
      // Input 2: condenser on an acoustic guitar.
      peaks[1] = lvl(h, -26 + 10 * Math.abs(Math.sin(t * 3.1)) * rnd(), 1);
      // Inputs 3-4: stereo keys, linked.
      const keys = -24 + 8 * Math.abs(Math.sin(t * 1.3));
      peaks[2] = lvl(h, keys + rnd() * 2, 2);
      peaks[3] = lvl(h, keys - 1 + rnd() * 2, 3);
      for (let ch = 4; ch < 8; ch++) peaks[ch] = lvl(h, -90 + rnd() * 3, ch);
    } else {
      // Twin X input 1: a bass in the Hi-Z jack; input 2: a room mic.
      peaks[0] = lvl(h, -14 + 6 * Math.abs(Math.sin(t * 2.2)) * rnd(), 0);
      peaks[1] = lvl(h, -40 + 8 * rnd() * Math.abs(Math.sin(t * 0.7)), 1);
    }
    // Monitor L/R carries the mix (record slots 25-26 on the x8p).
    const mix = Math.max(peaks[0], peaks[1], peaks[2]) * 0.7;
    peaks[24] = h.mon.mute ? 0 : Math.round(mix);
    peaks[25] = h.mon.mute ? 0 : Math.round(mix * 0.96);
    // [Mix] The monitor-mix loopback (slots 26-27, before the monitor level): the inputs through the mix set here.
    const ins = mixBuses(capsOf(unitType(idx))).filter(b => b !== TALKBACK_BUS), vals = ins.map(b => mixOfBus(idx, b));
    const solo = vals.some(v => v.solo);
    let out = 0;
    ins.forEach((b, i) => {
      const v = vals[i];
      if (!v.mute && !(solo && !v.solo) && v.level > SEND_OFF) out = Math.max(out, peaks[b] * Math.pow(10, v.level / 40));
    });
    peaks[26] = peaks[27] = Math.min(FULL_SCALE, Math.round(out));
    onLevels(idx, true, peaks);
  });
  DEMO_INFO.servicePasses = Math.round(t * 100);
  DEMO_INFO.interruptCount = Math.round(t * 46.9);
  if (now() - demo.lastLive >= 1000) { demo.lastLive = now(); onLive(demoLive()); }
}

/* The host's "live" message as the x8p answered it on Nov 14 2026: strips built on all eight
   inputs, the 1 ms engine running, Windows playing to MON L/R. */
function demoLive() {
  const t = (now() - demo.t0) / 1000;
  const jitter = s => (Math.random() - 0.5) * s;
  const ep = (name, circuit, render, slots, extra) => ({
    name, circuit, render, channels: 2, slots, flags: 0x3, packetCount: 0, packetBytes: 0, streams: 0,
    underruns: 0, overruns: 0, dropped: 0, ...extra,
  });
  return {
    type: 'live', unit: 0,
    engine: {
      version: 1, running: 1, lost: 0, starts: 1, ticks: Math.round(t * 1000), lateTicks: 0, busStalls: 0,
      deadReads: 0, maxIntervalUs: 1210, asioState: 0, asioFrames: 0, asioIn: 0, asioOut: 0,
      asioUnderruns: 0, asioOverruns: 0, asioSkipped: 0,
      ticksPerSec: 1000 + jitter(0.4), framesPerSec: 48000 + jitter(0.3),
    },
    strips: {
      version: 2, state: 4, flags: 0x79, enable: 1, mask: 0xFF, stepsLimit: 9, stepsDone: 9, phantomState: 1,
      phantomMask: 0x02, why: 'stage 9 (+ 48V resync) done on StripMask 0xff, DSP 1', perInput: true,
      taken: new Array(PREAMPS).fill(2), started: new Array(PREAMPS).fill(1),
    },
    endpoints: {
      version: 1, count: 2, enabledMask: 0x3, prepared: 1, layoutOk: true,
      rows: [
        ep('MON L/R', 'ApolloMon', 1, [0, 1], { flags: 0x1F, packetCount: 2, packetBytes: 3840, streams: 1 }),
        ep('MON L/R', 'ApolloMonIn', 0, [26, 27]),
      ],
    },
  };
}

const DEMO_LOG = [
  'apollo.sys loading (phase 0 diagnostic build)',
  'BAR0 at 0x6a000000, 65536 bytes',
  "FPGA 0xa2160a11 subsys 0x0014 serial 'DEMO2017SIM00001' type 0x22 public 0x16 DSPs 6 fw 11.4.0 irq64 1",
  'previous owner left 0x01A4=0x00000000 0x01AC=0x00000000',
  'liveness: SEQ_WR 41 SEQ_RD 41 MIXER 0x00000001',
  'warm start',
  '[early] DSP 0 status 0x00001c00 fault bits 0x00000000',
  'ACEFACE answered on try 1',
  'rate ack 48000 Hz sent to 6 DSPs',
  'mixer init: SEQ 41, capture-routing batch queued',
  'bring-up complete: connected, transport idle',
  '(demo: simulated log)',
].join('\n') + '\n';

/* ------------------------------------------------------------------ */
/* Knob                                                                */
/* ------------------------------------------------------------------ */

const KNOB_START = -135, KNOB_END = 135;
function polar(r, deg) {
  const a = (deg * Math.PI) / 180;
  return [50 + r * Math.sin(a), 50 - r * Math.cos(a)];
}
function arcPath(r, from, to) {
  const [x0, y0] = polar(r, from), [x1, y1] = polar(r, to);
  const large = to - from > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

/*
 * Knob: the model of a rotary control (value, range, step, taper, format, parse, reset, locked / unconfirmed / refused,
 * the accessible name and the keyboard map, the same in every theme). Its view and pointer interaction come from the
 * active theme pack (skin.js: knob.build), rebuilt in the same host element when the theme changes.
 * Keys (every theme): arrows one step, Page Up / Down a page, Home / End, Enter types a value; wheel steps (Shift: fine);
 * double-click resets to o.reset where there is one (unity, centre), else types a value.
 */
class Knob {
  constructor(o) {
    this.o = Object.assign({ size: 64, min: 0, max: 100, step: 1, wheelStep: 1, pageStep: 10, label: '' }, o);
    this.o.toPos = this.o.toPos || (v => (v - this.o.min) / (this.o.max - this.o.min));
    this.o.fromPos = this.o.fromPos || (p => this.o.min + p * (this.o.max - this.o.min));
    this.value = this.o.value ?? this.o.min;
    this.max = this.o.max;
    this.locked = false;
    this.dragging = false;
    this.readout = el('div', { class: 'k-readout' });
    this.node = el('div', {
      class: 'knob', tabindex: '0', role: 'slider', 'aria-label': this.o.aria || this.o.label,
      'aria-valuemin': String(this.o.min), 'aria-valuemax': String(this.o.max),
    });
    this.node.style.setProperty('--k-size', `${this.o.size}px`);
    this.readout.addEventListener('dblclick', e => { e.stopPropagation(); this.edit(); });
    this.node.addEventListener('dblclick', () => this.reset());
    this.node.addEventListener('wheel', e => this.wheel(e), { passive: false });
    this.node.addEventListener('keydown', e => this.key(e));
    this.skinRebuild();
    Skin.attach(this);
  }

  /* the active theme's view, in this host (theme switches call it again) */
  skinRebuild() {
    if (this.view && this.view.destroy) this.view.destroy();
    this.node.replaceChildren();
    this.skinId = Skin.id();
    this.node.dataset.skin = this.skinId;
    this.view = Skin.r('knob').build(this, Skin.R);
    this.draw();
  }

  set(v) { if (this.dragging) return; this.value = v; this.draw(); }
  setMax(max) { this.max = max; this.node.setAttribute('aria-valuemax', String(max)); }
  setLocked(on) { this.locked = on; this.node.classList.toggle('locked', on); }
  setUnconfirmed(on) { this.node.classList.toggle('unconfirmed', on); }

  draw() {
    if (this.skinId !== Skin.id() && this.node.isConnected) { this.skinRebuild(); return; }
    const p = clamp(this.o.toPos(this.value), 0, 1);
    const text = this.o.format(this.value);
    this.view.draw({ p, p0: this.o.origin || 0, text, value: this.value });
    if (this.readout.textContent !== text) this.readout.textContent = text;
    this.node.setAttribute('aria-valuenow', String(this.value));
    this.node.setAttribute('aria-valuetext', text);
  }

  commit(v, final) {
    v = clamp(v, this.o.min, this.max);
    if (v === this.value && !final) return;
    const changed = v !== this.value;
    this.value = v;
    this.draw();
    if (changed) this.o.onChange(v);
  }

  blocked() {
    if (!this.locked) return false;
    toastOnce(this.o.lockedText || 'This control is locked in Settings.', 'info');
    return true;
  }

  snap(v) { return clamp(Math.round(v / this.o.step) * this.o.step, this.o.min, this.max); }
  reset() {
    if (this.blocked()) return;
    if (this.o.reset !== undefined) this.commit(this.snap(this.o.reset), true);
    else this.edit();
  }
  wheel(e) {
    e.preventDefault();
    if (this.blocked()) return;
    const dir = e.deltaY < 0 ? 1 : -1;
    this.commit(this.snap(this.value + dir * (e.shiftKey ? this.o.step : this.o.wheelStep)));
  }
  key(e) {
    const k = e.key;
    let v = null;
    if (k === 'ArrowUp' || k === 'ArrowRight') v = this.value + this.o.step;
    else if (k === 'ArrowDown' || k === 'ArrowLeft') v = this.value - this.o.step;
    else if (k === 'PageUp') v = this.value + this.o.pageStep;
    else if (k === 'PageDown') v = this.value - this.o.pageStep;
    else if (k === 'Home') v = this.o.min;
    else if (k === 'End') v = this.max;
    else if (k === 'Enter') { this.edit(); e.preventDefault(); return; }
    if (v === null) return;
    e.preventDefault();
    if (this.blocked()) return;
    this.commit(this.snap(v));
  }
  edit() {
    if (this.blocked() || !this.o.parse || !this.readout.isConnected) return;
    const input = el('input', { class: 'knob-edit', type: 'text', value: this.o.editText(this.value), 'aria-label': this.o.aria || this.o.label });
    this.readout.replaceWith(input);
    input.focus();
    input.select();
    let closed = false;
    const close = apply => {
      if (closed) return;
      closed = true;
      if (apply) {
        const v = this.o.parse(input.value);
        if (v === null || Number.isNaN(v)) toast('Not a value this control takes.', 'warn');
        else this.commit(this.snap(v), true);
      }
      if (input.isConnected) input.replaceWith(this.readout);
      this.node.focus();
    };
    input.addEventListener('keydown', e => {
      if (e.key === 'Enter') close(true);
      else if (e.key === 'Escape') close(false);
      e.stopPropagation();
    });
    input.addEventListener('blur', () => close(true));
  }
}

/* Monitor taper: the bottom quarter covers -96..-48 dB, the rest -48..0 dB. */
const monToPos = raw => {
  const db = rawToDb(raw);
  return db <= -48 ? ((db + 96) / 48) * 0.25 : 0.25 + ((db + 48) / 48) * 0.75;
};
const monFromPos = p => {
  const db = p <= 0.25 ? -96 + (p / 0.25) * 48 : -48 + ((p - 0.25) / 0.75) * 48;
  return LEVEL_MAX + 2 * db;
};
const parseDb = s => {
  const v = parseFloat(String(s).replace(/db/i, '').trim());
  return Number.isFinite(v) ? Math.round(LEVEL_MAX + 2 * clamp(v, -96, 0)) : null;
};

/* ------------------------------------------------------------------ */
/* Meter                                                               */
/* ------------------------------------------------------------------ */

const METERS = [];
function frac(db, floor) {
  if (!(db > floor)) return 0;
  return Math.pow(Math.min(1, (db - floor) / -floor), 1.6);
}

/*
 * A peak meter of one record slot: group 'control' (unit: the segment's slot) or 'channels' (the x8p). The model keeps
 * the ballistics (release, hold, clip), a ghost peak (the highest of the last 3 s, falling slowly) and an average (a 300 ms
 * power average of the peaks: an RMS-like figure from the 30 Hz peak feed). The theme pack's meter view (skin.js
 * meter.build) adds what it shows of them to the shared bar, clip light and scale, and paints only on change.
 */
class Meter {
  constructor({ scale = true, slot = 0, group = 'control', unit = 0 } = {}) {
    this.slot = slot;
    this.group = group;
    this.unit = unit;
    this.hasScale = scale;
    this.cur = -Infinity;
    this.hold = -Infinity;
    this.holdAt = 0;
    this.ghost = -Infinity;
    this.ghostAt = 0;
    this.avgPow = 0;
    this.clip = false;
    this.last = '';
    this.node = el('div', { class: 'meter-wrap', title: 'Click to clear the peak hold and clip light', onclick: () => this.reset() });
    METERS.push(this);
    this.skinRebuild();
    Skin.attach(this);
  }
  skinRebuild() {
    if (this.view && this.view.destroy) this.view.destroy();
    const noclock = this.meter ? this.meter.classList.contains('noclock') : false;
    this.clipEl = el('div', { class: 'meter-clip', title: 'Clip. Click the meter to clear.' });
    this.fill = el('div', { class: 'meter-fill' });
    this.holdEl = el('div', { class: 'meter-hold' });
    this.bar = el('div', { class: 'meter-bar' }, this.fill, this.holdEl);
    this.scaleEl = this.hasScale ? el('div', { class: 'meter-scale', 'aria-hidden': 'true' }) : null;
    this.col = el('div', { class: 'meter-col' }, this.clipEl, this.bar);
    this.meter = el('div', { class: 'meter' }, this.col, this.scaleEl);
    this.meter.classList.toggle('noclock', noclock);
    this.node.replaceChildren(this.meter);
    this.horiz = false;                                    /* a view may lay the meter on its side (rows) */
    this.skinId = Skin.id();
    this.node.dataset.skin = this.skinId;
    this.view = Skin.r('meter').build(this, Skin.R) || { paint() {} };
    this.clipEl.classList.toggle('on', this.clip);
    this.applyFloor();
  }
  applyFloor() {
    const floor = cfg.meters.floorDb;
    const y18 = (frac(-18, floor) * 100).toFixed(1), y6 = (frac(-6, floor) * 100).toFixed(1);
    this.fill.style.background =
      `linear-gradient(to ${this.horiz ? 'right' : 'top'}, var(--meter-lo) 0 ${y18}%, var(--meter-mid) ${y18}% ${y6}%, var(--meter-hi) ${y6}% 100%)`;
    this.node.classList.toggle('horiz', this.horiz);
    this.node.style.setProperty('--y18', `${y18}%`);
    this.node.style.setProperty('--y6', `${y6}%`);
    if (this.scaleEl) {
      const ticks = METER_TICKS[String(floor)] || METER_TICKS['-60'];
      this.scaleEl.replaceChildren(...ticks.map(d => el('span', { style: `bottom:${(frac(d, floor) * 100).toFixed(1)}%`, text: String(d) })));
    }
    this.last = '';
  }
  reset() { this.hold = -Infinity; this.ghost = -Infinity; this.clip = false; this.clipEl.classList.remove('on'); this.node.classList.remove('clipped'); }
  update(target, dt, t) {
    if (this.skinId !== Skin.id()) this.skinRebuild();
    const rel = cfg.meters.releaseDbPerSec;
    this.cur = target >= this.cur ? target : Math.max(target, this.cur - rel * dt);
    const holdMs = cfg.meters.holdMs;
    if (holdMs === 0) this.hold = -Infinity;
    else if (target >= this.hold) { this.hold = target; this.holdAt = t; }
    else if (holdMs > 0 && t - this.holdAt > holdMs) this.hold = Math.max(this.cur, this.hold - rel * 2 * dt);
    if (target >= this.ghost) { this.ghost = target; this.ghostAt = t; }
    else if (t - this.ghostAt > 3000) this.ghost = Math.max(this.cur, this.ghost - rel * 0.5 * dt);
    const pw = target > -200 ? Math.pow(10, target / 10) : 0;
    this.avgPow += (pw - this.avgPow) * Math.min(1, dt / 0.3);
    this.avg = this.avgPow > 0 ? 10 * Math.log10(this.avgPow) : -Infinity;
    if (target >= -0.1 && !this.clip) {
      this.clip = true;
      this.clipEl.classList.add('on');
      this.node.classList.add('clipped');
      Skin.on('clip', this.node);
    }
    const floor = cfg.meters.floorDb;
    const f = frac(this.cur, floor), h = frac(this.hold, floor), g = frac(this.ghost, floor), a = frac(this.avg, floor);
    const key = `${f.toFixed(3)}|${h.toFixed(3)}|${g.toFixed(3)}|${a.toFixed(3)}`;
    if (key === this.last) return;
    this.last = key;
    if (this.horiz) {
      this.fill.style.clipPath = `inset(0 ${((1 - f) * 100).toFixed(2)}% 0 0)`;
      this.holdEl.style.left = `calc(${(h * 100).toFixed(2)}% - 1px)`;
    } else {
      this.fill.style.clipPath = `inset(${((1 - f) * 100).toFixed(2)}% 0 0 0)`;
      this.holdEl.style.bottom = `calc(${(h * 100).toFixed(2)}% - 1px)`;
    }
    this.holdEl.style.opacity = h > 0.004 ? '0.85' : '0';
    this.view.paint(f, h, g, a);
  }
  setNoClock(on) { this.meter.classList.toggle('noclock', on); }
}

function peakText(db) {
  return db > cfg.meters.floorDb ? db.toFixed(1) : '--';
}

/* ------------------------------------------------------------------ */
/* Fader: a console channel fader for the monitor mix                  */
/* ------------------------------------------------------------------ */

/*
 * Fader: the model of a channel fader for the monitor mix: off (-inf) .. +12 dB on the send taper (sendToPos: -60..-20 dB
 * over the lower third, -20..+12 dB over the rest), a numeric readout (placed by the strip) and the keyboard map (arrows
 * 0.5 dB, Page Up/Down 6 dB, Home off, End +12 dB, Enter types a value), double-click for 0 dB, the wheel (Shift / Ctrl /
 * Alt: fine). role slider, vertical. Its view (scale, track, cap) and drag feel come from the theme pack (skin.js
 * fader.build): the view must create this.track (class fd-track), the drag surface; trackDrag() is the shared drag
 * (the cap drags relative, the track jumps; Ctrl / Alt / Shift fine; `opts.steps` quantizes).
 */
const FADER_TICKS = [12, 6, 0, -6, -12, -20, -30, -40, -60];
class Fader {
  constructor(o) {
    this.o = Object.assign({ min: SEND_OFF, max: SEND_MAX, value: 0, unity: 0, toPos: sendToPos, fromPos: sendFromPos,
      format: fmtSend, parse: parseSend }, o);
    this.value = this.o.value;
    this.dragging = false;
    this.node = el('div', {
      class: 'fader', tabindex: '0', role: 'slider', 'aria-orientation': 'vertical', 'aria-label': this.o.aria,
      'aria-valuemin': String(this.o.min), 'aria-valuemax': String(this.o.max),
    });
    this.readout = el('div', { class: 'fd-readout num', title: 'Double-click the fader for 0 dB, or here to type a value' });
    this.node.addEventListener('dblclick', () => this.commit(this.o.unity));
    this.readout.addEventListener('dblclick', () => this.edit());
    this.node.addEventListener('wheel', e => this.wheel(e), { passive: false });
    this.node.addEventListener('keydown', e => this.key(e));
    this.skinRebuild();
    Skin.attach(this);
  }
  skinRebuild() {
    if (this.view && this.view.destroy) this.view.destroy();
    this.node.replaceChildren();
    this.node.setAttribute('aria-orientation', 'vertical');
    this.skinId = Skin.id();
    this.node.dataset.skin = this.skinId;
    this.view = Skin.r('fader').build(this, Skin.R);
    this.draw();
  }
  blocked() { return false; }
  snap(v) { return Math.round(v); }
  /* the printed dB legend most views put beside the track */
  scaleNode(extra) {
    return el('div', { class: 'fd-scale', 'aria-hidden': 'true' },
      FADER_TICKS.map(db => el('span', { class: db === 0 ? 'unity' : null, style: `bottom:${(this.o.toPos(db * 2) * 100).toFixed(1)}%`, text: db > 0 ? `+${db}` : String(db) })),
      el('span', { style: 'bottom:0%', text: '∞' }), extra || null);
  }
  /* the shared drag on the view's track: returns the undo for destroy() */
  trackDrag(track, opts = {}) {
    const posAt = y => { const r0 = track.getBoundingClientRect(); return clamp((r0.bottom - y) / Math.max(1, r0.height), 0, 1); };
    const q = p => (opts.steps ? Math.round(p * opts.steps) / opts.steps : p);
    const down = e => {
      if (e.button !== 0) return;
      track.setPointerCapture(e.pointerId);
      this.node.focus();
      this.dragging = true;
      this.lastY = e.clientY;
      this.pos = clamp(this.o.toPos(this.value), 0, 1);
      if (!(e.target.closest && e.target.closest('.fd-cap'))) { this.pos = posAt(e.clientY); this.commitPos(q(this.pos)); }
    };
    const move = e => {
      if (!this.dragging) return;
      const r0 = track.getBoundingClientRect(), fine = e.ctrlKey || e.altKey || e.shiftKey;
      this.pos = clamp(this.pos + (this.lastY - e.clientY) / Math.max(1, r0.height) * (fine ? 0.15 : 1), 0, 1);
      this.lastY = e.clientY;
      this.commitPos(q(this.pos));
    };
    const up = () => { this.dragging = false; };
    track.addEventListener('pointerdown', down);
    track.addEventListener('pointermove', move);
    track.addEventListener('pointerup', up);
    track.addEventListener('pointercancel', up);
    return () => { track.removeEventListener('pointerdown', down); track.removeEventListener('pointermove', move); track.removeEventListener('pointerup', up); track.removeEventListener('pointercancel', up); };
  }
  set(v) { if (this.dragging) return; this.value = v; this.draw(); }
  setUnconfirmed(on) { this.node.classList.toggle('unconfirmed', on); }
  draw() {
    if (this.skinId !== Skin.id() && this.node.isConnected) { this.skinRebuild(); return; }
    const p = clamp(this.o.toPos(this.value), 0, 1);
    const text = this.o.format(this.value);
    this.view.draw({ p, text, value: this.value });
    if (this.readout.textContent !== text) this.readout.textContent = text;
    this.node.setAttribute('aria-valuenow', String(this.value));
    this.node.setAttribute('aria-valuetext', text);
  }
  commit(v) {
    v = clamp(Math.round(v), this.o.min, this.o.max);
    if (v === this.value) return;
    this.value = v;
    this.draw();
    this.o.onChange(v);
  }
  commitPos(p) {
    const v = clamp(this.o.fromPos(p), this.o.min, this.o.max);
    if (v !== this.value) { this.value = v; this.draw(); this.o.onChange(v); }
    else this.draw();
  }
  step(n) { this.commit(this.value + n); }
  wheel(e) {
    e.preventDefault();
    const dir = e.deltaY < 0 ? 1 : -1;
    this.step(dir * (e.shiftKey || e.ctrlKey || e.altKey ? 1 : 2));
  }
  key(e) {
    const k = e.key;
    if (k === 'ArrowUp' || k === 'ArrowRight') this.step(1);
    else if (k === 'ArrowDown' || k === 'ArrowLeft') this.step(-1);
    else if (k === 'PageUp') this.step(12);
    else if (k === 'PageDown') this.step(-12);
    else if (k === 'Home') this.commit(this.o.min);
    else if (k === 'End') this.commit(this.o.max);
    else if (k === 'Enter') this.edit();
    else return;
    e.preventDefault();
  }
  edit() {
    if (!this.readout.isConnected) return;
    const input = el('input', { class: 'knob-edit fd-edit', type: 'text', value: this.value <= SEND_OFF ? 'off' : (this.value / 2).toFixed(1), 'aria-label': this.o.aria });
    this.readout.replaceWith(input);
    input.focus();
    input.select();
    let closed = false;
    const close = apply => {
      if (closed) return;
      closed = true;
      if (apply) {
        const v = this.o.parse(input.value);
        if (v === null || Number.isNaN(v)) toast('Not a value this fader takes: -60 to +12 dB, or off.', 'warn');
        else this.commit(v);
      }
      if (input.isConnected) input.replaceWith(this.readout);
      this.node.focus();
    };
    input.addEventListener('keydown', e => {
      if (e.key === 'Enter') close(true);
      else if (e.key === 'Escape') close(false);
      e.stopPropagation();
    });
    input.addEventListener('blur', () => close(true));
  }
}

/* ------------------------------------------------------------------ */
/* Control view: one segment per unit, side by side                    */
/* ------------------------------------------------------------------ */

/*
 * Every present unit of a model the app knows gets a segment, side by side and never behind tabs: its head (name,
 * model, clock, sample rate, DSP room), its input strips in signal-flow order (preamp, plug-in insert, monitor-mix
 * fader with pan, M and S, and the input meter), a talkback strip and its master section (monitor level, MUTE,
 * DIM, main out L/R meters, MUTE ALL). Every control sends to its own unit (hwSend with the strip's slot).
 * S.sel is the focused unit: the monitor column (source, speakers, headphones, ALT, talkback), the top bar,
 * presets, the tray and M / D / O follow it; a click on a segment's name or its master's MON button focuses it.
 * A unit that leaves greys out in place (nothing reflows) until it comes back or is hidden; a placeholder that
 * never connected is not kept. The strips scroll sideways when the window is narrower than every segment at
 * STRIP_MIN.
 */
const segs = new Map();                /* unit slot -> segment */
const STRIP_MIN = 70, TB_W = 52, MASTER_W = 98;

/* ---------- which units the Control view shows, and the focused one ---------- */

/* The units the Control view can drive: every present unit of a model it knows (MODEL_CAPS), in
   slot order. Without a host list (an older host, or before the first one) the x8p's slot 0. */
function controlUnits() {
  if (!S.unitList) return [{ index: 0, id: '', deviceType: unitType(0), serial: '' }];
  return S.unitList.filter(u => u.present && u.index >= 0 && u.index < MAX_UNITS && MODEL_CAPS[u.deviceType] &&
    (u.index === 0 || S.units[u.index])).sort((a, b) => a.index - b.index);
}

/* --view control:<twinx|x8p|slot> (?view= in a browser) asks for a unit until the user picks one. */
const pinMatches = (u, pin) => String(u.index) === pin ||
  (capsOf(u.deviceType).short || '').replace(/\s+/g, '').toLowerCase() === pin;

/* The focused slot: the unit asked for, else the one picked last (cfg.controlUnit) while it is present, else the
   x8p's slot 0, else the first present unit. */
function wantedUnit() {
  const list = controlUnits();
  const u = (S.pin && list.find(x => pinMatches(x, S.pin))) ||
    (cfg.controlUnit && list.find(x => x.id === cfg.controlUnit)) || list.find(x => x.index === 0) || list[0];
  return u ? u.index : 0;
}

/* After any change in who is present: the segments follow, and the focus follows the wanted unit (a remembered one
   that comes back is focused again; one that leaves hands over to the x8p). */
function syncUnit() {
  syncSegments();
  const idx = wantedUnit();
  if (idx !== S.sel) { showUnit(idx); return; }
  renderUnitBar();
}

/* The user focuses a unit: remembered by its unit id for the next start. */
function selectUnit(idx) {
  const u = controlUnits().find(x => x.index === idx);
  if (!u) return;
  S.pin = '';
  if (u.id && cfg.controlUnit !== u.id) { cfg.controlUnit = u.id; saveConfig(); }
  if (idx !== S.sel) showUnit(idx);
  else renderUnitBar();
}

/* The monitor column, the top bar, presets and the tray follow slot idx from now on. Nothing is sent. */
function showUnit(idx) {
  S.sel = idx;
  syncSegments();
  Plug.renderAll();                    /* [Plug-ins] the browser lists the focused unit's plug-ins when it hosts */
  renderMonitor();
  renderCues();
  renderUnitBar();
  renderStatus();
  sendTrayState();
}

/* ---------- the unit bars: the Cues page's selector, the monitor column's, and the notes ---------- */

/* [Monitor] ctlBar: every unit's notes over the segments (no selector: the segments are side by side);
   cueBar: the Cues page's selector and its unit's notes. */
const newBar = (id, all) => ({ id, all, root: null, row: null, seg: null, segKey: '', buttons: [], notes: null, notesKey: '' });
const bars = [newBar('ctlBar', true), newBar('cueBar', false)];

function joinList(items, max = 4) {
  const shown = items.length > max ? items.slice(0, max - 1) : items;
  const rest = items.length - shown.length;
  if (rest) return `${shown.join(', ')} and ${rest} more`;
  return shown.length > 1 ? `${shown.slice(0, -1).join(', ')} and ${shown[shown.length - 1]}` : shown.join('');
}

/* One button per unit, pressed on the focused one (the Cues page and the monitor column). */
function unitButtons(holder, list) {
  const key = list.map(u => `${u.index}:${u.deviceType}:${u.serial}`).join(',');
  if (key !== holder.segKey) {
    holder.segKey = key;
    const label = u => capsOf(u.deviceType).short || u.model;
    holder.buttons = list.map(u => {
      const twin = list.some(x => x !== u && label(x) === label(u));
      const text = twin && u.serial ? `${label(u)} ${u.serial.slice(-4)}` : label(u);
      const b = el('button', { 'aria-pressed': 'false', title: [u.model, u.serial].filter(Boolean).join(' · ') || null,
        onclick: () => selectUnit(u.index) }, text);
      b.dataset.unit = String(u.index);
      return b;
    });
    holder.seg.replaceChildren(...holder.buttons);
  }
  for (const b of holder.buttons) setPressed(b, Number(b.dataset.unit) === S.sel);
}

function renderUnitBar() {
  for (const bar of bars) renderOneBar(bar);
  renderMonUnits();
  renderSegHeads();
  renderNotes();
}

function renderOneBar(bar) {
  if (!bar.root) {
    bar.root = document.getElementById(bar.id);
    if (!bar.root) return;
    bar.seg = el('div', { class: 'seg ctl-units', role: 'group', 'aria-label': 'Unit to control' });
    bar.row = el('div', { class: 'ctl-row' }, el('span', { class: 'lbl', text: 'Unit' }), bar.seg);
    bar.notes = el('div', { class: 'ctl-notes', 'aria-live': 'polite' });
    bar.root.append(bar.row, bar.notes);
  }
  const list = controlUnits();
  if (!bar.all) unitButtons(bar, list);
  bar.row.hidden = bar.all || list.length < 2;     /* one unit: the top bar already names it */
}

/* The monitor column's unit buttons (two or more units). */
function renderMonUnits() {
  if (!mon.units) return;
  const list = controlUnits();
  unitButtons(mon.unitsHolder, list);
  mon.units.hidden = list.length < 2;
  put(mon.which, list.length > 1 ? `${unitShort(S.sel)} · ${unitModel(S.sel)}` : unitModel(S.sel));
}

/*
 * Notes, inline: a driver that reports no preamps for a unit (input settings will be refused), then its refused
 * writes grouped by reason, each with Dismiss. ctlBar shows every unit's; [Monitor] the Cues page its unit's.
 */
function renderNotes() {
  for (const bar of bars) renderBarNotes(bar);
}

function unitNotes(idx) {
  const c = idx === 0 ? ctl(0) : S.ctl[idx], i = unitInfo(idx), lines = [];
  if (!c) return lines;
  if (i && i.version >= 6 && i.numPreamps === 0 && capsOf(unitType(idx)).preamps > 0 && !unitReadOnly(idx)) {
    lines.push({ idx, kind: 'info', text: `The installed apollo.sys reports no preamps for the ${unitModel(idx)}, so it refuses ` +
      `input settings. A driver build with ${unitShort(idx)} preamp control takes them.` });
  }
  const byReason = new Map();
  for (const [key, r] of c.refused) {
    if (!byReason.has(r.reason)) byReason.set(r.reason, []);
    byReason.get(r.reason).push([key, r.what]);
  }
  for (const [reason, items] of byReason) {
    const whats = [...new Set(items.map(x => x[1]))];
    lines.push({ idx, kind: 'bad', keys: items.map(x => x[0]),
      text: `${joinList(whats)} ${whats.length > 1 ? 'were' : 'was'} not changed: ${reason}.` });
  }
  return lines;
}

function renderBarNotes(bar) {
  if (!bar.notes) return;
  const idxs = bar.all ? controlUnits().map(u => u.index) : [S.sel];
  const lines = idxs.flatMap(unitNotes);
  const key = `${idxs.join(',')}|${JSON.stringify(lines.map(l => [l.idx, l.kind, l.text]))}`;
  if (key !== bar.notesKey) {
    bar.notesKey = key;
    bar.notes.replaceChildren(...lines.map(l => el('div', { class: 'ctl-note', dataset: { kind: l.kind } },
      el('span', { text: l.text }),
      l.keys ? el('button', { class: 'btn btn-quiet ctl-dismiss', onclick: () => dismissRefusals(l.idx, l.keys) }, 'Dismiss') : null)));
  }
  bar.notes.hidden = !lines.length;
  bar.root.hidden = bar.row.hidden && bar.notes.hidden;
}

function dismissRefusals(idx, keys) {
  const c = S.ctl[idx];
  if (c) for (const k of keys) c.refused.delete(k);
  renderAllInputs();
  renderMonitor();
  renderCues();
  renderNotes();
}

/* ---------- segments ---------- */

const pendingOn = (idx, key) => !!S.ctl[idx] && S.ctl[idx].pending.has(key);
const refusedOn = (idx, key) => !!S.ctl[idx] && S.ctl[idx].refused.has(key);
/* "Input 3" on the x8p, "Twin X input 1" on another unit: the start of every control's accessible name */
const stripWho = (idx, ch) => (idx ? `${unitShort(idx)} input ${ch + 1}` : `Input ${ch + 1}`);
const tbWho = idx => (idx ? `${unitShort(idx)} talkback` : 'Talkback');
/* The record slots a unit's monitor-mix loopback comes back on: its live endpoint list's ApolloMonIn row, else 26-27. */
function monSlots(idx) {
  const L = S.units[idx] && S.units[idx].live, rows = L && L.endpoints && Array.isArray(L.endpoints.rows) ? L.endpoints.rows : [];
  const r = rows.find(x => x && x.circuit === 'ApolloMonIn' && Array.isArray(x.slots) && x.slots.length >= 2);
  return r ? [Number(r.slots[0]), Number(r.slots[1])] : MIX_OUT_SLOTS;
}

/* The segments follow the present units: new ones are built, a slot whose model changed is rebuilt, one that left
   greys out in place (or goes, if it never connected). Order: slot order. */
function syncSegments() {
  const root = document.getElementById('inputs');
  if (!root) return;
  const list = controlUnits(), here = new Set(list.map(u => u.index));
  for (const [idx, sg] of [...segs]) {
    if (here.has(idx) && unitType(idx) !== sg.type) { dropSeg(idx); continue; }
    const gone = !here.has(idx);
    if (gone && !sg.seen) { dropSeg(idx); continue; }
    if (gone !== sg.gone) { sg.gone = gone; sg.goneAt = gone ? new Date() : null; }
  }
  for (const u of list) if (!segs.has(u.index)) buildSeg(u.index);
  const nodes = [...segs.keys()].sort((a, b) => a - b).map(i => segs.get(i).node);
  if (nodes.length !== root.children.length || nodes.some((n, i) => root.children[i] !== n)) root.replaceChildren(...nodes);
  root.dataset.units = String(nodes.length);
  for (const sg of segs.values()) {
    sg.node.classList.toggle('gone', sg.gone);
    sg.body.inert = sg.gone;
  }
  renderAllInputs();
  renderSegHeads();
  renderMasters();
  renderMix();
  Plug.renderSlots();
  renderClockState();
}

function dropSeg(idx) {
  const sg = segs.get(idx);
  if (!sg) return;
  for (const m of sg.meters) { const n = METERS.indexOf(m); if (n >= 0) METERS.splice(n, 1); }
  sg.node.remove();
  segs.delete(idx);
  Aux.forget(idx);
}

/* Hide a disconnected unit's greyed segment (it comes back when the unit does). */
function hideSeg(idx) {
  const sg = segs.get(idx);
  if (!sg || !sg.gone) return;
  dropSeg(idx);
  const root = document.getElementById('inputs');
  if (root) root.dataset.units = String(segs.size);
}

function buildSeg(idx) {
  const type = unitType(idx), k = capsOf(type);
  Aux.forget(idx);
  const sg = { idx, type, k, strips: [], pairs: [], tb: null, gone: false, seen: false, meters: [] };
  sg.name = el('button', { class: 'useg-name', 'aria-pressed': 'false',
    title: 'Focus this unit: the monitor column, presets, M / D / O and the tray follow it', onclick: () => selectUnit(idx) }, k.short || 'unit');
  sg.model = el('span', { class: 'useg-model' });
  sg.clockTxt = el('span', { class: 'num' });
  sg.clock = el('span', { class: 'useg-clock', title: 'The unit\'s audio clock' }, el('span', { class: 'lamp', 'aria-hidden': 'true' }), sg.clockTxt);
  sg.rate = el('span', { class: 'useg-rate' });
  sg.goneTxt = el('span', { class: 'num' });
  sg.gonebar = el('div', { class: 'useg-gone', hidden: true }, sg.goneTxt,
    el('button', { class: 'btn btn-quiet', 'aria-label': `Hide the disconnected ${k.short}`, onclick: () => hideSeg(idx) }, 'Hide'));
  sg.head = el('div', { class: 'useg-head' },
    el('div', { class: 'useg-row' }, sg.name, sg.model),
    el('div', { class: 'useg-row useg-stat' }, sg.clock, sg.rate),
    Plug.dspNode(idx), Sig.headTools(idx), sg.gonebar);
  sg.body = el('div', { class: 'useg-body' });
  for (let p = 0; p < k.preamps >> 1; p++) {
    const a = p * 2 + 1, b = a + 1;
    const link = (k.linkPairs >>> p) & 1
      ? sw('LINK', { cls: 'link-btn', title: `Link inputs ${a} and ${b}: one set of controls for both`, onClick: () => setLink(p, !ctl(idx).links[p], idx) })
      : null;
    const head = el('div', { class: 'pair-head' }, el('span', { class: 'pair-name num', text: `${a} - ${b}` }), link);
    const body = el('div', { class: 'pair-body' });
    const node = el('div', { class: 'pair' }, head, body);
    sg.pairs.push({ node, link });
    for (const ch of [p * 2, p * 2 + 1]) {
      const s = buildStrip(ch, k, idx, sg);
      sg.strips.push(s);
      body.append(s.node);
    }
    sg.body.append(node);
  }
  if (!k.preamps) sg.body.append(el('p', { class: 'fine', text: 'Inputs of this model are not shown yet.' }));
  if (k.preamps && k.talkback) { sg.tb = buildTbStrip(idx, sg); sg.body.append(sg.tb.node); }
  sg.aux = k.preamps ? Aux.section(idx) : null;       /* [Ins] AUX 1 / AUX 2: inserts, cue sends, PRE / POST, return */
  if (sg.aux) sg.body.append(sg.aux);
  sg.master = buildMaster(idx, k, sg);
  sg.body.append(sg.master.node);
  sg.node = el('section', { class: 'useg', dataset: { unit: String(idx), model: k.short }, 'aria-label': `${k.short || 'Unit'} inputs` }, sg.head, sg.body);
  sizeSeg(sg);
  segs.set(idx, sg);
  return sg;
}

/* [Ins] a segment's minimum width (the strip area scrolls sideways below it): the AUX strips count while shown */
function sizeSeg(sg) {
  const k = sg.k, open = !!sg.aux && cfg.auxStrips.open;
  if (sg.aux) sg.aux.dataset.open = open ? '1' : '0';
  const minW = k.preamps * STRIP_MIN + (sg.tb ? TB_W : 0) + (open ? 2 * Aux.W : 0) + MASTER_W + 8;
  sg.node.style.flex = `${Math.max(1, k.preamps)} 1 ${minW}px`;
  sg.node.style.minWidth = `${minW}px`;
}

/* [Ins] the master's AUX button: the AUX 1 / AUX 2 strips shown or not, every unit alike (the console layouts) */
function setAuxOpen(on) {
  cfg.auxStrips.open = !!on;
  for (const sg of segs.values()) {
    sizeSeg(sg);
    const b = sg.master && sg.master.node.querySelector('.mo-aux');
    if (b) b.setAttribute('aria-expanded', on ? 'true' : 'false');
  }
  saveConfig();
}

/* A segment's head: focus, model and serial, clock, rate, live or gone. */
function renderSegHead(idx) {
  const sg = segs.get(idx);
  if (!sg) return;
  const i = unitInfo(idx), e = listedUnit(idx);
  if (!sg.gone && (idx === 0 ? S.host.present || S.demo : !!S.units[idx])) sg.seen = true;
  setPressed(sg.name, idx === S.sel);
  sg.node.classList.toggle('focused', idx === S.sel && segs.size > 1);
  sg.head.dataset.player = `PLAYER ${[...segs.keys()].sort((a, b) => a - b).indexOf(idx) + 1}`;
  const serial = (i && i.serial) || (e && e.serial) || '';
  put(sg.model, `${unitModel(idx)}${serial ? ` · ${serial.slice(-4)}` : ''}`);
  const up = !sg.gone && !!i && i.stage === STAGE.CONNECTED, run = up && (i.flags & FLAG.TRANSPORT) !== 0;
  sg.clock.dataset.state = !up ? 'off' : run ? 'ok' : 'warn';
  put(sg.clockTxt, !up ? 'OFFLINE' : `${kHz(i.sampleRate || 48000)} · CLK ${run ? 'RUN' : 'STOP'}`);
  if (!sg.gone) renderRateIn(sg.rate, idx, 'seg');
  sg.gonebar.hidden = !sg.gone;
  if (sg.gone) put(sg.goneTxt, `DISCONNECTED${sg.goneAt ? ` ${sg.goneAt.toTimeString().slice(0, 5)}` : ''}`);
  sg.node.dataset.live = !sg.gone && !unitReadOnly(idx) && (S.demo || connected(idx)) ? '1' : '0';
}

function renderSegHeads() { for (const idx of segs.keys()) renderSegHead(idx); }

/* ---------- strips ---------- */

function buildStrip(ch, k, idx, sg) {
  const who = stripWho(idx, ch);
  const num = el('div', { class: 'strip-num num', text: String(ch + 1).padStart(2, '0') });
  const hiz = ch < k.hiz ? el('span', { class: 'tag', title: 'Front-panel instrument jack', text: 'HI-Z' }) : null;
  const name = el('div', { class: 'strip-name', title: 'Double-click to rename; right-click to copy or paste the channel' });
  name.addEventListener('dblclick', () => editName(ch, name, idx));
  name.addEventListener('contextmenu', e => ChCopy.menu(e, idx, ch));            /* [Copy] copy / paste the channel */
  num.addEventListener('contextmenu', e => ChCopy.menu(e, idx, ch));
  const gain = new Knob({
    size: 48, min: 10, max: 65, step: 1, wheelStep: 1, pageStep: 6, value: 10,
    aria: `${who} gain`,
    format: v => `${v} dB`, editText: v => String(v),
    parse: s => { const v = parseInt(s, 10); return Number.isFinite(v) ? clamp(v, 10, 65) : null; },
    lockedText: 'Gain knobs are locked. Unlock them in Settings, Safety.',
    onChange: v => setInput(ch, 'gain', v, { unit: idx }),
  });
  const ml = seg(['MIC', 'LINE'], i => setInput(ch, 'line', i === 1, { unit: idx }), `${who} mic or line`);
  const p48 = sw('48V', { red: true, title: 'Phantom power', onClick: () => request48v(ch, !ctl(idx).inputs[ch].p48, idx) });
  const pad = sw('PAD', { title: 'Input pad', onClick: () => setInput(ch, 'pad', !ctl(idx).inputs[ch].pad, { unit: idx }) });
  const lc = sw('LO CUT', { title: 'Low-cut filter', onClick: () => setInput(ch, 'lowcut', !ctl(idx).inputs[ch].lowcut, { unit: idx }) });
  const ph = sw('Ø', { title: 'Polarity invert', aria: `${who} polarity invert`, onClick: () => setInput(ch, 'phase', !ctl(idx).inputs[ch].phase, { unit: idx }) });
  /* Hi-Z impedance (Twin X input 1). Every strip of such a model gets the row, so the rows line up. */
  const load = (k.hizLoad >>> ch) & 1 ? el('select', {
    class: 'select hiz-load', 'aria-label': `${who} Hi-Z impedance`,
    title: 'Input impedance of the front instrument (Hi-Z) jack. The unit does not report it: this is the last value set from this app.',
    onchange: e => setHizLoad(ch, Number(e.target.value), idx),
  }, el('option', { value: '-1', text: 'not set', disabled: true }), HIZ_LOADS.map((t, i) => el('option', { value: String(i), text: t }))) : null;
  const extra = k.hizLoad ? el('div', { class: 'strip-extra' }, load ? [el('span', { class: 'lbl', text: 'Hi-Z' }), load] : null) : null;
  /* [Plug-ins] the insert slot(s): apollo.sys hosts plug-ins on the x8p's DSP 1; other units say so */
  const insert = Plug.slotNodesFor(idx, ch);
  /* [Mix] the monitor-mix channel: pan, M, S, the fader and, beside it, the input meter (its record slot) */
  const mix = mixChannel(idx, ch, who);
  const meter = new Meter({ slot: ch, group: 'control', unit: idx, scale: false });
  sg.meters.push(meter);
  const peak = el('div', { class: 'strip-peak num', text: '--', title: 'Input peak, dBFS (hold). Click the meter to clear.' });
  /* [Themes] the measurement numbers (the panels composition shows them first) and the focus view's More */
  const stats = el('div', { class: 'st-stats num', 'aria-hidden': 'true' });
  const more = el('button', { class: 'btn btn-quiet st-more', type: 'button', 'aria-expanded': 'false', title: 'Pan, mute, solo and the plug-in insert of this input',
    onclick: e => { e.stopPropagation(); const on = node.classList.toggle('more'); more.setAttribute('aria-expanded', on ? 'true' : 'false'); more.textContent = on ? 'Less' : 'More'; } }, 'More');
  const node = el('div', { class: 'strip', dataset: { ch: String(ch), unit: String(idx) } },
    el('div', { class: 'strip-top' }, num, hiz),
    name,
    el('div', { class: 'st-sec st-pre' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'PRE' }), gain.node, ml.node,
      el('div', { class: 'sw-grid' }, p48, pad, lc, ph), extra),
    el('div', { class: 'st-sec st-ins' }, el('div', { class: 'st-ins-head' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'INS' }), Plug.recmonNode(idx, ch)), insert),
    el('div', { class: 'st-sec st-aux' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'AUX' }), Aux.stripSends(idx, ch, who)),
    el('div', { class: 'st-sec st-mix' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'MON MIX' }), mix.top,
      el('div', { class: 'strip-meter' },
        el('div', { class: 'fd-row' }, mix.fader.node, mix.linked, meter.node),
        el('div', { class: 'fd-foot' }, mix.fader.readout, peak))),
    el('div', { class: 'st-out num', 'aria-hidden': 'true', text: `→ REC ${String(ch + 1).padStart(2, '0')} · MIX` }), more,
    el('button', { class: 'btn st-guide', type: 'button', title: 'Guided gain: speak or play for 5 seconds, then a gain for good headroom',
      onclick: e => { e.stopPropagation(); Sig.guide(idx, ch); } }, 'Set my level'));
  node.insertBefore(stats, node.children[2]);
  /* the focus composition (Day): a press on the strip, not on a control, opens it; again closes it */
  node.addEventListener('click', e => {
    const comp = document.getElementById('view-control').dataset.comp;
    /* the panels composition (Lab): a press on a panel's title bar folds it to its numbers */
    if (comp === 'panels' && e.target.closest('.strip-top')) { node.classList.toggle('collapsed'); return; }
    if (comp !== 'focus' || e.target.closest('button, .knob, .fader, select, input, .meter-wrap')) return;
    focusStrip(idx, ch);
  });
  return { ch, node, name, hiz, gain, ml, p48, pad, lc, ph, load, meter, peak, lastPeak: '', mix, stats, lastStats: '' };
}

/* One monitor-mix channel's controls: the fader, pan (not the talkback), M and S. */
function mixChannel(idx, bus, who) {
  const tb = bus === TALKBACK_BUS;
  const fader = new Fader({ aria: `${who} mix level`, onChange: v => setMix(bus, 'level', v, idx) });
  const pan = tb ? null : new Knob({
    size: 22, min: -100, max: 100, step: 1, wheelStep: 5, pageStep: 25, value: 0, origin: 0.5,
    aria: `${who} mix pan`, format: fmtPan, editText: fmtPan, parse: parsePan,
    onChange: v => setMix(bus, 'pan', v, idx),
  });
  const mute = mixSwitch('M', `${who} mute in the monitor mix`, true, () => setMix(bus, 'mute', !mixOfBus(idx, bus).mute, idx));
  const solo = mixSwitch('S', `${who} solo in the monitor mix`, false, () => setMix(bus, 'solo', !mixOfBus(idx, bus).solo, idx));
  const linked = el('div', { class: 'fd-linked', hidden: true, title: 'Linked: the pair\'s first input drives the level, M and S of both; this pan places this input' },
    el('span', { text: 'LINKED' }));
  const top = el('div', { class: 'mix-top' }, pan ? pan.node : el('span', { class: 'mix-nopan' }), el('div', { class: 'mix-ms' }, mute, solo));
  return { bus, fader, pan, mute, solo, linked, top };
}

/* The talkback mic's monitor-mix channel (no record slot of its own here, so no meter). */
function buildTbStrip(idx, sg) {
  const mix = mixChannel(idx, TALKBACK_BUS, tbWho(idx));
  const node = el('div', { class: 'tb-strip', dataset: { bus: String(TALKBACK_BUS), unit: String(idx) } },
    el('div', { class: 'strip-top' }, el('div', { class: 'strip-num num', text: 'TB' })),
    el('div', { class: 'strip-name tb-name', text: 'Talk', title: 'The talkback mic in the monitor mix' }),
    el('div', { class: 'st-sec st-mix' }, el('span', { class: 'st-lbl', 'aria-hidden': 'true', text: 'MIX' }), mix.top,
      el('div', { class: 'strip-meter' }, el('div', { class: 'fd-row' }, mix.fader.node, mix.linked), el('div', { class: 'fd-foot' }, mix.fader.readout))),
    el('div', { class: 'st-out num', 'aria-hidden': 'true', text: '→ MIX' }));
  return { node, mix };
}

/* A segment's master section: monitor level, MUTE, DIM, the main out L/R meters (the monitor mix as heard, from the
   unit's loopback slots), MUTE ALL, and MON (focus: the monitor column shows this unit). */
function buildMaster(idx, k, sg) {
  const short = unitShort(idx), who = monLabel(idx);
  const level = new Knob({
    size: 50, min: 0, max: LEVEL_MAX, step: 1, wheelStep: 2, pageStep: 12, value: 152,
    aria: `${who} level`, toPos: monToPos, fromPos: monFromPos,
    format: fmtLevel, editText: v => rawToDb(v).toFixed(1), parse: parseDb,
    onChange: v => setMon('level', v, idx),
  });
  const mute = sw('MUTE', { red: true, aria: idx ? `${short} monitor mute` : null, title: 'Mute the monitor outputs (M: the focused unit)',
    onClick: () => setMon('mute', !ctl(idx).mon.mute, idx) });
  const dim = sw('DIM', { aria: idx ? `${short} monitor dim` : null, title: 'Dim the monitor outputs (D: the focused unit)',
    onClick: () => setMon('dim', !ctl(idx).mon.dim, idx) });
  const all = mixSwitch('MUTE ALL', idx ? `Mute every ${short} input and the talkback in the monitor mix` : 'Mute every input and the talkback in the monitor mix', true,
    () => setMixMuteAll(all.getAttribute('aria-checked') !== 'true', idx));
  /* [Mix] Console's SoloClear: every solo of this unit's monitor mix off at once (shown while one is on) */
  const clrSolo = el('button', { class: 'btn btn-quiet mo-clrsolo', type: 'button', hidden: true,
    title: idx ? `Clear every ${short} solo in the monitor mix` : 'Clear every solo in the monitor mix',
    onclick: () => { for (const b of mixBuses(capsOf(unitType(idx)))) if (mixOfBus(idx, b).solo) setMix(b, 'solo', false, idx); } }, 'CLR SOLO');
  const auxBtn = el('button', { class: 'btn btn-quiet mo-aux', type: 'button', 'aria-expanded': cfg.auxStrips.open ? 'true' : 'false',
    title: 'Show or hide the AUX 1 / AUX 2 strips (returns, cue sends, inserts)', onclick: () => setAuxOpen(!cfg.auxStrips.open) }, 'AUX');
  const focus = el('button', { class: 'btn btn-quiet mo-focus', 'aria-pressed': 'false', 'aria-label': `Show the ${short} monitor settings`,
    title: 'Source, speakers, headphones, ALT and talkback of this unit: the monitor column', onclick: () => selectUnit(idx) }, 'MON ›');
  const [sl, sr] = monSlots(idx);
  const mL = new Meter({ slot: sl, group: 'control', unit: idx, scale: false });
  const mR = new Meter({ slot: sr, group: 'control', unit: idx, scale: true });
  sg.meters.push(mL, mR);
  const pL = el('span', { class: 'mo-peak num', text: '--' }), pR = el('span', { class: 'mo-peak num', text: '--' });
  const node = el('div', { class: 'master', role: 'group', 'aria-label': `${short} master` },
    el('div', { class: 'mo-title', 'aria-hidden': 'true' }, el('span', { text: 'MAIN' }), el('span', { class: 'mo-lr', text: 'L R' })),
    el('div', { class: 'mo-body' },
      el('div', { class: 'mo-ctl' }, level.node, mute, dim, el('div', { class: 'mo-gap' }), all, clrSolo, auxBtn, focus),
      el('div', { class: 'mo-meters', title: 'Main out L/R: the monitor mix as the unit plays it (its loopback, before the monitor level), dBFS' },
        el('div', { class: 'mo-bars' }, mL.node, mR.node),
        el('div', { class: 'mo-peaks' }, el('span', { class: 'mo-pk' }, el('span', { class: 'mo-ch', text: 'L' }), pL),
          el('span', { class: 'mo-pk' }, el('span', { class: 'mo-ch', text: 'R' }), pR)))));
  return { node, level, mute, dim, all, clrSolo, focus, mL, mR, pL, pR, lastL: '', lastR: '' };
}

/* [Themes] the focus view (Day): one input open with everything about it, the others an overview */
function focusStrip(idx, ch) {
  for (const sg of segs.values()) for (const s of sg.strips) {
    const on = sg.idx === idx && s.ch === ch && !s.node.classList.contains('focus');
    s.node.classList.toggle('focus', on);
  }
  const vc = document.getElementById('view-control');
  vc.classList.toggle('has-focus', !!document.querySelector('#inputs .strip.focus'));
}

function editName(ch, nameEl, idx = S.sel) {
  const c = chanCfg(idx, ch);
  if (c === NO_NAME) { toast('The host has not named this unit yet, so its input names cannot be kept.', 'warn'); return; }
  const input = el('input', { class: 'strip-name-edit', type: 'text', maxlength: '24', value: c.name, 'aria-label': `${stripWho(idx, ch)} name` });
  nameEl.replaceWith(input);
  input.focus();
  input.select();
  let closed = false;
  const close = apply => {
    if (closed) return;
    closed = true;
    if (apply) { c.name = input.value.trim().slice(0, 24); saveConfig(); }
    input.replaceWith(nameEl);
    renderInput(ch, idx);
    if (idx === 0) { renderSettings(); buildBridge(); }
  };
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter') close(true);
    else if (e.key === 'Escape') close(false);
    e.stopPropagation();
  });
  input.addEventListener('blur', () => close(true));
}

function renderInput(ch, idx = S.sel) {
  const sg = segs.get(idx);
  if (!sg || sg.gone) return;
  const s = sg.strips[ch];
  if (!s) return;
  const c = ctl(idx), inp = c.inputs[ch], cc = chanCfg(idx, ch);
  const pend = key => c.pending.has(key), bad = key => c.refused.has(key);
  s.name.textContent = cc.name || 'Name';
  s.name.classList.toggle('empty', !cc.name);
  if (cc.color && STRIP_COLORS[cc.color]) {
    s.node.dataset.color = cc.color;
    s.node.style.setProperty('--strip-color', STRIP_COLORS[cc.color]);
  } else {
    delete s.node.dataset.color;
  }
  s.node.hidden = !!cc.hidden;
  if (s.hiz) s.hiz.classList.toggle('on', inp.hiz);
  s.gain.set(inp.gain);
  s.node.querySelector('.strip-top').dataset.lv = String(Math.round((inp.gain - 10) / 55 * 99)).padStart(2, '0');
  s.gain.setLocked(cfg.safety.lockGain);
  s.gain.setUnconfirmed(pend(`in:${ch}:gain`));
  s.gain.node.classList.toggle('refused', bad(`in:${ch}:gain`));
  s.ml.set(inp.line ? 1 : 0);
  s.ml.node.classList.toggle('refused', bad(`in:${ch}:line`));
  for (const [node, field] of [[s.p48, 'p48'], [s.pad, 'pad'], [s.lc, 'lowcut'], [s.ph, 'phase']]) {
    setPressed(node, inp[field]);
    node.classList.toggle('unconfirmed', pend(`in:${ch}:${field}`));
    node.classList.toggle('refused', bad(`in:${ch}:${field}`));
  }
  if (s.load) {
    s.load.value = String(hizLoadOf(idx));
    s.load.classList.toggle('refused', bad(`hiz:${ch}`));
  }
}

function renderPair(p, idx = S.sel) {
  const sg = segs.get(idx);
  if (!sg || sg.gone) return;
  const pr = sg.pairs[p];
  if (!pr) return;
  const c = ctl(idx);
  if (pr.link) {
    setPressed(pr.link, c.links[p]);
    pr.link.classList.toggle('unconfirmed', c.pending.has(`link:${p}`));
    pr.link.classList.toggle('refused', c.refused.has(`link:${p}`));
  }
  pr.node.classList.toggle('linked', !!c.links[p]);
  const a = chanCfg(idx, p * 2).hidden, b = chanCfg(idx, p * 2 + 1).hidden;
  pr.node.hidden = a && b;
  pr.node.classList.toggle('single', a !== b);
}

function renderUnitInputs(idx) {
  const sg = segs.get(idx);
  if (!sg || sg.gone) return;
  for (let ch = 0; ch < sg.k.preamps; ch++) renderInput(ch, idx);
  for (let p = 0; p < sg.pairs.length; p++) renderPair(p, idx);
}

function renderAllInputs() {
  for (const idx of segs.keys()) renderUnitInputs(idx);
}

/* ---------- [Mix] the monitor mix on the strips ---------- */

/*
 * A unit's monitor mix, what its monitor outputs play: on every strip of its segment a fader (-inf..+12 dB), a pan,
 * M and S, and the talkback strip; MUTE ALL in the master section for feedback safety. The driver sends nothing for
 * a bus until it is set here, so an untouched channel shows UA's default (on at 0 dB, centre). A linked pair is one
 * stereo channel: its first input's fader, M and S drive both; each input keeps its pan (hard left and right when
 * linked here). M, S and MUTE ALL are switches (role "switch"), so they are told apart from the monitor's MUTE and
 * the TALKBACK button. The bus coefficients do not read back; the main out meters are the unit's monitor-mix
 * loopback (record slots 26-27), the level those speakers get before the monitor knob.
 */
function mixSwitch(text, aria, red, onClick) {
  const b = el('button', { class: `sw mix-sw${red ? ' sw-red' : ''}`, role: 'switch', 'aria-checked': 'false', 'aria-label': aria,
    title: aria, onclick: onClick }, el('span', { class: 'sw-lab', text }));
  Skin.decorate('button', b);
  return b;
}
const setChecked = (node, on) => node.setAttribute('aria-checked', on ? 'true' : 'false');

function renderMixFor(idx) {
  const sg = segs.get(idx);
  if (!sg || sg.gone) return;
  Aux.render(idx);                     /* [Ins] the aux sends and strips */
  const k = sg.k, buses = mixBuses(k);
  const vals = new Map(buses.map(b => [b, mixOfBus(idx, b)]));
  const anySolo = buses.some(b => vals.get(b).solo);
  const chans = sg.strips.map(s => ({ ...s.mix, box: s.node })).concat(sg.tb ? [{ ...sg.tb.mix, box: sg.tb.node }] : []);
  for (const r of chans) {
    const v = vals.get(r.bus) || mixOfBus(idx, r.bus), pair = mixPair(idx, r.bus);
    const follower = !!pair && pair[1] === r.bus;
    r.fader.set(v.level);
    if (r.pan) r.pan.set(v.pan);
    setChecked(r.mute, v.mute);
    setChecked(r.solo, v.solo);
    r.fader.node.classList.toggle('refused', refusedOn(idx, `mix:mix:${r.bus}`));
    if (r.pan) r.pan.node.classList.toggle('refused', refusedOn(idx, `mix:mixpan:${r.bus}`));
    r.mute.classList.toggle('refused', refusedOn(idx, `mix:mixmute:${r.bus}`));
    r.solo.classList.toggle('refused', refusedOn(idx, `mix:mixsolo:${r.bus}`));
    r.box.classList.toggle('silent', v.mute || (anySolo && !v.solo));
    r.box.classList.toggle('follower', follower);
    r.fader.node.hidden = follower;
    r.fader.readout.hidden = follower;
    r.mute.hidden = r.solo.hidden = follower;
    r.linked.hidden = !follower;
  }
  if (sg.master) {
    setChecked(sg.master.all, buses.length > 0 && buses.every(b => vals.get(b).mute));
    if (sg.master.clrSolo) sg.master.clrSolo.hidden = !anySolo;      /* [Mix] SoloClear */
    sg.master.all.classList.toggle('refused', refusedOn(idx, 'mix:mixmuteall'));
  }
}

function renderMix() { for (const idx of segs.keys()) renderMixFor(idx); }

/* ---------- master sections ---------- */

function renderMaster(idx) {
  const sg = segs.get(idx);
  if (!sg || sg.gone || !sg.master) return;
  const m = sg.master, c = ctl(idx);
  m.level.setMax(LEVEL_MAX + 2 * cfg.safety.monitorLimitDb);
  m.level.set(c.mon.level);
  m.level.setUnconfirmed(c.pending.has('mon:level'));
  m.level.node.classList.toggle('refused', c.refused.has('mon:level'));
  for (const [node, f] of [[m.mute, 'mute'], [m.dim, 'dim']]) {
    setPressed(node, c.mon[f]);
    node.classList.toggle('unconfirmed', c.pending.has(`mon:${f}`));
    node.classList.toggle('refused', c.refused.has(`mon:${f}`));
  }
  const [sl, sr] = monSlots(idx);
  m.mL.slot = sl;
  m.mR.slot = sr;
  setPressed(m.focus, idx === S.sel);
}

function renderMasters() { for (const idx of segs.keys()) renderMaster(idx); }

/* ------------------------------------------------------------------ */
/* [Themes] Studio: the meter bridge across the top of the console     */
/* ------------------------------------------------------------------ */

/*
 * A row of needle meters over the segments, one per input of every unit and the main out L/R of each, drawn on one canvas
 * (60 fps, nothing rebuilt per frame). Standards (cfg.bridge.std): VU (0 VU = -18 dBFS, 300 ms integration), PPM (fast
 * rise, about 8.6 dB/s fall, the scale in dBFS) and K-14 (an average, 0 = -14 dBFS). STAGES swaps the needles for the
 * gain-staging view (Studio's signature): per input its preamp gain, insert, level, monitor fader and its share of the
 * main out at a glance. Shown in the console composition only.
 */
const MB = { root: null, canvas: null, ctx: null, cells: [], std: 'vu', stages: false, w: 0, h: 0, dpr: 1, key: '' };
const MB_STD = {
  vu: { name: 'VU', lo: -20, hi: 3, ref: -18, marks: [-20, -10, -7, -5, -3, -1, 0, 1, 2, 3], red: 0, tau: 0.12, avg: false },
  ppm: { name: 'PPM', lo: -24, hi: 0, ref: 0, marks: [-24, -20, -16, -12, -8, -4, 0], red: -8, rise: 0.01, fall: 8.6, avg: false },
  k14: { name: 'K-14', lo: -20, hi: 4, ref: -14, marks: [-20, -12, -8, -4, 0, 4], red: 0, tau: 0.3, avg: true },
};

function buildMeterBridge() {
  const desk = document.getElementById('desk');
  if (!desk || MB.root) return;
  MB.std = (cfg.bridge && cfg.bridge.std) || 'vu';
  MB.canvas = el('canvas', { class: 'mb-canvas', 'aria-hidden': 'true' });
  const std = seg(['VU', 'PPM', 'K-14'], i => { MB.std = ['vu', 'ppm', 'k14'][i]; std.set(i); for (const c of MB.cells) c.v = -Infinity; if (cfg.bridge) { cfg.bridge.std = MB.std; saveConfig(); } }, 'Meter bridge standard');
  std.set(['vu', 'ppm', 'k14'].indexOf(MB.std));
  MB.stagesBtn = sw('STAGES', { title: 'Gain staging: every stage of every input at a glance (preamp, insert, level, monitor fader, main out)',
    onClick: () => { MB.stages = !MB.stages; setPressed(MB.stagesBtn, MB.stages); MB.key = ''; } });
  MB.root = el('div', { class: 'meter-bridge', id: 'meterBridge', role: 'img', 'aria-label': 'Meter bridge: every input and the main out' },
    el('div', { class: 'mb-tools' }, el('span', { class: 'lbl', text: 'Meter bridge' }), std.node, MB.stagesBtn), MB.canvas);
  desk.insertBefore(MB.root, document.getElementById('inputs'));
  new ResizeObserver(() => { MB.key = ''; }).observe(MB.canvas);
}

/* the cells: each live unit's inputs, then its main out L and R */
function mbCells() {
  const out = [];
  for (const idx of [...segs.keys()].sort((a, b) => a - b)) {
    const sg = segs.get(idx);
    if (sg.gone) continue;
    for (const s of sg.strips) if (!s.node.hidden) out.push({ idx, slot: s.ch, ch: s.ch, label: `${sg.k.short === 'x8p' ? '' : 'TX '}${s.ch + 1}`, main: false });
    if (sg.master) { const [l, r] = monSlots(idx); out.push({ idx, slot: l, label: `${sg.k.short === 'x8p' ? 'L' : 'TX L'}`, main: true }, { idx, slot: r, label: `${sg.k.short === 'x8p' ? 'R' : 'TX R'}`, main: true }); }
  }
  return out;
}

function drawMeterBridge(dt) {
  if (!MB.root) return;
  const show = document.getElementById('view-control').dataset.comp === 'console' && S.view === 'control';
  MB.root.hidden = !show;
  if (!show) return;
  const list = mbCells(), key = list.map(c => `${c.idx}:${c.slot}`).join(',');
  const cw = MB.canvas.clientWidth, chh = MB.canvas.clientHeight, dpr = window.devicePixelRatio || 1;
  if (key !== MB.key || cw !== MB.w || chh !== MB.h) {
    MB.key = key; MB.w = cw; MB.h = chh; MB.dpr = dpr;
    MB.canvas.width = Math.max(1, Math.round(cw * dpr));
    MB.canvas.height = Math.max(1, Math.round(chh * dpr));
    MB.ctx = MB.canvas.getContext('2d');
    const old = new Map(MB.cells.map(c => [`${c.idx}:${c.slot}`, c.v]));
    MB.cells = list.map(c => ({ ...c, v: old.get(`${c.idx}:${c.slot}`) ?? -Infinity }));
  }
  const g = MB.ctx, n = MB.cells.length;
  if (!g || !n) return;
  const cs = getComputedStyle(document.documentElement);
  const col = { face: '#e9e2cf', ink: '#2a2620', red: '#c0392b', needle: '#1b1a17', label: cs.getPropertyValue('--text-dim').trim() || '#aaa' };
  g.setTransform(MB.dpr, 0, 0, MB.dpr, 0, 0);
  g.clearRect(0, 0, MB.w, MB.h);
  const gap = 4, w = (MB.w - gap * (n - 1)) / n, h = MB.h, st = MB_STD[MB.std];
  MB.cells.forEach((c, i) => {
    const x = i * (w + gap), u = S.ctl[c.idx];
    /* ballistics on the peak feed: VU and K-14 integrate, PPM rises fast and falls slowly */
    const peak = u && u.levelsValid ? u.peaks[c.slot] : -Infinity;
    const lin = peak > -200 ? Math.pow(10, peak / 20) : 0, cur = c.v > -200 ? Math.pow(10, c.v / 20) : 0;
    if (MB.std === 'ppm') {
      c.v = peak >= c.v ? peak : Math.max(peak, (c.v > -200 ? c.v : -120) - st.fall * dt);
    } else {
      const a = 1 - Math.exp(-dt / st.tau), nl = cur + (lin * (st.avg ? 0.707 : 1) - cur) * a;
      c.v = nl > 0 ? 20 * Math.log10(nl) : -Infinity;
    }
    if (MB.stages) { drawStage(g, c, x, w, h, col); return; }
    const rel = c.v - st.ref, frac0 = v => clamp((v - st.lo) / (st.hi - st.lo), 0, 1);
    /* the face */
    g.fillStyle = c.main ? '#ddd3bb' : col.face;
    g.fillRect(x, 0, w, h - 12);
    const cx = x + w / 2, cy = (h - 12) * 1.18, r = Math.min(w * 0.62, (h - 12) * 1.02), a0 = -0.85, a1 = 0.85;
    const ang = f => -Math.PI / 2 + a0 + f * (a1 - a0);
    g.lineWidth = 1;
    g.strokeStyle = col.ink;
    g.beginPath(); g.arc(cx, cy, r * 0.86, ang(0), ang(1)); g.stroke();
    g.strokeStyle = col.red; g.lineWidth = 3;
    g.beginPath(); g.arc(cx, cy, r * 0.86, ang(frac0(st.red)), ang(1)); g.stroke();
    g.lineWidth = 1; g.strokeStyle = col.ink;
    for (const m of st.marks) {
      const t = ang(frac0(m)), r0 = r * 0.86, r1 = r * (Math.abs(m) % 10 === 0 || m === 0 ? 0.74 : 0.79);
      g.beginPath(); g.moveTo(cx + r0 * Math.cos(t), cy + r0 * Math.sin(t)); g.lineTo(cx + r1 * Math.cos(t), cy + r1 * Math.sin(t)); g.stroke();
    }
    /* the needle */
    const t = ang(frac0(Number.isFinite(rel) ? rel : st.lo - 5));
    g.strokeStyle = rel >= st.red ? col.red : col.needle; g.lineWidth = 1.4;
    g.beginPath(); g.moveTo(cx, cy); g.lineTo(cx + r * 0.95 * Math.cos(t), cy + r * 0.95 * Math.sin(t)); g.stroke();
    g.fillStyle = col.ink; g.font = '8px Segoe UI'; g.textAlign = 'center';
    g.fillText(st.name, cx, (h - 12) * 0.82);
    g.fillStyle = col.label; g.font = '9px Cascadia Mono, Consolas, monospace';
    g.fillText(c.label, cx, h - 2);
  });
}

/* Studio's signature: gain staging, every stage of one input in a column of bars */
function drawStage(g, c, x, w, h, col) {
  const idx = c.idx, cc = S.ctl[idx];
  g.fillStyle = '#16171a'; g.fillRect(x, 0, w, h - 12);
  const rows = [];
  if (!c.main) {
    const inp = cc ? cc.inputs[c.ch] : null, mix = mixOfBus(idx, c.ch), ins = typeof Plug !== 'undefined' ? Plug.insertLabel(idx, c.ch) : '';
    rows.push(['GAIN', inp ? (inp.gain - 10) / 55 : 0, inp ? `${inp.gain}` : '-', '#f5a623']);
    rows.push(['INS', ins ? 1 : 0, ins ? ins.slice(0, 6) : '--', ins ? '#4fd472' : '#555']);
    rows.push(['LEVEL', clamp(frac(c.v, -60), 0, 1), peakText(c.v), c.v > -6 ? '#ff4040' : c.v > -18 ? '#e8c547' : '#4fd472']);
    rows.push(['FADER', sendToPos(mix.level), mix.level <= SEND_OFF ? 'off' : (mix.level / 2).toFixed(1), mix.mute ? '#7a776f' : '#f4f1e8']);
  } else {
    rows.push(['MAIN', clamp(frac(c.v, -60), 0, 1), peakText(c.v), c.v > -6 ? '#ff4040' : '#4fd472']);
  }
  const rh = (h - 14) / Math.max(rows.length, 1);
  g.font = '8px Cascadia Mono, Consolas, monospace'; g.textAlign = 'left';
  rows.forEach(([lbl, f, txt, color], i) => {
    const y = 2 + i * rh;
    g.fillStyle = '#7a776f'; g.fillText(lbl, x + 2, y + 8);
    g.fillStyle = '#2a2b2e'; g.fillRect(x + 2, y + 10, w - 4, 4);
    g.fillStyle = color; g.fillRect(x + 2, y + 10, (w - 4) * clamp(f, 0, 1), 4);
    g.fillStyle = '#ece9e2'; g.textAlign = 'right'; g.fillText(txt, x + w - 2, y + 8); g.textAlign = 'left';
  });
  g.fillStyle = col.label; g.font = '9px Cascadia Mono, Consolas, monospace'; g.textAlign = 'center';
  g.fillText(c.label, x + w / 2, h - 2);
}

/* ------------------------------------------------------------------ */
/* Control view: the monitor column (the focused unit)                 */
/* ------------------------------------------------------------------ */

const mon = {};

function buildMonitor() {
  const root = document.getElementById('monitor');
  root.replaceChildren();
  mon.unitsHolder = { segKey: '', buttons: [], seg: null };
  mon.units = el('div', { class: 'seg mon-units', id: 'monUnits', role: 'group', 'aria-label': 'Unit the monitor column shows' });
  mon.unitsHolder.seg = mon.units;
  mon.which = el('div', { class: 'mon-which num' });
  mon.mono = sw('MONO', { big: true, onClick: () => setMon('mono', !S.mon.mono) });
  mon.dimLevel = el('select', { class: 'select', id: 'dimLevel', onchange: e => setMon('dimlevel', Number(e.target.value)) },
    DIM_LEVELS.map((db, i) => el('option', { value: String(i + 1), text: `${db} dB` })));

  /* The dim level row is the x8p's: the Twin X's driver plan refuses it. [Monitor] The speaker and source
     choices follow the focused model (syncMonitorModel): its ALTs that are on, MIX and its cues. */
  mon.dimField = el('div', { class: 'field' }, el('label', { for: 'dimLevel', text: 'Dim level' }), mon.dimLevel);
  mon.spkWrap = el('div', {});
  mon.spkField = el('div', { class: 'field' }, el('span', { class: 'lbl', text: 'Speakers' }), mon.spkWrap);
  mon.srcWrap = el('div', {});
  mon.srcField = el('div', { class: 'field field-stack' }, el('span', { class: 'lbl', text: 'Source' }), mon.srcWrap);
  root.append(el('div', { class: 'mon-card mon-main' },
    el('div', { class: 'mon-head' }, el('div', { class: 'sect-title', text: 'Monitor' }), mon.units),
    mon.which,
    el('span', { class: 'hp-note', text: 'Level, MUTE, DIM and the main out meters: each unit\'s master section.' }),
    mon.mono,
    mon.dimField,
    mon.spkField,
    mon.srcField));

  mon.hp1 = new Knob({
    size: 56, min: 0, max: LEVEL_MAX, step: 1, wheelStep: 2, pageStep: 12, value: S.mon.hp1,
    aria: 'Headphone 1 level', toPos: monToPos, fromPos: monFromPos,
    format: fmtLevel, editText: v => rawToDb(v).toFixed(1), parse: parseDb,
    onChange: v => setMon('hp1', v),
  });
  /* Shown per model (MODEL_CAPS): the x8p's level knobs are on the unit; [Monitor] the sources (CUE 1 /
     CUE 2, by the model's cue names) on both units, HP 2 where there is one. */
  mon.hp1Lbl = el('span', { class: 'lbl muted', text: 'HP 1 source' });
  mon.hp2Lbl = el('span', { class: 'lbl muted', text: 'HP 2 source' });
  mon.hp1Wrap = el('div', {});
  mon.hp2Wrap = el('div', {});
  mon.hpSide = el('div', { class: 'hp-side' }, mon.hp1Lbl, mon.hp1Wrap, mon.hp2Lbl, mon.hp2Wrap);
  mon.hpNote = el('span', { class: 'hp-note' });
  root.append(el('div', { class: 'mon-card mon-hp' },
    el('div', { class: 'sect-title', text: 'Headphones' }),
    el('div', { class: 'hp' }, mon.hp1.node, mon.hpSide),
    mon.hpNote));

  /* [Monitor] ALT speakers: per ALT the model has, the line pair it takes over and its trim. */
  mon.altRows = el('div', { class: 'alt-rows' });
  mon.altCard = el('div', { class: 'mon-card mon-alt' }, el('div', { class: 'sect-title', text: 'ALT speakers' }), mon.altRows,
    el('span', { class: 'hp-note', text: 'An ALT pair follows the monitor level; trim matches it to the main speakers.' }));
  root.append(mon.altCard);
  root.append(Outs.card());            /* [Out] reference levels, S/PDIF mirror, FCN, meter holds, aux mono, identify */

  let downAt = 0, wasOn = false;
  mon.talk = sw('TALKBACK', { big: true, cls: 'talk', title: 'Click to latch, hold to talk' });
  mon.talk.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    downAt = now(); wasOn = S.mon.talkback;
    if (!wasOn) setMon('talkback', true);
  });
  mon.talk.addEventListener('pointerup', () => {
    if (!downAt) return;
    const held = now() - downAt > 400;
    downAt = 0;
    if (wasOn || held) setMon('talkback', false);
  });
  mon.talk.addEventListener('keydown', e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setMon('talkback', !S.mon.talkback); }
  });
  /* [Monitor] talkback dims the monitor (followTalkback); its cue sends are on the Cues page, its monitor-mix
     channel is the TB strip */
  mon.tbDim = el('input', { type: 'checkbox', onchange: e => { cfg.talkbackDim = e.target.checked; saveConfig(); } });
  mon.talkCard = el('div', { class: 'mon-card' }, mon.talk,
    el('span', { class: 'hp-note', text: 'Click to latch. Hold to talk, release to stop.' }),
    el('label', { class: 'check' }, mon.tbDim, 'Dim the monitor while talking'),
    el('button', { class: 'btn btn-quiet talk-cues', title: 'The talk mic\'s sends to each cue: the Cues page',
      onclick: () => showView('cues') }, 'Cue sends'));
  root.append(mon.talkCard);
  renderMonitor();
}

/* ---------- [Monitor] the parts of the monitor column that differ per model ---------- */

let monBuiltType = -1;

function syncMonitorModel() {
  const type = unitType(S.sel);
  if (type === monBuiltType || !mon.mono) return;
  monBuiltType = type;
  const k = capsOf(type);
  mon.source = seg(['MIX', ...k.cues], i => setMon('source', i), 'Monitor source');
  if (type === 0x22) {
    for (const b of mon.source.node.querySelectorAll('button')) {
      if (/CUE [34]/.test(b.textContent)) b.title = 'CUE 3 and CUE 4 as the monitor source follow the driver\'s encoding, not yet checked on the x8p';
    }
  }
  mon.srcWrap.replaceChildren(mon.source.node);
  const hpNames = k.cues.slice(0, 2);
  mon.hp1src = seg(hpNames, i => setMon('hp1src', i), 'Headphone 1 source');
  mon.hp2src = seg(hpNames, i => setMon('hp2src', i), 'Headphone 2 source');
  mon.hp1Wrap.replaceChildren(mon.hp1src.node);
  mon.hp2Wrap.replaceChildren(mon.hp2src.node);
  mon.hp1Lbl.textContent = k.hps > 1 ? 'HP 1 source' : 'Source';
  const rows = [];
  mon.alts = k.alts.map((out, n) => {
    const on = sw(`ALT ${n + 1}`, { title: `Use ${out} as the ALT ${n + 1} speaker pair`, onClick: () => setAlt(n, 'on', !altOn(S.sel, n)) });
    const trim = new Knob({
      size: 40, min: 0, max: 120, step: 1, wheelStep: 1, pageStep: 12, value: ALT_TRIM_UNITY, origin: 0.5,
      aria: `ALT ${n + 1} trim`, format: fmtTrim, editText: v => ((v - ALT_TRIM_UNITY) / 2).toFixed(1),
      parse: s => { const v = parseFloat(String(s).replace(/db/i, '')); return Number.isFinite(v) ? Math.round(ALT_TRIM_UNITY + 2 * clamp(v, -30, 30)) : null; },
      onChange: v => setAlt(n, 'trim', v),
    });
    rows.push(el('div', { class: 'alt-row' }, on, el('span', { class: 'alt-out', text: out }), trim.node));
    return { on, trim };
  });
  mon.altRows.replaceChildren(...rows);
  mon.spkKey = '';                     /* the speaker choices follow too */
}

const fmtTrim = v => { const db = (v - ALT_TRIM_UNITY) / 2; return `${db > 0 ? '+' : ''}${db.toFixed(1)} dB`; };

function renderSpeakers() {
  const k = caps(), m = S.mon, idx = S.sel;
  const on = k.alts.map((_, n) => altOn(idx, n));
  const key = on.map(Number).join('');
  if (mon.spkKey !== `${idx}:${key}:${k.alts.length}`) {
    mon.spkKey = `${idx}:${key}:${k.alts.length}`;
    const opts = [0, ...k.alts.map((_, n) => n + 1).filter(n => on[n - 1])];
    mon.speakers = seg(opts.map(n => SPEAKERS[n]), i => setMon('altsel', opts[i]), 'Speakers');
    mon.speakers.opts = opts;
    mon.spkWrap.replaceChildren(mon.speakers.node);
  }
  mon.spkField.hidden = !on.some(Boolean);
  mon.speakers.set(mon.speakers.opts.indexOf(m.alt));
  mon.speakers.node.classList.toggle('refused', isRefused('mon:alt'));
  for (const b of mon.speakers.node.querySelectorAll('button')) b.classList.toggle('unconfirmed', isPending('mon:alt'));
  mon.altCard.hidden = !k.alts.length;
  const mix = mixOf(idx);
  (mon.alts || []).forEach((a, n) => {
    setPressed(a.on, on[n]);
    a.on.classList.toggle('refused', isRefused(`alt:alt${n + 1}`));
    a.trim.set(mix.alt[n].trim === undefined ? ALT_TRIM_UNITY : mix.alt[n].trim);
    a.trim.node.classList.toggle('refused', isRefused(`alt:alt${n + 1}trim`));
  });
}

/* Every segment's master and mix, then the focused unit's monitor column. */
function renderMonitor() {
  renderMasters();
  renderMix();
  if (!mon.mono) return;
  syncMonitorModel();
  Outs.render();                       /* [Out] */
  const m = S.mon;
  mon.hp1.set(m.hp1);
  mon.hp1.setUnconfirmed(isPending('mon:hp1'));
  mon.hp1.node.classList.toggle('refused', isRefused('mon:hp1'));
  /* The focused unit's model (MODEL_CAPS) decides which controls exist. */
  const c = caps();
  mon.hp1.node.hidden = !c.hpLevel;
  mon.hpSide.hidden = !c.hpSource;
  mon.hp2Lbl.hidden = mon.hp2Wrap.hidden = c.hps < 2;
  put(mon.hpNote, [c.hpLevel ? null : 'Headphone levels: the knobs on the unit.',
    c.hpSource ? null : 'Headphone source: not available yet.'].filter(Boolean).join(' '));
  mon.hpNote.hidden = c.hpLevel && c.hpSource;
  mon.dimField.hidden = !c.dimLevel;
  mon.srcField.hidden = !c.monSource;
  mon.talkCard.hidden = !c.talkback;
  mon.tbDim.checked = !!cfg.talkbackDim;
  for (const k of ['mono', 'talkback']) {
    const node = k === 'talkback' ? mon.talk : mon[k];
    setPressed(node, m[k]);
    node.classList.toggle('unconfirmed', isPending(`mon:${k}`));
    node.classList.toggle('refused', isRefused(`mon:${k}`));
  }
  mon.dimLevel.value = String(m.dimLevel);
  mon.dimLevel.classList.toggle('refused', isRefused('mon:dimLevel'));
  mon.source.set(m.source);
  mon.source.node.classList.toggle('refused', isRefused('mon:source'));
  mon.hp1src.set(m.hp1src);
  mon.hp1src.node.classList.toggle('refused', isRefused('mon:hp1src'));
  mon.hp2src.set(m.hp2src);
  mon.hp2src.node.classList.toggle('refused', isRefused('mon:hp2src'));
  renderSpeakers();
  renderMonUnits();
}

/* ------------------------------------------------------------------ */
/* [Monitor] Cues view: the selected unit's cue mixes                  */
/* ------------------------------------------------------------------ */

/*
 * One cue at a time (CUE 1-4 on the x8p; HP and LINE 3/4 on the Twin X): a strip per source (the unit's
 * inputs and its talkback mic) with that source's send level and pan, and the cue's own settings: its source
 * (its own mix, or the monitor mix), mono, mute, and where it is heard now. The sends are DSP bus
 * coefficients with no readback: the app keeps them per unit and sends them again when the unit connects.
 */
const cues = { built: -1, sel: 0, strips: [], root: null };

/* Send taper: off at the bottom, -60..-20 dB over the next third, -20..+12 dB over the rest. */
const sendToPos = v => {
  if (v <= SEND_OFF) return 0;
  const db = v / 2;
  return 0.03 + 0.97 * (db <= -20 ? ((db + 60) / 40) * 0.35 : 0.35 + ((db + 20) / 32) * 0.65);
};
const sendFromPos = p => {
  if (p < 0.02) return SEND_OFF;
  const q = Math.max(0, (p - 0.03) / 0.97);
  const db = q <= 0.35 ? -60 + (q / 0.35) * 40 : -20 + ((q - 0.35) / 0.65) * 32;
  return clamp(Math.round(db * 2), -120, SEND_MAX);
};
const fmtSend = v => (v <= SEND_OFF ? 'off' : `${v > 0 ? '+' : ''}${(v / 2).toFixed(1)} dB`);
const parseSend = s => {
  const t = String(s).trim().toLowerCase();
  if (t === 'off' || t === '-inf') return SEND_OFF;
  const v = parseFloat(t.replace(/db/, ''));
  return Number.isFinite(v) ? Math.round(2 * clamp(v, -60, 12)) : null;
};
const fmtPan = v => (v === 0 ? 'C' : v < 0 ? `L${-v}` : `R${v}`);
const parsePan = s => {
  const t = String(s).trim().toUpperCase();
  if (t === 'C') return 0;
  const m = /^([LR])\s*(\d+)$/.exec(t);
  if (m) return clamp(Number(m[2]), 0, 100) * (m[1] === 'L' ? -1 : 1);
  const v = parseInt(t, 10);
  return Number.isFinite(v) ? clamp(v, -100, 100) : null;
};

const cueSrcLabel = (idx, bus) => (bus === TALKBACK_BUS ? 'Talkback' : (chanCfg(idx, bus).name || `Input ${bus + 1}`));

function buildCues() {
  cues.root = document.getElementById('cues');
  if (!cues.root) return;
  const type = unitType(S.sel), k = capsOf(type);
  cues.built = type;
  if (cues.sel >= k.cues.length) cues.sel = 0;
  cues.pick = seg(k.cues, i => { cues.sel = i; renderCues(); }, 'Cue mix');
  cues.strips = cueBuses(k).map(bus => {
    const tb = bus === TALKBACK_BUS, who = tb ? 'Talkback' : `Input ${bus + 1}`;
    const level = new Knob({
      size: 56, min: SEND_OFF, max: SEND_MAX, step: 1, wheelStep: 2, pageStep: 12, value: SEND_OFF,
      aria: `${who} send`, toPos: sendToPos, fromPos: sendFromPos, format: fmtSend,
      editText: v => (v <= SEND_OFF ? 'off' : (v / 2).toFixed(1)), parse: parseSend,
      onChange: v => setSend(cues.sel, bus, 'level', v),
    });
    const pan = new Knob({
      size: 38, min: -100, max: 100, step: 1, wheelStep: 5, pageStep: 25, value: 0, label: 'Pan', origin: 0.5,
      aria: `${who} pan`, format: fmtPan, editText: fmtPan, parse: parsePan,
      onChange: v => setSend(cues.sel, bus, 'pan', v),
    });
    const name = el('div', { class: 'strip-name' });
    const node = el('div', { class: 'cue-strip', dataset: { bus: String(bus) } },
      el('div', { class: 'strip-top' }, el('div', { class: 'strip-num', text: tb ? 'TB' : String(bus + 1) })),
      name, level.node, pan.node);
    return { bus, level, pan, name, node };
  });
  cues.title = el('div', { class: 'sect-title' });
  cues.src = seg(['CUE', 'MIX'], i => setCueFlag(cues.sel, 'mix', i), 'Cue source');
  cues.src.node.title = 'CUE: this cue\'s own mix of the sends. MIX: the monitor mix instead.';
  cues.mono = sw('MONO', { big: true, onClick: () => setCueFlag(cues.sel, 'mono', !mixOf(S.sel).cues[cues.sel].mono) });
  cues.mute = sw('MUTE', { red: true, big: true, onClick: () => setCueFlag(cues.sel, 'mute', !mixOf(S.sel).cues[cues.sel].mute) });
  cues.heard = el('div', { class: 'cue-heard' });
  cues.notes = el('div', { class: 'cue-notes' });
  const card = el('div', { class: 'mon-card cue-card' }, cues.title,
    el('div', { class: 'field' }, el('span', { class: 'lbl', text: 'Source' }), cues.src.node),
    el('div', { class: 'row2' }, cues.mono, cues.mute),
    el('div', { class: 'cue-heard-wrap' }, el('span', { class: 'lbl', text: 'Heard on' }), cues.heard),
    cues.notes);
  cues.row = el('div', { class: 'cue-strips', 'aria-label': 'Cue sends' }, cues.strips.map(s => s.node));
  cues.empty = el('p', { class: 'fine', text: 'This model has no cue mixes Apollo Control knows.' });
  cues.root.replaceChildren(
    el('div', { class: 'cue-head' }, el('span', { class: 'lbl', text: 'Cue' }), cues.pick.node),
    el('div', { class: 'cue-body' }, cues.row, card),
    cues.empty);
}

/* Where cue c of unit idx is heard with the sources set now. */
function cueHeardOn(idx, c) {
  const k = capsOf(unitType(idx)), m = ctl(idx).mon, out = [];
  if (k.hps >= 1 && c === m.hp1src) out.push(k.hps > 1 ? 'HP 1' : 'Headphones');
  if (k.hps >= 2 && c === m.hp2src) out.push('HP 2');
  if (unitType(idx) === 0x99 && c === 1 && !altOn(idx, 0)) out.push('Line 3/4');
  if (m.source === c + 1) out.push('Monitor');
  return out.length ? joinList(out) : 'nowhere yet: pick it as a headphone or monitor source';
}

function renderCues() {
  if (!cues.root) return;
  if (unitType(S.sel) !== cues.built) buildCues();
  const idx = S.sel, k = caps(), c = cues.sel;
  cues.empty.hidden = k.cues.length > 0;
  cues.root.querySelector('.cue-head').hidden = cues.root.querySelector('.cue-body').hidden = !k.cues.length;
  if (!k.cues.length) return;
  const q = mixOf(idx).cues[c];
  cues.pick.set(c);
  for (const s of cues.strips) {
    const v = sendOf(idx, c, s.bus);
    s.level.set(v.level);
    s.pan.set(v.pan);
    s.level.node.classList.toggle('refused', isRefused(`cue:send:${c + 1}:${s.bus}`));
    s.pan.node.classList.toggle('refused', isRefused(`cue:pan:${c + 1}:${s.bus}`));
    const n = cueSrcLabel(idx, s.bus);
    if (s.name.textContent !== n) s.name.textContent = n;
    s.node.hidden = s.bus !== TALKBACK_BUS && !!chanCfg(idx, s.bus).hidden;
  }
  cues.row.classList.toggle('muted', !!q.mute);
  cues.title.textContent = `${k.cues[c]}${idx ? ` · ${unitShort(idx)}` : ''}`;
  cues.src.set(q.mix === 1 ? 1 : 0);
  cues.src.node.classList.toggle('refused', isRefused(`cue:cue${c + 1}mix`));
  setPressed(cues.mono, !!q.mono);
  cues.mono.classList.toggle('refused', isRefused(`cue:cue${c + 1}mono`));
  setPressed(cues.mute, !!q.mute);
  cues.mute.classList.toggle('refused', isRefused(`cue:cue${c + 1}mute`));
  put(cues.heard, cueHeardOn(idx, c));
  const notes = ['The talkback mic has no input strip on this driver yet, so its sends carry nothing until one exists.'];
  if (unitType(idx) === 0x22 && c >= 2) notes.unshift('Headphones take CUE 1 or CUE 2 here: CUE 3 and CUE 4 are heard as the monitor source.');
  notes.push('The unit does not report cue sends: Apollo Control keeps them and sends them again when the unit connects.');
  const key = notes.join('|');
  if (cues.notesKey !== key) {
    cues.notesKey = key;
    cues.notes.replaceChildren(...notes.map(t => el('p', { class: 'hp-note', text: t })));
  }
}

/* ------------------------------------------------------------------ */
/* Channels view                                                       */
/* ------------------------------------------------------------------ */

let bridgeCells = [];

function buildBridge() {
  for (const c of bridgeCells) METERS.splice(METERS.indexOf(c.meter), 1);
  bridgeCells = [];
  const root = document.getElementById('bridge');
  root.replaceChildren();
  for (const g of REC_GROUPS) {
    const row = el('div', { class: 'bgroup-row' });
    g.labels.forEach((label, i) => {
      const slot = g.from + i;
      const meter = new Meter({ scale: false, slot, group: 'channels' });
      meter.setNoClock(!ctl(0).levelsValid);
      const db = el('div', { class: 'bcell-db num', text: '--' });
      const custom = g.inputs ? cfg.channels[i].name : '';
      const base = g.inputs ? `In ${label}` : g.name === 'Other' ? label : `${g.name} ${label}`;
      const name = el('div', { class: 'bcell-name', text: custom || base, title: custom ? `Input ${i + 1}: ${custom}` : null });
      row.append(el('div', { class: 'bcell', title: `Record slot ${slot + 1} of the capture ring` }, db, meter.node, name,
        el('div', { class: 'bcell-slot num', text: `#${slot + 1}` })));
      bridgeCells.push({ slot, meter, db, last: '' });
    });
    root.append(el('div', { class: 'bgroup' }, el('div', { class: 'bgroup-name', text: g.name }), row));
  }
}

/* ------------------------------------------------------------------ */
/* Status view: what is live, one section per unit                     */
/* ------------------------------------------------------------------ */

/* Unit 0 is the x8p on apollo.sys. When the x8p and the Twin X run as one device the Twin X is
   unit 1 and gets its own section from the same code; the combined device's settings (channel
   order, clock master, output routing) belong on the Units & routing view, not here. */

const STRIPS_STATES = {
  0: ['off', 'off'], 1: ['idle', 'off'], 2: ['armed, waiting to build', 'warn'], 3: ['building', 'warn'],
  4: ['live, pass complete', 'ok'], 5: ['FAILED', 'bad'],
};
const STRIPS_FLAG = { QUARANTINED: 0x02, TAINTED: 0x04 };
const STRIP_TAKEN = 2, STRIP_QUEUED = 1;
const EP_FLAG = { ENABLED: 0x01, ADDED: 0x02, STREAM: 0x04, PREPARED: 0x08, RUNNING: 0x10 };
const ASIO = { CLOSED: 0, OPEN: 1, RUNNING: 2 };
const TROUBLE_MS = 10000;              /* a fault counter that rose this recently shows amber */

function onLive(m) {
  const idx = Number(m.unit) | 0;
  if (idx < 0 || idx >= MAX_UNITS) return;
  if (idx > 0 && !S.units[idx]) return;          /* late, from a unit that left (see onUnitStatus) */
  const u = S.units[idx] || (S.units[idx] = newUnit());
  const counts = {};
  if (m.engine) {
    counts.late = m.engine.lateTicks;
    counts.asioUnder = m.engine.asioUnderruns;
    counts.asioOver = m.engine.asioOverruns;
  }
  if (m.endpoints && Array.isArray(m.endpoints.rows)) {
    m.endpoints.rows.forEach((r, i) => { counts[`ep${i}u`] = r.underruns; counts[`ep${i}o`] = r.overruns; });
  }
  const t = now();
  if (u.counts) {
    for (const [k, v] of Object.entries(counts)) if (u.counts[k] !== undefined && v > u.counts[k]) u.rose[k] = t;
  }
  u.counts = counts;
  u.live = m;
  renderUnits();
  Sess.onLive(idx);                    /* [Sess] the last state waits for the unit's strips to be Live */
}

/* only: one unit slot; none = every unit (demo on/off). */
function clearLive(only) {
  S.units.forEach((u, idx) => {
    if (u && (only === undefined || idx === only)) { u.live = null; u.counts = null; u.rose = {}; }
  });
  renderUnits();
}

const rose = (u, k) => k in u.rose && now() - u.rose[k] < TROUBLE_MS;
const fwText = i => { const fw = i.fwRelease >>> 0; return `${fw >>> 24}.${(fw >>> 16) & 0xFF}.${fw & 0xFFFF}`; };

/* '' (nothing to report), 'ok', 'warn' (a fault counter rose lately) or 'bad'. */
function unitHealth(u) {
  const L = u.live;
  if (!L) return '';
  const e = L.engine, s = L.strips;
  if ((e && e.lost) || (s && (s.state === 5 || (s.flags & (STRIPS_FLAG.QUARANTINED | STRIPS_FLAG.TAINTED))))) return 'bad';
  if (Object.keys(u.rose).some(k => rose(u, k))) return 'warn';
  return e && e.running ? 'ok' : '';
}

function naReason(code) {
  if (!S.demo && !S.host.present) return 'No device on apollo.sys';
  return code === 1 ? 'This apollo.sys build does not report it: install the current driver'
    : code === 21 ? 'Waiting for the unit to finish starting up'
    : code === 5 ? 'The installed driver refused access'
    : code === 13 ? 'The driver answered with less than this app expects'
    : code ? `Error ${code}` : 'No data from the driver yet';
}

/* In-place updates: the view refreshes every second without rebuilding, so hover and tooltips hold. */
function put(node, text, cls, title) {
  if (text !== null && node.textContent !== text) node.textContent = text;
  if (cls !== undefined && node.className !== cls) node.className = cls;
  if (title !== undefined) {
    if (title) { if (node.title !== title) node.title = title; } else if (node.hasAttribute('title')) node.removeAttribute('title');
  }
}

/* Cell spec: a string, or { t, dot: 'ok'|'warn'|'bad'|'off'|'idle', cls, title }. */
function patchRows(tbody, rows) {
  while (tbody.rows.length > rows.length) tbody.lastElementChild.remove();
  rows.forEach((cells, r) => {
    let tr = tbody.rows[r];
    if (!tr || tr.cells.length !== cells.length) {
      const fresh = el('tr', {}, cells.map(() => el('td', {}, el('span', { class: 'dot', hidden: true }), el('span'))));
      if (tr) tr.replaceWith(fresh); else tbody.append(fresh);
      tr = fresh;
    }
    cells.forEach((c, i) => {
      const spec = (c !== null && typeof c === 'object') ? c : { t: String(c) };
      const td = tr.cells[i];
      const [dot, txt] = td.children;
      dot.hidden = !spec.dot;
      if (spec.dot) put(dot, '', `dot ${spec.dot}`);
      put(txt, spec.t === undefined ? '' : String(spec.t));
      put(td, null, spec.cls || '', spec.title || '');
    });
  });
}

const counter = (u, key, n, title) => ({ t: String(n), cls: 'n' + (rose(u, key) ? ' warn-text' : ''), title });
const NA = { t: '-', cls: 'faint' };
const NA_N = { t: '-', cls: 'n faint' };       /* in a number column */

function buildUnitView(idx) {
  const head = (labels) => el('thead', {}, el('tr', {}, labels.map(([t, cls, title]) => el('th', { class: cls || null, title: title || null, text: t }))));
  const v = {
    name: el('span', { class: 'unit-name' }),
    meta: el('span', { class: 'unit-meta num' }),
    stripDot: el('span', { class: 'dot', hidden: true }),
    stripText: el('span'),
    stripTags: el('span', { class: 'st-tags' }),
    tagKey: '',
    why: el('div', { class: 'st-why' }),
    inNote: el('div', { class: 'st-in-note', hidden: true }),
    inBody: el('tbody'),
    engNa: el('div', { class: 'st-na', text: 'unavailable' }),
    engBody: el('tbody'),
    epSum: el('span', { class: 'muted num' }),
    epNa: el('div', { class: 'st-na', text: 'unavailable' }),
    epBody: el('tbody'),
  };
  v.engTable = el('table', { class: 'st-table st-kv' }, v.engBody);
  v.epTable = el('table', { class: 'st-table' },
    head([['Dir'], ['Name'], ['Slots', 'n'], ['State'], ['Streams', 'n'],
      ['Underruns', 'n', 'Playback packets played as silence: the app was late'],
      ['Overruns', 'n', 'Record packets overwritten before the app read them']]),
    v.epBody);
  const inputs = el('div', { class: 'panel st-inputs' },
    el('div', { class: 'panel-head' }, el('h2', { text: 'Inputs' }),
      el('span', { class: 'st-line' }, el('span', { class: 'muted', text: 'Strips' }), v.stripDot, v.stripText, v.stripTags)),
    v.why,
    v.inNote,
    el('table', { class: 'st-table' },
      head([['In', 'n'], ['Name'], ['Strip', null, 'Input strip on the DSP: taken = the DSP took the whole transaction'],
        ['Started', null, 'SYNCH sent: the strip runs'], ['48V'], ['Input'], ['Gain', 'n'], ['Pad'], ['Low cut'], ['Ø', null, 'Polarity invert']]),
      v.inBody));
  const engine = el('div', { class: 'panel st-engine' },
    el('div', { class: 'panel-head' }, el('h2', { text: 'Audio engine' })), v.engNa, v.engTable);
  const endpoints = el('div', { class: 'panel st-endpoints' },
    el('div', { class: 'panel-head' }, el('h2', { text: 'Windows endpoints' }), v.epSum), v.epNa, v.epTable);
  v.node = el('section', { class: 'unit', dataset: { unit: String(idx) } },
    el('div', { class: 'unit-head' }, v.name, v.meta),
    el('div', { class: 'unit-grid' }, inputs, engine, endpoints));
  v.node.setAttribute('aria-label', `Unit ${idx + 1} status`);
  return v;
}

const unitViews = [];

/* One section per present unit slot, in slot order; a unit that left loses its section. */
function renderUnits() {
  renderStatusDot();
  if (S.view !== 'status') return;
  const root = document.getElementById('units');
  const n = Math.max(S.units.length, unitViews.length);
  for (let idx = 0; idx < n; idx++) {
    const u = S.units[idx];
    let v = unitViews[idx];
    if (!u) {
      if (v) { v.node.remove(); unitViews[idx] = undefined; }
      continue;
    }
    if (!v) {
      v = unitViews[idx] = buildUnitView(idx);
      const next = unitViews.slice(idx + 1).find(Boolean);
      if (next) root.insertBefore(v.node, next.node); else root.append(v.node);
    }
    renderUnit(v, u, idx);
  }
}

function renderStatusDot() {
  const rank = { '': 0, ok: 1, warn: 2, bad: 3 };
  let worst = '';
  for (const u of S.units) { if (!u) continue; const h = unitHealth(u); if (rank[h] > rank[worst]) worst = h; }
  const dot = document.getElementById('statusDot');
  if (!dot) return;
  dot.hidden = !worst;
  if (worst && dot.dataset.state !== worst) dot.dataset.state = worst;
  const tab = document.getElementById('tab-status');
  const label = worst === 'bad' ? 'Status: a fault needs attention' : worst === 'warn' ? 'Status: dropouts in the last 10 seconds' : null;
  if (label) tab.title = label; else tab.removeAttribute('title');
}

/* Preamp state from readback words, spec 4.1: channels 0-3 in words 0 (flags) and 3 (GainC),
   4-7 in words 5 and 7, Hi-Z jacks in word 0 bits 24+. The same bits on the x8p and the Twin X. */
function parsePreamps(w, n, hiz) {
  return Array.from({ length: n }, (_, ch) => {
    const fw = ch < 4 ? w[0] : w[5], b = 6 * (ch & 3);
    const gc = ((ch < 4 ? w[3] : w[7]) >>> ((ch & 1) * 6 + (ch & 2) * 8)) & 0x3F;
    return {
      line: bit(fw, b), pad: bit(fw, b + 1), p48: bit(fw, b + 3), lowcut: bit(fw, b + 4), phase: bit(fw, b + 5),
      gain: gc === 0 ? 10 : gc >= 0x39 ? 65 : gc + 9, hiz: ch < hiz ? bit(w[0], 24 + ch) : false,
    };
  });
}
const blankPreamps = n => Array.from({ length: n }, () => ({ gain: 10, line: false, pad: false, p48: false, lowcut: false, phase: false, hiz: false }));

function renderUnit(v, u, idx) {
  const own = idx === 0;                        /* the x8p: its status in S.host, its inputs in ctl(0) */
  const i = own ? info() : u.info;              /* other units carry their own status info */
  const entry = listedUnit(idx);
  const L = u.live;
  const type = (i && i.deviceType) || (entry && entry.deviceType) || 0;
  /* Below Mode 2 the host sends the unit no setting. Observe: identified and quiesced, nothing runs. */
  const ro = readOnlyMode(i, entry);
  const observe = ro === 'observe';
  put(v.name, (i && i.modelName) || (entry && entry.model) || DEVICE_NAMES[type] || (own ? 'Apollo x8p' : `Unit ${idx + 1}`));
  let meta = 'not connected';
  if (i || (!own && entry)) {
    meta = [(i ? i.serial : entry.serial) || '-', i ? `fw ${fwText(i)}` : entry.fw ? `fw ${entry.fw}` : null,
      i && i.sampleRate ? `${(i.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz` : null,
      ro ? `${ro} mode, read-only` : null,
      !i || i.stage === STAGE.CONNECTED ? null : (STAGE_NAMES[i.stage] || `stage ${i.stage}`)].filter(Boolean).join(' · ');
  }
  put(v.meta, meta);
  /* Inputs: the x8p from its control state (ctl(0)), another unit from its own readback. An
     observe unit runs nothing, and a model without a preamp table shows a note instead. */
  const nIn = own ? PREAMPS : observe ? 0 : (i && i.numPreamps) || capsOf(type).preamps;
  const inputs = own ? ctl(0).inputs : u.rbValid ? parsePreamps(u.words, nIn, capsOf(type).hiz) : blankPreamps(nIn);
  put(v.inNote, nIn ? '' : observe
    ? 'Observe mode: identified only. Nothing runs on this unit and no setting is sent to it.'
    : 'Inputs of this model are not shown yet.');
  v.inNote.hidden = nIn > 0;

  // Strips summary
  const s = L && L.strips;
  if (!s) {
    v.stripDot.hidden = true;
    put(v.stripText, 'unavailable', 'st-na', naReason(L ? L.stripsErr : 0));
    v.why.hidden = true;
  } else {
    const [txt, dot] = STRIPS_STATES[s.state] || [`state ${s.state}`, 'off'];
    v.stripDot.hidden = false;
    put(v.stripDot, '', `dot ${dot}`);
    const stage = s.version >= 2 && s.stepsLimit ? ` · stage ${s.stepsDone} of ${s.stepsLimit}` : '';
    put(v.stripText, txt + stage, s.state === 5 ? 'bad-text' : '', '');
    put(v.why, s.why ? `Why: ${s.why}` : '');
    v.why.hidden = !s.why;
  }
  const tags = [];
  if (s && (s.flags & STRIPS_FLAG.QUARANTINED)) tags.push(['QUARANTINED', 'An outcome could not be proven. Power-cycle the Apollo.']);
  if (s && (s.flags & STRIPS_FLAG.TAINTED)) tags.push(['TAINTED', 'A raw register write happened. Power-cycle the Apollo.']);
  const tagKey = tags.map(t => t[0]).join(',');
  if (tagKey !== v.tagKey) {
    v.tagKey = tagKey;
    v.stripTags.replaceChildren(...tags.map(([t, title]) => el('span', { class: 'tag bad', title, text: t })));
  }

  // Inputs: strip per input, then 48V and the preamp from readback
  const rb = own ? ctl(0).rbValid && (S.demo || connected(0)) : !!u.rbValid;
  const noRb = { t: '-', cls: 'faint', title: 'No readback from the unit' };
  patchRows(v.inBody, inputs.map((inp, ch) => {
    let strip = NA, started = NA;
    if (s && s.perInput) {
      const k = s.taken[ch];
      strip = k === STRIP_TAKEN ? { t: 'taken', dot: 'ok' }
        : k === STRIP_QUEUED ? { t: 'queued, unproven', dot: 'warn', title: 'Published to the DSP, not proven taken' }
        : { t: '-', cls: 'faint', title: (s.mask >>> ch) & 1 ? 'Nothing sent yet' : 'Not in StripMask' };
      started = s.started[ch] ? { t: 'yes' } : NA;
    }
    const sw = on => (on ? { t: 'on' } : NA);
    return [
      { t: String(ch + 1), cls: 'n' },
      { t: (own && cfg.channels[ch].name) || '', cls: 'dim' },     /* channel names are the x8p's */
      strip, started,
      rb ? (inp.p48 ? { t: '48V', cls: 'phantom' } : NA) : noRb,
      rb ? { t: inp.hiz ? 'HI-Z' : inp.line ? 'LINE' : 'MIC' } : noRb,
      rb ? { t: `${inp.gain} dB`, cls: 'n' } : { ...noRb, cls: 'n faint' },
      rb ? sw(inp.pad) : noRb,
      rb ? sw(inp.lowcut) : noRb,
      rb ? sw(inp.phase) : noRb,
    ];
  }));

  // Audio engine
  const e = L && L.engine;
  v.engNa.hidden = !!e;
  v.engTable.hidden = !e;
  if (!e) {
    v.engNa.title = naReason(L ? L.engineErr : 0);
  } else {
    const nominal = i && i.sampleRate ? ` (nominal ${i.sampleRate})` : '';
    const asio = e.asioState === ASIO.RUNNING ? { t: `streaming, ${e.asioFrames} frames`, dot: 'ok' }
      : e.asioState === ASIO.OPEN ? { t: `open, ${e.asioFrames} frames`, dot: 'idle', title: 'Buffers mapped, not streaming' }
      : { t: 'none', cls: 'faint' };
    if (e.asioState !== ASIO.CLOSED) asio.title = `${asio.title ? asio.title + '. ' : ''}${e.asioIn} in, ${e.asioOut} out`;
    patchRows(v.engBody, [
      ['State', e.lost ? { t: 'LOST', dot: 'bad', cls: 'bad-text', title: 'The device stopped answering. Restart the transport or replug the unit.' }
        : e.running ? { t: 'running', dot: 'ok' } : { t: 'stopped', dot: 'off' }],
      ['Tick rate', e.ticksPerSec == null ? NA : { t: `${e.ticksPerSec.toFixed(1)} /s`, cls: 'n' }],
      ['Frames/s', e.framesPerSec == null ? NA
        : { t: e.framesPerSec.toFixed(1), cls: 'n', title: `Measured from the hardware position: the real sample rate${nominal}` }],
      ['Late ticks', counter(u, 'late', e.lateTicks, 'The render head fell behind the hardware: frames lost')],
      ['Underruns', counter(u, 'asioUnder', e.asioUnderruns, 'ASIO playback halves zeroed: the client was late')],
      ['Overruns', counter(u, 'asioOver', e.asioOverruns, 'ASIO record halves overwritten before the client finished')],
      ['ASIO client', asio],
    ]);
  }

  // Windows endpoints: as many rows as the driver reports
  const ep = L && L.endpoints;
  const epOk = !!ep && ep.layoutOk !== false && Array.isArray(ep.rows);
  v.epNa.hidden = epOk;
  v.epTable.hidden = !epOk;
  put(v.epSum, epOk ? `${ep.rows.length} endpoint${ep.rows.length === 1 ? '' : 's'} · ${ep.prepared} stream${ep.prepared === 1 ? '' : 's'} prepared` : '');
  if (!epOk) {
    v.epNa.title = ep ? 'This app was built for a different endpoint table: rebuild it against the current driver header' : naReason(L ? L.endpointsErr : 0);
  } else {
    patchRows(v.epBody, ep.rows.map((r, n) => {
      const a = (r.slots[0] >>> 0) + 1, b = (r.slots[1] >>> 0) + 1;
      return [
        { t: r.render ? 'out' : 'in', cls: 'dim' },
        { t: r.name || '-', title: r.circuit ? `Circuit ${r.circuit}` : '' },
        { t: r.channels > 1 ? `${a}-${b}` : String(a), cls: 'n' },
        endpointState(r),
        { t: String(r.streams), cls: 'n' },
        r.render ? counter(u, `ep${n}u`, r.underruns) : NA_N,
        r.render ? NA_N : counter(u, `ep${n}o`, r.overruns),
      ];
    }));
  }
}

function endpointState(r) {
  const f = r.flags;
  if (!(f & EP_FLAG.ENABLED)) return { t: 'disabled', cls: 'faint', title: 'Not in EnabledEndpoints' };
  if (!(f & EP_FLAG.ADDED)) return { t: 'created, not added', dot: 'warn', title: 'Windows does not see it' };
  if (f & EP_FLAG.RUNNING) return { t: 'running', dot: 'ok', title: `${r.packetCount} x ${r.packetBytes} B packets` };
  if (f & EP_FLAG.PREPARED) return { t: 'paused', dot: 'idle' };
  if (f & EP_FLAG.STREAM) return { t: 'stream open', dot: 'idle' };
  return { t: 'idle', dot: 'off' };
}

/* ------------------------------------------------------------------ */
/* Device view                                                         */
/* ------------------------------------------------------------------ */

let logTimer = 0;

function buildDevice() {
  document.getElementById('logRefresh').addEventListener('click', requestLog);
  document.getElementById('paramsRefresh').addEventListener('click', requestParams);    /* [Params] */
  document.getElementById('diagSave').addEventListener('click', () => {                 /* [Diag] app/src/diag_host.h */
    if (S.demo || !host) { toast('Diagnostics come from the driver: not in the demo.', 'warn'); return; }
    send(JSON.stringify({ cmd: 'diag-save' }));
    toast('Saving diagnostics...');
  });
  document.getElementById('logCopy').addEventListener('click', () => {
    const text = document.getElementById('log').textContent;
    navigator.clipboard.writeText(text).then(() => toast('Driver log copied.'), () => toast('Copy failed.', 'warn'));
  });
  document.getElementById('logAuto').addEventListener('change', e => {
    clearInterval(logTimer);
    if (e.target.checked) { requestLog(); logTimer = setInterval(requestLog, 2000); }
  });
}

function requestLog() {
  if (S.demo) showLog(DEMO_LOG, DEMO_LOG.length);
  else if (S.host.present) send('getLog');
  else showLog('No device: the log lives in the driver, which is not loaded.', 0);
}

function showLog(text, total) {
  const pre = document.getElementById('log');
  const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 8;
  pre.textContent = (total > text.length ? `(older lines dropped: showing the last ${text.length} of ${total} bytes)\n` : '') + text;
  if (atBottom) pre.scrollTop = pre.scrollHeight;
}

function renderDevice() {
  const i = info();
  const dl = document.getElementById('devInfo');
  const rows = [];
  if (!i) {
    rows.push(['Status', S.host.error === 5 ? 'access denied by the installed driver' : 'no device on apollo.sys']);
    if (S.host.detail) rows.push(['Detail', S.host.detail]);
  } else {
    const fw = i.fwRelease >>> 0;
    rows.push(
      ['Model', `${DEVICE_NAMES[i.deviceType] || 'unknown'} (type ${hex(i.deviceType, 2)}, public ${hex(i.publicType, 2)})`],
      ['Serial', i.serial || '-'],
      ['Stage', `${STAGE_NAMES[i.stage] || i.stage}${i.stage === STAGE.FAILED ? ` at ${STAGE_NAMES[i.failedStage] || i.failedStage}` : ''}`],
      ['Last status', i.lastStatus ? hex(i.lastStatus) : 'none'],
      ['Firmware', `${fw >>> 24}.${(fw >>> 16) & 0xFF}.${fw & 0xFFFF}`],
      ['FPGA revision', hex(i.fpgaRev)],
      ['Ext caps', hex(i.extCaps)],
      ['PCI subsystem', hex(i.subsystemId, 4)],
      ['DSPs / preamps', `${i.numDsps} / ${i.numPreamps}`],
      ['Sample rate', i.sampleRate ? `${i.sampleRate} Hz` : '-'],
      ['Slots rec / play', `${i.recChannels} / ${i.playChannels}`],
      ['Ring', `${i.ringFrames} frames`],
      ['Service passes', String(i.servicePasses)],
      ['Interrupts', String(i.interruptCount)],
      ['Notif. status seen', hex(i.notifStatusSeen)],
      ['Driver interface', `v${i.version}`],
    );
  }
  rows.push(['App', `Apollo Control ${appVersion}${S.demo ? ' (demo)' : ''}`]);
  dl.replaceChildren(...rows.flatMap(([k, v]) => [el('dt', { text: k }), el('dd', { text: v })]));
  const flags = i ? i.flags : 0;
  document.getElementById('devFlags').replaceChildren(
    ...FLAG_CHIPS.map(([f, label]) => el('span', { class: 'chip' + ((flags & f) ? ' on' : ''), text: label })));
  renderDeviceRate();
  renderParams();
}

/* ------------------------------------------------------------------ */
/* [Params] the selected unit's driver Parameters (app/src/params_host.h; read-only here)               */
/* ------------------------------------------------------------------ */

const paramsByUnit = new Map();         /* unit id -> the last "params" message */
const PARAM_SRC = { D: 'built-in default', U: 'this unit (Units key)', M: 'model key', G: 'legacy global', X: 'forced by Mode' };

function requestParams() {
  const id = mixKey(S.sel);
  if (S.demo || !id) { renderParams(); return; }
  send(JSON.stringify({ cmd: 'params-get', unit: id }));
}

/*
 * [Events] What the driver reported as it happened (app/src/main.cpp EventPumpMain, include/apollo_events.h): a DSP that
 * stopped answering or a lost engine is said at once; a rate or clock change asks for the new state now; parameters
 * stored for the next start refresh the Device view. Stage and leaving changes come through the status as before.
 */
function onDriverEvent(m) {
  const idx = msgUnit(m), name = unitShort(idx) || 'Apollo';
  if (m.reset) return;                                     /* the ring of a driver that just started: old news */
  if (m.kind === 'dsp-fault') {
    toast(`${name}: DSP ${m.a} stopped answering and is set aside until the ${name} is power-cycled (0x${(m.b >>> 0).toString(16).toUpperCase()}).`, 'bad', 15000);
  } else if (m.kind === 'engine-lost') {
    toast(`${name}: the audio engine lost the unit. Check the Thunderbolt cable and power.`, 'bad', 10000);
  } else if (m.kind === 'rate' || m.kind === 'clock') {
    RATE.asked = -1e9;
    rateAsk();
  } else if (m.kind === 'params' && S.view === 'device' && idx === S.sel) {
    requestParams();
  }
}

function onParams(m) {
  if (typeof m.unit === 'string') paramsByUnit.set(m.unit, m);
  if (S.view === 'device') renderParams();
}

function renderParams() {
  const box = document.getElementById('devParams');
  if (!box) return;
  const m = paramsByUnit.get(mixKey(S.sel));
  if (!m) { box.replaceChildren(el('p', { class: 'muted', text: S.demo ? 'Not in the demo.' : 'Press Refresh to read them.' })); return; }
  if (!m.present || m.error) {
    box.replaceChildren(el('p', { class: 'muted', text: m.error === 1 ? 'The installed driver predates this (no GET_PARAMS).' : `Not read: ${m.detail || `error ${m.error}`}` }));
    return;
  }
  const fmt = (e, v) => (v === null || v === undefined ? '-' : e.hex ? hex(v, 1) : String(v));
  const head = el('tr', {}, ...['Parameter', 'In force', 'From', 'Default', 'Stored'].map(t => el('th', { text: t })));
  const rows = (m.entries || []).map(e => el('tr', { class: e.pending ? 'pending' : '' },
    el('td', { text: e.name }), el('td', { class: 'num', text: fmt(e, e.value) }),
    el('td', { text: e.src, title: PARAM_SRC[e.src] || e.src }), el('td', { class: 'num', text: fmt(e, e.default) }),
    el('td', { class: 'num', text: fmt(e, e.stored) + (e.pending ? ' (next start)' : '') })));
  const pend = (m.entries || []).filter(e => e.pending).length;
  box.replaceChildren(
    el('p', { class: 'muted', text: `Units\\${m.key || '-'}${m.resolved ? '' : ' (not identified)'}. Read-only here: an elevated apollo-diag param <name> <value> stores one for the unit's next start.` }),
    el('table', { class: 'params-table' }, el('thead', {}, head), el('tbody', {}, ...rows)),
    pend ? el('p', { class: 'muted', text: `${pend} stored value(s) apply at the unit's next start.` }) : null);
}

/* ------------------------------------------------------------------ */
/* [Rate] Sample rate                                                   */
/* ------------------------------------------------------------------ */

/*
 * The host's rate-get / rate-set (app/src/rate_host.h, docs/apollo-control-units.md 1.6). Units are named by their id
 * (the "units" list's), not their slot. The UI sends {"cmd":"rate-get"} at hello, when a unit connects, when the set
 * of units changes and when a status brings another sampleRate. Each {"type":"rate","unit":<id>,...} says what the unit
 * runs at, what it offers and allows, whether it is changing, an ASIO host has it, its inserts wait for 48 kHz
 * (plugins48k), its speaker correction stopped (correctionMismatch), and whether it runs in the combined device
 * (combined.member): then one shared selector stands for every member, and rate-set goes with combined:true. A change
 * answers rate-result pending:true at once, then ok / code / why (shown as is under the selector; busy is a toast), then
 * a fresh rate per unit. The selectors offer the unit's offered rates, those not allowed disabled; they are disabled
 * while a change is under way. No answer within 1.5 s, or an apollo.sys without rate change (an error in its rate):
 * disabled at the unit's own rate, "fixed in this driver build". Each segment's head and the Device view.
 */
const RATES = [44100, 48000, 88200, 96000, 176400, 192000];
const RATE = { asked: -1e9, waited: false, byId: {}, pending: {}, notes: {}, timer: 0, seen: {} };
const kHz = r => `${(r / 1000).toFixed(1).replace(/\.0$/, '')} kHz`;

function rateReset() {
  clearTimeout(RATE.timer);
  Object.assign(RATE, { asked: -1e9, waited: false, byId: {}, pending: {}, notes: {}, timer: 0, seen: {} });
}

function rateAsk() {
  if (S.demo) { demoRates(); return; }
  if (!host || now() - RATE.asked < 400) return;
  RATE.asked = now();
  send(JSON.stringify({ cmd: 'rate-get' }));
  clearTimeout(RATE.timer);
  RATE.timer = setTimeout(() => { RATE.waited = true; renderRates(); }, 1500);
}

/* A status with another rate than the last one seen for the slot (a DAW or apollo-diag changed it): ask again. */
function rateSeen(idx, i) {
  const r = i && i.sampleRate;
  if (!r) return;
  if (RATE.seen[idx] && RATE.seen[idx] !== r) rateAsk();
  RATE.seen[idx] = r;
}

/* A slot's unit id (the units list), and the rate entry for a slot. Without a list, the one unit that answered is the x8p. */
function unitIdOf(idx) {
  const e = listedUnit(idx);
  if (e && e.id) return e.id;
  const ids = Object.keys(RATE.byId);
  return idx === 0 && !S.unitList && ids.length === 1 ? ids[0] : '';
}
const slotOfId = id => { const u = (S.unitList || []).find(x => x.id === id); return u ? u.index : (unitIdOf(0) === id ? 0 : -1); };
function rateFor(idx) { const id = unitIdOf(idx); return (id && RATE.byId[id]) || null; }

/* The combined device, from its members' rate entries: { ids, slots, rate, offered, allowed, changing, asio, mismatch } or null. */
function rateAgg() {
  const m = Object.values(RATE.byId).filter(r => r.combined && r.combined.member);
  if (!m.length) return null;
  const c = m[0].combined;
  return { ids: m.map(r => r.id), slots: m.map(r => slotOfId(r.id)).filter(i => i >= 0), rate: c.rate || m[0].rate, offered: m[0].offered || RATES,
    allowed: Array.isArray(c.allowed) ? c.allowed : [], changing: m.some(r => r.changing), asio: !!c.asio, mismatch: !!c.mismatch };
}

function onRate(m) {
  const id = String(m.unit || '');
  if (!id) return;
  if (m.present === false) delete RATE.byId[id];
  else RATE.byId[id] = { ...m, id };
  renderRates();
  Plug.renderAll();                    /* [Plug-ins] plugins48k: the inserts wait at other rates */
}

function onRateResult(m) {
  const key = m.combined ? 'agg' : String(m.unit || '');
  if (m.pending) {
    RATE.pending[key] = { rate: Number(m.rate) || 0, why: String(m.why || '') };
    delete RATE.notes[key];
  } else {
    delete RATE.pending[key];
    if (m.ok) delete RATE.notes[key];
    else if (m.code === 'busy') toastOnce(m.why || 'Another rate change is under way.', 'warn');
    else RATE.notes[key] = String(m.why || `Not changed (${m.code || 'error'}).`);
  }
  renderRates();
}

/* The scope a unit's rate is set in: 'agg' while it runs in the combined device, else its id. */
function rateKey(idx) { const r = rateFor(idx); return r && r.combined && r.combined.member ? 'agg' : (unitIdOf(idx) || `slot${idx}`); }

function rateSet(key, idx, rate) {
  const agg = key === 'agg' ? rateAgg() : null, r = agg ? null : rateFor(idx);
  const cur = agg ? agg.rate : r && r.rate;
  if (!(agg || r) || cur === rate || RATE.pending[key]) return;
  RATE.pending[key] = { rate, why: '' };
  delete RATE.notes[key];
  const msg = { cmd: 'rate-set', unit: agg ? agg.ids[0] : r.id, rate, combined: !!agg };
  if (S.demo) demoRateSet(msg);
  else send(JSON.stringify(msg));
  renderRates();
}

/* One selector: the unit's (or the combined device's) offered rates, those not allowed disabled; disabled while a
   change runs, an ASIO host has it, or the host never answered (then "fixed in this driver build"). */
function rateSelect(key, idx, label) {
  const agg = key === 'agg' ? rateAgg() : null, r = agg ? null : rateFor(idx), i = unitInfo(idx) || {};
  const s = el('select', { class: 'select rate-sel num', 'aria-label': label, dataset: { key } });
  const offered = agg ? agg.offered : r && Array.isArray(r.offered) ? r.offered : [];
  if (!(agg || (r && !r.error && offered.length))) {
    const fixed = RATE.waited || (r && r.error) || (!host && !S.demo);
    s.append(el('option', { value: '', text: `${kHz(i.sampleRate || 48000)}${fixed ? ' (fixed in this driver build)' : ' ...'}` }));
    s.disabled = true;
    s.dataset.fixed = fixed ? '1' : '0';
    s.title = r && r.detail ? r.detail : fixed ? 'The installed driver does not take a sample rate from Apollo Control.' : 'Asking the driver for its rates';
    return s;
  }
  const allowed = agg ? agg.allowed : Array.isArray(r.allowed) ? r.allowed : offered, cur = agg ? agg.rate : r.rate, p = RATE.pending[key];
  for (const v of RATES.filter(x => offered.includes(x))) {
    s.append(el('option', { value: String(v), text: kHz(v), disabled: !allowed.includes(v) && v !== cur }));
  }
  if (cur && !offered.includes(cur)) s.prepend(el('option', { value: String(cur), text: `${kHz(cur)} (now)`, disabled: true }));
  s.value = String(p ? p.rate : cur);
  const changing = agg ? agg.changing : r.changing, asio = agg ? agg.asio : r.asio;
  s.disabled = !!p || !!changing || !!asio;
  s.classList.toggle('unconfirmed', !!p || !!changing);
  s.classList.toggle('refused', !!RATE.notes[key]);
  s.title = p ? (p.why || `Changing to ${kHz(p.rate)}`) : changing ? 'A rate change is under way' : asio ? 'An ASIO host (a DAW) has it: change the rate there'
    : `Running at ${kHz(cur)}. A change pauses Windows apps playing through it while Windows re-opens them; plug-in inserts run at 48 kHz only.`;
  s.addEventListener('change', () => rateSet(key, idx, Number(s.value)));
  return s;
}

/* The lines under a selector: a refusal (the host's own words), a change under way, what the rate means now. */
function rateNotes(key, idx) {
  const agg = key === 'agg' ? rateAgg() : null, r = agg ? null : rateFor(idx), out = [];
  if (RATE.notes[key]) out.push(['bad', RATE.notes[key]]);
  if (RATE.pending[key]) out.push(['info', RATE.pending[key].why || `Changing to ${kHz(RATE.pending[key].rate)}...`]);
  if (agg && agg.asio) out.push(['info', 'A DAW has "Apollo (Avium)": change the rate there.']);
  if (agg && agg.mismatch) out.push(['bad', 'A unit runs at another rate and is muted until the rates agree.']);
  if (r && r.asio) out.push(['info', 'A DAW has this unit: change the rate there.']);
  if (r && r.correctionMismatch) out.push(['info', 'Speaker correction stopped: its profile is for another rate (Speakers, load it again).']);
  return out;
}

/* A rate control in a box (a segment's head, the shared bar, the Device view), rebuilt only when it changes. */
function renderRateIn(box, idx, place) {
  const key = rateKey(idx), shared = key === 'agg' && place === 'seg';
  const stamp = JSON.stringify([key, shared, key === 'agg' ? rateAgg() : rateFor(idx), RATE.pending[key], RATE.notes[key], RATE.waited,
    (unitInfo(idx) || {}).sampleRate, !!host]);
  if (box.dataset.stamp === stamp) return;
  box.dataset.stamp = stamp;
  if (shared) {
    const agg = rateAgg();
    box.replaceChildren(el('span', { class: 'rate-shared num', title: 'Set for the combined device above the segments: every unit of it runs at one rate',
      text: `${kHz(agg.rate)} · SHARED` }));
    return;
  }
  const label = key === 'agg' ? 'Combined device sample rate' : `${unitShort(idx)} sample rate`;
  box.replaceChildren(rateSelect(key, idx, label),
    ...rateNotes(key, idx).map(([kind, t]) => el('span', { class: 'rate-note', dataset: { kind }, role: 'status', text: t })));
}

/* The shared selector over the segments while units run combined. */
function renderRateBar() {
  const bar = document.getElementById('rateBar');
  if (!bar) return;
  const agg = rateAgg(), live = agg ? agg.slots.filter(i => segLive(i)) : [];
  bar.hidden = !agg || live.length < 1;
  if (bar.hidden) { bar.replaceChildren(); return; }
  if (!bar.firstChild) bar.append(el('span', { class: 'lbl', text: 'Rate' }), el('span', { class: 'rate-who' }), el('span', { class: 'rate-box' }));
  put(bar.querySelector('.rate-who'), `Combined device: ${joinList(agg.slots.map(i => unitShort(i)))}, one rate for all`);
  renderRateIn(bar.querySelector('.rate-box'), live[0], 'bar');
}

/* The Device view's rate panel: the combined device's selector, then one per unit outside it. */
function renderDeviceRate() {
  const box = document.getElementById('devRate');
  if (!box) return;
  const units = controlUnits().map(u => u.index), agg = rateAgg();
  const rows = [];
  if (agg) rows.push({ label: `Combined (${joinList(agg.slots.map(i => unitShort(i)))})`, idx: agg.slots[0] ?? 0 });
  for (const idx of units) if (!agg || !agg.slots.includes(idx)) rows.push({ label: unitShort(idx) || 'x8p', idx });
  const key = rows.map(r => r.label).join('|');
  if (box.dataset.rows !== key) {
    box.dataset.rows = key;
    box.replaceChildren(...rows.map(r => el('div', { class: 'set-row rate-row' }, el('span', { class: 'lbl', text: r.label }), el('span', { class: 'rate-box', dataset: { idx: String(r.idx) } }))));
  }
  for (const b of box.querySelectorAll('.rate-box')) renderRateIn(b, Number(b.dataset.idx), 'device');
}

function renderRates() {
  renderSegHeads();
  renderRateBar();
  if (S.view === 'device') renderDeviceRate();
}

/* The demo answers like the host: every rate offered, the combined device (both units) allows the four it runs cleanly,
   a change is pending for a moment and then done; the x8p's inserts wait away from 48 kHz. */
function demoEntry(id, rate, member) {
  return { id, unit: id, present: true, rate, locked: true, changing: false, offered: RATES.slice(), allowed: RATES.slice(), stored: rate,
    asio: false, streams: false, plugins48k: rate !== 48000, correctionMismatch: false, stripsRebuilt: false,
    combined: { member, rate, allowed: member ? [44100, 48000, 88200, 96000] : [], units: member ? 2 : 0, stored: rate, asio: false, mismatch: false },
    last: { code: '', why: '', ms: 0, changes: 0 } };
}
function demoRates() {
  RATE.waited = false;
  const here = controlUnits();
  const ids = new Set(here.map(u => u.id || `demo${u.index}`));
  for (const id of Object.keys(RATE.byId)) if (!ids.has(id)) delete RATE.byId[id];
  const both = here.length > 1;
  for (const u of here) {
    const id = u.id || `demo${u.index}`, old = RATE.byId[id];
    RATE.byId[id] = demoEntry(id, old ? old.rate : DEMO_INFO.sampleRate || 48000, both);
  }
  renderRates();
}
function demoRateSet(msg) {
  const key = msg.combined ? 'agg' : msg.unit;
  setTimeout(() => onRateResult({ type: 'rate-result', ...msg, pending: true, why: `Changing to ${kHz(msg.rate)}` }), 0);
  setTimeout(() => {
    if (!S.demo) return;
    const ids = msg.combined ? Object.keys(RATE.byId) : [msg.unit];
    for (const id of ids) if (RATE.byId[id]) RATE.byId[id] = demoEntry(id, msg.rate, RATE.byId[id].combined.member);
    DEMO_INFO.sampleRate = msg.rate;
    delete RATE.pending[key];
    onRateResult({ type: 'rate-result', ...msg, pending: false, ok: true, code: '', why: `${kHz(msg.rate)}, locked` });
    Plug.renderAll();
  }, 500);
}

/* ------------------------------------------------------------------ */
/* Settings view                                                       */
/* ------------------------------------------------------------------ */

function checkbox(checked, onChange, aria) {
  return el('input', { type: 'checkbox', checked: !!checked, 'aria-label': aria, onchange: e => onChange(e.target.checked) });
}

function selectBox(options, value, onChange, aria) {
  const s = el('select', { class: 'select', 'aria-label': aria, onchange: e => onChange(e.target.value) },
    options.map(([v, label]) => el('option', { value: String(v), text: label })));
  s.value = String(value);
  return s;
}

function setRow(title, desc, control) {
  return el('div', { class: 'set-row' },
    el('div', { class: 'set-label' }, el('span', { text: title }), desc ? el('span', { class: 'desc', text: desc }) : null),
    control);
}

function renderSettings() {
  const root = document.getElementById('settings');
  if (!root) return;
  const keepScroll = document.getElementById('view-settings').scrollTop;

  // Inputs
  const tbody = el('tbody');
  cfg.channels.forEach((c, ch) => {
    const nameIn = el('input', {
      class: 'text-in', type: 'text', maxlength: '24', value: c.name, placeholder: `Input ${ch + 1}`, 'aria-label': `Input ${ch + 1} name`,
      onchange: e => { c.name = e.target.value.trim().slice(0, 24); saveConfig(); renderInput(ch, 0); buildBridge(); },
    });
    const swatches = el('div', { class: 'swatches', role: 'group', 'aria-label': `Input ${ch + 1} color` },
      [['', 'none'], ...Object.keys(STRIP_COLORS).map(k => [k, k])].map(([key, label]) => {
        const b = el('button', {
          class: 'swatch' + (key ? '' : ' none'), title: label, 'aria-label': `${label}`, 'aria-pressed': c.color === key ? 'true' : 'false',
          style: key ? `--sw:${STRIP_COLORS[key]}` : null,
          onclick: () => { c.color = key; saveConfig(); renderInput(ch, 0); renderSettings(); },
        });
        return b;
      }));
    const show = checkbox(!c.hidden, v => { c.hidden = !v; saveConfig(); renderAllInputs(); }, `Show input ${ch + 1}`);
    tbody.append(el('tr', {}, el('td', { text: String(ch + 1) }), el('td', {}, nameIn), el('td', {}, swatches), el('td', {}, show)));
  });
  /* These are the x8p's; another unit's names are edited on its strips (cfg.unitPrefs). */
  const others = controlUnits().filter(u => u.index !== 0).map(u => capsOf(u.deviceType).short);
  const inputsPanel = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: others.length ? 'x8p inputs' : 'Inputs' }),
    others.length ? el('span', { class: 'desc', text: `${joinList(others)} input names: double-click a name on the Control view.` }) : null,
    el('table', { class: 'chan-table' },
      el('thead', {}, el('tr', {}, el('th', { text: '#' }), el('th', { text: 'Name' }), el('th', { text: 'Color' }), el('th', { text: 'Show' }))),
      tbody));

  // Safety
  const limitVal = el('span', { class: 'num', text: `${cfg.safety.monitorLimitDb} dB` });
  const limit = el('input', {
    type: 'range', min: '-40', max: '0', step: '1', value: String(cfg.safety.monitorLimitDb), 'aria-label': 'Monitor level limit',
    oninput: e => { cfg.safety.monitorLimitDb = Number(e.target.value); limitVal.textContent = `${e.target.value} dB`; renderMonitor(); saveConfig(); },
  });
  const safety = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'Safety' }),
    setRow('Ask before turning on 48V', 'Phantom power can damage ribbon mics.',
      checkbox(cfg.safety.confirm48v, v => { cfg.safety.confirm48v = v; saveConfig(); }, 'Ask before turning on 48V')),
    setRow('Monitor level limit', 'The monitor knob in this app stops here. The knob on the unit is not limited.',
      el('div', { class: 'range-wrap' }, limit, limitVal)),
    setRow('Lock gain knobs', 'Blocks gain changes from this app. The unit\'s own gain knob still works.',
      checkbox(cfg.safety.lockGain, v => { cfg.safety.lockGain = v; saveConfig(); renderAllInputs(); }, 'Lock gain knobs')));

  // Meters
  const meters = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'Meters' }),
    setRow('Peak hold', null, selectBox([[0, 'Off'], [1000, '1 second'], [2000, '2 seconds'], [5000, '5 seconds'], [-1, 'Until clicked']],
      cfg.meters.holdMs, v => { cfg.meters.holdMs = Number(v); METERS.forEach(m => m.reset()); saveConfig(); }, 'Peak hold')),
    setRow('Range', 'The bottom of the meter scale.', selectBox([[-48, '-48 dBFS'], [-60, '-60 dBFS'], [-90, '-90 dBFS']],
      cfg.meters.floorDb, v => { cfg.meters.floorDb = Number(v); METERS.forEach(m => m.applyFloor()); saveConfig(); }, 'Meter range')),
    setRow('Release', 'How fast a meter falls after a peak.', selectBox([[10, 'Slow, 10 dB/s'], [20, 'Normal, 20 dB/s'], [40, 'Fast, 40 dB/s']],
      cfg.meters.releaseDbPerSec, v => { cfg.meters.releaseDbPerSec = Number(v); saveConfig(); }, 'Meter release')));

  // Appearance: the looks (themes.css), each item drawn in its own look as a preview
  const look = cfg.appearance.look;
  const items = LOOKS.map(L => el('button', {
    class: 'look-item', role: 'radio', 'aria-checked': look === L.id ? 'true' : 'false', tabindex: look === L.id ? '0' : '-1',
    'aria-label': `${L.name}: ${L.desc}`, dataset: { look: L.id }, onclick: () => setLook(L.id),
  },
  el('span', { class: 'look-sw', 'aria-hidden': 'true' },
    ['bg', 'panel', 'accent', 'ok', 'warn', 'red'].map(t => el('span', { class: `ls ls-${t}` }))),
  el('span', { class: 'look-name', text: L.name }),
  el('span', { class: 'look-desc', text: L.desc }),
  el('span', { class: 'look-num num', 'aria-hidden': 'true', text: '-12.5 dB' })));
  const lookList = el('div', { class: 'look-list', role: 'radiogroup', 'aria-label': 'Look' }, items);
  lookList.addEventListener('keydown', e => {
    const i = LOOKS.findIndex(L => L.id === cfg.appearance.look);
    const d = e.key === 'ArrowDown' || e.key === 'ArrowRight' ? 1 : e.key === 'ArrowUp' || e.key === 'ArrowLeft' ? -1 : 0;
    if (!d) return;
    e.preventDefault();
    const next = LOOKS[(i + d + LOOKS.length) % LOOKS.length].id;
    setLook(next);
    const b = document.querySelector(`.look-item[data-look="${next}"]`);
    if (b) b.focus();
  });
  const accents = el('div', { class: 'swatches', role: 'group', 'aria-label': 'Accent color' },
    el('button', { class: 'swatch swatch-look', title: "The look's own accent", 'aria-label': 'look', 'aria-pressed': cfg.appearance.accent === 'look' ? 'true' : 'false',
      onclick: () => { cfg.appearance.accent = 'look'; applyAppearance(); saveConfig(); renderSettings(); } }, 'L'),
    Object.entries(ACCENTS).map(([k, [c]]) => el('button', {
      class: 'swatch', title: k, 'aria-label': k, 'aria-pressed': cfg.appearance.accent === k ? 'true' : 'false', style: `--sw:${c}`,
      onclick: () => { cfg.appearance.accent = k; applyAppearance(); saveConfig(); renderSettings(); },
    })));
  const appearance = el('div', { class: 'panel panel-wide' },
    el('div', { class: 'sect-title', text: 'Appearance' }),
    el('span', { class: 'desc', text: 'Each theme is its own design language: its knobs, faders, buttons, meters, plug-in browser and how they respond. ' +
      'Every screen follows, nothing is lost when switching. Ctrl+Shift+T steps through them.' }),
    lookList,
    el('div', {}, el('button', { class: 'btn', onclick: () => showView('styleguide') }, 'Open the style guide')),
    setRow('Day when Windows is light', 'Uses the Day look while Windows is in light mode, your look otherwise.',
      checkbox(cfg.appearance.followSystem, v => { cfg.appearance.followSystem = v; applyAppearance(); saveConfig(); }, 'Day when Windows is light')),
    setRow('Accent', "L: the look's own. A colour here replaces it in every look.", accents),
    setRow('Scanlines', 'Arcade only: a CRT line overlay. Off by default.',
      checkbox(cfg.appearance.scanlines, v => { cfg.appearance.scanlines = v; applyAppearance(); saveConfig(); }, 'Scanlines in the Arcade look')),
    setRow('Size', null, selectBox([[90, '90%'], [100, '100%'], [110, '110%'], [125, '125%']], cfg.appearance.scale,
      v => { cfg.appearance.scale = Number(v); applyAppearance(); saveConfig(); }, 'Interface size')));

  // Window: [Sess] Start with Windows (the HKCU Run value, as Windows has it), close to the tray
  const auto = Sess.autostart();
  const autoBox = checkbox(auto.on, v => Sess.setAutostart(v), 'Start Apollo Control when I sign in');
  autoBox.disabled = !host || S.capture || (!auto.known && !!host);
  const autoDesc = !host ? 'The desktop app only.'
    : !auto.known ? 'Asking Windows...'
    : auto.on && !auto.current ? 'On, but the entry starts another copy of Apollo Control: turn it off and on again to start this one.'
    : 'Starts minimized to the tray icon when you sign in to Windows. For your Windows account only: no administrator rights, nothing installed.';
  const windowPanel = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'Window and tray' }),
    setRow('Start Apollo Control when I sign in', autoDesc, autoBox),
    setRow('Close to the tray', `Closing the window keeps Apollo Control running in the notification area. Its menu: open, monitor volume up and down, mute, dim, recent sessions, quit.${cfg.windowPrefs.closeToTraySet ? '' : ' On by default while it starts with Windows.'}`,
      checkbox(Sess.closeToTray(), v => { cfg.windowPrefs.closeToTray = v; cfg.windowPrefs.closeToTraySet = true; sendPrefs(); saveConfig(); renderSettings(); }, 'Close to the tray')),
    host ? null : el('span', { class: 'desc', text: 'These apply to the desktop app only.' }));

  // Presets
  const list = el('div', { class: 'preset-list' });
  if (!cfg.presets.length) list.append(el('span', { class: 'desc', text: 'No presets yet.' }));
  cfg.presets.forEach((p, idx) => {
    list.append(el('div', { class: 'preset-item' },
      el('div', {}, el('div', { text: p.name }), el('div', { class: 'when', text: p.saved ? `Saved ${new Date(p.saved).toLocaleString()}` : '' })),
      el('div', { class: 'preset-actions' },
        el('button', { class: 'btn', onclick: () => recallPreset(p) }, 'Recall'),
        el('button', { class: 'btn', onclick: () => renamePreset(idx) }, 'Rename'),
        el('button', { class: 'btn', onclick: () => deletePreset(idx) }, 'Delete'))));
  });
  const presets = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'Presets' }),
    el('span', { class: 'desc', text: 'A preset stores every preamp setting, the links, and monitor routing (source, headphone sources, dim level, mono). Monitor and headphone levels are left where they are.' }),
    list,
    el('div', {}, el('button', { class: 'btn btn-primary', onclick: savePreset }, 'Save current as preset')));

  // Keyboard
  const keys = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'Keyboard' }),
    el('div', { class: 'keys' },
      el('kbd', { text: 'M' }), el('span', { text: 'Mute monitor' }),
      el('kbd', { text: 'D' }), el('span', { text: 'Dim monitor' }),
      el('kbd', { text: 'O' }), el('span', { text: 'Monitor mono' }),
      el('kbd', { text: `Ctrl+1..${VIEWS.length}` }), el('span', { text: VIEW_NAMES }),
      el('kbd', { text: 'Ctrl+Shift+T' }), el('span', { text: 'Next look' }),
      el('kbd', { text: 'Ctrl+S' }), el('span', { text: 'Save the session (Save as when it has no name yet)' }),
      el('kbd', { text: 'Ctrl+O' }), el('span', { text: 'Sessions: the list, load, delete, export, import' }),
      el('kbd', { text: 'Ctrl+K' }), el('span', { text: 'Command palette (every action, sessions too)' }),
      el('kbd', { text: 'Arrows' }), el('span', { text: 'Focused knob: one step. Fader: 0.5 dB. Page Up/Down: 6 dB. Enter: type a value.' }),
      el('kbd', { text: 'Shift+drag' }), el('span', { text: 'Fine knob adjustment (faders: Ctrl, Alt or Shift). Double-click a fader: 0 dB.' }),
      el('kbd', { text: 'Ctrl+F' }), el('span', { text: 'Search plug-ins (Control view)' }),
      el('kbd', { text: 'Enter' }), el('span', { text: 'On a plug-in: pick it up, then Enter on a lit insert slot. On a loaded slot: its parameters.' }),
      el('kbd', { text: 'M / B / Del' }), el('span', { text: 'On a loaded insert slot: move it, bypass it, remove it.' }),
      el('kbd', { text: 'Esc' }), el('span', { text: 'Cancel placing a plug-in; close its parameters.' })));

  // About
  const about = el('div', { class: 'panel' },
    el('div', { class: 'sect-title', text: 'About' }),
    el('span', { class: 'desc', text: `Apollo Control ${appVersion}, for the apollo.sys open driver. Settings are saved in %APPDATA%\\Avium\\ApolloControl\\config.json.` }),
    el('div', {}, el('button', { class: 'btn', onclick: resetSettings }, 'Reset all settings')));

  root.replaceChildren(inputsPanel, safety, Plug.settingsPanel(), Endpoints.panel(), meters, appearance, windowPanel, presets, keys, about);
  Endpoints.ask();                     /* [Endpoints] Windows audio: the host's rows (at most every 3 s) */
  document.getElementById('view-settings').scrollTop = keepScroll;
}

async function resetSettings() {
  const ok = await dialog({ title: 'Reset all settings?', body: 'Names, colors, safety options, appearance and presets go back to defaults. The x8p itself is not changed.', confirm: 'Reset', kind: 'danger' });
  if (!ok) return;
  cfg = defaultConfig();
  applyConfig();
  sendPrefs();
  saveConfig();
}

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/* ------------------------------------------------------------------ */

/* The selected unit's setup: only the inputs its model has (model: its device type). */
function captureState() {
  return {
    model: unitType(S.sel),
    inputs: S.inputs.slice(0, caps().preamps).map(i => ({ gain: i.gain, line: i.line, pad: i.pad, p48: i.p48, lowcut: i.lowcut, phase: i.phase })),
    links: [...S.links],
    mon: { source: S.mon.source, hp1src: S.mon.hp1src, hp2src: S.mon.hp2src, dimLevel: S.mon.dimLevel, mono: S.mon.mono },
  };
}

async function savePreset() {
  closeMenu();
  const name = await dialog({ title: 'Save preset', body: 'Name this setup.', confirm: 'Save', input: `Preset ${cfg.presets.length + 1}` });
  if (!name) return;
  const existing = cfg.presets.findIndex(p => p.name === name);
  const entry = { name, saved: Date.now(), state: captureState() };
  if (existing >= 0) {
    const ok = await dialog({ title: `Replace "${name}"?`, body: 'A preset with this name exists.', confirm: 'Replace' });
    if (!ok) return;
    cfg.presets[existing] = entry;
  } else {
    cfg.presets.push(entry);
  }
  saveConfig();
  renderSettings();
  buildPresetMenu();
  toast(`Saved preset "${name}".`);
}

async function renamePreset(idx) {
  const p = cfg.presets[idx];
  const name = await dialog({ title: 'Rename preset', body: '', confirm: 'Rename', input: p.name });
  if (!name) return;
  p.name = name;
  saveConfig();
  renderSettings();
  buildPresetMenu();
}

async function deletePreset(idx) {
  const p = cfg.presets[idx];
  const ok = await dialog({ title: `Delete "${p.name}"?`, body: 'This cannot be undone.', confirm: 'Delete', kind: 'danger' });
  if (!ok) return;
  cfg.presets.splice(idx, 1);
  saveConfig();
  renderSettings();
  buildPresetMenu();
}

/* Recalled on the selected unit: the inputs, pairs and monitor controls its model has (an x8p
   preset on the Twin X sets inputs 1-2 and link 1-2; the rest of the preset is left out). */
async function recallPreset(p) {
  closeMenu();
  const idx = S.sel, c = cur(), k = caps();
  if (!S.demo && !connected(idx)) { toast(`Connect the ${unitShort(idx)} before recalling a preset.`, 'warn'); return; }
  if (!writable()) return;
  const st = p.state;
  /* a pair the preset leaves linked (or links) is one control: both inputs take its first input's values */
  const linkAfter = pair => !!((k.linkPairs >>> pair) & 1) &&
    (Array.isArray(st.links) && st.links[pair] !== undefined ? !!st.links[pair] : c.links[pair]);
  const raw = st.inputs.slice(0, k.preamps);
  const ins = raw.map((x, i) => ((i & 1) && linkAfter(i >> 1) && raw[i - 1] ? raw[i - 1] : x));
  const turnOn = ins.map((x, i) => (x.p48 && !c.inputs[i].p48 ? i : -1)).filter(i => i >= 0);
  if (turnOn.length && cfg.safety.confirm48v && !(await confirm48v(turnOn))) return;
  if (S.sel !== idx) return;                   /* the unit changed under the dialog: nothing sent */
  (Array.isArray(st.links) ? st.links : []).forEach((on, pair) => {
    if (!((k.linkPairs >>> pair) & 1) || c.links[pair] === !!on) return;
    markPending(c, `link:${pair}`, !!on, c.links[pair]);
    c.links[pair] = !!on;
    c.refused.delete(`link:${pair}`);
    hwSend(idx, 'input', pair * 2, 'link', on ? 1 : 0);
  });
  ins.forEach((x, ch) => {
    for (const key of ['line', 'pad', 'lowcut', 'phase', 'gain', '48v']) {
      const v = x[INPUT_FIELDS[key]];
      if (v !== undefined) setInput(ch, key, key === 'gain' ? clamp(Math.round(v), 10, 65) : !!v, { noLink: true });
    }
  });
  if (st.mon) {
    if (st.mon.source !== undefined) setMon('source', clamp(st.mon.source | 0, 0, 4));     /* [Monitor] per model in setMon */
    if (st.mon.hp1src !== undefined) setMon('hp1src', clamp(st.mon.hp1src | 0, 0, 1));
    if (st.mon.hp2src !== undefined) setMon('hp2src', clamp(st.mon.hp2src | 0, 0, 1));
    if (st.mon.dimLevel !== undefined) setMon('dimlevel', clamp(st.mon.dimLevel | 0, 1, 7));
    if (st.mon.mono !== undefined) setMon('mono', !!st.mon.mono);
  }
  renderAllInputs();
  toast(`Recalled "${p.name}".`);
}

function buildPresetMenu() {
  const menu = document.getElementById('presetMenu');
  const items = cfg.presets.length
    ? cfg.presets.map(p => el('button', { role: 'menuitem', onclick: () => recallPreset(p) }, el('span', { text: p.name }), el('span', { class: 'muted', text: 'Recall' })))
    : [el('div', { class: 'empty', text: 'No presets yet' })];
  menu.replaceChildren(...items, el('hr'),
    el('button', { role: 'menuitem', onclick: savePreset }, 'Save current as preset'),
    el('button', { role: 'menuitem', onclick: () => { closeMenu(); Cmd.pickScript(); } }, 'Run a command script...'),
    el('button', { role: 'menuitem', onclick: () => { closeMenu(); showView('settings'); } }, 'Manage presets'));
}

function closeMenu() {
  document.getElementById('presetMenu').hidden = true;
  document.getElementById('presetBtn').setAttribute('aria-expanded', 'false');
}

/* ------------------------------------------------------------------ */
/* Status, banner, tabs, appearance                                    */
/* ------------------------------------------------------------------ */

/* The top bar and the banner describe the unit the Control view drives (S.sel). */
function renderStatus() {
  const idx = S.sel, i = unitInfo(idx), name = unitShort(idx), ro = unitReadOnly(idx);
  const conn = document.getElementById('conn');
  let state = 'idle', text;
  if (S.demo) { state = 'demo'; text = `Demo: a simulated ${name}, nothing reaches hardware`; }
  else if (idx === 0 && !S.host.present) { state = S.host.error === 5 ? 'bad' : 'idle'; text = S.host.error === 5 ? 'The driver refused access' : 'No Apollo on the open driver'; }
  else if (!i) { state = 'busy'; text = `Waiting for the ${name}'s status`; }
  else if (i.stage === STAGE.GONE) { state = 'bad'; text = `${name} removed or powered off`; }
  else if (i.stage === STAGE.FAILED) { state = 'bad'; text = `Bring-up failed at ${STAGE_NAMES[i.failedStage] || i.failedStage}`; }
  else if (ro) { state = 'idle'; text = `${i.serial || name} · ${ro} mode, read-only`; }
  else if (i.stage < STAGE.CONNECTED) { state = 'busy'; text = `Starting up: ${STAGE_NAMES[i.stage] || i.stage}`; }
  else {
    state = 'ok';
    text = `${i.serial || name} · ${(i.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz · clock ${(i.flags & FLAG.TRANSPORT) ? 'running' : 'stopped'}`;
  }
  conn.dataset.state = state;
  document.getElementById('connText').textContent = text;
  document.body.dataset.live = !ro && (S.demo || connected(idx)) ? '1' : '0';
  renderSegHeads();                    /* each segment's clock, rate and live state */
  document.getElementById('brandModel').textContent = DEVICE_NAMES[(i && i.deviceType) || unitType(idx)] || 'Apollo x8p';

  renderBanner();
  if (S.view === 'device') renderDevice();
  renderClockState();
  renderUnits();
}

/* Meters without a running clock are hatched: the Control view's follow the selected unit, the
   Channels view's (the x8p's record slots) the x8p. */
function renderClockState() {
  const i = info(), x8p = ctl(0);
  for (const m of METERS) {
    const c = m.group === 'control' ? S.ctl[m.unit] : x8p;    /* Control: each segment's unit; Channels: the x8p */
    m.setNoClock(!(c && c.levelsValid));
  }
  document.getElementById('bridgeNote').textContent = !x8p.levelsValid
    ? 'The audio clock is stopped, so there is nothing to meter. Phase 2 starts it with the driver; until then run apollo-diag start in an admin prompt.'
    : `${i ? i.recChannels : 32} slots at ${i ? i.sampleRate : 48000} Hz, peak over the last 21 ms`;
}

function renderBanner() {
  const banner = document.getElementById('banner');
  const text = document.getElementById('bannerText');
  const actions = document.getElementById('bannerActions');
  const idx = S.sel, i = unitInfo(idx), name = unitShort(idx), c = cur(), ro = unitReadOnly(idx);
  let kind = null, msg = '', btns = [];
  const btn = (label, fn, cls = 'btn') => el('button', { class: cls, onclick: fn }, label);

  if (S.demo) {
    kind = 'demo';
    msg = `Demo mode. Controls drive a simulated ${name} so you can try the app. Nothing is sent to hardware.`;
    if (S.host.present) btns.push(btn('Use the x8p', exitDemo, 'btn btn-primary'));
    else if (host) btns.push(btn('Leave demo', exitDemo));
  } else if (idx === 0 && !S.host.present) {
    kind = 'warn';
    msg = S.host.error === 5
      ? 'The installed apollo.sys only lets administrators connect. Install the current build (tools\\phase1\\install.ps1) and this app can control the unit.'
      : 'apollo.sys is not running on the x8p yet. The controls come alive once it is installed (tools\\phase1\\install.ps1).';
    btns.push(btn('Try the demo', enterDemo));
  } else if (!i) {
    kind = 'warn';
    msg = `Waiting for the ${name} to report its status.`;
  } else if (i.stage === STAGE.FAILED) {
    kind = 'bad';
    msg = `Bring-up stopped at ${STAGE_NAMES[i.failedStage] || i.failedStage} (status ${hex(i.lastStatus)}). The driver log has the details.`;
    btns.push(btn('Open the log', () => { showView('device'); requestLog(); }));
  } else if (i.stage === STAGE.GONE) {
    kind = 'bad';
    msg = `The ${name} dropped off the bus. Check the Thunderbolt cable and power; the app reconnects by itself.`;
  } else if (ro) {
    kind = 'warn';
    msg = `The ${name} is in ${ro} mode, so it is read-only: Apollo Control sends it no setting until the driver runs it in full (Mode 2).`;
  } else if (i.stage < STAGE.CONNECTED) {
    kind = 'warn';
    msg = `The ${name} is starting up (${STAGE_NAMES[i.stage] || i.stage}). This can take up to a minute after power-on.`;
  } else if (!(i.flags & FLAG.MIXER_READY)) {
    kind = 'warn';
    msg = 'Connected, but the mixer is not ready yet, so controls cannot be sent.';
  } else if (i.flags & FLAG.ENGINE_LOST) {
    kind = 'bad';
    msg = `The audio engine stopped: the ${name} is not answering. Restart the transport or replug the unit.`;
    btns.push(btn('Open Status', () => showView('status')));
  } else if (i.flags & FLAG.FW_UNTESTED) {
    const fw = i.fwRelease >>> 0;
    kind = 'warn';
    msg = `This ${name} runs firmware ${fw >>> 24}.${(fw >>> 16) & 0xFF}.${fw & 0xFFFF}, not the ${TESTED_FW} the driver was tested on. ` +
      'Something may not work; do not accept firmware updates from UA Connect.';
  } else if (!c.rbValid && now() - c.connectedAt > 4000) {
    kind = 'warn';
    msg = 'The unit is not sending readback. Front-panel changes will not show here and changes cannot be confirmed (grey dots).';
  }
  const key = `${kind}|${msg}|${btns.map(b => b.textContent).join(',')}`;
  if (key === lastBannerKey) return;           /* called often: do not rebuild buttons under the cursor */
  lastBannerKey = key;
  banner.hidden = !kind;
  if (!kind) return;
  banner.dataset.kind = kind;
  text.textContent = msg;
  actions.replaceChildren(...btns);
}
let lastBannerKey = '';

function showView(v) {
  const was = S.view;
  /* --view control:<twinx|x8p|slot> opens the Control view on that unit (until the user picks one). */
  const pin = /^control:(\w+)$/i.exec(String(v || ''));
  if (pin) { S.pin = pin[1].toLowerCase(); v = 'control'; syncUnit(); }
  /* [Plug-ins] the old Plug-ins tab: the browser on the Control view now */
  if (v === 'plugins') { v = 'control'; setTimeout(() => Plug.focusSearch(), 0); }
  if (!VIEWS.includes(v) && v !== 'styleguide') v = 'control';
  S.view = v;
  for (const name of VIEWS) {
    document.getElementById(`view-${name}`).hidden = name !== v;
    const tab = document.getElementById(`tab-${name}`);
    tab.setAttribute('aria-selected', name === v ? 'true' : 'false');
    tab.tabIndex = name === v ? 0 : -1;
  }
  document.getElementById('view-styleguide').hidden = v !== 'styleguide';
  if (v === 'styleguide') StyleGuide.show();      /* the style guide: every control in every state (styleguide.js) */
  if (was !== v) Skin.on('view', document.querySelector('main'));
  if (v === 'status') renderUnits();
  if (v === 'cues') renderCues();      /* [Monitor] */
  if (v === 'control') Plug.onShow();  /* [Plug-ins] the browser and slots: the host refreshes while shown */
  else if (was === 'control') Plug.onHide();
  if (v === 'units') Units.render();   /* units.js */
  if (v === 'correction') Correction.show();   /* [Corr] correction.js */
  if (v === 'device') { renderDevice(); requestLog(); }
  if (v === 'settings') renderSettings();
  if (cfg.view !== v && v !== 'styleguide') { cfg.view = v; saveConfig(); }
}

/* The look on screen: the chosen one, or Day while Windows is light and the user asked for that. */
function currentLook() {
  const a = cfg.appearance, id = LOOKS.some(l => l.id === a.look) ? a.look : 'studio';
  return a.followSystem && !window.matchMedia('(prefers-color-scheme: dark)').matches ? 'day' : id;
}

function applyAppearance() {
  const rootEl = document.documentElement, id = currentLook(), L = LOOKS.find(l => l.id === id) || LOOKS[0];
  rootEl.dataset.look = id;
  rootEl.dataset.theme = L.mode;
  rootEl.dataset.scan = id === 'arcade' && cfg.appearance.scanlines ? '1' : '0';
  const acc = ACCENTS[cfg.appearance.accent];
  if (acc) { rootEl.style.setProperty('--accent', acc[0]); rootEl.style.setProperty('--accent-ink', acc[1]); }
  else { rootEl.style.removeProperty('--accent'); rootEl.style.removeProperty('--accent-ink'); }
  const cs = getComputedStyle(rootEl);
  send('theme', L.mode, cs.getPropertyValue('--panel').trim(), cs.getPropertyValue('--text').trim());
  /* every control takes this theme's view and interaction (skin.js): live and lossless */
  if (Skin.use(id)) {
    if (typeof Plug !== 'undefined') Plug.renderAll();
    StyleGuide.onTheme();
  }
  /* layer 2, composition: the pack's arrangement of the Control view (console, hud, panels, rows, focus, cards) */
  const vc = document.getElementById('view-control');
  if (vc) vc.dataset.comp = (Skin.manifest().layout || {}).composition || 'console';
  for (const sg of segs.values()) renderSegHead(sg.idx);
  Sess.onTheme();                      /* [Sess] the session list in this theme's layout */
  const z = cfg.appearance.scale / 100;
  if (host) send('zoom', z);
  else document.body.style.zoom = String(z);
}

function setLook(id) {
  if (!LOOKS.some(l => l.id === id)) return;
  cfg.appearance.look = id;
  applyAppearance();
  saveConfig();
  if (S.view === 'settings') renderSettings();
}

/* Ctrl+Shift+T: the next look. */
function cycleLook() {
  const i = LOOKS.findIndex(l => l.id === cfg.appearance.look);
  const next = LOOKS[(i + 1) % LOOKS.length];
  setLook(next.id);
  toast(`Look: ${next.name}${cfg.appearance.followSystem && currentLook() !== next.id ? ' (Day while Windows is light)' : ''}`);
}

/* [Sess] the window's close button: to the tray or quit. The second field is informational (the host writes the Run value
   only on autostart-set). */
function sendPrefs() {
  if (S.capture) return;
  send('prefs', Sess.closeToTray() ? 1 : 0, Sess.autostart().on ? 1 : 0);
}

function applyConfig() {
  applyAppearance();
  METERS.forEach(m => m.applyFloor());
  syncUnit();                          /* the remembered unit (cfg.controlUnit), once it is listed */
  renderAllInputs();
  renderMonitor();
  renderCues();
  buildBridge();
  buildPresetMenu();
  setAuxOpen(cfg.auxStrips.open);      /* [Ins] the AUX strips as saved */
  for (const idx of segs.keys()) Aux.render(idx);
  if (S.view === 'settings') renderSettings();
}

/* ------------------------------------------------------------------ */
/* Animation loop: meters                                              */
/* ------------------------------------------------------------------ */

let lastFrame = 0;
let lastText = 0;
let lastDevice = 0;
function frame(t) {
  const dt = lastFrame ? Math.min(0.1, (t - lastFrame) / 1000) : 0;
  lastFrame = t;
  const x8p = ctl(0);
  if (S.view === 'control') drawMeterBridge(dt);
  Sig.frame(dt, t);                    /* [Themes] the signature features (signatures.js) */
  for (const m of METERS) {
    if (m.group !== S.view) continue;
    const c = m.group === 'control' ? S.ctl[m.unit] : x8p;    /* Control: each segment's unit; Channels: the x8p */
    m.update(c && c.levelsValid ? c.peaks[m.slot] : -Infinity, dt, t);
  }
  if (t - lastText > 100) {
    lastText = t;
    if (S.view === 'control') {
      const held = mt => peakText(mt.hold > -Infinity ? mt.hold : mt.cur);
      for (const sg of segs.values()) {
        if (sg.gone) continue;
        const panels = document.getElementById('view-control').dataset.comp === 'panels';
        for (const s of sg.strips) {
          const txt = held(s.meter);
          if (txt !== s.lastPeak) { s.peak.textContent = txt; s.lastPeak = txt; }
          s.peak.classList.toggle('clip', s.meter.clip);
          if (panels) {                  /* [Themes] Lab: peak, average and crest factor, numbers first */
            /* crest factor: the recent peak (the meter's 3 s ghost) over the 300 ms average, never below 0 */
            const pk = s.meter.cur, av = s.meter.avg, gh = Math.max(s.meter.ghost, pk), cf = gh > -200 && av > -200 ? Math.max(0, gh - av) : NaN;
            const st = `PK ${peakText(pk)}\nAVG ${peakText(av)}\nCF ${Number.isFinite(cf) ? cf.toFixed(1) : '--'}`;
            if (st !== s.lastStats) { s.stats.textContent = st; s.lastStats = st; }
          }
        }
        const mo = sg.master;          /* [Mix] main out L/R: the monitor-mix loopback */
        if (mo) {
          const l = held(mo.mL), r = held(mo.mR);
          if (l !== mo.lastL) { mo.pL.textContent = l; mo.lastL = l; }
          if (r !== mo.lastR) { mo.pR.textContent = r; mo.lastR = r; }
          mo.pL.classList.toggle('clip', mo.mL.clip);
          mo.pR.classList.toggle('clip', mo.mR.clip);
        }
      }
    } else if (S.view === 'channels') {
      for (const c of bridgeCells) {
        const txt = peakText(c.meter.hold > -Infinity ? c.meter.hold : c.meter.cur);
        if (txt !== c.last) { c.db.textContent = txt; c.last = txt; }
      }
    }
    if (!S.demo && unitPresent(S.sel)) renderBanner();
  }
  if (S.view === 'device' && S.demo && t - lastDevice > 1000) { lastDevice = t; renderDevice(); }
  requestAnimationFrame(frame);
}

/* ------------------------------------------------------------------ */
/* Keyboard, tabs, menus                                               */
/* ------------------------------------------------------------------ */

function wireChrome() {
  for (const b of document.querySelectorAll('.tabs button')) b.addEventListener('click', () => showView(b.dataset.view));
  document.querySelector('.tabs').addEventListener('keydown', e => {
    const i = VIEWS.indexOf(S.view);
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') {
      const next = VIEWS[(i + (e.key === 'ArrowRight' ? 1 : VIEWS.length - 1)) % VIEWS.length];
      showView(next);
      document.getElementById(`tab-${next}`).focus();
      e.preventDefault();
    }
  });
  const btn = document.getElementById('presetBtn');
  btn.addEventListener('click', e => {
    const menu = document.getElementById('presetMenu');
    const open = menu.hidden;
    menu.hidden = !open;
    btn.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) { const first = menu.querySelector('button'); if (first) first.focus(); }
    e.stopPropagation();
  });
  document.addEventListener('click', e => { if (!e.target.closest('.menu-wrap')) closeMenu(); });

  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      closeMenu();
      if (!document.getElementById('dlg').open && S.view === 'control' && Plug.escape()) { e.preventDefault(); return; }
    }
    if (document.getElementById('dlg').open) return;
    if (e.ctrlKey && e.shiftKey && !e.altKey && e.key.toLowerCase() === 't') { cycleLook(); e.preventDefault(); return; }
    if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'k') { Cmd.palette(); e.preventDefault(); return; }   /* every theme */
    /* [Sess] Ctrl+S saves the session (Save as without a name), Ctrl+O opens the session list: every theme */
    if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 's') { Sess.quickSave(); e.preventDefault(); return; }
    if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'o') { Sess.open(); e.preventDefault(); return; }
    if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'f' && S.view === 'control') { Plug.focusSearch(); e.preventDefault(); return; }
    const tag = (e.target.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
    if (Undo.key(e)) { e.preventDefault(); return; }               /* [Undo] Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z */
    const n = /^[1-9]$/.test(e.key) ? Number(e.key) : 0;
    if (e.ctrlKey && !e.altKey && n >= 1 && n <= VIEWS.length) {
      showView(VIEWS[n - 1]);
      e.preventDefault();
      return;
    }
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    const k = e.key.toLowerCase();
    if (k === 'm') setMon('mute', !S.mon.mute);
    else if (k === 'd') setMon('dim', !S.mon.dim);
    else if (k === 'o') setMon('mono', !S.mon.mono);
  });
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    if (cfg.appearance.followSystem) applyAppearance();
  });
}

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

function boot() {
  wireChrome();
  buildMonitor();
  syncSegments();
  buildMeterBridge();                  /* [Themes] Studio's meter bridge (shown by the console composition) */
  Sig.build();                         /* [Themes] HI-SCORE, the measurement panel, the signal-flow overlay (signatures.js) */
  Cmd.build();                         /* [Themes] Mono's command line (cmd.js; Ctrl+K is the palette everywhere) */
  Sess.build();                        /* [Sess] the session button and list (sessions.js) */
  Undo.build();                        /* [Undo] the undo / redo buttons (undo.js) */
  buildCues();
  renderUnitBar();
  buildBridge();
  buildDevice();
  buildPresetMenu();
  applyAppearance();
  showView('control');
  renderStatus();
  if (host) {
    host.addEventListener('message', e => onHostMessage(e.data));
    send('hello');
  } else {
    let loaded = null;
    try { loaded = JSON.parse(localStorage.getItem('apollo-control') || 'null'); } catch (e) { loaded = null; }
    cfg = merge(defaultConfig(), loaded);
    migrateLooks(loaded);
    applyConfig();
    enterDemo();
    Sess.hello();                      /* [Sess] the demo keeps its sessions in this browser */
    const view = new URLSearchParams(location.search).get('view');
    showView(view || cfg.view);
  }
  requestAnimationFrame(frame);
}

boot();
