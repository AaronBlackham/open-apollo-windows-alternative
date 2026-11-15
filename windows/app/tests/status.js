// Status view: demo data, then host-style "live" messages (older driver, more endpoints, rising
// counters, quarantine, engine lost, absent device). Checks the existing banners still win.
const { chromium } = require(process.env.PLAYWRIGHT_DIR || 'C:/Users/aaron/Desktop/Avium/Avium Invoid/node_modules/playwright');
const path = require('path');
const out = process.argv[2] || require('os').tmpdir();
const url = require('url').pathToFileURL(path.resolve(__dirname, '../ui/index.html')).href + '?view=status';
const check = (c, l) => console.log(`${c ? 'PASS' : 'FAIL'}  ${l}`);

(async () => {
  const browser = await chromium.launch();
  const page = await (await browser.newContext({ viewport: { width: 1240, height: 800 } })).newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(url);
  await page.waitForTimeout(1300);

  const unit = page.locator('.unit[data-unit="0"]');
  const inRows = unit.locator('.st-inputs tbody tr');
  const engCell = label => unit.locator('.st-engine tr', { hasText: label }).locator('td').nth(1);
  const dot = () => page.evaluate(() => { const d = document.getElementById('statusDot'); return d.hidden ? '' : d.dataset.state; });

  // 1. Demo: strips, preamps, engine, endpoints.
  check(await page.evaluate(() => S.view) === 'status', 'opens on the Status view');
  check(await inRows.count() === 8, 'eight input rows');
  check(await unit.locator('.st-inputs tbody td:nth-child(3)', { hasText: 'taken' }).count() === 8, 'demo: all eight strips taken');
  check(/live, pass complete · stage 9 of 9/.test(await unit.locator('.st-line').textContent()), 'strip summary line');
  check(/^Why: /.test(await unit.locator('.st-why').textContent()), 'Why text shown');
  check(await inRows.nth(1).locator('td').nth(4).textContent() === '48V', 'demo input 2 shows 48V on');
  check(await inRows.nth(2).locator('td').nth(5).textContent() === 'LINE', 'demo input 3 shows LINE');
  check(await engCell('State').textContent() === 'running', 'engine running');
  check(/^99\d\.\d \/s$|^1000\.\d \/s$/.test(await engCell('Tick rate').textContent()), `tick rate "${await engCell('Tick rate').textContent()}"`);
  check(/^4799\d\.\d$|^48000\.\d$/.test(await engCell('Frames/s').textContent()), 'frames/s near 48000');
  check(await engCell('ASIO client').textContent() === 'none', 'no ASIO client');
  check(await unit.locator('.st-endpoints tbody tr').count() === 2, 'two endpoint rows');
  check(await dot() === 'ok', 'tab dot ok');
  await page.screenshot({ path: path.join(out, 'ui-status-demo.png') });

  // 2. Host path, older driver: every live IOCTL failed with ERROR_INVALID_FUNCTION.
  await page.evaluate(() => exitDemo());
  const base = { version: 5, failedStage: 0, lastStatus: 0, fpgaRev: 0xA241C5AC, extCaps: 0x81110611, fwRelease: 0x0B0FD613,
    subsystemId: 0x14, deviceType: 0x22, publicType: 0x16, numDsps: 6, playChannels: 34, recChannels: 32, ringFrames: 8192,
    sampleRate: 48000, servicePasses: 10, interruptCount: 0, notifStatusSeen: 0, numPreamps: 8, serial: 'X8P2017ABCDEFGHI' };
  const flags = 0x40 | 0x80 | 0x100 | 0x200 | 0x400;
  await page.evaluate(([b, f]) => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 5, flags: f } }), [base, flags]);
  await page.evaluate(() => onHostMessage({ type: 'live', unit: 0, engine: null, engineErr: 1, strips: null, stripsErr: 1, endpoints: null, endpointsErr: 1 }));
  const na = await page.evaluate(() => [...document.querySelectorAll('.unit[data-unit="0"] .st-na')].filter(n => !n.hidden && n.offsetParent).map(n => n.textContent));
  check(na.length === 3 && na.every(t => t === 'unavailable'), `older driver: strips, engine, endpoints unavailable (${JSON.stringify(na)})`);
  check(await unit.locator('.st-engine table').isHidden() && await unit.locator('.st-endpoints table').isHidden(), 'no stale tables');
  check(await dot() === '', 'no tab dot without data');

  // 3. Newer driver: 10 endpoints (MON out/in + MIC/LINE 1-8); one capture endpoint running.
  const ep = (name, render, s, extra = {}) => ({ name, circuit: name.replace(/\W/g, ''), render, channels: render ? 2 : 1, slots: [s, s + 1],
    flags: 3, packetCount: 0, packetBytes: 0, streams: 0, underruns: 0, overruns: 0, dropped: 0, ...extra });
  const rows = [ep('MON L/R', 1, 0), ep('MON L/R', 0, 26)];
  for (let i = 0; i < 8; i++) rows.push(ep(`MIC/LINE ${i + 1}`, 0, i, i === 0 ? { flags: 0x1F, streams: 1, packetCount: 2, packetBytes: 1920 } : {}));
  const live = (late, under, extra = {}) => ({ type: 'live', unit: 0,
    engine: { version: 1, running: 1, lost: 0, starts: 1, ticks: 1000, lateTicks: late, busStalls: 0, deadReads: 0, maxIntervalUs: 1100,
      asioState: 2, asioFrames: 256, asioIn: 32, asioOut: 34, asioUnderruns: under, asioOverruns: 0, asioSkipped: 0,
      ticksPerSec: 1000.1, framesPerSec: 47999.87 },
    strips: { version: 2, state: 4, flags: 0x79, enable: 1, mask: 0xFF, stepsLimit: 9, stepsDone: 9, phantomState: 1, phantomMask: 3,
      why: 'stage 8 done on StripMask 0xff, DSP 1', perInput: true, taken: [2, 2, 2, 2, 1, 0, 0, 0], started: [1, 1, 1, 1, 0, 0, 0, 0] },
    endpoints: { version: 1, count: rows.length, enabledMask: 0x3FF, prepared: 1, layoutOk: true, rows },
    ...extra });
  await page.evaluate(m => onHostMessage(m), live(3, 0));
  check(await unit.locator('.st-endpoints tbody tr').count() === 10, 'ten endpoint rows from the data');
  check(await unit.locator('.st-endpoints tbody tr').nth(2).locator('td').nth(3).textContent() === 'running', 'MIC/LINE 1 running');
  check(await engCell('Frames/s').textContent() === '47999.9', 'frames/s from the host');
  check(await engCell('ASIO client').textContent() === 'streaming, 256 frames', 'ASIO client streaming');
  check(await inRows.nth(4).locator('td').nth(2).textContent() === 'queued, unproven', 'input 5 strip queued');
  check(await inRows.nth(5).locator('td').nth(2).textContent() === '-', 'input 6 no strip');
  check(await inRows.nth(0).locator('td').nth(6).textContent() === '-', 'no readback yet: preamp columns empty');
  check(await dot() === 'ok', 'tab dot ok');

  // 4. Counters rise: late ticks and ASIO underruns go amber, tab dot warn.
  await page.evaluate(m => onHostMessage(m), live(5, 2));
  check((await engCell('Late ticks').getAttribute('class')).includes('warn-text'), 'rising late ticks are amber');
  check((await engCell('Underruns').getAttribute('class')).includes('warn-text'), 'rising underruns are amber');
  check(await dot() === 'warn', 'tab dot warn');

  // 5. Quarantine: tag and red tab dot.
  const q = live(5, 2);
  q.strips = { ...q.strips, state: 5, flags: 0x79 | 0x02, why: 'transaction 3 unproven: power-cycle' };
  await page.evaluate(m => onHostMessage(m), q);
  check(await unit.locator('.st-tags .tag', { hasText: 'QUARANTINED' }).count() === 1, 'QUARANTINED tag');
  check(await dot() === 'bad', 'tab dot bad');
  await page.screenshot({ path: path.join(out, 'ui-status-host.png'), fullPage: true });

  // 5b. [G2] A second unit: a Twin X in observe mode as unit 1 (status + live), then it leaves.
  const twx = { ...base, version: 6, stage: 1, flags: 0x3, deviceType: 0x99, publicType: 0x19, subsystemId: 0x19, numDsps: 4,
    numPreamps: 0, playChannels: 0, recChannels: 0, sampleRate: 0, fwRelease: 0x01020003, serial: 'TWX2024ABCDEF', mode: 0, observe: true,
    modelName: 'Apollo Twin X (Gen 1)', unitFlags: 0x15 };
  await page.evaluate(([b, f]) => onHostMessage({ type: 'status', unit: 0, present: true, info: { ...b, version: 6, mode: 2, observe: false, stage: 5, flags: f } }), [base, flags]);
  await page.evaluate(t => onHostMessage({ type: 'status', unit: 1, present: true, info: t }), twx);
  await page.evaluate(() => onHostMessage({ type: 'live', unit: 1,
    engine: { version: 1, running: 0, lost: 0, starts: 0, ticks: 0, lateTicks: 0, busStalls: 0, deadReads: 0, maxIntervalUs: 0,
      asioState: 0, asioFrames: 0, asioIn: 0, asioOut: 0, asioUnderruns: 0, asioOverruns: 0, asioSkipped: 0, ticksPerSec: null, framesPerSec: null },
    strips: null, stripsErr: 21, endpoints: { version: 1, count: 10, enabledMask: 0, prepared: 0, layoutOk: true, rows: [] } }));
  await page.evaluate(() => onHostMessage({ type: 'units', units: [
    { id: '0123ABCD89EF4567', index: 0, model: 'Apollo x8p', deviceType: 34, serial: 'X8P2017ABCDEFGHI', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 },
    { id: 'FEDC000011112222', index: 1, model: 'Apollo Twin X (Gen 1)', deviceType: 153, serial: 'TWX2024ABCDEF', fw: '1.2.3', mode: 0, present: true, active: false, observe: true, stage: 1 }] }));
  const u1 = page.locator('.unit[data-unit="1"]');
  check(await u1.count() === 1, 'unit 1 section appears');
  check(await u1.locator('.unit-name').textContent() === 'Apollo Twin X (Gen 1)', 'unit 1 named by the driver');
  check(/observe mode, read-only/.test(await u1.locator('.unit-meta').textContent()), `unit 1 meta says observe ("${await u1.locator('.unit-meta').textContent()}")`);
  check(await u1.locator('.st-inputs tbody tr').count() === 0 && /Observe mode/.test(await u1.locator('.st-in-note').textContent()),
    'unit 1: no x8p input rows, an observe note');
  check(await u1.locator('.st-engine tr', { hasText: 'State' }).locator('td').nth(1).textContent() === 'stopped', 'unit 1 engine stopped');
  check(await inRows.count() === 8 && await unit.locator('.unit-name').textContent() === 'Apollo x8p', 'unit 0 still the x8p with eight inputs');
  check(await page.evaluate(() => S.host.info.deviceType) === 0x22, 'the controls still point at the x8p');
  // It leaves: the host sends present:false and a units list without it.
  await page.evaluate(() => onHostMessage({ type: 'status', unit: 1, present: false }));
  await page.evaluate(() => onHostMessage({ type: 'units', units: [
    { id: '0123ABCD89EF4567', index: 0, model: 'Apollo x8p', deviceType: 34, serial: 'X8P2017ABCDEFGHI', fw: '11.15.54803', mode: 2, present: true, active: true, observe: false, stage: 5 }] }));
  check(await u1.count() === 0, 'unit 1 section removed when it leaves');
  check(await unit.count() === 1 && await page.evaluate(() => S.host.present), 'unit 0 stays');
  // A unit that only disappears from the units list goes too.
  await page.evaluate(t => onHostMessage({ type: 'status', unit: 2, present: true, info: t }), twx);
  check(await page.locator('.unit[data-unit="2"]').count() === 1, 'unit 2 section appears');
  await page.evaluate(() => onHostMessage({ type: 'units', units: [] }));
  check(await page.locator('.unit[data-unit="2"]').count() === 0, 'unit 2 removed by the units list');

  // 6. Banners: firmware-untested still shows; engine lost takes priority over it.
  await page.evaluate(([b, f]) => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 5, flags: f | 0x1000, fwRelease: 0x0B0E0000 } }), [base, flags]);
  check(/not the 11\.15\.54803/.test(await page.locator('#bannerText').textContent()), 'firmware-untested banner');
  await page.evaluate(([b, f]) => onHostMessage({ type: 'status', present: true, info: { ...b, stage: 5, flags: f | 0x1000 | 0x2000 } }), [base, flags]);
  check(/audio engine stopped/.test(await page.locator('#bannerText').textContent()), 'engine-lost banner');

  // 7. Device gone: unit says not connected, everything unavailable.
  await page.evaluate(() => onHostMessage({ type: 'status', present: false, error: 2, detail: 'apollo.sys is not loaded on any device' }));
  check(await unit.locator('.unit-meta').textContent() === 'not connected', 'absent: not connected');
  check(await unit.locator('.st-engine .st-na').isVisible() && await unit.locator('.st-endpoints .st-na').isVisible(), 'absent: unavailable');
  check(await dot() === '', 'absent: no tab dot');

  // 8. Ctrl+2 opens Status from Control.
  await page.evaluate(() => showView('control'));
  await page.locator('body').click({ position: { x: 600, y: 30 } });
  await page.keyboard.press('Control+2');
  check(await page.evaluate(() => S.view) === 'status', 'Ctrl+2 opens Status');

  console.log(errors.length ? 'ERRORS:\n' + errors.join('\n') : 'no page errors');
  await browser.close();
})();
