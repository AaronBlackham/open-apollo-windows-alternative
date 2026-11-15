// Control view with two units side by side, one segment each (no tabs): the focus (the monitor column's unit
// buttons, remembered), the Twin X's strips (2 inputs, link 1-2, Hi-Z impedance on input 1) and its master and
// monitor (level, mute, dim, mono, talkback, HP level, [Monitor] source and HP source; no dim level), per-unit state,
// readback, pending writes and meters, sends tagged with the unit, refusals shown inline (and reverted), a read-only
// unit, a unit that leaves (greyed in place) and comes back, the Twin X alone, the --view pin, the demo, and 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'C:/Users/aaron/Desktop/Avium/Avium Invoid/node_modules/playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
const check = (c, l) => console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`);

const X8P = { id: 'SN22532017106180', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 4, playChannels: 10,
  recChannels: 24, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };

const words = f => { const w = new Array(40).fill(0); f(w); w[38] = 0x00010203; return w.map(x => x >>> 0); };
/* x8p: input 1 48V at 42 dB, monitor -26 dB, HP 1 raw 180. */
const X8P_RB = words(w => { w[0] = 1 << 3; w[3] = 33; w[2] = 140 | (180 << 8); });
/* Twin X: input 1 MIC at 30 dB with a cable in the Hi-Z jack, input 2 LINE at 10 dB, monitor -21 dB muted, HP raw 140, clock locked. */
const twRb = (gc1 = 21, extra = () => {}) => words(w => {
  w[0] = (1 << 6) | (1 << 24); w[3] = gc1 | (1 << 6); w[2] = 150 | (140 << 8) | (1 << 16); w[6] = (1 << 30) | (1 << 31); extra(w);
});

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const watch = p => {
    p.on('pageerror', e => errors.push(e.message));
    p.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  };
  const stub = () => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  };

  /* ---------------- A. Host path: x8p and Twin X ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx.addInitScript(stub);
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  /* hardware commands only: trayState and saveConfig are debounced and may land in any window */
  const sentSince = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^(input|monitor)\t/.test(s)), n);
  const unitBtn = name => page.locator('#monUnits').getByRole('button', { name, exact: true });
  const tw = page.locator('.useg[data-unit="1"]'), x8 = page.locator('.useg[data-unit="0"]');
  const twGain = ch => tw.locator(`.strip[data-ch="${ch}"] .st-pre .k-readout`);
  const pressed = async loc => (await loc.getAttribute('aria-pressed')) === 'true';
  const toasts = () => page.locator('.toast').allTextContents().then(t => t.join(' | '));
  const clearToasts = () => page.evaluate(() => document.getElementById('toasts').replaceChildren());
  const notes = () => page.locator('#ctlBar .ctl-note').allTextContents().then(t => t.join(' | '));
  const strips = page.locator('#inputs .strip');
  const state = idx => page.evaluate(i => { const c = S.ctl[i]; return c ? { inputs: c.inputs.map(x => ({ ...x })), links: [...c.links], mon: { ...c.mon },
    pending: [...c.pending.keys()], refused: [...c.refused.keys()] } : null; }, idx);
  const twUp = async (info = TW_INFO, entry = TWX, rb = twRb()) => {
    await host({ type: 'units', units: [X8P, entry] });
    await host({ type: 'status', unit: entry.index, present: true, info });
    if (rb) await host({ type: 'readback', unit: entry.index, valid: true, seq: 1, words: rb });
  };

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P] });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: X8P_RB });

  // 1. One unit: one segment, no unit buttons.
  check(await page.locator('#monUnits').isHidden() && await page.locator('#inputs .useg').count() === 1, 'one unit: one segment, no unit buttons');
  check(await strips.count() === 8 && await page.evaluate(() => S.sel) === 0, 'x8p: eight strips');

  // 2. The Twin X arrives: its segment beside the x8p's, the monitor column's buttons list both, the x8p stays focused and untouched.
  const x8pBefore = await state(0);
  await twUp();
  check(await unitBtn('x8p').isVisible() && await unitBtn('Twin X').isVisible() && await page.locator('#monUnits button').count() === 2 &&
    await page.locator('#inputs .useg').count() === 2 && await page.locator('#ctlBar .ctl-units').isHidden(),
    'two units: both segments side by side, no tabs; the monitor column\'s buttons list both');
  check(await pressed(unitBtn('x8p')) && !(await pressed(unitBtn('Twin X'))), 'the x8p stays selected');
  const x8pAfter = await state(0);
  check(JSON.stringify(x8pAfter.inputs) === JSON.stringify(x8pBefore.inputs) && x8pAfter.mon.level === 140 && x8pAfter.mon.mute === false,
    'the Twin X readback leaves the x8p controls alone');
  const tw0 = await state(1);
  check(tw0 && tw0.inputs[0].gain === 30 && tw0.inputs[0].hiz && tw0.inputs[1].line && tw0.inputs[1].gain === 10 && tw0.mon.level === 150 && tw0.mon.mute,
    'the Twin X keeps its own control state from its readback while not shown');
  check(await strips.count() === 10 && await tw.locator('.strip').count() === 2 &&
    await twGain(0).textContent() === '30 dB', 'ten strips at once: the Twin X\'s two show its own state');

  // 3. Focus the Twin X: its strips, link, Hi-Z, master, monitor and headphones.
  await unitBtn('Twin X').click();
  check(await pressed(unitBtn('Twin X')) && await page.evaluate(() => S.sel) === 1, 'Twin X focused');
  check(await tw.locator('.strip').count() === 2 && await tw.locator('.pair').count() === 1, 'Twin X: two strips in one pair');
  check(await tw.locator('.link-btn').count() === 1 && /1 - 2/.test(await tw.locator('.pair-name').textContent()), 'Twin X: one LINK, on 1-2');
  check(await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).count() === 1 &&
    await page.getByRole('combobox', { name: 'Twin X input 2 Hi-Z impedance' }).count() === 0, 'Twin X: Hi-Z impedance on input 1 only');
  const loads = await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).locator('option').allTextContents();
  check(loads.slice(1).join() === '2.2M,1M,453k,232k,100k,70k,47k,21k', `Hi-Z impedance labels (${loads.join()})`);
  check(await tw.locator('.strip[data-ch="0"] .tag').count() === 1 && await tw.locator('.strip[data-ch="1"] .tag').count() === 0 &&
    (await tw.locator('.strip[data-ch="0"] .tag').getAttribute('class')).includes('on'), 'Twin X: HI-Z jack on input 1, lit by readback');
  check(await twGain(0).textContent() === '30 dB' && await twGain(1).textContent() === '10 dB', 'Twin X gains from its readback');
  check(await pressed(page.getByRole('group', { name: 'Twin X input 2 mic or line' }).getByRole('button', { name: 'LINE' })), 'Twin X input 2 on LINE');
  check(await page.getByRole('slider', { name: 'Twin X monitor level', exact: true }).getAttribute('aria-valuetext') === '-21.0 dB' &&
    await pressed(page.getByRole('button', { name: 'Twin X monitor mute', exact: true })), 'Twin X master: level and mute from its readback');
  check(await page.getByRole('slider', { name: 'Headphone 1 level' }).isVisible(), 'Twin X: HP level knob shown (digital)');
  check(await page.locator('#dimLevel').isHidden() &&
    (await page.getByRole('group', { name: 'Monitor source' }).getByRole('button').allTextContents()).join() === 'MIX,HP,LINE 3/4',
    'Twin X: no dim level; [Monitor] monitor source MIX / HP / LINE 3/4');
  check(await page.getByRole('button', { name: 'TALKBACK' }).isVisible(), 'Twin X: talkback');
  check(await page.locator('#brandModel').textContent() === 'Apollo Twin X' && /^2253202500117 · 48 kHz · clock running$/.test(await page.locator('#connText').textContent()),
    'top bar names the Twin X');
  await page.waitForTimeout(500);
  const saved = await page.evaluate(() => window.__sent.filter(s => s.startsWith('saveConfig\t')).pop() || '');
  check(JSON.parse(saved.split('\t')[1] || '{}').controlUnit === TWX.id, 'the choice is saved (controlUnit = the Twin X id)');
  await page.screenshot({ path: path.join(out, 'ui-control-twinx.png') });

  // 4. Sends carry the unit; pending is per unit.
  let n = await mark();
  await page.getByRole('slider', { name: 'Twin X input 1 gain', exact: true }).focus();
  await page.keyboard.press('PageUp');
  await page.waitForTimeout(100);
  let s = await sentSince(n);
  check(s.includes('input\t0\tgain\t36\t1'), `gain to the Twin X: ${JSON.stringify(s)}`);
  check((await state(1)).pending.includes('in:0:gain') && !(await state(0)).pending.includes('in:0:gain'), 'pending on the Twin X only');
  await host({ type: 'readback', unit: 0, valid: true, seq: 2, words: X8P_RB });
  check((await state(1)).pending.includes('in:0:gain'), 'an x8p readback does not confirm a Twin X write');
  await host({ type: 'readback', unit: 1, valid: true, seq: 2, words: twRb(27) });
  check(!(await state(1)).pending.includes('in:0:gain') && (await state(1)).inputs[0].gain === 36, 'the Twin X readback confirms it');

  n = await mark();
  await tw.locator('.link-btn').click();
  await page.waitForTimeout(100);
  s = await sentSince(n);
  check(s[0] === 'input\t0\tlink\t1\t1' && s.includes('input\t1\tline\t0\t1') && s.includes('input\t1\tgain\t36\t1'),
    `link 1-2 on the Twin X copies input 1 to 2 (${JSON.stringify(s)})`);
  check((await state(1)).links[0] && !(await state(0)).links[0], 'link state per unit');

  n = await mark();
  await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).selectOption('2');
  s = await sentSince(n);
  check(s.includes('input\t0\thizimp\t2\t1'), `Hi-Z impedance 453k: ${JSON.stringify(s)}`);
  check(await page.evaluate(id => cfg.unitPrefs[id] && cfg.unitPrefs[id].hizImp, TWX.id) === 2, 'Hi-Z impedance kept per unit (no readback field)');

  n = await mark();
  await page.locator('.brand').click();   // focus off any control (not a fixed spot in the tab bar, which grows with tabs)
  await page.keyboard.press('m');
  await page.getByRole('slider', { name: 'Headphone 1 level' }).focus();
  await page.keyboard.press('PageUp');
  await page.getByRole('button', { name: 'TALKBACK' }).focus();
  await page.keyboard.press('Enter');
  await page.evaluate(() => { setMon('dimlevel', 3); setMon('source', 1); setMon('hp1src', 1); });
  await page.waitForTimeout(100);
  s = await sentSince(n);
  check(s.includes('monitor\tmute\t0\t1') && s.includes('monitor\thp1\t152\t1') && s.includes('monitor\ttalkback\t1\t1'),
    `M, HP level and talkback go to the Twin X (${JSON.stringify(s)})`);
  check(!s.some(x => /^monitor\tdimlevel/.test(x)) && s.includes('monitor\tsource\t1\t1') && s.includes('monitor\thp1src\t1\t1'),
    'no dim level to the Twin X; [Monitor] its source (HP) and HP source (LINE 3/4) go out');
  check((await state(0)).mon.mute === false && (await state(0)).mon.hp1 === 180, 'the x8p monitor untouched');

  // 5. Meters: each segment its own unit's levels.
  await host({ type: 'levels', unit: 1, valid: true, peaks: [4194304, 0] });
  await host({ type: 'levels', unit: 0, valid: true, peaks: [838861, 838861] });
  await page.waitForTimeout(300);
  check(/^-6\.0$/.test(await tw.locator('.strip[data-ch="0"] .strip-peak').textContent()) &&
    !(await tw.locator('.strip[data-ch="0"] .meter').getAttribute('class')).includes('noclock'), 'Twin X meter from the Twin X levels');
  check(/^-20\.0$/.test(await x8.locator('.strip[data-ch="0"] .strip-peak').textContent()), 'the x8p\'s meter from the x8p levels, at the same time');

  // 6. Refusals: inline, reverted, no toast; a passing error still toasts.
  await clearToasts();
  await page.getByRole('slider', { name: 'Twin X input 1 gain', exact: true }).focus();
  await page.keyboard.press('PageUp');                                   /* 36 -> 42 */
  await host({ type: 'error', op: 'input', key: 'gain', index: 0, unit: 1, code: 50, detail: 'The request is not supported' });
  let nt = await notes();
  check(/Twin X input 1 gain was not changed: not supported by the installed driver on the Apollo Twin X \(Gen 1\)\./.test(nt), `inline note: "${nt}"`);
  check(!/input 1 gain/.test(await toasts()), 'no toast for a refusal on the unit on screen');
  check(await twGain(0).textContent() === '36 dB' &&
    (await tw.locator('.strip[data-ch="0"] .st-pre .knob').getAttribute('class')).includes('refused'), 'gain back to what the unit holds, marked refused');
  await host({ type: 'error', op: 'monitor', key: 'talkback', index: 70, unit: 1, code: 50, detail: 'The request is not supported' });
  await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).selectOption('5');
  await host({ type: 'error', op: 'input', key: 'hizimp', index: 0, unit: 1, code: 50, detail: 'The request is not supported' });
  nt = await notes();
  check(/Twin X input 1 gain, Twin X monitor talkback and Twin X input 1 Hi-Z impedance were not changed/.test(nt), `refusals grouped: "${nt}"`);
  check(!(await state(1)).mon.talkback && (await page.getByRole('button', { name: 'TALKBACK' }).getAttribute('class')).includes('refused'),
    'talkback reverted (readback off) and marked');
  check(await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).inputValue() === '2' &&
    await page.evaluate(id => cfg.unitPrefs[id].hizImp, TWX.id) === 2, 'Hi-Z impedance back to the last value that was not refused');
  await page.screenshot({ path: path.join(out, 'ui-control-twinx-refused.png') });
  await host({ type: 'error', op: 'monitor', key: 'level', index: 1, unit: 1, code: 21, detail: 'The device is not ready' });
  check(/Twin X monitor level was not changed: the mixer is not ready yet/.test(await toasts()) && !/monitor level/.test(await notes()),
    'a passing error (not ready) is a toast, not a note');
  await page.locator('#ctlBar').getByRole('button', { name: 'Dismiss' }).click();
  check(await page.locator('#ctlBar .ctl-notes').isHidden() && (await state(1)).refused.length === 0 &&
    !(await tw.locator('.strip[data-ch="0"] .st-pre .knob').getAttribute('class')).includes('refused'), 'Dismiss clears the note and the marks');

  // 7. Focus back on the x8p: its own state, no Twin X marks; a Twin X refusal shows in the bar (its segment is on screen).
  await unitBtn('x8p').click();
  check(await x8.locator('.strip').count() === 8 && await x8.locator('.strip[data-ch="0"] .st-pre .k-readout').textContent() === '42 dB' &&
    await page.getByRole('slider', { name: 'Monitor level', exact: true }).getAttribute('aria-valuetext') === '-26.0 dB', 'x8p: its own strips and master');
  check(await page.getByRole('slider', { name: 'Headphone 1 level' }).isHidden() && await page.locator('#dimLevel').isVisible(),
    'x8p: HP knob hidden, dim level shown');
  check(await page.locator('#ctlBar .ctl-notes').isHidden(), 'x8p: no notes');
  await clearToasts();
  await host({ type: 'error', op: 'input', key: 'pad', index: 1, unit: 1, code: 50, detail: 'The request is not supported' });
  check(/Twin X input 2 PAD was not changed/.test(await notes()) && !/PAD/.test(await toasts()),
    'a refusal for the Twin X while the x8p is focused: its note in the bar, no toast (both segments are on screen)');
  await unitBtn('Twin X').click();
  check(/Twin X input 2 PAD was not changed/.test(await notes()), '... and still there with the Twin X focused');
  await page.locator('#ctlBar').getByRole('button', { name: 'Dismiss' }).click();

  // 8. The driver reports no preamps for the Twin X (the G6 build): a note before anything fails.
  await host({ type: 'status', unit: 1, present: true, info: { ...TW_INFO, numPreamps: 0 } });
  check(/reports no preamps for the Apollo Twin X \(Gen 1\), so it refuses input settings/.test(await notes()), 'no-preamp driver: note shown up front');
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  check(await page.locator('#ctlBar .ctl-notes').isHidden(), 'preamps reported: note gone');

  // 9. Read-only (observe) Twin X: banner, dimmed, nothing sent.
  await host({ type: 'units', units: [X8P, { ...TWX, mode: 0, observe: true, active: false, stage: 1 }] });
  await host({ type: 'status', unit: 1, present: true, info: { ...TW_INFO, mode: 0, observe: true, stage: 1, flags: 0x1 } });
  check(await page.evaluate(() => S.sel) === 1 && /Twin X is in observe mode, so it is read-only/.test(await page.locator('#bannerText').textContent()),
    'observe Twin X: read-only banner');
  check(await page.evaluate(() => document.body.dataset.live) === '0', 'observe Twin X: controls dimmed');
  n = await mark();
  await tw.locator('.strip[data-ch="1"]').getByRole('button', { name: 'PAD' }).click({ force: true });
  await page.evaluate(() => setMon('mute', !S.mon.mute));
  check((await sentSince(n)).length === 0, 'observe Twin X: nothing sent');
  await twUp();
  check(await page.evaluate(() => document.body.dataset.live) === '1', 'back in full mode: live');

  // 10. The Twin X leaves: its segment greys out in place (nothing reflows), focus back on the x8p, late messages dropped;
  //     it returns: live again and focused.
  await clearToasts();
  await host({ type: 'status', unit: 1, present: false });
  await host({ type: 'units', units: [X8P] });
  check(await page.evaluate(() => S.sel) === 0 && await strips.count() === 10 && (await tw.getAttribute('class')).includes('gone') &&
    /DISCONNECTED/.test(await tw.locator('.useg-gone').textContent()) && await page.locator('#monUnits').isHidden(),
    'Twin X gone: its segment greyed in place, the x8p focused, no unit buttons');
  await host({ type: 'readback', unit: 1, valid: true, seq: 9, words: twRb() });
  await host({ type: 'levels', unit: 1, valid: true, peaks: [4194304] });
  check(await page.evaluate(() => S.ctl[1] === undefined), 'late readback and levels from the Twin X dropped');
  await twUp();
  check(await page.evaluate(() => S.sel) === 1 && await strips.count() === 10 && !(await tw.getAttribute('class')).includes('gone'),
    'the Twin X returns: its segment live again, focused again');
  check(await page.getByRole('combobox', { name: 'Twin X input 1 Hi-Z impedance' }).inputValue() === '2', 'its Hi-Z impedance is remembered');

  // 11. Tray mute acts on the selected unit.
  n = await mark();
  await host({ type: 'tray', action: 'mute' });
  check((await sentSince(n)).includes('monitor\tmute\t0\t1'), 'tray mute goes to the selected Twin X');
  const lastCfg = await page.evaluate(() => JSON.stringify(cfg));
  await ctx.close();

  /* ---------------- B. Restart, Twin X alone, --view pin ---------------- */
  const ctx2 = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx2.addInitScript(stub);
  const p2 = await ctx2.newPage();
  watch(p2);
  await p2.goto(ui);
  const host2 = m => p2.evaluate(x => onHostMessage(x), m);
  await host2({ type: 'hello', app: 'test', config: lastCfg, demo: false, view: 'control' });
  await host2({ type: 'status', present: true, info: INFO });
  await host2({ type: 'units', units: [X8P] });
  check(await p2.evaluate(() => S.sel) === 0, 'restart: the x8p while the Twin X is not listed');
  await host2({ type: 'units', units: [X8P, TWX] });
  await host2({ type: 'status', unit: 1, present: true, info: TW_INFO });
  check(await p2.evaluate(() => S.sel) === 1, 'restart: the remembered Twin X once listed');

  // Twin X alone (no x8p in slot 0).
  await host2({ type: 'status', present: false, error: 2, detail: 'No Apollo x8p on apollo.sys' });
  await host2({ type: 'units', units: [TWX] });
  await host2({ type: 'readback', unit: 1, valid: true, seq: 1, words: twRb() });
  check(await p2.evaluate(() => S.sel) === 1 && await p2.evaluate(() => document.body.dataset.live) === '1', 'Twin X alone: selected and live');
  check((await p2.locator('#banner').isHidden() || !/not running on the x8p/.test(await p2.locator('#bannerText').textContent())) &&
    await p2.locator('#monUnits').isHidden(),
    'Twin X alone: no x8p-absent banner, no unit buttons');
  const n2 = await p2.evaluate(() => window.__sent.length);
  await p2.evaluate(() => setInput(1, 'pad', true));
  check((await p2.evaluate(k => window.__sent.slice(k), n2)).includes('input\t1\tpad\t1\t1'), 'Twin X alone: sends to slot 1');

  // --view control:x8p beats the remembered Twin X until the user picks.
  await host2({ type: 'hello', app: 'test', config: lastCfg, demo: false, view: 'control:x8p' });
  await host2({ type: 'status', present: true, info: INFO });
  await host2({ type: 'units', units: [X8P, TWX] });
  check(await p2.evaluate(() => S.sel) === 0 && await p2.evaluate(() => S.view) === 'control', '--view control:x8p opens the x8p');
  await p2.locator('#monUnits').getByRole('button', { name: 'Twin X', exact: true }).click();
  await host2({ type: 'units', units: [X8P, TWX] });
  check(await p2.evaluate(() => S.sel) === 1, 'a pick ends the pin');
  await ctx2.close();

  /* ---------------- C. Demo ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 800 } });
  watch(demo);
  await demo.goto(ui + '?view=control:twinx');
  await demo.waitForTimeout(700);
  check(await demo.evaluate(() => S.sel) === 1 && await demo.locator('.useg[data-unit="1"] .strip').count() === 2 && await demo.locator('#inputs .strip').count() === 10,
    'demo: ?view=control:twinx focuses the Twin X, both units on screen');
  await demo.getByRole('slider', { name: 'Twin X input 2 gain', exact: true }).focus();
  await demo.keyboard.press('PageUp');
  await demo.waitForTimeout(400);
  const d1 = await demo.evaluate(() => ({ g: S.ctl[1].inputs[1].gain, p: [...S.ctl[1].pending.keys()], x: S.ctl[0].inputs[1].gain }));
  check(d1.g === 16 && !d1.p.length && d1.x === 32, `demo: Twin X gain confirmed by its simulated readback (${JSON.stringify(d1)})`);
  await demo.waitForTimeout(300);
  check(await demo.locator('.useg[data-unit="1"] .strip[data-ch="0"] .strip-peak').textContent() !== '--', 'demo: Twin X meters move');
  // An x8p preset recalled on the Twin X sets inputs 1-2 only.
  await demo.evaluate(() => { selectUnit(0); cfg.presets.push({ name: 'Eight', saved: 1, state: captureState() }); buildPresetMenu(); selectUnit(1); });
  await demo.getByRole('button', { name: 'Presets' }).click();
  await demo.getByRole('menuitem', { name: /Eight/ }).click();
  check(/Turn on 48V for Twin X input 2\?/.test(await demo.locator('#dlg h3').textContent()), 'demo: the 48V question names the Twin X input');
  await demo.getByRole('button', { name: 'Turn on 48V' }).click();
  await demo.waitForTimeout(500);
  const d2 = await demo.evaluate(() => ({ tw: S.ctl[1].inputs.slice(0, 2).map(i => i.gain), x: S.ctl[0].inputs.map(i => i.gain) }));
  check(d2.tw.join() === '38,32' && d2.x.join() === '38,32,24,24,10,10,10,10', `demo: x8p preset on the Twin X sets inputs 1-2 (${JSON.stringify(d2)})`);
  await demo.close();

  /* ---------------- D. 980 px ---------------- */
  const narrow = await browser.newPage({ viewport: { width: 980, height: 760 } });
  watch(narrow);
  await narrow.goto(ui + '?view=control:twinx');
  await narrow.waitForTimeout(700);
  const overflow = () => narrow.evaluate(() => {
    const bad = [], docW = document.documentElement.clientWidth;
    if (document.documentElement.scrollWidth > docW) bad.push('document');
    for (const q of ['#view-control', '#ctlBar', '#inputs', '#monitor']) {
      const node = document.querySelector(q);
      if (node && node.offsetParent && node.scrollWidth > node.clientWidth + 1) bad.push(q);
    }
    return bad;
  });
  let bad = (await overflow()).filter(b => b !== '#inputs');     /* ten strips scroll sideways inside their area at this width */
  check(!bad.length, `980 px, Twin X focused: no horizontal overflow beyond the strip scroller (${bad.join(', ') || 'none'})`);
  await narrow.evaluate(() => selectUnit(0));
  bad = (await overflow()).filter(b => b !== '#inputs');
  check(!bad.length, `980 px, x8p focused: no horizontal overflow beyond the strip scroller (${bad.join(', ') || 'none'})`);
  await narrow.screenshot({ path: path.join(out, 'ui-control-980.png') });
  await narrow.close();

  /* ---------------- E. A short window: banner, unit bar and a note (800 px at 125%) ---------------- */
  const short = await browser.newPage({ viewport: { width: 992, height: 590 } });
  watch(short);
  await short.goto(ui + '?view=control:twinx');
  await short.waitForTimeout(700);
  await short.evaluate(() => {
    cur().refused.set('in:0:gain', { what: 'Twin X input 1 gain', reason: 'not supported by the installed driver on the Apollo Twin X' });
    renderNotes(); renderAllInputs();
  });
  const clipped = await short.evaluate(() => {
    const bad = [];
    for (const s of document.querySelectorAll('#inputs .strip')) {
      const ch = s.dataset.ch, pair = s.closest('.pair').getBoundingClientRect();
      const h = q => { const n = s.querySelector(q); return n ? n.getBoundingClientRect().height : -1; };
      if (h('.strip-name') < 18) bad.push(`input ${+ch + 1} name ${h('.strip-name')}`);
      if (h('.seg') < 24) bad.push(`input ${+ch + 1} mic/line ${h('.seg')}`);
      if (h('.sw-grid') < 51) bad.push(`input ${+ch + 1} switches ${h('.sw-grid')}`);   /* two rows of 24 px switches, 3 px apart */
      if (s.querySelector('.strip-extra') && h('.strip-extra') < 26) bad.push(`input ${+ch + 1} Hi-Z row ${h('.strip-extra')}`);
      if (s.querySelector('.strip-peak').getBoundingClientRect().bottom > pair.bottom + 0.5) bad.push(`input ${+ch + 1} peak below the pair`);
    }
    return bad;
  });
  check(!clipped.length, `short window, Twin X with banner, bar and note: no strip row squashed or cut (${clipped.join(', ') || 'none'})`);
  await short.screenshot({ path: path.join(out, 'ui-control-short.png') });
  await short.close();

  /* ---------------- F. [polish Nov 15] A linked pair is one control, written to its first input ---------------- */
  // The Twin X ignores writes to input 2 while 1-2 are linked and runs both from input 1 (its readback then
  // shows input 1's settings on both). A change on either input goes to input 1 first, then the same value to
  // input 2 (so it holds it after an unlink, as UA does); both inputs show it.
  const ctx3 = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx3.addInitScript(stub);
  const p3 = await ctx3.newPage();
  watch(p3);
  await p3.goto(ui);
  const host3 = m => p3.evaluate(x => onHostMessage(x), m);
  const mark3 = () => p3.evaluate(() => window.__sent.length);
  const sent3 = k => p3.evaluate(i => window.__sent.slice(i).filter(s => /^input\t/.test(s)), k);
  const tw3 = () => p3.evaluate(() => { const c = S.ctl[1]; return { inputs: c.inputs.slice(0, 2).map(x => ({ ...x })), links: [...c.links], pending: [...c.pending.keys()] }; });
  /* the unit's readback: input 1's flags f and GainC gc on both inputs while linked, each its own when not */
  const twPair = (link, f1, gc1, f2 = f1, gc2 = gc1) => words(w => {
    w[0] = (link ? 0x104 : 0) | f1 | (f2 << 6); w[3] = gc1 | (gc2 << 6); w[2] = 150 | (140 << 8); w[6] = (1 << 30) | (1 << 31);
  });
  await host3({ type: 'hello', app: 'test', config: null, demo: false, view: 'control:twinx' });
  await host3({ type: 'status', present: true, info: INFO });
  await host3({ type: 'units', units: [X8P, TWX] });
  await host3({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host3({ type: 'readback', unit: 1, valid: true, seq: 1, words: twPair(true, 0, 21) });
  let f3 = await tw3();
  const tws3 = p3.locator('.useg[data-unit="1"]');
  check(await p3.evaluate(() => S.sel) === 1 && f3.links[0] && f3.inputs[0].gain === 30 && f3.inputs[1].gain === 30 &&
    (await tws3.locator('.pair').first().getAttribute('class')).includes('linked'), 'linked Twin X 1-2 from its readback, both at 30 dB');

  let k3 = await mark3();
  await tws3.locator('.strip[data-ch="1"]').getByRole('button', { name: 'LO CUT' }).click();
  await p3.waitForTimeout(100);
  let s3 = await sent3(k3);
  check(s3.length === 2 && s3[0] === 'input\t0\tlowcut\t1\t1' && s3[1] === 'input\t1\tlowcut\t1\t1',
    `linked: LO CUT on input 2 goes to input 1 first, then input 2 (${JSON.stringify(s3)})`);
  f3 = await tw3();
  check(f3.inputs[0].lowcut && f3.inputs[1].lowcut && f3.pending.includes('in:0:lowcut') && f3.pending.includes('in:1:lowcut') &&
    await pressed(tws3.locator('.strip[data-ch="0"]').getByRole('button', { name: 'LO CUT' })) &&
    await pressed(tws3.locator('.strip[data-ch="1"]').getByRole('button', { name: 'LO CUT' })), 'linked: both inputs show LO CUT on, both wait for readback');
  await host3({ type: 'readback', unit: 1, valid: true, seq: 2, words: twPair(true, 1 << 4, 21) });
  f3 = await tw3();
  check(f3.inputs[0].lowcut && f3.inputs[1].lowcut && !f3.pending.length, 'linked: the unit\'s readback (input 1\'s low cut on both) confirms both');

  k3 = await mark3();
  await p3.getByRole('slider', { name: 'Twin X input 2 gain', exact: true }).focus();
  await p3.keyboard.press('PageUp');
  await p3.waitForTimeout(100);
  s3 = await sent3(k3);
  check(s3.length === 2 && s3[0] === 'input\t0\tgain\t36\t1' && s3[1] === 'input\t1\tgain\t36\t1' &&
    await tws3.locator('.strip[data-ch="0"] .st-pre .k-readout').textContent() === '36 dB' &&
    await tws3.locator('.strip[data-ch="1"] .st-pre .k-readout').textContent() === '36 dB', `linked: input 2's gain knob drives input 1 first, both show 36 dB (${JSON.stringify(s3)})`);
  await host3({ type: 'readback', unit: 1, valid: true, seq: 3, words: twPair(true, 1 << 4, 27) });

  k3 = await mark3();
  await tws3.locator('.strip[data-ch="1"]').getByRole('button', { name: '48V' }).click();
  check(/Turn on 48V for Twin X input 1, Twin X input 2\?/.test(await p3.locator('#dlg h3').textContent()), 'linked: 48V on input 2 asks for both inputs');
  await p3.getByRole('button', { name: 'Turn on 48V' }).click();
  await p3.waitForTimeout(100);
  s3 = await sent3(k3);
  check(s3.length === 2 && s3[0] === 'input\t0\t48v\t1\t1' && s3[1] === 'input\t1\t48v\t1\t1', `linked: 48V to input 1 first, then input 2 (${JSON.stringify(s3)})`);
  await host3({ type: 'readback', unit: 1, valid: true, seq: 4, words: twPair(true, (1 << 4) | (1 << 3), 27) });
  f3 = await tw3();
  check(f3.inputs[0].p48 && f3.inputs[1].p48 && !f3.pending.length, 'linked: 48V confirmed on both');

  // A preset that links 1-2: both inputs take its input 1 values; input 2's own values in the preset are not sent.
  k3 = await mark3();
  await p3.evaluate(() => recallPreset({ name: 'Linked', saved: 1, state: { links: [true], inputs: [
    { gain: 40, line: false, pad: true, lowcut: true, phase: false, p48: true },
    { gain: 55, line: true, pad: false, lowcut: false, phase: true, p48: false }] } }));
  await p3.waitForTimeout(100);
  s3 = await sent3(k3);
  f3 = await tw3();
  check(s3.join() === 'input\t0\tpad\t1\t1,input\t0\tgain\t40\t1,input\t1\tpad\t1\t1,input\t1\tgain\t40\t1' &&
    f3.inputs[1].gain === 40 && f3.inputs[1].pad && !f3.inputs[1].line && !f3.inputs[1].phase && f3.inputs[1].p48,
  `linked preset: input 1's values on both, input 1 first, nothing of input 2's own (${JSON.stringify(s3)})`);
  await host3({ type: 'readback', unit: 1, valid: true, seq: 5, words: twPair(true, (1 << 1) | (1 << 3) | (1 << 4), 31) });

  // Unlinked, each input is its own: input 2 alone.
  await host3({ type: 'readback', unit: 1, valid: true, seq: 6, words: twPair(false, (1 << 1) | (1 << 3) | (1 << 4), 31) });
  k3 = await mark3();
  await tws3.locator('.strip[data-ch="1"]').getByRole('button', { name: 'LO CUT' }).click();
  await p3.waitForTimeout(100);
  s3 = await sent3(k3);
  f3 = await tw3();
  check(!f3.links[0] && s3.length === 1 && s3[0] === 'input\t1\tlowcut\t0\t1' && f3.inputs[0].lowcut && !f3.inputs[1].lowcut,
    `unlinked: LO CUT on input 2 goes to input 2 only (${JSON.stringify(s3)})`);
  await ctx3.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
