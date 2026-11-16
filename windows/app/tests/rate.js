// [Rate] The sample-rate selectors through a stub host (app/src/rate_host.h, docs/apollo-control-units.md 1.6): rate-get on
// connect, disabled at the unit's rate when the host does not answer, only the offered rates (not allowed: disabled), a
// change sent with the unit's id, pending then the host's own words on a refusal, one shared selector with combined:true
// while the units run combined, plug-ins waiting away from 48 kHz, and the Device view's panel.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: '000000003A00C0DE', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '20000000000001', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '000000002F00BEEF', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0, extCaps: 0, fwRelease: 0x0B0FD613, subsystemId: 0x14, deviceType: 0x22, publicType: 0x16,
  numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10, interruptCount: 0, notifStatusSeen: 0,
  numPreamps: 8, serial: X8P.serial, stage: 5, flags: 0x40 | 0x80 | 0x100 | 0x200 | 0x400, mode: 2, observe: false };
const TW_INFO = { ...INFO, deviceType: 0x99, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };
const SIX = [44100, 48000, 88200, 96000, 176400, 192000];
const rate = (id, over = {}, comb = {}) => ({ type: 'rate', unit: id, present: true, rate: 48000, locked: true, changing: false, offered: SIX,
  allowed: [44100, 48000, 88200, 96000], stored: 48000, asio: false, streams: false, plugins48k: false, correctionMismatch: false, stripsRebuilt: false,
  combined: { member: false, rate: 0, allowed: [], units: 0, stored: 0, asio: false, mismatch: false, ...comb }, last: { code: '', why: '', ms: 0, changes: 0 }, ...over });

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const ctx = await browser.newContext({ viewport: { width: 1536, height: 864 } });
  await ctx.addInitScript(() => { window.__sent = []; window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } }; });
  const page = await ctx.newPage();
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const rateSent = () => page.evaluate(() => window.__sent.filter(s => /^\{"cmd":"rate-/.test(s)).map(s => JSON.parse(s)));
  const x8Rate = page.getByRole('combobox', { name: 'x8p sample rate' });
  const head = page.locator('.useg[data-unit="0"] .useg-rate');

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P] });
  check((await rateSent()).some(x => x.cmd === 'rate-get'), 'rate-get on connect');
  await page.waitForTimeout(1700);
  check(await x8Rate.isDisabled() && (await x8Rate.locator('option').allTextContents()).join() === '48 kHz (fixed in this driver build)',
    'no answer: "48 kHz (fixed in this driver build)", disabled');

  await host(rate(X8P.id));
  const opts = await x8Rate.locator('option').evaluateAll(o => o.map(x => `${x.textContent}${x.disabled ? '-' : ''}`));
  check(!(await x8Rate.isDisabled()) && opts.join() === '44.1 kHz,48 kHz,88.2 kHz,96 kHz,176.4 kHz-,192 kHz-' && await x8Rate.inputValue() === '48000',
    `the offered rates, those not allowed disabled (${opts.join()})`);
  await x8Rate.selectOption('96000');
  const set = (await rateSent()).filter(x => x.cmd === 'rate-set');
  check(set.length === 1 && set[0].unit === X8P.id && set[0].rate === 96000 && set[0].combined === false, `rate-set with the unit's id (${JSON.stringify(set)})`);
  await host({ type: 'rate-result', unit: X8P.id, rate: 96000, combined: false, pending: true, code: '', why: 'Changing to 96.0 kHz', ms: 0 });
  check(await x8Rate.isDisabled() && /Changing to 96\.0 kHz/.test(await head.textContent()), 'pending: disabled, "Changing to 96.0 kHz"');
  await host({ type: 'rate-result', unit: X8P.id, rate: 96000, combined: false, pending: false, ok: false, code: 'asio', why: 'Cubase has the x8p: change the rate there', ms: 3 });
  check(/Cubase has the x8p/.test(await head.textContent()) && !(await x8Rate.isDisabled()) && await x8Rate.inputValue() === '48000',
    'refused: the host\'s own words under the selector, back at 48 kHz');
  await host(rate(X8P.id, { rate: 96000, plugins48k: true }));
  check(/Plug-ins run at 48 kHz only: the x8p runs at 96 kHz/.test(await page.locator('#plb .plb-notes').textContent()),
    'plugins48k: the browser says plug-ins run at 48 kHz only');

  // combined: one shared selector, SHARED in the heads, rate-set with combined:true
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  const comb = { member: true, rate: 48000, allowed: [44100, 48000], units: 2, stored: 48000 };
  await host(rate(X8P.id, {}, comb));
  await host(rate(TWX.id, {}, comb));
  const shared = page.getByRole('combobox', { name: 'Combined device sample rate' });
  check(await page.locator('#rateBar').isVisible() && await shared.count() === 1 && await x8Rate.count() === 0 &&
    /SHARED/.test(await page.locator('.useg[data-unit="1"] .useg-rate').textContent()), 'combined: one shared selector, each segment says SHARED');
  await shared.selectOption('44100');
  const agg = (await rateSent()).filter(x => x.cmd === 'rate-set').pop();
  check(agg.combined === true && agg.rate === 44100 && (agg.unit === X8P.id || agg.unit === TWX.id), `rate-set combined:true (${JSON.stringify(agg)})`);
  await host({ type: 'rate-result', unit: agg.unit, rate: 44100, combined: true, pending: false, ok: false, code: 'combined-asio', why: 'a DAW has "Apollo (Avium)"' });
  check(/a DAW has "Apollo \(Avium\)"/.test(await page.locator('#rateBar').textContent()), 'refused: said on the shared bar');
  await page.evaluate(() => showView('device'));
  check(await page.getByRole('combobox', { name: 'Combined device sample rate' }).isVisible() && /Combined \(x8p and Twin X\)/.test(await page.locator('#devRate').textContent()),
    'the Device view: the combined device\'s selector');

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
