'use strict';
/*
 * [Endpoints] Settings, Windows audio (docs/apollo-control-units.md 1.7): which Windows playback and recording endpoints
 * each unit has, one checkbox per row, for the set the unit shows now (its own, or the combined set the x8p hosts).
 * The host's rows carry their names and flags; this file never needs the driver's tables. A change is one
 * endpoints-set (one administrator prompt) and applies at the unit's next start, so the boxes are a draft until Apply.
 *   UI -> host  {"cmd":"endpoints-get"[,"unit":id]}  {"cmd":"endpoints-set","unit":id,"rows":[..]|"default":true}
 *   host -> UI  {"type":"endpoints",..}  {"type":"endpoints-result","unit":id,"pending":bool,"ok":bool,"code":"","detail":""}
 * The demo answers with the tables of section 1.7 (x8p, Twin X, combined) and stores nothing.
 */
const Endpoints = (() => {
  const EP = { byId: {}, draft: {}, busy: {}, notes: {}, askedAt: -1e9, replied: false, waited: false, node: null, sig: '' };
  const send = o => { if (host) host.postMessage(JSON.stringify(o)); };
  const CODES = {
    cancelled: 'The administrator prompt was declined: nothing changed.',
    elevation: 'The administrator step did not start: nothing changed.',
    registry: 'The endpoint set could not be stored.',
    busy: 'Another change is waiting for its administrator prompt.',
    absent: 'The unit is not on apollo.sys right now.',
    none: 'This unit shows no endpoint set of its own.',
    unit: 'The host does not know this unit.',
    mask: 'The host refused these rows.',
  };

  /* ---------------------------------------------------------- host traffic */

  function ask(force) {
    const t = performance.now();
    if (!force && t - EP.askedAt < 3000) return;
    EP.askedAt = t;
    if (S.demo) { demoGet(); return; }
    if (!host) return;
    send({ cmd: 'endpoints-get' });
    setTimeout(() => { if (!EP.replied) { EP.waited = true; render(); } }, 4000);
  }

  function onHost(m) {
    if (!m || !m.unit) return;
    if (m.type === 'endpoints') {
      EP.replied = true;
      EP.byId[m.unit] = m;
      if (!dirty(m.unit)) delete EP.draft[m.unit];
    } else if (m.type === 'endpoints-result') {
      if (m.pending) {
        EP.busy[m.unit] = m.detail || 'Waiting for the administrator prompt';
      } else {
        delete EP.busy[m.unit];
        if (m.ok) {
          delete EP.draft[m.unit];
          EP.notes[m.unit] = { ok: true, text: m.detail || 'Stored. Windows shows the new set after the unit\'s next start.' };
          toast('Windows audio: stored. It applies at the unit\'s next start.');
        } else {
          EP.notes[m.unit] = { ok: false, text: CODES[m.code] || m.detail || 'The change was not made.' };
          if (m.code !== 'cancelled') toast(`Windows audio: ${EP.notes[m.unit].text}`, 'warn');
        }
        ask(true);
      }
    }
    render();
  }

  /* ---------------------------------------------------------- the draft (the boxes before Apply) */

  const nextRows = e => (e.rows || []).filter(r => r.next).map(r => r.row);
  const draftOf = id => EP.draft[id] || new Set(nextRows(EP.byId[id] || {}));
  function dirty(id) {
    const e = EP.byId[id], d = EP.draft[id];
    if (!e || !d) return false;
    const n = nextRows(e);
    return n.length !== d.size || n.some(r => !d.has(r));
  }
  function toggle(id, row, on) {
    const d = new Set(draftOf(id));
    if (on) d.add(row); else d.delete(row);
    EP.draft[id] = d;
    delete EP.notes[id];
    render();
  }

  async function apply(id) {
    const e = EP.byId[id], d = draftOf(id);
    if (!e || EP.busy[id]) return;
    const lost = (e.rows || []).filter(r => r.added && (r.open || r.running) && !d.has(r.row));
    if (lost.length) {
      const ok = await dialog({
        title: 'Remove an endpoint an app is using?',
        body: `${lost.map(r => r.name).join(', ')} ${lost.length === 1 ? 'is' : 'are'} open in an app now. ${lost.length === 1 ? 'It goes' : 'They go'} away at the unit's next start.`,
        confirm: 'Apply',
      });
      if (!ok) return;
    }
    EP.busy[id] = 'Asking for administrator rights';
    delete EP.notes[id];
    render();
    const msg = { cmd: 'endpoints-set', unit: id, rows: [...d].sort((a, b) => a - b) };
    if (S.demo) demoSet(msg); else send(msg);
  }

  function useDefaults(id) {
    const e = EP.byId[id];
    if (!e || EP.busy[id]) return;
    EP.busy[id] = 'Asking for administrator rights';
    delete EP.notes[id];
    delete EP.draft[id];
    render();
    const msg = { cmd: 'endpoints-set', unit: id, default: true };
    if (S.demo) demoSet(msg); else send(msg);
  }

  /* ---------------------------------------------------------- the panel */

  const unitName = id => {
    const u = (S.unitList || []).find(x => x.id === id);
    return u ? capsOf(u.deviceType).short || u.model : (EP.byId[id] && EP.byId[id].model) || 'unit';
  };
  const order = () => {
    const ids = Object.keys(EP.byId);
    const pos = id => { const u = (S.unitList || []).find(x => x.id === id); return u ? u.index : 99; };
    return ids.sort((a, b) => pos(a) - pos(b));
  };

  function rowNode(id, r, d, locked) {
    /* words only where a row differs from the plain case (a default row Windows has and keeps) */
    const on = d.has(r.row), tags = [];
    if (r.added !== on) tags.push(['change', on ? 'Added at next start' : 'Removed at next start']);
    else if (!on) tags.push(['off', 'Off']);
    if (r.running) tags.push(['use', r.render ? 'Playing' : 'Recording']);
    else if (r.open) tags.push(['use', 'Open in an app']);
    if (r.noSlot) tags.push(['silent', 'Silent at this rate']);
    if (r.model === null && r.render) tags.push(['routed', 'Follows the routing']);
    if (!r.default) tags.push(['optional', 'Not in the defaults']);
    const box = el('input', { type: 'checkbox', checked: on, disabled: locked, 'aria-label': `${r.name}: ${r.render ? 'playback' : 'recording'} endpoint`,
      dataset: { key: `${id}:${r.row}` }, onchange: ev => toggle(id, r.row, ev.target.checked) });
    return el('label', { class: `ep-row${on ? ' on' : ''}${r.added !== on ? ' changing' : ''}`, dataset: { row: String(r.row) } },
      box, el('span', { class: 'ep-name', text: r.name }),
      el('span', { class: 'ep-tags' }, tags.map(([k, t]) => el('span', { class: `ep-tag ep-${k}`, text: t }))));
  }

  function unitBlock(id) {
    const e = EP.byId[id], name = unitName(id);
    const head = el('div', { class: 'ep-head' }, el('span', { class: 'ep-unit', text: name }),
      el('span', { class: 'ep-set', text: !e.present ? 'not connected' : e.set === 'combined' ? 'combined set: both units, through the x8p' : e.set === 'none' ? 'no set of its own' : 'its own set' }));
    if (!e.present) return el('div', { class: 'ep-unitblock', dataset: { unit: id } }, head, el('span', { class: 'desc', text: e.detail || 'Not on apollo.sys right now.' }));
    if (e.set === 'none') {
      return el('div', { class: 'ep-unitblock', dataset: { unit: id } }, head,
        el('span', { class: 'desc', text: `The ${name}'s endpoints are part of the combined set, shown by the x8p: change them there.` }));
    }
    const d = draftOf(id), busy = EP.busy[id], old = e.version && e.version < 2, locked = !!busy || old;
    const rows = e.rows || [];
    const group = (title, list) => (list.length ? el('fieldset', { class: 'ep-group' }, el('legend', { text: title }),
      list.map(r => rowNode(id, r, d, locked))) : null);
    const changed = dirty(id);
    const isDefault = e.storedIn === 'default' || e.stored === null;
    const note = EP.notes[id];
    const summary = `${d.size} of ${rows.length} endpoints at the next start${changed ? ' (not applied yet)' : ''}.`;
    const status = busy ? busy
      : note ? note.text
      : e.restartNeeded ? 'Stored: Windows shows this set after the unit\'s next start. Until then it keeps the set it has now.'
      : '';
    return el('div', { class: 'ep-unitblock', dataset: { unit: id } }, head,
      old ? el('span', { class: 'desc ep-warn', text: 'This driver (endpoint table version 1) does not read the set Apollo Control stores: update apollo.sys to change it here.' }) : null,
      el('div', { class: 'ep-groups' },
        group('Playback (outputs)', rows.filter(r => r.render)),
        group('Recording (inputs)', rows.filter(r => !r.render))),
      el('div', { class: 'ep-foot' },
        el('span', { class: 'ep-sum num', text: summary }),
        el('div', { class: 'ep-acts' },
          el('button', { class: 'btn btn-primary', type: 'button', disabled: locked || !changed, onclick: () => apply(id) }, 'Apply'),
          el('button', { class: 'btn', type: 'button', disabled: locked || !changed, onclick: () => { delete EP.draft[id]; render(); } }, 'Undo changes'),
          el('button', { class: 'btn', type: 'button', disabled: locked || (isDefault && !changed),
            title: 'The driver\'s default set (bold in the list: every jack, the monitor mix and the mic/line inputs)', onclick: () => useDefaults(id) }, 'Use defaults'))),
      status ? el('div', { class: `ep-status${note && !note.ok ? ' bad' : ''}${busy ? ' busy' : ''}`, role: 'status', text: status }) : null);
  }

  function panel() {
    const ids = order();
    const body = ids.length ? ids.map(unitBlock)
      : [el('span', { class: 'desc', text: S.demo || host ? (EP.waited ? 'The driver does not report Windows endpoints in this build.' : 'Asking the driver...') : 'The desktop app shows the endpoints of the connected units here.' })];
    EP.node = el('div', { class: 'panel panel-wide ep-panel', id: 'epPanel' },
      el('div', { class: 'sect-title', text: 'Windows audio' }),
      el('span', { class: 'desc', text: 'The playback and recording devices Windows shows for each unit. A change needs administrator rights (one Windows prompt) and applies at the unit\'s next start: power-cycle the Apollo, or restart it from Device Manager after closing apps that play through it.' }),
      body);
    return EP.node;
  }

  /* re-draw the panel in place, keeping the focused checkbox */
  function render() {
    if (!EP.node || !EP.node.isConnected) return;
    const f = document.activeElement, key = f && f.dataset ? f.dataset.key : null;
    const old = EP.node;
    old.replaceWith(panel());
    if (key) { const n = EP.node.querySelector(`[data-key="${key}"]`); if (n) n.focus(); }
  }

  /* ---------------------------------------------------------- the demo (section 1.7's tables) */

  const X8P_OUT = ['MON L/R', 'LINE 1-2', 'LINE 3-4', 'LINE 5-6', 'LINE 7-8', 'HP 1', 'HP 2', 'S/PDIF'];
  const TWX_OUT = ['MON L/R', 'LINE 3-4', 'HP'];
  const ins = n => Array.from({ length: n }, (_, i) => `MIC/LINE ${i + 1}`);
  function demoRows(kind) {
    const rows = [];
    const add = (name, render, model, def) => rows.push({ row: rows.length, name, circuit: '', render, channels: render ? 2 : 1, model, enabled: def, added: def,
      default: def, next: def, open: false, running: false, noSlot: false });
    if (kind === 'combined') {
      add('Monitor', true, null, true);
      X8P_OUT.forEach(n => add(`x8p ${n}`, true, 'x8p', true));
      TWX_OUT.forEach(n => add(`Twin X ${n}`, true, 'twinx', true));
      add('Monitor mix', false, null, true);
      ins(8).forEach(n => add(`x8p ${n}`, false, 'x8p', true));
      ins(2).forEach(n => add(`Twin X ${n}`, false, 'twinx', true));
      ['VIRTUAL 1-2', 'VIRTUAL 3-4', 'VIRTUAL 5-6', 'VIRTUAL 7-8', 'S/PDIF'].forEach(n => add(`x8p ${n}`, false, 'x8p', false));
      ['VIRTUAL 1-2', 'VIRTUAL 3-4', 'S/PDIF'].forEach(n => add(`Twin X ${n}`, false, 'twinx', false));
    } else if (kind === 'x8p') {
      X8P_OUT.forEach(n => add(n, true, 'x8p', true));
      add('Monitor mix', false, 'x8p', true);
      ins(8).forEach(n => add(n, false, 'x8p', true));
      ['VIRTUAL 1-2', 'VIRTUAL 3-4', 'VIRTUAL 5-6', 'VIRTUAL 7-8', 'S/PDIF'].forEach(n => add(n, false, 'x8p', false));
    } else {
      TWX_OUT.forEach(n => add(n, true, 'twinx', true));
      add('Monitor mix', false, 'twinx', true);
      ins(2).forEach(n => add(n, false, 'twinx', true));
      ['VIRTUAL 1-2', 'VIRTUAL 3-4', 'S/PDIF'].forEach(n => add(n, false, 'twinx', false));
    }
    if (rows[1]) { rows[1].open = true; rows[1].running = true; }      /* something plays through the first jack */
    return rows;
  }
  const maskOf = rows => rows.reduce((m, r) => (r.enabled ? m + 2 ** r.row : m), 0);

  function demoGet() {
    const combined = typeof rateAgg === 'function' && !!rateAgg();
    for (const idx of segs.keys()) {
      const id = unitIdOf(idx) || `demo${idx}`;
      if (EP.byId[id]) continue;
      const tw = String(capsOf(unitType(idx)).short || '').toLowerCase().startsWith('twin');
      const set = combined ? (tw ? 'none' : 'combined') : 'standalone';
      const rows = set === 'none' ? [] : demoRows(set === 'combined' ? 'combined' : tw ? 'twinx' : 'x8p');
      const mask = maskOf(rows);
      EP.byId[id] = { type: 'endpoints', unit: id, present: true, model: tw ? 'twinx' : 'x8p', set, combinedOn: combined, count: rows.length, mask, defaultMask: mask,
        stored: null, storedIn: 'default', next: mask, restartNeeded: false, version: 2, rows };
    }
    EP.replied = true;
    render();
  }

  function demoSet(msg) {
    const e = EP.byId[msg.unit];
    setTimeout(() => {
      if (!e) return;
      for (const r of e.rows) r.next = msg.default ? r.default : msg.rows.includes(r.row);
      e.next = e.rows.reduce((m, r) => (r.next ? m + 2 ** r.row : m), 0);
      e.stored = msg.default ? null : e.next;
      e.storedIn = msg.default ? 'default' : e.set === 'combined' ? 'Aggregate' : 'unit';
      e.restartNeeded = e.next !== e.mask;
      onHost({ type: 'endpoints-result', unit: msg.unit, pending: false, ok: true, code: '',
        detail: 'Demo: stored (nothing reaches Windows). The real app asks for administrator rights; the set applies at the unit\'s next start.', restartNeeded: e.restartNeeded });
    }, 500);
  }

  function demoExit() { EP.byId = {}; EP.draft = {}; EP.busy = {}; EP.notes = {}; EP.replied = false; EP.waited = false; EP.askedAt = -1e9; }

  return { ask, onHost, panel, render, demoExit, get state() { return EP; } };
})();
