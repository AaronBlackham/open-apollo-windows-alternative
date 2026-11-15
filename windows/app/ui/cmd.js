'use strict';
/*
 * Typed control (docs/THEMES.md 3): Mono's command line (its signature) and the Ctrl+K command palette, which every
 * theme has. One interpreter for both, so a command does the same thing wherever it is typed:
 *   in 3 gain 32 · in 3 gain +3 · in 1-2 link · in 4 48v on · in 2 pad · in 5 line · in 1 level -6 · in 1 pan L30
 *   in 2 mute · in 7 name Kick · insert 1176 on 2 · remove 2 · mute all · unmute all · mute · dim · mono
 *   level -18 · rate 96k · theme cyberpunk · view status · focus tx · flow off · log on · measure · help
 *   [Sess] session save <name> · session load <name> · session list · session delete <name> · session last · export · import
 * A leading unit word (x8p, tx / twin) sends one line to that unit, else to the focused one. History (Up / Down), Tab
 * completion, and a script file of commands (one per line, # for notes) that runs like a preset (Presets menu, or `run`).
 */
const Cmd = (() => {
  const C = { hist: [], hpos: -1, lines: [], root: null, input: null, out: null, hint: null, file: null, pal: null, running: false };
  const LOOK_WORDS = { cyberpunk: 'cyber', hud: 'cyber', terminal: 'mono', analyzer: 'lab', analyser: 'lab', console: 'studio', guide: 'day', cabinet: 'arcade', retro: 'arcade' };
  const VIEW_WORDS = { routing: 'units', style: 'styleguide', 'style-guide': 'styleguide', speakers: 'correction', home: 'control' };
  const TOP = ['in', 'insert', 'remove', 'aux', 'mute', 'unmute', 'dim', 'mono', 'level', 'rate', 'theme', 'view', 'focus', 'flow', 'log',
    'measure', 'hiscore', 'guide', 'run', 'save', 'history', 'clear', 'help', 'session'];
  const IN_KEYS = ['gain', '48v', 'pad', 'lowcut', 'phase', 'mic', 'line', 'link', 'unlink', 'level', 'pan', 'mute', 'solo', 'name', 'guide', 'rec', 'mon', 'aux1', 'aux2'];
  const ONOFF = ['on', 'off'];

  /* ------------------------------------------------------------ the interpreter */

  const unitWords = () => {
    const out = [];
    for (const idx of segs.keys()) {
      const sh = String(capsOf(unitType(idx)).short || '').toLowerCase().replace(/[\s-]/g, '');
      out.push([sh, idx]);
      if (sh.startsWith('twin')) out.push(['tx', idx], ['twin', idx]);
    }
    return out;
  };
  function unitOf(tok) {
    const t = String(tok || '').toLowerCase().replace(/[\s-]/g, '');
    const m = /^u(\d)$/.exec(t);
    if (m && segs.has(Number(m[1]))) return Number(m[1]);
    const hit = unitWords().find(([w]) => w === t);
    return hit ? hit[1] : null;
  }

  /* "3", "1-2", "1,3,5", "all": 0-based inputs of the unit */
  function inputsOf(tok, idx) {
    const n = capsOf(unitType(idx)).preamps || 0, t = String(tok || '').toLowerCase();
    if (t === 'all') return [...Array(n).keys()];
    const out = [];
    for (const part of t.split(',')) {
      const m = /^(\d+)(?:-(\d+))?$/.exec(part);
      if (!m) return null;
      const a = Number(m[1]), b = m[2] ? Number(m[2]) : a;
      if (a < 1 || b < a || b > n) return null;
      for (let i = a; i <= b; i++) out.push(i - 1);
    }
    return out.length ? out : null;
  }

  const onOff = (tok, was) => {
    const t = String(tok || '').toLowerCase();
    if (t === 'on' || t === '1' || t === 'yes') return true;
    if (t === 'off' || t === '0' || t === 'no') return false;
    if (t === '' || t === 'toggle') return !was;
    return null;
  };

  function parseRate(tok) {
    const t = String(tok || '').toLowerCase().replace(/hz$/, '');
    const m = /^(\d+(?:\.\d+)?)(k?)$/.exec(t);
    if (!m) return null;
    let v = Number(m[1]);
    if (m[2] || v < 1000) v *= 1000;
    v = Math.round(v);
    return RATES.includes(v) ? v : null;
  }

  function lookOf(word) {
    const t = String(word || '').toLowerCase();
    const id = LOOK_WORDS[t] || t;
    const L = LOOKS.find(l => l.id === id) || LOOKS.find(l => l.name.toLowerCase() === t) || LOOKS.find(l => l.name.toLowerCase().startsWith(t) || l.id.startsWith(t));
    return t && L ? L : null;
  }

  const ok = text => ({ ok: true, text });
  const no = text => ({ ok: false, text });
  const HELP = [
    'in <n|a-b|all> gain <dB|+dB|-dB> · 48v|pad|lowcut|phase [on|off] · mic · line · link · unlink',
    'in <n> level <dB|off> · pan <C|L30|R30> · mute|solo [on|off] · name <text> · guide',
    'insert <plug-in> on <n|aux1|aux2> [slot <1-4>] · remove <n|aux1> [slot <1-4>] · in <n> rec|mon · in <n> aux1 <dB|off>',
    'aux <1|2> return <dB|off> · aux <1|2> mute [on|off] · aux <1|2> pre|post · aux <1|2> cue <c> <dB|off>',
    'mute all · unmute all · mute · dim · mono [on|off] · level <dB>',
    'rate <44.1k|48k|88.2k|96k|176.4k|192k> · theme <name> · view <tab> · focus <x8p|tx> · flow on|off',
    'log on|off|save · measure · hiscore reset · run (a script file) · save (history as a script) · history · clear',
    'session save <name> · session load <name> · session list · session delete <name> · session last · session export [name] · session import',
    'Start a line with x8p or tx to send it to that unit. Up / Down: history. Tab: completes.',
  ];

  async function exec(line0) {
    const line = String(line0 || '').trim();
    if (!line || line.startsWith('#')) return null;
    let toks = line.split(/\s+/), idx = S.sel;
    const u = unitOf(toks[0]);
    if (u !== null && toks.length > 1) { idx = u; toks = toks.slice(1); }
    const w = toks[0].toLowerCase(), rest = toks.slice(1), uname = unitShort(idx);
    const k = capsOf(unitType(idx));
    switch (w) {
      case 'in': case 'input': return execInput(idx, rest);
      case 'insert': {
        const lw = rest.map(x => x.toLowerCase()), j = lw.lastIndexOf('on');
        if (rest[0] && rest[0].toLowerCase() === 'off') return execRemove(idx, rest.slice(1));
        if (j < 1 || j === rest.length - 1) return no('insert <plug-in> on <input|aux1|aux2> [slot <1-4>], for example: insert 1176 on 2');
        const sl = lw.indexOf('slot', j), slot = sl > 0 ? Number(rest[sl + 1]) - 1 : null;
        if (sl > 0 && !(slot >= 0 && slot < 4)) return no('slot <1-4>');
        const ch = targetOf(rest[j + 1], idx);
        if (ch === null) return no(`No input ${rest[j + 1]} on the ${uname}.`);
        const r = await Plug.insertByName(rest.slice(0, j).join(' '), idx, ch, slot);
        return r.ok ? ok(r.why) : no(r.why);
      }
      case 'aux': return execAux(idx, rest);
      case 'remove': return execRemove(idx, rest[0] && rest[0].toLowerCase() === 'insert' ? rest.slice(rest[1] && rest[1].toLowerCase() === 'on' ? 2 : 1) : rest);
      case 'mute': case 'unmute': {
        if (rest[0] && rest[0].toLowerCase() === 'all') {
          if (!k.preamps) return no(`The ${uname} has no inputs to mute.`);
          setMixMuteAll(w === 'mute', idx);
          return ok(`${w === 'mute' ? 'Muted' : 'Unmuted'} every ${uname} input and the talkback in the monitor mix.`);
        }
        const on = w === 'unmute' ? false : onOff(rest[0], ctl(idx).mon.mute);
        if (on === null) return no('mute [on|off] · mute all · unmute all');
        setMon('mute', on, idx);
        return ok(`${uname} monitor ${on ? 'muted' : 'unmuted'}.`);
      }
      case 'dim': case 'mono': {
        const on = onOff(rest[0], ctl(idx).mon[w]);
        if (on === null) return no(`${w} [on|off]`);
        setMon(w, on, idx);
        return ok(`${uname} monitor ${w} ${on ? 'on' : 'off'}.`);
      }
      case 'level': case 'mon': {
        const v = parseDb(rest[0]);
        if (v === null) return no('level <dB>, for example: level -18 (0 dB is the top)');
        setMon('level', v, idx);
        return ok(`${uname} monitor level ${rawToDb(v).toFixed(1)} dB.`);
      }
      case 'rate': {
        const hz = parseRate(rest[0]);
        if (!hz) return no(`rate <${RATES.map(r => kHz(r).replace(/\s*kHz/i, 'k')).join('|')}>`);
        const key = rateKey(idx), agg = key === 'agg' ? rateAgg() : null, r = agg ? null : rateFor(idx);
        if (!(agg || r)) return no('The driver gives no sample rate to set on this build.');
        const allowed = agg ? agg.allowed : (r.allowed || r.offered || []);
        if (allowed && allowed.length && !allowed.includes(hz)) return no(`${kHz(hz)} is not allowed now${agg ? ' on the combined device' : ` on the ${uname}`}.`);
        rateSet(key, idx, hz);
        return ok(`Asked for ${kHz(hz)}${agg ? ' on the combined device' : ` on the ${uname}`}.`);
      }
      case 'theme': case 'look': {
        const L = lookOf(rest.join(' '));
        if (!L) return no(`theme <${LOOKS.map(l => l.id).join('|')}>`);
        setLook(L.id);
        return ok(`Theme: ${L.name}.`);
      }
      case 'view': case 'go': case 'tab': {
        const t = String(rest[0] || '').toLowerCase(), v = VIEW_WORDS[t] || t;
        if (!VIEWS.includes(v) && v !== 'styleguide') return no(`view <${VIEWS.join('|')}|styleguide>`);
        showView(v);
        return ok(`View: ${v}.`);
      }
      case 'focus': case 'unit': {
        const f = unitOf(rest[0]);
        if (f === null) return no(`focus <${unitWords().map(x => x[0]).join('|')}>`);
        selectUnit(f);
        return ok(`Focused the ${unitShort(f)}.`);
      }
      case 'flow': {
        const on = onOff(rest[0], Sig.flowOn);
        if (on === null) return no('flow on|off');
        Sig.setFlow(on);
        return ok(`Signal flow ${on ? 'on' : 'off'} (the Cyberpunk theme shows it).`);
      }
      case 'log': {
        const t = String(rest[0] || '').toLowerCase();
        if (t === 'save') { Sig.saveLog(); return ok('Saving the level log as CSV.'); }
        const on = onOff(t, Sig.logging);
        if (on === null) return no('log on|off|save');
        Sig.setLog(on);
        return ok(`Level log ${on ? 'started' : 'stopped'}.`);
      }
      case 'measure': Sig.measureOpen(); return ok('Opening the routing in Avium Measure.');
      case 'hiscore': Sig.hiReset(); return ok('HI-SCORE reset.');
      case 'guide': {
        const chs = inputsOf(rest[0], idx);
        if (!chs || chs.length !== 1) return no('guide <input>');
        Sig.guide(idx, chs[0]);
        return ok(`Guided gain on ${stripWho(idx, chs[0])}.`);
      }
      case 'session': case 'sessions': return Sess.exec(rest);     /* [Sess] sessions.js */
      case 'run': pickScript(); return ok('Choose a script file (one command per line).');
      case 'save': saveScript(); return ok('Saving the history as a script.');
      case 'history': return ok(C.hist.slice(-12).join('\n') || '(empty)');
      case 'clear': C.lines = []; renderOut(); return null;
      case 'help': case '?': return ok(HELP.join('\n'));
      default: return no(`Unknown command "${w}". Type help.`);
    }
  }

  /* an insert target: an input number, or aux1 / aux2 */
  function targetOf(tok, idx) {
    const a = /^aux\s*([12])$/i.exec(String(tok || ''));
    if (a) return Plug.AUX_T + Number(a[1]) - 1;
    const chs = inputsOf(tok, idx);
    return chs && chs.length === 1 ? chs[0] : null;
  }

  async function execRemove(idx, rest) {
    const ch = targetOf(rest[0], idx), lw = rest.map(x => String(x).toLowerCase()), sl = lw.indexOf('slot');
    if (ch === null) return no('remove <input|aux1|aux2> [slot <1-4>]');
    const slot = sl > 0 ? Number(rest[sl + 1]) - 1 : null;
    const r = await Plug.removeAt(idx, ch, slot !== null && slot >= 0 && slot < 4 ? slot : null);
    return r.ok ? ok(r.why) : no(r.why);
  }

  /* aux <1|2> return <dB|off> · mute [on|off] · pre · post · cue <c> <dB|off> */
  function execAux(idx, rest) {
    const a = Number(rest[0]) - 1, w = String(rest[1] || '').toLowerCase(), uname = unitShort(idx);
    if (!(a === 0 || a === 1)) return no('aux <1|2> return <dB|off> · mute [on|off] · pre · post · cue <n> <dB|off>');
    if (!capsOf(unitType(idx)).preamps) return no(`The ${uname} has no aux buses here.`);
    if (w === 'return' || w === 'ret' || w === 'level') {
      const v = parseSend(rest[2]);
      if (v === null) return no('aux <1|2> return <-60..+12 dB|off>');
      Aux.setRet(idx, a, v);
      return ok(`${uname} AUX ${a + 1} return ${fmtSend(v)}.`);
    }
    if (w === 'mute' || w === 'unmute') {
      const on = w === 'unmute' ? false : onOff(rest[2], Aux.muteOf(idx, a));
      if (on === null) return no('aux <1|2> mute [on|off]');
      Aux.setMute(idx, a, on);
      return ok(`${uname} AUX ${a + 1} ${on ? 'muted' : 'unmuted'}.`);
    }
    if (w === 'pre' || w === 'post') { Aux.setPost(idx, a, w === 'post'); return ok(`${uname} AUX ${a + 1} sends ${w}-fader.`); }
    if (w === 'cue') {
      const c = Number(rest[2]) - 1, v = parseSend(rest[3]), n = (capsOf(unitType(idx)).cues || []).length;
      if (!(c >= 0 && c < n) || v === null) return no(`aux <1|2> cue <1-${n}> <dB|off>`);
      Aux.setCue(idx, a, c, 'level', v);
      return ok(`${uname} AUX ${a + 1} into CUE ${c + 1}: ${fmtSend(v)}.`);
    }
    return no('aux <1|2> return <dB|off> · mute [on|off] · pre · post · cue <n> <dB|off>');
  }

  async function execInput(idx, rest) {
    const k = capsOf(unitType(idx)), c = ctl(idx), uname = unitShort(idx);
    const chs = inputsOf(rest[0], idx);
    if (!chs) return no(`No input ${rest[0] || ''} on the ${uname} (it has ${k.preamps || 0}).`);
    const key = String(rest[1] || '').toLowerCase(), arg = rest[2], label = chs.length > 1 ? `inputs ${rest[0]}` : stripWho(idx, chs[0]).toLowerCase();
    const KEY = { pad: 'pad', lowcut: 'lowcut', locut: 'lowcut', lc: 'lowcut', hpf: 'lowcut', phase: 'phase', pol: 'phase', polarity: 'phase' };
    if (key === 'gain') {
      const m = /^([+-]?)(\d+)(?:db)?$/i.exec(String(arg || ''));
      if (!m) return no('gain <10..65> or +n / -n');
      if (cfg.safety.lockGain) return no('Gain knobs are locked (Settings, Safety).');
      for (const ch of chs) {
        const v = m[1] ? c.inputs[ch].gain + (m[1] === '-' ? -1 : 1) * Number(m[2]) : Number(m[2]);
        setInput(ch, 'gain', clamp(v, 10, 65), { unit: idx });
      }
      return ok(`${label} gain ${chs.map(ch => `${c.inputs[ch].gain} dB`).join(', ')}.`);
    }
    if (key === '48v' || key === 'phantom') {
      const on = onOff(arg, c.inputs[chs[0]].p48);
      if (on === null) return no('48v [on|off]');
      for (const ch of chs) await request48v(ch, on, idx);
      return ok(`${label} 48V ${chs.every(ch => c.inputs[ch].p48 === on) ? (on ? 'on' : 'off') : 'unchanged'}.`);
    }
    if (KEY[key]) {
      const f = INPUT_FIELDS[KEY[key]], on = onOff(arg, c.inputs[chs[0]][f]);
      if (on === null) return no(`${key} [on|off]`);
      for (const ch of chs) setInput(ch, KEY[key], on, { unit: idx });
      return ok(`${label} ${KEY[key]} ${on ? 'on' : 'off'}.`);
    }
    if (key === 'mic' || key === 'line') {
      for (const ch of chs) setInput(ch, 'line', key === 'line', { unit: idx });
      return ok(`${label} ${key}.`);
    }
    if (key === 'link' || key === 'unlink') {
      const pairs = [...new Set(chs.map(ch => ch >> 1))];
      for (const p of pairs) {
        if (!((k.linkPairs >>> p) & 1)) return no(`Inputs ${p * 2 + 1}-${p * 2 + 2} of the ${uname} do not link.`);
        await setLink(p, key === 'link', idx);
      }
      return ok(`${pairs.map(p => `${p * 2 + 1}-${p * 2 + 2}`).join(', ')} ${key === 'link' ? 'linked' : 'unlinked'}.`);
    }
    if (key === 'level' || key === 'fader' || key === 'mix') {
      const v = parseSend(arg);
      if (v === null) return no('level <-60..+12 dB|off>');
      for (const ch of chs) setMix(ch, 'level', v, idx);
      return ok(`${label} mix level ${fmtSend(v)}.`);
    }
    if (key === 'pan') {
      const v = parsePan(arg);
      if (v === null) return no('pan <C|L30|R30|-100..100>');
      for (const ch of chs) setMix(ch, 'pan', v, idx);
      return ok(`${label} pan ${fmtPan(v)}.`);
    }
    if (key === 'mute' || key === 'solo') {
      const on = onOff(arg, mixOfBus(idx, chs[0])[key]);
      if (on === null) return no(`${key} [on|off]`);
      for (const ch of chs) setMix(ch, key, on, idx);
      return ok(`${label} ${key} ${on ? 'on' : 'off'} in the monitor mix.`);
    }
    if (key === 'name') {
      if (chs.length !== 1) return no('name: one input at a time');
      const cc = chanCfg(idx, chs[0]);
      if (cc === NO_NAME) return no('The host has not named this unit yet, so its input names cannot be kept.');
      cc.name = rest.slice(2).join(' ').slice(0, 24);
      saveConfig();
      renderInput(chs[0], idx);
      return ok(`${label} is "${cc.name || '(no name)'}".`);
    }
    if (key === 'guide') { Sig.guide(idx, chs[0]); return ok(`Guided gain on ${label}.`); }
    if (key === 'rec' || key === 'mon') {
      for (const ch of chs) Plug.setRecMon(idx, ch, key === 'mon');
      return ok(`${label}: ${key === 'mon' ? 'MON, recorded dry, inserts heard only' : 'REC, inserts recorded'}.`);
    }
    const ax = /^aux([12])$/.exec(key);
    if (ax) {
      const a = Number(ax[1]) - 1, field = String(arg || '').toLowerCase() === 'pan' ? 'pan' : 'level';
      const v = field === 'pan' ? parsePan(rest[3]) : parseSend(arg);
      if (v === null) return no(`in <n> aux${a + 1} <dB|off> · in <n> aux${a + 1} pan <C|L30|R30>`);
      for (const ch of chs) Aux.setSend(idx, a, ch, field, v);
      return ok(`${label} send to AUX ${a + 1}${field === 'pan' ? ` pan ${fmtPan(v)}` : ` ${fmtSend(v)}`}.`);
    }
    return no(`in <n> ${IN_KEYS.join('|')}`);
  }

  /* one line typed anywhere: run, remember, print */
  async function run(line, from = 'line') {
    const t = String(line || '').trim();
    if (!t) return null;
    if (from !== 'script' && C.hist[C.hist.length - 1] !== t) {
      C.hist.push(t);
      if (C.hist.length > 200) C.hist.shift();
      try { localStorage.setItem('apollo-cmd-history', JSON.stringify(C.hist.slice(-100))); } catch (e) { /* not kept */ }
    }
    C.hpos = -1;
    print(`> ${t}`, 'cmd');
    let r;
    try { r = await exec(t); } catch (e) { r = no(`Failed: ${e.message || e}`); }
    if (r) print(r.text, r.ok ? 'ok' : 'err');
    return r;
  }

  /* ------------------------------------------------------------ scripts (run like a preset) */

  function pickScript() {
    if (!C.file) {
      C.file = el('input', { type: 'file', accept: '.txt,.apc,.cmd,text/plain', hidden: true, onchange: async e => {
        const f = e.target.files && e.target.files[0];
        e.target.value = '';
        if (f) runScript(await f.text(), f.name);
      } });
      document.body.append(C.file);
    }
    C.file.click();
  }

  async function runScript(text, name = 'script') {
    if (C.running) { toast('A script is running already.', 'warn'); return; }
    const lines = String(text).split(/\r?\n/).map(s => s.trim()).filter(s => s && !s.startsWith('#'));
    if (!lines.length) { toast(`${name} has no commands.`, 'warn'); return; }
    if (lines.length > 500) { toast(`${name} has ${lines.length} lines: the most a script runs is 500.`, 'warn'); return; }
    C.running = true;
    print(`# ${name}: ${lines.length} command${lines.length === 1 ? '' : 's'}`, 'cmd');
    let bad = 0;
    for (const l of lines) {
      const r = await run(l, 'script');
      if (r && !r.ok) bad++;
      await new Promise(res => setTimeout(res, 60));
    }
    C.running = false;
    const msg = `${name}: ${lines.length - bad} of ${lines.length} commands ran${bad ? `, ${bad} did not (see the command line)` : ''}.`;
    print(`# ${msg}`, bad ? 'err' : 'ok');
    toast(msg, bad ? 'warn' : 'info');
  }

  function saveScript() {
    const body = ['# Apollo Control command script: one command per line, # for notes. Run it from Presets, Run a command script.', ...C.hist.filter(l => !/^(run|save|history|clear|help)\b/i.test(l))].join('\n');
    const a = el('a', { href: URL.createObjectURL(new Blob([`${body}\n`], { type: 'text/plain' })), download: 'apollo-commands.txt' });
    document.body.append(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }

  /* ------------------------------------------------------------ completion */

  function candidates(line) {
    const parts = line.split(/\s+/), last = parts.pop() || '';
    let pre = parts.map(x => x.toLowerCase());
    if (pre.length && unitOf(pre[0]) !== null) pre = pre.slice(1);
    const from = list => list.filter(x => x.toLowerCase().startsWith(last.toLowerCase()));
    if (!pre.length) return from([...TOP, ...unitWords().map(x => x[0])]);
    const w = pre[0];
    if (w === 'insert') {
      const typed = line.replace(/^\s*(\S+\s+)?insert\s+/i, m => '').toLowerCase();
      if (!/\son\s*\S*$/.test(` ${typed}`)) {
        const idx = unitOf(parts[0]) ?? S.sel;
        return Plug.itemNames(idx).filter(n => n.toLowerCase().startsWith(typed)).map(n => ({ full: n, tail: true }));
      }
      return [];
    }
    if (w === 'in' || w === 'input') {
      if (pre.length === 2) return from(IN_KEYS);
      if (pre.length === 3 && ['48v', 'pad', 'lowcut', 'phase', 'mute', 'solo'].includes(pre[2])) return from(ONOFF);
      return [];
    }
    if (w === 'session' || w === 'sessions') {
      if (pre.length === 1) return from(Sess.subs());
      if (['load', 'delete', 'export', 'open', 'rm'].includes(pre[1])) {
        const typed = line.replace(/^\s*(\S+\s+)?sessions?\s+\S+\s+/i, '').toLowerCase();
        return Sess.names().filter(n => n.toLowerCase().startsWith(typed)).map(n => ({ full: n, tail: true, end: '' }));
      }
      return [];
    }
    if ((w === 'theme' || w === 'look') && pre.length === 1) return from([...LOOKS.map(l => l.id), ...Object.keys(LOOK_WORDS)]);
    if ((w === 'view' || w === 'go') && pre.length === 1) return from([...VIEWS, 'styleguide']);
    if (w === 'focus' && pre.length === 1) return from(unitWords().map(x => x[0]));
    if (['mute', 'unmute'].includes(w) && pre.length === 1) return from(['all', ...ONOFF]);
    if (['dim', 'mono', 'flow'].includes(w) && pre.length === 1) return from(ONOFF);
    if (w === 'log' && pre.length === 1) return from([...ONOFF, 'save']);
    if (w === 'rate' && pre.length === 1) return from(RATES.map(r => `${r % 1000 ? (r / 1000).toFixed(1) : r / 1000}k`));
    return [];
  }

  function complete(input) {
    const line = input.value, list = candidates(line);
    if (!list.length) { hint(''); return; }
    const plug = typeof list[0] === 'object';
    const names = plug ? list.map(x => x.full) : list;
    const common = names.reduce((a, b) => { let i = 0; while (i < a.length && i < b.length && a[i].toLowerCase() === b[i].toLowerCase()) i++; return a.slice(0, i); });
    let head;
    if (plug) head = line.replace(/^(.*?(?:insert|sessions?\s+\S+)\s+)(.*)$/i, (m, a) => a);
    else head = line.replace(/\S*$/, '');
    const done = names.length === 1;
    const end = plug && typeof list[0].end === 'string' ? list[0].end : ' on ';     /* [Sess] a session name ends the line */
    input.value = `${head}${done ? names[0] : common}${done ? (plug ? end : ' ') : ''}`;
    hint(done ? '' : names.slice(0, 12).join('  '));
  }

  function hint(text) { if (C.hint) C.hint.textContent = text; }

  /* ------------------------------------------------------------ Mono's command line */

  function print(text, kind = 'note') {
    for (const l of String(text).split('\n')) C.lines.push({ text: l, kind });
    if (C.lines.length > 300) C.lines.splice(0, C.lines.length - 300);
    renderOut();
  }
  /* every toast lands in the log tail too (Mono's feedback: the log at the bottom) */
  function note(text, kind) { if (C.out) print(`· ${text}`, kind === 'warn' ? 'err' : 'note'); }

  function renderOut() {
    if (!C.out) return;
    C.out.replaceChildren(...C.lines.slice(-60).map(l => el('div', { class: `cl-${l.kind}`, text: l.text })));
    C.out.scrollTop = C.out.scrollHeight;
  }

  function historyKey(e, input) {
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      if (!C.hist.length) return false;
      C.hpos = C.hpos < 0 ? C.hist.length : C.hpos;
      C.hpos = clamp(C.hpos + (e.key === 'ArrowUp' ? -1 : 1), 0, C.hist.length);
      input.value = C.hist[C.hpos] || '';
      return true;
    }
    return false;
  }

  function build() {
    const desk = document.getElementById('desk');
    if (!desk || C.root) return;
    try { C.hist = JSON.parse(localStorage.getItem('apollo-cmd-history') || '[]').filter(x => typeof x === 'string'); } catch (e) { C.hist = []; }
    C.out = el('div', { class: 'cl-out num', role: 'log', 'aria-live': 'polite', 'aria-label': 'Command output' });
    C.hint = el('div', { class: 'cl-hint num', 'aria-live': 'polite' });
    C.input = el('input', { class: 'cl-input num', type: 'text', spellcheck: 'false', autocomplete: 'off', 'aria-label': 'Command line (type help)',
      placeholder: 'in 3 gain 32 · insert 1176 on 2 · rate 96k · help' });
    C.input.addEventListener('keydown', async e => {
      if (e.ctrlKey && !e.altKey && ['k', 't', 's', 'o'].includes(e.key.toLowerCase())) return;   /* the palette, the next theme, [Sess] save, sessions */
      e.stopPropagation();
      if (e.key === 'Enter') { const t = C.input.value; C.input.value = ''; hint(''); await run(t); e.preventDefault(); }
      else if (e.key === 'Tab' && C.input.value.trim()) { complete(C.input); e.preventDefault(); }
      else if (e.key === 'Escape') { C.input.value = ''; hint(''); }
      else if (historyKey(e, C.input)) e.preventDefault();
    });
    C.root = el('section', { class: 'cmd-line', id: 'cmdLine', 'aria-label': 'Command line' },
      C.out, el('div', { class: 'cl-row' }, el('span', { class: 'cl-ps num', 'aria-hidden': 'true', text: '>' }), C.input,
        el('button', { class: 'btn btn-quiet', type: 'button', onclick: pickScript, title: 'Run a script file of commands, one per line' }, 'RUN FILE')),
      C.hint);
    desk.append(C.root);
    print('Apollo Control command line. Type help, or press Ctrl+K in any theme.', 'note');
  }

  /* ------------------------------------------------------------ the Ctrl+K palette (every theme) */

  function actions() {
    const A = [];
    const add = (label, o) => A.push({ label, ...o });
    VIEWS.forEach((v, i) => add(`Go to ${document.getElementById(`tab-${v}`) ? document.getElementById(`tab-${v}`).textContent.trim() : v}`, { cmd: `view ${v}`, key: `Ctrl+${i + 1}` }));
    add('Open the style guide', { cmd: 'view styleguide' });
    LOOKS.forEach(l => add(`Theme: ${l.name}`, { cmd: `theme ${l.id}`, key: l.id === cfg.appearance.look ? 'now' : '' }));
    add('Next theme', { run: () => cycleLook(), key: 'Ctrl+Shift+T' });
    add('Mute every input in the monitor mix', { cmd: 'mute all' });
    add('Unmute every input in the monitor mix', { cmd: 'unmute all' });
    add('Monitor mute on / off', { cmd: 'mute', key: 'M' });
    add('Monitor dim on / off', { cmd: 'dim', key: 'D' });
    add('Monitor mono on / off', { cmd: 'mono', key: 'O' });
    add('Search plug-ins', { run: () => { showView('control'); Plug.focusSearch(); }, key: 'Ctrl+F' });
    add('Run a command script (a file, one command per line)', { cmd: 'run' });
    add('Save the command history as a script', { cmd: 'save' });
    add('Signal flow on / off (Cyberpunk)', { cmd: 'flow' });
    add('Level log start / stop (Lab)', { cmd: 'log' });
    add('Save the level log as CSV (Lab)', { cmd: 'log save' });
    add('Open the routing in Avium Measure (Lab)', { cmd: 'measure' });
    add('Reset the HI-SCORE (Arcade)', { cmd: 'hiscore reset' });
    for (const a of Sess.actions()) A.push(a);                    /* [Sess] save, save as, the list, load each, last state, export, import */
    for (const [w, idx] of unitWords()) if (!['tx', 'twin'].includes(w) || w === 'tx') add(`Focus the ${unitShort(idx)}`, { cmd: `focus ${w}` });
    ['in 1 gain 40', 'in 1-2 link', 'in 3 48v on', 'in 2 level -6', 'insert  on 1', 'rate 96k'].forEach(x => add(`Type: ${x.replace('  ', ' <plug-in> ')}`, { fill: x.replace('  ', ' ') }));
    for (const n of Plug.itemNames(S.sel).slice(0, 400)) add(`Insert ${n}`, { fill: `insert ${n} on ` });
    return A;
  }

  function score(label, q) {
    const l = label.toLowerCase(), words = q.toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) return 1;
    let s = 0;
    for (const w of words) { const i = l.indexOf(w); if (i < 0) return 0; s += i === 0 ? 3 : l[i - 1] === ' ' ? 2 : 1; }
    return s;
  }

  function palette() {
    if (C.pal) { C.pal.input.focus(); return; }
    const back = document.activeElement;
    const all = actions();
    const input = el('input', { class: 'pal-in', type: 'text', role: 'combobox', 'aria-expanded': 'true', 'aria-controls': 'palList', 'aria-autocomplete': 'list',
      'aria-label': 'Command or action', placeholder: 'Type an action or a command (in 3 gain 32)', spellcheck: 'false', autocomplete: 'off' });
    const list = el('ul', { class: 'pal-list', id: 'palList', role: 'listbox', 'aria-label': 'Actions' });
    const foot = el('div', { class: 'pal-foot num', text: 'Enter runs · Tab fills · Up / Down · Esc closes' });
    const box = el('div', { class: 'pal-box', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Command palette' }, input, list, foot);
    const root = el('div', { class: 'palette', id: 'palette' }, box);
    let shown = [], sel = 0;
    const close = () => { root.remove(); C.pal = null; if (back && back.isConnected) back.focus(); };
    const render = () => {
      const q = input.value.trim();
      const typed = q && (TOP.includes(q.split(/\s+/)[0].toLowerCase()) || unitOf(q.split(/\s+/)[0]) !== null);
      shown = all.map(a => ({ a, s: score(a.label, q) })).filter(x => x.s > 0).sort((x, y) => y.s - x.s).slice(0, 12).map(x => x.a);
      if (typed) shown.unshift({ label: `Run: ${q}`, cmd: q });
      sel = clamp(sel, 0, Math.max(0, shown.length - 1));
      list.replaceChildren(...(shown.length ? shown.map((a, i) => el('li', { id: `pal-${i}`, role: 'option', class: 'pal-item', 'aria-selected': i === sel ? 'true' : 'false',
        onmousedown: e => { e.preventDefault(); sel = i; choose(false); } }, el('span', { text: a.label }), a.key ? el('span', { class: 'pal-key num', text: a.key }) : null))
        : [el('li', { class: 'pal-none', text: 'Nothing matches. Enter runs it as a command.' })]));
      input.setAttribute('aria-activedescendant', shown.length ? `pal-${sel}` : '');
      const cur = list.querySelector('[aria-selected="true"]');
      if (cur) cur.scrollIntoView({ block: 'nearest' });
    };
    const choose = async fillOnly => {
      const a = shown[sel];
      if (!a) { const q = input.value.trim(); if (q) { close(); const r = await run(q, 'palette'); if (r) toast(r.text, r.ok ? 'info' : 'warn'); } return; }
      if (a.fill || fillOnly) { input.value = a.fill || a.cmd || input.value; sel = 0; render(); return; }
      close();
      if (a.run) { a.run(); return; }
      const r = await run(a.cmd, 'palette');
      if (r && !(a.cmd || '').startsWith('view ')) toast(r.text, r.ok ? 'info' : 'warn');
    };
    input.addEventListener('input', () => { sel = 0; render(); });
    input.addEventListener('keydown', e => {
      e.stopPropagation();
      if (e.key === 'Escape') { close(); e.preventDefault(); }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { sel = clamp(sel + (e.key === 'ArrowDown' ? 1 : -1), 0, Math.max(0, shown.length - 1)); render(); e.preventDefault(); }
      else if (e.key === 'Enter') { choose(false); e.preventDefault(); }
      else if (e.key === 'Tab') { choose(true); e.preventDefault(); }
    });
    root.addEventListener('mousedown', e => { if (e.target === root) close(); });
    document.body.append(root);
    C.pal = { root, input };
    render();
    input.focus();
  }

  return { build, run, exec, palette, note, pickScript, runScript, get history() { return C.hist.slice(); } };
})();
