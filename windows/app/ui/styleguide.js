'use strict';
/*
 * The style guide (docs/THEMES.md 2): every control in every state, drawn by the current theme pack with the real model
 * classes (Knob, Fader, Meter, sw, seg, the plug-in slot), so a theme is reviewed, and its screenshots taken, here.
 * Open it from Settings > Appearance, ?view=styleguide, or the command palette. Nothing on it sends anything.
 */
const StyleGuide = (() => {
  let root = null;
  const nop = () => {};
  const row = (title, ...kids) => el('div', { class: 'sg-row' }, el('div', { class: 'sg-label lbl', text: title }), el('div', { class: 'sg-items' }, kids));
  const cell = (cap, node) => el('div', { class: 'sg-cell' }, node, el('div', { class: 'sg-cap', text: cap }));

  function knob(cap, v, o = {}, state) {
    const k = new Knob({ size: o.size || 52, min: 0, max: 100, step: 1, pageStep: 10, value: v, aria: `Style guide ${cap}`, format: x => `${x}%`,
      editText: String, parse: x => Number(x), onChange: nop, origin: o.origin, reset: o.reset, label: o.label });
    if (state === 'waiting') k.setUnconfirmed(true);
    if (state === 'refused') k.node.classList.add('refused');
    if (state === 'locked') k.setLocked(true);
    return cell(cap, k.node);
  }
  function fader(cap, v, state) {
    const f = new Fader({ aria: `Style guide fader ${cap}`, value: v, onChange: nop });
    if (state === 'waiting') f.setUnconfirmed(true);
    if (state === 'refused') f.node.classList.add('refused');
    return cell(cap, el('div', { class: 'sg-fader' }, f.node, f.readout));
  }
  function button(cap, label, pressed, o = {}) {
    const b = sw(label, { red: o.red, big: o.big, cls: o.cls });
    setPressed(b, pressed);
    if (o.state === 'waiting') b.classList.add('unconfirmed');
    if (o.state === 'refused') b.classList.add('refused');
    if (o.disabled) b.disabled = true;
    return cell(cap, b);
  }
  function selector(cap, options, i, state) {
    const s = seg(options, nop, `Style guide ${cap}`);
    s.set(i);
    if (state === 'refused') s.node.classList.add('refused');
    return cell(cap, el('div', { class: 'sg-seg' }, s.node));
  }
  let meters = [];
  function meter(cap, db, o = {}) {
    const m = new Meter({ scale: true, group: 'sg-static', slot: 0 });
    meters.push(m);
    const t = performance.now();
    for (let i = 0; i < 40; i++) m.update(o.peak ?? db, 0.03, t + i * 30);
    m.update(db, 0.03, t + 1300);
    if (o.noclock) m.setNoClock(true);
    return cell(cap, el('div', { class: 'sg-meter' }, m.node));
  }
  function slot(cap, state, name, o = {}) {
    const b = el('button', { class: 'ins-slot', type: 'button', dataset: { state, refused: o.refused ? '1' : '0', ch: '0', unit: 'sg' }, 'aria-label': `Style guide slot: ${cap}` },
      el('span', { class: 'ins-lamp', 'aria-hidden': 'true' }), el('span', { class: 'ins-name', text: name }), el('span', { class: 'ins-why', 'aria-hidden': 'true', text: o.why || '' }));
    if (o.drop) b.dataset.drop = o.drop;
    Skin.decorate('slot', b);
    return cell(cap, el('div', { class: 'sg-slot ins-slots' }, el('div', { class: 'ins-row' }, b)));
  }
  const word = (cap, cls, text) => cell(cap, el('span', { class: `sg-state ${cls}` }, el('span', { class: 'ins-lamp', 'aria-hidden': 'true' }), el('span', { text })));

  function build() {
    root = document.getElementById('sg');
    if (!root) return;
    for (const m of meters) { const i = METERS.indexOf(m); if (i >= 0) METERS.splice(i, 1); }
    meters = [];
    const m = Skin.manifest();
    root.replaceChildren(
      el('div', { class: 'panel sg-head' },
        el('div', { class: 'sect-title', text: `Style guide · ${m.name}` }),
        el('p', { class: 'desc', text: `${m.metaphor || ''} ${m.desc || ''}`.trim() }),
        el('p', { class: 'fine', text: 'Every control in every state, drawn by this theme. Nothing here sends anything. Ctrl+Shift+T steps through the themes.' })),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'Knobs' }),
        row('States', knob('normal', 40), knob('centre (pan)', 50, { origin: 0.5, reset: 50 }), knob('waiting', 62, {}, 'waiting'),
          knob('refused', 30, {}, 'refused'), knob('locked', 70, {}, 'locked')),
        row('Sizes', knob('22 px', 50, { size: 22, origin: 0.5 }), knob('44 px', 25, { size: 44 }), knob('64 px', 75, { size: 64 }))),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'Faders' }),
        row('States', fader('0 dB', 0), fader('off', SEND_OFF), fader('+12 dB', SEND_MAX), fader('-18 dB, waiting', -36, 'waiting'), fader('refused', -12, 'refused'))),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'Buttons and selectors' }),
        row('Switches', button('off', 'PAD', false), button('on', 'PAD', true), button('48V on', '48V', true, { red: true }),
          button('waiting', 'LO CUT', true, { state: 'waiting' }), button('refused', 'LINK', false, { state: 'refused' }), button('disabled', 'MONO', false, { disabled: true }),
          button('big', 'MUTE', true, { red: true, big: true }), button('momentary', 'TALKBACK', false, { big: true, cls: 'talk' })),
        row('Selectors', selector('two', ['MIC', 'LINE'], 0), selector('five', ['MIX', 'CUE 1', 'CUE 2', 'CUE 3', 'CUE 4'], 2), selector('refused', ['CUE 1', 'CUE 2'], 1, 'refused'))),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'Meters' }),
        row('Levels', meter('silent', -120), meter('-36 dBFS', -36), meter('-18 dBFS', -18), meter('-6 dBFS', -6), meter('clip, held', -12, { peak: 0 }), meter('no clock', -120, { noclock: true }))),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'Plug-in slots' }),
        row('States', slot('empty', 'empty', 'INSERT'), slot('running', 'running', 'Precision De-Esser'), slot('loading', 'loading', 'Lexicon 480L'),
          slot('waiting', 'waiting', 'UA 1176LN'), slot('bypassed', 'bypassed', 'Pultec EQP-1A'), slot('failed', 'failed', 'Oxford Limiter'),
          slot('refused', 'empty', 'INSERT', { refused: true }), slot('not on this unit', 'na', 'N/A')),
        row('While placing', slot('takes it', 'empty', 'INSERT', { drop: 'ok', why: 'DROP' }), slot('replaces', 'running', 'Cambridge EQ', { drop: 'ok', why: 'REPLACE' }),
          slot('no room', 'empty', 'INSERT', { drop: 'no', why: 'NO ROOM' }), slot('not licensed', 'empty', 'INSERT', { drop: 'no', why: 'NOT LICENSED' }))),
      el('div', { class: 'panel sg-sec' }, el('div', { class: 'sect-title', text: 'States (never colour alone)' }),
        row('Words and shapes', word('running', 'st-running', 'Running'), word('waiting', 'st-waiting', 'Waiting'), word('refused', 'st-refused', 'Refused'),
          word('bypassed', 'st-bypassed', 'Bypassed'), word('clip', 'st-clip', 'Clip'), word('disconnected', 'st-gone', 'Disconnected')),
        row('Notes', cell('info', el('div', { class: 'ctl-note', dataset: { kind: 'info' } }, el('span', { text: 'Needs an x8p power cycle: saved inserts come back after it.' }))),
          cell('refused', el('div', { class: 'ctl-note', dataset: { kind: 'bad' } }, el('span', { text: 'Input 3 gain was not changed: not supported by the installed driver.' })))),
        row('Numbers', cell('field', el('input', { class: 'knob-edit', type: 'text', value: '-12.5', 'aria-label': 'Style guide number field' })),
          cell('readout', el('span', { class: 'num sg-num', text: '-12.5 dB  48 kHz  15,390 w' })))));
  }

  function show() { build(); }
  function onTheme() { if (S.view === 'styleguide') build(); }
  return { show, onTheme };
})();
