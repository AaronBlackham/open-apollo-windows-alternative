// [Corr] Speaker correction (ui/correction.js; host side app/src/corr_host.h, driver include/apollo_corr.h): the
// Speakers tab, the unit it acts on (the Twin X by default), loading a profile file (checked by the host, shown with
// its filters, target and curves, outputs chosen, applied), on/off and A/B as corr-set, the state the driver reports,
// refusals inline, removing it, the hardware-monitoring note, an old driver, the demo, and 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
const REF = path.resolve(__dirname, '../../tools/corr-test/ref');
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: 'SN20000000000001', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '20000000000001', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const FLAGS = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: FLAGS, mode: 2, observe: false };
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 2, playChannels: 46,
  recChannels: 34, numPreamps: 2, serial: TWX.serial, modelName: 'Apollo Twin X (Gen 1)' };

const STUDIO = JSON.parse(fs.readFileSync(path.join(REF, 'studio.json'), 'utf8'));
const SUMMARY = 'Standard, 9 spots, loopback timing. Target Studio tilt, -0.8 dB/oct around 1 kHz. Boosts up to 6 dB.';
/* The driver's copy as the host sends it (corr_profile.h ProfileJson): the 48 kHz coefficients on slots 0/1. */
const driverProfile = (slots = { L: 0, R: 1 }) => ({
  name: STUDIO.name, id: STUDIO.id, created: STUDIO.created, summary: SUMMARY, rate: 48000, preampDb: STUDIO.preamp_db,
  channels: STUDIO.channels.filter(c => slots[c.id] >= 0).map(c => ({ id: c.id, slot: slots[c.id], output: c.output, gainDb: c.gain_db,
    delayMs: c.delay_ms, filters: c.filters.map(b => ({ type: b.type, enabled: b.enabled, freq: b.freq_hz, gain: b.gain_db || 0,
      q: b.q || 0.7071, coef: b.coefficients['48000'] })) })),
});
const state = (o = {}) => ({ flags: 0, loaded: false, enabled: false, bypass: false, running: false, rateMismatch: false, stored: false,
  slotRange: false, unitIdle: false, unitRate: 48000, wet: 0, swap: 1000, runs: 0, frames: 0, late: 0, lastNs: 0, maxNs: 0,
  storeStatus: 0, from: 0, profile: null, ...o });
const ON = () => state({ loaded: true, enabled: true, running: true, wet: 1000, stored: true, lastNs: 5100, maxNs: 9300, from: 2,
  profile: driverProfile() });

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

  /* ---------------- A. Host path ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx.addInitScript(stub);
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  const corrSince = n => page.evaluate(k => window.__sent.slice(k).filter(s => typeof s === 'string' && s.startsWith('{'))
    .map(s => JSON.parse(s)).filter(o => /^corr-/.test(o.cmd)), n);
  const stateText = () => page.locator('.cr-state').textContent();
  const pressed = async loc => (await loc.getAttribute('aria-pressed')) === 'true';

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });

  // 1. The tab: after Units & routing (Ctrl+5: the plug-ins live on the Control view now, no tab); it asks the driver about the Twin X (the monitor unit) first.
  const tabs = await page.locator('.tabs button').allTextContents();
  check(tabs.indexOf('Speakers') === tabs.indexOf('Units & routing') + 1 && tabs.slice(0, 3).join() === 'Control,Status,Cues',
    `a Speakers tab after Units & routing (${tabs.join(', ')})`);
  let n = await mark();
  await page.locator('.brand').click();   // focus off any control (not a fixed spot in the tab bar, which grows with tabs)
  await page.keyboard.press('Control+5');
  check(await page.evaluate(() => S.view) === 'correction' && await page.locator('#view-correction').isVisible(), 'Ctrl+5 opens it');
  let c = await corrSince(n);
  check(c.length >= 1 && c[0].cmd === 'corr-get' && c[0].unit === TWX.id, `corr-get for the Twin X first (${JSON.stringify(c)})`);
  check(/Reading the driver/.test(await stateText()), 'until the driver answers: reading');
  check(/never passes through the computer, so it is not corrected/.test(await page.locator('.cr-hw').textContent()) &&
    /Windows/.test(await page.locator('.cr-hw').textContent()) && /ASIO/.test(await page.locator('.cr-hw').textContent()),
    'the note: hardware input monitoring is not corrected; Windows and the DAW are');

  // 2. No profile yet.
  await host({ type: 'corr', unit: TWX.id, present: true, state: state(), error: 0, detail: '' });
  check(/No profile on this unit/.test(await stateText()), 'no profile: says so');
  check(await page.getByRole('button', { name: 'Speaker correction on' }).isDisabled() &&
    await page.getByRole('button', { name: 'A/B: listen to the uncorrected signal' }).isDisabled() &&
    await page.getByRole('button', { name: 'Remove' }).isDisabled(), 'on/off, A/B and Remove wait for a profile');
  check(await page.locator('.cr-bar').isVisible() && await pressed(page.locator('.cr-bar button', { hasText: 'Twin X' })),
    'two units: the selector, Twin X selected');

  // 3. Load a file: the host checks it for this unit.
  n = await mark();
  let chooser = page.waitForEvent('filechooser');
  await page.getByRole('button', { name: 'Load profile...' }).click();
  await (await chooser).setFiles(path.join(REF, 'studio.json'));
  await page.waitForTimeout(150);
  c = (await corrSince(n)).filter(x => x.cmd === 'corr-check');
  check(c.length === 1 && c[0].unit === TWX.id && /avium-correction-profile/.test(c[0].text) && c[0].seq > 0,
    'the file text goes to the host as corr-check for the Twin X');
  check(/Checking the file/.test(await page.locator('.cr-preview').textContent()), 'checking...');
  await host({ type: 'corr-check', unit: TWX.id, seq: c[0].seq + 99, ok: false, problems: ['stale'], unitRate: 48000, designed: false, summary: '' });
  check(!/stale/.test(await page.locator('.cr-preview').textContent()), 'an answer about another check is ignored');
  await host({ type: 'corr-check', unit: TWX.id, seq: c[0].seq, ok: true, problems: [], unitRate: 48000, designed: false, summary: SUMMARY });
  const preview = page.locator('.cr-preview');
  check(await preview.locator('table.cr-filters').count() === 2 &&
    await preview.locator('table.cr-filters').nth(0).locator('tbody tr').count() === 7 &&
    await preview.locator('table.cr-filters').nth(1).locator('tbody tr').count() === 6, 'the filters: 7 on L (one off), 6 on R');
  check(await preview.locator('tr.cr-off').count() === 1 && /OFF/.test(await preview.locator('tr.cr-off').textContent()), 'the disabled one marked OFF');
  check(/45\.3 Hz/.test(await preview.textContent()) && /-8\.2 dB/.test(await preview.textContent()) && /5\.10/.test(await preview.textContent()),
    'first filter: Peak 45.3 Hz -8.2 dB Q 5.10');
  check((await preview.textContent()).includes(SUMMARY) && /-2\.5 dB/.test(await preview.locator('.cr-meta').textContent()),
    'the target summary and the preamp');
  check(await preview.locator('svg.cr-chart path.cr-curve').count() === 2, 'two correction curves');
  check(await page.getByRole('combobox', { name: 'Output for L' }).inputValue() === '0' &&
    await page.getByRole('combobox', { name: 'Output for R' }).inputValue() === '1' &&
    (await page.getByRole('combobox', { name: 'Output for L' }).locator('option').allTextContents()).join() === 'Not corrected,MON L,MON R,LINE 3,LINE 4',
    'outputs: L on MON L, R on MON R, from the Twin X\'s list');
  check(await preview.getByRole('checkbox', { name: 'Turn correction on' }).isChecked(), 'turn on: checked');
  await page.screenshot({ path: path.join(out, 'ui-correction-preview.png') });

  // 4. Apply: corr-load with the slots; the driver's state follows.
  n = await mark();
  await preview.getByRole('button', { name: 'Apply to the Twin X' }).click();
  c = (await corrSince(n)).filter(x => x.cmd === 'corr-load');
  check(c.length === 1 && c[0].unit === TWX.id && c[0].slots.L === 0 && c[0].slots.R === 1 && c[0].enable === true &&
    /avium-correction-profile/.test(c[0].text), `corr-load: L 0, R 1, enable (${JSON.stringify({ ...c[0], text: '...' })})`);
  check(await preview.getByRole('button', { name: 'Loading...' }).isDisabled(), 'Apply waits for the answer');
  await host({ type: 'corr-result', unit: TWX.id, op: 'load', ok: true, error: 0, detail: '' });
  await host({ type: 'corr', unit: TWX.id, present: true, state: ON(), error: 0, detail: '' });
  check(await page.locator('.cr-preview').count() === 0, 'loaded: the preview closes');
  check(/On: Twin X speakers corrected/.test(await stateText()), 'state: on');
  check(await pressed(page.getByRole('button', { name: 'Speaker correction on' })), 'the on switch lit');
  check(/L on MON L, R on MON R/.test(await page.locator('.cr-kv').textContent()) && /Studio A, listening position/.test(await page.locator('.cr-kv').textContent()),
    'applies to: L on MON L, R on MON R; the profile name');
  check(/5\.1 µs per 1 ms tick/.test(await page.locator('.cr-kv').textContent()) && /kept for the next start/.test(await page.locator('.cr-kv').textContent()),
    'the driver line: cost per tick, stored');
  check(await page.locator('.cr-panel').nth(1).locator('table.cr-filters').count() === 2 && /on MON L/.test(await page.locator('.cr-ch-head').first().textContent()),
    'the loaded profile below, channels named with their outputs');
  check(/Profile loaded on the Twin X/.test(await page.locator('.toasts').textContent()), 'a toast says so');

  // 5. A/B and off: corr-set; the page follows the driver's state.
  n = await mark();
  await page.getByRole('button', { name: 'A/B: listen to the uncorrected signal' }).click();
  c = await corrSince(n);
  check(c.some(x => x.cmd === 'corr-set' && x.op === 'bypass' && x.value === true && x.unit === TWX.id), 'A/B: corr-set bypass on');
  await host({ type: 'corr-result', unit: TWX.id, op: 'bypass', ok: true, error: 0, detail: '' });
  await host({ type: 'corr', unit: TWX.id, present: true, state: { ...ON(), bypass: true, wet: 0 }, error: 0, detail: '' });
  check(/uncorrected/.test(await stateText()) && await pressed(page.getByRole('button', { name: 'A/B: listen to the uncorrected signal' })) &&
    /Hearing: uncorrected/.test(await page.locator('.cr-ab-text').textContent()), 'A/B: hearing uncorrected, lit');
  n = await mark();
  await page.getByRole('button', { name: 'A/B: listen to the uncorrected signal' }).click();
  check((await corrSince(n)).some(x => x.cmd === 'corr-set' && x.op === 'bypass' && x.value === false), 'A/B again: back');
  await host({ type: 'corr', unit: TWX.id, present: true, state: ON(), error: 0, detail: '' });
  n = await mark();
  await page.getByRole('button', { name: 'Speaker correction on' }).click();
  check((await corrSince(n)).some(x => x.cmd === 'corr-set' && x.op === 'enable' && x.value === false), 'the switch: corr-set enable off');
  await host({ type: 'corr-result', unit: TWX.id, op: 'enable', ok: true, error: 0, detail: '' });
  await host({ type: 'corr', unit: TWX.id, present: true, state: { ...ON(), enabled: false, running: false, wet: 0 }, error: 0, detail: '' });
  check(/Off: the outputs play uncorrected/.test(await stateText()) && !(await pressed(page.getByRole('button', { name: 'Speaker correction on' }))) &&
    await page.getByRole('button', { name: 'A/B: listen to the uncorrected signal' }).isDisabled(), 'off: says so, A/B disabled');
  await host({ type: 'corr', unit: TWX.id, present: true, state: { ...ON(), rateMismatch: true, unitRate: 96000 }, error: 0, detail: '' });
  check(/the profile is for 48000 Hz and the unit runs 96000 Hz/.test(await stateText()), 'a rate mismatch explained');
  await host({ type: 'corr', unit: TWX.id, present: true, state: ON(), error: 0, detail: '' });

  // 6. A file Avium Measure's checks refuse: the problems, no Apply.
  const bad = JSON.parse(fs.readFileSync(path.join(REF, 'cases.json'), 'utf8')).find(x => x.name === 'unstable');
  const badPath = path.join(out, 'corr-unstable.json');
  fs.writeFileSync(badPath, bad.text);
  n = await mark();
  chooser = page.waitForEvent('filechooser');
  await page.getByRole('button', { name: 'Load profile...' }).click();
  await (await chooser).setFiles(badPath);
  await page.waitForTimeout(150);
  c = (await corrSince(n)).filter(x => x.cmd === 'corr-check');
  await host({ type: 'corr-check', unit: TWX.id, seq: c[0].seq, ok: false, problems: bad.problems, unitRate: 48000, designed: false, summary: '' });
  check(/cannot be used/.test(await page.locator('.cr-problems').textContent()) &&
    (await page.locator('.cr-problems li').allTextContents()).join() === bad.problems.join(), `the problems listed (${bad.problems.join(', ')})`);
  check(await page.locator('.cr-preview').getByRole('button', { name: /Apply/ }).count() === 0, 'no Apply for it');
  await page.locator('.cr-preview').getByRole('button', { name: 'Close' }).click();
  check(await page.locator('.cr-preview').count() === 0, 'Close drops the file');

  // 7. A good file the driver refuses: the reason inline; outputs on one slot refused before it is sent.
  n = await mark();
  chooser = page.waitForEvent('filechooser');
  await page.getByRole('button', { name: 'Load profile...' }).click();
  await (await chooser).setFiles(path.join(REF, 'studio.json'));
  await page.waitForTimeout(150);
  c = (await corrSince(n)).filter(x => x.cmd === 'corr-check');
  await host({ type: 'corr-check', unit: TWX.id, seq: c[0].seq, ok: true, problems: [], unitRate: 48000, designed: true, summary: SUMMARY });
  check(/no coefficients for 48000 Hz: Apollo Control designs them/.test(await page.locator('.cr-preview').textContent()), 'designed-from-parameters note');
  await page.getByRole('combobox', { name: 'Output for R' }).selectOption('0');
  n = await mark();
  await page.locator('.cr-preview').getByRole('button', { name: 'Apply to the Twin X' }).click();
  check((await corrSince(n)).filter(x => x.cmd === 'corr-load').length === 0 && /Two channels are on the same output/.test(await page.locator('.cr-error').textContent()),
    'L and R on one output: refused on the page');
  await page.getByRole('combobox', { name: 'Output for R' }).selectOption('3');
  await page.locator('.cr-preview').getByRole('button', { name: 'Apply to the Twin X' }).click();
  c = (await corrSince(n)).filter(x => x.cmd === 'corr-load');
  check(c.length === 1 && c[0].slots.R === 3, 'R moved to LINE 4 goes out as slot 3');
  await host({ type: 'corr-result', unit: TWX.id, op: 'load', ok: false, error: 10, detail: 'the corrected signal could clip: the preamp is too high (channel 1)' });
  check(/could clip/.test(await page.locator('.cr-error').textContent()) && await page.locator('.cr-preview').count() === 1,
    'the driver\'s refusal inline, the file stays open');
  await page.locator('.cr-preview').getByRole('button', { name: 'Close' }).click();

  // 8. Remove: asks, then corr-set clear.
  n = await mark();
  await page.getByRole('button', { name: 'Remove' }).click();
  await page.locator('#dlg').getByRole('button', { name: 'Remove' }).click();
  check((await corrSince(n)).some(x => x.cmd === 'corr-set' && x.op === 'clear' && x.unit === TWX.id), 'Remove: confirmed, corr-set clear');
  await host({ type: 'corr-result', unit: TWX.id, op: 'clear', ok: true, error: 0, detail: '' });
  await host({ type: 'corr', unit: TWX.id, present: true, state: state(), error: 0, detail: '' });
  check(/No profile on this unit/.test(await stateText()) && await page.locator('.cr-panel').count() === 1, 'removed: no profile, no profile panel');

  // 9. The x8p; a driver without speaker correction.
  n = await mark();
  await page.locator('.cr-bar button', { hasText: 'x8p' }).click();
  check((await corrSince(n)).some(x => x.cmd === 'corr-get' && x.unit === X8P.id), 'the x8p selected: corr-get for it');
  await host({ type: 'corr', unit: X8P.id, present: true, state: null, error: 1, detail: 'this driver build has no speaker correction (install a newer apollo.sys)' });
  check(/no speaker correction/.test(await stateText()) && /Apollo x8p/.test(await page.locator('.cr-panel h2').first().textContent()),
    'an old driver: says so, on the x8p');
  await page.setViewportSize({ width: 980, height: 760 });
  await page.locator('.cr-bar button', { hasText: 'Twin X' }).click();
  await host({ type: 'corr', unit: TWX.id, present: true, state: ON(), error: 0, detail: '' });
  const over = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth ||
    [...document.querySelectorAll('#view-correction .panel')].some(p => p.getBoundingClientRect().right > document.documentElement.clientWidth + 1));
  check(!over, '980 px: no horizontal overflow');
  await page.screenshot({ path: path.join(out, 'ui-correction-980.png'), fullPage: true });
  await ctx.close();

  /* ---------------- B. Demo ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=correction');
  await demo.waitForTimeout(800);
  check(await demo.locator('#view-correction').isVisible() && /On: Twin X speakers corrected/.test(await demo.locator('.cr-state').textContent()),
    'demo: the Twin X with a profile, on');
  await demo.getByRole('button', { name: 'A/B: listen to the uncorrected signal' }).click();
  await demo.waitForTimeout(200);
  check(/uncorrected/.test(await demo.locator('.cr-state').textContent()), 'demo: A/B works locally');
  await demo.screenshot({ path: path.join(out, 'ui-correction-demo.png') });
  await demo.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  if (errors.length) failed++;
  await browser.close();
  if (failed) process.exitCode = 1;
})();
