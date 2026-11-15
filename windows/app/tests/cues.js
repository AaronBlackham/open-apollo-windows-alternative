// [Monitor] The monitor features both units have (docs/console-parity.md "Monitor"; driver/monitor_plan.h):
// the Cues page (cue per model, sends and pans per input and the talkback, cue source / mono / mute, where
// a cue is heard, refusals inline, saved per unit and re-sent when the unit connects), ALT speakers on the
// Control view (per model, trim, the speaker set confirmed by readback word 6 [29:28]), the monitor source per
// model, talkback dimming the monitor, the demo, and 980 px.
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
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 2, playChannels: 46,
  recChannels: 34, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };
const words = f => { const w = new Array(40).fill(0); f(w); w[38] = 0x00010203; return w.map(x => x >>> 0); };
const x8pRb = (w6 = 0xC0000020) => words(w => { w[2] = 140; w[6] = w6; });
const twRb = (extra = () => {}) => words(w => { w[2] = 150 | (140 << 8); w[6] = 0xC0000011; extra(w); });

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
  const sentSince = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^monitor\t/.test(s)), n);
  const cueStrips = page.locator('#cues .cue-strip');
  const cueBtn = name => page.getByRole('group', { name: 'Cue mix' }).getByRole('button', { name, exact: true });
  const pressed = async loc => (await loc.getAttribute('aria-pressed')) === 'true';
  const notes = () => page.locator('#cueBar .ctl-note').allTextContents().then(t => t.join(' | '));
  const toasts = () => page.locator('.toast').allTextContents().then(t => t.join(' | '));
  const mix = id => page.evaluate(k => JSON.parse(JSON.stringify(cfg.mixes[k] || null)), id);

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: x8pRb() });
  await host({ type: 'readback', unit: 1, valid: true, seq: 1, words: twRb() });
  /* [Ins] each aux's return and mute go out once per connection (docs 1.9.6: the unit's own default leaves them silent) */
  const upSent = await sentSince(0);
  check(upSent.filter(x => !/^monitor\taux(ret|mute):[12]\t/.test(x)).length === 0 && upSent.filter(x => /^monitor\tauxret:1\t0(\t1)?$/.test(x)).length === 2,
    `nothing saved yet: nothing re-sent when the units come up but each aux's return and mute (${JSON.stringify(upSent)})`);

  // 1. The Cues tab: after Status (Ctrl+2 stays Status), Ctrl+3.
  check((await page.locator('.tabs button').allTextContents()).slice(0, 3).join() === 'Control,Status,Cues', 'tabs: Control, Status, Cues');
  await page.locator('body').click({ position: { x: 600, y: 30 } });
  await page.keyboard.press('Control+3');
  check(await page.evaluate(() => S.view) === 'cues' && await page.locator('#view-cues').isVisible(), 'Ctrl+3 opens Cues');

  // 2. The x8p: CUE 1-4, inputs 1-8 and the talkback.
  check((await page.getByRole('group', { name: 'Cue mix' }).getByRole('button').allTextContents()).join() === 'CUE 1,CUE 2,CUE 3,CUE 4',
    'x8p: CUE 1-4');
  check(await cueStrips.count() === 9 && /TB/.test(await cueStrips.nth(8).locator('.strip-num').textContent()) &&
    await page.getByRole('slider', { name: 'Talkback send' }).isVisible(), 'x8p: nine sources, inputs 1-8 and the talkback');
  check(await page.locator('#cueBar .ctl-units button').count() === 2, 'the unit selector on the Cues page too');
  check(/Heard on\s*HP 1/.test(await page.locator('.cue-heard-wrap').textContent()), 'CUE 1 heard on HP 1 (HP 1 source CUE 1)');

  // 3. Sends and pans go out as monitor params send:<cue>:<bus> / pan:<cue>:<bus>, untagged for slot 0.
  let n = await mark();
  await page.getByRole('slider', { name: 'Input 2 send' }).locator('.k-readout').dblclick();
  await page.keyboard.type('-6');
  await page.keyboard.press('Enter');
  await page.getByRole('slider', { name: 'Input 2 pan' }).focus();
  await page.keyboard.press('PageDown');
  await page.waitForTimeout(120);
  let s = await sentSince(n);
  check(s.includes('monitor\tsend:1:1\t-12') && s.includes('monitor\tpan:1:1\t-25'), `input 2 into CUE 1 at -6 dB, pan L25 (${JSON.stringify(s)})`);
  check(await page.getByRole('slider', { name: 'Input 2 send' }).getAttribute('aria-valuetext') === '-6.0 dB' &&
    await page.getByRole('slider', { name: 'Input 2 pan' }).getAttribute('aria-valuetext') === 'L25', 'the knobs show -6.0 dB and L25');
  await cueBtn('CUE 4').click();
  n = await mark();
  await page.evaluate(() => setSend(3, 10, 'level', -20));
  await page.waitForTimeout(80);
  check((await sentSince(n)).includes('monitor\tsend:4:10\t-20'), 'the talkback into CUE 4 (bus 10)');
  check(await page.getByRole('slider', { name: 'Input 2 send' }).getAttribute('aria-valuetext') === 'off', 'CUE 4 has its own sends (input 2 off)');
  check(/Headphones take CUE 1 or CUE 2 here/.test(await page.locator('.cue-notes').textContent()) &&
    /nowhere yet/.test(await page.locator('.cue-heard').textContent()), 'x8p CUE 4: heard nowhere until it is the monitor source; the note says so');

  // 4. Cue source, mono, mute.
  n = await mark();
  await page.getByRole('group', { name: 'Cue source' }).getByRole('button', { name: 'MIX' }).click();
  await page.locator('.cue-card').getByRole('button', { name: 'MONO' }).click();
  await page.locator('.cue-card').getByRole('button', { name: 'MUTE' }).click();
  s = await sentSince(n);
  check(s.includes('monitor\tcue4mix\t1') && s.includes('monitor\tcue4mono\t1') && s.includes('monitor\tcue4mute\t1'),
    `CUE 4: source MIX, mono, mute (${JSON.stringify(s)})`);
  check(await pressed(page.locator('.cue-card').getByRole('button', { name: 'MUTE' })) &&
    (await page.locator('#cues .cue-strips').getAttribute('class')).includes('muted'), 'muted cue shown on its strips');
  const m0 = await mix('x8p');
  check(m0 && m0.cues[0].sends[1].level === -12 && m0.cues[0].sends[1].pan === -25 && m0.cues[3].sends[10].level === -20 &&
    m0.cues[3].mix === 1 && m0.cues[3].mono === true && m0.cues[3].mute === true, 'kept in cfg.mixes.x8p');
  await page.waitForTimeout(500);
  const saved = await page.evaluate(() => window.__sent.filter(x => x.startsWith('saveConfig\t')).pop() || '');
  check(JSON.parse(saved.split('\t')[1] || '{}').mixes.x8p.cues[3].mute === true, 'and saved with the settings');

  // 5. The Twin X: HP and LINE 3/4, inputs 1-2 and the talkback; sends carry the unit.
  await page.locator('#cueBar').getByRole('button', { name: 'Twin X', exact: true }).click();
  check((await page.getByRole('group', { name: 'Cue mix' }).getByRole('button').allTextContents()).join() === 'HP,LINE 3/4' &&
    await cueStrips.count() === 3, 'Twin X: cues HP and LINE 3/4, three sources');
  check(await page.evaluate(() => S.sel) === 1 &&
    await page.evaluate(() => document.querySelector('#monUnits button[data-unit="1"]').getAttribute('aria-pressed')) === 'true',
    'one selection for both pages');
  await cueBtn('LINE 3/4').click();
  check(/Heard on\s*Line 3\/4/.test(await page.locator('.cue-heard-wrap').textContent()), 'LINE 3/4 cue heard on Line 3/4');
  n = await mark();
  await page.evaluate(() => { setSend(1, 0, 'level', -12); setSend(1, 0, 'pan', 30); setCueFlag(1, 'mono', true); });
  await page.waitForTimeout(80);
  s = await sentSince(n);
  check(s.includes('monitor\tsend:2:0\t-12\t1') && s.includes('monitor\tpan:2:0\t30\t1') && s.includes('monitor\tcue2mono\t1\t1'),
    `Twin X: LINE 3/4 cue send, pan, mono, tagged unit 1 (${JSON.stringify(s)})`);
  check((await mix(TWX.id)).cues[1].sends[0].level === -12, 'kept per unit id');
  n = await mark();
  await page.evaluate(() => { setSend(2, 0, 'level', 0); setSend(0, 5, 'level', 0); });
  check((await sentSince(n)).length === 0, 'Twin X: no CUE 3, no input 6: nothing sent');

  // 6. A refusal: inline under the Cues page's selector, the knob marked and put back.
  await host({ type: 'error', op: 'monitor', key: 'send:2:0', index: 0x1100, unit: 1, code: 50, detail: 'The request is not supported' });
  let nt = await notes();
  check(/Twin X input 1 send to LINE 3\/4 was not changed: not supported by the installed driver on the Apollo Twin X \(Gen 1\)\./.test(nt),
    `inline note on the Cues page: "${nt}"`);
  check(!/send to LINE/.test(await toasts()), 'no toast for it');
  check(await page.getByRole('slider', { name: 'Input 1 send' }).getAttribute('aria-valuetext') === 'off' &&
    (await page.getByRole('slider', { name: 'Input 1 send' }).getAttribute('class')).includes('refused'), 'the send back to off, marked');
  check(!((await mix(TWX.id)).cues[1].sends[0] || {}).level || (await mix(TWX.id)).cues[1].sends[0].level === -121, 'and the saved mix too');
  await page.locator('#cueBar').getByRole('button', { name: 'Dismiss' }).click();
  check(await page.locator('#cueBar .ctl-notes').isHidden(), 'Dismiss clears it');
  await page.evaluate(() => setSend(1, 1, 'level', -20));
  await host({ type: 'error', op: 'monitor', key: 'send:2:1', index: 0x1101, unit: 1, code: 170, detail: 'The requested resource is in use' });
  check(await page.evaluate(() => sendOf(1, 1, 1).level) === -20 && /input 2 send to LINE 3\/4 was not changed: the driver is busy/.test(await toasts()) &&
    await page.locator('#cueBar .ctl-notes').isHidden(), 'a full ring (busy): the send stays as set for the next connection, a toast, no note');
  await page.screenshot({ path: path.join(out, 'ui-cues-twinx.png') });

  // 7. ALT speakers on the Control view: per model; the speaker set waits for readback word 6 [29:28].
  await page.keyboard.press('Control+1');
  check(await page.locator('.mon-alt .alt-row').count() === 1 && await page.locator('.mon-alt .alt-out').textContent() === 'Line 3/4',
    'Twin X: one ALT, on Line 3/4');
  check(await page.getByRole('group', { name: 'Speakers' }).isHidden(), 'no ALT on: no speaker choice');
  n = await mark();
  await page.locator('.mon-alt').getByRole('button', { name: 'ALT 1' }).click();
  check((await sentSince(n)).includes('monitor\talt1\t1\t1') &&
    (await page.getByRole('group', { name: 'Speakers' }).getByRole('button').allTextContents()).join() === 'MAIN,ALT 1',
    'ALT 1 on: alt1 sent, speakers MAIN / ALT 1');
  n = await mark();
  await page.getByRole('slider', { name: 'ALT 1 trim' }).focus();
  await page.keyboard.press('PageDown');
  await page.getByRole('group', { name: 'Speakers' }).getByRole('button', { name: 'ALT 1' }).click();
  await page.waitForTimeout(80);
  s = await sentSince(n);
  check(s.includes('monitor\talt1trim\t48\t1') && s.includes('monitor\taltsel\t1\t1'), `ALT 1 trim -6 dB and ALT 1 selected (${JSON.stringify(s)})`);
  check(await page.getByRole('slider', { name: 'ALT 1 trim' }).getAttribute('aria-valuetext') === '-6.0 dB', 'trim shows -6.0 dB');
  check(await page.evaluate(() => S.ctl[1].pending.has('mon:alt')), 'the speaker set waits for the readback');
  await host({ type: 'readback', unit: 1, valid: true, seq: 2, words: twRb(w => { w[6] = 0xD0000011; }) });
  check(!(await page.evaluate(() => S.ctl[1].pending.has('mon:alt'))) && await page.evaluate(() => S.ctl[1].mon.alt) === 1,
    'readback word 6 [29:28] = 1 confirms ALT 1');
  await page.evaluate(() => setMon('altsel', 2, 1));
  check(!(await page.evaluate(() => window.__sent.some(x => x.startsWith('monitor\taltsel\t2')))), 'Twin X: no ALT 2 to select');

  // 8. The monitor source per model; on the x8p CUE 3/4 and both HP sources.
  check((await page.getByRole('group', { name: 'Monitor source' }).getByRole('button').allTextContents()).join() === 'MIX,HP,LINE 3/4',
    'Twin X source: MIX, HP, LINE 3/4');
  await page.locator('#monUnits').getByRole('button', { name: 'x8p', exact: true }).click();
  check((await page.getByRole('group', { name: 'Monitor source' }).getByRole('button').allTextContents()).join() === 'MIX,CUE 1,CUE 2,CUE 3,CUE 4' &&
    await page.locator('.mon-alt .alt-row').count() === 2, 'x8p: source MIX and CUE 1-4, two ALT rows');
  n = await mark();
  await page.getByRole('group', { name: 'Monitor source' }).getByRole('button', { name: 'CUE 3' }).click();
  await page.locator('.mon-alt').getByRole('button', { name: 'ALT 2' }).click();
  await page.getByRole('group', { name: 'Speakers' }).getByRole('button', { name: 'ALT 2' }).click();
  s = await sentSince(n);
  check(s.includes('monitor\tsource\t3') && s.includes('monitor\talt2\t1') && s.includes('monitor\taltsel\t2'),
    `x8p: source CUE 3, ALT 2 on and selected (${JSON.stringify(s)})`);
  await page.waitForTimeout(1700);
  await host({ type: 'readback', unit: 0, valid: true, seq: 3, words: x8pRb() });
  check(/Monitor speakers: sent ALT 2, the unit reports MAIN/.test(await toasts()) && await page.evaluate(() => S.ctl[0].mon.alt) === 0,
    'a speaker set the unit did not take: back to MAIN, with a toast');
  n = await mark();
  await page.locator('.mon-alt').getByRole('button', { name: 'ALT 2' }).click();
  check((await sentSince(n)).includes('monitor\talt2\t0') && await page.getByRole('group', { name: 'Speakers' }).isHidden(),
    'ALT 2 off: no ALT left, no speaker choice');

  // 9. Talkback dims the monitor (the unit's dim level), and undoes it when talkback ends.
  await page.locator('#monUnits').getByRole('button', { name: 'Twin X', exact: true }).click();
  n = await mark();
  await page.getByRole('button', { name: 'TALKBACK' }).focus();
  await page.keyboard.press('Enter');
  s = await sentSince(n);
  check(s.includes('monitor\ttalkback\t1\t1') && s.includes('monitor\tdim\t1\t1'), `talkback on dims the monitor (${JSON.stringify(s)})`);
  n = await mark();
  await page.keyboard.press('Enter');
  s = await sentSince(n);
  check(s.includes('monitor\ttalkback\t0\t1') && s.includes('monitor\tdim\t0\t1'), 'talkback off: the dim goes again');
  await host({ type: 'readback', unit: 1, valid: true, seq: 4, words: twRb(w => { w[6] = 0xD0000011; }) });
  n = await mark();
  await host({ type: 'readback', unit: 1, valid: true, seq: 5, words: twRb(w => { w[6] = 0xD0000111; }) });
  s = await sentSince(n);
  check(s.includes('monitor\tdim\t1\t1'), 'the unit\'s own TALK button (readback word 6 bit 8) dims it too');
  await page.locator('.mon-card .check input').uncheck();
  await host({ type: 'readback', unit: 1, valid: true, seq: 6, words: twRb(w => { w[6] = 0xD0000011; w[2] |= 1 << 31; }) });
  n = await mark();
  await page.getByRole('button', { name: 'TALKBACK' }).focus();
  await page.keyboard.press('Enter');
  s = await sentSince(n);
  check(s.includes('monitor\ttalkback\t1\t1') && !s.some(x => /^monitor\tdim/.test(x)) && (await page.evaluate(() => cfg.talkbackDim)) === false,
    'option off: talkback leaves the monitor alone');
  const lastCfg = await page.evaluate(() => JSON.stringify(cfg));
  await page.screenshot({ path: path.join(out, 'ui-control-alt.png') });
  await ctx.close();

  /* ---------------- B. Restart: the saved mixes go out when each unit connects ---------------- */
  const ctx2 = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx2.addInitScript(stub);
  const p2 = await ctx2.newPage();
  watch(p2);
  await p2.goto(ui);
  const host2 = m => p2.evaluate(x => onHostMessage(x), m);
  const sent2 = () => p2.evaluate(() => window.__sent.filter(s => /^monitor\t/.test(s)));
  await host2({ type: 'hello', app: 'test', config: lastCfg, demo: false, view: 'cues' });
  await host2({ type: 'status', present: true, info: { ...INFO, stage: 3, flags: 0x10 } });
  check((await sent2()).length === 0, 'not connected yet: nothing sent');
  await host2({ type: 'status', present: true, info: INFO });
  let r = await sent2();
  const at = k => r.indexOf(k);
  check(at('monitor\tcue4mute\t1') >= 0 && at('monitor\tcue4mute\t1') < at('monitor\tsend:4:10\t-20') &&
    r.includes('monitor\tsend:1:1\t-12') && r.includes('monitor\tpan:1:1\t-25') && r.includes('monitor\tcue4mix\t1') &&
    r.includes('monitor\talt2\t0'), `x8p connects: its saved mix, mute first (${JSON.stringify(r)})`);
  const before = r.length;
  await host2({ type: 'status', present: true, info: INFO });
  await host2({ type: 'readback', unit: 0, valid: true, seq: 1, words: x8pRb() });
  check((await sent2()).length === before, 'once per connection');
  await host2({ type: 'status', unit: 1, present: true, info: TW_INFO });
  check((await sent2()).length === before, 'the Twin X before its id is listed: waits');
  await host2({ type: 'units', units: [X8P, TWX] });
  r = (await sent2()).slice(before);
  check(r.includes('monitor\talt1\t1\t1') && r.includes('monitor\talt1trim\t48\t1') && r.includes('monitor\tcue2mono\t1\t1') &&
    r.includes('monitor\tpan:2:0\t30\t1') && r.includes('monitor\tsend:2:0\t-121\t1') && !r.includes('monitor\tsend:2:0\t-12\t1'),
    `Twin X once listed: ALT 1, its trim, LINE 3/4 mono, input 1's pan with the send off (the refused -6 dB is not kept) (${JSON.stringify(r)})`);
  await p2.locator('#cueBar').getByRole('button', { name: 'x8p', exact: true }).click();
  check(await p2.locator('#view-cues').isVisible() && await p2.getByRole('slider', { name: 'Input 2 send' }).getAttribute('aria-valuetext') === '-6.0 dB' &&
    await p2.getByRole('slider', { name: 'Input 2 pan' }).getAttribute('aria-valuetext') === 'L25', 'the Cues page shows the saved sends after a restart');
  await host2({ type: 'status', present: true, info: { ...INFO, stage: 0x200, flags: 0 } });
  await host2({ type: 'status', present: true, info: INFO });
  check((await sent2()).length > before + r.length, 'and again after the unit comes back');
  await ctx2.close();

  /* ---------------- C. Demo, and 980 px ---------------- */
  const demo = await browser.newPage({ viewport: { width: 980, height: 760 } });
  watch(demo);
  await demo.goto(ui + '?view=cues');
  await demo.waitForTimeout(700);
  check(await demo.locator('#cues .cue-strip').count() === 9, 'demo: the x8p\'s Cues page');
  await demo.evaluate(() => { setSend(0, 0, 'level', -6); setAlt(0, 'on', true); setMon('altsel', 1); });
  await demo.waitForTimeout(300);
  const d = await demo.evaluate(() => ({ lv: sendOf(0, 0, 0).level, alt: S.ctl[0].mon.alt, p: [...S.ctl[0].pending.keys()],
    saved: !!(cfg.mixes.x8p && cfg.mixes.x8p.cues[0].sends && cfg.mixes.x8p.cues[0].sends[0]) }));
  check(d.lv === -6 && d.alt === 1 && !d.p.includes('mon:alt') && !d.saved, `demo: send kept in memory, ALT 1 confirmed by the simulated readback (${JSON.stringify(d)})`);
  const overflow = await demo.evaluate(() => {
    const bad = [];
    if (document.documentElement.scrollWidth > document.documentElement.clientWidth) bad.push('document');
    const card = document.querySelector('.cue-card').getBoundingClientRect();
    if (card.right > document.documentElement.clientWidth) bad.push('cue card');
    return bad;
  });
  check(!overflow.length, `980 px: no horizontal overflow, the cue card in view (${overflow.join(', ') || 'none'})`);
  await demo.screenshot({ path: path.join(out, 'ui-cues-980.png') });
  await demo.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
