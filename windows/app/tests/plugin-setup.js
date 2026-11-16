// [PlugSetup] Set up plug-ins on a release install (plugin_engine.h SetupPoll / SetupStart, plugins/tools/setup-plugins.ps1),
// through a stub host: the box in the plug-in browser while plug-ins are not set up, the explanation dialog (downloads,
// sizes, time, offline reading of the UAD install, never Universal Audio, which plug-ins, the administrator prompt) before
// plug-setup-start, licensed or every plug-in, progress with Cancel, done (the box goes, a toast says what was mapped),
// a stopped setup, no UAD install, the Settings row (and a developer layout left alone), the demo, and 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };
const DOCKED = JSON.stringify({ appearance: { look: 'studio' } });   /* the docked browser (plugins.js tests do the same) */

const X8P = { id: 'SN20000000000001', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '20000000000001', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: 0x7C0, mode: 2, observe: false };
const DIR = 'C:\\Users\\someone\\AppData\\Local\\Avium\\ApolloPlugins';
const NOMAPS = { type: 'plugin-list', unit: 0, state: 'failed', why: 'UAD plug-ins are not set up on this PC yet: Set up plug-ins maps them from your own UAD install',
  licence: 'unknown', licenceWhy: 'plug-in hosting is off', mapped: 0, authorized: 0, items: [] };
const PHASES = ['check', 'python', 'packages', 'tools', 'specs', 'store', 'licence', 'map', 'modules'];
const status = over => ({ state: 'running', phase: 'map', text: 'Mapping 3/12: UAD Pultec EQP-1A (mono, stereo)...', percent: 42, done: 3, total: 12, eta: 300,
  code: '', error: '', summary: '', modules: '', mode: 'licensed', log: DIR + '\\setup\\setup.log',
  phases: PHASES.map((id, k) => ({ id, label: id, state: k < 7 ? 'done' : k === 7 ? 'running' : 'pending', detail: '' })), ...over });
const setup = over => ({ type: 'plugin-setup', offered: true, available: true, uad: true, dir: DIR, inUse: '', configured: '', dev: false, ready: false,
  running: false, licensed: -1, licUnits: 0, mappable: 119, cpus: 8, hosting: false, why: '', status: null, ...over });

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

  /* ---------------- A. A release install without plug-ins, through the stub host ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1536, height: 900 } });
  await ctx.addInitScript(stub);
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  const sent = n => page.evaluate(k => window.__sent.slice(k).filter(s => s.startsWith('{"cmd":"plug-setup')).map(s => JSON.parse(s)), n);
  const box = page.locator('#plbSetup');
  const dlg = page.locator('#dlg');

  let n = await mark();
  await host({ type: 'hello', app: 'test', config: DOCKED, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P] });
  check((await sent(n)).some(x => x.cmd === 'plug-setup-state'), 'hello and the Control view ask for the setup\'s state (plug-setup-state)');
  await host(NOMAPS);
  check(await box.isHidden(), 'no "plugin-setup" from the host yet (an older host): no box');
  await host(setup());
  check(await box.isVisible() && /UAD plug-ins are not set up on this PC/.test(await box.textContent()) &&
    await box.getByRole('button', { name: 'Set up plug-ins' }).isEnabled(), 'not set up: the box in the browser with Set up plug-ins');
  check((await page.locator('#plb .plb-foot').textContent()) === '', 'the browser\'s foot leaves the words to the box');
  await page.screenshot({ path: path.join(out, 'ui-plugin-setup-box.png') });

  // 1. The dialog explains before anything starts; Cancel sends nothing.
  n = await mark();
  await box.getByRole('button', { name: 'Set up plug-ins' }).click();
  await dlg.waitFor({ state: 'visible' });
  const text = await dlg.textContent();
  check(/Set up UAD plug-ins/.test(text) && /www\.python\.org/.test(text) && /files\.pythonhosted\.org/.test(text) && /github\.com/.test(text) &&
    /10\.9 MB/.test(text) && /about 42 MB/.test(text), 'the dialog lists each download, where from and how big');
  check(/SHA-256/.test(text) && /never contacts Universal Audio/.test(text) && /starts no Universal Audio program/.test(text) && /read as bytes/.test(text),
    'the dialog says every file is hash-checked and that the UAD install is read offline, never contacting Universal Audio');
  check(/up to 119/.test(text) && /no licence answer/.test(text) && /turn plug-in hosting on first/.test(text) && /about \d+ minutes/.test(text) &&
    /Chambers/.test(text) && /continues where it stopped/.test(text) && /230 MB/.test(text), 'no licence answer: every plug-in, with how to map only the licensed ones; time, disk, resumable');
  check(/None now\. Turning plug-in hosting on later copies/.test(text), 'hosting off: no administrator prompt now');
  check(await page.evaluate(() => document.getElementById('dlg').classList.contains('pl-setup-wide')), 'the dialog is the wide one');
  await page.screenshot({ path: path.join(out, 'ui-plugin-setup-dialog.png') });
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  check((await sent(n)).length === 0 && !(await page.evaluate(() => document.getElementById('dlg').classList.contains('pl-setup-wide'))), 'Cancel: nothing sent');
  n = await mark();
  await box.getByRole('button', { name: 'Set up plug-ins' }).click();
  await dlg.getByRole('button', { name: 'Start setup' }).click();
  let s = await sent(n);
  check(s.length === 1 && s[0].cmd === 'plug-setup-start' && s[0].all === true, `Start setup: plug-setup-start all (${JSON.stringify(s)})`);

  // 2. With a licence answer: the licensed ones by default, every one on request; hosting on: one prompt at the end.
  await host(setup({ licensed: 37, licUnits: 1, hosting: true }));
  n = await mark();
  await box.getByRole('button', { name: 'Set up plug-ins' }).click();
  const t2 = await dlg.textContent();
  check(/The plug-ins your unit licenses \(37/.test(t2) && await dlg.getByRole('radio', { name: /your unit licenses/ }).isChecked() &&
    /At the end, one administrator prompt/.test(t2), 'licence answer: the licensed plug-ins by default; hosting on: one administrator prompt at the end');
  const before = await dlg.locator('.pl-setup-time').textContent();
  await dlg.getByRole('radio', { name: /Every plug-in/ }).check();
  const after = await dlg.locator('.pl-setup-time').textContent();
  check(after !== before && /up to 119/.test(after) && /about 37 plug-ins/.test(before), 'choosing every plug-in changes the time estimate');
  await dlg.getByRole('radio', { name: /your unit licenses/ }).check();
  await dlg.getByRole('button', { name: 'Start setup' }).click();
  s = await sent(n);
  check(s.length === 1 && s[0].all === false, `licensed: plug-setup-start all false (${JSON.stringify(s)})`);

  // 3. Running: progress, the mapper's count and time left, Cancel.
  await host(setup({ running: true, ready: true, status: status() }));
  const bar = box.getByRole('progressbar', { name: 'Plug-in setup' });
  check(await box.isVisible() && await bar.getAttribute('aria-valuenow') === '42' && /Mapping plug-ins/i.test(await box.locator('.pl-setup-phase').textContent()) &&
    /3 of 12 plug-ins mapped, about 5 min left/.test(await box.textContent()), 'running: a progress bar (42 %), the phase, 3 of 12 mapped, about 5 min left');
  await page.screenshot({ path: path.join(out, 'ui-plugin-setup-running.png') });
  n = await mark();
  await box.getByRole('button', { name: 'Cancel' }).click();
  s = await sent(n);
  check(s.length === 1 && s[0].cmd === 'plug-setup-cancel', 'Cancel: plug-setup-cancel');
  await host(setup({ running: true, ready: false, status: status({ phase: 'packages', percent: 9, text: 'Downloading numpy-2.5.4-cp313-cp313-win_amd64.whl: 4.0 of 12.0 MB', done: 0, total: 0, eta: -1 }) }));
  check(/Downloading numpy/.test(await box.textContent()) && /Python packages/i.test(await box.locator('.pl-setup-phase').textContent()), 'a download: its own words');

  // 4. Done: the box goes, a toast says what was mapped.
  await host(setup({ running: false, ready: true, status: status({ state: 'done', phase: '', percent: 100, summary: '12 plug-ins mapped (24 maps, mono and stereo)',
    text: 'Plug-ins are set up: 12 plug-ins mapped (24 maps, mono and stereo).' }) }));
  check(await box.isHidden(), 'done: the box goes (the browser lists the maps)');
  check(/12 plug-ins mapped/.test(await page.locator('#toasts').textContent()), 'done: a toast says what was mapped');

  // 5. Stopped: the reason and Set up plug-ins again; with maps in use it can be dismissed.
  await host(setup({ running: true, ready: true, status: status() }));
  await host(setup({ running: false, ready: false, status: status({ state: 'failed', code: 'network', error: 'Could not download the panel specs from GitHub (see the log)' }) }));
  check(await box.isVisible() && /Plug-in setup stopped/.test(await box.textContent()) && /Could not download the panel specs/.test(await box.textContent()) &&
    await box.getByRole('button', { name: 'Set up plug-ins again' }).isVisible(), 'stopped: the reason, and Set up plug-ins again');
  check(/Plug-in setup stopped/.test(await page.locator('#toasts').textContent()), 'stopped: a toast');
  await host(setup({ ready: true, status: status({ state: 'stopped', error: 'It stopped (Apollo Control quit?)' }) }));
  await box.getByRole('button', { name: 'Dismiss' }).click();
  check(await box.isHidden(), 'stopped with maps in use: Dismiss hides the box');

  // 6. No UAD install, no setup script: the button is off and says why.
  await host(setup({ uad: false }));
  check(/No UAD install found on this PC/.test(await box.textContent()) && await box.getByRole('button', { name: 'Set up plug-ins' }).isDisabled(), 'no UAD install: said, the button off');
  await host(setup({ available: false }));
  check(/not installed with this Apollo Control/.test(await box.textContent()) && await box.getByRole('button', { name: 'Set up plug-ins' }).isDisabled(), 'no setup script: said, the button off');
  await host(setup({ why: 'the setup did not start: Access is denied.' }));
  check(/the setup did not start/.test(await box.textContent()), 'a start that failed in the host: its words in the box');

  // 7. Settings: the row, its progress and Cancel; a developer layout is left alone.
  await host(setup());
  await page.evaluate(() => showView('settings'));
  const row = page.locator('#plSetupRow');
  check(await row.isVisible() && /Not set up/.test(await row.textContent()) && /ApolloPlugins/.test(await row.textContent()), 'Settings: the setup row, not set up, with the folder');
  n = await mark();
  await row.getByRole('button', { name: 'Set up plug-ins' }).click();
  check(await dlg.isVisible(), 'Settings: Set up plug-ins opens the same dialog');
  await dlg.getByRole('button', { name: 'Cancel' }).click();
  await host(setup({ running: true, ready: false, status: status() }));
  check(await row.getByRole('progressbar').getAttribute('aria-valuenow') === '42', 'Settings: the progress while it runs');
  await row.getByRole('button', { name: 'Cancel' }).click();
  check((await sent(n)).some(x => x.cmd === 'plug-setup-cancel'), 'Settings: Cancel');
  await host(setup({ ready: true, status: status({ state: 'done', summary: '12 plug-ins mapped (24 maps, mono and stereo)', modules: 'Driver module store: 40 module(s) installed.' }) }));
  check(/12 plug-ins mapped/.test(await row.textContent()) && /Map again after you buy/.test(await row.textContent()) &&
    await row.getByRole('button', { name: 'Map plug-ins again' }).isVisible(), 'Settings: done, what was mapped, and Map plug-ins again');
  await page.screenshot({ path: path.join(out, 'ui-plugin-setup-settings.png') });
  await host(setup({ offered: false, dev: true, ready: true, inUse: 'C:\\src\\apollo-win\\plugins' }));
  check(/Developer maps in use/.test(await page.locator('#plSettings').textContent()) && await page.locator('#plSettings').getByRole('button', { name: /Set up plug-ins|Map plug-ins again/ }).count() === 0,
    'Settings: a developer layout is left as it is (no setup button)');
  await host(setup({ offered: false, configured: 'D:\\maps', ready: false }));
  check(/A maps folder is set above/.test(await page.locator('#plSettings').textContent()), 'Settings: a folder set above, said');
  await page.evaluate(() => showView('control'));
  check(await box.isHidden(), 'not offered (a developer layout or a folder set): no box in the browser');
  await ctx.close();

  /* ---------------- B. 980 px: the box and the dialog fit ---------------- */
  const nctx = await browser.newContext({ viewport: { width: 980, height: 700 } });
  await nctx.addInitScript(stub);
  const narrow = await nctx.newPage();
  watch(narrow);
  await narrow.goto(ui);
  const nh = m => narrow.evaluate(x => onHostMessage(x), m);
  await nh({ type: 'hello', app: 'test', config: DOCKED, demo: false, view: 'control' });
  await nh({ type: 'status', present: true, info: INFO });
  await nh(NOMAPS);
  await nh(setup({ running: true, status: status({ text: 'Downloading unicorn-2.1.4-cp37-abi3-win_amd64.whl: 12.0 of 15.2 MB', phase: 'packages' }) }));
  const over = await narrow.evaluate(() => {
    const bad = [];
    if (document.documentElement.scrollWidth > document.documentElement.clientWidth) bad.push('document');
    const v = document.getElementById('view-control').getBoundingClientRect();
    for (const n of document.querySelectorAll('#plb, #plbSetup, #plbSetup *')) { const r = n.getBoundingClientRect(); if (r.width && r.right > v.right + 1) bad.push(n.className || n.id); }
    return bad;
  });
  check(!over.length, `980 px: the setup box fits (${over.join(', ') || 'nothing past the edge'})`);
  await nh(setup());
  await narrow.locator('#plbSetup').getByRole('button', { name: 'Set up plug-ins' }).click();
  const d = await narrow.locator('#dlg').boundingBox();
  check(d && d.x >= 0 && d.x + d.width <= 980 && d.y >= 0 && d.y + d.height <= 700, `980 px: the dialog fits the window (${JSON.stringify(d)})`);
  await narrow.screenshot({ path: path.join(out, 'ui-plugin-setup-980.png') });
  await nctx.close();

  /* ---------------- C. The demo: plug-ins are invented, no setup ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=control');
  await demo.waitForTimeout(200);
  check(await demo.locator('#plbSetup').isHidden() && await demo.locator('#plb .plb-item').count() > 0, 'demo: its invented plug-ins, no setup box');
  await demo.evaluate(() => showView('settings'));
  check(await demo.locator('#plSetupRow').count() === 0, 'demo: no setup row in Settings');
  await demo.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
