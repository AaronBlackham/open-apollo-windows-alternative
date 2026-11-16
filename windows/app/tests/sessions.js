// [Sess] Sessions, Start with Windows and the tray (docs/apollo-control-units.md 1.11), against a stub host that keeps the
// session files, the HKCU Run value, the plug-in engine's chains and the sample rates in memory (nothing reaches the
// registry or the disk): a save / load round trip in the app's send order (the rate first and waited for, then the
// preamps, the mix, the plug-ins), the 48V question, a unit that is not connected getting its part when it connects, the
// last state once the strips are Live, typed sessions (Mono's command line, Ctrl+K), the autostart toggle writing and
// removing the Run value, and the tray's commands. Then the demo: the session list in every theme's layout.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
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
const CHAIN = { unit: X8P.id, input: 0, aux: null, stereo: false, mon: true, unison: null,
  slots: [{ slot: 0, bypass: false, name: 'UAD 1176LN', bundle: 'UAD 1176LN.mono.json', bundleSha: 'ab12', values: [3, 7], texts: ['3', '7'], diff: { 4097: 12 } },
    { slot: 2, bypass: true, name: 'UAD Pultec EQP-1A', bundle: 'UAD Pultec EQP-1A.mono.json', bundleSha: 'cd34', values: [1], texts: ['1'], diff: {} }] };

/* the host: the session files, the Run value, the engine's chains and the rates, in memory */
const stub = () => {
  window.__sent = [];
  window.__files = {};
  window.__last = null;
  window.__run = '';
  window.__doc = { version: 2, mapsDir: '', chains: [] };
  window.__rates = { SN22532017106180: 48000, '7C1E40A2D3F09B61': 48000 };
  const all = [44100, 48000, 88200, 96000, 176400, 192000];
  const reply = m => setTimeout(() => onHostMessage(m), 5);
  const rate = id => ({ type: 'rate', unit: id, present: true, rate: window.__rates[id], locked: true, changing: false, offered: all, allowed: all, stored: window.__rates[id],
    asio: false, streams: false, plugins48k: false, correctionMismatch: false, stripsRebuilt: false, combined: { member: false, rate: 0, allowed: [], units: 0 } });
  const list = () => ({ type: 'sessions', dir: 'C:\\stub\\sessions', last: window.__last ? { file: 'last-state.json', name: 'Last state', saved: window.__last.saved, units: [], inserts: 0 } : null,
    list: Object.entries(window.__files).map(([file, x]) => ({ file, name: x.name, saved: x.saved, version: x.version, bad: '',
      units: x.units.map(u => ({ id: u.id, model: u.model, deviceType: u.deviceType, rate: u.rate })), inserts: 0 })) });
  const auto = () => ({ type: 'autostart', on: !!window.__run, current: !!window.__run, command: window.__run, error: 0, detail: '' });
  window.chrome = { webview: { addEventListener: () => {}, postMessage: s => {
    window.__sent.push(s);
    if (typeof s !== 'string' || s[0] !== '{') return;
    const m = JSON.parse(s);
    const file = n => `${n}.json`;
    switch (m.cmd) {
      case 'session-list': reply(list()); break;
      case 'session-save': window.__files[file(m.name)] = m.session; reply({ type: 'session-result', op: 'save', ok: true, name: m.name, file: file(m.name), why: '' }); reply(list()); break;
      case 'session-autosave': window.__last = m.session; reply({ type: 'session-result', op: 'autosave', ok: true, name: '', file: 'last-state.json', why: '' }); break;
      case 'session-load': {
        const x = m.last ? window.__last : window.__files[m.file] || window.__files[file(m.name)];
        reply(x ? { type: 'session', op: m.last ? 'last' : 'load', name: x.name, file: m.file, session: JSON.parse(JSON.stringify(x)) }
          : { type: 'session-result', op: m.last ? 'last' : 'load', ok: false, name: m.name || '', file: '', why: 'none' });
        break;
      }
      case 'session-delete': delete window.__files[m.file || file(m.name)]; reply({ type: 'session-result', op: 'delete', ok: true, name: m.name, file: m.file, why: '' }); reply(list()); break;
      case 'session-plugins': reply({ type: 'plugin-session', op: 'get', doc: JSON.parse(JSON.stringify(window.__doc)) }); break;
      case 'plug-session-set': reply({ type: 'plugin-result', unit: m.unit, op: 'session', input: 0, aux: null, slot: null, ok: true, code: '', detail: '' }); break;
      case 'autostart-get': reply(auto()); break;
      case 'autostart-set': window.__run = m.on ? '"C:\\stub\\apollo-control.exe" --minimized' : ''; reply(auto()); break;
      case 'rate-get': for (const id of Object.keys(window.__rates)) reply(rate(id)); break;
      case 'rate-set':
        reply({ type: 'rate-result', unit: m.unit, rate: m.rate, combined: false, pending: true, code: '', why: 'Changing' });
        setTimeout(() => { window.__rates[m.unit] = m.rate; onHostMessage({ type: 'rate-result', unit: m.unit, rate: m.rate, combined: false, pending: false, ok: true, code: '', why: 'done' });
          for (const id of Object.keys(window.__rates)) onHostMessage(rate(id)); }, 300);
        break;
    }
  } } };
};

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const watch = p => {
    p.on('pageerror', e => errors.push(e.message));
    p.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  };

  /* ---------------- A. The host path ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx.addInitScript(stub);
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const mark = () => page.evaluate(() => window.__sent.length);
  const since = n => page.evaluate(k => window.__sent.slice(k), n);
  const hw = n => since(n).then(s => s.filter(x => /^(monitor|input)\t/.test(x) || /"cmd":"(rate-set|plug-session-set)"/.test(x)));
  const dlgTitle = () => page.locator('#dlg[open] h3').textContent();
  const answer = async label => { await page.locator('#dlg[open]').getByRole('button', { name: label, exact: true }).click(); };
  const settle = ms => page.waitForTimeout(ms);
  const jsonSent = (n, cmd) => since(n).then(s => s.filter(x => x[0] === '{').map(x => JSON.parse(x)).filter(x => x.cmd === cmd));

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await host({ type: 'readback', unit: 0, valid: true, seq: 1, words: words(w => { w[2] = 140; w[6] = 0xC0000020; }) });
  await host({ type: 'readback', unit: 1, valid: true, seq: 1, words: words(w => { w[2] = 150 | (140 << 8); w[6] = 0xC0000011; }) });
  await settle(200);
  let s = await since(0);
  check(s.some(x => /"cmd":"autostart-get"/.test(x)) && !s.some(x => /"cmd":"autostart-set"/.test(x)) && s.some(x => /"cmd":"session-list"/.test(x)),
    'hello: the session list and the Run value are read; nothing writes the Run value');

  // 1. Save: every unit's part, the engine's chains for the unit, the view, the recent list for the tray.
  await page.evaluate(c => {
    setInput(0, 'gain', 40, { unit: 0 }); setInput(1, '48v', true, { unit: 0, noLink: true }); setInput(0, 'gain', 30, { unit: 1 });
    setMix(0, 'level', -20, 0); setSend(0, 1, 'level', -12, 0); Aux.setRet(0, 0, -6); setMon('source', 1, 0);
    window.__doc = { version: 2, mapsDir: '', chains: [c] };
  }, CHAIN);
  await settle(150);
  let n = await mark();
  check(await page.evaluate(() => Sess.save('Tracking')), 'Sess.save resolves true');
  const file = await page.evaluate(() => window.__files['Tracking.json']);
  const xp = file && file.units.find(u => u.id === X8P.id), tp = file && file.units.find(u => u.id === TWX.id);
  check(file && file.format === 'apollo-control-session' && file.version === 1 && file.name === 'Tracking' && file.view.look === 'studio' && !!xp && !!tp,
    'the file: format, version 1, name, view, a part per unit');
  check(xp.inputs[0].gain === 40 && xp.inputs[1].p48 === true && xp.mix.main[0].level === -20 && xp.mix.cues[0].sends[1].level === -12 && xp.mix.aux[0].ret === -6 &&
    xp.monitor.source === 1 && xp.rate === 48000 && tp.inputs[0].gain === 30 && tp.inputs.length === 2,
    `x8p: preamps, monitor mix, cue send, aux return, monitor source, rate; Twin X: its two inputs (${JSON.stringify(xp.inputs[0])})`);
  check(xp.plugins.chains.length === 1 && xp.plugins.chains[0].slots[1].bypass === true && xp.plugins.chains[0].mon === true && tp.plugins.chains.length === 0,
    'plug-in chains: the engine\'s, per unit (slots, bypass, REC/MON, values) in the plugins.json v2 shape');
  check((await jsonSent(n, 'session-recent')).some(x => x.names[0] === 'Tracking'), 'the tray\'s recent list follows');

  // 2. Change everything, then load: the rate first and waited for, then preamps, mix, monitor; plug-ins last; 48V asks.
  await page.evaluate(() => {
    setInput(0, 'gain', 50, { unit: 0 }); setInput(1, '48v', false, { unit: 0, noLink: true }); setInput(0, 'gain', 20, { unit: 1 });
    setMix(0, 'level', -40, 0); setMix(3, 'mute', true, 0); setMon('source', 0, 0);
    window.__doc = { version: 2, mapsDir: '', chains: [] };
    window.__rates.SN22532017106180 = 96000;
  });
  await host({ type: 'rate', unit: X8P.id, present: true, rate: 96000, offered: [48000, 96000], allowed: [48000, 96000], combined: { member: false } });
  await settle(100);
  n = await mark();
  page.evaluate(() => Sess.loadByName('Tracking'));
  await page.locator('#dlg[open]').waitFor();
  check(/48V/.test(await dlgTitle()) && /Input 2/.test(await dlgTitle()) && (await hw(n)).length === 0, `48V would go on: asked first, naming the input, nothing sent yet ("${await dlgTitle()}")`);
  await answer('Load with 48V off');
  await settle(900);
  s = await hw(n);
  const at = re => s.findIndex(x => re.test(x));
  const iRate = at(/"cmd":"rate-set".*"rate":48000/), iFirst = at(/^(input|monitor)\t/), iPlug = at(/"cmd":"plug-session-set".*"unit":0/);
  check(iRate === 0 && iFirst > iRate, `the rate goes first (${s.slice(0, 2).join(' | ')})`);
  check(s.includes('input\t0\tgain\t40') && s.includes('input\t0\tgain\t30\t1') && s.includes('monitor\tmix:0\t-20') && s.includes('monitor\tmixmute:3\t0') &&
    s.includes('monitor\tsend:1:1\t-12') && s.includes('monitor\tsource\t1') && !s.some(x => /^input\t1\t48v\t1/.test(x)),
    'then the preamps, the mix (the input muted since is unmuted), the monitor; 48V left off as answered');
  const x8pSend = x => (/^monitor\t/.test(x) && x.split('\t').length === 3) || (/^input\t/.test(x) && x.split('\t').length === 4);
  check(iPlug > s.map(x8pSend).lastIndexOf(true) && iPlug > at(/^monitor\tsource\t1$/) &&
    (await jsonSent(n, 'plug-session-set')).some(x => x.unit === 0 && x.unitId === X8P.id && x.chains.length === 1 && x.chains[0].slots.length === 2),
    'the x8p\'s plug-in chains go to the engine after its preamps, mix and monitor');
  check(await page.evaluate(() => S.ctl[0].mon.source === 1 && cfg.mixes.x8p.main[0].level === -20 && !(cfg.mixes.x8p.main[3] || {}).mute && cfg.session.name === 'Tracking'),
    'the app shows the session: monitor source, the stored mix, the session name');

  // 3. 48V: Turn on sends it; Cancel sends nothing at all.
  n = await mark();
  page.evaluate(() => Sess.loadByName('Tracking'));
  await page.locator('#dlg[open]').waitFor();
  await answer('Turn on 48V');
  await settle(400);
  check((await hw(n)).includes('input\t1\t48v\t1'), 'Turn on 48V: input 2\'s phantom goes on');
  await page.evaluate(() => setInput(1, '48v', false, { unit: 0, noLink: true }));
  await settle(100);
  n = await mark();
  page.evaluate(() => Sess.loadByName('Tracking'));
  await page.locator('#dlg[open]').waitFor();
  await answer('Cancel');
  await settle(300);
  check((await hw(n)).length === 0, 'Cancel: nothing is sent');

  // 4. A unit that is not connected keeps its part and gets it when it connects.
  await host({ type: 'units', units: [X8P] });
  await host({ type: 'status', unit: 1, present: false });
  n = await mark();
  page.evaluate(() => Sess.loadByName('Tracking'));
  await page.locator('#dlg[open]').waitFor();
  await answer('Load with 48V off');
  await settle(500);
  s = await hw(n);
  const pendNow = await page.evaluate(id => !!cfg.session.pending[id], TWX.id);
  const toTwin = x => /\t1$/.test(x) && x.split('\t').length === (/^input\t/.test(x) ? 5 : /^monitor\t/.test(x) ? 4 : 0);
  check(!s.some(toTwin) && pendNow, `the Twin X is not there: nothing sent to it, its part waits (cfg.session.pending) (${s.filter(toTwin).join(' | ') || 'none sent'})`);
  await page.evaluate(() => Sess.open());
  check(/WAITING/.test(await page.locator('#sessPanel').textContent()), 'the session list says WAITING for it');
  await page.evaluate(() => Sess.close(false));
  n = await mark();
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  await settle(400);
  s = await hw(n);
  check(s.includes('input\t0\tgain\t30\t1') && await page.evaluate(id => !cfg.session.pending[id], TWX.id), `it connects: its part goes out, nothing waits any more (${s.filter(x => /\t1$/.test(x)).slice(0, 3).join(' | ')})`);

  // 5. The last state: on a reconnect it waits for the strips to be Live, then what does not read back goes out.
  await host({ type: 'live', unit: 0, engine: null, engineErr: 1, strips: { state: 4 }, endpoints: null, endpointsErr: 1 });
  await host({ type: 'status', present: false, error: 1167, detail: 'gone' });
  n = await mark();
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'live', unit: 0, engine: null, engineErr: 1, strips: { state: 3 }, endpoints: null, endpointsErr: 1 });
  const early = (await hw(n)).filter(x => /^monitor\t/.test(x)).length;
  await host({ type: 'live', unit: 0, engine: null, engineErr: 1, strips: { state: 4 }, endpoints: null, endpointsErr: 1 });
  s = (await hw(n)).filter(x => /^(monitor|input)\t/.test(x));
  check(early === 0 && s.includes('monitor\tmix:0\t-20') && s.includes('monitor\tsource\t1') && !s.some(x => /\tgain\t|\t48v\t/.test(x)),
    `reconnect: nothing while the strips build, then the mix and the monitor routing; preamps and 48V stay as the unit reads back (${s.length} sends)`);

  // 6. Typed: Mono's command line and the Ctrl+K palette.
  let r = await page.evaluate(() => Cmd.run('session save Mixdown'));
  check(r && r.ok && await page.evaluate(() => !!window.__files['Mixdown.json']), `session save Mixdown (${r && r.text})`);
  r = await page.evaluate(() => Cmd.run('session list'));
  check(r.ok && /Tracking/.test(r.text) && /\* Mixdown/.test(r.text), 'session list: both, the current one starred');
  await page.keyboard.press('Control+K');
  await page.locator('.pal-in').fill('load session');
  const pal = await page.locator('.pal-item').allTextContents();
  check(pal.some(t => /Load session: Mixdown/.test(t)) && pal.some(t => /Load session: Tracking/.test(t)), 'Ctrl+K: Load session: <name> for each');
  await page.keyboard.press('Escape');
  n = await mark();
  r = await page.evaluate(() => Cmd.run('session load Mixdown'));
  check(r.ok && (await jsonSent(n, 'session-load')).some(x => x.name === 'Mixdown'), 'session load Mixdown');

  // 7. The session button: Ctrl+O opens the list, Esc closes it and gives focus back.
  await page.locator('body').click({ position: { x: 5, y: 600 } });
  await page.keyboard.press('Control+O');
  check(await page.locator('#sessPanel').isVisible() && await page.locator('#sessPanel').getAttribute('data-layout') === 'console' &&
    await page.evaluate(() => document.activeElement && document.activeElement.closest('#sessPanel') !== null), 'Ctrl+O: the list opens (Studio: the console menu), focus in it');
  await page.keyboard.press('Escape');
  check(await page.locator('#sessPanel').isHidden() && await page.evaluate(() => document.activeElement.id) === 'sessBtn', 'Esc closes it, focus back on the Session button');

  // 8. Start with Windows: the toggle writes the Run value, again removes it; close to the tray follows by default.
  await page.evaluate(() => showView('settings'));
  const box = page.getByRole('checkbox', { name: 'Start Apollo Control when I sign in', exact: true });
  check(!(await box.isChecked()) && !(await box.isDisabled()), 'Settings: off, as the Run value says');
  n = await mark();
  await box.click();
  await settle(150);
  const prefsOn = (await since(n)).filter(x => /^prefs\t/.test(x)).pop();
  check(await page.evaluate(() => window.__run) === '"C:\\stub\\apollo-control.exe" --minimized' && await box.isChecked() && prefsOn === 'prefs\t1\t1' &&
    await page.getByRole('checkbox', { name: 'Close to the tray', exact: true }).isChecked(), `on: the Run value starts it minimized; closing goes to the tray (${prefsOn})`);
  n = await mark();
  await box.click();
  await settle(150);
  check(await page.evaluate(() => window.__run) === '' && !(await box.isChecked()) && (await since(n)).filter(x => /^prefs\t/.test(x)).pop() === 'prefs\t0\t0',
    'off again: the Run value is removed, closing quits');
  check((await jsonSent(0, 'autostart-set')).length === 2, 'the Run value was written only by the two clicks');

  // 9. The tray: volume up / down (2 dB, never past the limit), mute, dim, a recent session.
  await page.evaluate(() => showView('control'));
  const lvl = await page.evaluate(() => S.ctl[0].mon.level);
  n = await mark();
  await host({ type: 'tray', action: 'volup' });
  await settle(120);
  await host({ type: 'tray', action: 'voldown' });
  await host({ type: 'tray', action: 'voldown' });
  await settle(120);
  await host({ type: 'tray', action: 'mute' });
  await host({ type: 'tray', action: 'dim' });
  await settle(120);
  s = await hw(n);
  check(s.includes(`monitor\tlevel\t${lvl + 4}`) && s.includes(`monitor\tlevel\t${lvl - 4}`) && s.includes('monitor\tmute\t1') && s.includes('monitor\tdim\t1'),
    `tray: volume up and down by 2 dB, mute, dim (${s.join(' | ')})`);
  await page.evaluate(() => { S.ctl[0].mon.level = 192; cfg.safety.monitorLimitDb = 0; });
  n = await mark();
  await host({ type: 'tray', action: 'volup' });
  await settle(120);
  check(!(await hw(n)).some(x => /^monitor\tlevel\t19[3-9]/.test(x)), 'tray: volume up stops at the limit (0 dB)');
  n = await mark();
  await host({ type: 'tray', action: 'session', name: 'Tracking' });
  await settle(200);
  check((await jsonSent(n, 'session-load')).some(x => x.name === 'Tracking') && (await since(n)).includes('show'), 'tray: a recent session loads (the window shows for its 48V question)');
  if (await page.locator('#dlg[open]').count()) await answer('Cancel');

  // 10. Delete, from the command line, after asking.
  page.evaluate(() => Cmd.run('session delete Mixdown'));
  await page.locator('#dlg[open]').waitFor();
  await answer('Delete');
  await settle(200);
  check(await page.evaluate(() => !window.__files['Mixdown.json'] && cfg.session.name === ''), 'session delete: asked, removed, no current session');
  await ctx.close();

  /* ---------------- B. The demo: one model, a layout per theme ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=control');
  await demo.waitForTimeout(700);
  await demo.evaluate(async () => { Plug.demoFill(); await new Promise(r => setTimeout(r, 100)); await Sess.save('Tracking drums'); });
  const want = { studio: 'console', cyber: 'hud', lab: 'table', mono: 'terminal', day: 'plain', arcade: 'slots' };
  const seen = [];
  for (const [look, lay] of Object.entries(want)) {
    await demo.evaluate(l => { setLook(l); Sess.open(); }, look);
    const loadBtn = demo.locator('#sessPanel').getByRole('button', { name: /^(Load|Open) Tracking drums \(current\)/ });
    const ok = await demo.locator('#sessPanel').getAttribute('data-layout') === lay && await loadBtn.count() === 1 &&
      await demo.locator('#sessPanel').getByRole('button', { name: 'Save the session', exact: true }).count() >= 1;
    seen.push(`${look}:${ok ? lay : 'x'}`);
    await demo.evaluate(() => Sess.close(false));
  }
  check(seen.every(x => !/:x$/.test(x)), `every theme: its own layout, the same actions and names (${seen.join(' ')})`);
  const rt = await demo.evaluate(async () => {
    Plug.demoSessionSet([]);
    setInput(0, 'gain', 60, { unit: 0 });
    await new Promise(r => setTimeout(r, 400));          /* the demo answers each write after a random 40-120 ms */
    await Sess.loadByName('Tracking drums');
    await new Promise(r => setTimeout(r, 300));
    const st = Plug.state.sts[0];
    return { gain: S.ctl[0].inputs[0].gain, chains: (st.chains || []).filter(c => c.slots.length).length };
  });
  check(rt.gain === 38 && rt.chains === 3, `demo round trip: preamp and the three filled chains come back (${JSON.stringify(rt)})`);
  await demo.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
