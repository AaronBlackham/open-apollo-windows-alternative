'use strict';
/*
 * [Virtual] Settings, Virtual inputs (docs/apollo-control-units.md 1.13, docs/STRIPS.md 13): which VIRTUAL inputs get an
 * Input Strip, per unit. VIRTUAL n carries what a DAW plays to the unit's VIRTUAL n playback channel over ASIO (Windows has
 * no playback device for them); with its strip it records, joins the monitor mix and takes cue and aux sends (app.js shows
 * its strip after the Mic strips), without one it is silent and the mixer refuses its bus. The driver builds the strips its
 * registry value StripVirtualMask asks for (bit n-1 = VIRTUAL n; x8p 1-8, Twin X 1-4) as the unit starts, so Apply is one
 * params-set: the host stores the value elevated (one Windows prompt) and restarts the unit.
 *   UI -> host  {"cmd":"params-set","unit":id,"name":"StripVirtualMask","value":n}   {"cmd":"params-get"}
 *   host -> UI  {"type":"params-result","unit":id,"pending":bool,"ok","stored","restarted","code","detail"}   {"type":"params"}
 * What runs comes from each unit's live strips (virtualMask built, virtualAsked at this start, virtualWhy a layout refusal);
 * what the next start reads, from the "params" message's StripVirtualMask entry. The boxes are a draft until Apply.
 * The demo models the strips report (demoFields: VIRTUAL, aux returns) and answers params-set as a restart would.
 */
const Virt = (() => {
  const NAME = 'StripVirtualMask';
  const V = { draft: {}, busy: {}, notes: {}, node: null, askedAt: -1e9, demo: [] };
  const CODES = {
    cancelled: 'The administrator prompt was declined: nothing changed.',
    elevation: 'The administrator step did not start: nothing changed.',
    busy: 'Another change is waiting for its administrator prompt.',
    absent: 'The unit is not on apollo.sys right now.',
    unit: 'The host cannot store values for this unit.',
    name: 'The host does not change this value.',
    value: 'The host refused these inputs for this model.',
  };

  /* ---------------------------------------------------------- what the unit runs, and what its next start reads */

  const stripsOf = idx => { const u = S.units[idx]; return (u && u.live && u.live.strips) || null; };
  const countOf = idx => capsOf(unitType(idx)).virtuals || 0;
  const all = idx => (1 << countOf(idx)) - 1;
  /* the driver reports VIRTUAL strips (GET_STRIPS version 4): the host sends virtualMask */
  const reports = idx => { const st = stripsOf(idx); return !!st && st.virtualMask !== undefined; };
  function paramEntry(idx) {
    const m = paramsByUnit.get(unitIdOf(idx));
    return m && m.present && !m.error && Array.isArray(m.entries) ? m.entries.find(e => e && e.name === NAME) || null : null;
  }
  /* the mask the unit's next start reads: the params entry (stored, else what this start resolved), else what it asked */
  function nextOf(idx) {
    const e = paramEntry(idx);
    if (e) return ((e.pending ? (e.stored ?? e.default) : e.value) >>> 0) & all(idx);
    const st = stripsOf(idx);
    return st && st.virtualAsked !== undefined ? (st.virtualAsked >>> 0) & all(idx) : virtMaskOf(idx);
  }
  const draftOf = (id, idx) => (id in V.draft ? V.draft[id] : nextOf(idx));
  const names = m => { const out = []; for (let n = 1; n <= 8; n++) if ((m >>> (n - 1)) & 1) out.push(n); return out; };
  const list = m => (names(m).length ? `VIRTUAL ${joinList(names(m).map(String))}` : 'none');

  /* ---------------------------------------------------------- host traffic */

  const send = o => { if (host) host.postMessage(JSON.stringify(o)); };

  /* every unit's parameters, at most every 3 s while Settings is on screen (not in the demo: it models them) */
  function ask(force) {
    const t = performance.now();
    if (S.demo || !host || S.view !== 'settings' || (!force && t - V.askedAt < 3000)) return;
    V.askedAt = t;
    send({ cmd: 'params-get' });
  }

  function onHost(m) {
    if (!m || typeof m.unit !== 'string' || m.name !== NAME) return;
    const id = m.unit, idx = slotOfId(id), short = idx >= 0 ? unitShort(idx) : 'unit';
    if (m.pending) {
      V.busy[id] = m.detail || 'Waiting for the administrator prompt';
    } else {
      delete V.busy[id];
      if (m.stored) delete V.draft[id];
      if (m.ok) {
        V.notes[id] = { kind: 'ok', text: `Stored. The ${short} restarts now: its audio stops for a few seconds, then its VIRTUAL strips follow.` };
        toast(`Virtual inputs: stored; the ${short} restarts.`);
      } else if (m.stored) {
        V.notes[id] = { kind: 'warn', text: `Stored, not running yet: ${m.detail || 'the unit did not restart'}.` };
        toast(`Virtual inputs: stored; the ${short} did not restart.`, 'warn', 7000);
      } else {
        V.notes[id] = { kind: 'bad', text: CODES[m.code] || m.detail || 'The change was not made.' };
        if (m.code !== 'cancelled') toast(`Virtual inputs: ${V.notes[id].text}`, 'warn');
      }
      ask(true);
    }
    render();
  }

  /* ---------------------------------------------------------- the draft and Apply */

  function toggle(id, idx, n, on) {
    const d = draftOf(id, idx), b = 1 << (n - 1);
    V.draft[id] = on ? d | b : d & ~b;
    if (V.draft[id] === nextOf(idx)) delete V.draft[id];
    delete V.notes[id];
    render();
  }

  async function apply(id, idx, value) {
    if (V.busy[id] || !writable(idx)) return;
    const short = unitShort(idx), add = names(value & ~virtMaskOf(idx));
    const ok = await dialog({
      title: `Restart the ${short} for its VIRTUAL inputs?`,
      body: `Windows asks for administrator rights, then the ${short} restarts to build ${value ? list(value) : 'no VIRTUAL inputs'}: ` +
        `audio on the ${short} stops for a few seconds and comes back on its own. ` +
        (add.length || value ? 'Nothing plays through a VIRTUAL input until a DAW sends audio to it over ASIO: Windows has no playback device for them.' : ''),
      confirm: `Restart the ${short}`,
    });
    if (!ok || V.busy[id]) return;
    V.busy[id] = 'Asking for administrator rights';
    delete V.notes[id];
    render();
    const msg = { cmd: 'params-set', unit: id, name: NAME, value };
    if (S.demo) demoSet(idx, msg); else send(msg);
  }

  /* ---------------------------------------------------------- the panel */

  function rowNode(id, idx, n, d, built, asked, locked) {
    const b = 1 << (n - 1), on = !!(d & b), run = !!(built & b), tags = [], when = id in V.draft ? 'the restart' : 'the next start';
    if (run !== on) tags.push(['change', on ? `Built at ${when}` : `Removed at ${when}`]);
    else if (run) tags.push(['use', 'Running']);
    if (!run && (asked & b) && reports(idx)) tags.push(['silent', 'Asked, not built']);
    const box = el('input', { type: 'checkbox', checked: on, disabled: locked, 'aria-label': `${unitShort(idx)} VIRTUAL ${n}`,
      dataset: { key: `${id}:v${n}` }, onchange: ev => toggle(id, idx, n, ev.target.checked) });
    return el('label', { class: `ep-row vi-row${on ? ' on' : ''}${run !== on ? ' changing' : ''}`, dataset: { virt: String(n) } },
      box, el('span', { class: 'ep-name', text: `VIRTUAL ${n}` }),
      el('span', { class: 'ep-tags' }, tags.map(([k, t]) => el('span', { class: `ep-tag ep-${k}`, text: t }))));
  }

  function unitBlock(u) {
    const idx = u.index, id = u.id, short = unitShort(idx), count = countOf(idx);
    const st = stripsOf(idx), built = virtMaskOf(idx), asked = st && st.virtualAsked !== undefined ? st.virtualAsked >>> 0 : built;
    const ro = unitReadOnly(idx), busy = V.busy[id], note = V.notes[id];
    const head = el('div', { class: 'ep-head' }, el('span', { class: 'ep-unit', text: short }),
      el('span', { class: 'ep-set', text: built ? `${list(built)} running` : 'no VIRTUAL strips running' }));
    const block = (...kids) => el('div', { class: 'ep-unitblock vi-unitblock', dataset: { unit: id } }, head, ...kids);
    if (ro) return block(el('span', { class: 'desc', text: `The ${short} is in ${ro} mode: read-only here.` }));
    if (!st && !S.demo) return block(el('span', { class: 'desc', text: `Waiting for the ${short}'s strips report.` }));
    const old = !reports(idx), off = !!st && (!st.enable || st.state === 0);
    const d = draftOf(id, idx), next = nextOf(idx), changed = id in V.draft, locked = !!busy || old || off;
    const why = st && st.virtualWhy ? `The driver found no room for ${list(asked & ~built)} on the strip DSP (pool ${st.virtualWhy - 8}): ` +
      'it built the Mic strips without them.' : '';
    /* stored for the next start but not running: a failed or put-off restart, or a value set elsewhere */
    const waiting = !changed && !busy && next !== built && reports(idx) && !why;
    const status = busy ? { cls: ' busy', text: busy }
      : note ? { cls: note.kind === 'bad' ? ' bad' : '', text: note.text }
      : waiting ? { cls: '', text: `Stored for the ${short}'s next start: ${list(next)}. Restart it to build them now.` }
      : null;
    const acts = el('div', { class: 'ep-acts' },
      el('button', { class: 'btn btn-primary', type: 'button', disabled: locked || !changed, onclick: () => apply(id, idx, d) }, 'Apply'),
      el('button', { class: 'btn', type: 'button', disabled: locked || !changed, onclick: () => { delete V.draft[id]; render(); } }, 'Undo changes'),
      waiting && !locked && !(note && note.kind === 'ok') ? el('button', { class: 'btn', type: 'button', title: `Store the same value again and restart the ${short}`,
        onclick: () => apply(id, idx, next) }, `Restart the ${short}`) : null);
    return block(
      old ? el('span', { class: 'desc ep-warn', text: 'The installed apollo.sys builds no VIRTUAL strips (an older build): install a newer driver to use them.' }) : null,
      off ? el('span', { class: 'desc ep-warn', text: `The ${short}'s input strips are off (StripsEnable): VIRTUAL inputs need them.` }) : null,
      why ? el('span', { class: 'desc ep-warn', text: why }) : null,
      el('fieldset', { class: 'ep-group vi-group' }, el('legend', { text: 'Input Strips' }),
        Array.from({ length: count }, (_, i) => rowNode(id, idx, i + 1, d, built, asked, locked))),
      el('div', { class: 'ep-foot' },
        el('span', { class: 'ep-sum num', text: `${names(d).length} of ${count} at the ${changed ? 'restart (not applied yet)' : 'next start'}.` }), acts),
      status ? el('div', { class: `ep-status${status.cls}`, role: 'status', text: status.text }) : null);
  }

  function panel() {
    const units = controlUnits().filter(u => countOf(u.index) > 0 && (u.id || S.demo));
    V.node = el('div', { class: 'panel panel-wide ep-panel vi-panel', id: 'viPanel' },
      el('div', { class: 'sect-title', text: 'Virtual inputs' }),
      el('span', { class: 'desc', text: 'A VIRTUAL input carries what a DAW plays to it over ASIO into the unit\'s monitor mix, cues and aux buses, ' +
        'and records it back. Windows has no playback device for them. Each one needs an Input Strip on the unit\'s DSP, built as the unit ' +
        'starts: Apply asks for administrator rights (one Windows prompt) and restarts the unit, so its audio stops for a few seconds.' }),
      units.length ? units.map(unitBlock) : el('span', { class: 'desc', text: host || S.demo ? 'No unit with VIRTUAL inputs is connected.' : 'The desktop app shows the connected units here.' }));
    ask();
    return V.node;
  }

  /* re-draw the panel in place, keeping the focused checkbox */
  function render() {
    if (!V.node || !V.node.isConnected) return;
    const f = document.activeElement, key = f && f.dataset ? f.dataset.key : null;
    V.node.replaceWith(panel());
    if (key) { const n = V.node.querySelector(`[data-key="${key}"]`); if (n) n.focus(); }
  }

  /* ---------------------------------------------------------- the demo */

  /* what the demo's units report, as a driver with VIRTUAL and aux-return strips would (both models' defaults: no VIRTUAL
     strip, AUX 1 and AUX 2 return strips running) */
  function demoOf(idx) {
    return V.demo[idx] || (V.demo[idx] = { virtualMask: 0, virtualAsked: 0, virtualWhy: 0, auxMask: 0x5, auxAsked: 0x5, auxWhy: 0, auxLive: 0x3 });
  }
  /* the strips fields of a demo "live" (app.js demoLive, units.js demoTwinLive); null keys are left out (an older driver) */
  function demoFields(idx) {
    const out = {};
    for (const [k, v] of Object.entries(demoOf(idx))) if (v !== null) out[k] = v;
    return out;
  }
  /* tests and the demo: change what a demo unit reports, then publish it as its next "live" */
  function demoState(idx, patch) {
    Object.assign(demoOf(idx), patch || {});
    demoPublish(idx);
    return { ...demoOf(idx) };
  }
  function demoPublish(idx) {
    if (!S.demo) return;
    if (idx === 0) onLive(demoLive());
    else if (Units.demoTwinLive && S.units[idx]) onLive(Units.demoTwinLive());
  }
  /* params-set in the demo: the prompt, then the stored value and a restart that builds it */
  function demoSet(idx, msg) {
    const base = { type: 'params-result', unit: msg.unit, name: NAME, value: msg.value };
    setTimeout(() => onHost({ ...base, pending: true, code: '', detail: 'Demo: no administrator prompt, nothing reaches a driver' }), 60);
    setTimeout(() => {
      if (!S.demo) return;
      const v = (msg.value >>> 0) & all(idx);
      Object.assign(demoOf(idx), { virtualMask: v, virtualAsked: v });
      onHost({ ...base, pending: false, ok: true, stored: true, restarted: true, code: '', detail: 'Demo: stored; the unit restarted' });
      demoPublish(idx);
    }, 500);
  }
  function demoExit() { V.demo = []; V.draft = {}; V.busy = {}; V.notes = {}; }

  return { NAME, panel, render, ask, onHost, demoFields, demoState, demoExit, get state() { return V; } };
})();
