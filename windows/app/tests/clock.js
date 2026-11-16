// [Clock] The clock source, digital I/O formats and the monitor link (app/ui/clockui.js, docs/apollo-control-units.md 1.10)
// through a stub host: the Device view's panel from "clock" for the x8p and the Twin X, a proven source (Internal) sent
// without try, an experimental one only after the confirmation and with try:true, a pending result disabling the selector
// and the final one enabling it again, the fall-back notice with Try again, digital-set for the optical input, the prefs
// saved after an ok result (by unit id, the x8p's too) and sent again after the unit arrives in a units list, panel-link
// after a units list with both units, the segment badge and the talkback note; then the demo (nothing sent, no errors).
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const SIX = [44100, 48000, 88200, 96000, 176400, 192000];
const X8P = { id: '000000003A00C0DE', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true,
  active: true, observe: false, stage: 5, clock: { source: 'internal', wanted: 'internal', locked: true, fellBack: false }, panel: { knob: 'monitor', role: null } };
const TWX = { id: '000000002F00BEEF', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2,
  present: true, active: true, observe: false, stage: 5 };
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0, extCaps: 0, fwRelease: 0x0B0FD613, subsystemId: 0x14, deviceType: 0x22, publicType: 0x16,
  numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10, interruptCount: 0, notifStatusSeen: 0,
  numPreamps: 8, serial: X8P.serial, stage: 5, flags: 0x40 | 0x80 | 0x100 | 0x200 | 0x400, mode: 2, observe: false };
const TW_INFO = { ...INFO, deviceType: 0x99, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };

const base = (id, over) => ({ type: 'clock', unit: id, present: true, source: 'internal', wanted: 'internal', locked: true, changing: false, fellBack: false,
  hold: false, reapply: false, proven: ['internal'], rate: 48000, ratesOnSource: SIX, registers: { clockCfg: 524, rateInfo: 513, clockInfo: 48000 },
  readbackLock: true, combined: { member: true, timeline: false }, last: { code: '', why: '', ms: 0, changes: 0, fallbacks: 0, lockLosses: 0 },
  measuredHz: 47999.722, ...over });
const x8Clock = (over = {}, dig = {}) => base(X8P.id, { sources: ['internal', 'spdif', 'adat', 'wordclock'], offered: ['internal', 'spdif', 'adat', 'wordclock', 'peer'],
  usable: ['internal', 'spdif', 'wordclock'], combined: { member: true, timeline: true },
  digital: { has: ['optical-in', 'optical-out', 'coax', 'src'], proven: [], known: false, opticalIn: 'spdif', opticalOut: 'spdif', coax: true, src: false,
    channels: { adatIn: 8, adatOut: 8, spdifIn: 2, spdifOut: 2 }, ...dig }, ...over });
const twClock = (over = {}, dig = {}) => base(TWX.id, { sources: ['internal', 'spdif', 'adat'], offered: ['internal', 'spdif', 'adat', 'peer'], usable: ['internal', 'spdif'],
  digital: { has: ['optical-in', 'src'], proven: [], known: false, opticalIn: 'spdif', opticalOut: null, coax: false, src: false,
    channels: { adatIn: 8, adatOut: 0, spdifIn: 2, spdifOut: 0 }, ...dig }, ...over });
const panel = (id, strip, link) => ({ type: 'panel', unit: id, present: true, valid: true, seq: 3, knob: 'monitor', preamp: 1, level: 148, levelDb: -22, hp: 112,
  hpDb: -40, mute: false, dim: false, mono: false, alt: 0, talkback: false, lock: true, role: null, talkDimmed: false, unitDims: false, pending: false,
  talkbackChannel: { recChannels: 34, slots: [32, 33], strip },
  link: link || { on: false, monitor: null, level: false, mute: false, dim: false, mono: false, talkDim: false, applied: 0, busy: 0, dropped: 0 }, events: [] });
const wait = ms => new Promise(r => setTimeout(r, ms));

async function stubPage(browser, errors) {
  const ctx = await browser.newContext({ viewport: { width: 1536, height: 864 } });
  await ctx.addInitScript(() => { window.__sent = []; window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } }; });
  const page = await ctx.newPage();
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(ui);
  return page;
}

(async () => {
  const browser = await chromium.launch();
  const errors = [];

  /* ---------------------------------------------------------------- A: no saved prefs */
  const page = await stubPage(browser, errors);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const cmds = () => page.evaluate(() => window.__sent.filter(s => s[0] === '{').map(s => JSON.parse(s)).filter(x => /^(clock|digital|panel)-/.test(x.cmd)));
  const mark = async () => (await cmds()).length;
  const since = async n => (await cmds()).slice(n);
  const box = page.locator('#devClock');
  const dlg = page.locator('#dlg');

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  let sent = await cmds();
  check([X8P.id, TWX.id].every(id => sent.some(x => x.cmd === 'clock-get' && x.unit === id) && sent.some(x => x.cmd === 'panel-get' && x.unit === id)),
    'a units list: clock-get and panel-get for each unit');
  check(!sent.some(x => x.cmd === 'panel-link'), 'no panel-link while the link is off (the default)');

  await host(x8Clock());
  await host(twClock());
  await host(panel(TWX.id, false));
  await host(panel(X8P.id, true));
  check(/INT · LOCKED/.test(await page.locator('.useg[data-unit="0"] .clk-badge').textContent()) &&
    await page.locator('.useg[data-unit="0"] .clk-badge').isVisible(), 'the x8p segment head: a clock badge "INT · LOCKED"');
  const tbNote = page.locator('.useg[data-unit="1"] .tb-strip .tb-nosig');
  check(await tbNote.isVisible() && /talkback mic: no signal yet \(its input strip is not built in this driver\)/.test(await tbNote.textContent()),
    'the Twin X talkback strip: "talkback mic: no signal yet (its input strip is not built in this driver)"');
  check(await page.locator('.useg[data-unit="0"] .tb-strip .tb-nosig:visible').count() === 0, 'the x8p (strip true): no note');

  // the Device view: the x8p
  await page.evaluate(() => { selectUnit(0); showView('device'); });
  const x8Sel = page.getByRole('combobox', { name: 'x8p clock source' });
  const opts = await x8Sel.locator('option').evaluateAll(o => o.map(x => `${x.textContent}${x.disabled ? ' [off]' : ''}`));
  check(opts.join('|') === 'Internal|S/PDIF (Experimental)|ADAT (Experimental): set the optical input to ADAT first [off]|Word clock (Experimental)',
    `x8p: the sources, Experimental marked, ADAT disabled with the reason (${opts.join('|')})`);
  const txt = async () => (await box.textContent()).replace(/\s+/g, ' ');
  let t = await txt();
  check(/LOCKED/.test(t) && !/NOT LOCKED/.test(t), 'x8p: LOCKED');
  check(/Measured 47999\.722 Hz/.test(t) && /clockCfg 0x20C/.test(t) && /rateInfo 0x201/.test(t) && /clockInfo 48000/.test(t), 'x8p: measured Hz and the raw registers in the details line');
  check(/Optical in/.test(t) && /Optical out/.test(t) && /SR Convert/.test(t) && /ADAT in 8 ch, S\/PDIF in 2 ch/.test(t),
    'x8p digital I/O: optical in, optical out, SR Convert, "ADAT in 8 ch, S/PDIF in 2 ch"');
  check(/Talkback mic: no signal yet/.test(t) === false && /Knob: MONITOR/.test(t), 'x8p front panel: the knob mode, no talkback note (strip true)');

  // the Twin X
  await box.getByRole('button', { name: 'Twin X', exact: true }).click();
  const twSel = page.getByRole('combobox', { name: 'Twin X clock source' });
  t = await txt();
  const twOpts = await twSel.locator('option').evaluateAll(o => o.map(x => `${x.textContent}${x.disabled ? ' [off]' : ''}`));
  check(twOpts.join('|') === 'Internal|S/PDIF (Experimental)|ADAT (Experimental): set the optical input to ADAT first [off]' &&
    /Optical in/.test(t) && !/Optical out/.test(t) && /Talkback mic: no signal yet \(its input strip is not built in this driver\)/.test(t),
  `Twin X: its sources, optical input only, the talkback note (${twOpts.join('|')})`);

  // an experimental source: cancel sends nothing, confirm sends try:true
  let n = await mark();
  await twSel.selectOption('spdif');
  await wait(150);
  check(await dlg.isVisible() && /never run on these units: the unit may go silent for up to 2 s and comes back on Internal if the source does not lock/.test(await dlg.textContent()),
    'S/PDIF (experimental): the confirmation, worded as 1.10.4');
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  await wait(150);
  check((await since(n)).length === 0 && await twSel.inputValue() === 'internal', 'cancelled: nothing sent, the selector back on Internal');
  await twSel.selectOption('spdif');
  await wait(150);
  await dlg.getByRole('button', { name: 'Use S/PDIF' }).click();
  await wait(150);
  let s = await since(n);
  check(s.length === 1 && s[0].cmd === 'clock-set' && s[0].unit === TWX.id && s[0].source === 'spdif' && s[0].try === true && s[0].hold === false,
    `confirmed: clock-set with try:true (${JSON.stringify(s)})`);
  check(await twSel.isDisabled(), 'sent: the selector waits');
  await host({ type: 'clock-result', unit: TWX.id, what: 'source', value: 'spdif', pending: true, code: '', why: 'Switching the clock (up to 4 s; it goes back to Internal if the source does not lock)', ms: 0 });
  check(await twSel.isDisabled() && /Switching the clock \(up to 4 s/.test(await txt()), 'pending: disabled, the host\'s words shown');
  const lockWhy = 'S/PDIF was not acknowledged in 2 s (no signal on the input?): back on Internal';
  await host({ type: 'clock-result', unit: TWX.id, what: 'source', value: 'spdif', pending: false, ok: false, code: 'lock', why: lockWhy, ms: 2260 });
  await host(twClock());
  check(!(await twSel.isDisabled()) && (await txt()).includes(lockWhy) && await twSel.inputValue() === 'internal', 'final: enabled again, the why shown as is');
  check(await page.evaluate(id => !(cfg.unitPrefs[id] && cfg.unitPrefs[id].clock), TWX.id), 'a failed change is not saved');

  // a proven source: Internal, no try, no dialog
  await host(x8Clock({ source: 'wordclock', wanted: 'wordclock' }));
  await box.getByRole('button', { name: 'x8p', exact: true }).click();
  n = await mark();
  await x8Sel.selectOption('internal');
  await wait(150);
  s = await since(n);
  check(!(await dlg.isVisible()) && s.length === 1 && JSON.stringify(s[0]) === JSON.stringify({ cmd: 'clock-set', unit: X8P.id, source: 'internal' }),
    `Internal (proven): sent at once, without try (${JSON.stringify(s)})`);
  check(await x8Sel.isDisabled(), 'Internal: the selector waits for the result');
  await host({ type: 'clock-result', unit: X8P.id, what: 'source', value: 'internal', pending: true, code: '', why: 'Switching the clock (up to 4 s; it goes back to Internal if the source does not lock)', ms: 0 });
  check(await x8Sel.isDisabled(), 'pending result: still disabled');
  await host({ type: 'clock-result', unit: X8P.id, what: 'source', value: 'internal', pending: false, ok: true, code: '', why: 'On Internal, locked', ms: 1010 });
  await host(x8Clock());
  check(!(await x8Sel.isDisabled()) && /On Internal, locked/.test(await txt()), 'final ok: enabled, its words shown');
  const prefs = await page.evaluate(id => ({ p: cfg.unitPrefs[id], slot0: unitPrefs(0), ch: cfg.channels.length }), X8P.id);
  check(prefs.p && prefs.p.clock && prefs.p.clock.source === 'internal' && prefs.p.clock.hold === false && Array.isArray(prefs.p.channels) && prefs.slot0 === null && prefs.ch === 8,
    `saved after ok: cfg.unitPrefs[x8p id].clock; unitPrefs(0) still null, cfg.channels kept (${JSON.stringify(prefs.p && prefs.p.clock)})`);

  // 192 kHz: digital I/O off
  await host(x8Clock({ rate: 192000 }, { channels: { adatIn: 0, adatOut: 0, spdifIn: 0, spdifOut: 0 } }));
  check(/Digital I\/O off at this rate/i.test(await txt()), '192 kHz: "digital I/O off at this rate"');
  await host(x8Clock());

  // the fall-back: notice, toast, Try again
  await box.getByRole('button', { name: 'Twin X', exact: true }).click();
  await host(twClock({ source: 'internal', wanted: 'adat', fellBack: true }, { opticalIn: 'adat', known: true }));
  check(/ADAT lost lock: back on Internal/.test(await txt()) && /ADAT lost lock, back on Internal/.test(await page.locator('#toasts').textContent()),
    'fellBack: "ADAT lost lock: back on Internal" in the panel and a toast');
  check(/ADAT LOST · INT/.test(await page.locator('.useg[data-unit="1"] .clk-badge').textContent()), 'fellBack: the Twin X badge says so');
  n = await mark();
  await box.getByRole('button', { name: 'Try again' }).click();
  await wait(100);
  s = await since(n);
  check(s.length === 1 && s[0].cmd === 'clock-set' && s[0].unit === TWX.id && s[0].source === 'adat' && s[0].try === true, `Try again: the same clock-set (${JSON.stringify(s)})`);
  await host({ type: 'clock-result', unit: TWX.id, what: 'source', value: 'adat', pending: false, ok: false, code: 'lock', why: 'ADAT was not acknowledged in 2 s (no signal on the input?): back on Internal', ms: 2200 });
  await host(twClock());

  // digital-set: the optical input
  n = await mark();
  await page.getByRole('group', { name: 'Twin X optical input format' }).getByRole('button', { name: 'ADAT' }).click();
  await wait(150);
  check(await dlg.isVisible() && /experimental, never run on these units/.test(await dlg.textContent()), 'optical input ADAT: Experimental, a confirmation');
  await dlg.getByRole('button', { name: 'Set to ADAT' }).click();
  await wait(150);
  s = await since(n);
  check(s.length === 1 && JSON.stringify(s[0]) === JSON.stringify({ cmd: 'digital-set', unit: TWX.id, what: 'optical-in', value: 'adat', try: true }),
    `digital-set optical-in adat, try:true (${JSON.stringify(s)})`);
  await host({ type: 'clock-result', unit: TWX.id, what: 'optical-in', value: 'adat', pending: true, code: '', why: 'Writing the format', ms: 0 });
  check(await twSel.isDisabled(), 'a format write pending: the clock selector waits too');
  await host({ type: 'clock-result', unit: TWX.id, what: 'optical-in', value: 'adat', pending: false, ok: true, code: '', why: '', ms: 40 });
  await host(twClock({ usable: ['internal', 'adat'] }, { opticalIn: 'adat', known: true, channels: { adatIn: 8, adatOut: 0, spdifIn: 0, spdifOut: 0 } }));
  check(await page.evaluate(id => cfg.unitPrefs[id] && cfg.unitPrefs[id].digital && cfg.unitPrefs[id].digital.opticalIn, TWX.id) === 'adat' &&
    /ADAT in 8 ch/.test(await txt()) && !(await twSel.isDisabled()), 'ok: saved as cfg.unitPrefs[Twin X id].digital.opticalIn, channels follow');

  // the hold checkbox goes with the next change
  await box.getByRole('checkbox', { name: 'Stay on the source when its signal drops' }).check();
  n = await mark();
  await twSel.selectOption('adat');
  await wait(150);
  await dlg.getByRole('button', { name: 'Use ADAT' }).click();
  await wait(150);
  s = await since(n);
  check(s.length === 1 && s[0].source === 'adat' && s[0].hold === true && s[0].try === true, `hold checked: clock-set with hold:true (${JSON.stringify(s)})`);
  await host({ type: 'clock-result', unit: TWX.id, what: 'source', value: 'adat', pending: false, ok: true, code: '', why: 'On ADAT, locked', ms: 1100 });
  await host(twClock({ source: 'adat', wanted: 'adat', hold: true, usable: ['internal', 'adat'] }, { opticalIn: 'adat', known: true }));
  check(JSON.stringify(await page.evaluate(id => cfg.unitPrefs[id].clock, TWX.id)) === '{"source":"adat","hold":true}', 'ok: cfg.unitPrefs[Twin X id].clock = {adat, hold}');

  // the monitor link (Units & routing)
  await page.evaluate(() => showView('units'));
  const linkBox = page.locator('.clk-link');
  check(await linkBox.isVisible() && /Monitor link/.test(await linkBox.textContent()), 'Units & routing: the Monitor link panel');
  n = await mark();
  await linkBox.getByRole('group', { name: 'Speakers on' }).getByRole('button', { name: 'Twin X' }).click();
  await wait(100);
  s = await since(n);
  check(s.length === 1 && JSON.stringify(s[0]) === JSON.stringify({ cmd: 'panel-link', monitor: TWX.id, level: true, mute: true, dim: true, mono: false, talkDim: true }) &&
    await page.evaluate(() => cfg.panelLink.monitor) === TWX.id, `speakers on the Twin X: panel-link sent and saved (${JSON.stringify(s)})`);
  n = await mark();
  await linkBox.getByRole('checkbox', { name: 'Mono' }).check();
  await wait(100);
  s = await since(n);
  check(s.length === 1 && s[0].mono === true && await page.evaluate(() => cfg.panelLink.mono) === true, 'Mono: sent again with mono:true, saved');
  await host({ type: 'panel-link', ok: false, code: 'error', why: 'The device is not ready.', monitor: null });
  check(/The device is not ready\./.test(await linkBox.textContent()), 'a refused panel-link: the host\'s words in the panel');
  await host({ type: 'panel-link', ok: true, code: '', why: 'the monitor link is on', monitor: TWX.id });
  await host(panel(TWX.id, false, { on: true, monitor: TWX.id, level: true, mute: true, dim: true, mono: true, talkDim: true, applied: 3, busy: 0, dropped: 1 }));
  t = (await linkBox.textContent()).replace(/\s+/g, ' ');
  check(/LINK ON/.test(t) && /Passed on 3/.test(t) && /dropped 1/.test(t) && !/not ready/.test(t), `the link's state: on, applied, dropped (${t.slice(t.indexOf('On:'), t.indexOf('On:') + 120)})`);
  n = await mark();
  await linkBox.getByRole('group', { name: 'Speakers on' }).getByRole('button', { name: 'Off' }).click();
  await wait(100);
  s = await since(n);
  check(s.length === 1 && JSON.stringify(s[0]) === JSON.stringify({ cmd: 'panel-link', monitor: null }) && await page.evaluate(() => cfg.panelLink.monitor) === '',
    'Off: panel-link monitor:null, saved as off');

  /* ---------------------------------------------------------------- B: saved prefs and link, sent after the units list */
  const p2 = await stubPage(browser, errors);
  const host2 = m => p2.evaluate(x => onHostMessage(x), m);
  const cmds2 = () => p2.evaluate(() => window.__sent.filter(s => s[0] === '{').map(s => JSON.parse(s)).filter(x => /^(clock|digital|panel)-/.test(x.cmd)));
  const saved = {
    unitPrefs: { [TWX.id]: { clock: { source: 'adat', hold: true }, digital: { opticalIn: 'adat', bogus: 1 } }, [X8P.id]: { clock: { source: 'internal', hold: false } } },
    panelLink: { monitor: TWX.id, level: true, mute: false, dim: true, mono: false, talkDim: true },
  };
  await host2({ type: 'hello', app: 'test', config: JSON.stringify(saved), demo: false, view: 'control' });
  const loaded = await p2.evaluate(id => cfg.unitPrefs[id], TWX.id);
  check(loaded && JSON.stringify(loaded.clock) === '{"source":"adat","hold":true}' && JSON.stringify(loaded.digital) === '{"opticalIn":"adat"}' &&
    Array.isArray(loaded.channels) && loaded.channels.length === 8 && loaded.hizImp === -1, 'a saved config: clock and digital prefs kept (cleaned), names and Hi-Z as before');
  await host2({ type: 'status', present: true, info: INFO });
  await host2({ type: 'units', units: [X8P] });
  check(!(await cmds2()).some(x => x.cmd === 'panel-link'), 'one unit: no panel-link yet');
  await host2({ type: 'units', units: [X8P, TWX] });
  await host2({ type: 'status', unit: 1, present: true, info: TW_INFO });
  let c2 = await cmds2();
  let links = c2.filter(x => x.cmd === 'panel-link');
  check(links.length === 1 && JSON.stringify(links[0]) === JSON.stringify({ cmd: 'panel-link', monitor: TWX.id, level: true, mute: false, dim: true, mono: false, talkDim: true }),
    `both units in the list: the saved panel-link sent (${JSON.stringify(links)})`);
  await host2({ type: 'units', units: [X8P, TWX] });
  check((await cmds2()).filter(x => x.cmd === 'panel-link').length === 1, 'the same pair again: not sent twice');
  check(c2.some(x => x.cmd === 'clock-get' && x.unit === TWX.id) && !c2.some(x => x.cmd === 'clock-set' || x.cmd === 'digital-set'), 'arrival: clock-get first, nothing set blind');
  await host2(x8Clock());                               /* the x8p's saved Internal: nothing to send */
  await host2(twClock());
  c2 = await cmds2();
  let sets = c2.filter(x => x.cmd === 'clock-set' || x.cmd === 'digital-set');
  check(sets.length === 1 && JSON.stringify(sets[0]) === JSON.stringify({ cmd: 'digital-set', unit: TWX.id, what: 'optical-in', value: 'adat', try: true }),
    `the Twin X's clock: its saved optical format goes out first, try:true (${JSON.stringify(sets)})`);
  await host2({ type: 'clock-result', unit: TWX.id, what: 'optical-in', value: 'adat', pending: true, code: '', why: 'Writing the format', ms: 0 });
  await host2({ type: 'clock-result', unit: TWX.id, what: 'optical-in', value: 'adat', pending: false, ok: true, code: '', why: '', ms: 30 });
  sets = (await cmds2()).filter(x => x.cmd === 'clock-set' || x.cmd === 'digital-set');
  check(sets.length === 2 && JSON.stringify(sets[1]) === JSON.stringify({ cmd: 'clock-set', unit: TWX.id, source: 'adat', try: true, hold: true }),
    `then the saved clock: clock-set adat, try:true, hold:true (${JSON.stringify(sets[1])})`);
  await host2({ type: 'clock-result', unit: TWX.id, what: 'source', value: 'adat', pending: false, ok: true, code: '', why: 'On ADAT, locked', ms: 1200 });
  // the Twin X leaves and comes back: asked again, and the link goes out again
  await host2({ type: 'units', units: [X8P] });
  await host2({ type: 'units', units: [X8P, TWX] });
  c2 = await cmds2();
  check(c2.filter(x => x.cmd === 'panel-link').length === 2 && c2.filter(x => x.cmd === 'clock-get' && x.unit === TWX.id).length === 2,
    'the Twin X back: clock-get again, the panel-link again');
  await host2(twClock({ source: 'adat', wanted: 'adat', hold: true, usable: ['internal', 'adat'] }, { opticalIn: 'adat', known: true }));
  check((await cmds2()).filter(x => x.cmd === 'clock-set' || x.cmd === 'digital-set').length === 2, 'its clock already as saved: nothing sent');

  /* ---------------------------------------------------------------- C: the demo (no host) */
  const p3 = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  p3.on('pageerror', e => errors.push(`demo: ${e.message}`));
  p3.on('console', m => { if (m.type() === 'error') errors.push(`demo: ${m.text()}`); });
  await p3.goto(ui + '?view=device');
  await wait(300);
  const dsel = p3.getByRole('combobox', { name: 'x8p clock source' });
  check(await dsel.isVisible() && /Optical in/.test(await p3.locator('#devClock').textContent()), 'demo: the Device view shows a clock state');
  await dsel.selectOption('wordclock');
  await wait(150);
  await p3.locator('#dlg').getByRole('button', { name: 'Use Word clock' }).click();
  await wait(1300);
  check(/Demo: on Word clock, locked \(simulated\)/.test(await p3.locator('#devClock').textContent()) && await dsel.inputValue() === 'wordclock',
    'demo: a change answers like a host, simulated');
  check(await p3.evaluate(() => !Object.values(cfg.unitPrefs).some(p => p.clock)), 'demo: nothing saved');
  await p3.evaluate(() => showView('units'));
  check(/Not in the demo/.test(await p3.locator('.clk-link').textContent()), 'demo: the monitor link says "Not in the demo"');
  await p3.evaluate(() => showView('control'));
  await wait(200);
  check(await p3.locator('.useg[data-unit="1"] .tb-strip .tb-nosig').isVisible(), 'demo: the Twin X talkback note');

  check(errors.length === 0, `no page errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
  await browser.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})();
