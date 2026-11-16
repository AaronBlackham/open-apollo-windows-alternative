'use strict';
/*
 * [Copy] Copy and paste one input's settings onto another (UA Console's CMxCopyChannel / CMxPasteChannel; open-apollo
 * parity "channel presets and copy/paste"). Right-click a strip's number or name: Copy channel, Paste channel.
 * What travels: the preamp (gain, mic / line, pad, low cut, polarity; 48V is never switched on by a paste, only off),
 * the monitor-mix level, pan and mute (not solo), the input's send and pan to every cue, and its AUX 1 / AUX 2 send, pan
 * (PRE / POST is per aux bus here, so it stays). Across units (x8p to Twin X) too; what the target model lacks is left out. Not the name, the link, the
 * Hi-Z load or plug-in inserts. Every write goes through the app's own paths (setInput, setMix, setSend, Aux.setSend),
 * so a paste is one undo step (undo.js).
 * Loaded before app.js; uses its globals only inside functions.
 */
const ChCopy = (() => {
  let clip = null;                          /* { from, pre, mix, cues: [{ level, pan }], aux: [{ level, pan }] } */
  let menuNode = null;

  function copy(idx, ch) {
    const c = ctl(idx), k = capsOf(unitType(idx)), i = c.inputs[ch];
    if (!i || ch >= k.preamps) return;
    const m = mixOfBus(idx, ch);
    clip = {
      from: inputLabel(ch, idx),
      pre: { gain: i.gain, line: !!i.line, pad: !!i.pad, lowcut: !!i.lowcut, phase: !!i.phase, p48: !!i.p48 },
      mix: { level: m.level, pan: m.pan, mute: !!m.mute },
      cues: k.cues.map((_, q) => ({ ...sendOf(idx, q, ch) })),
      aux: [0, 1].map(a => ({ ...Aux.sendOf(idx, a, ch) })),
    };
    toast(`Copied ${clip.from}.`);
  }

  function paste(idx, ch) {
    if (!clip) { toast('Nothing copied yet: right-click an input and choose Copy channel.', 'warn'); return; }
    const c = ctl(idx), k = capsOf(unitType(idx));
    if (ch >= k.preamps || !c.inputs[ch]) return;
    if (!writable(idx)) { toast(`The ${unitShort(idx)} is read-only or not connected: nothing was pasted.`, 'warn'); return; }
    const p = clip.pre, notes = [];
    for (const key of ['line', 'pad', 'lowcut', 'phase']) setInput(ch, key, p[INPUT_FIELDS[key]], { unit: idx, noLink: true });
    if (!cfg.safety.lockGain) setInput(ch, 'gain', p.gain, { unit: idx, noLink: true });
    else if (p.gain !== c.inputs[ch].gain) notes.push('gain kept: gain knobs are locked');
    if (!p.p48 && c.inputs[ch].p48) setInput(ch, '48v', false, { unit: idx, noLink: true });
    else if (p.p48 && !c.inputs[ch].p48) notes.push('48V left off (turn it on yourself)');
    setMix(ch, 'level', clip.mix.level, idx);
    setMix(ch, 'pan', clip.mix.pan, idx);
    setMix(ch, 'mute', clip.mix.mute, idx);
    clip.cues.forEach((s, q) => {
      if (q >= k.cues.length) return;
      setSend(q, ch, 'level', s.level, idx);
      setSend(q, ch, 'pan', s.pan, idx);
    });
    clip.aux.forEach((s, a) => {
      Aux.setSend(idx, a, ch, 'level', s.level);
      Aux.setSend(idx, a, ch, 'pan', s.pan);
    });
    renderUnitInputs(idx);
    renderMixFor(idx);
    toast(`Pasted ${clip.from} onto ${inputLabel(ch, idx)}${notes.length ? `: ${notes.join('; ')}` : ''}.`);
  }

  function close() {
    if (menuNode) { menuNode.remove(); menuNode = null; }
  }

  /* the strip's right-click menu */
  function menu(e, idx, ch) {
    e.preventDefault();
    close();
    const item = (text, fn, dis = false) => el('button', { role: 'menuitem', type: 'button', disabled: dis || null,
      onclick: () => { close(); fn(); } }, text);
    menuNode = el('div', { class: 'menu ch-menu', role: 'menu', style: `position:fixed;left:${e.clientX}px;top:${e.clientY}px;z-index:50` },
      item(`Copy ${inputLabel(ch, idx)}`, () => copy(idx, ch)),
      item(clip ? `Paste ${clip.from}` : 'Paste channel', () => paste(idx, ch), !clip));
    document.body.append(menuNode);
    const first = menuNode.querySelector('button:not([disabled])');
    if (first) first.focus();
    setTimeout(() => document.addEventListener('click', close, { once: true }), 0);
  }

  document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
  return { copy, paste, menu, close, get clip() { return clip; } };
})();
