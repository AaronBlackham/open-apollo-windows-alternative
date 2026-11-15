// [Endpoints] Settings, Windows audio, through a stub host (app/src/endpoints_host.h, docs/apollo-control-units.md 1.7):
// endpoints-get when Settings opens, a checkbox per row checked from `next`, a draft until Apply, endpoints-set with the
// unit's id and rows, pending then the host's words, "Use defaults" as default:true, a warning before dropping an endpoint
// an app has open, the Twin X under the combined set, and a unit that is not there.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'C:/Users/aaron/Desktop/Avium/Avium Invoid/node_modules/playwright');
const path = require('path');
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
let failed = 0;
const check = (c, l) => { if (!c) failed++; console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`); };

const X8P = { id: '000000003A00C0DE', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '22532017106180', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const TWX = { id: '000000002F00BEEF', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 };
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0, extCaps: 0, fwRelease: 0x0B0FD613, subsystemId: 0x14, deviceType: 0x22, publicType: 0x16,
  numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10, interruptCount: 0, notifStatusSeen: 0,
  numPreamps: 8, serial: X8P.serial, stage: 5, flags: 0x40 | 0x80 | 0x100 | 0x200 | 0x400, mode: 2, observe: false };
const row = (n, name, render, def, over = {}) => ({ row: n, name, circuit: `C${n}`, render, channels: render ? 2 : 1, model: 'x8p',
  enabled: def, added: def, default: def, next: def, open: false, running: false, noSlot: false, ...over });
const ROWS = [row(0, 'MON L/R', true, true, { open: true, running: true }), row(1, 'LINE 1-2', true, true), row(2, 'HP 1', true, true),
  row(3, 'Monitor mix', false, true), row(4, 'MIC/LINE 1', false, true), row(5, 'VIRTUAL 1-2', false, false)];
const ep = (over = {}) => ({ type: 'endpoints', unit: X8P.id, present: true, model: 'x8p', set: 'standalone', combinedOn: false, count: ROWS.length,
  mask: 31, defaultMask: 31, stored: null, storedIn: 'default', next: 31, restartNeeded: false, version: 2, rows: ROWS, ...over });

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
  const sent = () => page.evaluate(() => window.__sent.filter(s => /^\{"cmd":"endpoints-/.test(s)).map(s => JSON.parse(s)));
  const panel = page.locator('#epPanel');
  const box = name => panel.getByRole('checkbox', { name: new RegExp(`^${name.replace(/[/]/g, '.')}:`) });

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'control' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'units', units: [X8P] });
  await page.evaluate(() => showView('settings'));
  check((await sent()).some(x => x.cmd === 'endpoints-get'), 'endpoints-get when Settings opens');
  check(/Asking the driver/.test(await panel.textContent()), 'before an answer: "Asking the driver..."');

  await host(ep());
  check(await panel.locator('.ep-row').count() === 6, 'a checkbox per row (6)');
  check(await box('LINE 1-2').isChecked() && !(await box('VIRTUAL 1-2').isChecked()), 'checked from next: LINE 1-2 on, VIRTUAL 1-2 off');
  check(/Playing/i.test(await panel.locator('.ep-row[data-row="0"]').textContent()) && /Not in the defaults/i.test(await panel.locator('.ep-row[data-row="5"]').textContent()),
    'row words: an app plays through MON L/R, VIRTUAL 1-2 is not in the defaults');
  const apply = panel.getByRole('button', { name: 'Apply' });
  check(await apply.isDisabled(), 'Apply waits for a change');

  await box('HP 1').uncheck();
  await box('VIRTUAL 1-2').check();
  check(!(await apply.isDisabled()) && /Removed at next start/i.test(await panel.locator('.ep-row[data-row="2"]').textContent()), 'a draft: Apply on, HP 1 "removed at next start"');
  await apply.click();
  let set = (await sent()).filter(x => x.cmd === 'endpoints-set');
  check(set.length === 1 && set[0].unit === X8P.id && JSON.stringify(set[0].rows) === '[0,1,3,4,5]', `endpoints-set with the unit's id and rows (${JSON.stringify(set)})`);
  await host({ type: 'endpoints-result', unit: X8P.id, pending: true, code: '', detail: 'asking for administrator rights to store the endpoint set', restartNeeded: true });
  check(await box('LINE 1-2').isDisabled() && /asking for administrator rights/.test(await panel.textContent()), 'pending: boxes locked, the host\'s words');
  await host({ type: 'endpoints-result', unit: X8P.id, pending: false, ok: false, code: 'cancelled', detail: 'declined', restartNeeded: false });
  check(/administrator prompt was declined: nothing changed/.test(await panel.textContent()) && !(await box('LINE 1-2').isDisabled()) && !(await box('HP 1').isChecked()),
    'cancelled: said so, the draft kept');
  const next = ROWS.map(r => ({ ...r, next: [0, 1, 3, 4, 5].includes(r.row) }));
  await host({ type: 'endpoints-result', unit: X8P.id, pending: false, ok: true, code: '', detail: 'stored; Windows shows the new set after the unit\'s next start', restartNeeded: true });
  await host(ep({ rows: next, stored: 59, storedIn: 'unit', next: 59, restartNeeded: true }));
  check(await apply.isDisabled() && /next start/.test(await panel.locator('.ep-status').textContent()), 'stored: no draft left, "after the unit\'s next start"');

  await box('MON L/R').uncheck();
  await apply.click();
  await page.waitForTimeout(100);
  check(await page.evaluate(() => document.getElementById('dlg').open) && /MON L\/R/.test(await page.locator('#dlg').textContent()), 'dropping an endpoint an app has open: asks first, names it');
  await page.keyboard.press('Escape');
  await page.waitForTimeout(100);
  check((await sent()).filter(x => x.cmd === 'endpoints-set').length === 1, 'declined: nothing sent');
  await panel.getByRole('button', { name: 'Undo changes' }).click();
  await panel.getByRole('button', { name: 'Use defaults' }).click();
  set = (await sent()).filter(x => x.cmd === 'endpoints-set');
  check(set.length === 2 && set[1].default === true && set[1].unit === X8P.id, 'Use defaults: default:true');

  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'endpoints', unit: TWX.id, present: true, model: 'twinx', set: 'none', combinedOn: true, count: 0, mask: 0, defaultMask: 0, stored: null,
    storedIn: 'default', next: 0, restartNeeded: false, version: 2, rows: [] });
  check(/part of the combined set/.test(await panel.locator('.ep-unitblock[data-unit="000000002F00BEEF"]').textContent()), 'Twin X under the combined set: change them on the x8p');
  await host({ type: 'endpoints', unit: TWX.id, present: false, error: 2, detail: 'that unit is not on apollo.sys right now' });
  check(/not on apollo\.sys right now/.test(await panel.locator('.ep-unitblock[data-unit="000000002F00BEEF"]').textContent()), 'a unit that is not there: the host\'s words');

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
  if (errors.length || failed) process.exitCode = 1;
})();
