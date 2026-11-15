// [Themes] The six signature features (docs/THEMES.md 3, step 4), light checks in demo mode: each shows only in its own
// theme's composition, Mono's command line runs commands, Ctrl+K opens the palette in another theme, HI-SCORE fills from
// the levels, Day's guided gain starts from the focus view, and Lab's Avium Measure hand-off posts the routing to the host.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'C:/Users/aaron/Desktop/Avium/Avium Invoid/node_modules/playwright');
const path = require('path');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const page = await browser.newPage({ viewport: { width: 1536, height: 864 } });
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(ui + '?view=control');
  await page.waitForTimeout(600);

  const SIG = { studio: '#meterBridge', arcade: '#hiScore', lab: '#labBench', cyber: '.flow-canvas', mono: '#cmdLine' };
  for (const look of ['studio', 'cyber', 'lab', 'mono', 'day', 'arcade']) {
    await page.evaluate(l => setLook(l), look);
    await page.waitForTimeout(200);
    const shown = [];
    for (const [id, sel] of Object.entries(SIG)) if (await page.locator(sel).isVisible()) shown.push(id);
    check(shown.join() === (SIG[look] ? look : ''), `${look}: only its own signature shows (${shown.join() || 'none'})`);
  }
  await page.waitForTimeout(400);
  check(await page.locator('#hiScore .hs-row:not(.hs-empty)').count() > 0, 'HI-SCORE fills from the levels, with a time');

  await page.evaluate(() => setLook('mono'));
  const cl = page.getByRole('textbox', { name: 'Command line (type help)' });
  for (const c of ['in 3 gain 32', 'x8p in 5-6 link', 'in 2 pad on', 'unknownthing']) { await cl.fill(c); await cl.press('Enter'); await page.waitForTimeout(80); }
  const st = await page.evaluate(() => ({ g: ctl(0).inputs[2].gain, l: ctl(0).links[2], p: ctl(0).inputs[1].pad }));
  check(st.g === 32 && st.l === true && st.p === true, `the command line: gain, link (with a unit word), pad (${JSON.stringify(st)})`);
  check(/Unknown command "unknownthing"/.test(await page.locator('#cmdLine .cl-out').textContent()), 'a wrong command says so in the log tail');
  await cl.fill('in 1 lo');
  await cl.press('Tab');
  check(await cl.inputValue() === 'in 1 lowcut ', 'Tab completes');
  await cl.fill('');
  await cl.press('ArrowUp');
  check(await cl.inputValue() === 'unknownthing', 'Up: history');

  await page.evaluate(() => setLook('studio'));
  await page.locator('body').click({ position: { x: 5, y: 860 } });
  await page.keyboard.press('Control+k');
  const pal = page.getByRole('dialog', { name: 'Command palette' });
  check(await pal.isVisible(), 'Ctrl+K opens the palette in Studio');
  await page.keyboard.type('theme lab');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(150);
  check(await page.evaluate(() => cfg.appearance.look) === 'lab' && !(await pal.count()), 'the palette runs a typed command and closes');

  await page.evaluate(() => { setLook('day'); focusStrip(0, 3); });
  await page.locator('#inputs .strip.focus .st-guide').click();
  check(/Speak or play normally/.test(await page.locator('#inputs .strip.focus .st-guide-box').textContent()), 'Day: guided gain asks for 5 seconds of sound');
  await page.evaluate(() => setLook('studio'));
  await page.waitForTimeout(100);
  check(await page.locator('.st-guide-box').count() === 0, 'leaving Day closes guided gain');
  await page.close();

  // Lab's hand-off through a stub host: measure-open with the routing
  const ctx = await browser.newContext({ viewport: { width: 1536, height: 864 } });
  await ctx.addInitScript(() => { window.__sent = []; window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } }; });
  const p2 = await ctx.newPage();
  p2.on('pageerror', e => errors.push(e.message));
  await p2.goto(ui);
  await p2.evaluate(() => onHostMessage({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' }));
  await p2.evaluate(() => Sig.measureOpen());
  const m = await p2.evaluate(() => window.__sent.filter(s => /^\{"cmd":"measure-open"/.test(s)).map(s => JSON.parse(s)));
  check(m.length === 1 && m[0].routing && m[0].routing.format === 'apollo-routing' && Array.isArray(m[0].routing.units), 'Lab: measure-open posts the routing');
  await p2.evaluate(() => onHostMessage({ type: 'measure-result', ok: true, launched: false, file: 'C:\\x\\measure-handoff.json', why: 'Avium Measure is not installed here' }));
  check(/not installed here/.test(await p2.locator('#toasts').textContent()), 'the host\'s answer is said');

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
