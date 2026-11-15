// [Mix] The monitor mix on the Control view (docs/console-parity.md "Mixer"; driver/monitor_plan.h APOLLO_MON_MIX_*):
// a console fader with pan, M and S on every strip of every unit's segment and a talkback strip, MUTE ALL in each
// segment's master section, a linked pair as one stereo channel (UA: the first input's level, mute and solo on both
// buses, pans hard left / right on link), refusals inline and put back, saved per unit and re-sent on connect with
// mutes and solos first, nothing sent for an untouched input, the names kept apart from the monitor's MUTE and
// TALKBACK buttons, faders by mouse and keyboard, the main out L/R meters on the loopback, and 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'C:/Users/aaron/Desktop/Avium/Avium Invoid/node_modules/playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: 'SN22532017106180', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 2, playChannels: 46,
  recChannels: 34, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };
const words = f => { const w = new Array(40).fill(0); f(w); w[38] = 0x00010203; return w.map(x => x >>> 0); };
const x8pRb = () => words(w => { w[2] = 140; w[6] = 0xC0000020; });
/* the Twin X's readback; link: the pair 1-2 link bits (0x104 in word 0, as the unit reports a linked pair) */
const twRb = (link = false) => words(w => { w[0] = link ? 0x104 : 0; w[2] = 150 | (140 << 8); w[6] = 0xC0000011; });

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
  const sentSince = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^(monitor|input)\t/.test(s)), n);
  const mixSent = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^monitor\tmix/.test(s)), n);
  const card = page.locator('.useg[data-unit="0"]');
  const rows = card.locator('.strip, .tb-strip');
  const twin = page.locator('.useg[data-unit="1"]');
  const twRows = twin.locator('.strip, .tb-strip');
  const sw = name => page.getByRole('switch', { name, exact: true });
  const checked = async loc => (await loc.getAttribute('aria-checked')) === 'true';
  const notes = () => page.locator('#ctlBar .ctl-note').allTextContents().then(t => t.join(' | '));
  const toasts = () => page.locator('.toast').allTextContents().then(t => t.join(' | '));
  const saved = id => page.evaluate(k => JSON.parse(JSON.stringify((cfg.mixes[k] || {}).main || null)), id);
  const editKnob = async (name, text) => {
    await page.getByRole('slider', { name, exact: true }).focus();
    await page.keyboard.press('Enter');                  /* knobs and faders: Enter types a value */
    await page.keyboard.type(text);
    await page.keyboard.press('Enter');
  };

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: x8pRb() });
  await host({ type: 'readback', unit: 1, valid: true, seq: 1, words: twRb() });

  // 1. The faders: one per x8p strip and the talkback, UA's default shown, nothing sent for an untouched input.
  const faders = await card.getByRole('slider', { name: / mix level$/ }).evaluateAll(f => f.map(x => x.getAttribute('aria-label')));
  check(await card.isVisible() && await page.locator('.mon-mix').count() === 0 && faders.length === 9, `Control view: a fader on every x8p strip and the talkback, no card (${faders.length})`);
  check(await rows.count() === 9 && faders.join() === [1, 2, 3, 4, 5, 6, 7, 8].map(i => `Input ${i} mix level`).concat('Talkback mix level').join(),
    'x8p: channels 1-8 and TB');
  check(await page.getByRole('slider', { name: 'Input 1 mix level', exact: true }).getAttribute('aria-valuetext') === '0.0 dB' &&
    await page.getByRole('slider', { name: 'Input 1 mix pan', exact: true }).getAttribute('aria-valuetext') === 'C' &&
    !(await checked(sw('Input 1 mute in the monitor mix'))), 'an untouched input shows UA\'s default: 0.0 dB, centre, not muted');
  check((await mixSent(0)).length === 0, 'nothing saved: nothing sent when the units come up');
  check(await page.getByRole('slider', { name: 'Talkback mix level', exact: true }).isVisible() &&
    await page.getByRole('slider', { name: 'Talkback mix pan' }).count() === 0, 'the talkback row: a level, no pan (one weight on both sides)');
  // The names stay clear of the existing lookups: the monitor's MUTE and the TALKBACK button are still the only buttons so named.
  check(await page.getByRole('button', { name: 'MUTE', exact: true }).count() === 1 && await page.getByRole('button', { name: 'TALKBACK' }).count() === 1 &&
    await page.getByRole('slider', { name: 'Monitor level', exact: true }).count() === 1,
  'M, S and MUTE ALL are switches: one MUTE button (the Twin X\'s is "Twin X monitor mute"), one TALKBACK button, one Monitor level');

  // 2. Level, pan, M, S go out as mix:<bus>, mixpan:<bus>, mixmute:<bus>, mixsolo:<bus>, untagged for slot 0.
  let n = await mark();
  await editKnob('Input 2 mix level', '-6');
  await page.getByRole('slider', { name: 'Input 2 mix pan', exact: true }).focus();
  await page.keyboard.press('PageDown');
  await page.waitForTimeout(120);
  let s = await mixSent(n);
  check(s.includes('monitor\tmix:1\t-12') && s.includes('monitor\tmixpan:1\t-25'), `input 2 at -6 dB, pan L25 (${JSON.stringify(s)})`);
  check(await page.getByRole('slider', { name: 'Input 2 mix level', exact: true }).getAttribute('aria-valuetext') === '-6.0 dB' &&
    await page.getByRole('slider', { name: 'Input 2 mix pan', exact: true }).getAttribute('aria-valuetext') === 'L25', 'the row shows -6.0 dB and L25');
  n = await mark();
  await sw('Input 2 mute in the monitor mix').click();
  await sw('Input 3 solo in the monitor mix').click();
  s = await mixSent(n);
  check(s.join('|') === 'monitor\tmixmute:1\t1|monitor\tmixsolo:2\t1', `M on input 2, S on input 3 (${JSON.stringify(s)})`);
  const silent = await rows.evaluateAll(r => r.map(x => x.classList.contains('silent')));
  check(await checked(sw('Input 2 mute in the monitor mix')) && await checked(sw('Input 3 solo in the monitor mix')) &&
    silent.join() === 'true,true,false,true,true,true,true,true,true', `the solo silences every other row, the mute its own (${silent.join()})`);
  check(JSON.stringify(await saved('x8p')) === JSON.stringify({ 1: { level: -12, pan: -25, mute: true }, 2: { solo: true } }),
    `kept in cfg.mixes.x8p.main, only what was touched (${JSON.stringify(await saved('x8p'))})`);
  n = await mark();
  await sw('Input 3 solo in the monitor mix').click();
  check((await mixSent(n)).join() === 'monitor\tmixsolo:2\t0' && !(await rows.nth(0).getAttribute('class')).includes('silent'), 'S off: the others come back');

  // 3. MUTE ALL: one driver call, every row follows; again: all unmuted.
  n = await mark();
  await sw('Mute every input and the talkback in the monitor mix').click();
  s = await mixSent(n);
  const allM = await card.getByRole('switch', { name: /mute in the monitor mix$/ }).evaluateAll(b => b.map(x => x.getAttribute('aria-checked')));
  check(s.join() === 'monitor\tmixmuteall\t1' && allM.every(v => v === 'true') && allM.length === 9 &&
    await checked(sw('Mute every input and the talkback in the monitor mix')), `MUTE ALL: mixmuteall 1, all nine rows muted (${s.join()})`);
  n = await mark();
  await sw('Mute every input and the talkback in the monitor mix').click();
  check((await mixSent(n)).join() === 'monitor\tmixmuteall\t0' && !(await checked(sw('Input 2 mute in the monitor mix'))) &&
    (await saved('x8p'))[5].mute === false, 'again: mixmuteall 0, every row unmuted (input 2\'s own mute too)');
  await page.screenshot({ path: path.join(out, 'ui-mix-x8p.png') });

  // 4. The Twin X: inputs 1-2 and the talkback; writes carry the unit.
  check(await twRows.count() === 3 && (await twin.getByRole('slider', { name: / mix level$/ }).count()) === 3, 'Twin X segment: channels 1, 2 and TB, beside the x8p');
  check(await page.getByRole('slider', { name: 'Twin X input 2 mix level', exact: true }).getAttribute('aria-valuetext') === '0.0 dB',
    'its own mix (the x8p\'s input 2 at -6 dB is not shown here)');
  n = await mark();
  await editKnob('Twin X input 1 mix level', '-12');
  await sw('Twin X input 1 mute in the monitor mix').click();
  await page.waitForTimeout(80);
  s = await mixSent(n);
  check(s.includes('monitor\tmix:0\t-24\t1') && s.includes('monitor\tmixmute:0\t1\t1'), `Twin X input 1: -12 dB and M, tagged unit 1 (${JSON.stringify(s)})`);
  check((await saved(TWX.id))[0].level === -24, 'kept per unit id');

  // 5. A refusal: inline under the selector, the switch put back and marked; no toast.
  await host({ type: 'error', op: 'monitor', key: 'mixmute:0', index: 0x4200, unit: 1, code: 50, detail: 'The request is not supported' });
  let nt = await notes();
  check(/Twin X input 1 monitor mix mute was not changed: not supported by the installed driver on the Apollo Twin X \(Gen 1\)\./.test(nt),
    `inline note: "${nt}"`);
  check(!/monitor mix mute/.test(await toasts()), 'no toast for it');
  check(!(await checked(sw('Twin X input 1 mute in the monitor mix'))) && (await sw('Twin X input 1 mute in the monitor mix').getAttribute('class')).includes('refused') &&
    (await saved(TWX.id))[0].mute === undefined, 'the M switch back off, marked; the saved mix too');
  await host({ type: 'error', op: 'monitor', key: 'mix:0', index: 0x4000, unit: 1, code: 170, detail: 'The requested resource is in use' });
  check((await saved(TWX.id))[0].level === -24 && /input 1 monitor mix level was not changed: the driver is busy/.test(await toasts()),
    'busy: the level stays as set for the next connection, a toast');
  await page.locator('#ctlBar').getByRole('button', { name: 'Dismiss' }).click();
  n = await mark();
  await sw('Mute every Twin X input and the talkback in the monitor mix').click();
  await host({ type: 'error', op: 'monitor', key: 'mixmuteall', index: 0x4400, unit: 1, code: 87, detail: 'The parameter is incorrect' });
  const twM = await twin.getByRole('switch', { name: /mute in the monitor mix$/ }).evaluateAll(b => b.map(x => x.getAttribute('aria-checked')));
  check((await mixSent(n)).includes('monitor\tmixmuteall\t1\t1') && twM.every(v => v === 'false') &&
    /Twin X monitor mix mute-all was not changed: the driver rejected the value/.test(await notes()), `a refused MUTE ALL: every row back (${twM.join()})`);
  await page.locator('#ctlBar').getByRole('button', { name: 'Dismiss' }).click();

  // 6. Link from the app: the mix follows UA (pans hard left / right, the first input's level and mute on both).
  n = await mark();
  await twin.locator('.pair').nth(0).getByRole('button', { name: 'LINK' }).click();
  await page.waitForTimeout(80);
  s = await sentSince(n);
  check(s.includes('input\t0\tlink\t1\t1') && s.includes('monitor\tmixpan:0\t-100\t1') && s.includes('monitor\tmixpan:1\t100\t1') &&
    s.includes('monitor\tmix:1\t-24\t1') && !s.some(x => /mixmute/.test(x)),
    `LINK 1-2: pans L100 / R100 and input 1's -12 dB on input 2 (${JSON.stringify(s.filter(x => /mix/.test(x)))})`);
  await host({ type: 'readback', unit: 1, valid: true, seq: 2, words: twRb(true) });
  const r2 = twRows.nth(1);
  check((await twin.locator('.pair').nth(0).getAttribute('class')).includes('linked') && (await r2.getAttribute('class')).includes('follower') &&
    await page.getByRole('slider', { name: 'Twin X input 2 mix level', exact: true }).count() === 0 &&
    await page.getByRole('slider', { name: 'Twin X input 2 mix pan', exact: true }).getAttribute('aria-valuetext') === 'R100' &&
    await sw('Twin X input 2 mute in the monitor mix').count() === 0, 'linked: input 1\'s fader, M and S drive the pair; input 2 keeps only its pan (R100)');
  n = await mark();
  await editKnob('Twin X input 1 mix level', '-3');
  await sw('Twin X input 1 solo in the monitor mix').click();
  await page.waitForTimeout(80);
  s = await mixSent(n);
  check(s.includes('monitor\tmix:0\t-6\t1') && s.includes('monitor\tmix:1\t-6\t1') && s.includes('monitor\tmixsolo:0\t1\t1') &&
    s.includes('monitor\tmixsolo:1\t1\t1'), `the pair's level and solo go to both buses (${JSON.stringify(s)})`);
  check(!(await r2.getAttribute('class')).includes('silent') && (await twRows.nth(2).getAttribute('class')).includes('silent'),
    'the pair soloed: both its rows audible, the talkback silenced');
  n = await mark();
  await page.getByRole('slider', { name: 'Twin X input 2 mix pan', exact: true }).focus();
  await page.keyboard.press('PageDown');
  await page.waitForTimeout(80);
  check((await mixSent(n)).join() === 'monitor\tmixpan:1\t75\t1', 'input 2\'s pan alone places input 2');
  n = await mark();
  await twin.locator('.pair').nth(0).getByRole('button', { name: 'LINK' }).click();
  s = await mixSent(n);
  check(s.includes('monitor\tmixpan:0\t0\t1') && s.includes('monitor\tmixpan:1\t0\t1'), 'unlink: both pans back to the centre');
  await host({ type: 'readback', unit: 1, valid: true, seq: 3, words: twRb(false) });
  check(!(await twRows.nth(1).getAttribute('class')).includes('follower') && await page.getByRole('slider', { name: 'Twin X input 2 mix level', exact: true }).count() === 1,
    'unlinked: two faders again');

  // 7. Faders: drag with the mouse (the cap follows), Ctrl for fine steps, double-click for 0 dB, keys.
  const f3 = page.getByRole('slider', { name: 'Input 3 mix level', exact: true });
  await card.locator('.strip[data-ch="2"] .fd-track').scrollIntoViewIfNeeded();
  const tr = await card.locator('.strip[data-ch="2"] .fd-track').boundingBox();
  n = await mark();
  await page.mouse.move(tr.x + tr.width / 2, tr.y + tr.height * 0.25);
  await page.mouse.down();
  await page.mouse.move(tr.x + tr.width / 2, tr.y + tr.height * 0.6, { steps: 4 });
  await page.mouse.up();
  await page.waitForTimeout(120);
  const dragged = Number(await f3.getAttribute('aria-valuenow'));
  s = await mixSent(n);
  check(dragged < 0 && dragged > -121 && s.some(x => x.startsWith('monitor\tmix:2\t')), `drag down: input 3's fader goes down and sends mix:2 (${dragged}, ${s.slice(-1)})`);
  await page.mouse.move(tr.x + tr.width / 2, tr.y + tr.height * 0.6);
  await page.keyboard.down('Control');
  await page.mouse.down();
  await page.mouse.move(tr.x + tr.width / 2, tr.y + tr.height * 0.4, { steps: 4 });
  await page.mouse.up();
  await page.keyboard.up('Control');
  const fine = Number(await f3.getAttribute('aria-valuenow'));
  check(fine > dragged && fine - dragged < 12, `Ctrl-drag: fine steps (${dragged} -> ${fine})`);
  await f3.dblclick();
  check(await f3.getAttribute('aria-valuetext') === '0.0 dB', 'double-click: 0 dB');
  n = await mark();
  await f3.focus();
  await page.keyboard.press('PageDown');
  await page.keyboard.press('ArrowUp');
  await page.waitForTimeout(150);
  check(await f3.getAttribute('aria-valuetext') === '-5.5 dB' && (await mixSent(n)).pop() === 'monitor\tmix:2\t-11', 'keys: Page Down -6 dB, arrow up +0.5 dB');
  await page.keyboard.press('Home');
  check(await f3.getAttribute('aria-valuetext') === 'off' && await card.locator('.strip[data-ch="2"] .fd-readout').textContent() === 'off', 'Home: off (-inf), the readout says so');
  const lastCfg = await page.evaluate(() => JSON.stringify(cfg));
  await page.screenshot({ path: path.join(out, 'ui-mix-twinx.png') });
  await ctx.close();

  /* ---------------- B. Restart: the saved mix goes out on connect, mutes and solos first ---------------- */
  const ctx2 = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx2.addInitScript(stub);
  const p2 = await ctx2.newPage();
  watch(p2);
  await p2.goto(ui);
  const host2 = m => p2.evaluate(x => onHostMessage(x), m);
  const sent2 = () => p2.evaluate(() => window.__sent.filter(s => /^monitor\t/.test(s)));
  const cfg2 = JSON.parse(lastCfg);
  cfg2.mixes.x8p.main = { 0: { mute: true }, 4: { level: -20, pan: 30 }, 10: { level: -12, solo: true } };
  await host2({ type: 'hello', app: 'test', config: JSON.stringify(cfg2), demo: false, view: 'control' });
  await host2({ type: 'status', present: true, info: { ...INFO, stage: 3, flags: 0x10 } });
  check((await sent2()).length === 0, 'not connected yet: nothing sent');
  await host2({ type: 'status', present: true, info: INFO });
  const r = (await sent2()).filter(x => /^monitor\tmix/.test(x));
  check(r.join('|') === 'monitor\tmixmute:0\t1|monitor\tmixsolo:10\t1|monitor\tmixpan:4\t30|monitor\tmix:4\t-20|monitor\tmix:10\t-12',
    `x8p connects: mutes, solos, then pans and levels, only the touched buses (${JSON.stringify(r)})`);
  const all2 = await sent2();
  check(all2.indexOf('monitor\tmixmute:0\t1') === 0, 'the monitor mix\'s mutes before anything else');
  await host2({ type: 'units', units: [X8P, TWX] });
  await host2({ type: 'status', unit: 1, present: true, info: TW_INFO });
  const rt = (await sent2()).slice(all2.length).filter(x => /^monitor\tmix/.test(x));
  check(rt.length > 0 && rt.every(x => /\t1$/.test(x)) && rt.includes('monitor\tmix:0\t-6\t1') && rt.includes('monitor\tmixpan:1\t0\t1') &&
    rt.includes('monitor\tmixsolo:0\t1\t1') && rt.indexOf('monitor\tmixsolo:0\t1\t1') < rt.indexOf('monitor\tmix:0\t-6\t1') &&
    !rt.some(x => /^monitor\tmixmute:/.test(x)), `Twin X once listed: its saved mix, tagged, solos first, the refused M not kept (${JSON.stringify(rt)})`);
  await ctx2.close();

  /* ---------------- C. Demo: the main out L/R meters are the monitor-mix loopback, input meters hold their peaks ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=control');
  await demo.waitForTimeout(900);
  const outTxt = () => demo.locator('.useg[data-unit="0"] .mo-peak').allTextContents();
  const loop = () => demo.evaluate(() => S.ctl[0].peaks[26]);
  check((await outTxt()).every(t => /^-\d+\.\d$/.test(t)) && await loop() > -60, `demo: main out L/R show the mix level (${(await outTxt()).join(' / ')})`);
  const held = await demo.evaluate(() => { const m = segs.get(0).strips[0].meter; return [m.hold, m.cur]; });
  check(held[0] >= held[1] && held[0] > -60, `demo: input 1's meter holds its peak (${held.map(x => x.toFixed(1)).join(' >= ')})`);
  await demo.getByRole('switch', { name: 'Mute every input and the talkback in the monitor mix', exact: true }).click();
  await demo.waitForTimeout(400);
  check(await loop() === -Infinity, 'demo: MUTE ALL silences the loopback the main out meters show');
  await demo.getByRole('switch', { name: 'Mute every input and the talkback in the monitor mix', exact: true }).click();
  await demo.waitForTimeout(400);
  check(await loop() > -60 && await demo.evaluate(() => !(cfg.mixes.x8p && cfg.mixes.x8p.main && cfg.mixes.x8p.main[0])),
    'demo: back; kept in memory only');
  await demo.close();

  /* ---------------- D. 980 px: the strips scroll inside their area, the columns stay whole ---------------- */
  const narrow = await browser.newPage({ viewport: { width: 980, height: 760 } });
  watch(narrow);
  await narrow.goto(ui + '?view=control');
  await narrow.waitForTimeout(700);
  const fits = () => narrow.evaluate(() => {
    const bad = [], m = document.querySelector('#monitor');
    if (document.documentElement.scrollWidth > document.documentElement.clientWidth) bad.push('document');
    if (m.scrollWidth > m.clientWidth + 1) bad.push('#monitor');
    for (const n of document.querySelectorAll('.master > *')) if (n.scrollWidth > n.clientWidth + 1) bad.push(n.className);
    return bad;
  });
  let bad = await fits();
  check(!bad.length, `980 px, x8p focused: nothing past an edge but the strip scroller (${bad.join(', ') || 'none'})`);
  await narrow.evaluate(() => selectUnit(1));
  await narrow.waitForTimeout(200);
  bad = await fits();
  check(!bad.length && await narrow.locator('.useg[data-unit="1"] .tb-strip').count() === 1, `980 px, Twin X focused: the same (${bad.join(', ') || 'none'})`);
  await narrow.screenshot({ path: path.join(out, 'ui-mix-980.png') });
  await narrow.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
