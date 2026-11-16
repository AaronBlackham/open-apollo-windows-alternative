// Units & routing: host-style "units" and "agg" messages (a stub WebView2 host records what the UI
// sends), unit added/removed notices and Status sections, the clock master default, presets,
// Apply with the elevation and pending/error states, validation, forget, slot gaps, the host's
// "not available" agg reply, the demo scenarios, and the layout at 980 px.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const ui = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href;
const check = (c, l) => console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`);

const X8P = { id: 'SN20000000000001', index: 0, model: 'Apollo x8p', deviceType: 34, serial: '20000000000001', fw: '11.15.54803', mode: 2, present: true, active: true };
const TWX = { id: '7C1E40A2D3F09B61', index: 1, model: 'Apollo Twin X', deviceType: 153, serial: '2253202500117', fw: '11.15.54803', mode: 2, present: true, active: true };
const INFO = { version: 6, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613, subsystemId: 0x14,
  deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192, sampleRate: 48000, servicePasses: 10,
  interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: X8P.serial, stage: 5, flags: 0x40 | 0x80 | 0x100 | 0x200 | 0x400 };
const TW_INFO = { ...INFO, fpgaRev: 0, extCaps: 0x81300410, subsystemId: 0x19, deviceType: 0x99, publicType: 0x19, numDsps: 4, playChannels: 10,
  recChannels: 24, numPreamps: 2, serial: TWX.serial };
const engine = fps => ({ version: 1, running: 1, lost: 0, starts: 1, ticks: 5000, lateTicks: 0, busStalls: 0, deadReads: 0, maxIntervalUs: 1100,
  asioState: 0, asioFrames: 0, asioIn: 0, asioOut: 0, asioUnderruns: 0, asioOverruns: 0, asioSkipped: 0, ticksPerSec: 1000.0, framesPerSec: fps });

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const watch = p => {
    p.on('pageerror', e => errors.push(e.message));
    p.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  };

  /* ---------------- A. Host path ---------------- */
  const ctx = await browser.newContext({ viewport: { width: 1240, height: 860 } });
  await ctx.addInitScript(() => {
    window.__sent = [];
    window.chrome = { webview: { postMessage: m => window.__sent.push(m), addEventListener: () => {} } };
  });
  const page = await ctx.newPage();
  watch(page);
  await page.goto(ui);
  const host = m => page.evaluate(x => onHostMessage(x), m);
  const cmds = () => page.evaluate(() => window.__sent.filter(s => s.startsWith('{')).map(s => JSON.parse(s)));
  const toastText = () => page.locator('.toast').allTextContents().then(t => t.join(' | '));
  const clearToasts = () => page.evaluate(() => document.getElementById('toasts').replaceChildren());
  const rows = page.locator('.ur-units tbody tr');
  const cell = name => page.getByRole('button', { name, exact: true });
  const pressed = async name => (await cell(name).getAttribute('aria-pressed')) === 'true';
  const bar = page.locator('#urBar');
  const barText = () => bar.textContent();
  const applyBtn = bar.getByRole('button', { name: 'Apply' });
  const revertBtn = bar.getByRole('button', { name: 'Revert' });
  const master = () => page.evaluate(() => (document.querySelector('input[name="urMaster"]:checked') || {}).value || '');

  await host({ type: 'hello', app: 'test', config: null, demo: false, view: 'units' });
  await host({ type: 'status', present: true, info: INFO });
  await host({ type: 'live', unit: 0, engine: engine(47999.85), strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 });

  // 1. The x8p alone; the UI asks the host for the driver's configuration.
  await host({ type: 'units', units: [X8P] });
  check((await cmds()).some(c => c.cmd === 'agg-get'), 'UI sends {"cmd":"agg-get"} as a JSON string');
  check(await rows.count() === 1, 'one unit row');
  await host({ type: 'agg', config: null, pending: false, error: null, needsAdmin: true, elevated: false });
  check(/not saved in the driver yet/.test(await barText()), 'nothing saved: the bar offers to save the layout');
  check(/needs administrator rights/.test(await barText()), 'elevation note shown');

  // 2. Twin X plugged in: notice, row, Status section, master defaults to the Twin X.
  await clearToasts();
  await host({ type: 'units', units: [X8P, TWX] });
  await host({ type: 'status', unit: 1, present: true, info: TW_INFO });
  const tw = new Array(40).fill(0); tw[0] = 1 << 6; tw[3] = 21 | (1 << 6); tw[6] = (1 << 30) | (1 << 31); tw[38] = 0x00010203;
  await host({ type: 'readback', unit: 1, valid: true, seq: 3, words: tw.map(x => x >>> 0) });
  await host({ type: 'live', unit: 1, engine: engine(48000.62), strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 });
  check(/Apollo Twin X connected/.test(await toastText()), `notice: "${await toastText()}"`);
  check(await rows.count() === 2, 'two unit rows');
  check(await master() === TWX.id, 'clock master defaults to the Twin X');
  check(await page.evaluate(() => S.host.info.deviceType) === 0x22, 'the Twin X status did not replace the x8p status');
  await page.waitForTimeout(1200);
  const drift = await rows.nth(1).locator('td').nth(7).textContent();
  check(/^-16\.\d ppm$/.test(drift), `x8p drift against the Twin X: "${drift}"`);
  check(await rows.nth(0).locator('td').nth(5).textContent() === 'locked', 'Twin X clock locked from readback word 6');
  check(await pressed('DAW mains to TwinX MON') && await pressed('Windows audio to TwinX MON') && !(await pressed('DAW mains to x8p MON')),
    'Studio: DAW mains and Windows to the Twin X MON');
  await page.evaluate(() => showView('status'));
  const tsec = page.locator('.unit[data-unit="1"]');
  check(await tsec.count() === 1 && await tsec.locator('.unit-name').textContent() === 'Apollo Twin X', 'Status: a Twin X section appears');
  check(await tsec.locator('.st-inputs tbody tr').count() === 2, 'Status: two Twin X preamps');
  check(await tsec.locator('.st-inputs tbody tr').nth(0).locator('td').nth(6).textContent() === '30 dB' &&
    await tsec.locator('.st-inputs tbody tr').nth(1).locator('td').nth(5).textContent() === 'LINE', 'Status: Twin X preamps from its own readback');
  check(await page.locator('.unit[data-unit="0"] .st-inputs tbody tr').count() === 8, 'Status: x8p section intact');
  await page.evaluate(() => showView('units'));
  await page.setViewportSize({ width: 1240, height: 2150 });
  await page.screenshot({ path: path.join(out, 'ui-units-host.png') });
  await page.setViewportSize({ width: 1240, height: 860 });

  // 3. Presets fill the grid.
  await page.getByRole('button', { name: 'Mirror', exact: true }).click();
  check(await pressed('DAW mains to TwinX MON') && await pressed('DAW mains to x8p MON') &&
    await pressed('Windows audio to TwinX MON') && await pressed('Windows audio to x8p MON'), 'Mirror: both MONs get DAW mains and Windows');
  await page.getByRole('button', { name: 'Headphones split', exact: true }).click();
  check(await pressed('Windows audio to TwinX HP') && !(await pressed('Windows audio to TwinX MON')), 'Headphones split: Windows to the Twin X headphones');
  await page.getByRole('button', { name: 'Studio', exact: true }).click();
  check(!(await pressed('DAW mains to x8p MON')) && await pressed('ASIO out 11-12 to x8p MON'), 'Studio again; the x8p MON pair (ASIO 11-12) keeps its own output');

  // 4. Apply: elevation, then pending, then the prompt cancelled, then applied.
  await applyBtn.click();
  let set = (await cmds()).filter(c => c.cmd === 'agg-set');
  check(set.length === 1 && set[0].config.master === TWX.id && set[0].config.units.map(u => u.id).join() === `${TWX.id},${X8P.id}`,
    'agg-set carries the config: Twin X master and first');
  check(set[0].config.routes.some(r => r.src === 'daw' && r.unit === TWX.id && r.port === 'mon' && r.ids.join() === '2305,2306'), 'route names the MON channel IDs 0x0901/0x0902');
  check(await bar.getAttribute('data-state') === 'busy' && /Waiting for administrator approval/.test(await barText()), 'Apply: waiting for approval');
  check(await applyBtn.isDisabled() && await revertBtn.isDisabled(), 'Apply and Revert locked while it runs');
  await host({ type: 'agg', pending: true, detail: 'Applying: the elevated helper is writing the configuration.' });
  check(/elevated helper is writing/.test(await barText()), 'pending reply shown');
  await host({ type: 'agg', pending: false, error: { code: 1223, detail: 'cancelled' } });
  check(await bar.getAttribute('data-state') === 'error' && /Not applied: the administrator prompt was cancelled/.test(await barText()), 'error reply shown');
  check(!(await applyBtn.isDisabled()), 'Apply can be retried');
  await applyBtn.click();
  set = (await cmds()).filter(c => c.cmd === 'agg-set');
  await host({ type: 'agg', config: set[1].config, pending: false, error: null, needsAdmin: true });
  check(await bar.getAttribute('data-state') === 'clean' && /Saved in the driver/.test(await barText()), 'applied: saved, nothing to apply');
  check(/Combined device updated/.test(await toastText()), 'applied notice');

  // 5. Validation: nothing routed blocks Apply.
  const on = page.locator('.rt[aria-pressed="true"]');
  for (let i = 0; i < 60 && await on.count(); i++) await on.first().click();
  check(/Nothing is routed to any output/.test(await page.locator('.ur-issues').textContent()), 'nothing routed: error shown');
  check(await applyBtn.isDisabled(), 'nothing routed: Apply disabled');
  await revertBtn.click();
  check(await bar.getAttribute('data-state') === 'clean', 'Revert restores the saved routing');

  // 6. Master change warns about the ASIO reset; channel blocks reorder.
  await page.locator(`input[name="urMaster"][value="${X8P.id}"]`).click();
  check(/Changing the clock master resets ASIO clients/.test(await page.locator('.ur-clock').textContent()), 'master change: ASIO reset warning');
  check(/clock master Apollo Twin X → Apollo x8p/.test(await barText()) && /Windows streams get one short gap/.test(await barText()), 'bar lists the master change and what it interrupts');
  await page.getByRole('button', { name: 'Move Apollo Twin X down' }).click();
  check(/^1x8p MIC\/LINE 1$/.test(await page.locator('.ur-list').first().locator('li').first().textContent()), 'simple order: the x8p block first');
  check(/ASIO out 1-2 · x8p MON/.test(await page.locator('.ur-grid tbody tr').first().textContent()), 'DAW mains follow the new first block');
  await revertBtn.click();

  // 7. Twin X unplugged: notice, section gone, config kept greyed with Forget.
  await clearToasts();
  await host({ type: 'units', units: [X8P] });
  check(/Apollo Twin X disconnected/.test(await toastText()), 'notice: disconnected');
  const overlap = await page.evaluate(() => {
    const a = document.querySelector('.toast').getBoundingClientRect();
    const b = [...document.querySelectorAll('#urBar button')].map(n => n.getBoundingClientRect());
    return b.some(r => a.left < r.right && r.left < a.right && a.top < r.bottom && r.top < a.bottom);
  });
  check(!overlap, 'the notice sits above the Apply bar, not over its buttons');
  const gone = rows.filter({ hasText: 'Apollo Twin X' });
  check(await gone.count() === 1 && (await gone.getAttribute('class')).includes('absent') && /not connected/.test(await gone.textContent()), 'Twin X kept, greyed, not connected');
  check(await gone.getByRole('button', { name: /^Forget Apollo Twin X/ }).count() === 1, 'Forget offered');
  check(/acting master/.test(await page.locator('.ur-clock').textContent()), 'x8p acting master while the Twin X is away');
  check(await bar.getAttribute('data-state') === 'clean', 'unplugging changes nothing saved');
  await host({ type: 'live', unit: 1, engine: engine(48000.6), strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 });
  await page.evaluate(() => showView('status'));
  check(await page.locator('.unit[data-unit="1"]').count() === 0, 'Status: Twin X section gone, late live dropped');
  check(await page.locator('#units > .unit').count() === 1, 'Status: only the x8p');
  await page.evaluate(() => showView('units'));
  await page.setViewportSize({ width: 1240, height: 2150 });
  await page.screenshot({ path: path.join(out, 'ui-units-unplugged.png') });
  await page.setViewportSize({ width: 1240, height: 860 });

  // 8. Forget is explicit and goes through Apply; Revert brings it back.
  await gone.getByRole('button', { name: /^Forget Apollo Twin X/ }).click();
  await page.locator('#dlg .btn-danger').click();
  check(await rows.filter({ hasText: 'Apollo Twin X' }).count() === 0 && /forget Apollo Twin X/.test(await barText()), 'forget: removed from the draft, Apply pending');
  await revertBtn.click();
  check(await rows.filter({ hasText: 'Apollo Twin X' }).count() === 1, 'Revert keeps the saved Twin X');

  // 9. Back in observe mode: read-only in Status, flagged in routing.
  await clearToasts();
  await host({ type: 'units', units: [X8P, { ...TWX, mode: 0, active: false }] });
  await host({ type: 'status', unit: 1, present: true, info: { ...TW_INFO, stage: 1, flags: 0x1, sampleRate: 0 } });
  check(/Apollo Twin X connected/.test(await toastText()), 'notice: reconnected');
  check(/observe, read-only/.test(await rows.filter({ hasText: 'Apollo Twin X' }).textContent()), 'Units: observe, read-only');
  check(/is in observe mode: it plays nothing/.test(await page.locator('.ur-issues').textContent()), 'routes to an observe unit are flagged');
  await page.evaluate(() => showView('status'));
  check(/observe mode, read-only/.test(await page.locator('.unit[data-unit="1"] .unit-meta').textContent()), 'Status: Twin X read-only below Mode 2');

  // 10. Advanced order: hide a channel, the numbering closes up.
  await page.evaluate(() => showView('units'));
  await page.getByRole('button', { name: 'Advanced', exact: true }).click();
  await page.getByRole('checkbox', { name: 'Show TwinX ADAT 1' }).uncheck();
  check(/55 in · 44 out/.test(await page.locator('.panel', { hasText: 'Channel order' }).locator('.panel-head').textContent()), 'hiding a channel: 55 inputs');
  await page.getByRole('button', { name: 'Simple', exact: true }).click();
  await page.locator('#dlg .btn-primary').click();
  check(/56 in · 44 out/.test(await page.locator('.panel', { hasText: 'Channel order' }).locator('.panel-head').textContent()), 'back to simple: every channel');
  check(await bar.getAttribute('data-state') === 'clean', 'back to simple: nothing left to apply');

  // 11. Slot numbers can have gaps (the host keeps slot 0 for the x8p, others take the lowest free
  //     of 1-3): the Twin X back in full mode in slot 2, slot 1 empty.
  const TWX2 = { ...TWX, index: 2 };
  await host({ type: 'status', unit: 2, present: true, info: TW_INFO });
  await host({ type: 'units', units: [X8P, TWX2] });
  await host({ type: 'readback', unit: 2, valid: true, seq: 4, words: tw.map(x => x >>> 0) });
  await host({ type: 'live', unit: 2, engine: engine(48000.62), strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 });
  await host({ type: 'live', unit: 1, engine: engine(48000.62), strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 });
  await page.waitForTimeout(1200);                  /* the clock and drift cells refresh once a second */
  const twRow = rows.filter({ hasText: 'Apollo Twin X' });
  check(await rows.count() === 2 && /full/.test(await twRow.textContent()) && await twRow.locator('td').nth(5).textContent() === 'locked',
    'slot 2: Twin X row in full mode, clock from its own readback');
  await page.evaluate(() => showView('status'));
  check(await page.locator('.unit[data-unit="2"]').count() === 1 && await page.locator('.unit[data-unit="1"]').count() === 0,
    'Status: a section for slot 2, none for the empty slot 1');
  check(await page.locator('.unit[data-unit="2"] .st-inputs tbody tr').nth(0).locator('td').nth(6).textContent() === '30 dB',
    'Status: slot 2 preamps from its own readback');
  await page.evaluate(() => showView('units'));

  // 12. The host's real answer until the driver has AGG_* (main.cpp): preview only, no elevation.
  await host({ type: 'agg', config: null, pending: false, error: { code: 1, detail: 'not available in this build' }, needsAdmin: false });
  const t12 = await barText();
  check(await bar.getAttribute('data-state') === 'off' && /Preview only/.test(t12) && await applyBtn.isDisabled(), 'no combined device in this build: preview, Apply off');
  check(/No administrator rights needed/.test(t12) && !/needs administrator rights/.test(t12), 'Apply is not said to need administrator rights');
  await ctx.close();

  /* ---------------- B. Demo scenarios ---------------- */
  const demo = await browser.newPage({ viewport: { width: 1240, height: 860 } });
  watch(demo);
  await demo.goto(ui + '?view=units');
  await demo.waitForTimeout(600);
  const drows = demo.locator('.ur-units tbody tr');
  check(await drows.count() === 2 && /MASTER/.test(await drows.first().textContent()), 'demo: x8p + Twin X, Twin X master');
  await demo.getByRole('button', { name: 'Twin X unplugged', exact: true }).click();
  check(/Apollo Twin X disconnected/.test((await demo.locator('.toast').allTextContents()).join()), 'demo unplug: notice');
  check((await drows.filter({ hasText: 'Apollo Twin X' }).getAttribute('class')).includes('absent'), 'demo unplug: greyed row');
  await demo.screenshot({ path: path.join(out, 'ui-units-demo-unplugged.png') });
  await demo.getByRole('button', { name: 'x8p only', exact: true }).click();
  check(await drows.count() === 1, 'demo x8p only: one row, no saved Twin X');
  await demo.getByRole('button', { name: 'x8p + Twin X', exact: true }).click();
  check(await drows.count() === 2, 'demo replug: two rows');
  await demo.getByRole('button', { name: 'Mirror', exact: true }).click();
  await demo.locator('#urBar').getByRole('button', { name: 'Apply' }).click();
  check(await demo.locator('#urBar').getAttribute('data-state') === 'busy', 'demo Apply: busy');
  await demo.waitForTimeout(1300);
  check(await demo.locator('#urBar').getAttribute('data-state') === 'clean', 'demo Apply: saved');
  await demo.close();

  /* ---------------- C. 980 px ---------------- */
  const narrow = await browser.newPage({ viewport: { width: 980, height: 760 } });
  watch(narrow);
  await narrow.goto(ui + '?view=units&units=unplugged');
  await narrow.waitForTimeout(1200);
  const overflow = () => narrow.evaluate(() => {
    const bad = [];
    const docW = document.documentElement.clientWidth;
    if (document.documentElement.scrollWidth > docW) bad.push('document');
    const top = document.querySelector('.top');
    if (top.scrollWidth > top.clientWidth) bad.push('top bar');
    for (const n of document.querySelectorAll('#view-units, #view-units .panel, #view-units .ur-bar, #view-status')) {
      if (n.offsetParent && n.scrollWidth > n.clientWidth + 1) bad.push(n.id || n.className);
    }
    for (const n of document.querySelectorAll('#view-units *')) {
      const r = n.getBoundingClientRect();
      if (r.width && r.right > docW + 0.5) { bad.push(`${n.tagName}.${n.className} right ${Math.round(r.right)}`); break; }
    }
    return bad;
  });
  let bad = await overflow();
  check(!bad.length, `980 px: no horizontal overflow (${bad.join(', ') || 'none'})`);
  await narrow.screenshot({ path: path.join(out, 'ui-units-980.png') });
  await narrow.getByRole('button', { name: 'x8p + Twin X', exact: true }).click();
  await narrow.getByRole('button', { name: 'Advanced', exact: true }).click();
  bad = await overflow();
  check(!bad.length, `980 px, advanced order: no horizontal overflow (${bad.join(', ') || 'none'})`);
  await narrow.evaluate(() => showView('status'));
  bad = await overflow();
  check(!bad.length, `980 px, Status with two units: no horizontal overflow (${bad.join(', ') || 'none'})`);
  await narrow.screenshot({ path: path.join(out, 'ui-units-status-980.png') });
  await narrow.close();

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
