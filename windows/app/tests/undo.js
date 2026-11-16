// [Undo] Undo / redo on the host path (app/ui/undo.js): a monitor-mix mute and an output setting go back with Ctrl+Z and
// forward with Ctrl+Y, the sends say so, the front panel's level (readback) never becomes a step, and the buttons follow.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: 'SN22532017106180', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const words = lvl => { const w = new Array(40).fill(0); w[2] = lvl; w[6] = 0xC0000020; w[38] = 0x00010203; return w.map(x => x >>> 0); };
const wait = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx.addInitScript(() => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  });
  const page = await ctx.newPage();
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const sent = n => page.evaluate(k => window.__sent.slice(k).filter(s => /^monitor\t/.test(s)), n);
  const mark = () => page.evaluate(() => window.__sent.length);

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P] });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: words(140) });
  await wait(1300);                                    /* the baseline follows the unit once a second */

  const undoBtn = page.getByRole('button', { name: 'Undo', exact: true }), redoBtn = page.getByRole('button', { name: 'Redo', exact: true });
  check(await undoBtn.isDisabled(), 'nothing to undo at the start');

  /* the front panel turns the monitor level: not a step */
  await host({ type: 'readback', unit: 0, valid: true, seq: 2, words: words(120) });
  await wait(1300);
  check(await undoBtn.isDisabled(), 'a readback change (the unit\'s own knob) is not an undo step');

  /* a change made here: input 1 muted in the monitor mix */
  await page.evaluate(() => setMix(0, 'mute', true, 0));
  await wait(900);
  check(!(await undoBtn.isDisabled()), 'a mute made here is an undo step');
  let n = await mark();
  await page.locator('body').click({ position: { x: 5, y: 5 } });
  await page.keyboard.press('Control+z');
  await wait(200);
  const s1 = await sent(n);
  check(s1.some(s => /^monitor\tmixmute:0\t0/.test(s)), `Ctrl+Z sends the mute back off (${s1.filter(s => /mixmute:0/.test(s)).join(', ') || 'none'})`);
  check(await page.evaluate(() => !mixOfBus(0, 0).mute), 'the strip shows it unmuted again');
  check(!(await redoBtn.isDisabled()), 'redo is offered');

  n = await mark();
  await page.keyboard.press('Control+y');
  await wait(200);
  const s2 = await sent(n);
  check(s2.some(s => /^monitor\tmixmute:0\t1/.test(s)), 'Ctrl+Y mutes it again');
  check(await page.evaluate(() => mixOfBus(0, 0).mute), 'the strip shows it muted');

  /* an output setting (outputs.js) undoes too */
  await wait(900);
  await page.evaluate(() => Outs.set('monref', true, 0));
  await wait(900);
  n = await mark();
  await page.keyboard.press('Control+z');
  await wait(200);
  check((await sent(n)).some(s => /^monitor\tmonref\t0/.test(s)), 'the monitor reference level goes back to +4 dBu');

  /* a new change clears redo */
  await page.evaluate(() => setMix(1, 'solo', true, 0));
  await wait(900);
  check(await redoBtn.isDisabled(), 'a new change clears redo');

  /* [Copy] input 1 copied onto input 3: preamp, monitor mix, cue send; 48V never switched on */
  await page.evaluate(() => { setInput(0, 'pad', true, { unit: 0 }); setMix(0, 'level', -12, 0); setSend(0, 0, 'level', -20, 0); });
  await wait(300);
  n = await mark();
  await page.evaluate(() => { ctl(0).inputs[0].p48 = true; ChCopy.copy(0, 0); ChCopy.paste(0, 2); });
  await wait(300);
  const all = await page.evaluate(k => window.__sent.slice(k), n);
  check(all.some(s => /^input\t2\tpad\t1/.test(s)), 'paste: input 3 PAD on, as input 1');
  check(all.some(s => /^monitor\tmix:2\t-12/.test(s)), 'paste: input 3 monitor-mix level -6 dB');
  check(all.some(s => /^monitor\tsend:1:2\t-20/.test(s)), 'paste: input 3 send to CUE 1');
  check(!all.some(s => /^input\t2\t48v\t1/.test(s)), 'paste never switches 48V on');

  check(errors.length === 0, `no page errors${errors.length ? ': ' + errors.join(' | ') : ''}`);
  await browser.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})();
