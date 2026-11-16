'use strict';
/*
 * The themes' signature features (docs/THEMES.md 3): one way of working that only that theme has.
 * Studio's meter bridge lives in app.js (MB); Mono's command line and the Ctrl+K palette in cmd.js. Here:
 *   Day       guided gain: "speak or play normally for 5 seconds", then a suggested (or set) preamp gain per input.
 *   Arcade    HI-SCORE: every input's highest peak since reset, with the time it happened.
 *   Lab       the measurement panel: level history, peak / RMS logging to CSV, and the Avium Measure hand-off.
 *   Cyberpunk the live signal-flow overlay: packets travel input -> preamp -> insert -> monitor mix -> main out,
 *             their density and speed following the level, stopping where the sound stops.
 * Sig.frame(dt, t) runs every animation frame (app.js frame()); each feature does its own work only while its
 * composition is on screen, except the level bookkeeping (HI-SCORE, history), which is cheap and always kept.
 */
const Sig = (() => {
  const comp = () => { const v = document.getElementById('view-control'); return v ? v.dataset.comp : ''; };
  const onControl = c => S.view === 'control' && comp() === c;
  const peakOf = (idx, slot) => { const c = S.ctl[idx]; return c && c.levelsValid ? c.peaks[slot] : -Infinity; };
  const two = n => String(n).padStart(2, '0');
  const clock = d => `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
  const dbText = db => (db > -200 ? db.toFixed(1) : '--');
  const liveSegs = () => [...segs.keys()].sort((a, b) => a - b).map(i => segs.get(i)).filter(sg => sg && !sg.gone);
  const playerOf = idx => [...segs.keys()].sort((a, b) => a - b).indexOf(idx) + 1;
  const unitTag = idx => (idx ? `${unitShort(idx)} ` : '');

  /* the level cells every feature reads: each live unit's inputs, then its main out L and R */
  function cells() {
    const out = [];
    for (const sg of liveSegs()) {
      for (const s of sg.strips) out.push({ key: `${sg.idx}:${s.ch}`, idx: sg.idx, slot: s.ch, ch: s.ch, main: false,
        label: `${unitTag(sg.idx)}${two(s.ch + 1)}`, name: (chanCfg(sg.idx, s.ch) || {}).name || '' });
      if (sg.master) {
        const [l, r] = monSlots(sg.idx);
        out.push({ key: `${sg.idx}:L`, idx: sg.idx, slot: l, main: true, label: `${unitTag(sg.idx)}MAIN L`, name: '' },
          { key: `${sg.idx}:R`, idx: sg.idx, slot: r, main: true, label: `${unitTag(sg.idx)}MAIN R`, name: '' });
      }
    }
    return out;
  }

  /* ================================================================== Day: guided gain */

  const GUIDE_MS = 5000, GUIDE_TARGET = -12;
  const G = { run: null };

  function guide(idx, ch) {
    if (comp() !== 'focus') { toast('Guided gain is the Day theme\'s: switch to Day (Ctrl+K, then "theme day").'); return; }
    const sg = segs.get(idx), s = sg && sg.strips[ch];
    if (!s || sg.gone) return;
    stopGuide();
    if (!s.node.classList.contains('focus')) focusStrip(idx, ch);
    const box = el('div', { class: 'st-guide-box', role: 'status', 'aria-live': 'polite' });
    s.node.append(box);
    G.run = { idx, ch, s, box, t0: performance.now(), peak: -Infinity, clip: false, done: false, last: '' };
    renderGuide();
  }

  function stopGuide() {
    if (G.run && G.run.box) G.run.box.remove();
    G.run = null;
  }

  function guideFrame(t) {
    const g = G.run;
    if (!g) return;
    if (comp() !== 'focus' || !g.s.node.isConnected || !g.s.node.classList.contains('focus')) { stopGuide(); return; }
    if (g.done) return;
    const c = ctl(g.idx), pair = linkedPair(c, g.ch, capsOf(unitType(g.idx))) || [g.ch];
    for (const ch of pair) {
      const v = peakOf(g.idx, ch);
      if (v > g.peak) g.peak = v;
      if (v >= -0.1) g.clip = true;
    }
    if (t - g.t0 >= GUIDE_MS) { g.done = true; renderGuide(); return; }
    renderGuide(t);
  }

  function renderGuide(t = performance.now()) {
    const g = G.run;
    if (!g) return;
    const who = stripWho(g.idx, g.ch);
    if (!g.done) {
      const left = Math.max(0, Math.ceil((GUIDE_MS - (t - g.t0)) / 1000));
      const txt = `${left}|${dbText(g.peak)}`;
      if (txt === g.last) return;
      g.last = txt;
      g.box.replaceChildren(
        el('p', { class: 'sg-head', text: `Speak or play normally into ${who} for 5 seconds.` }),
        el('p', { text: `${left} s left · loudest so far ${dbText(g.peak)} dBFS` }),
        el('button', { class: 'btn btn-quiet', type: 'button', onclick: e => { e.stopPropagation(); stopGuide(); } }, 'Stop'));
      return;
    }
    const inp = ctl(g.idx).inputs[g.ch], now0 = inp.gain;
    const locked = cfg.safety.lockGain;
    let msg, want = null;
    if (!(g.peak > -70)) {
      msg = `Nothing came in on ${who} in those 5 seconds. Check the cable and that the source is playing${!inp.line && !inp.p48 ? ', and turn on 48V if it is a condenser mic' : ''}.`;
    } else if (g.clip) {
      want = clamp(now0 - 12, 10, 65);
      msg = want < now0
        ? `It clipped: the loudest part reached 0 dBFS. Lower the gain to ${want} dB (now ${now0} dB), then try again.`
        : `It clipped even at the lowest gain.${inp.pad ? '' : ' Turn on PAD,'} move the mic back a little, or lower the source, then try again.`;
      if (want >= now0) want = null;
    } else {
      const ideal = Math.round(now0 + (GUIDE_TARGET - g.peak));
      want = clamp(ideal, 10, 65);
      if (Math.abs(want - now0) < 3) {
        msg = `Good level: the loudest was ${dbText(g.peak)} dBFS, about ${Math.round(-g.peak)} dB below clipping. Keep ${now0} dB.`;
        want = null;
      } else if (ideal > 65 && want === now0) {
        msg = `Too quiet even at the highest gain (loudest ${dbText(g.peak)} dBFS). Move closer to the mic${!inp.line && !inp.p48 ? ' or turn on 48V for a condenser mic' : ''}.`;
        want = null;
      } else if (ideal < 10 && want === now0) {
        msg = `Too loud even at the lowest gain (loudest ${dbText(g.peak)} dBFS).${inp.pad ? '' : ' Turn on PAD.'}`;
        want = null;
      } else {
        msg = `The loudest was ${dbText(g.peak)} dBFS. Set the gain to ${want} dB (now ${now0} dB) for peaks near ${GUIDE_TARGET} dBFS, with room for louder moments.`;
      }
    }
    const btns = [];
    if (want !== null && !locked) {
      btns.push(el('button', { class: 'btn btn-primary', type: 'button', onclick: e => {
        e.stopPropagation();
        setInput(g.ch, 'gain', want, { unit: g.idx });
        g.box.replaceChildren(el('p', { text: `Gain set to ${want} dB on ${who}.` }),
          el('button', { class: 'btn btn-quiet', type: 'button', onclick: ev => { ev.stopPropagation(); guide(g.idx, g.ch); } }, 'Check again'),
          el('button', { class: 'btn btn-quiet', type: 'button', onclick: ev => { ev.stopPropagation(); stopGuide(); } }, 'Close'));
      } }, `Set ${want} dB`));
    }
    btns.push(el('button', { class: 'btn btn-quiet', type: 'button', onclick: e => { e.stopPropagation(); guide(g.idx, g.ch); } }, 'Try again'),
      el('button', { class: 'btn btn-quiet', type: 'button', onclick: e => { e.stopPropagation(); stopGuide(); } }, 'Close'));
    g.box.replaceChildren(...[el('p', { text: msg }),
      locked && want !== null ? el('p', { class: 'fine', text: `Gain knobs are locked (Settings, Safety): unlock them to set ${want} dB.` }) : null,
      el('div', { class: 'sg-acts' }, btns)].filter(Boolean));
  }

  /* ================================================================== Arcade: HI-SCORE */

  const HI = { since: new Date(), best: new Map(), root: null, list: null, sinceEl: null, last: '', at: 0 };

  function hiFrame() {
    for (const sg of liveSegs()) {
      for (const s of sg.strips) {
        const v = peakOf(sg.idx, s.ch);
        if (!(v > -200)) continue;
        const k = `${sg.idx}:${s.ch}`, b = HI.best.get(k);
        if (!b || v > b.db + 0.05) HI.best.set(k, { db: v, at: new Date(), idx: sg.idx, ch: s.ch });
      }
    }
  }

  function hiReset() {
    HI.best.clear();
    HI.since = new Date();
    HI.last = '';
    renderHi(true);
  }

  function buildHi(desk, before) {
    HI.list = el('ol', { class: 'hs-list' });
    HI.sinceEl = el('span', { class: 'hs-since num' });
    HI.root = el('section', { class: 'hiscore', id: 'hiScore', 'aria-label': 'HI-SCORE: the highest peak of every input since reset', hidden: true },
      el('div', { class: 'hs-head' }, el('span', { class: 'hs-title', text: 'HI-SCORE' }), HI.sinceEl,
        el('button', { class: 'btn btn-quiet hs-reset', type: 'button', onclick: hiReset, title: 'Clear every high score' }, 'RESET')),
      HI.list);
    desk.insertBefore(HI.root, before);
  }

  function renderHi(force) {
    if (!HI.root) return;
    const show = onControl('cards');
    setHidden(HI.root, !show);
    if (!show) return;
    const rows = [...HI.best.values()].filter(b => segs.has(b.idx)).sort((a, b) => b.db - a.db).slice(0, 8);
    const key = rows.map(b => `${b.idx}:${b.ch}:${b.db.toFixed(1)}`).join(',');
    if (!force && key === HI.last) return;
    HI.last = key;
    HI.sinceEl.textContent = `SINCE ${clock(HI.since)}`;
    const RANK = ['1ST', '2ND', '3RD', '4TH', '5TH', '6TH', '7TH', '8TH'];
    HI.list.replaceChildren(...(rows.length ? rows.map((b, i) => {
      const name = (chanCfg(b.idx, b.ch) || {}).name || '';
      const clip = b.db >= -0.1;
      return el('li', { class: `hs-row${clip ? ' clip' : ''}`, title: `${stripWho(b.idx, b.ch)}: ${b.db.toFixed(1)} dBFS at ${clock(b.at)}` },
        el('span', { class: 'hs-rank', text: RANK[i] }),
        el('span', { class: 'hs-who', text: `P${playerOf(b.idx)} ${two(b.ch + 1)}${name ? ` ${name.slice(0, 8).toUpperCase()}` : ''}` }),
        el('span', { class: 'hs-db num', text: clip ? 'CLIP!' : b.db.toFixed(1) }),
        el('span', { class: 'hs-at num', text: clock(b.at) }));
    }) : [el('li', { class: 'hs-row hs-empty', text: 'INSERT SIGNAL' })]));
  }

  /* ================================================================== Lab: the measurement panel */

  const HIST_N = 3000, HIST_DT = 100;                 /* 5 minutes at 10 Hz */
  const LAB = { hist: new Map(), at: -1, n: 0, lastT: 0, avg: new Map(), win: 600, sel: '0:0', root: null,
    canvas: null, ctx: null, stats: null, selEl: null, logBtn: null, saveBtn: null, log: null, keys: [], drawn: 0 };

  function labSample(dt, t) {
    const list = cells();
    for (const c of list) {
      const v = peakOf(c.idx, c.slot), p = v > -200 ? Math.pow(10, v / 10) : 0;
      const a = LAB.avg.get(c.key) || 0;
      LAB.avg.set(c.key, a + (p - a) * Math.min(1, dt / 0.3));
    }
    if (t - LAB.lastT < HIST_DT) return;
    LAB.lastT = t;
    LAB.at = (LAB.at + 1) % HIST_N;
    LAB.n = Math.min(HIST_N, LAB.n + 1);
    const row = LAB.log ? [new Date().toISOString()] : null;
    for (const c of list) {
      let h = LAB.hist.get(c.key);
      if (!h) { h = { pk: new Float32Array(HIST_N).fill(-200), av: new Float32Array(HIST_N).fill(-200) }; LAB.hist.set(c.key, h); }
      const v = peakOf(c.idx, c.slot), a = LAB.avg.get(c.key) || 0;
      h.pk[LAB.at] = v > -200 ? v : -200;
      h.av[LAB.at] = a > 0 ? 10 * Math.log10(a) : -200;
    }
    if (row) {
      for (const k of LAB.log.keys) {
        const h = LAB.hist.get(k);
        row.push(h && h.pk[LAB.at] > -200 ? h.pk[LAB.at].toFixed(1) : '', h && h.av[LAB.at] > -200 ? h.av[LAB.at].toFixed(1) : '');
      }
      LAB.log.rows.push(row.join(','));
      if (LAB.log.rows.length >= 36000) { toast('Level log stopped after one hour. Save it as CSV.'); setLog(false); }
    }
  }

  function buildLab(desk, before) {
    LAB.canvas = el('canvas', { class: 'lb-hist', 'aria-hidden': 'true' });
    LAB.selEl = el('select', { class: 'select lb-sel num', 'aria-label': 'Channel shown in the level history', onchange: e => { LAB.sel = e.target.value; LAB.drawn = 0; } });
    const win = seg(['10 S', '60 S', '5 MIN'], i => { LAB.win = [100, 600, 3000][i]; win.set(i); LAB.drawn = 0; }, 'History window');
    win.set(1);
    LAB.logBtn = sw('LOG', { title: 'Log every input\'s peak and RMS, 10 times a second, until pressed again (up to an hour)', onClick: () => setLog(!LAB.log || !LAB.log.on) });
    LAB.saveBtn = el('button', { class: 'btn btn-quiet', type: 'button', disabled: true, onclick: saveLog, title: 'Save the level log as a CSV file' }, 'SAVE CSV');
    const measure = el('button', { class: 'btn btn-quiet lb-measure', type: 'button', onclick: measureOpen,
      title: 'Open the current routing (units, inputs, gains, main outs, sample rate) in Avium Measure for a sweep or a correction check' }, 'OPEN IN AVIUM MEASURE');
    LAB.stats = el('div', { class: 'lb-stats num', 'aria-live': 'off' });
    LAB.root = el('section', { class: 'lab-bench', id: 'labBench', 'aria-label': 'Measurement panel', hidden: true },
      el('div', { class: 'lb-bar' }, el('span', { class: 'lbl', text: 'MEASUREMENT' }), LAB.selEl, win.node, LAB.logBtn, LAB.saveBtn, measure),
      el('div', { class: 'lb-body' }, LAB.canvas, LAB.stats));
    desk.insertBefore(LAB.root, before);
  }

  function setLog(on) {
    if (on) {
      LAB.log = { on: true, keys: cells().map(c => c.key), labels: cells().map(c => c.label), rows: [], t0: new Date() };
      toast('Logging every input\'s peak and RMS, 10 times a second.');
    } else if (LAB.log) {
      LAB.log.on = false;
      LAB.log.t1 = new Date();
    }
    renderLabBar();
  }

  function renderLabBar() {
    if (!LAB.logBtn) return;
    const on = !!(LAB.log && LAB.log.on);
    setPressed(LAB.logBtn, on);
    LAB.saveBtn.disabled = !(LAB.log && LAB.log.rows.length);
  }

  function saveLog() {
    if (!LAB.log || !LAB.log.rows.length) return;
    const head = ['time', ...LAB.log.labels.flatMap(l => [`${l} peak dBFS`, `${l} rms dBFS`])].join(',');
    const d = LAB.log.t0;
    download(`apollo-levels-${d.getFullYear()}${two(d.getMonth() + 1)}${two(d.getDate())}-${two(d.getHours())}${two(d.getMinutes())}${two(d.getSeconds())}.csv`,
      `${head}\n${LAB.log.rows.join('\n')}\n`, 'text/csv');
  }

  function download(name, text, type) {
    const a = el('a', { href: URL.createObjectURL(new Blob([text], { type })), download: name });
    document.body.append(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }

  /* the routing Avium Measure needs for a sweep or a correction check: units, rate, inputs, main outs */
  function routing() {
    const units = liveSegs().map(sg => {
      const i = unitInfo(sg.idx) || {}, c = ctl(sg.idx), r = typeof rateFor === 'function' ? rateFor(sg.idx) : null;
      return {
        slot: sg.idx, model: unitModel(sg.idx), serial: i.serial || '', id: typeof unitIdOf === 'function' ? unitIdOf(sg.idx) || '' : '',
        rate: (r && r.rate) || i.sampleRate || 48000,
        inputs: sg.strips.map(s => {
          const inp = c.inputs[s.ch] || {};
          return { ch: s.ch + 1, name: (chanCfg(sg.idx, s.ch) || {}).name || '', gain: inp.gain, line: !!inp.line, p48: !!inp.p48, pad: !!inp.pad,
            lowcut: !!inp.lowcut, phase: !!inp.phase, insert: Plug.insertLabel(sg.idx, s.ch) || '' };
        }),
        mainOut: { loopbackSlots: monSlots(sg.idx), mute: !!c.mon.mute, dim: !!c.mon.dim, levelDb: rawToDb(c.mon.level) },
      };
    });
    const [fi, fc] = LAB.sel.split(':');
    return { format: 'apollo-routing', version: 1, created: new Date().toISOString(), app: `Apollo Control ${appVersion}`,
      combined: !!(typeof rateAgg === 'function' && rateAgg()), focus: { slot: Number(fi), input: /^\d+$/.test(fc) ? Number(fc) + 1 : fc }, units };
  }

  function measureOpen() {
    const r = routing();
    if (S.demo || !host) {
      download('apollo-routing.json', `${JSON.stringify(r, null, 1)}\n`, 'application/json');
      toast('Demo: the routing was saved as apollo-routing.json. With the app connected it opens in Avium Measure.');
      return;
    }
    host.postMessage(JSON.stringify({ cmd: 'measure-open', routing: r }));
  }

  function onMeasure(m) {
    if (m.launched) toast('Avium Measure opened with this routing.');
    else if (m.ok) toast(`The routing was written to ${m.file}. ${m.why || 'Avium Measure was not found.'}`, 'warn', 8000);
    else toast(`The routing could not be written: ${m.why || 'unknown error'}.`, 'warn', 8000);
  }

  function renderLab(t) {
    if (!LAB.root) return;
    const show = onControl('panels');
    setHidden(LAB.root, !show);
    if (!show) return;
    const list = cells();
    const opts = list.map(c => `${c.key}=${c.label}${c.name ? ` ${c.name}` : ''}`).join('|');
    if (opts !== LAB.selEl.dataset.opts) {
      LAB.selEl.dataset.opts = opts;
      LAB.selEl.replaceChildren(...list.map(c => el('option', { value: c.key, text: `${c.label}${c.name ? ` ${c.name}` : ''}` })));
      if (!list.some(c => c.key === LAB.sel) && list.length) LAB.sel = list[0].key;
      LAB.selEl.value = LAB.sel;
    }
    renderLabBar();
    if (t - LAB.drawn < 100) return;
    LAB.drawn = t;
    drawHist();
  }

  function drawHist() {
    const cv = LAB.canvas, w = cv.clientWidth, h = cv.clientHeight, dpr = window.devicePixelRatio || 1;
    if (!w || !h) return;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
    const g = cv.getContext('2d'), cs = getComputedStyle(document.documentElement);
    const col = n => cs.getPropertyValue(n).trim() || '#888';
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    const floor = -60, L = 30, y = db => 4 + (h - 8) * (1 - (Math.max(floor, Math.min(0, db)) - floor) / -floor);
    g.font = `10px ${col('--mono') || 'monospace'}`;
    g.fillStyle = col('--text-faint');
    g.strokeStyle = col('--line');
    g.lineWidth = 1;
    for (const d of [0, -6, -12, -18, -24, -40, -60]) {
      const yy = Math.round(y(d)) + 0.5;
      g.globalAlpha = d === -18 ? 0.9 : 0.5;
      g.beginPath(); g.moveTo(L, yy); g.lineTo(w, yy); g.stroke();
      g.globalAlpha = 1;
      g.fillText(String(d), 2, yy + 3);
    }
    const hst = LAB.hist.get(LAB.sel), n = Math.min(LAB.win, LAB.n);
    const st = { pk: -200, pow: 0, cnt: 0, clips: 0 };
    if (hst && n > 1) {
      const xw = (w - L) / (LAB.win - 1), x0 = w - (n - 1) * xw;
      const at = i => (LAB.at - (n - 1 - i) + HIST_N) % HIST_N;
      g.beginPath();
      g.moveTo(x0, h);
      for (let i = 0; i < n; i++) g.lineTo(x0 + i * xw, y(hst.av[at(i)]));
      g.lineTo(w, h);
      g.closePath();
      g.globalAlpha = 0.35;
      g.fillStyle = col('--accent');
      g.fill();
      g.globalAlpha = 1;
      g.beginPath();
      for (let i = 0; i < n; i++) {
        const v = hst.pk[at(i)];
        if (i === 0) g.moveTo(x0, y(v)); else g.lineTo(x0 + i * xw, y(v));
        if (v > st.pk) st.pk = v;
        if (v > -200) { st.pow += Math.pow(10, hst.av[at(i)] / 10); st.cnt++; }
        if (v >= -0.1) st.clips++;
      }
      g.strokeStyle = col('--text');
      g.stroke();
    }
    const rms = st.cnt && st.pow > 0 ? 10 * Math.log10(st.pow / st.cnt) : -200;
    const c = cells().find(x => x.key === LAB.sel);
    const logTxt = LAB.log ? `${LAB.log.on ? 'LOGGING' : 'LOGGED'} ${LAB.log.rows.length} ROWS` : 'NOT LOGGING';
    const txt = `${c ? c.label : '--'}\nPEAK MAX ${dbText(st.pk)} dBFS\nRMS ${dbText(rms)} dBFS\nCREST ${st.pk > -200 && rms > -200 ? (st.pk - rms).toFixed(1) : '--'} dB\nCLIPS ${st.clips}\nWINDOW ${LAB.win / 10} S\n${logTxt}`;
    if (LAB.stats.textContent !== txt) LAB.stats.textContent = txt;
  }

  /* ================================================================== Cyberpunk: the signal-flow overlay */

  const FLOW = { on: true, canvas: null, dirty: true, geo: [], w: 0, h: 0, at: 0, colors: null, btns: [], lastStatic: 0, geoKey: '', drawn: '' };

  function buildFlow(desk) {
    FLOW.canvas = el('canvas', { class: 'flow-canvas', 'aria-hidden': 'true', hidden: true });
    FLOW.canvas.addEventListener('contextrestored', () => { FLOW.drawn = ''; });   /* a GPU reset blanks it: draw again */
    desk.append(FLOW.canvas);
    const inputs = document.getElementById('inputs');
    inputs.addEventListener('scroll', () => { FLOW.dirty = true; }, { passive: true });
    new ResizeObserver(() => { FLOW.dirty = true; }).observe(inputs);
    try { FLOW.on = localStorage.getItem('apollo-flow') !== '0'; } catch (e) { FLOW.on = true; }
  }

  /* a segment head's FLOW switch (the hud composition shows it) */
  function headTools(idx) {
    const b = sw('FLOW', { cls: 'sig-flow-btn', title: 'Signal flow: packets show where the sound goes and where it stops', onClick: () => setFlow(!FLOW.on) });
    setPressed(b, FLOW.on);
    FLOW.btns.push(b);
    return b;
  }

  function setFlow(on) {
    FLOW.on = !!on;
    try { localStorage.setItem('apollo-flow', on ? '1' : '0'); } catch (e) { /* not kept */ }
    FLOW.btns = FLOW.btns.filter(b => b.isConnected);
    for (const b of FLOW.btns) setPressed(b, FLOW.on);
    FLOW.dirty = true;
  }

  function flowGeometry() {
    const inputs = document.getElementById('inputs'), desk = document.getElementById('desk');
    const ir = inputs.getBoundingClientRect(), dr = desk.getBoundingClientRect();
    const cv = FLOW.canvas, dpr = window.devicePixelRatio || 1;
    Object.assign(cv.style, { left: `${ir.left - dr.left}px`, top: `${ir.top - dr.top}px`, width: `${ir.width}px`, height: `${ir.height}px` });
    if (cv.width !== Math.round(ir.width * dpr) || cv.height !== Math.round(ir.height * dpr)) {
      cv.width = Math.round(ir.width * dpr); cv.height = Math.round(ir.height * dpr);
    }
    FLOW.w = ir.width; FLOW.h = ir.height;
    const mid = n => { const r = n.getBoundingClientRect(); return r.width ? [r.left - ir.left + r.width / 2, r.top - ir.top + r.height / 2] : null; };
    FLOW.geo = [];
    for (const sg of liveSegs()) {
      if (!sg.master) continue;
      const body = sg.body.getBoundingClientRect(), bus = body.bottom - ir.top - 5;
      const out = mid(sg.master.node.querySelector('.mo-bars') || sg.master.node);
      if (!out) continue;
      for (const s of sg.strips) {
        if (s.node.hidden) continue;
        const top = s.node.querySelector('.strip-top'), slot = s.node.querySelector('.st-ins .ins-slot') || s.node.querySelector('.st-ins');
        const pts = [top && mid(top), mid(s.gain.node), slot && mid(slot), mid(s.mix.fader.node)];
        if (pts.some(p => !p)) continue;
        pts[0][1] -= 4;
        const d = pts[3];
        pts.push([d[0], bus], [out[0], bus], out);
        const len = [0];
        for (let i = 1; i < pts.length; i++) len.push(len[i - 1] + Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]));
        FLOW.geo.push({ idx: sg.idx, ch: s.ch, pts, len });
      }
    }
    const cs = getComputedStyle(document.documentElement), v = n => cs.getPropertyValue(n).trim();
    FLOW.colors = { line: v('--line-strong') || '#444', ok: v('--ok') || '#0ff', acc: v('--accent') || '#f0f', hot: v('--red') || '#f33' };
    FLOW.geoKey = JSON.stringify([cv.width, cv.height, dpr, FLOW.colors, FLOW.geo.map(g => g.pts)]);
    FLOW.dirty = false;
    FLOW.at = performance.now();
  }

  const pointAt = (g, d) => {
    let i = 1;
    while (i < g.len.length - 1 && g.len[i] < d) i++;
    const a = g.pts[i - 1], b = g.pts[i], seg0 = g.len[i] - g.len[i - 1] || 1, f = (d - g.len[i - 1]) / seg0;
    return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f];
  };

  /* where this strip's sound stops: the mix (muted, off, or another input soloed), the main out (monitor muted), or not */
  function stopOf(g) {
    const m = mixOfBus(g.idx, g.ch) || {}, c = ctl(g.idx);
    const solo = capsOf(unitType(g.idx)).preamps && [...Array(capsOf(unitType(g.idx)).preamps).keys()].some(b => (mixOfBus(g.idx, b) || {}).solo);
    if (m.mute || m.level <= SEND_OFF || (solo && !m.solo)) return { at: g.len[3], why: 'mix' };
    if (c.mon && c.mon.mute) return { at: g.len[5], why: 'out' };
    return null;
  }

  function flowFrame(t) {
    const cv = FLOW.canvas;
    if (!cv) return;
    const show = FLOW.on && onControl('hud');
    setHidden(cv, !show);
    if (!show) return;
    const still = Skin.R.reduced();
    if (still && t - FLOW.lastStatic < 250) return;      /* reduced motion: a still picture, 4 times a second */
    FLOW.lastStatic = t;
    if (FLOW.dirty || t - FLOW.at > 1000) flowGeometry();
    /* silence everywhere: only the routes, which are on the canvas already unless the geometry moved */
    const quiet = FLOW.geo.every(p => !(peakOf(p.idx, p.ch) > -60));
    if (quiet && FLOW.drawn === FLOW.geoKey) return;
    FLOW.drawn = quiet ? FLOW.geoKey : '';
    const g = cv.getContext('2d'), dpr = window.devicePixelRatio || 1, C = FLOW.colors;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, FLOW.w, FLOW.h);
    for (const p of FLOW.geo) {
      const lv = peakOf(p.idx, p.ch), n = lv > -60 ? Math.min(1, (lv + 60) / 60) : 0;
      const stop = stopOf(p), end = stop ? stop.at : p.len[p.len.length - 1];
      g.globalAlpha = 0.35;
      g.strokeStyle = C.line;
      g.lineWidth = 1;
      g.beginPath();
      p.pts.forEach((q, i) => (i ? g.lineTo(q[0], q[1]) : g.moveTo(q[0], q[1])));
      g.stroke();
      g.globalAlpha = 1;
      if (stop && n > 0) {                                /* the stop: a small cross where the sound ends */
        const [x, y] = pointAt(p, end);
        g.strokeStyle = C.hot;
        g.lineWidth = 1.5;
        g.beginPath(); g.moveTo(x - 4, y - 4); g.lineTo(x + 4, y + 4); g.moveTo(x + 4, y - 4); g.lineTo(x - 4, y + 4); g.stroke();
      }
      if (n <= 0) continue;
      const speed = 70 + 330 * n, gap = Math.max(14, 80 - 62 * n), off = still ? 0 : ((t / 1000) * speed) % gap;
      g.fillStyle = lv > -6 ? C.hot : lv > -18 ? C.acc : C.ok;
      const sz = n > 0.75 ? 5 : 4;
      for (let d = off; d <= end; d += gap) {
        const [x, y] = pointAt(p, d);
        g.fillRect(x - sz / 2, y - sz / 2, sz, sz);
      }
    }
  }

  /* ================================================================== the frame and the boards */

  let lastText = 0;
  function frame(dt, t) {
    hiFrame();
    labSample(dt, t);
    guideFrame(t);
    flowFrame(t);
    const hiShow = !!HI.root && onControl('cards');
    if (HI.root && HI.root.hidden === hiShow) renderHi(true);          /* shown or hidden at once with its composition */
    else if (t - lastText > 250) { lastText = t; renderHi(false); }
    renderLab(t);
  }

  function build() {
    const desk = document.getElementById('desk'), inputs = document.getElementById('inputs');
    if (!desk || !inputs || HI.root) return;
    buildHi(desk, inputs);
    buildLab(desk, inputs);
    buildFlow(desk);
  }

  return {
    build, frame, guide, stopGuide, headTools, setFlow, hiReset, measureOpen, onMeasure, setLog, saveLog,
    get flowOn() { return FLOW.on; },
    get hiScores() { return [...HI.best.values()]; },
    get logging() { return !!(LAB.log && LAB.log.on); },
  };
})();
