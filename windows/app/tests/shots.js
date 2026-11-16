// Screenshots of the themes (not a test): per theme, the Control view at 1920 x 1080 and 125 % zoom (1536 x 864 CSS px)
// in demo mode with the x8p and the Twin X side by side, the plug-in browser, filled insert chains (docs 1.9), the AUX
// strips, an insert's parameter view open, and the theme's style-guide page. node shots.js <folder> (default: tests\shots).
// The demo's plug-ins are invented and their previews are drawn placeholders: nothing from a UAD install is in these.
// [Sess] Then the session list per theme (session-<theme>.png): three saved sessions, the current one edited since its
// save, the autosaved last state. node shots.js <folder> sessions takes only those.
// Classic (the default look) at 1240 x 860, a fresh config as a new user sees it: node shots.js <folder> classic writes
// classic-*.png only (the x8p side of the strips, the Twin X side, filled chains with the AUX strips, an insert's
// parameter view, the style guide, Settings > Appearance, the session list).
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, 'shots');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
fs.mkdirSync(out, { recursive: true });

async function classicShots(browser) {
  const page = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  const errors = [], files = [];
  page.on('pageerror', e => errors.push(e.message));
  const shot = async (name, settle = 300) => {
    await page.waitForTimeout(settle);
    await page.evaluate(() => document.getElementById('toasts').replaceChildren());
    files.push(path.join(out, `classic-${name}.png`));
    await page.screenshot({ path: files[files.length - 1] });
  };
  await page.goto(ui + '?view=control');                 /* no saved config: the default look */
  await page.evaluate(() => document.fonts.ready);
  await page.mouse.move(5, 500);
  await shot('control', 1500);                           /* meters settle */
  await page.evaluate(() => { document.getElementById('inputs').scrollLeft = 1e5; });
  await shot('control-twinx');
  await page.evaluate(() => {
    document.getElementById('inputs').scrollLeft = 0;
    Plug.demoFill();
    setAuxOpen(true);
    Aux.setSend(0, 0, 0, 'level', -12); Aux.setSend(0, 0, 1, 'level', -20); Aux.setSend(0, 1, 0, 'level', -30);
  });
  await page.mouse.move(5, 500);
  await shot('control-inserts', 600);
  await page.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="1"]').click();
  await page.mouse.move(5, 500);
  await shot('control-params', 400);
  await page.evaluate(() => { Plug.openDrawer(null); setAuxOpen(false); showView('styleguide'); });
  await shot('styleguide', 500);
  await page.evaluate(() => { document.getElementById('view-styleguide').scrollTop = 1e5; });
  await shot('styleguide-2');
  await page.evaluate(() => { showView('settings'); const l = document.querySelector('.look-list'); if (l) l.closest('.panel').scrollIntoView(); });
  await shot('settings');
  await page.evaluate(() => { showView('control'); Sess.open(); });
  await shot('sessions', 500);
  await page.evaluate(() => Sess.close(false));
  await page.close();
  return { files, errors };
}

(async () => {
  const browser = await chromium.launch();
  if (process.argv[3] === 'classic') {
    const r = await classicShots(browser);
    await browser.close();
    console.log(r.files.join('\n'));
    if (r.errors.length) console.log('ERRORS:\n' + r.errors.join('\n'));
    return;
  }
  const page = await browser.newPage({ viewport: { width: 1536, height: 864 }, deviceScaleFactor: 1.25 });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(ui + '?view=control');
  await page.waitForTimeout(800);
  /* [Ins] filled chains (input 1: three inserts, the EQ bypassed; input 2: two, MON; AUX 1: one), aux sends and a cue
     send, the AUX strips shown; then an insert on input 5 by keyboard, as a user would, and input 1's compressor open */
  await page.evaluate(() => {
    Plug.demoFill();
    setAuxOpen(true);
    Aux.setSend(0, 0, 0, 'level', -12); Aux.setSend(0, 0, 1, 'level', -20); Aux.setSend(0, 1, 0, 'level', -30); Aux.setCue(0, 0, 0, 'level', -6);
  });
  await page.waitForTimeout(200);
  await page.locator('#plb .plb-item[data-name="Demo EQ"]').focus();
  await page.keyboard.press('Enter');
  await page.locator('#inputs .ins-slot[data-unit="0"][data-ch="4"][data-n="0"]').focus();
  await page.keyboard.press('Enter');
  await page.waitForTimeout(200);
  await page.locator('#inputs .ins-slot[data-unit="0"][data-ch="0"][data-n="1"]').click();
  await page.mouse.move(5, 500);
  const files = [];
  const themes = await page.evaluate(() => LOOKS.map(l => l.id));
  const only = process.argv[3] || '';
  for (const look of only === 'sessions' ? [] : themes) {
    await page.evaluate(l => { setLook(l); showView('control'); }, look);
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(1200);                     /* meters settle */
    await page.evaluate(() => document.getElementById('toasts').replaceChildren());
    files.push(path.join(out, `control-${look}.png`));
    await page.screenshot({ path: files[files.length - 1] });
    await page.evaluate(() => showView('styleguide'));
    await page.waitForTimeout(500);
    files.push(path.join(out, `styleguide-${look}.png`));
    await page.screenshot({ path: files[files.length - 1], fullPage: false });
    await page.evaluate(() => { document.getElementById('view-styleguide').scrollTop = 10000; });
    await page.waitForTimeout(150);
    files.push(path.join(out, `styleguide-${look}-2.png`));
    await page.screenshot({ path: files[files.length - 1] });
    await page.evaluate(() => { document.getElementById('view-styleguide').scrollTop = 0; });
  }
  /* [Sess] the session list in each theme's layout */
  await page.evaluate(async () => {
    setLook('studio');
    showView('control');
    Plug.openDrawer(null);
    await Sess.save('Vocal overdubs');
    setInput(0, 'gain', 44, { unit: 0 });
    setMix(2, 'level', -8, 0);
    await Sess.save('Live room, full band');
    setInput(0, 'gain', 38, { unit: 0 });
    setMix(2, 'level', 0, 0);
    await Sess.save('Tracking drums');
    setMix(1, 'level', -12, 0);                          /* edited since its save */
  });
  await page.waitForTimeout(6500);                       /* the last state autosaves a few seconds after a change */
  for (const look of themes) {
    await page.evaluate(l => { setLook(l); showView('control'); Sess.open(); }, look);
    await page.evaluate(() => document.fonts.ready);
    await page.waitForTimeout(900);
    await page.evaluate(() => document.getElementById('toasts').replaceChildren());
    files.push(path.join(out, `session-${look}.png`));
    await page.screenshot({ path: files[files.length - 1] });
    await page.evaluate(() => Sess.close(false));
  }
  await page.evaluate(() => { setLook('studio'); showView('control'); });
  await browser.close();
  console.log(files.join('\n'));
  if (errors.length) console.log('ERRORS:\n' + errors.join('\n'));
})();
