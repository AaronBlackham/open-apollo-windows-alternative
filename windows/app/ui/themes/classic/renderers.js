'use strict';
/* Classic: the standard console-app layout (docs/THEMES.md 2), for people who already know how a mixer app for their
   interface is laid out. No metaphor on purpose; the arrangement follows the usual one:
     a View column on the left    strip views (OVERVIEW / INPUTS / INSERTS / SENDS), SHOW switches, CLEAR CLIPS,
                                  CLEAR SOLO, SETTINGS at the foot (ClassicDesk below);
     the input strips             input select, the preamp switches, the gain with the Unison slot right under it,
                                  the inserts, the sends (AUX 1, AUX 2, CUE 1, CUE 2), pan, solo and mute, the fader with
                                  the meter beside it, the value, the channel label at the foot;
     the aux returns after them, and each unit's monitor section at its right (meters on top, the monitor knob below);
     the control room column on the far right, the info bar (sample rate) along the bottom; the plug-in browser opens
     from an empty insert instead of standing beside the strips.
   Controls: flat rotary knobs with a value arc and a pointer (vertical drag, Shift fine, double-click resets, wheel);
   faders with a flat cap on a thin slot (drag the cap, press the slot or the scale to jump, double-click for 0 dB);
   rectangular buttons that light when on; solid green / yellow / red meters. Only short state transitions move.
   All artwork here is drawn by this pack (SVG and CSS); nothing is taken from any other software. */

/* [Classic] The desk furniture (app.js applyAppearance calls desk.on() whenever the look is applied while this pack is
   on, desk.off() when another pack takes over). Everything it adds drives the model functions the other views use
   (setAuxOpen, setSend, setMix, the meters' reset, showView, Plug.focusSearch): it adds ways in and takes none away.
   The strip views only show or hide sections; OVERVIEW, the default, shows every control. */
const ClassicDesk = (() => {
  const VIEWS = [['overview', 'OVERVIEW'], ['inputs', 'INPUTS'], ['inserts', 'INSERTS'], ['sends', 'SENDS']];
  const D = { on: false, col: null, timer: 0, cues: new Map(), soloMem: [], obs: null, picking: false, lastSlot: null, keyOff: null };
  const vc = () => document.getElementById('view-control');
  const state = () => cfg.classic || (cfg.classic = { view: 'overview', ctrlRoom: true });
  const fmtSendShort = v => (v <= SEND_OFF ? 'off' : (v / 2).toFixed(1));

  /* every soloed bus of every live unit, as [unit, bus] */
  function soloed() {
    const out = [];
    for (const sg of segs.values()) {
      if (sg.gone) continue;
      for (const b of mixBuses(sg.k)) if (mixOfBus(sg.idx, b).solo) out.push([sg.idx, b]);
    }
    return out;
  }
  /* CLEAR SOLO: every solo off; pressed again before any new solo, the cleared ones come back */
  function clearSolo() {
    const on = soloed();
    if (on.length) { D.soloMem = on; for (const [idx, b] of on) setMix(b, 'solo', false, idx); }
    else if (D.soloMem.length) { for (const [idx, b] of D.soloMem) if (segs.has(idx)) setMix(b, 'solo', true, idx); D.soloMem = []; }
    tick();
  }
  function setView(v) { state().view = v; saveConfig(); apply(); }
  function togglePlb() {
    const v = vc(), open = v.dataset.clPlb !== '1';
    v.dataset.clPlb = open ? '1' : '0';
    if (open) Plug.focusSearch();
    else {
      const plb = document.getElementById('plb');
      if (plb && plb.classList.contains('picking')) Plug.escape();
      if (plb && plb.contains(document.activeElement)) D.plbBtn.focus();
    }
    syncCol();
  }

  function build(v) {
    D.view = seg(VIEWS.map(x => x[1]), i => setView(VIEWS[i][0]), 'Strip view');
    D.view.node.classList.add('cl-vc-views');
    D.aux = sw('AUX', { aria: 'Show the AUX return strips', title: 'Show or hide the AUX 1 / AUX 2 return strips',
      onClick: () => { setAuxOpen(!cfg.auxStrips.open); syncCol(); } });
    D.ctrl = sw('CTRL ROOM', { aria: 'Show the control room column', title: 'Show or hide the control room column: source, headphones, ALT speakers, outputs, talkback',
      onClick: () => { const s = state(); s.ctrlRoom = s.ctrlRoom === false; saveConfig(); apply(); } });
    D.plbBtn = sw('PLUG-INS', { aria: 'Show the plug-in browser', title: 'Show or hide the plug-in browser (a click on an empty insert opens it too)', onClick: togglePlb });
    D.clips = el('button', { class: 'btn cl-vc-btn', type: 'button', title: 'Clear every clip light and peak hold',
      onclick: () => { for (const m of METERS) m.reset(); } }, 'CLEAR CLIPS');
    D.soloN = el('span', { class: 'cl-count num', 'aria-hidden': 'true' });
    D.solo = el('button', { class: 'btn cl-vc-btn cl-solo', type: 'button', onclick: clearSolo }, el('span', { text: 'CLEAR SOLO' }), D.soloN);
    D.settings = el('button', { class: 'btn cl-vc-btn', type: 'button', title: 'Settings', onclick: () => showView('settings') }, 'SETTINGS');
    D.col = el('nav', { class: 'cl-viewcol', id: 'clViewCol', 'aria-label': 'Mixer view' },
      el('div', { class: 'cl-vc-lbl', 'aria-hidden': 'true', text: 'View' }), D.view.node,
      el('div', { class: 'cl-vc-lbl', 'aria-hidden': 'true', text: 'Show' }), D.aux, D.ctrl, D.plbBtn,
      el('div', { class: 'cl-vc-gap' }), D.clips, D.solo, D.settings);
    v.prepend(D.col);
    /* the browser opens from an empty insert (it has .picking while it waits for a choice); once the plug-in is placed,
       or Esc, focus goes back to that insert so the panel closes again. Esc with nothing being placed closes it too. */
    const plb = document.getElementById('plb');
    if (!plb) return;
    D.obs = new MutationObserver(() => {
      const now = plb.classList.contains('picking');
      if (now && !D.picking) D.lastSlot = document.querySelector('#inputs .ins-slot.target');
      if (!now && D.picking && vc().dataset.clPlb !== '1' && plb.contains(document.activeElement)) {
        if (D.lastSlot && D.lastSlot.isConnected) D.lastSlot.focus(); else document.activeElement.blur();
      }
      D.picking = now;
    });
    D.obs.observe(plb, { attributes: true, attributeFilter: ['class'] });
    const key = e => {
      if (e.key !== 'Escape' || plb.classList.contains('picking') || vc().dataset.clPlb === '1') return;
      const s = plb.querySelector('.plb-search');
      if (s && e.target === s && s.value) return;                        /* the first Esc clears the search */
      if (plb.contains(document.activeElement)) { document.activeElement.blur(); e.preventDefault(); }
    };
    plb.addEventListener('keydown', key, true);
    D.keyOff = () => plb.removeEventListener('keydown', key, true);
  }

  /* the strip views, the control room column and the browser panel: data attributes on the Control view */
  function apply() {
    const v = vc(), s = state();
    if (!v) return;
    v.dataset.clView = VIEWS.some(x => x[0] === s.view) ? s.view : 'overview';
    v.dataset.clCtrl = s.ctrlRoom === false ? '0' : '1';
    if (v.dataset.clPlb !== '1') v.dataset.clPlb = '0';
    syncCol();
  }

  function syncCol() {
    if (!D.col) return;
    const s = state(), v = vc();
    D.view.set(Math.max(0, VIEWS.findIndex(x => x[0] === v.dataset.clView)));
    setPressed(D.aux, !!cfg.auxStrips.open);
    setPressed(D.ctrl, s.ctrlRoom !== false);
    setPressed(D.plbBtn, v.dataset.clPlb === '1');
    const n = soloed().length;
    D.solo.classList.toggle('cl-lit', n > 0);
    put(D.soloN, n ? String(n) : '');
    const aria = n ? `Clear solo: ${n} soloed` : D.soloMem.length ? 'Clear solo: none soloed, press to bring back the cleared solos' : 'Clear solo: none soloed';
    if (D.solo.getAttribute('aria-label') !== aria) { D.solo.setAttribute('aria-label', aria); D.solo.title = aria; }
  }

  /* each input strip's sends to the unit's cues, beside AUX 1 / AUX 2 (OVERVIEW shows the first two, SENDS every one) */
  function syncCues() {
    for (const sg of segs.values()) {
      if (sg.gone) continue;
      for (const s of sg.strips) {
        let rec = D.cues.get(s.node);
        if (!rec) {
          const grid = s.node.querySelector('.st-aux .aux-sends');
          if (!grid) continue;
          rec = [];
          sg.k.cues.forEach((name, c) => {
            const kn = new Knob({
              size: 22, min: SEND_OFF, max: SEND_MAX, step: 1, wheelStep: 2, pageStep: 12, value: sendOf(sg.idx, c, s.ch).level, label: name,
              aria: `${stripWho(sg.idx, s.ch)} send to ${name}`, toPos: sendToPos, fromPos: sendFromPos,
              format: fmtSendShort, editText: fmtSendShort, parse: parseSend, onChange: x => setSend(c, s.ch, 'level', x, sg.idx),
            });
            const cell = el('div', { class: 'aux-send cl-cue', dataset: { cue: String(c) } }, kn.node);
            grid.append(cell);
            rec.push({ kn, c, cell });
          });
          D.cues.set(s.node, rec);
        }
        for (const { kn, c } of rec) {
          const lv = sendOf(sg.idx, c, s.ch).level;
          if (kn.value !== lv && !kn.dragging && document.activeElement !== kn.node) kn.set(lv);
          kn.node.classList.toggle('off', lv <= SEND_OFF);
          kn.node.classList.toggle('refused', refusedOn(sg.idx, `cue:send:${c + 1}:${s.ch}`));
        }
      }
    }
    for (const node of [...D.cues.keys()]) if (!node.isConnected) D.cues.delete(node);
  }

  /* a quarter-second look while the Control view is on screen (cue sends, solos and AUX can change from elsewhere) */
  function tick() {
    if (!D.on || (S.view !== 'control' && D.cues.size)) return;
    syncCues();
    syncCol();
  }

  function on() {
    const v = vc();
    if (!v || typeof segs === 'undefined') return;
    if (!D.col || !D.col.isConnected) build(v);
    D.on = true;
    apply();
    if (!D.timer) D.timer = setInterval(tick, 250);
    tick();
  }

  function off() {
    D.on = false;
    clearInterval(D.timer);
    D.timer = 0;
    if (D.obs) { D.obs.disconnect(); D.obs = null; }
    if (D.keyOff) { D.keyOff(); D.keyOff = null; }
    if (D.col) D.col.remove();
    D.col = null;
    for (const rec of D.cues.values()) for (const r of rec) r.cell.remove();
    D.cues.clear();
    const v = vc();
    if (v) { delete v.dataset.clView; delete v.dataset.clCtrl; delete v.dataset.clPlb; }
  }

  return { on, off, tick };
})();

Skin.register('classic', {
  knob: {
    build(k, R) {
      /* never under 24 px: the pan and send knobs (22 px elsewhere) are drawn at 24 so they stay a full target */
      const S = R.svg, sz = Math.max(24, k.o.size), small = sz <= 30;
      /* small knobs (pan, sends) get thicker strokes so the arc and the pointer stay readable at 18-22 px */
      const sw = small ? 11 : 8, rArc = small ? 43 : 44, rBody = small ? 27 : 32;
      const g = S('svg', { class: 'sk-classic cl-knob', width: sz, height: sz, viewBox: '0 0 100 100', 'aria-hidden': 'true' });
      const track = S('path', { class: 'cl-k-track', 'stroke-width': sw, d: R.arc(50, 50, rArc, -135, 135) });
      const val = S('path', { class: 'cl-k-val', 'stroke-width': sw });
      const body = S('circle', { class: 'cl-k-body', cx: 50, cy: 50, r: rBody });
      const ptr = S('line', { class: 'cl-k-ptr', 'stroke-width': small ? 10 : 7 });
      g.append(track, val, body, ptr);
      /* the send knobs' short names in full, as a mixer prints them: A1 -> AUX 1, C1 -> CUE 1 (visual only) */
      if (k.o.label) k.node.append(R.deco('div', 'k-label', String(k.o.label).replace(/^A(\d)$/, 'AUX $1').replace(/^C(\d)$/, 'CUE $1')));
      k.node.append(g, k.readout);
      const off = R.vertical(k, g);
      return {
        draw({ p, p0 }) {
          const a = -135 + p * 270, b = -135 + p0 * 270;
          val.setAttribute('d', Math.abs(p - p0) > 0.002 ? R.arc(50, 50, rArc, Math.min(a, b), Math.max(a, b)) : '');
          const [x0, y0] = R.polar(50, 50, small ? 4 : 10, a), [x1, y1] = R.polar(50, 50, rBody - 2, a);
          ptr.setAttribute('x1', x0.toFixed(2)); ptr.setAttribute('y1', y0.toFixed(2));
          ptr.setAttribute('x2', x1.toFixed(2)); ptr.setAttribute('y2', y1.toFixed(2));
        },
        destroy: off,
      };
    },
  },
  fader: {
    build(f) {
      f.cap = el('div', { class: 'fd-cap cl-cap' });
      f.track = el('div', { class: 'fd-track cl-track sk-classic' }, f.cap);
      const scale = f.scaleNode();
      scale.classList.add('cl-scale');
      /* a press on the scale moves the fader there (the slot itself jumps too: Fader.trackDrag) */
      const jump = e => {
        if (e.button !== 0) return;
        const r0 = f.track.getBoundingClientRect();
        f.node.focus();
        f.commitPos(clamp((r0.bottom - e.clientY) / Math.max(1, r0.height), 0, 1));
      };
      scale.addEventListener('pointerdown', jump);
      f.node.append(scale, f.track);
      const off = f.trackDrag(f.track);
      return {
        draw({ p }) { f.cap.style.bottom = `${(p * 100).toFixed(2)}%`; },
        destroy() { off(); scale.removeEventListener('pointerdown', jump); },
      };
    },
  },
  /* solid bars on the shared fill (green to -18, yellow to -6, red above), a clip box on top, a hold line */
  meter: { build(m) { m.meter.classList.add('cl-meter'); return { paint() {} }; } },
  /* buttons, selectors and slots: the shared markup, styled in theme.css; nothing added */
  button: { decorate() {} },
  selector: { decorate(n) { n._skinSet = null; } },
  slot: { decorate() {} },
  browser: { layout: 'list' },
  params: { layout: 'grid' },
  on: {
    drop(node, R) { R.pulse(node, 'cl-dropped', 450); },
    refuse(node, R) { R.pulse(node, 'cl-refuse', 450); },
  },
  /* [Classic] the View column and the strips' cue sends (ClassicDesk above) */
  desk: ClassicDesk,
  /* [Sess] the session list in plain words (sessions.js layout "plain"), at desktop density in theme.css */
  sessions: { layout: 'plain' },
});
