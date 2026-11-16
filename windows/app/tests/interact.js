// Drives the demo UI: 48V confirm, link copy, mute, knob keys, preset save/recall; checks state.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const url = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href + '?view=control';

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 800 } });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(url);
  await page.waitForTimeout(800);
  const st = () => page.evaluate(() => ({ inputs: S.inputs.map(i => ({ ...i })), links: [...S.links], mon: { ...S.mon }, pending: [...S.pending.keys()] }));
  const check = (cond, label) => console.log(`${cond ? 'PASS' : 'FAIL'}  ${label}`);

  // 48V on input 5: dialog appears, confirm turns it on (and readback confirms it).
  const strip5 = page.locator('.strip[data-ch="4"]');
  await strip5.getByRole('button', { name: '48V' }).click();
  await page.waitForSelector('#dlg[open]');
  await page.screenshot({ path: path.join(out, 'ui-48v-dialog.png') });
  await page.getByRole('button', { name: 'Turn on 48V' }).click();
  await page.waitForTimeout(400);
  let s = await st();
  check(s.inputs[4].p48 === true && !s.pending.includes('in:4:p48'), '48V confirm turns input 5 on and readback confirms');

  // Cancel path leaves it off.
  await page.locator('.strip[data-ch="5"]').getByRole('button', { name: '48V' }).click();
  await page.getByRole('button', { name: 'Cancel' }).click();
  await page.waitForTimeout(300);
  s = await st();
  check(s.inputs[5].p48 === false, '48V cancel leaves input 6 off');

  // Gain by keyboard on input 1: focus knob, PageUp = +6 dB.
  const before = s.inputs[0].gain;
  await page.getByRole('slider', { name: 'Input 1 gain', exact: true }).focus();
  await page.keyboard.press('PageUp');
  await page.waitForTimeout(400);
  s = await st();
  check(s.inputs[0].gain === before + 6 && !s.pending.includes('in:0:gain'), `gain PageUp ${before} -> ${s.inputs[0].gain}, confirmed`);

  // Link 7-8 copies 7's settings to 8.
  await page.locator('.strip[data-ch="6"]').getByRole('button', { name: 'PAD' }).click();
  await page.locator('.pair').nth(3).getByRole('button', { name: 'LINK' }).click();
  await page.waitForTimeout(400);
  s = await st();
  check(s.links[3] && s.inputs[7].pad === true, 'link 7-8 copies PAD from 7 to 8');
  await page.locator('.strip[data-ch="7"]').getByRole('button', { name: 'LO CUT' }).click();
  await page.waitForTimeout(400);
  s = await st();
  check(s.inputs[6].lowcut && s.inputs[7].lowcut, 'linked pair: LO CUT on 8 also sets 7');

  // Mute by keyboard shortcut.
  await page.locator('.brand').click();   // focus off any control (not a fixed spot in the tab bar, which grows with tabs)
  await page.keyboard.press('m');
  await page.waitForTimeout(300);
  s = await st();
  check(s.mon.mute === true, 'M mutes the monitor');
  await page.keyboard.press('m');

  // Monitor knob respects the limit: set -20 dB limit in settings, then End key.
  await page.evaluate(() => { cfg.safety.monitorLimitDb = -20; renderMonitor(); });
  await page.getByRole('slider', { name: 'Monitor level', exact: true }).focus();
  await page.keyboard.press('End');
  await page.waitForTimeout(300);
  s = await st();
  check(s.mon.level === 192 - 40, `monitor End stops at the limit (raw ${s.mon.level})`);

  // Save and recall a preset.
  await page.getByRole('button', { name: 'Presets' }).click();
  await page.getByRole('menuitem', { name: 'Save current as preset' }).click();
  await page.locator('#dlg input').fill('Podcast');
  await page.getByRole('button', { name: 'Save', exact: true }).click();
  await page.waitForTimeout(200);
  await page.getByRole('button', { name: 'Input 3 polarity invert' }).click();
  await page.waitForTimeout(300);
  check((await st()).inputs[2].phase === true, 'polarity on input 3 turns on');
  await page.waitForTimeout(300);
  await page.getByRole('button', { name: 'Presets' }).click();
  await page.getByRole('menuitem', { name: /Podcast/ }).click();
  await page.waitForTimeout(500);
  s = await st();
  check(s.inputs[2].phase === false, 'recalling the preset undoes the later polarity change');

  await page.screenshot({ path: path.join(out, 'ui-after-interact.png') });
  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
