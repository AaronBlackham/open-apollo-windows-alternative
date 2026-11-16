'use strict';
/*
 * [Undo] Undo / redo of what Apollo Control changed on a unit (UA Console's kPropUndoRedo; open-apollo parity "Undo/redo"):
 * the preamps (gain, pad, mic / line, low cut, polarity, link, Hi-Z load), the monitor section, the monitor mix, the cues,
 * the aux buses, the ALT and output settings. Not the plug-in chains (their engine keeps them), not names, never 48V on
 * (an undo that would switch phantom power on leaves it off and says so).
 *
 * How: each unit has a baseline, the state at the last quiet moment (sessions.js undoPart, the same part a session file
 * holds). A change this app sends (app.js hwSend calls touch) opens a step; 0.7 s after the last send of a run the state
 * is compared with the baseline and, if it differs, the baseline goes on the unit's undo stack. While nothing is being
 * changed here the baseline follows the unit (once a second), so the front panel's knob, readback and a connect's replay
 * never become steps. Undo applies the step through the session paths (sessions.js undoApply), the current state goes on
 * the redo stack. Per unit, the unit the Control view focuses; 50 steps each. Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z.
 *
 * Loaded after sessions.js and before app.js; it uses their globals only inside functions.
 */
const Undo = (() => {
  const MAX = 50, QUIET_MS = 700, FOLLOW_MS = 1000;
  const U = new Map();                      /* unit slot -> { base, fp, undo: [], redo: [], timer } */
  let suspended = 0, btns = null;

  const unit = idx => { if (!U.has(idx)) U.set(idx, { base: null, fp: '', undo: [], redo: [], timer: 0 }); return U.get(idx); };
  const snap = idx => { try { return Sess.undoPart(idx); } catch (e) { return null; } };
  const fpOf = p => JSON.stringify(p);
  const applying = () => suspended > 0 || !!(Sess.state && Sess.state.applying);

  /* a change this app sent to unit idx (hwSend) */
  function touch(idx) {
    if (applying()) return;
    const u = unit(idx);
    if (!u.base) return;                    /* no baseline yet (just connected): nothing to go back to */
    clearTimeout(u.timer);
    u.timer = setTimeout(() => commit(idx), QUIET_MS);
  }

  function commit(idx) {
    const u = unit(idx);
    u.timer = 0;
    const p = snap(idx);
    if (!p) return;
    const f = fpOf(p);
    if (u.base && f !== u.fp) {
      u.undo.push(u.base);
      if (u.undo.length > MAX) u.undo.shift();
      u.redo = [];
    }
    u.base = p;
    u.fp = f;
    render();
  }

  /* the baseline follows the unit while nothing is changed here */
  function follow() {
    if (applying() || typeof controlUnits !== 'function') return;
    for (const c of controlUnits()) {
      const idx = c.index, u = unit(idx);
      if (u.timer || !(S.demo || connected(idx))) continue;
      const p = snap(idx);
      if (!p) continue;
      u.base = p;
      u.fp = fpOf(p);
    }
  }

  function step(dir, idx = S.sel) {
    const u = unit(idx);
    if (u.timer) { clearTimeout(u.timer); commit(idx); }
    const from = dir < 0 ? u.undo : u.redo, to = dir < 0 ? u.redo : u.undo;
    if (!from.length) { toast(dir < 0 ? 'Nothing to undo.' : 'Nothing to redo.'); return; }
    if (!S.demo && !connected(idx)) { toast(`The ${unitShort(idx)} is not connected: nothing was changed.`, 'warn'); return; }
    const target = from.pop(), cur = snap(idx);
    if (cur) to.push(cur);
    suspended++;                            /* the apply's own sends are not a step (it sends synchronously) */
    let notes = [];
    try {
      notes = Sess.undoApply(idx, target) || [];
    } finally {
      suspended--;
    }
    u.base = target;
    u.fp = fpOf(target);
    toast(`${dir < 0 ? 'Undone' : 'Redone'} on the ${unitShort(idx)}${notes.length ? `: ${notes.join('; ')}` : ''}.`);
    render();
  }

  function render() {
    if (!btns) return;
    const u = unit(S.sel);
    btns.undo.disabled = !u.undo.length;
    btns.redo.disabled = !u.redo.length;
    btns.undo.title = `Undo (Ctrl+Z)${u.undo.length ? `: ${u.undo.length} step(s) on the ${unitShort(S.sel)}` : ''}`;
    btns.redo.title = `Redo (Ctrl+Y)${u.redo.length ? `: ${u.redo.length} step(s)` : ''}`;
  }

  function build() {
    const wrap = document.getElementById('undoWrap');
    if (!wrap) return;
    btns = {
      undo: el('button', { class: 'btn btn-quiet undo-btn', type: 'button', 'aria-label': 'Undo', onclick: () => step(-1) }, '↶'),
      redo: el('button', { class: 'btn btn-quiet undo-btn', type: 'button', 'aria-label': 'Redo', onclick: () => step(1) }, '↷'),
    };
    wrap.replaceChildren(btns.undo, btns.redo);
    setInterval(() => { follow(); render(); }, FOLLOW_MS);
    render();
  }

  /* app.js keydown: true when it was ours */
  function key(e) {
    if (!e.ctrlKey || e.altKey || e.metaKey) return false;
    const k = e.key.toLowerCase();
    if (k === 'z' && !e.shiftKey) { step(-1); return true; }
    if (k === 'y' || (k === 'z' && e.shiftKey)) { step(1); return true; }
    return false;
  }

  return { touch, step, build, render, key, forget: idx => U.delete(idx) };
})();
